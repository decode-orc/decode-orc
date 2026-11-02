/*
 * File:        naplps_lint_repair.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Lint-directed NAPLPS repair implementation (X3.110 §5.3, §6.1)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_lint_repair.h"

#include <fmt/format.h>

#include <algorithm>
#include <utility>

#include "naplps_code_env.h"
#include "vbi-services/teletext_page_decoder.h"

namespace orc {

namespace {

/// Bits in a data byte, b1 to b7 of the code position and the parity bit b8
/// CEA-516 §3.3 puts above them. A single-bit error is a flip of any one of
/// them, so a correction is a flip of any one of them.
constexpr int kBitsPerByte = 8;

/**
 * @brief The byte a repair writes where a byte must stop meaning something
 *
 * X3.110 §6.1.6.4 makes 1/10 a null operation at the presentation layer, and it
 * is not one of the transparent controls §6.1.4 to §6.1.6.1 let stand inside a
 * PDI sequence — so unlike the NUL a lost packet leaves, writing it ends the
 * operand run where it stands and draws nothing at all.
 *
 * That combination is what lets a repair take bytes out of a run without
 * moving anything after them: the record keeps its length, and every offset a
 * reader has been shown still means what it meant.
 */
uint8_t null_operation_byte() { return teletext_odd_parity_encode(kNaplpsSdc); }

/// |value| as a data byte, with the odd parity of CEA-516 §3.3.
uint8_t as_data_byte(uint8_t value) {
  return teletext_odd_parity_encode(static_cast<uint8_t>(value & 0x7F));
}

/**
 * @brief Whether a byte playing this part puts marks on the page
 *
 * The linter says what part a byte plays; this says what that part is worth to
 * a reader. Only four kinds draw: the opcode that names a drawing, the numeric
 * data it draws from, a displayable character, and a macro invocation that
 * expands into more of the same. Everything else — a control, an escape
 * sequence, a byte of a definition being stored, a byte from a null set —
 * arranges the page or arranges the decoder, and a reader loses nothing they
 * can see if it stops doing so.
 */
bool span_presents(NaplpsSpanKind kind) {
  switch (kind) {
    case NaplpsSpanKind::kPdiOpcode:
    case NaplpsSpanKind::kPdiOperand:
    case NaplpsSpanKind::kGraphic:
    case NaplpsSpanKind::kMacroInvocation:
      return true;
    default:
      return false;
  }
}

/// Whether the byte at |offset| puts marks on the page, as |lint| read the
/// record. A byte no span covers was never reached, and draws nothing.
bool presents_at(const NaplpsLintResult& lint, size_t offset) {
  const NaplpsSpan* span = lint.span_at(offset);
  return span != nullptr && span_presents(span->kind);
}

/**
 * @brief Whether the byte at |offset| decides how the bytes after it are read
 *
 * Three kinds of byte reach past themselves to the end of the record:
 *
 *   - Any byte of an escape sequence (§4.3.2). Which G-set a slot designates,
 *     and which slot, is settled here and read from every graphic byte after —
 *     and one bit off the ESC itself abandons the sequence, leaving its
 *     remaining bytes to print as characters.
 *   - A locking shift, SO or SI (§6.1.3), which is the other half of the same
 *     thing: it says which slot is in use.
 *   - DOMAIN and RESET (§5.3.2.2, §5.3.2.9), which set how many bytes an
 *     operand word occupies. Change one and every coordinate in the record is
 *     read from different six-bit groups.
 *
 * A correction to one of these is not a local repair whatever the grammar makes
 * of it, and neither is a correction *into* one — so the test is applied to the
 * byte as it stands and to the byte a candidate would make it.
 */
