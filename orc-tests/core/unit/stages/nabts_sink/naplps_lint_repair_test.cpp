/*
 * File:        naplps_lint_repair_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for lint-directed NAPLPS repair (X3.110 §5.3, §6.1)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_lint_repair.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "naplps_interpreter.h"
#include "vbi-services/teletext_page_decoder.h"

namespace orc {
namespace {

/// A code position in the standard's column/row notation.
constexpr uint8_t code(int column, int row) {
  return static_cast<uint8_t>((column << 4) | row);
}

/// A numeric data byte carrying |payload| in b6-b1 (§5.3.1, Figure 10).
constexpr uint8_t numeric(uint8_t payload) {
  return static_cast<uint8_t>(0x40 | (payload & 0x3F));
}

constexpr uint8_t kSo = 0x0E;
constexpr uint8_t kEsc = 0x1B;
/// §6.1.6.4: the null operation a repair writes to take a byte out of a run.
constexpr uint8_t kSdc = 0x1A;

/// CEA-516 §3.3's odd parity in b8, which is how a record arrives.
std::vector<uint8_t> with_parity(const std::vector<uint8_t>& bytes) {
  std::vector<uint8_t> out;
  out.reserve(bytes.size());
  for (const uint8_t byte : bytes) {
    out.push_back(
        teletext_odd_parity_encode(static_cast<uint8_t>(byte & 0x7F)));
  }
  return out;
}

/// The seven-bit payloads of |bytes|, which is what the interpreter reads.
std::vector<uint8_t> payloads(const std::vector<uint8_t>& bytes) {
  std::vector<uint8_t> out;
  out.reserve(bytes.size());
  for (const uint8_t byte : bytes) {
    out.push_back(static_cast<uint8_t>(byte & 0x7F));
  }
  return out;
}

NaplpsRepairResult repair(const std::vector<uint8_t>& record) {
  NaplpsLintRepairer repairer;
  return repairer.repair(record);
}

/// |record| repaired against evidence that doubts the byte at |offset| beyond
/// anything its parity says — the detector, or the vote between copies, having
/// been unsure of it.
NaplpsRepairResult repair_doubting(const std::vector<uint8_t>& record,
                                   size_t offset) {
  std::vector<uint8_t> confidence(record.size(), 255);
  confidence[offset] = 0;
  NaplpsLintRepairer repairer;
  return repairer.repair(record,
                         NaplpsSuspectMap::from_record(record, {}, confidence));
}

/**
 * @brief |record| with one bit of the byte at |offset| flipped
 *
 * Which is exactly what a single-bit error looks like on the wire: the payload
 * changes and the odd parity CEA-516 §3.3 put on the byte no longer matches it,
 * so the recovery knows the byte is wrong without knowing which bit.
 */
std::vector<uint8_t> flip_bit(std::vector<uint8_t> record, size_t offset,
                              uint8_t mask) {
  record[offset] = static_cast<uint8_t>(record[offset] ^ mask);
  return record;
}

/// Primitives of |record| as the interpreter draws them.
std::vector<NabtsPrimitive> draw(const std::vector<uint8_t>& record) {
  NaplpsInterpreter interpreter;
  interpreter.reset_decoder();
  return interpreter.run(record).primitives;
}

/**
 * @brief One three-byte coordinate word carrying |x| and |y| (Figure 11)
 *
 * Each byte holds one three-bit field of each component, X in b6-b4 and Y in
 * b3-b1, the first byte carrying the most significant. Nine bits per component
 * at the default multi-value length of §5.3.2.2.3, read as a two's-complement
 * binary fraction — so a value below 0x100 leaves the sign bit clear and names
 * a point inside the unit screen, which is what an absolute coordinate must do.
 */
std::vector<uint8_t> coordinate(uint16_t x, uint16_t y) {
  std::vector<uint8_t> out;
  for (int field = 2; field >= 0; --field) {
    const uint8_t shift = static_cast<uint8_t>(field * 3);
    const uint8_t packed = static_cast<uint8_t>((((x >> shift) & 0x7u) << 3) |
                                                ((y >> shift) & 0x7u));
    out.push_back(numeric(packed));
  }
  return out;
}

/// |head| followed by every word of |words|, with parity applied.
std::vector<uint8_t> pdi_record(
    const std::vector<uint8_t>& head,
    const std::vector<std::vector<uint8_t>>& words) {
  std::vector<uint8_t> plain = head;
  for (const std::vector<uint8_t>& word : words) {
    plain.insert(plain.end(), word.begin(), word.end());
  }
  return with_parity(plain);
}

/// A record that draws two lines: SO invokes the PDI set, then LINE ABS (2/8)
/// with two absolute coordinate words. §5.3.2.2.5 repeats the opcode for the
/// second word, so one word is one line drawn.
std::vector<uint8_t> two_line_record() {
  return pdi_record({kSo, code(2, 8)},
                    {coordinate(0x040, 0x050), coordinate(0x0C0, 0x0D0)});
}

