/*
 * File:        nabts_catalogue_view.cpp
 * Module:      nabts_sink stage plugin
 * Purpose:     The record catalogue as an SDK CatalogueDataset the host can
 *              browse
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_catalogue_view.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "nabts_raster_view.h"
#include "nabts_record_catalogue.h"

namespace orc {

namespace {

// Bits per gun in the transmitted colour (X3.110 §5.3.1, Table D1 item 5(4)),
// and the full-scale value that gives.
constexpr int kGunBits = 3;
constexpr int kGunMax = (1 << kGunBits) - 1;  // 7

// Unit-space slack for deciding whether two character origins are a step apart.
// Table D1 item 8 puts the nominal resolution at 256 x 200, so one pixel is
// about 1/256; this is three orders of magnitude below that and exists only to
// absorb the arithmetic of resolving a relative coordinate.
constexpr double kOriginEpsilon = 1e-6;

// The widest step that still counts as the next character field along the path.
// X3.110 §5.3.2.3.4 Table 8's inter-character spacings are 1, 1.25 and 1.5
// character fields, and proportional spacing is at least one; two fields is
// clear of all of them and well short of a jump to another line.
constexpr double kMaxStepFields = 2.0;

// SMPTE 170M: 525-line NTSC scans 59.94 fields per second, so a frame is
// 1001/30000 of a second. Caption cue extents are frame counts, and this is
// what turns one into a time a reader recognises.
constexpr double kNtscFramesPerSecond = 30000.0 / 1001.0;

/// A three-bit gun value as an 8-bit channel, full scale to full scale.
uint8_t to_channel(uint8_t gun) {
  return static_cast<uint8_t>((std::min<int>(gun, kGunMax) * 255) / kGunMax);
}

CatalogueColour to_colour(const NabtsColour& colour) {
  CatalogueColour out;
  out.red = to_channel(colour.red);
  out.green = to_channel(colour.green);
  out.blue = to_channel(colour.blue);
  out.transparent = colour.transparent;
  return out;
}

CataloguePoint to_point(const NabtsPoint& point) {
  return CataloguePoint{point.x, point.y};
}

CatalogueSize to_size(const NabtsSize& size) {
  return CatalogueSize{size.dx, size.dy};
}

/**
 * @brief One six-bit numeric colour specification as a colour (X3.110 Fig. 12)
 *
 * The six payload bits are two three-tuples in the order green, red, blue, so
 * each gun gets two bits taken one per tuple, most significant first. §5.3.2.5
 * zero-extends a value with fewer bits than the map holds, which for two bits
 * into three is a single shift.
 */
NabtsColour colour_from_numeric(uint8_t payload) {
  const auto gun = [payload](int offset) {
    const uint32_t high = (payload >> (3 + offset)) & 0x1u;
    const uint32_t low = (payload >> offset) & 0x1u;
    return static_cast<uint8_t>(((high << 1) | low) << (kGunBits - 2));
  };
  NabtsColour colour;
  colour.green = gun(2);
  colour.red = gun(1);
  colour.blue = gun(0);
  return colour;
}

/// One entry of an incremental colour run, resolved (§5.3.3.6.3): a value in
/// colour mode 0, a colour-map address in modes 1 and 2.
CatalogueColour resolve_incremental(uint8_t entry, NabtsColourMode mode,
                                    const NabtsPageSnapshot& snapshot) {
  if (mode == NabtsColourMode::kDirect) {
    return to_colour(colour_from_numeric(entry));
  }
  const size_t address = static_cast<size_t>(entry) % kNabtsColourMapEntries;
  return to_colour(snapshot.colour_map[address]);
}

CatalogueDrawKind to_draw_kind(NabtsPrimitiveKind kind) {
  switch (kind) {
    case NabtsPrimitiveKind::kPoint:
      return CatalogueDrawKind::kPoint;
    case NabtsPrimitiveKind::kLine:
      return CatalogueDrawKind::kLine;
    case NabtsPrimitiveKind::kArc:
      return CatalogueDrawKind::kArc;
    case NabtsPrimitiveKind::kRectangle:
      return CatalogueDrawKind::kRectangle;
    case NabtsPrimitiveKind::kPolygon:
      return CatalogueDrawKind::kPolygon;
    case NabtsPrimitiveKind::kIncrementalPoints:
      return CatalogueDrawKind::kColourRun;
    case NabtsPrimitiveKind::kCharacter:
      break;
  }
  // Split by repertoire at the call site; never reached with a character.
  return CatalogueDrawKind::kText;
}

int rotation_degrees(NabtsCharRotation rotation) {
  switch (rotation) {
    case NabtsCharRotation::kNone:
      return 0;
    case NabtsCharRotation::k90:
      return 90;
    case NabtsCharRotation::k180:
      return 180;
    case NabtsCharRotation::k270:
      return 270;
  }
  return 0;
}

/// Everything an operation carries that is not geometry: two operations that
/// agree on all of it can be drawn by one call.
void copy_attributes(const NabtsPrimitive& from, CatalogueDrawOp& to) {
  to.filled = from.filled;
  // X3.110 §5.3.2.4.3: a highlighted figure is filled as usual and outlined in
  // nominal black, or in the background colour where there is one.
  to.outlined = from.highlighted;
  to.pen_size = to_size(from.logical_pel);
  to.line_style = static_cast<CatalogueLineStyle>(from.line_texture);
  to.fill_pattern = static_cast<CatalogueFillPattern>(from.texture_pattern);
  to.fill_mask_size = to_size(from.texture_mask_size);
  to.colour = to_colour(from.colour);
  to.has_background =
      from.colour_mode == NabtsColourMode::kMappedWithBackground;
  to.background = to_colour(from.background);
  to.blinking = from.blinking;
  to.blink_to = to_colour(from.blink_to);
  to.rotation_degrees = rotation_degrees(from.rotation);
  to.reverse_video = from.reverse_video;
  to.underlined = from.underlined;
}