bool arranges_what_follows(const NaplpsLintResult& lint, size_t offset,
                           uint8_t byte) {
  const uint8_t payload = static_cast<uint8_t>(byte & 0x7F);
  if (payload == kNaplpsSo || payload == kNaplpsSi || payload == kNaplpsEsc) {
    return true;
  }
  const NaplpsSpan* span = lint.span_at(offset);
  if (span == nullptr) {
    return false;
  }
  if (span->kind == NaplpsSpanKind::kEscape) {
    return true;
  }
  if (span->kind == NaplpsSpanKind::kPdiOpcode) {
    const NaplpsPdi pdi = static_cast<NaplpsPdi>(payload);
    return pdi == NaplpsPdi::kDomain || pdi == NaplpsPdi::kReset;
  }
  return false;
}

/**
 * @brief One primitive as a value that can be compared with another
 *
 * The display list is what a reader sees, so two primitives are the same only
 * where every part a renderer would draw from is the same — geometry, texture,
 * colours and the map addresses they were resolved through. Hashed rather than
 * compared field by field because the question asked of it is a set question:
 * how much of what the page drew is still being drawn.
 */
uint64_t primitive_digest(const NabtsPrimitive& primitive) {
  uint64_t hash = 1469598103934665603ull;  // FNV-1a
  const auto mix = [&hash](const void* bytes, size_t length) {
    const auto* at = static_cast<const uint8_t*>(bytes);
    for (size_t i = 0; i < length; ++i) {
      hash ^= at[i];
      hash *= 1099511628211ull;
    }
  };
  const auto mix_point = [&mix](const NabtsPoint& point) {
    mix(&point.x, sizeof(point.x));
    mix(&point.y, sizeof(point.y));
  };
  const auto mix_size = [&mix](const NabtsSize& size) {
    mix(&size.dx, sizeof(size.dx));
    mix(&size.dy, sizeof(size.dy));
  };
  const auto mix_colour = [&mix](const NabtsColour& colour) {
    mix(&colour.green, sizeof(colour.green));
    mix(&colour.red, sizeof(colour.red));
    mix(&colour.blue, sizeof(colour.blue));
    mix(&colour.transparent, sizeof(colour.transparent));
  };

  mix(&primitive.kind, sizeof(primitive.kind));
  for (const NabtsPoint& point : primitive.points) {
    mix_point(point);
  }
  mix_point(primitive.origin);
  mix_size(primitive.size);
  mix(&primitive.filled, sizeof(primitive.filled));
  mix(&primitive.highlighted, sizeof(primitive.highlighted));
  mix_size(primitive.logical_pel);
  mix(&primitive.line_texture, sizeof(primitive.line_texture));
  mix(&primitive.texture_pattern, sizeof(primitive.texture_pattern));
  mix_size(primitive.texture_mask_size);
  mix(&primitive.colour_mode, sizeof(primitive.colour_mode));
  mix_colour(primitive.colour);
  mix_colour(primitive.background);
  mix(&primitive.colour_map_address, sizeof(primitive.colour_map_address));
  mix(&primitive.background_map_address,
      sizeof(primitive.background_map_address));
  return hash;
}

std::vector<uint64_t> digests_of(const std::vector<NabtsPrimitive>& page) {
  std::vector<uint64_t> out;
  out.reserve(page.size());
  for (const NabtsPrimitive& primitive : page) {
    out.push_back(primitive_digest(primitive));
  }
  std::sort(out.begin(), out.end());
  return out;
}

/// Primitives of |before| that |after| no longer draws. Counted as a multiset
/// so that drawing something twice is not the same as drawing it once, and so
/// that a primitive moving up or down the list — which a renderer paints in
/// order but a reader sees all at once — is not counted as lost.
size_t primitives_lost(const std::vector<NabtsPrimitive>& before,
                       const std::vector<NabtsPrimitive>& after) {
  const std::vector<uint64_t> was = digests_of(before);
  const std::vector<uint64_t> now = digests_of(after);
  size_t kept = 0;
  size_t i = 0;
  size_t j = 0;
  while (i < was.size() && j < now.size()) {
    if (was[i] == now[j]) {
      ++kept;
      ++i;
      ++j;
    } else if (was[i] < now[j]) {
      ++i;
    } else {
      ++j;
    }
  }
  return was.size() - kept;
}