////////////////////////////////////////////////////////////////////////////////
// Task 2.1 — parity-guided single-bit repair
////////////////////////////////////////////////////////////////////////////////

TEST(NaplpsLintRepair, LeavesACleanRecordExactlyAsItArrived) {
  const std::vector<uint8_t> record = two_line_record();
  const NaplpsRepairResult result = repair(record);

  EXPECT_EQ(result.data, record);
  EXPECT_TRUE(result.summary.ran);
  EXPECT_EQ(result.summary.suspect_bytes, 0u);
  EXPECT_EQ(result.summary.total_repairs(), 0u);
  EXPECT_EQ(result.summary.errors_before, 0u);
  EXPECT_EQ(result.summary.errors_after, 0u);
}

// §5.3.1 puts numeric data in columns 4 to 7, so b7 is what makes a byte an
// operand. Knock b7 out of an operand and the PDI ends early; the grammar then
// admits exactly one correction, because no other single bit puts the byte back
// in the numeric columns.
TEST(NaplpsLintRepair, RestoresAnOperandByteKnockedOutOfItsColumn) {
  const std::vector<uint8_t> record = two_line_record();
  // b7 is what puts a byte in the numeric columns, so clearing it drops the
  // operand out of the run (§5.3.1).
  const std::vector<uint8_t> damaged = flip_bit(record, 3, 0x40);

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 1u);
  EXPECT_EQ(result.data, record);
  EXPECT_LT(result.summary.errors_after, result.summary.errors_before);
}

// The correction is the flip of any one of the eight bits, the parity bit
// included: a byte whose payload was right and whose parity bit alone was hit
// needs no change to what it means.
TEST(NaplpsLintRepair, LeavesAByteWhoseOnlyDamagedBitIsTheParityBit) {
  const std::vector<uint8_t> record = two_line_record();
  const std::vector<uint8_t> damaged = flip_bit(record, 3, 0x80);

  const NaplpsRepairResult result = repair(damaged);

  // The payload still parses, so nothing is improved by changing it and
  // nothing is changed. The record reads as it always did.
  EXPECT_EQ(payloads(result.data), payloads(record));
  EXPECT_EQ(result.summary.bytes_repaired, 0u);
  EXPECT_EQ(result.summary.suspect_bytes, 1u);
}

// A value bit inside an operand leaves the byte in its own column, so the
// grammar has nothing to say about which bit was hit. Six corrections would
// parse equally well, and guessing between them would invent a page.
TEST(NaplpsLintRepair, DeclinesAByteWhoseDamageTheGrammarCannotDecide) {
  const std::vector<uint8_t> record = two_line_record();
  const std::vector<uint8_t> damaged =
      flip_bit(record, 3, 0x01);  // a value bit

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 0u);
  EXPECT_EQ(payloads(result.data), payloads(damaged));
}

// The one thing a repair pass must never do is change a byte the recovery had
// no reason to doubt.
TEST(NaplpsLintRepair, NeverChangesAByteTheEvidenceDoesNotDoubt) {
  // A record the linter faults — an operand run that is not a whole number of
  // words — but every byte of which arrived with its parity intact.
  std::vector<uint8_t> plain = {kSo, code(2, 8)};
  const std::vector<uint8_t> word = coordinate(0x040, 0x050);
  plain.insert(plain.end(), word.begin(), word.end());
  plain.push_back(numeric(0x01));  // one byte more than a whole word
  const std::vector<uint8_t> record = with_parity(plain);

  const NaplpsRepairResult result = repair(record);

  EXPECT_GT(result.summary.warnings_before, 0u);
  EXPECT_EQ(result.data, record);
  EXPECT_EQ(result.summary.total_repairs(), 0u);
  EXPECT_EQ(result.summary.suspect_bytes, 0u);
}

// §5.3.1: numeric data with no opcode in front of it is discarded, so a
// corrupted opcode costs the whole PDI. Putting it back is the repair that buys
// the most.
TEST(NaplpsLintRepair, RestoresAPdiOpcodeCorruptedIntoNumericData) {
  const std::vector<uint8_t> record = two_line_record();
  // Setting b7 on the opcode makes it read as an operand (§5.3.1).
  const std::vector<uint8_t> damaged = flip_bit(record, 1, 0x40);

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 1u);
  EXPECT_EQ(payloads(result.data), payloads(record));
  EXPECT_EQ(draw(result.data).size(), draw(record).size());
}