/// Whether |candidate| could be drawn as part of a run already carrying the
/// attributes of |run|.
bool attributes_match(const CatalogueDrawOp& run,
                      const CatalogueDrawOp& candidate) {
  return run.colour == candidate.colour &&
         run.has_background == candidate.has_background &&
         run.background == candidate.background &&
         run.blinking == candidate.blinking &&
         run.blink_to == candidate.blink_to &&
         run.rotation_degrees == candidate.rotation_degrees &&
         run.reverse_video == candidate.reverse_video &&
         run.underlined == candidate.underlined &&
         std::fabs(run.size.dx - candidate.size.dx) < kOriginEpsilon &&
         std::fabs(run.size.dy - candidate.size.dy) < kOriginEpsilon;
}

/**
 * @brief The state of a text run being built up
 *
 * NAPLPS emits one primitive per character, because that is what the record
 * transmits; a renderer would rather have the word. Coalescing needs both
 * halves of "the same run": the attributes have to match, and the character has
 * to sit where the cursor would have left it — one character field along a
 * single axis, in the same direction as the run's earlier steps.
 */
class TextRun {
 public:
  bool open() const { return open_; }

  /// Start a run at |primitive|, discarding whatever was there.
  void start(const NabtsPrimitive& primitive, std::string glyph) {
    op_ = CatalogueDrawOp{};
    op_.kind = CatalogueDrawKind::kText;
    copy_attributes(primitive, op_);
    op_.origin = to_point(primitive.origin);
    op_.size = to_size(primitive.size);
    op_.advance = nominal_advance(primitive);
    op_.points.push_back(op_.origin);
    op_.text = std::move(glyph);
    op_.character_count = 1;
    last_origin_ = op_.origin;
    have_step_ = false;
    open_ = true;
  }

  /// Extend the run with |primitive| if it belongs to it; false if it does not.
  bool extend(const NabtsPrimitive& primitive, const std::string& glyph) {
    if (!open_) {
      return false;
    }
    CatalogueDrawOp candidate;
    copy_attributes(primitive, candidate);
    candidate.size = to_size(primitive.size);
    if (!attributes_match(op_, candidate)) {
      return false;
    }

    const double dx = primitive.origin.x - last_origin_.x;
    const double dy = primitive.origin.y - last_origin_.y;
    if (!step_is_along_the_path(dx, dy)) {
      return false;
    }

    op_.text += glyph;
    ++op_.character_count;
    // The measured step beats the nominal one: it carries whatever
    // inter-character spacing (§5.3.2.3.4) the record asked for.
    op_.advance = CatalogueSize{dx, dy};
    last_origin_ = to_point(primitive.origin);
    step_x_ = dx;
    step_y_ = dy;
    have_step_ = true;
    return true;
  }

  /// Compose |mark| onto the character just added, which is what a non-spacing
  /// mark at the same origin is (X3.110 §7.2).
  void compose(const std::string& mark) { op_.text += mark; }

  CatalogueDrawOp take() {
    open_ = false;
    return std::move(op_);
  }

 private:
  /// The step the character path would take with no extra spacing, which is all
  /// a run of one character says about its direction (§5.3.2.3.3 Table 7).
  static CatalogueSize nominal_advance(const NabtsPrimitive& primitive) {
    const double width = std::fabs(primitive.size.dx);
    const double height = std::fabs(primitive.size.dy);
    switch (primitive.path) {
      case NabtsCharPath::kRight:
        return CatalogueSize{width, 0.0};
      case NabtsCharPath::kLeft:
        return CatalogueSize{-width, 0.0};
      case NabtsCharPath::kUp:
        return CatalogueSize{0.0, height};
      case NabtsCharPath::kDown:
        return CatalogueSize{0.0, -height};
    }
    return CatalogueSize{width, 0.0};
  }

  /// One character field along a single axis, in the run's established
  /// direction. Two fields is the ceiling — see kMaxStepFields.
  bool step_is_along_the_path(double dx, double dy) const {
    const double width = std::fabs(op_.size.dx);
    const double height = std::fabs(op_.size.dy);
    const bool horizontal =
        std::fabs(dy) < kOriginEpsilon && std::fabs(dx) > kOriginEpsilon &&
        std::fabs(dx) <= width * kMaxStepFields + kOriginEpsilon;
    const bool vertical =
        std::fabs(dx) < kOriginEpsilon && std::fabs(dy) > kOriginEpsilon &&
        std::fabs(dy) <= height * kMaxStepFields + kOriginEpsilon;
    if (!horizontal && !vertical) {
      return false;
    }
    if (!have_step_) {
      return true;  // the second character establishes the direction
    }
    // A record that turned a corner mid-word started a new run, whatever the
    // attributes say.
    return std::fabs(dx - step_x_) < kOriginEpsilon &&
           std::fabs(dy - step_y_) < kOriginEpsilon;
  }

  CatalogueDrawOp op_;
  CataloguePoint last_origin_;
  double step_x_ = 0.0;
  double step_y_ = 0.0;
  bool have_step_ = false;
  bool open_ = false;
};

/// CEA-516 §5.2.2: record types 0, 1 and 3 carry presentation data, which §6.1
/// makes NAPLPS; type 2 carries application data. The reserved types 4 to 15
/// say nothing about their data, so nothing is assumed of them.
bool type_is_presentation(uint8_t type) {
  return type == 0 || type == 1 || type == 3;
}