/**
 * @brief Whether |after| is still the page |before| drew
 *
 * Additions are free: a correction that restores an orphaned opcode brings a
 * whole run of drawing back, which is the best thing this pass ever does. What
 * is weighed is loss — drawing that was there and is not, or is not the same
 * any more — because that is what a page being replaced rather than repaired
 * looks like from the inside.
 */
bool redraw_is_local(const std::vector<NabtsPrimitive>& before,
                     const std::vector<NabtsPrimitive>& after) {
  if (before.empty()) {
    return true;  // Nothing was being drawn, so nothing can be lost.
  }
  const size_t allowed = std::max(kNaplpsMinRedrawnPrimitives,
                                  before.size() / kNaplpsMaxRedrawnDenominator);
  return primitives_lost(before, after) <= allowed;
}

}  // namespace

// ---------------------------------------------------------------------------
// Reporting
// ---------------------------------------------------------------------------

std::string NaplpsRepairSummary::summary() const {
  if (!ran) {
    return {};
  }
  if (total_repairs() == 0 && suspect_bytes == 0 && errors_before == 0) {
    return {};
  }

  std::string out = fmt::format(
      "NAPLPS repair: {} byte(s) corrected, {} run(s) resynchronised, {} "
      "coordinate word(s) dropped\n",
      bytes_repaired, pdis_resynchronised, coordinate_words_dropped);
  out += fmt::format("  Evidence:  {} byte(s) in doubt, {} of them known wrong",
                     suspect_bytes, bytes_offered);
  if (bytes_ambiguous > 0) {
    out += fmt::format(", {} the grammar could not decide", bytes_ambiguous);
  }
  if (changes_declined_by_reach > 0) {
    out += fmt::format(", {} refused for redrawing the page",
                       changes_declined_by_reach);
  }
  if (suspect_bytes_uninspected > 0) {
    out += fmt::format(", {} past the inspection limit",
                       suspect_bytes_uninspected);
  }
  out += "\n";
  out += fmt::format(
      "  Findings:  {} error(s) and {} warning(s) before, {} "
      "and {} after\n",
      errors_before, warnings_before, errors_after, warnings_after);
  out += fmt::format(
      "  Page:      {}, {} primitive(s) drawn before and {} after\n",
      drawing_changed ? "draws differently" : "draws as it arrived",
      primitives_before, primitives_after);
  if (operand_words_retained > 0) {
    out += fmt::format(
        "  Retained:  {} operand word(s) kept past the damage by repeating "
        "their opcode\n",
        operand_words_retained);
  }
  return out;
}

void naplps_stamp_repair_diagnostics(const NaplpsRepairSummary& summary,
                                     NabtsDecodeDiagnostics& diagnostics) {
  if (!summary.ran) {
    return;
  }
  diagnostics.repaired_bytes = summary.bytes_repaired;
  diagnostics.resynchronised_pdis = summary.pdis_resynchronised;
  diagnostics.dropped_coordinate_words = summary.coordinate_words_dropped;
  diagnostics.undecided_suspect_bytes = summary.bytes_ambiguous;
  diagnostics.changes_declined_by_reach = summary.changes_declined_by_reach;
  diagnostics.repair_changed_drawing = summary.drawing_changed;
}

// ---------------------------------------------------------------------------
// The repairer
// ---------------------------------------------------------------------------

NaplpsLintRepairer::NaplpsLintRepairer() { reset_decoder(); }

void NaplpsLintRepairer::reset_decoder() { linter_.reset_decoder(); }