// The correction that empties a page is always available and always looks like
// the best one there is. Clearing b7 of an operand moves it out of columns 4 to
// 7 into a transparent control, which §5.3.1 lets stand inside the run without
// ending it — so the byte leaves the operand sequence and every word after it
// is read from different six-bit groups. Where the run was misaligned that
// silences every fault in it at a stroke, and the geometry it draws afterwards
// is not the geometry that arrived. The grammar cannot tell the difference, so
// the part the byte plays is what rules the correction out.
TEST(NaplpsLintRepair, DeclinesACorrectionThatWouldStopAByteDrawing) {
  // Three words whose third byte, read as the first byte of a word, names a
  // point outside the unit screen — so a run shifted by one byte faults twice
  // over, and a run reading straight does not fault at all.
  const std::vector<uint8_t> shifts_out_of_range = coordinate(0x044, 0x044);
  std::vector<uint8_t> plain = {kSo, code(2, 8)};
  const std::vector<uint8_t> first = coordinate(0x040, 0x050);
  plain.insert(plain.end(), first.begin(), first.end());
  // One byte more than the words account for, standing between them. Clearing
  // its b7 leaves 0/2 (STX), which is transparent.
  plain.push_back(numeric(0x02));
  for (int word = 0; word < 3; ++word) {
    plain.insert(plain.end(), shifts_out_of_range.begin(),
                 shifts_out_of_range.end());
  }
  std::vector<uint8_t> record = with_parity(plain);
  // The odd byte is the one the recovery doubts, its parity having gone stale.
  const size_t odd_byte = 5;
  record[odd_byte] = static_cast<uint8_t>(record[odd_byte] ^ 0x80);

  const NaplpsRepairResult result = repair(record);

  EXPECT_EQ(result.summary.bytes_repaired, 0u);
  EXPECT_EQ(result.data, record);
  // The faults it would have silenced are still reported, which is the point:
  // a reader is told what arrived rather than shown a page invented to parse.
  EXPECT_GT(result.summary.errors_after, 0u);
}

// Only a parity failure says a byte is *wrong*. A close vote between copies, or
// a detector reporting low confidence, says how sure the recovery is — which is
// a statement about the recovery, not about the byte. With a fifth of a record
// held in doubt on a damaged recording, treating that as licence to rewrite is
// a search wide enough to find something that parses better almost anywhere.
TEST(NaplpsLintRepair, DeclinesAByteTheRecoveryOnlyFeltUnsureOf) {
  // The very damage RestoresAnOperandByteKnockedOutOfItsColumn repairs — an
  // operand out of columns 4 to 7 — but arriving with parity that matches it.
  std::vector<uint8_t> plain = {kSo, code(2, 8)};
  const std::vector<uint8_t> first = coordinate(0x040, 0x050);
  const std::vector<uint8_t> second = coordinate(0x0C0, 0x0D0);
  plain.insert(plain.end(), first.begin(), first.end());
  plain.insert(plain.end(), second.begin(), second.end());
  plain[3] = static_cast<uint8_t>(plain[3] & ~0x40);
  const std::vector<uint8_t> record = with_parity(plain);

  const NaplpsRepairResult result = repair_doubting(record, 3);

  EXPECT_EQ(result.summary.suspect_bytes, 1u);
  EXPECT_EQ(result.summary.bytes_offered, 0u);
  EXPECT_EQ(result.summary.bytes_repaired, 0u);
  EXPECT_EQ(result.data, record);
}

// §4.3.2's escape sequences say which G-set a slot designates and which slot is
// in use, so every graphic byte after one is read through it. One bit off the
// ESC abandons the sequence and its remaining bytes print as characters; one
// bit off an intermediate moves the designation to another slot. Either parses,
// and the grammar has no way of preferring the one that was sent.
TEST(NaplpsLintRepair, LeavesTheBytesOfAnEscapeSequenceAlone) {
  // ESC 4/4 is DEF TEXTURE (§6.2.4), ESC 4/5 the END that closes it.
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(4, 4), code(4, 2), kEsc, code(4, 5)});

  // The control byte of the sequence, hit.
  const std::vector<uint8_t> control_hit = flip_bit(record, 1, 0x10);
  const NaplpsRepairResult on_control = repair(control_hit);
  ASSERT_GT(on_control.summary.bytes_offered, 0u);
  EXPECT_EQ(on_control.summary.bytes_repaired, 0u);
  EXPECT_EQ(on_control.data, control_hit);

  // And the ESC itself, which no correction may put back either: a byte that
  // would *become* one of these reaches just as far as one that is.
  const std::vector<uint8_t> esc_hit = flip_bit(record, 0, 0x04);
  const NaplpsRepairResult on_esc = repair(esc_hit);
  ASSERT_GT(on_esc.summary.bytes_offered, 0u);
  EXPECT_EQ(on_esc.summary.bytes_repaired, 0u);
  EXPECT_EQ(on_esc.data, esc_hit);
}