/// CEA-516 §5.2.2's record types. 4 to 15 are reserved, and reporting the
/// number is more use than inventing a name for it.
std::string record_type_name(uint8_t type) {
  switch (type) {
    case 0:
      return "Cyclic presentation";
    case 1:
      return "Non-cyclic presentation";
    case 2:
      return "Application";
    case 3:
      return "Priority presentation";
    default:
      break;
  }
  return "Reserved (" + std::to_string(static_cast<int>(type)) + ")";
}

/// Short form of a record type for the list, which has no room for the full
/// name.
std::string short_type_name(uint8_t type) {
  switch (type) {
    case 0:
      return "Cyclic";
    case 1:
      return "Non-cyclic";
    case 2:
      return "Application";
    case 3:
      return "Priority";
    default:
      break;
  }
  return "Type " + std::to_string(static_cast<int>(type));
}

std::string to_upper(std::string text) {
  for (char& character : text) {
    if (character >= 'a' && character <= 'z') {
      character = static_cast<char>(character - 'a' + 'A');
    }
  }
  return text;
}

/// §5.2.1: channel, record address and version together are the identity, so
/// all three are shown — two records at one address in different versions are
/// different records.
std::string record_identity(const NabtsCataloguedRecord& record) {
  char version[8];
  std::snprintf(version, sizeof(version), "%X", record.version & 0xF);
  return to_upper(record.channel_text) + " v" + version;
}

std::string frame_range(uint64_t first, uint64_t last) {
  // Frame numbers are 1-based in the UI; the dataset carries them 0-based.
  const uint64_t from = first + 1;
  const uint64_t to = last + 1;
  if (from == to) {
    return std::to_string(from);
  }
  return std::to_string(from) + "-" + std::to_string(to);
}

/// Zero-padded decimal, for the fixed-width fields of a timecode.
std::string padded(int64_t value, size_t width) {
  std::string text = std::to_string(value);
  while (text.size() < width) {
    text.insert(text.begin(), '0');
  }
  return text;
}

/// A frame index as a wall-clock position, HH:MM:SS.mmm.
std::string frame_time(uint64_t frame) {
  const double seconds_total =
      static_cast<double>(frame) / kNtscFramesPerSecond;
  const auto millis =
      static_cast<int64_t>(std::llround(seconds_total * 1000.0));
  return padded(millis / 3600000, 2) + ":" + padded((millis / 60000) % 60, 2) +
         ":" + padded((millis / 1000) % 60, 2) + "." + padded(millis % 1000, 3);
}

std::string plural(uint64_t count, const char* singular, const char* many) {
  return std::to_string(count) + " " + (count == 1 ? singular : many);
}

std::string join(const std::vector<std::string>& parts,
                 const std::string& separator) {
  if (parts.empty()) {
    return {};
  }
  std::string text = parts.front();
  for (size_t i = 1; i < parts.size(); ++i) {
    text += separator + parts[i];
  }
  return text;
}

/// The classification flags a record declared (§5.2.7.2), as a list.
std::vector<std::string> classification_flags(
    const NabtsCataloguedRecord& record) {
  std::vector<std::string> flags;
  if (record.caption) flags.emplace_back("caption");
  if (record.cyclic_marker) flags.emplace_back("cyclic marker");
  if (record.priority) flags.emplace_back("priority");
  if (record.alarm) flags.emplace_back("alarm");
  if (record.update) flags.emplace_back("update");
  if (record.support_record) flags.emplace_back("support record");
  if (record.support_needed) flags.emplace_back("support needed");
  if (record.index) flags.emplace_back("index");
  if (record.more) flags.emplace_back("more");
  return flags;
}

/**
 * @brief Whether the repair altered this record on the way to the screen
 *
 * The three counters between them cover everything the pass can do to a record
 * (see naplps_lint_repair.h). All zero on a page presented as transmitted, so
 * this is false throughout with the repair turned off — which is what a reader
 * toggling it expects to see.
 */
bool repair_touched(const NabtsCataloguedRecord& record) {
  if (!type_is_presentation(record.record_type)) {
    return false;
  }
  const auto& diagnostics = record.page.diagnostics;
  return diagnostics.repaired_bytes > 0 ||
         diagnostics.resynchronised_pdis > 0 ||
         diagnostics.dropped_coordinate_words > 0;
}

std::string record_condition(const NabtsCataloguedRecord& record,
                             bool presentation) {
  std::vector<std::string> parts;
  if (!record.complete) {
    // §5.2.6: a message missing a link renders short, and there is no way to
    // tell that from a record the service meant to be short.
    parts.emplace_back("incomplete");
  } else if (record.times_intact == 0) {
    // No copy of this record ever arrived clean, so what is shown was voted for
    // rather than picked out — and how many copies voted is how much weight the
    // reading carries.
    parts.emplace_back(record.copies_voted > 1
                           ? "no undamaged copy, combined from " +
                                 plural(record.copies_voted, "copy", "copies")
                           : std::string("no undamaged copy"));
  } else {
    parts.emplace_back("complete");
  }
  if (record.records_in_message > 1) {
    parts.push_back(
        plural(record.records_in_message, "linked record", "linked records"));
  }
  parts.push_back(plural(record.data.size(), "byte", "bytes"));

  if (presentation) {
    const auto& diagnostics = record.page.diagnostics;
    if (diagnostics.truncated_pdis > 0) {
      parts.push_back(plural(diagnostics.truncated_pdis,
                             "truncated instruction",
                             "truncated instructions"));
    }
    if (diagnostics.unresolved_macros > 0) {
      parts.push_back(plural(diagnostics.unresolved_macros, "unresolved macro",
                             "unresolved macros"));
    }
    if (diagnostics.storage_refusals > 0) {
      parts.push_back(
          plural(diagnostics.storage_refusals, "definition", "definitions") +
          " over the storage budget");
    }

    // What the grammar did to this page on the way to the screen, in the fewest
    // words that still say it: a reader judging what is in front of them needs
    // to know it was altered and roughly how much. The rest — which bytes, at
    // what offsets, against what evidence — is in the log.
    if (diagnostics.repaired_bytes > 0) {
      parts.push_back(plural(diagnostics.repaired_bytes, "byte", "bytes") +
                      " corrected");
    }
    if (diagnostics.resynchronised_pdis > 0) {
      parts.push_back(
          plural(diagnostics.resynchronised_pdis, "drawing", "drawings") +
          " trimmed at a gap");
    }
    if (diagnostics.dropped_coordinate_words > 0) {
      parts.push_back(plural(diagnostics.dropped_coordinate_words,
                             "off-screen point", "off-screen points") +
                      " dropped");
    }
    if (diagnostics.undecided_suspect_bytes > 0) {
      parts.push_back(
          plural(diagnostics.undecided_suspect_bytes, "byte", "bytes") +
          " left in doubt");
    }
    // And the part of it a reader can actually check against what they are
    // looking at. Bytes corrected says how much of the record was altered;
    // this says whether the alteration reached the page, which is the thing
    // in front of them.
    if (diagnostics.repaired_bytes > 0 || diagnostics.resynchronised_pdis > 0 ||
        diagnostics.dropped_coordinate_words > 0) {
      parts.emplace_back(diagnostics.repair_changed_drawing
                             ? "the page draws differently"
                             : "the page draws as it arrived");
    }
  }
  return join(parts, ", ");
}