NaplpsLintResult NaplpsLintRepairer::trial_lint(
    const std::vector<uint8_t>& data, const NaplpsSuspectMap& suspects) const {
  // The service state is forked rather than advanced: a candidate byte may
  // define a macro or designate a set, and none of that may outlive the trial
  // that considered it. Copying between lints is safe — see NaplpsLinter.
  NaplpsLinter scratch = linter_;
  return scratch.lint(data, suspects);
}

std::vector<NabtsPrimitive> NaplpsLintRepairer::trial_render(
    const std::vector<uint8_t>& data) const {
  NaplpsInterpreter scratch = interpreter_;
  return scratch.run(data).primitives;
}

NaplpsRepairResult NaplpsLintRepairer::repair(
    const std::vector<uint8_t>& record) {
  return repair(record, NaplpsSuspectMap::from_record(record));
}

NaplpsRepairResult NaplpsLintRepairer::repair(
    const std::vector<uint8_t>& record, const NaplpsSuspectMap& suspects) {
  NaplpsRepairResult result;
  result.data = record;
  result.summary.ran = true;

  const NaplpsLintResult before = trial_lint(result.data, suspects);
  result.summary.errors_before = before.findings.errors;
  result.summary.warnings_before = before.findings.warnings;

  substitute_bytes(suspects, result.data, result.summary);
  resynchronise_runs(suspects, result.data, result.summary);

  // What the pass did to the page, which is the account a reader needs and the
  // fault counts cannot give. Rendered only where something was changed: a
  // record that arrived intact draws what it draws, and running it twice to
  // prove it would cost every clean record a render.
  if (result.data != record) {
    const std::vector<NabtsPrimitive> was = trial_render(record);
    const std::vector<NabtsPrimitive> now = trial_render(result.data);
    result.summary.primitives_before = static_cast<uint32_t>(was.size());
    result.summary.primitives_after = static_cast<uint32_t>(now.size());
    result.summary.drawing_changed =
        was.size() != now.size() || primitives_lost(was, now) > 0;
  }

  // The last pass is the committing one: the service state advances as the
  // *repaired* record ran, which is what the record after this one will be
  // linted and repaired against.
  const NaplpsLintResult after = linter_.lint(result.data, suspects);
  result.findings = after.findings;
  result.summary.errors_after = after.findings.errors;
  result.summary.warnings_after = after.findings.warnings;
  interpreter_.run(result.data);
  return result;
}