// The last question, and the only one asked of the page rather than the
// grammar. One flipped bit is one damaged instruction; a correction that leaves
// most of the page gone or drawn in another colour has not corrected an
// instruction, it has made the record say something else — and said it
// grammatically, which is why nothing above this can catch it.
TEST(NaplpsLintRepair, DeclinesACorrectionThatWouldRepaintThePage) {
  // Eight coordinate words with one odd byte standing between them, so the run
  // is misaligned and faults. Clearing that byte's b7 leaves 3/12 — SET COLOUR,
  // whose operands have no word structure at all (§5.3.2.5) — which silences
  // every fault in the tail and draws the rest of the page in whatever colour
  // those bytes happen to name.
  std::vector<uint8_t> plain = {kSo, code(2, 8)};
  for (int word = 0; word < 4; ++word) {
    const std::vector<uint8_t> point =
        coordinate(static_cast<uint16_t>(0x040 + word * 0x10), 0x050);
    plain.insert(plain.end(), point.begin(), point.end());
  }
  const size_t odd_byte = plain.size();
  plain.push_back(numeric(0x3C));
  for (int word = 0; word < 4; ++word) {
    const std::vector<uint8_t> point =
        coordinate(static_cast<uint16_t>(0x0C0 - word * 0x10), 0x0D0);
    plain.insert(plain.end(), point.begin(), point.end());
  }
  std::vector<uint8_t> record = with_parity(plain);
  // The odd byte is the one known to be wrong, its parity having gone stale.
  record[odd_byte] = static_cast<uint8_t>(record[odd_byte] ^ 0x80);

  const NaplpsRepairResult result = repair(record);

  EXPECT_EQ(result.summary.bytes_repaired, 0u);
  // Twice over, as it happens: the substitution is refused, and so is the cut
  // the structural pass would otherwise make on the same misaligned run. Both
  // would have redrawn the page rather than repaired the run.
  EXPECT_GE(result.summary.changes_declined_by_reach, 1u);
  EXPECT_EQ(result.summary.pdis_resynchronised, 0u);
  EXPECT_EQ(result.data, record);
}

// What the pass did to the *page*, which is the account a reader can check
// against what is in front of them. The fault counts say whether the record
// parses better, and a record can parse very much better while drawing
// something the sender never sent.
TEST(NaplpsLintRepair, SaysWhetherThePageItselfCameOutDifferent) {
  const std::vector<uint8_t> record = two_line_record();
  std::vector<uint8_t> damaged = record;
  damaged[3] = 0x00;  // a lost packet's filler inside the operands

  const NaplpsRepairResult altered = repair(damaged);

  EXPECT_GT(altered.summary.total_repairs(), 0u);
  EXPECT_TRUE(altered.summary.drawing_changed);
  EXPECT_GT(altered.summary.primitives_before, 0u);

  // A record nothing was done to is not run at all, and says so with zeroes
  // rather than claiming the page was checked.
  const NaplpsRepairResult untouched = repair(record);

  EXPECT_FALSE(untouched.summary.drawing_changed);
  EXPECT_EQ(untouched.summary.primitives_before, 0u);
  EXPECT_EQ(untouched.summary.primitives_after, 0u);
}

////////////////////////////////////////////////////////////////////////////////
// Task 2.2 — hole and truncation resynchronisation
////////////////////////////////////////////////////////////////////////////////

// A lost packet leaves 0x00 fillers where its bytes should have been. §6.1.4
// makes NUL a transparent control, which §5.3.1 lets stand inside a PDI without
// ending it — so the operands close up over the gap and every word after it is
// read from the wrong six-bit groups.
TEST(NaplpsLintRepair,
     ResynchronisesAnOperandRunWithBytesMissingFromTheMiddle) {
  const std::vector<uint8_t> record = two_line_record();
  std::vector<uint8_t> damaged = record;
  damaged[3] = 0x00;  // one byte of the first coordinate word lost

  const std::vector<NabtsPrimitive> before = draw(damaged);
  const NaplpsRepairResult result = repair(damaged);
  const std::vector<NabtsPrimitive> after = draw(result.data);

  EXPECT_EQ(result.summary.pdis_resynchronised, 1u);
  EXPECT_EQ(result.data.size(), record.size());

  // Before the repair the run read as two words assembled from the surviving
  // five bytes plus zero-extension: geometry the sender never described.
  ASSERT_FALSE(before.empty());
  // After it, the trailing whole word is kept by repeating the opcode
  // (§5.3.2.2.5) and the broken leading word is gone.
  EXPECT_EQ(result.summary.operand_words_retained, 1u);
  ASSERT_EQ(after.size(), 1u);
  // The surviving line is the one the intact second word describes.
  const std::vector<uint8_t> tail_only =
      pdi_record({kSo, code(2, 8)}, {coordinate(0x0C0, 0x0D0)});
  const std::vector<NabtsPrimitive> expected = draw(tail_only);
  ASSERT_EQ(expected.size(), 1u);
  EXPECT_DOUBLE_EQ(after.front().points.back().x,
                   expected.front().points.back().x);
  EXPECT_DOUBLE_EQ(after.front().points.back().y,
                   expected.front().points.back().y);
}