/// An application record's descriptors as a listing (§7.2.2).
std::string function_listing(const NabtsCataloguedRecord& record) {
  if (record.functions.empty()) {
    return "This application record carried no function descriptors.";
  }
  std::vector<std::string> lines;
  lines.push_back(plural(record.functions.size(), "function descriptor",
                         "function descriptors") +
                  " (CEA-516 §7.2.2):");
  lines.emplace_back();
  for (const auto& function : record.functions) {
    const std::string kind = function.control ? "control" : "information";
    // §7.2.3.1: a descriptor with no arguments asks for that function's
    // initial state back.
    const std::string arguments = function.arguments.empty()
                                      ? "(restore initial state)"
                                      : function.arguments;
    lines.push_back(function.code + "  [" + kind + "]  " + arguments);
  }
  return join(lines, "\n");
}

/**
 * @brief One line on how the run went, for a reader rather than for a decoder
 *
 * What a reader needs from this is: how much was read, how much came out, and
 * whether they should trust it. The packet, group and block accounting behind
 * those answers — orphaned packets, refused prefixes, corrected blocks — is
 * diagnostic detail: it goes to the run's report and to the log, where someone
 * chasing a bad transfer will look for it, rather than across the foot of a
 * window someone is trying to read a page in.
 */
std::string run_headline(const NabtsRecoverySummary& summary,
                         size_t records_carried) {
  if (summary.frames_analysed == 0 && summary.packets_recovered == 0) {
    return {};
  }

  std::string out = plural(records_carried, "record", "records") +
                    " read from " +
                    plural(summary.frames_analysed, "frame", "frames");

  // Whether to trust it. A recording that lost packets, failed to complete
  // groups or gave up blocks beyond repair is one whose pages may be missing
  // pieces, and that is worth saying in words rather than in counters.
  const bool lossy = summary.lost_packets_estimate > 0 ||
                     summary.groups_incomplete > 0 ||
                     summary.blocks_damaged > 0 || summary.messages_partial > 0;
  if (lossy) {
    out +=
        "; parts of this recording were lost in transfer, so some pages "
        "will be incomplete";
  }
  if (summary.records_truncated) {
    out +=
        "; the list stops at the catalogue limit, so the recording carried "
        "more than is shown";
  }

  // Records the list no longer holds. A reader who saw a longer list before —
  // or who is comparing this against a transfer of the same tape — is owed the
  // reason it is shorter, and "the address was never received as transmitted"
  // is a reason rather than an apology.
  const VbiIdentityReconciliation& identities = summary.identity_reconciliation;
  const uint64_t misread =
      identities.identities_folded + identities.identities_dropped;
  if (misread == 1) {
    out +=
        "; one address was never received as transmitted and has been read as "
        "a misreading of the records above rather than listed as a record of "
        "its own";
  } else if (misread > 1) {
    out += "; " + std::to_string(misread) +
           " addresses were never received as transmitted and have been read "
           "as misreadings of the records above rather than listed as records "
           "of their own";
  }
  return out;
}

/**
 * @brief What a reader is told about another application's data on a channel
 *
 * A recording can carry NABTS that is not teletext at all — a channel of data
 * groups whose type (§4.2.2) the standard reserves. Without this the list is
 * simply empty, which reads as a recovery that failed rather than one that
 * found a service it has no way to read.
 */
std::string foreign_notice(const NabtsForeignGroups& foreign) {
  std::array<char, 8> channel{};
  std::snprintf(channel.data(), channel.size(), "%03X", foreign.channel);
  std::string out = "Channel " + std::string(channel.data()) + " carried " +
                    plural(foreign.groups, "data group", "data groups") +
                    " of type " + std::to_string(foreign.type);
  out += foreign.type == kNabtsPrivateGroupType
             ? ", which CEA-516 reserves for the service provider's private "
               "use"
             : ", a type CEA-516 reserves and never defined";
  out +=
      ": another service's data rather than teletext, in a format the "
      "standard does not describe, so it is not decoded.";
  return out;
}

/**
 * @brief What the syntax repair did to the catalogue being shown
 *
 * A page repaired without saying so is a page a reader cannot judge, and a
 * reader who cannot see that the pass ran cannot tell "it found nothing" from
 * "it never happened". So the notice is written either way — but kept to one
 * clause. How many bytes were corrected in which record is the log's business
 * (and the run report's); what belongs in front of a reader is whether their
 * pages have been altered and how widely.
 */