void NaplpsLintRepairer::substitute_bytes(const NaplpsSuspectMap& suspects,
                                          std::vector<uint8_t>& data,
                                          NaplpsRepairSummary& summary) const {
  std::vector<size_t> inspect;
  for (size_t offset = 0; offset < data.size(); ++offset) {
    if (!suspects.suspect(offset)) {
      continue;
    }
    ++summary.suspect_bytes;

    // Only a parity failure licenses a substitution. It is the one piece of
    // evidence that says a byte is *wrong*: CEA-516 §3.3 puts odd parity on
    // every data byte, so a byte that fails it has an odd number of bits the
    // wrong way round and there is something to correct. Everything else the
    // recovery knows is a statement about how sure it is, not about what
    // arrived — a vote the copies left level, a detector reporting low
    // confidence — and a byte nobody can point to a fault in is a byte to
    // leave alone. Doubt still stops this pass acting elsewhere; it does not
    // license it to act here.
    //
    // A byte that never arrived is not a byte with a bit wrong in it either.
    // Parity is a statement about something that was received; the filler
    // standing in for a lost packet carries no information at all, and
    // "correcting" it would be writing a plausible byte into a gap rather than
    // recovering one. The structural pass works around those instead.
    if (!suspects.parity_failed(offset) || suspects.missing(offset)) {
      continue;
    }
    ++summary.bytes_offered;
    if (inspect.size() < kNaplpsMaxInspectedSuspectBytes) {
      inspect.push_back(offset);
    } else {
      ++summary.suspect_bytes_uninspected;
    }
  }
  if (inspect.empty()) {
    return;
  }

  for (const size_t offset : inspect) {
    // Recomputed rather than carried, because an earlier repair in this same
    // pass may have changed what the record's remaining faults are.
    const NaplpsLintResult before = trial_lint(data, suspects);
    const NaplpsLintGrade baseline = naplps_lint_grade(before.findings);
    const bool was_presenting = presents_at(before, offset);
    const uint8_t original = data[offset];

    // A byte that says how the bytes after it are read is not something one
    // correction can be weighed on its own: the record after it means one thing
    // or another depending on the answer, and the grammar prefers whichever
    // reading happens to fault less. Left exactly as it arrived.
    if (arranges_what_follows(before, offset, original)) {
      continue;
    }

    uint8_t chosen = original;
    NaplpsLintGrade best;
    size_t attained_best = 0;

    for (int bit = 0; bit < kBitsPerByte; ++bit) {
      const uint8_t candidate = static_cast<uint8_t>(original ^ (1u << bit));
      data[offset] = candidate;
      const NaplpsLintResult probe = trial_lint(data, suspects);
      data[offset] = original;

      // A byte that draws may not be corrected into one that does not. The
      // grammar has no way of preferring a drawing to a silence — a silence
      // never breaks it — so the part the byte plays is what bounds the
      // corrections admissible at it (§5.3.1). The reverse is allowed: a byte
      // knocked out of its columns by the damage is put back.
      if (was_presenting && !presents_at(probe, offset)) {
        continue;
      }
      // Nor may a correction *make* a byte that says how the record is read.
      if (arranges_what_follows(probe, offset, candidate)) {
        continue;
      }

      const NaplpsLintGrade trial = naplps_lint_grade(probe.findings);
      if (attained_best == 0 || trial < best) {
        best = trial;
        chosen = candidate;
        attained_best = 1;
      } else if (trial == best) {
        ++attained_best;
      }
    }

    if (attained_best == 0) {
      continue;  // Every correction would have stopped the byte drawing.
    }
    // Strictly better than leaving it alone, not merely as good: a change that
    // buys nothing had no evidence for it beyond the parity failure, and the
    // parity failure alone does not say which bit was hit.
    if (!(best < baseline)) {
      continue;
    }
    if (attained_best == 1) {
      // The last question, and the only one asked of the page rather than of
      // the grammar: does the record still draw what it drew? One bit of damage
      // is one damaged instruction, so a correction that leaves a quarter of
      // the page gone or altered has not corrected an instruction — it has made
      // the record say something else, and said it grammatically.
      const std::vector<NabtsPrimitive> was = trial_render(data);
      data[offset] = chosen;
      if (!redraw_is_local(was, trial_render(data))) {
        data[offset] = original;
        ++summary.changes_declined_by_reach;
        continue;
      }
      ++summary.bytes_repaired;
    } else {
      // Two corrections leave the record equally grammatical, and the grammar
      // is all there is to go on. Guessing between them would be inventing a
      // page rather than recovering one.
      ++summary.bytes_ambiguous;
    }
  }
}