// The bytes a repair takes out of a run are overwritten with the null operation
// of §6.1.6.4 rather than removed, so nothing after them moves and every offset
// a reader has been shown still means what it meant.
TEST(NaplpsLintRepair, TakesBytesOutOfARunWithoutMovingAnythingAfterThem) {
  const std::vector<uint8_t> record = two_line_record();
  std::vector<uint8_t> damaged = record;
  damaged[3] = 0x00;

  const NaplpsRepairResult result = repair(damaged);

  ASSERT_EQ(result.data.size(), record.size());
  // The bytes before the instruction the damage broke are untouched.
  EXPECT_EQ(result.data[0], record[0]);
  // That instruction kept no whole word before the damage, so its opcode goes
  // with its operands: left with none, both receivers would draw it to the
  // origin (§5.3.2.2.5's zero fill).
  EXPECT_EQ(result.data[1] & 0x7F, kSdc);
  // The broken leading word and the filler became null operations, bar the one
  // byte carrying the repeated opcode.
  EXPECT_EQ(result.data[2] & 0x7F, kSdc);
  EXPECT_EQ(result.data[3] & 0x7F, kSdc);
  EXPECT_EQ(result.data[4] & 0x7F, code(2, 8));  // the opcode, said again
  // The intact trailing word is exactly as it arrived.
  EXPECT_EQ(result.data[5], record[5]);
  EXPECT_EQ(result.data[6], record[6]);
  EXPECT_EQ(result.data[7], record[7]);
}

// A gap that happens to take whole operand words with it leaves everything
// after it reading correctly, so there is nothing to resynchronise.
TEST(NaplpsLintRepair, LeavesARunWhoseGapFellOnAnOperandBoundary) {
  // A hole three bytes wide — one whole coordinate word — after the first.
  const std::vector<uint8_t> record = two_line_record();
  std::vector<uint8_t> damaged = record;
  damaged[5] = 0x00;
  damaged[6] = 0x00;
  damaged[7] = 0x00;

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.pdis_resynchronised, 0u);
  // The surviving word still draws what it always described.
  ASSERT_EQ(draw(result.data).size(), 1u);
}

// A cut divides the run on the word and block sizes read from the opcode and
// the DOMAIN format in force at it (§5.3.2.2). Where the evidence doubts the
// opcode, what follows it may be no run at all — and a cut made on a structure
// inferred from a doubtful byte would null bytes that were never its operands.
TEST(NaplpsLintRepair, LeavesARunWhoseOpcodeTheEvidenceDoubts) {
  // SET LINE ABS (2/10) takes two coordinate words per execution, so the run
  // below is one execution with a hole in it — the case the test above has
  // resynchronised. Its opcode is one whose damage the grammar cannot decide,
  // so no substitution is made to it and the cut is all that is in question.
  const std::vector<uint8_t> record = pdi_record(
      {kSo, code(2, 10)}, {coordinate(0x040, 0x050), coordinate(0x0C0, 0x0D0)});
  std::vector<uint8_t> damaged = record;
  damaged[3] = 0x00;  // one byte of the first word lost with its packet

  // Nothing is doubted but the opcode, the hole being evidence enough of
  // itself. The run is left exactly as it arrived, hole and all.
  const NaplpsRepairResult doubted = repair_doubting(damaged, 1);

  EXPECT_EQ(doubted.summary.pdis_resynchronised, 0u);
  EXPECT_EQ(doubted.data, damaged);

  // The same record with the opcode arriving intact is cut, which is what
  // makes the refusal above the opcode's doing rather than the hole's.
  const NaplpsRepairResult trusted = repair(damaged);

  EXPECT_EQ(trusted.summary.pdis_resynchronised, 1u);
}

// Where the bytes after the damage are not a whole number of executions there
// is nothing to re-anchor them to, and keeping them would draw a shape
// assembled from the wrong bytes.
TEST(NaplpsLintRepair, DiscardsATailThatDoesNotRealign) {
  // SET LINE ABS (2/10) takes two coordinate words per execution (§5.3.3.2.4),
  // so a six-byte block. Four words, with the gap in the second block: one and
  // a half words survive after it, which no repeat of the opcode can consume.
  const std::vector<uint8_t> record = pdi_record(
      {kSo, code(2, 10)}, {coordinate(0x010, 0x020), coordinate(0x030, 0x040),
                           coordinate(0x050, 0x060), coordinate(0x070, 0x080)});
  std::vector<uint8_t> damaged = record;
  damaged[10] = 0x00;

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.pdis_resynchronised, 1u);
  EXPECT_EQ(result.summary.operand_words_retained, 0u);
  // The first whole execution — two words, six bytes — is kept.
  EXPECT_EQ(result.data[2], record[2]);
  EXPECT_EQ(result.data[7], record[7]);
  // Everything from the damage on is a null operation.
  for (size_t offset = 8; offset < result.data.size(); ++offset) {
    EXPECT_EQ(result.data[offset] & 0x7F, kSdc) << "at " << offset;
  }
}

////////////////////////////////////////////////////////////////////////////////
// Task 2.3 — range and sequence repairs
////////////////////////////////////////////////////////////////////////////////