std::string lint_notice(const std::vector<NabtsCataloguedRecord>& records,
                        bool repair) {
  if (!repair) {
    return "Syntax repair off: pages are read exactly as they were recovered.";
  }

  uint64_t pages = 0;
  uint64_t changed = 0;
  uint64_t redrawn = 0;
  for (const NabtsCataloguedRecord& record : records) {
    if (!type_is_presentation(record.record_type)) {
      continue;
    }
    ++pages;
    const auto& diagnostics = record.page.diagnostics;
    if (diagnostics.repaired_bytes > 0 || diagnostics.resynchronised_pdis > 0 ||
        diagnostics.dropped_coordinate_words > 0) {
      ++changed;
      if (diagnostics.repair_changed_drawing) {
        ++redrawn;
      }
    }
  }

  if (changed == 0) {
    return "Syntax repair on: no page needed altering.";
  }
  // "Corrected" is a claim about the outcome that the counters cannot support:
  // what is known is that the records were altered, and how many of the pages
  // that changed. A reader deciding whether to trust what is in front of them
  // is told both, and told the second in the same breath.
  return "Syntax repair on: " + std::to_string(changed) + " of " +
         std::to_string(pages) + " pages altered, " + std::to_string(redrawn) +
         " of them now drawing differently. Altered pages are marked * in the "
         "list.";
}

}  // namespace

CatalogueDisplayList nabts_page_display_list(const NabtsPageSnapshot& snapshot,
                                             NaplpsRenderMode mode) {
  CatalogueDisplayList out;
  out.aspect_height = kNabtsDisplayAreaHeight;

  // §4.2.2: the guaranteed-visible part of the unit screen fills a display
  // area that a television set gives a 4:3 aspect, over a pixel grid that is
  // not square on screen (Table D1 item 10). Stating the two separately is what
  // puts a rectangular receiver pixel on screen as a rectangle.
  out.display_aspect_height = kNaplpsDisplayAspectHeight;

  // The receiver the page is resolved against. Every mode emits the same
  // geometry and differs only in the grid it carries, which is what sizes
  // everything §5.3.2.2.6 measures in the receiver's pixels — stroke width
  // above all. A mode that emits pixels rather than geometry
  // (naplps_mode_emits_pixels) would deposit the page into this grid and
  // emit the result instead; that is not built yet, so the pixel modes are
  // presently the vector emission at their own scale.
  const NaplpsRenderGrid grid = naplps_render_grid(mode);
  out.nominal_width = grid.width;
  out.nominal_height = grid.height;

  if (naplps_mode_emits_pixels(mode)) {
    // A receiver deposits the page into its frame buffer and displays the
    // pixels; the emitted list is those pixels, as runs a renderer can scale.
    // Everything the standard measures in physical pixels — stroke width, the
    // dot and dash lengths of §5.3.2.4.2, hatch spacing, the incremental raster
    // — comes out exact rather than approximated, which is the whole point of
    // naming a receiver.
    naplps_emit_raster_page(snapshot, grid, out);
    return out;
  }

  out.palette.reserve(kNabtsColourMapEntries);
  for (size_t i = 0; i < kNabtsColourMapEntries; ++i) {
    out.palette.push_back(to_colour(snapshot.colour_map[i]));
  }

  // The glyph list is indexed by the operations, so it is built first and the
  // code position a character was defined at maps to its place in the list.
  std::vector<int> glyph_slot(1u << 8, -1);
  for (const auto& glyph : snapshot.drcs) {
    if (!glyph.defined()) {
      continue;  // §5.6: a character never defined is displayed as SPACE
    }
    CatalogueBitmap bitmap;
    bitmap.width = glyph.width;
    bitmap.height = glyph.height;
    bitmap.elements = glyph.elements;
    glyph_slot[glyph.code] = static_cast<int>(out.glyphs.size());
    out.glyphs.push_back(std::move(bitmap));
  }

  out.fill_masks.reserve(kNabtsTextureMaskCount);
  for (size_t i = 0; i < kNabtsTextureMaskCount; ++i) {
    const NabtsTextureMask& mask = snapshot.texture_masks[i];
    CatalogueBitmap bitmap;
    bitmap.width = mask.width;
    bitmap.height = mask.height;
    bitmap.elements = mask.elements;
    out.fill_masks.push_back(std::move(bitmap));
  }

  TextRun run;
  // A non-spacing mark arrives before the letter it modifies and shares its
  // origin, so it is held until that letter arrives and composed onto it —
  // Unicode's order, which is the reverse of the transmission's.
  std::string pending_marks;

  const auto flush_run = [&] {
    if (run.open()) {
      out.ops.push_back(run.take());
    }
  };

  for (const NabtsPrimitive& primitive : snapshot.primitives) {
    if (primitive.kind != NabtsPrimitiveKind::kCharacter) {
      flush_run();
      CatalogueDrawOp op;
      op.kind = to_draw_kind(primitive.kind);
      copy_attributes(primitive, op);
      op.points.reserve(primitive.points.size());
      for (const NabtsPoint& point : primitive.points) {
        op.points.push_back(to_point(point));
      }
      op.origin = to_point(primitive.origin);
      op.size = to_size(primitive.size);
      if (op.kind == CatalogueDrawKind::kColourRun) {
        op.colour_run.reserve(primitive.incremental_colours.size());
        for (const uint8_t entry : primitive.incremental_colours) {
          op.colour_run.push_back(
              resolve_incremental(entry, primitive.colour_mode, snapshot));
        }
      }
      out.ops.push_back(std::move(op));
      continue;
    }

    switch (primitive.repertoire) {
      case NabtsPrimitive::Repertoire::kMosaic: {
        flush_run();
        CatalogueDrawOp op;
        op.kind = CatalogueDrawKind::kMosaic;
        copy_attributes(primitive, op);
        op.origin = to_point(primitive.origin);
        op.size = to_size(primitive.size);
        op.points.push_back(op.origin);
        // §5.4: the positions §5.4 does not assign "shall be displayed as
        // SPACE", which is a mosaic with nothing lit.
        op.mosaic_pattern = nabts_is_mosaic_code(primitive.character)
                                ? nabts_mosaic_sixels(primitive.character)
                                : 0;
        // §6.2.7.15: underline mode is what puts mosaics into separated mode.
        op.mosaic_separated = primitive.underlined;
        op.underlined = false;
        out.ops.push_back(std::move(op));
        break;
      }

      case NabtsPrimitive::Repertoire::kDrcs: {
        flush_run();
        CatalogueDrawOp op;
        op.kind = CatalogueDrawKind::kGlyph;
        copy_attributes(primitive, op);
        op.origin = to_point(primitive.origin);
        op.size = to_size(primitive.size);
        op.points.push_back(op.origin);
        op.glyph_index = glyph_slot[primitive.character];
        out.ops.push_back(std::move(op));
        break;
      }

      case NabtsPrimitive::Repertoire::kSupplementary:
        if (nabts_supplementary_is_nonspacing(primitive.character)) {
          const std::string mark = nabts_character_to_utf8(
              primitive.character, primitive.repertoire);
          if (run.open()) {
            // The mark modifies the character the run just took.
            pending_marks += mark;
          } else {
            // A mark with no letter behind it — a record that ended mid
            // composition. Kept rather than dropped: it is what arrived.
            run.start(primitive, mark);
          }
          continue;
        }
        [[fallthrough]];

      case NabtsPrimitive::Repertoire::kPrimary: {
        std::string glyph =
            nabts_character_to_utf8(primitive.character, primitive.repertoire);
        glyph += pending_marks;
        pending_marks.clear();
        if (!run.extend(primitive, glyph)) {
          flush_run();
          run.start(primitive, std::move(glyph));
        }
        break;
      }
    }
  }

  if (!pending_marks.empty() && run.open()) {
    run.compose(pending_marks);
  }
  flush_run();

  return out;
}