void NaplpsLintRepairer::resynchronise_runs(
    const NaplpsSuspectMap& suspects, std::vector<uint8_t>& data,
    NaplpsRepairSummary& summary) const {
  const NaplpsLintResult state = trial_lint(data, suspects);

  // Which coordinate words the linter faulted for naming a point outside the
  // unit screen, so a run carrying one can be cut at it.
  std::vector<size_t> out_of_range;
  for (const NaplpsLintFinding& finding : state.findings.findings) {
    if (finding.rule == NaplpsLintRule::kCoordinateOutOfRange) {
      out_of_range.push_back(finding.offset);
    }
  }

  for (const NaplpsPdiObservation& pdi : state.pdis) {
    if (pdi.block_bytes == 0 || pdi.operand_offsets.empty()) {
      continue;  // No word structure to cut on.
    }
    // The word and block sizes a cut divides on are read from the opcode and
    // the DOMAIN format in force at it (§5.3.2.2). Where the evidence doubts
    // the opcode itself, what follows it may be no run at all, and a cut made
    // on a structure inferred from a doubtful byte would silence bytes that
    // were never its operands.
    if (suspects.suspect(pdi.opcode_offset)) {
      continue;
    }
    const size_t total = pdi.operand_offsets.size();

    // A lost packet's filler standing among the operands means bytes went
    // missing from the middle of the run. The filler itself is skipped when the
    // operands are gathered (§5.3.1), so it shifts nothing by being there — but
    // the real bytes it stands in for are gone, and every word after the gap is
    // read from the wrong six-bit groups because of it.
    //
    // Proof rather than doubt, as everywhere else the pass acts: the byte has
    // to have gone missing with its packet, or to have failed its parity. A
    // transparent control the recovery merely felt unsure of is a control the
    // sender may well have embedded, and §5.3.1 says it changes nothing.
    bool holed = false;
    size_t hole_index = total;
    for (const size_t control : pdi.embedded_controls) {
      if (!suspects.missing(control) && !suspects.parity_failed(control)) {
        continue;  // A control the sender really did embed.
      }
      const auto it = std::upper_bound(pdi.operand_offsets.begin(),
                                       pdi.operand_offsets.end(), control);
      hole_index = static_cast<size_t>(it - pdi.operand_offsets.begin());
      holed = true;
      break;
    }

    // Only worth cutting where the surviving words really are misaligned. A gap
    // that happens to fall on an execution boundary took whole words with it
    // and left everything after it reading correctly.
    if (holed && hole_index % pdi.block_bytes != 0 &&
        cut_run(pdi, hole_index, hole_index, suspects, data, summary)) {
      ++summary.pdis_resynchronised;
      continue;
    }

    // A coordinate word naming a point outside the unit screen, at least one of
    // whose bytes the evidence doubts. §5.3.1 makes the coordinate an error and
    // leaves the handling open; the interpreter clamps it, which is right for a
    // sender's mistake and wrong for noise. Dropped rather than clamped, so the
    // page shows what arrived rather than a point invented for it.
    for (const size_t word_offset : out_of_range) {
      const auto it = std::lower_bound(pdi.operand_offsets.begin(),
                                       pdi.operand_offsets.end(), word_offset);
      if (it == pdi.operand_offsets.end() || *it != word_offset) {
        continue;  // Not this run's.
      }
      const size_t word_index =
          static_cast<size_t>(it - pdi.operand_offsets.begin());
      const size_t word_end = word_index + pdi.word_bytes;
      if (word_end > total) {
        continue;
      }
      // Dropping a word is an action, so it takes proof rather than doubt: one
      // of its bytes has to have failed its parity or gone missing with a
      // packet. A word the recovery merely felt unsure of, out of range, is
      // either the sender's own mistake or a reading nobody can better — and
      // §5.3.1 leaves the handling of the sender's mistake to the interpreter,
      // which clamps it.
      bool proven = false;
      for (size_t index = word_index; index < word_end && !proven; ++index) {
        const size_t byte = pdi.operand_offsets[index];
        proven = suspects.parity_failed(byte) || suspects.missing(byte);
      }
      if (!proven) {
        continue;
      }
      if (cut_run(pdi, word_index, word_end, suspects, data, summary)) {
        ++summary.coordinate_words_dropped;
      }
      break;
    }
  }
}