// §5.3.1 makes a coordinate a two's-complement fraction in [-1, 1), so the sign
// bit of an absolute coordinate puts it outside the unit screen. Where one
// single-bit correction brings it back, that is the correction.
TEST(NaplpsLintRepair, RestoresACoordinateWhoseSignBitWasHit) {
  const std::vector<uint8_t> record = two_line_record();
  // b6 of the first numeric byte is the most significant bit of X, which
  // Figure 11 weights -1.
  const std::vector<uint8_t> damaged = flip_bit(record, 2, 0x20);

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 1u);
  EXPECT_EQ(payloads(result.data), payloads(record));
}

// Where no single correction brings the coordinate back, the word is dropped
// rather than clamped: §5.3.1 leaves the handling open, and clamping is the
// right answer for a sender's mistake but not for noise.
TEST(NaplpsLintRepair, DropsACoordinateWordThatCannotBeBroughtBackInRange) {
  // Two coordinate words, the first with the sign bit of *both* components set,
  // which no single correction can clear. Its parity is stale, so the recovery
  // doubts it.
  std::vector<uint8_t> damaged = pdi_record(
      {kSo, code(2, 8)}, {coordinate(0x140, 0x150), coordinate(0x0C0, 0x0D0)});
  damaged[2] = static_cast<uint8_t>(damaged[2] ^ 0x80);
  damaged[3] = static_cast<uint8_t>(damaged[3] ^ 0x80);

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.coordinate_words_dropped, 1u);
  EXPECT_EQ(result.summary.operand_words_retained, 1u);
  // The intact second word is kept, re-anchored by repeating the opcode.
  EXPECT_EQ(result.data[4] & 0x7F, code(2, 8));
  ASSERT_EQ(draw(result.data).size(), 1u);
}

// A coordinate the sender really did put outside the unit screen is its own
// mistake, and the interpreter's existing clamp is the right answer for it.
TEST(NaplpsLintRepair, LeavesAnOutOfRangeCoordinateThatArrivedIntact) {
  const std::vector<uint8_t> record =
      pdi_record({kSo, code(2, 8)}, {coordinate(0x140, 0x050)});

  const NaplpsRepairResult result = repair(record);

  EXPECT_EQ(result.summary.coordinate_words_dropped, 0u);
  EXPECT_EQ(result.data, record);
  // The interpreter still clamps it, exactly as before.
  EXPECT_EQ(draw(result.data).size(), 1u);
}

// §6.2.4: the texture mask code must name mask A to D, 4/1 to 4/4. The linter
// faults anything else, so a single-bit correction that lands inside is the one
// the grammar picks.
TEST(NaplpsLintRepair, RestoresATextureMaskCodeKnockedOutOfRange) {
  // ESC 4/4 (DEF TEXTURE), code 4/2 (mask B), END.
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(4, 4), code(4, 2), kEsc, code(4, 5)});
  const std::vector<uint8_t> damaged = flip_bit(record, 2, 0x10);  // 4/2 -> 5/2

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 1u);
  EXPECT_EQ(payloads(result.data), payloads(record));
}

// §6.2.2.1 requires only that a DEF MACRO's code byte be a graphic character,
// and there are ninety-six of those. A code knocked out of that range therefore
// has several corrections that put it back, all of them naming a perfectly
// legal macro — so the grammar cannot say which macro was meant, and the
// repair declines. The narrow ranges are the repairable ones; this is what the
// wide ones look like.
TEST(NaplpsLintRepair, DeclinesADefinitionCodeWithSeveralValidCorrections) {
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(4, 0), code(2, 1), 'A', kEsc, code(4, 5)});
  const std::vector<uint8_t> damaged = flip_bit(record, 2, 0x20);  // into C0

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 0u);
  EXPECT_EQ(result.summary.bytes_ambiguous, 1u);
  EXPECT_EQ(result.data, damaged);
}

// The interpreter already closes a definition a truncated record left open, so
// there is nothing for a repair to do about one — see NaplpsInterpreter::run().
// The linter reports it and the repair leaves it alone.
TEST(NaplpsLintRepair, LeavesADefinitionTheInterpreterAlreadyCloses) {
  const std::vector<uint8_t> record =
      with_parity({kEsc, code(4, 0), code(2, 1), 'A', 'B'});

  const NaplpsRepairResult result = repair(record);

  EXPECT_EQ(result.data, record);
  EXPECT_EQ(result.findings.count(NaplpsLintRule::kUnterminatedDefinition), 1u);
}

TEST(NaplpsLintRepair, RepairsSeveralKindsOfDamageInOneRecord) {
  // A LINE ABS with one word, a line feed to end the sequence (§5.3.1), then a
  // POINT ABS (2/6) with one word.
  constexpr uint8_t kApd = 0x0A;
  std::vector<uint8_t> plain = {kSo, code(2, 8)};
  const std::vector<uint8_t> first = coordinate(0x040, 0x050);
  const std::vector<uint8_t> second = coordinate(0x0C0, 0x0D0);
  plain.insert(plain.end(), first.begin(), first.end());
  plain.push_back(kApd);
  plain.push_back(code(2, 6));
  plain.insert(plain.end(), second.begin(), second.end());
  const std::vector<uint8_t> record = with_parity(plain);

  // An operand knocked out of its column, and an opcode knocked into one.
  const std::vector<uint8_t> damaged =
      flip_bit(flip_bit(record, 3, 0x40), 6, 0x40);

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 2u);
  EXPECT_EQ(payloads(result.data), payloads(record));
  EXPECT_EQ(result.summary.errors_after, 0u);
}