namespace {

/// "CCC/AAA" for a chain base address — the short form where the base is one,
/// the nine-digit long form otherwise (§5.2.5).
std::string chain_base_label(uint16_t channel, uint64_t base) {
  char buffer[24];
  if ((base & 0xFFu) == 0 && (base >> 8) <= 0xFFFu) {
    std::snprintf(buffer, sizeof(buffer), "%03X/%03llX", channel & 0xFFF,
                  static_cast<unsigned long long>(base >> 8));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%03X/%09llX", channel & 0xFFF,
                  static_cast<unsigned long long>(base));
  }
  return buffer;
}

}  // namespace

std::vector<CatalogueViewOption> naplps_view_options() {
  return {
      CatalogueViewOption{
          naplps_render_mode_name(NaplpsRenderMode::kReference),
          "256 x 200 (reference receiver)",
          "The receiver the standard's own service reference model describes "
          "(Table D1 item 10), and the one a set-top decoder of the period "
          "displayed. The page as its author would have seen it."},
      CatalogueViewOption{
          naplps_render_mode_name(NaplpsRenderMode::kTwice), "512 x 400",
          "The reference grid at twice the resolution, which Appendix D names "
          "as an example of a receiver that \"may exceed the requirements of "
          "the respective SRM\" and so \"may produce more pleasing images\"."},
      CatalogueViewOption{
          naplps_render_mode_name(NaplpsRenderMode::kThrice), "768 x 600",
          "The reference grid at three times, for reading the finest detail a "
          "page carries. No receiver of the period was this good, and a whole "
          "multiple is what keeps the page's own pixels and letterforms "
          "intact."},
      CatalogueViewOption{
          naplps_render_mode_name(NaplpsRenderMode::kTwiceVector),
          "512 x 400 (vector)",
          "The same geometry drawn as shapes rather than pixels: smooth at any "
          "size, and the clearest reading of what the page describes, but "
          "without the pixel structure a receiver of any resolution had."},
  };
}

CatalogueViewToggle naplps_repair_toggle(bool active) {
  return CatalogueViewToggle{
      kNabtsRepairToggleId, "Syntax repair",
      "NAPLPS is a language with a defined grammar (ANSI X3.110-1983), so a "
      "page recovered from a damaged recording can be checked against it and, "
      "where a byte is known to be wrong, corrected. Only bytes that failed "
      "their parity are ever changed, only where the grammar leaves one "
      "answer, and only where the page still comes out as the page that was "
      "there — a byte the recovery merely felt unsure of is reported and left "
      "alone. Turn it off to read the page as transmitted. Either way this "
      "changes only what is drawn here — the packet stream and record files a "
      "run exports are always the recording's own bytes.",
      active};
}