bool NaplpsLintRepairer::cut_run(const NaplpsPdiObservation& pdi,
                                 size_t cut_index, size_t resume_index,
                                 const NaplpsSuspectMap& suspects,
                                 std::vector<uint8_t>& data,
                                 NaplpsRepairSummary& summary) const {
  const size_t total = pdi.operand_offsets.size();
  const size_t block = pdi.block_bytes;
  if (block == 0 || cut_index > total || resume_index > total ||
      resume_index < cut_index) {
    return false;
  }

  // Everything up to the last whole execution before the damage is kept: those
  // words were read from the right six-bit groups and describe what the sender
  // drew.
  const size_t keep = (cut_index / block) * block;
  if (keep >= total) {
    return false;  // Nothing before the damage to cut back to.
  }

  // The tail is kept only where it is a whole number of executions, so the
  // re-anchored opcode gets exactly the operands it expects. Anything else and
  // the tail would draw a shape assembled from the wrong bytes, which is the
  // very thing being repaired.
  //
  // Where the damage is a gap, the bytes just past it are usually the tail of
  // the word the gap broke, and are no more use than the gap itself — so the
  // resume point is the earliest one at or after |resume_index| that leaves
  // whole executions behind it, rather than |resume_index| exactly.
  size_t resume = total;
  for (size_t candidate = resume_index; candidate < total; ++candidate) {
    if ((total - candidate) % block == 0) {
      resume = candidate;
      break;
    }
  }
  const size_t tail = total - resume;
  const bool retain = tail > 0;

  // Where no whole execution comes before the damage, the opcode is nulled
  // with its operands. Left standing with none it is not nothing: §5.3.2.2.5
  // zero-fills a missing operand, and both receivers of the day draw LINE
  // (Absolute) with no data as a line to the origin (see
  // NaplpsInterpreter::pdi_line()). So the instruction the damage broke goes
  // whole, and a tail that is kept is re-anchored by the opcode said again
  // below.
  const size_t null_from =
      keep == 0 ? pdi.opcode_offset : pdi.operand_offsets[keep];
  const size_t null_to = retain ? pdi.operand_offsets[resume] : pdi.run_end;
  if (null_to <= null_from || null_to > data.size()) {
    return false;
  }

  // How much this can null needs no bound of its own. |keep| is at most one
  // block below the damage and |resume| at most one above the word after it, so
  // a cut never takes more than three executions of operands out however long
  // the run is; the rest of the span is the lost packet's own filler, which
  // drew nothing to begin with. What has to be guarded is whether the run was a
  // run at all, which is the opcode's business — see resynchronise_runs().

  std::vector<uint8_t> candidate = data;
  size_t nulled = null_to - null_from;
  for (size_t offset = null_from; offset < null_to; ++offset) {
    candidate[offset] = null_operation_byte();
  }

  if (retain) {
    // §5.3.2.2.5: numeric data beyond one execution "is taken as an indication
    // to repeat the execution of the opcode with the subsequent numeric data
    // taken as new operands". Writing the opcode again into the last byte
    // nulled is therefore not an invention — it is the same instruction the
    // run already carried implicitly, said again where the damage broke it.
    candidate[null_to - 1] = as_data_byte(pdi.opcode);
    --nulled;
  }

  // Weighed as a substitution is, and for the same reason: what is done to a
  // record has to earn its place. A cut that leaves the record no more
  // grammatical than it found it has bought nothing with the bytes it spent.
  if (!(naplps_lint_grade(trial_lint(candidate, suspects).findings) <
        naplps_lint_grade(trial_lint(data, suspects).findings))) {
    return false;
  }

  // And weighed against the page, which is the test with teeth here: nulling
  // bytes always improves the grade, so the grammar cannot tell a run being
  // resynchronised from a page being silenced. A cut drops the operands of one
  // damaged run, and one run is a small part of a page.
  const std::vector<NabtsPrimitive> was = trial_render(data);
  if (!redraw_is_local(was, trial_render(candidate))) {
    ++summary.changes_declined_by_reach;
    return false;
  }

  data = std::move(candidate);
  summary.bytes_nulled += static_cast<uint32_t>(nulled);
  if (retain) {
    summary.operand_words_retained +=
        static_cast<uint32_t>(tail / pdi.word_bytes);
  }
  return true;
}

}  // namespace orc