// Two faults in the same operand run can mask each other: with the second still
// wrong, correcting the first buys nothing the grammar can see, and the other
// way about. Both are declined rather than guessed at, and the structural pass
// salvages what it can of the run.
TEST(NaplpsLintRepair, DeclinesTwoFaultsInOneRunThatMaskEachOther) {
  std::vector<uint8_t> plain = {kSo, code(2, 8)};
  const std::vector<uint8_t> first = coordinate(0x040, 0x050);
  const std::vector<uint8_t> second = coordinate(0x0C0, 0x0D0);
  plain.insert(plain.end(), first.begin(), first.end());
  plain.push_back(code(2, 6));
  plain.insert(plain.end(), second.begin(), second.end());
  const std::vector<uint8_t> record = with_parity(plain);

  const std::vector<uint8_t> damaged =
      flip_bit(flip_bit(record, 3, 0x40), 5, 0x40);

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.bytes_repaired, 0u);
  // What survives is the intact trailing word, re-anchored to its opcode.
  EXPECT_EQ(result.summary.pdis_resynchronised, 1u);
  EXPECT_LE(result.summary.errors_after, result.summary.errors_before);
}

////////////////////////////////////////////////////////////////////////////////
// Service state
////////////////////////////////////////////////////////////////////////////////

// A page invoking a macro its channel's Support Record defined (CEA-516
// §5.2.7.9) has to be repaired by something that has already seen that Support
// Record, or every such invocation looks like damage.
TEST(NaplpsLintRepair, CarriesServiceStateBetweenRecords) {
  const std::vector<uint8_t> support =
      with_parity({kEsc, code(4, 0), code(2, 1), 'A', kEsc, code(4, 5)});
  const std::vector<uint8_t> page =
      with_parity({kEsc, code(2, 9), code(7, 10), kSo, code(2, 1)});

  NaplpsLintRepairer repairer;
  EXPECT_EQ(repairer.repair(support).summary.errors_after, 0u);
  const NaplpsRepairResult result = repairer.repair(page);
  EXPECT_EQ(result.findings.count(NaplpsLintRule::kUndefinedMacro), 0u);
  EXPECT_EQ(result.data, page);
}

// A candidate byte considered and rejected must leave nothing of itself behind:
// a trial that defined a macro would make the next record's invocation of it
// look resolvable when it is not.
TEST(NaplpsLintRepair, DoesNotCommitTheStateOfACandidateItRejected) {
  // A record whose damaged byte, under some candidate corrections, would open a
  // macro definition. Whatever is tried, only the chosen record's state stands.
  const std::vector<uint8_t> damaged = flip_bit(
      with_parity({kEsc, code(4, 0), code(2, 1), 'A', kEsc, code(4, 5)}), 2,
      0x04);

  NaplpsLintRepairer repairer;
  const NaplpsRepairResult first = repairer.repair(damaged);

  // Whichever macro code the repaired record defined, exactly one was defined:
  // invoking a different one is still undefined.
  const std::vector<uint8_t> page =
      with_parity({kEsc, code(2, 9), code(7, 10), kSo, code(7, 15)});
  const NaplpsRepairResult second = repairer.repair(page);
  EXPECT_EQ(second.findings.count(NaplpsLintRule::kUndefinedMacro), 1u);
  EXPECT_GE(first.summary.suspect_bytes, 1u);
}

////////////////////////////////////////////////////////////////////////////////
// Task 2.4 — diagnostics
////////////////////////////////////////////////////////////////////////////////

// The interpreter is never told a record was repaired, so it leaves the repair
// counters at the zeroes it built them with — which is the honest reading for a
// page presented as transmitted.
TEST(NaplpsLintRepair, LeavesTheRepairCountersZeroWhenNothingRanThem) {
  NaplpsInterpreter interpreter;
  interpreter.reset_decoder();
  const NabtsPageSnapshot page = interpreter.run(two_line_record());

  EXPECT_EQ(page.diagnostics.repaired_bytes, 0u);
  EXPECT_EQ(page.diagnostics.resynchronised_pdis, 0u);
  EXPECT_EQ(page.diagnostics.dropped_coordinate_words, 0u);
  EXPECT_EQ(page.diagnostics.undecided_suspect_bytes, 0u);
}