CatalogueDataset build_nabts_catalogue(const NabtsAnalysisDataset& data,
                                       NaplpsRenderMode mode, bool repair) {
  CatalogueDataset out;

  // The pages are built here rather than by recovery, because what a page looks
  // like depends on the receiver it is drawn for and recovery has no business
  // fixing that: X3.110 §6.2.3 sizes a DRCS character's storage buffer from the
  // physical resolution its character field covers. Interpreting on the way to
  // the screen is what lets the render resolution be changed without reading
  // the recording again.
  std::vector<NabtsCataloguedRecord> records = data.records;
  nabts_interpret_records(records, naplps_render_grid(mode), repair);

  out.schema.columns = {
      CatalogueColumn{"address", "Address", false},
      CatalogueColumn{"type", "Type", false},
      CatalogueColumn{"seen", "Seen", true},
      CatalogueColumn{"frames", "Frames", true},
  };
  out.schema.item_noun = "Record";
  out.schema.variant_noun = "Sub-page";
  out.schema.highlight_label = "Show display area";
  out.schema.empty_message = "No NABTS records were recovered";

  // Which receiver the pages are drawn against is offered to the reader, not
  // only to the project: it changes nothing the recovery found and everything
  // about how a page looks, and the reason to have four of them is to put one
  // beside another.
  out.schema.view_label = "Receiver";
  out.schema.view_options = naplps_view_options();
  out.schema.view_option = naplps_render_mode_name(mode);

  // Whether a damaged page is presented as recovered or as transmitted is the
  // reader's to decide while looking, for the same reason the receiver is: it
  // changes nothing the recovery found, and having the two side by side is how
  // anyone judges whether a repair improved the page or invented it.
  out.schema.toggles = {naplps_repair_toggle(repair)};

  // The More chains (§5.2.7.6) with more than one catalogued member. Each is
  // presented as one page whose members are stepped through as sub-pages —
  // which is what the records are: §5.2.7.8 presents each More Record over
  // the display its predecessor left.
  std::set<std::pair<uint16_t, uint64_t>> chains;
  for (const auto& record : records) {
    if (type_is_presentation(record.record_type) && record.chain_position > 0) {
      chains.insert({record.channel, record.chain_base_address});
    }
  }
  std::set<std::pair<uint16_t, uint64_t>> chain_parents_emitted;

  // Records are listed in catalogue order, except that a chain's members are
  // emitted together behind their page row and ordered by chain position —
  // the order the stepper walks them — rather than by address, which an
  // explicitly-linked chain (§5.2.8.4) need not follow.
  std::map<std::pair<uint16_t, uint64_t>, std::vector<size_t>> members_of;
  for (size_t i = 0; i < records.size(); ++i) {
    const auto& record = records[i];
    if (type_is_presentation(record.record_type)) {
      members_of[{record.channel, record.chain_base_address}].push_back(i);
    }
  }
  std::vector<size_t> emit_order;
  emit_order.reserve(records.size());
  std::vector<bool> scheduled(records.size(), false);
  for (size_t i = 0; i < records.size(); ++i) {
    if (scheduled[i]) {
      continue;
    }
    const auto& record = records[i];
    const std::pair<uint16_t, uint64_t> key{record.channel,
                                            record.chain_base_address};
    if (type_is_presentation(record.record_type) && chains.count(key) > 0) {
      std::vector<size_t> member_indices = members_of[key];
      // Stable, so versions of one position keep their ascending order.
      std::stable_sort(member_indices.begin(), member_indices.end(),
                       [&records](size_t a, size_t b) {
                         return records[a].chain_position <
                                records[b].chain_position;
                       });
      for (const size_t member : member_indices) {
        emit_order.push_back(member);
        scheduled[member] = true;
      }
    } else {
      emit_order.push_back(i);
      scheduled[i] = true;
    }
  }

  for (const size_t record_index : emit_order) {
    const auto& record = records[record_index];
    const bool presentation = type_is_presentation(record.record_type);
    const std::string identity = record_identity(record);

    const std::pair<uint16_t, uint64_t> chain_key{record.channel,
                                                  record.chain_base_address};
    const bool in_chain = presentation && chains.count(chain_key) > 0;
    if (in_chain && chain_parents_emitted.insert(chain_key).second) {
      // The chain's page row, emitted before its first member. It draws
      // nothing itself; the host shows the first sub-page when it is selected.
      uint64_t seen = 0;
      uint64_t first_frame = 0;
      uint64_t last_frame = 0;
      size_t members = 0;
      bool repaired_member = false;
      uint8_t base_type = record.record_type;
      for (const auto& member : records) {
        if (member.channel != record.channel ||
            member.chain_base_address != record.chain_base_address ||
            !type_is_presentation(member.record_type)) {
          continue;
        }
        seen += member.times_seen;
        first_frame = members == 0
                          ? member.first_seen_frame
                          : std::min(first_frame, member.first_seen_frame);
        last_frame = std::max(last_frame, member.last_seen_frame);
        repaired_member = repaired_member || repair_touched(member);
        ++members;
      }
      const std::string label =
          chain_base_label(record.channel, record.chain_base_address);

      CatalogueItem parent;
      // A marker byte keeps the id outside the "CCC/AAA vN" identity space.
      parent.id = std::string(1, '\x02') + label;
      parent.find_key = label;
      parent.values = {
          label,
          short_type_name(base_type),
          std::to_string(seen),
          frame_range(first_frame, last_frame),
      };
      // Marked in the list itself: the members are reached through the
      // sub-page stepper rather than listed as rows, so without this nothing
      // says the page holds more than one screen.
      parent.badges.push_back(plural(members, "sub-page", "sub-pages"));
      // A chain is presented as one page, so the mark belongs on it where any
      // of the records it is built from was altered.
      if (repaired_member) {
        parent.badges.emplace_back("*");
      }
      parent.tooltip =
          "This page is a chain of " + plural(members, "record", "records") +
          " linked as More Records (CEA-516 §5.2.7.6, §5.2.8.4). Each is "
          "presented over the display its predecessor left (§5.2.7.8); step "
          "through them under the page display.";
      out.items.push_back(std::move(parent));
      out.payloads.emplace_back();
    }

    CatalogueItem item;
    item.id = identity;
    item.find_key = identity;
    if (in_chain) {
      item.parent_id =
          std::string(1, '\x02') +
          chain_base_label(record.channel, record.chain_base_address);
      char suffix[16];
      // Positions count in decimal, as §7.3.4 has the address suffix do.
      std::snprintf(suffix, sizeof(suffix), "%02u v%u",
                    record.chain_position % 100, record.version);
      item.variant_label = suffix;
    }
    item.values = {
        identity,
        short_type_name(record.record_type),
        std::to_string(record.times_seen),
        frame_range(record.first_seen_frame, record.last_seen_frame),
    };

    // Marked in the list, so a reader stepping through pages can see which of
    // them the grammar had a hand in without opening each one. What was done
    // is on the condition line under the page; whether it reached the drawing
    // is the part worth knowing, so the mark says which.
    if (repair_touched(record)) {
      item.badges.emplace_back("*");
    }

    std::vector<std::string> tips;
    if (repair_touched(record)) {
      tips.push_back(record.page.diagnostics.repair_changed_drawing
                         ? "* Syntax repair altered this record, and the page "
                           "draws differently for it. Turn the repair off to "
                           "see what arrived."
                         : "* Syntax repair altered this record, though the "
                           "page draws exactly as it arrived.");
    }
    if (!record.reserved_purpose.empty()) {
      // §7.1.5 reserves a handful of channel/address pairings; a reader has no
      // way of knowing which without being told.
      tips.push_back("CEA-516 §7.1.5 reserves this address for: " +
                     record.reserved_purpose);
    }
    const auto flags = classification_flags(record);
    if (!flags.empty()) {
      tips.push_back("Classification flags (§5.2.7.2): " + join(flags, ", "));
    }
    tips.push_back(std::to_string(record.times_intact) + " of " +
                   std::to_string(record.times_seen) +
                   " copies arrived undamaged");
    item.tooltip = join(tips, "\n\n");

    CataloguePayload payload;
    payload.headline =
        "Record " + identity + " (" + record_type_name(record.record_type) +
        ") seen " + plural(record.times_seen, "time", "times") + ", frames " +
        frame_range(record.first_seen_frame, record.last_seen_frame);

    if (presentation) {
      payload.kind = CataloguePayload::Kind::kDisplayList;
      payload.display_list = nabts_page_display_list(record.page, mode);
      // A caption or an index page is usually read rather than looked at, so
      // the text goes beside the drawing. It comes from the snapshot rather
      // than from the display list: reading a record and drawing it want
      // different things out of the same characters, and the sink stage needs
      // the same reading for its caption export.
      payload.companion_text = nabts_page_text(record.page);
    } else {
      payload.kind = CataloguePayload::Kind::kText;
      payload.document.text = function_listing(record);
      payload.document.monospace = true;
    }
    payload.condition = record_condition(record, presentation);

    out.items.push_back(std::move(item));
    out.payloads.push_back(std::move(payload));
  }

  // The caption service as one further item. §7.3.10 carries captioning as
  // records with the Caption Flag of §5.2.7.3 set, each new caption a new
  // version of the same address, and reading them one record at a time tells a
  // viewer nothing about the service.
  const auto cues = nabts_caption_cues(records);
  if (!cues.empty()) {
    CatalogueItem item;
    // Outside the identity space records occupy: a record id is always
    // "<channel>/<address> v<n>", so a leading marker byte cannot collide.
    item.id = std::string(1, '\x01') + "captions";
    item.values = {
        "Caption track", "Captions", std::to_string(cues.size()),
        frame_range(cues.front().start_frame, cues.back().end_frame)};
    item.tooltip =
        "These records were transmitted with the Caption Flag set (CEA-516 "
        "§5.2.7.3), which is what makes a record part of the captioning "
        "service of §7.3.10. Selecting this reads them in order.";

    CataloguePayload payload;
    payload.kind = CataloguePayload::Kind::kTable;
    payload.table.columns = {
        CatalogueColumn{"frames", "Frames", true},
        CatalogueColumn{"time", "Time", false},
        CatalogueColumn{"text", "Caption", false},
    };
    std::vector<std::string> channels;
    for (const auto& cue : cues) {
      char channel[8];
      std::snprintf(channel, sizeof(channel), "%03X", cue.channel & 0xFFF);
      const std::string label =
          std::string(channel) + "/" + to_upper(cue.address_text);
      if (std::find(channels.begin(), channels.end(), label) ==
          channels.end()) {
        channels.push_back(label);
      }
      // A cue is often two lines; the table shows it on one so the list stays
      // scannable.
      std::string text = cue.text;
      std::replace(text.begin(), text.end(), '\n', ' ');
      payload.table.rows.push_back(
          {frame_range(cue.start_frame, cue.end_frame),
           frame_time(cue.start_frame) + " -> " + frame_time(cue.end_frame),
           std::move(text)});
    }
    payload.headline = plural(cues.size(), "caption", "captions") + " on " +
                       join(channels, ", ");

    out.items.push_back(std::move(item));
    out.payloads.push_back(std::move(payload));

    // Which channel carries them, because §7.3.10 makes A00 the entry point
    // but the captions themselves are wherever the Caption Flag is set.
    out.summary.notices.push_back(plural(cues.size(), "caption", "captions") +
                                  " on " + join(channels, ", "));
  }

  // Said whichever way the toggle is set, so the reader can always tell what
  // they are looking at.
  if (!records.empty()) {
    out.summary.notices.push_back(lint_notice(records, repair));
  }
  for (const NabtsForeignGroups& foreign : data.summary.foreign_groups) {
    out.summary.notices.push_back(foreign_notice(foreign));
  }
  if (records.empty() && !data.summary.foreign_groups.empty()) {
    out.schema.empty_message =
        "No teletext records: the NABTS this recording carries is another "
        "service's data";
  }

  out.summary.headline = run_headline(data.summary, records.size());
  return out;
}

}  // namespace orc