TEST(NaplpsLintRepair, StampsWhatTheRepairDidOntoADecodedPage) {
  NaplpsRepairSummary summary;
  summary.ran = true;
  summary.bytes_repaired = 3;
  summary.pdis_resynchronised = 2;
  summary.coordinate_words_dropped = 1;
  summary.bytes_ambiguous = 4;

  NabtsPageSnapshot page;
  page.diagnostics.bytes_read = 42;
  naplps_stamp_repair_diagnostics(summary, page.diagnostics);

  EXPECT_EQ(page.diagnostics.repaired_bytes, 3u);
  EXPECT_EQ(page.diagnostics.resynchronised_pdis, 2u);
  EXPECT_EQ(page.diagnostics.dropped_coordinate_words, 1u);
  EXPECT_EQ(page.diagnostics.undecided_suspect_bytes, 4u);
  // Everything the interpreter measured is left as it found it.
  EXPECT_EQ(page.diagnostics.bytes_read, 42u);
}

TEST(NaplpsLintRepair, StampsNothingForAPassThatNeverRan) {
  const NaplpsRepairSummary summary;  // ran == false
  NabtsPageSnapshot page;
  page.diagnostics.repaired_bytes = 7;
  naplps_stamp_repair_diagnostics(summary, page.diagnostics);
  EXPECT_EQ(page.diagnostics.repaired_bytes, 7u);
}

TEST(NaplpsRepairSummary, SummarisesNothingForAPassThatNeverRan) {
  EXPECT_TRUE(NaplpsRepairSummary{}.summary().empty());
}

TEST(NaplpsRepairSummary, SaysWhatItDidAndHowMuchWasInDoubt) {
  const std::vector<uint8_t> record = two_line_record();
  const std::vector<uint8_t> damaged = flip_bit(record, 3, 0x40);

  const std::string summary = repair(damaged).summary.summary();
  EXPECT_NE(summary.find("NAPLPS repair"), std::string::npos);
  EXPECT_NE(summary.find("in doubt"), std::string::npos);
}

////////////////////////////////////////////////////////////////////////////////
// Robustness
////////////////////////////////////////////////////////////////////////////////

// A recovered record is arbitrary bytes as far as the repairer is concerned. It
// must always terminate, always keep the record's length, and never leave it
// worse than it found it.
TEST(NaplpsLintRepair, SurvivesArbitraryCorruptionOfARealRecord) {
  const std::vector<uint8_t> record = with_parity(
      {kSo,           code(2, 1),    numeric(0x00), code(2, 8), numeric(0x04),
       code(2, 10),   numeric(0x08), numeric(0x0C), 0x0F,       'A',
       'B',           kEsc,          code(4, 0),    code(2, 1), 'C',
       kEsc,          code(4, 5),    kSo,           code(2, 4), numeric(0x10),
       numeric(0x14), numeric(0x18)});

  std::mt19937 rng(20260811u);  // fixed, so a failure is reproducible
  std::uniform_int_distribution<size_t> offset_of(0, record.size() - 1);
  std::uniform_int_distribution<int> bit_of(0, 7);

  for (int trial = 0; trial < 500; ++trial) {
    std::vector<uint8_t> damaged = record;
    const int hits = 1 + (trial % 4);
    for (int hit = 0; hit < hits; ++hit) {
      const size_t offset = offset_of(rng);
      damaged[offset] =
          static_cast<uint8_t>(damaged[offset] ^ (1u << bit_of(rng)));
    }

    const NaplpsRepairResult result = repair(damaged);
    ASSERT_EQ(result.data.size(), record.size()) << "trial " << trial;
    // Repair never makes the grammar worse: that is the whole contract.
    EXPECT_LE(result.summary.errors_after, result.summary.errors_before)
        << "trial " << trial;
    // And whatever it produced, the interpreter can run it without complaint.
    (void)draw(result.data);
  }
}

TEST(NaplpsLintRepair, HandlesAnEmptyRecord) {
  const NaplpsRepairResult result = repair({});
  EXPECT_TRUE(result.data.empty());
  EXPECT_TRUE(result.summary.ran);
  EXPECT_EQ(result.summary.total_repairs(), 0u);
}

// Beyond a certain amount of damage, byte-level repair is not the right tool
// and the cost of trying is real. The cap is reported rather than hidden.
TEST(NaplpsLintRepair, ReportsSuspectBytesItDidNotInspect) {
  const size_t offered = kNaplpsMaxInspectedSuspectBytes + 20;
  std::vector<uint8_t> plain = {kSo, code(2, 8)};
  for (size_t i = 0; i < offered; ++i) {
    plain.push_back(numeric(static_cast<uint8_t>(i & 0x3F)));
  }
  std::vector<uint8_t> damaged = with_parity(plain);
  // Every operand arrives with its parity bit hit, so every one is known to be
  // wrong and every one is offered to the pass. The head is left intact.
  for (size_t offset = 2; offset < damaged.size(); ++offset) {
    damaged[offset] = static_cast<uint8_t>(damaged[offset] ^ 0x80);
  }

  const NaplpsRepairResult result = repair(damaged);

  EXPECT_EQ(result.summary.suspect_bytes, offered);
  EXPECT_EQ(result.summary.bytes_offered, offered);
  EXPECT_EQ(result.summary.suspect_bytes_uninspected, 20u);
  EXPECT_EQ(result.data.size(), damaged.size());
}

}  // namespace
}  // namespace orc
