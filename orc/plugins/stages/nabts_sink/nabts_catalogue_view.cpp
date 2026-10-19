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
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "gif_decoder.h"
#include "nabts_datacast_files.h"
#include "nabts_navigation.h"
#include "nabts_raster_view.h"
#include "nabts_record_catalogue.h"
#include "naplps_font.h"
#include "naplps_pdi.h"
#include "naplps_raster.h"
#include "vbi-services/teletext_slicer.h"

namespace orc {

namespace {

// A long record address shifted down to A1 to A5: the hundreds of a short
// page number (§5.2.5 puts PQR at A5 to A7) and every digit above them.
constexpr unsigned kPageHundredsShift = 16;

// Unit-space slack for deciding whether two character origins are a step apart.
// Table D1 item 8 puts the nominal resolution at 256 x 200, so one pixel is
// about 1/256; this is three orders of magnitude below that and exists only to
// absorb the arithmetic of resolving a relative coordinate.
constexpr double kOriginEpsilon = 1e-6;

// A text run draws its character j at origin + j * advance, so a character
// joins it only at the run's own step to within the rounding of the
// interpreter's cursor arithmetic (about 1e-16 a step); one any further off
// starts a run of its own rather than being drawn where it was not sent.
constexpr double kRunStepEpsilon = 1e-12;

// The widest step that still counts as the next character field along the path.
// X3.110 §5.3.2.3.4 Table 8's inter-character spacings are 1, 1.25 and 1.5
// character fields, and proportional spacing is at least one; two fields is
// clear of all of them and well short of a jump to another line.
constexpr double kMaxStepFields = 2.0;

CataloguePoint to_point(const NabtsPoint& point) {
  return CataloguePoint{point.x, point.y};
}

CatalogueSize to_size(const NabtsSize& size) {
  return CatalogueSize{size.dx, size.dy};
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
    case NabtsPrimitiveKind::kScroll:
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
void copy_attributes(const NabtsPrimitive& from,
                     const NaplpsPaletteTimeline& palette,
                     CatalogueDrawOp& to) {
  to.filled = from.filled;
  // X3.110 §5.3.2.4.3: a highlighted figure is filled as usual and outlined in
  // nominal black, or in the background colour where there is one.
  to.outlined = from.highlighted;
  to.pen_size = to_size(from.logical_pel);
  to.line_style = static_cast<CatalogueLineStyle>(from.line_texture);
  to.fill_pattern = static_cast<CatalogueFillPattern>(from.texture_pattern);
  to.fill_mask_size = to_size(from.texture_mask_size);
  to.colour =
      naplps_catalogue_colour(from.colour, from.colour_map_address, palette);
  to.has_background =
      from.colour_mode == NabtsColourMode::kMappedWithBackground;
  to.background = naplps_catalogue_colour(from.background,
                                          from.background_map_address, palette);
  to.rotation_degrees = rotation_degrees(from.rotation);
  to.reverse_video = from.reverse_video;
  to.underlined = from.underlined;
  to.appear_ms = from.appear_ms;
}

/// Whether |candidate| could be drawn as part of a run already carrying the
/// attributes of |run|. Drawn at a different moment is different: a run is
/// one call, and a page that writes a word a letter at a time would otherwise
/// show the whole word at the first letter.
bool attributes_match(const CatalogueDrawOp& run,
                      const CatalogueDrawOp& candidate) {
  return run.colour == candidate.colour &&
         run.has_background == candidate.has_background &&
         run.background == candidate.background &&
         run.appear_ms == candidate.appear_ms &&
         run.rotation_degrees == candidate.rotation_degrees &&
         run.reverse_video == candidate.reverse_video &&
         run.underlined == candidate.underlined &&
         std::fabs(run.size.dx - candidate.size.dx) < kOriginEpsilon &&
         std::fabs(run.size.dy - candidate.size.dy) < kOriginEpsilon;
}

/// Whether |late|, listed after |early| in one stage of the presentation,
/// covers every pixel |early| drew: the same figure in the same place — kind,
/// points, extent, pel, texture, fill, outline, characters and orientation all
/// alike — whatever colours each was drawn in, appearing whole at the same
/// moment. A receiver drawing the stage showed only the later one.
bool drawn_over(const CatalogueDrawOp& early, const CatalogueDrawOp& late) {
  const auto same_point = [](const CataloguePoint& a, const CataloguePoint& b) {
    return std::fabs(a.x - b.x) < kOriginEpsilon &&
           std::fabs(a.y - b.y) < kOriginEpsilon;
  };
  const auto same_size = [](const CatalogueSize& a, const CatalogueSize& b) {
    return std::fabs(a.dx - b.dx) < kOriginEpsilon &&
           std::fabs(a.dy - b.dy) < kOriginEpsilon;
  };
  return late.reveal_duration_ms == 0 && early.appear_ms == late.appear_ms &&
         early.kind == late.kind && same_point(early.origin, late.origin) &&
         same_size(early.size, late.size) &&
         std::equal(early.points.begin(), early.points.end(),
                    late.points.begin(), late.points.end(), same_point) &&
         early.filled == late.filled && early.outlined == late.outlined &&
         same_size(early.pen_size, late.pen_size) &&
         early.line_style == late.line_style &&
         early.fill_pattern == late.fill_pattern &&
         same_size(early.fill_mask_size, late.fill_mask_size) &&
         early.has_background == late.has_background &&
         early.colour_run.size() == late.colour_run.size() &&
         early.text == late.text && same_size(early.advance, late.advance) &&
         early.character_count == late.character_count &&
         early.mosaic_pattern == late.mosaic_pattern &&
         early.mosaic_separated == late.mosaic_separated &&
         early.glyph_index == late.glyph_index &&
         early.rotation_degrees == late.rotation_degrees &&
         early.reverse_video == late.reverse_video &&
         early.underlined == late.underlined;
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
  explicit TextRun(const NaplpsPaletteTimeline& palette) : palette_(palette) {}

  bool open() const { return open_; }

  /// Start a run at |primitive|, discarding whatever was there.
  void start(const NabtsPrimitive& primitive, std::string glyph) {
    op_ = CatalogueDrawOp{};
    op_.kind = CatalogueDrawKind::kText;
    copy_attributes(primitive, palette_, op_);
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
    copy_attributes(primitive, palette_, candidate);
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

  CatalogueDrawOp take() {
    open_ = false;
    return std::move(op_);
  }

  /// Drop the run unfinished: a clear removed what it had drawn.
  void discard() { open_ = false; }

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
    return std::fabs(dx - step_x_) < kRunStepEpsilon &&
           std::fabs(dy - step_y_) < kRunStepEpsilon;
  }

  const NaplpsPaletteTimeline& palette_;
  CatalogueDrawOp op_;
  CataloguePoint last_origin_;
  double step_x_ = 0.0;
  double step_y_ = 0.0;
  bool have_step_ = false;
  bool open_ = false;
};

/// Non-spacing marks composed onto a letter: their combining characters, in
/// the order they arrived, which follow the letter in Unicode's order.
std::string combining_marks(const std::vector<const NabtsPrimitive*>& marks) {
  std::string out;
  for (const NabtsPrimitive* mark : marks) {
    out += nabts_character_to_utf8(mark->character, mark->repertoire);
  }
  return out;
}

/**
 * @brief Non-spacing marks with no letter to modify, as text
 *
 * Table 26 codes a diacritical mark standing alone as the mark with SPACE
 * ("S 4/2 2/0"), and §7.1 puts Table 27's marks in the repertoire "when
 * combined with SPACE". The first mark is written as the Unicode spacing
 * character that stands for it, and where Unicode has none — the vector
 * overbar and the slant — as the combining mark over a no-break space, which
 * is how Unicode shows a mark on its own. Further marks follow as combining
 * characters.
 */
std::string marks_standing_alone(
    const std::vector<const NabtsPrimitive*>& marks) {
  std::string out;
  for (size_t i = 0; i < marks.size(); ++i) {
    const char32_t mark = nabts_supplementary_to_unicode(marks[i]->character);
    if (i == 0) {
      const NaplpsMarkForms* forms = naplps_font_mark_forms(mark);
      if (forms != nullptr && forms->spacing != 0) {
        out += nabts_code_point_to_utf8(forms->spacing);
        continue;
      }
      out += nabts_code_point_to_utf8(U'\u00A0');
    }
    out += nabts_code_point_to_utf8(mark);
  }
  return out;
}

/// CEA-516 §5.2.2's record types. 4 to 15 are reserved, and reporting the
/// number is more use than inventing a name for it.
std::string record_type_name(uint8_t type) {
  switch (type) {
    case kNabtsRecordTypeCyclicPresentation:
      return "Cyclic presentation";
    case kNabtsRecordTypeNoncyclicPresentation:
      return "Non-cyclic presentation";
    case kNabtsRecordTypeApplication:
      return "Application";
    case kNabtsRecordTypePriorityPresentation:
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
    case kNabtsRecordTypeCyclicPresentation:
      return "Cyclic";
    case kNabtsRecordTypeNoncyclicPresentation:
      return "Non-cyclic";
    case kNabtsRecordTypeApplication:
      return "Application";
    case kNabtsRecordTypePriorityPresentation:
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

std::string frame_range(uint64_t first, uint64_t last) {
  // Frame numbers are 1-based in the UI; the dataset carries them 0-based.
  const uint64_t from = first + 1;
  const uint64_t to = last + 1;
  if (from == to) {
    return std::to_string(from);
  }
  return std::to_string(from) + "-" + std::to_string(to);
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

/// |parts| as a list in prose: "a", "a and b", "a, b and c".
std::string join_with_and(const std::vector<std::string>& parts) {
  std::string text;
  for (size_t i = 0; i < parts.size(); ++i) {
    text += (i == 0 ? "" : (i + 1 == parts.size() ? " and " : ", ")) + parts[i];
  }
  return text;
}

/// A record's version as its identity ends — "v4", or "v4 (2)" for the second
/// content carried under it — which is how a sub-page of its address is
/// labelled.
std::string version_label(const NabtsCataloguedRecord& record) {
  char version[8];
  std::snprintf(version, sizeof(version), "%X", record.version & 0xF);
  std::string label = std::string("v") + version;
  if (record.content > 0) {
    label += " (" + std::to_string(record.content + 1) + ")";
  }
  return label;
}

/// The tooltip line naming every version of |record|'s address.
std::string versions_tip(
    const NabtsCataloguedRecord& record,
    const std::vector<const NabtsCataloguedRecord*>& versions) {
  std::vector<std::string> named;
  for (const NabtsCataloguedRecord* version : versions) {
    named.push_back(
        version_label(*version) + " (frames " +
        frame_range(version->first_seen_frame, version->last_seen_frame) + ")");
  }
  std::string text = "Address " + to_upper(record.channel_text) +
                     " was transmitted in " + std::to_string(versions.size()) +
                     " versions during the recording: " + join_with_and(named);
  text +=
      ". CEA-516 §5.2.1 makes the version part of a record's identity and "
      "§5.2.7.2 has a service change it (Y16) when it revises the record, so "
      "each version is listed as a record of its own.";
  return text;
}

/**
 * @brief How much of two records of one address must be compared to judge
 *        them one page
 *
 * A service can give a carousel's pages the next version (CEA-516 §5.2.7.2
 * Y16) at every pass, even where the carousel has fewer pages than versions,
 * so one page comes round under several versions. Such records carry the same
 * bytes but where the recording damaged them. A page revised as it was re-sent
 * does not, and the change can be small: the next version of a sports quiz
 * draws its answer, a standings table moves on by a game in three bytes of
 * fifteen hundred, a story's "today" becomes "Mon.". So two records are one
 * page only where they agree on every byte both received unharmed — delivered,
 * and passing its parity — as they stand or slid by up to kNabtsMaxRecordSlip
 * bytes, as a misread header slides a record. A byte with two bits wrong
 * passes parity and so keeps a damaged copy apart from its page, which costs a
 * sub-page too many; taking a revision for a copy would hide a page.
 *
 * The bytes compared must cover kRollingSamePageJudgedPercent of the longer
 * record and never number fewer than kRollingSamePageMinJudged: a record mostly
 * lost could be any page of its carousel. And where the shorter record arrived
 * intact, so its length is its record's (kNabtsSameContentPercent), the longer
 * one more than kNabtsMaxRecordSlip bytes longer carries something it does not.
 */
constexpr unsigned kRollingSamePageJudgedPercent = 50;
constexpr size_t kRollingSamePageMinJudged = 64;

/// Whether byte |position| of |record| arrived unharmed: delivered by a copy
/// and passing its parity.
bool byte_arrived(const NabtsCataloguedRecord& record, size_t position) {
  return position < record.data.size() &&
         (position >= record.data_present.size() ||
          record.data_present[position] != 0) &&
         teletext_odd_parity_valid(record.data[position]);
}

/// Whether records |a| and |b| of one address are one page sent twice (see
/// kRollingSamePageJudgedPercent).
bool same_rolling_page(const NabtsCataloguedRecord& a,
                       const NabtsCataloguedRecord& b) {
  const NabtsCataloguedRecord& shorter = a.data.size() <= b.data.size() ? a : b;
  const size_t longer = std::max(a.data.size(), b.data.size());
  // A record kept from a copy that arrived intact has no mask: every byte of
  // it is its record's, and so is its length.
  if (shorter.times_intact > 0 && shorter.data_present.empty() &&
      longer - shorter.data.size() > kNabtsMaxRecordSlip) {
    return false;
  }
  const auto slip_limit = static_cast<std::ptrdiff_t>(kNabtsMaxRecordSlip);
  for (std::ptrdiff_t slip = -slip_limit; slip <= slip_limit; ++slip) {
    size_t judged = 0;
    bool differ = false;
    for (size_t position = 0; position < a.data.size() && !differ; ++position) {
      const std::ptrdiff_t source =
          static_cast<std::ptrdiff_t>(position) + slip;
      if (source < 0 || !byte_arrived(a, position) ||
          !byte_arrived(b, static_cast<size_t>(source))) {
        continue;
      }
      ++judged;
      differ = a.data[position] != b.data[static_cast<size_t>(source)];
    }
    if (!differ && judged >= kRollingSamePageMinJudged &&
        judged * 100 >= longer * kRollingSamePageJudgedPercent) {
      return true;
    }
  }
  return false;
}

/// One page a rolling address carried, and every record it was sent as.
struct RollingPage {
  /// The record the page is shown from: of those that sent it, the one with
  /// the most copies that arrived intact, then the most bytes received, then
  /// the most copies, then the first to arrive.
  const NabtsCataloguedRecord* shown = nullptr;
  /// Every record that sent the page, in the order they first arrived.
  std::vector<const NabtsCataloguedRecord*> sent_as;
};

/// How many of |record|'s bytes some copy delivered.
size_t bytes_received(const NabtsCataloguedRecord& record) {
  if (record.data_present.empty()) {
    return record.data.size();
  }
  return static_cast<size_t>(
      std::count_if(record.data_present.begin(), record.data_present.end(),
                    [](uint8_t present) { return present != 0; }));
}

/**
 * @brief The distinct pages among a rolling address's records
 *
 * @p members in the order they first arrived. Each record joins the first page
 * one of whose records it is the same page as (same_rolling_page()), or starts
 * a page of its own, so the pages come in the order they first arrived.
 */
std::vector<RollingPage> rolling_pages(
    const std::vector<const NabtsCataloguedRecord*>& members) {
  std::vector<RollingPage> pages;
  for (const NabtsCataloguedRecord* member : members) {
    RollingPage* joined = nullptr;
    for (RollingPage& page : pages) {
      if (std::any_of(page.sent_as.begin(), page.sent_as.end(),
                      [member](const NabtsCataloguedRecord* sent) {
                        return same_rolling_page(*sent, *member);
                      })) {
        joined = &page;
        break;
      }
    }
    if (joined == nullptr) {
      pages.push_back(RollingPage{member, {member}});
      continue;
    }
    joined->sent_as.push_back(member);
    const auto evidence = [](const NabtsCataloguedRecord& record) {
      return std::make_tuple(record.times_intact, bytes_received(record),
                             record.times_seen);
    };
    if (evidence(*member) > evidence(*joined->shown)) {
      joined->shown = member;
    }
  }
  return pages;
}

/// A rolling page's sub-page label: every version it was sent under, in the
/// order they first arrived — "v0, v2, v4 (2)".
std::string rolling_page_label(const RollingPage& page) {
  std::vector<std::string> labels;
  labels.reserve(page.sent_as.size());
  for (const NabtsCataloguedRecord* record : page.sent_as) {
    labels.push_back(version_label(*record));
  }
  return join(labels, ", ");
}

/// The recordings a page's rebuilt bytes came from, as its condition line names
/// them: the first three by name, and how many more beyond them. A reader
/// judging a rebuilt page wants to know which recordings vouched for it.
std::string recordings_named(const std::vector<std::string>& sources) {
  constexpr size_t kMostNamed = 3;
  if (sources.size() <= kMostNamed) {
    return join_with_and(sources);
  }
  std::vector<std::string> named(sources.begin(), sources.begin() + kMostNamed);
  named.push_back(plural(sources.size() - kMostNamed, "other recording",
                         "other recordings"));
  return join_with_and(named);
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
  if (record.auto_acquire) flags.emplace_back("auto acquire");
  return flags;
}

std::string record_condition(const NabtsCataloguedRecord& record,
                             bool presentation) {
  std::vector<std::string> parts;
  if (record.identity_lost) {
    // Said first: it is why the page has no number and starts short.
    parts.push_back("header lost, salvaged from " +
                    plural(record.copies_voted, "copy", "copies"));
  } else if (record.bytes_salvaged > 0) {
    parts.push_back(plural(record.bytes_salvaged, "byte", "bytes") +
                    " from copies whose header was lost");
  }
  // A page salvaged without its header says nothing of its completeness
  // beyond the header's absence, said above.
  if (record.identity_lost) {
    parts.emplace_back("starts short");
  } else if (!record.complete) {
    // §5.2.6: a message missing a link renders short, and there is no way to
    // tell that from a record the service meant to be short.
    parts.emplace_back("incomplete");
  } else if (record.times_intact == 0) {
    // No copy of this record ever arrived whole with its suffix check passed —
    // which on a service that sends no suffix (CEA-516 §3.4) is every copy — so
    // what is shown was voted for rather than picked out, and how many copies
    // voted is how much weight the reading carries.
    parts.emplace_back(record.copies_voted > 1
                           ? "no verified copy, combined from " +
                                 plural(record.copies_voted, "copy", "copies")
                           : std::string("no verified copy"));
  } else {
    parts.emplace_back("complete");
  }
  if (record.records_in_message > 1) {
    parts.push_back(
        plural(record.records_in_message, "linked record", "linked records"));
  }
  parts.push_back(plural(record.data.size(), "byte", "bytes"));
  if (record.classification_pointers > 1) {
    // §8.5.2.7.1: "If a Record is received with Y01 b8 = 1 then the entire
    // Record shall be ignored by FSS receivers."
    parts.push_back("outside the Full Service Standard (" +
                    plural(record.classification_pointers,
                           "classification pointer byte",
                           "classification pointer bytes") +
                    ")");
  }
  if (!record.reserved_flags.empty()) {
    parts.push_back("reserved flags " + join(record.reserved_flags, ", "));
  }

  if (presentation) {
    if (!record.presented_over.empty()) {
      // Said first among the page's conditions: it is why the page shows
      // more than its own record draws.
      parts.push_back("drawn over " + join_with_and(record.presented_over));
    }
    const auto& diagnostics = record.page.diagnostics;
    if (diagnostics.truncated_pdis > 0) {
      parts.push_back(plural(diagnostics.truncated_pdis,
                             "truncated instruction",
                             "truncated instructions"));
    }
    if (diagnostics.parity_skipped > 0) {
      parts.push_back(plural(diagnostics.parity_skipped,
                             "instruction failing parity",
                             "instructions failing parity") +
                      " skipped");
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

    // What rebuilding the lost bytes did to this page on the way to the screen,
    // in the fewest words that still say it: how much the recording lost, and
    // how much of it came back and from where. The rest — which bytes, at what
    // offsets — is in the log.
    if (diagnostics.lost_bytes > 0) {
      parts.push_back(plural(diagnostics.lost_bytes, "byte", "bytes") +
                      " lost in the recording");
      if (diagnostics.filled_bytes > 0) {
        parts.push_back(plural(diagnostics.filled_bytes, "byte", "bytes") +
                        " rebuilt from " +
                        recordings_named(diagnostics.fill_sources));
      }
      if (diagnostics.inferred_bytes > 0) {
        parts.push_back(plural(diagnostics.inferred_bytes, "shift", "shifts") +
                        " put back");
      }
    }
    // Bytes the recording delivered wrong, which parity proves: how many were
    // put right from another record agreeing around them, and from where.
    if (diagnostics.corrected_bytes > 0) {
      parts.push_back(plural(diagnostics.corrected_bytes, "byte", "bytes") +
                      " of " + std::to_string(diagnostics.corrupt_bytes) +
                      " received failing parity corrected from " +
                      recordings_named(diagnostics.fill_sources));
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

  // Lines that carry another signal behind NABTS's run-in and framing code
  // (nabts_lines_carry_no_nabts()): the list is empty on purpose, and an empty
  // list with no reason reads as a recovery that failed.
  if (summary.not_nabts) {
    return "No NABTS in " + plural(summary.frames_analysed, "frame", "frames") +
           ": the lines read carry another signal behind NABTS's run-in and "
           "framing code — of the " +
           std::to_string(summary.packets_recovered) +
           " packets read, a whole prefix arrived on only " +
           std::to_string(summary.packets_prefix_whole) +
           ", where a recording of NABTS gives 70 % or more — so nothing is "
           "listed";
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
  if (summary.captions_dropped > 0) {
    out += "; the caption track stops at its limit, " +
           plural(summary.captions_dropped, "caption", "captions") +
           " short of the recording's";
  }

  // Records the list no longer holds. A reader who saw a longer list before —
  // or who is comparing this against a transfer of the same tape — is owed the
  // reason it is shorter, and "the address was never received as transmitted"
  // is a reason rather than an apology.
  const VbiIdentityReconciliation& identities = summary.identity_reconciliation;
  if (identities.withheld && records_carried > 0) {
    out +=
        "; no address in this recording ever arrived undamaged, so none could "
        "be confirmed against another and every record is listed marked "
        "unconfirmed";
  }
  if (identities.identities_kept > 0) {
    out += "; " + plural(identities.identities_kept, "address", "addresses") +
           " never arrived undamaged but recurred with a page of their own, "
           "and " +
           (identities.identities_kept == 1 ? "is" : "are") +
           " listed marked unconfirmed";
  }
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
  // Removed whether attested or not: another page's packets that arrived on a
  // data channel the service never used (NabtsRecordCatalogue::
  // reconcile_identities()).
  if (identities.identities_misaddressed == 1) {
    out +=
        "; one record on a data channel the service never used was a page's "
        "packets misaddressed there and has been removed";
  } else if (identities.identities_misaddressed > 1) {
    out += "; " + std::to_string(identities.identities_misaddressed) +
           " records on data channels the service never used were pages' "
           "packets misaddressed there and have been removed";
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
      "standard does not describe, listed as its groups arrived.";
  return out;
}

/**
 * @brief What a reader is told about a service that is not teletext
 *
 * The census (NabtsServiceCensus) names what the addresses that carried no
 * teletext carried: a datacast, an encoder's idle fill, a test sequence, a
 * clock. Each is said once, with the addresses it was on and where what it
 * carried is listed, so a list with no records in it — or with fewer than the
 * packet count suggests — says why.
 */
std::string other_service_notice(const NabtsOtherService& service) {
  const bool several =
      service.channels.find_first_of("-,") != std::string::npos;
  std::string what;
  switch (service.kind) {
    case NabtsOtherService::Kind::kFecDatacast:
      what = "an FEC datacast";
      break;
    case NabtsOtherService::Kind::kIdleFill:
      what = "an encoder's idle fill";
      break;
    case NabtsOtherService::Kind::kTestSequence:
      what = "a test sequence";
      break;
    case NabtsOtherService::Kind::kClock:
      what = "clock packets";
      break;
    case NabtsOtherService::Kind::kPrivateText:
      what = "text in data groups of the provider's own";
      break;
  }
  // Where what it carried is listed: a datacast by its addresses, with the
  // files and services read out of it; a provider's own data with the other
  // services'.
  std::string listed = ": not teletext.";
  if (service.kind == NabtsOtherService::Kind::kFecDatacast) {
    listed =
        ": not teletext; each address's datacast is listed, with the files "
        "and services read out of it.";
  } else if (service.kind == NabtsOtherService::Kind::kPrivateText) {
    listed = ": not teletext; listed with the other services' data.";
  }
  return std::string(several ? "Channels " : "Channel ") + service.channels +
         " carried " + what + " (" + service.description + "), " +
         plural(service.packets, "packet", "packets") + listed;
}

/// Whether the page was drawn from rebuilt bytes (naplps_reconstruct.h).
bool reconstruction_touched(const NabtsCataloguedRecord& record) {
  const auto& diagnostics = record.page.diagnostics;
  return diagnostics.filled_bytes > 0 || diagnostics.inferred_bytes > 0 ||
         diagnostics.corrected_bytes > 0;
}

/**
 * @brief What the reconstruction did to the catalogue being shown
 *
 * Written only while it is on, and kept to how widely the pages were touched:
 * which bytes came from where is on each page.
 */
std::string reconstruction_notice(
    const std::vector<NabtsCataloguedRecord>& records, size_t references) {
  uint64_t pages = 0;
  uint64_t holed = 0;
  uint64_t rebuilt = 0;
  uint64_t lost = 0;
  uint64_t filled = 0;
  uint64_t corrupt = 0;
  uint64_t corrected = 0;
  for (const NabtsCataloguedRecord& record : records) {
    if (!nabts_type_is_presentation(record.record_type)) {
      continue;
    }
    ++pages;
    const auto& diagnostics = record.page.diagnostics;
    corrupt += diagnostics.corrupt_bytes;
    corrected += diagnostics.corrected_bytes;
    if (reconstruction_touched(record)) {
      ++rebuilt;
    }
    if (diagnostics.lost_bytes == 0) {
      continue;
    }
    ++holed;
    lost += diagnostics.lost_bytes;
    filled += diagnostics.filled_bytes;
  }
  const std::string from =
      references == 0
          ? std::string("this recording's own records")
          : "this recording and " +
                plural(references, "reference record", "reference records");
  const std::string corrections =
      corrupt == 0
          ? std::string()
          : " " + std::to_string(corrected) + " of " + std::to_string(corrupt) +
                " bytes received failing parity corrected.";
  if (holed == 0) {
    return "Reconstruction on: no page lost any bytes." + corrections +
           (rebuilt > 0 ? " Rebuilt pages are marked ~ in the list." : "");
  }
  return "Reconstruction on: " + std::to_string(holed) + " of " +
         std::to_string(pages) + " pages lost bytes; " +
         std::to_string(filled) + " of " + std::to_string(lost) +
         " lost bytes rebuilt from " + from + "." + corrections + " " +
         std::to_string(rebuilt) +
         " pages redrawn. Rebuilt pages are marked ~ in the list.";
}

/// The line over a record's payload: its identity, its type and how often and
/// when it was seen.
std::string record_headline(const NabtsCataloguedRecord& record) {
  return "Record " + nabts_record_identity(record) + " (" +
         record_type_name(record.record_type) + ") seen " +
         plural(record.times_seen, "time", "times") + ", frames " +
         frame_range(record.first_seen_frame, record.last_seen_frame);
}

/// The digits a page links (NabtsCataloguedRecord::linked_keys), ascending, as
/// the key ids CatalogueItem::keys carries.
std::vector<std::string> linked_key_ids(const NabtsCataloguedRecord& record) {
  std::vector<std::string> keys;
  for (uint8_t key = 0; key < kNaplpsLinkedKeyCount; ++key) {
    if ((record.linked_keys & (1u << key)) != 0) {
      keys.emplace_back(1, static_cast<char>('0' + key));
    }
  }
  return keys;
}

/**
 * @brief The operations of one stage of a page's presentation, as listed
 *
 * A stage is one instant of the presentation clock. A clear within it throws
 * away what the stage drew before, which was never on screen. Characters are
 * coalesced into runs as they arrive (TextRun). X3.110 §7.2 has a non-spacing
 * mark's code precede the letter it modifies, and §5.2 has the mark leave the
 * cursor where it was, so the letter lands in the mark's field: the marks are
 * held until then and composed onto it, taking no field of their own, and
 * marks no letter follows are shown on their own in their spacing form, in
 * the field they were sent to.
 */
class VectorStage {
 public:
  explicit VectorStage(const NaplpsPaletteTimeline& palette) : run_(palette) {}

  /// The operations listed so far in the stage, which a scroll moves.
  std::vector<CatalogueDrawOp>& ops() { return ops_; }

  /// List the text waiting. Whatever is drawn next is not the marks' letter,
  /// so they go down first, where they were sent in the drawing order.
  void flush_text() {
    release_marks();
    if (run_.open()) {
      ops_.push_back(run_.take());
    }
  }

  /// List |op|, after the text waiting.
  void draw(CatalogueDrawOp op) {
    flush_text();
    ops_.push_back(std::move(op));
  }

  /// Hold the non-spacing |mark| for the letter that follows it into its
  /// field. Marks waiting in another field have no letter coming.
  void hold_mark(const NabtsPrimitive& mark) {
    if (!pending_marks_.empty() &&
        !nabts_same_character_field(*pending_marks_.back(), mark)) {
      release_marks();
    }
    pending_marks_.push_back(&mark);
  }

  /// A spacing character, |glyph| its text.
  void add_letter(const NabtsPrimitive& primitive, std::string glyph) {
    if (!pending_marks_.empty() &&
        nabts_same_character_field(*pending_marks_.back(), primitive)) {
      // The letter the marks modify: one character in one field, the marks
      // following it in Unicode's order, which is the reverse of the
      // transmission's. With SPACE the marks stand alone (Table 26).
      glyph = glyph == " " ? marks_standing_alone(pending_marks_)
                           : glyph + combining_marks(pending_marks_);
      pending_marks_.clear();
    } else {
      release_marks();
    }
    add_character(primitive, std::move(glyph));
  }

  /// A clear: nothing the stage listed before it was on screen.
  void clear() {
    pending_marks_.clear();
    run_.discard();
    ops_.clear();
  }

  /**
   * @brief End the stage, its operations appended to |out|
   *
   * A figure the stage draws again in the same place — a page that draws a
   * shape and then redraws it in another colour, as a sparkle or a correction
   * — is wholly covered by the later drawing, so a receiver showed only that.
   * The earlier one is left out of the list: drawn beneath the later with
   * smooth edges, its edge would show as a faint fringe round it.
   */
  void end_into(std::vector<CatalogueDrawOp>& out) {
    flush_text();
    for (size_t i = 0; i < ops_.size(); ++i) {
      bool hidden = false;
      for (size_t j = i + 1; j < ops_.size() && !hidden; ++j) {
        hidden = drawn_over(ops_[i], ops_[j]);
      }
      if (!hidden) {
        out.push_back(std::move(ops_[i]));
      }
    }
    ops_.clear();
  }

 private:
  void add_character(const NabtsPrimitive& primitive, std::string glyph) {
    if (!run_.extend(primitive, glyph)) {
      if (run_.open()) {
        ops_.push_back(run_.take());
      }
      run_.start(primitive, std::move(glyph));
    }
  }

  void release_marks() {
    if (!pending_marks_.empty()) {
      add_character(*pending_marks_.back(),
                    marks_standing_alone(pending_marks_));
      pending_marks_.clear();
    }
  }

  std::vector<CatalogueDrawOp> ops_;
  TextRun run_;
  std::vector<const NabtsPrimitive*> pending_marks_;
};

/// The black display area a clear leaves, swept a row at a time at a
/// receiver's pace.
CatalogueDrawOp clear_op(const NaplpsPaintStep& step, NabtsFillSweep sweep) {
  CatalogueDrawOp clear;
  clear.kind = CatalogueDrawKind::kRectangle;
  clear.filled = true;
  clear.origin = CataloguePoint{0.0, 0.0};
  clear.size = CatalogueSize{1.0, kNabtsDisplayAreaHeight};
  clear.points.push_back(clear.origin);
  clear.colour = naplps_catalogue_colour(kNabtsNominalBlack);
  clear.appear_ms = step.time_ms;
  naplps_reveal_op(clear, step.draw_ms, 0.0, kNabtsDisplayAreaHeight, sweep);
  return clear;
}

/**
 * @brief Scroll what is listed with |primitive|, a kScroll at |time_ms|
 *
 * The display inside the area at |origin| of |size| moves by |points|[0], and
 * what moves out of the area is lost: SCROLL ON text whose cursor runs off the
 * display area (§6.2.7.13-14), and an INCREMENTAL POINT that runs out of rows
 * (§5.3.3.6.3). Where the area is the whole display area — every scroll the
 * recordings send — everything listed so far, in |listed| and in |stage|,
 * moves with it, and the stripe it moves off is filled with what scrolls in,
 * "nominal black in color modes 0 and 1 and the background color in color mode
 * 2" (§6.2.7.13). A list has no operation that moves part of the display, so a
 * scroll of a smaller area is shown unscrolled; the pixel modes scroll it.
 * What moves is moved where it was listed, so a page that scrolls after a WAIT
 * shows its earlier stages at their scrolled places from the start.
 */
void scroll_whole_display(const NabtsPrimitive& primitive, uint32_t time_ms,
                          const NaplpsRenderGrid& grid,
                          const NaplpsPaletteTimeline& palette,
                          std::vector<CatalogueDrawOp>& listed,
                          VectorStage& stage) {
  const double slack_x = grid.pitch_x() / 2.0;
  const double slack_y = grid.pitch_y() / 2.0;
  const bool whole_display =
      !primitive.points.empty() && primitive.origin.x <= slack_x &&
      primitive.origin.y <= slack_y &&
      primitive.origin.x + primitive.size.dx >= 1.0 - slack_x &&
      primitive.origin.y + primitive.size.dy >=
          kNabtsDisplayAreaHeight - slack_y;
  if (!whole_display) {
    return;
  }
  stage.flush_text();
  const NabtsPoint by = primitive.points.front();
  const auto shift = [&by](CatalogueDrawOp& op) {
    op.origin.x += by.x;
    op.origin.y += by.y;
    for (CataloguePoint& point : op.points) {
      point.x += by.x;
      point.y += by.y;
    }
  };
  for (CatalogueDrawOp& op : listed) {
    shift(op);
  }
  for (CatalogueDrawOp& op : stage.ops()) {
    shift(op);
  }
  // A filled rectangle takes in the outline its pel traces, which a renderer
  // lays one pixel of the list's grid past its far edges, so a stripe is given
  // one pixel less there and covers its own pixels only.
  const CatalogueColour ground =
      primitive.colour_mode == NabtsColourMode::kMappedWithBackground
          ? naplps_catalogue_colour(primitive.background,
                                    primitive.background_map_address, palette)
          : naplps_catalogue_colour(kNabtsNominalBlack);
  const auto stripe = [&](double x, double y, double dx, double dy) {
    CatalogueDrawOp op;
    op.kind = CatalogueDrawKind::kRectangle;
    op.filled = true;
    op.origin = CataloguePoint{x, y};
    op.size = CatalogueSize{std::max(0.0, dx - grid.pitch_x()),
                            std::max(0.0, dy - grid.pitch_y())};
    op.points.push_back(op.origin);
    op.colour = ground;
    op.appear_ms = time_ms;
    stage.draw(std::move(op));
  };
  if (by.y != 0.0) {
    const double rows = std::min(std::fabs(by.y), kNabtsDisplayAreaHeight);
    stripe(0.0, by.y > 0.0 ? 0.0 : kNabtsDisplayAreaHeight - rows, 1.0, rows);
  }
  if (by.x != 0.0) {
    const double columns = std::min(std::fabs(by.x), 1.0);
    stripe(by.x > 0.0 ? 0.0 : 1.0 - columns, 0.0, columns,
           kNabtsDisplayAreaHeight);
  }
}

/// One row of §5.3.3.6.3's raster, as a run whose field is that row: its lower
/// left corner, one pel high, the colours left to right.
CatalogueDrawOp colour_run_op(const NabtsPrimitive& primitive,
                              const NaplpsPaletteTimeline& palette) {
  CatalogueDrawOp op;
  op.kind = CatalogueDrawKind::kColourRun;
  copy_attributes(primitive, palette, op);
  const size_t count = primitive.incremental_colours.size();
  const double dx = primitive.logical_pel.dx;
  const double dy = primitive.logical_pel.dy;
  const double span = static_cast<double>(count) * dx;
  op.origin =
      CataloguePoint{std::min(primitive.origin.x, primitive.origin.x + span),
                     std::min(primitive.origin.y, primitive.origin.y + dy)};
  op.size = CatalogueSize{std::fabs(span), std::fabs(dy)};
  op.points.push_back(op.origin);
  op.colour_run.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    // Right to left where the pel runs that way.
    const size_t k = dx < 0.0 ? count - 1 - i : i;
    const int16_t address = k < primitive.incremental_addresses.size()
                                ? primitive.incremental_addresses[k]
                                : int16_t{-1};
    op.colour_run.push_back(naplps_catalogue_colour(
        primitive.incremental_colours[k], address, palette));
  }
  return op;
}

/// A geometric figure: a point, line, arc, rectangle or polygon. A fill the
/// receiver swept across its screen, at its pace, appears a row at a time over
/// the time it took.
CatalogueDrawOp figure_op(const NabtsPrimitive& primitive,
                          const NaplpsPaletteTimeline& palette,
                          NabtsFillSweep sweep) {
  CatalogueDrawOp op;
  op.kind = to_draw_kind(primitive.kind);
  copy_attributes(primitive, palette, op);
  op.points.reserve(primitive.points.size());
  for (const NabtsPoint& point : primitive.points) {
    op.points.push_back(to_point(point));
  }
  op.origin = to_point(primitive.origin);
  op.size = to_size(primitive.size);
  double low = 0.0;
  double high = 0.0;
  if (primitive.draw_ms > 0 && naplps_fill_rows(primitive, low, high)) {
    naplps_reveal_op(op, primitive.draw_ms, low, high, sweep);
  }
  return op;
}

/// An operation of |kind| for the character |primitive|, in its field.
CatalogueDrawOp field_op(CatalogueDrawKind kind,
                         const NabtsPrimitive& primitive,
                         const NaplpsPaletteTimeline& palette) {
  CatalogueDrawOp op;
  op.kind = kind;
  copy_attributes(primitive, palette, op);
  op.origin = to_point(primitive.origin);
  op.size = to_size(primitive.size);
  op.points.push_back(op.origin);
  return op;
}

/// A character too small to present (naplps_character_too_small()): one pixel
/// of the reference grid at the field's origin, as the Norpak TTX 650 draws it
/// and as the pixel modes do.
CatalogueDrawOp too_small_op(const NabtsPrimitive& primitive,
                             const NaplpsPaletteTimeline& palette) {
  CatalogueDrawOp op =
      field_op(CatalogueDrawKind::kRectangle, primitive, palette);
  op.filled = true;
  op.outlined = false;
  op.fill_pattern = CatalogueFillPattern::kSolid;
  op.rotation_degrees = 0;
  op.reverse_video = false;
  op.underlined = false;
  op.size = CatalogueSize{1.0 / 256.0, 1.0 / 256.0};
  return op;
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
  // above all.
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

  // How the colour map changes as the page is shown (§5.3.2.5, §5.3.2.7): the
  // operations name the entries that change, and the list carries how.
  const NaplpsPaletteTimeline palette =
      naplps_palette_timeline(snapshot.timeline);
  naplps_emit_palette(snapshot, palette, out);

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

  VectorStage stage(palette);
  const std::vector<NaplpsPaintStep> stream = naplps_paint_stream(snapshot);
  uint32_t stage_time = stream.empty() ? 0 : stream.front().time_ms;
  for (const NaplpsPaintStep& step : stream) {
    if (step.time_ms != stage_time) {
      stage.end_into(out.ops);
      stage_time = step.time_ms;
    }
    if (step.primitive == nullptr) {
      // A clear. What this stage drew before it was never seen; what earlier
      // stages drew goes under a black display area, which is what a clear
      // leaves.
      stage.clear();
      if (!out.ops.empty()) {
        stage.draw(clear_op(step, snapshot.timeline.fill_sweep));
      }
      continue;
    }
    const NabtsPrimitive& primitive = *step.primitive;

    if (primitive.kind == NabtsPrimitiveKind::kScroll) {
      scroll_whole_display(primitive, step.time_ms, grid, palette, out.ops,
                           stage);
      continue;
    }
    if (primitive.kind == NabtsPrimitiveKind::kIncrementalPoints) {
      stage.draw(colour_run_op(primitive, palette));
      continue;
    }
    if (primitive.kind != NabtsPrimitiveKind::kCharacter) {
      stage.draw(figure_op(primitive, palette, snapshot.timeline.fill_sweep));
      continue;
    }
    if (naplps_character_too_small(primitive, snapshot.drcs)) {
      stage.draw(too_small_op(primitive, palette));
      continue;
    }

    switch (primitive.repertoire) {
      case NabtsPrimitive::Repertoire::kMosaic: {
        CatalogueDrawOp op =
            field_op(CatalogueDrawKind::kMosaic, primitive, palette);
        // §5.4: the positions §5.4 does not assign "shall be displayed as
        // SPACE", which is a mosaic with nothing lit.
        op.mosaic_pattern = nabts_is_mosaic_code(primitive.character)
                                ? nabts_mosaic_sixels(primitive.character)
                                : 0;
        // §6.2.7.15: underline mode is what puts mosaics into separated mode.
        op.mosaic_separated = primitive.underlined;
        op.underlined = false;
        stage.draw(std::move(op));
        break;
      }

      case NabtsPrimitive::Repertoire::kDrcs: {
        CatalogueDrawOp op =
            field_op(CatalogueDrawKind::kGlyph, primitive, palette);
        op.glyph_index = glyph_slot[primitive.character];
        stage.draw(std::move(op));
        break;
      }

      case NabtsPrimitive::Repertoire::kSupplementary:
        if (nabts_supplementary_is_nonspacing(primitive.character)) {
          stage.hold_mark(primitive);
          break;
        }
        [[fallthrough]];

      case NabtsPrimitive::Repertoire::kPrimary:
        stage.add_letter(
            primitive,
            nabts_character_to_utf8(primitive.character, primitive.repertoire));
        break;
    }
  }
  stage.end_into(out.ops);

  return out;
}

namespace {

/// "AAA" for a record address — the short form where the address is one, the
/// nine-digit long form otherwise (§5.2.5).
std::string address_label(uint64_t address) {
  char buffer[24];
  if ((address & 0xFFu) == 0 && (address >> 8) <= 0xFFFu) {
    std::snprintf(buffer, sizeof(buffer), "%03llX",
                  static_cast<unsigned long long>(address >> 8));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%09llX",
                  static_cast<unsigned long long>(address));
  }
  return buffer;
}

/// "CCC/AAA" for an address in a data channel (see address_label()).
std::string chain_base_label(uint16_t channel, uint64_t base) {
  char buffer[8];
  std::snprintf(buffer, sizeof(buffer), "%03X/", channel & 0xFFF);
  return buffer + address_label(base);
}

/// What the record's header extension makes its More Record (§5.2.8.4), or
/// empty where it carries no such field.
std::string more_designation_tip(const NabtsCataloguedRecord& record) {
  if (!record.more_redefined) {
    return {};
  }
  if (!record.has_more_address) {
    char code[8];
    std::snprintf(code, sizeof(code), "%u/%u", (record.more_macro >> 4) & 0xFu,
                  record.more_macro & 0xFu);
    return std::string("Its header makes macro ") + code +
           " its More function (CEA-516 §5.2.8.4), in place of any More "
           "Record (§7.3.4). Macros are not run from here.";
  }
  const std::string target =
      chain_base_label(record.more_channel, record.more_address);
  if (record.more_channel != record.channel) {
    return "Its header names its More Record in another data channel, " +
           target +
           " (CEA-516 §5.2.8.4), which a receiver would change channel to "
           "fetch. It is not presented here as continuing into that record, "
           "and the More Flag's address is not its More Record (§7.3.4).";
  }
  return "Its header names its More Record, " + target +
         " (CEA-516 §5.2.8.4), which §7.3.4 has a receiver fetch in place of "
         "the More Flag's address.";
}

/// The records the header extension names for the NEXT and INDEX keys
/// (§5.2.8.4), or empty where it names neither.
std::string key_designation_tip(const NabtsCataloguedRecord& record) {
  std::vector<std::string> keys;
  if (record.has_next_address) {
    keys.push_back("NEXT " +
                   chain_base_label(record.channel, record.next_address));
  }
  if (record.has_index_address) {
    keys.push_back("INDEX " +
                   chain_base_label(record.channel, record.index_address));
  }
  if (keys.empty()) {
    return {};
  }
  return "Its header names the records its keys fetch, " + join(keys, " and ") +
         " (CEA-516 §5.2.8.2, §5.2.8.4), in place of the next page by number "
         "and the index of its section.";
}

/// What a record sent with nothing to present is for (see structural_role()),
/// or what it carries in place of a page (see not_naplps_role()).
struct StructuralRole {
  /// For the list, where the record would otherwise read as a blank page.
  std::string badge;
  /// For the pane the page would have been drawn in.
  std::string text;
  /// Whether |text| is a listing, set in a fixed-pitch face.
  bool monospace = false;
};

std::string data_as_hex(const NabtsCataloguedRecord& record);

/**
 * @brief Whether a record's code only resets the decoder
 *
 * NSR (ANSI X3.110 §6.1.6.5) puts the code sets, the domain, the text and
 * texture attributes and the colour mode back to their defaults, and with two
 * bytes from columns 4 to 7 after it puts the cursor where they say; SO and SI
 * choose the set the codes after them are read from (§6.1.3); a RESET whose
 * operand bytes ask for nothing, or that has none, resets nothing (§5.3.2.9.3);
 * NUL has no presentation effect (§6.1.4). A record of those codes alone leaves
 * the display as it found it. A service can send one every few seconds
 * whatever page its cycle is on — NSR to the top left twice, SO twice, RESET
 * with no operand bytes, SI twice, twenty NULs — at an address no keypad
 * could ask for. Every byte must have arrived with its parity, so a page
 * that lost its drawing is not taken for one.
 */
bool resets_only(const NabtsCataloguedRecord& record) {
  // A byte from columns 4 to 7: a cursor address after NSR, an operand in the
  // picture description set (§5.3.1).
  const auto numeric = [&record](size_t i) {
    return byte_arrived(record, i) && (record.data[i] & 0x7F) >= 0x40;
  };
  bool picture_set = false;  // SO has invoked G1, the PDI set (§6.1.3.1)
  bool reset = false;
  for (size_t i = 0; i < record.data.size(); ++i) {
    if (!byte_arrived(record, i)) {
      return false;
    }
    const auto code = static_cast<uint8_t>(record.data[i] & 0x7F);
    if (code == kNaplpsNul) {
      continue;
    }
    if (code == kNaplpsNsr) {
      if (numeric(i + 1) && numeric(i + 2)) {
        i += 2;
      }
      picture_set = false;  // the in-use code table back to its default
      reset = true;
    } else if (code == kNaplpsSo || code == kNaplpsSi) {
      picture_set = code == kNaplpsSo;
    } else if (picture_set && code == static_cast<uint8_t>(NaplpsPdi::kReset)) {
      for (; numeric(i + 1); ++i) {
        if ((record.data[i + 1] & 0x3F) != 0) {
          return false;
        }
      }
      reset = true;
    } else {
      return false;
    }
  }
  return reset;
}

/**
 * @brief The role a record with nothing to draw plays, or nullopt
 *
 * A presentation record whose every byte arrived as NUL, with the odd parity
 * §3.3 gives every data byte of a type-zero group, draws nothing: X3.110
 * §6.1.4 makes NUL a transparent control with no presentation effect. Services
 * send such records for what their headers say — the first record of a
 * data-channel cycle (§5.2.7.7), a Support Record with nothing to share
 * (§5.2.7.12), a record that hands the receiver straight on to its More Record
 * (§5.2.7.8) — and a receiver never shows one as a page. A record holding NULs
 * that stand in for bytes no copy delivered is not one of these: its page was
 * lost, not sent empty.
 *
 * So is a record that arrived whole and runs to the end drawing nothing — no
 * figure, character or clear, nothing cut short, skipped or refused, and
 * nothing defined for later (a page played on keys draws nothing either until
 * a key is pressed). A service can open such a record with a few set-up codes
 * rather than NULs — a shift, a colour mode and text attributes — whose Auto
 * Acquire Flag hands the receiver on to the page a viewer actually saw. (A
 * record whose code only resets the decoder is named for that first:
 * resets_only().)
 */
std::optional<StructuralRole> structural_role(
    const NabtsCataloguedRecord& record) {
  constexpr uint8_t kNulWithParity = 0x80;
  if (!nabts_type_is_presentation(record.record_type) || !record.complete ||
      std::any_of(record.data_present.begin(), record.data_present.end(),
                  [](uint8_t present) { return present == 0; })) {
    return std::nullopt;
  }
  const bool nuls_only =
      std::all_of(record.data.begin(), record.data.end(),
                  [](uint8_t byte) { return byte == kNulWithParity; });
  if (!nuls_only && resets_only(record)) {
    StructuralRole role;
    role.badge = "Reset only";
    role.monospace = true;
    role.text =
        "Its code resets the decoder and draws nothing. NSR (ANSI X3.110 "
        "§6.1.6.5) puts the code sets, the domain, the text and texture "
        "attributes and the colour mode back to their defaults, and the cursor "
        "where the two bytes after it say; SO and SI choose the set the codes "
        "after them are read from (§6.1.3); a RESET with no operand bytes, or "
        "none that ask for anything, resets nothing (§5.3.2.9.3); NUL has no "
        "presentation effect (§6.1.4). A receiver shows nothing new for it, so "
        "there is no page to show. Its bytes, parity stripped:\n\n" +
        data_as_hex(record);
    return role;
  }
  // A record that defines macros, DRCS or textures (DEF MACRO to DEF TEXTURE,
  // ESC 4/0 to 4/4 in the 7-bit code, X3.110 §6.2.2-6.2.4) leaves them for
  // what comes after it — a game's keys among them — and is not empty.
  const bool defines =
      record.linked_keys != 0 ||
      std::adjacent_find(
          record.data.begin(), record.data.end(),
          [](uint8_t escape, uint8_t final_byte) {
            const auto code = static_cast<uint8_t>(final_byte & 0x7F);
            return (escape & 0x7F) == kNaplpsEsc &&
                   code >= static_cast<uint8_t>(NaplpsC1::kDefMacro) &&
                   code <= static_cast<uint8_t>(NaplpsC1::kDefTexture);
          }) != record.data.end();
  const NabtsDecodeDiagnostics& diagnostics = record.page.diagnostics;
  const bool draws_nothing =
      !defines && record.chain_position == 0 &&
      record.page.primitives.empty() && record.page.timeline.clears.empty() &&
      record.page.drcs.empty() && diagnostics.truncated_pdis == 0 &&
      diagnostics.rejected_figures == 0 && diagnostics.parity_skipped == 0 &&
      diagnostics.unresolved_macros == 0 && diagnostics.storage_refusals == 0 &&
      std::all_of(record.data.begin(), record.data.end(),
                  [](uint8_t byte) { return teletext_odd_parity_valid(byte); });
  if (!nuls_only && !draws_nothing) {
    return std::nullopt;
  }
  const std::string nuls =
      nuls_only
          ? "Its data is " +
                plural(record.data.size(), "NUL byte", "NUL bytes") +
                ", which ANSI X3.110 §6.1.4 makes a transparent control with "
                "no presentation effect, so there is no page to show."
          : "Its " +
                plural(record.data.size(), "byte arrived whole and draws",
                       "bytes arrived whole and draw") +
                " nothing — no figure, character or clear — so there is no "
                "page to show.";

  StructuralRole role;
  const bool support_address =
      (record.address & 0xFFULL) == 0 && (record.address >> 8) == 0xFFFULL;
  if (record.support_record || support_address) {
    role.badge = "Support Record";
    role.text =
        "Support Record (CEA-516 §5.2.7.12, §7.1.4). A data channel's Support "
        "Record carries the PLPS code its pages share — colour maps, macros, "
        "DRCS — and a receiver executes it before a page that asks for it "
        "(§5.2.7.9) rather than showing it. This one shares nothing. " +
        nuls;
    return role;
  }
  if (record.cyclic_marker) {
    // §5.2.7.7: a Marker Record is a Cyclic Marker Record when it is also of
    // type 3, short-addressed and unlinked.
    const bool cyclic =
        record.record_type == 3 && (record.address & 0xFFULL) == 0 &&
        (record.address >> 8) <= 0xFFFULL && record.records_in_message <= 1;
    role.badge = cyclic ? "Cyclic Marker" : "Marker Record";
    role.text =
        cyclic ? "Cyclic Marker Record (CEA-516 §5.2.7.7): the first record of "
                 "its data channel's cycle (§7.1.2), which a receiver may use "
                 "to give up a search for a record the cycle does not carry. " +
                     nuls
               : "Marker Record (CEA-516 §5.2.7.7). The standard leaves a "
                 "Marker Record that is not a Cyclic Marker Record to the "
                 "service. " +
                     nuls;
    return role;
  }
  if (record.auto_acquire) {
    // §5.2.7.8 hands the receiver on to the More Record of §7.3.4: the one a
    // header extension names, or else the More Flag's.
    std::string target;
    if (record.has_more_address) {
      target = record.more_channel == record.channel
                   ? address_label(record.more_address)
                   : chain_base_label(record.more_channel, record.more_address);
    } else if (!record.more_redefined && record.more) {
      uint64_t successor = 0;
      if (nabts_algorithmic_more_address(record.address, successor)) {
        target = address_label(successor);
      }
    }
    role.badge = target.empty() ? "Auto Acquire" : "Auto Acquire → " + target;
    role.text =
        (target.empty()
             ? std::string(
                   "Its header sets the Auto Acquire Flag (CEA-516 §5.2.7.8), "
                   "which has a receiver present the record's More Record "
                   "(§7.3.4) as soon as this one is done, but it names no More "
                   "Record a receiver could fetch. ")
             : "Its header sets the Auto Acquire Flag (CEA-516 §5.2.7.8), so a "
               "receiver presents its More Record, " +
                   target +
                   ", as soon as this one is done, without the viewer asking: "
                   "this record is a way in to that page. ") +
        nuls;
    return role;
  }
  role.badge = "Empty record";
  role.text =
      "Its header gives it no role — cyclic marker, support record or "
      "Auto Acquire — that would explain sending it empty. " +
      nuls;
  return role;
}

/**
 * @brief The fewest bytes of a record the kind of its data is judged on
 *
 * Below this a record says too little to tell another service's data from a
 * NAPLPS fragment (see not_naplps_role()).
 */
constexpr size_t kNotNaplpsMinBytes = 64;

/**
 * @brief The share of transmission and device controls that makes a record
 *        binary data
 *
 * X3.110 §6.1.4-6.1.5 gives SOH to ACK, DLE, DC1 to DC4, NAK, SYN and ETB no
 * effect on a page, and a NAPLPS encoder has no reason to send them: on the
 * recordings no page carries more than one in twenty-six, and those are pages
 * a damaged tape left mostly wrong bytes of. Random 7-bit bytes carry 14 in
 * 128 (10.9 %); a record at three quarters of that is binary data.
 */
constexpr unsigned kNotNaplpsBinaryControlPercent = 8;

/**
 * @brief How few of the codes a NAPLPS page is built from make a record
 *        plain text
 *
 * A page sets up what it draws with codes only NAPLPS gives that meaning to:
 * SO and SI, SS2 and SS3, ESC, CS, CAN, APS, APH and NSR. Every page of the
 * recordings sends some, but for the continuation of a More chain, which
 * writes over the display its predecessor left. A record with fewer than one
 * in this many bytes, that breaks its text into lines with line feeds, is text
 * for a printer or a terminal.
 */
constexpr size_t kNotNaplpsPlainBytesPerCode = 1000;
constexpr size_t kNotNaplpsPlainMinLineFeeds = 2;

/**
 * @brief Text in teleprinter lines framed by link controls
 *
 * Pages break text with CR LF too — a table can do so dozens of times a page
 * — but never with transmission controls among it; a record with
 * kNotNaplpsFramedMinLineEnds CR LF line ends and
 * kNotNaplpsFramedControlPercent of them is text framed for a data link.
 */
constexpr size_t kNotNaplpsFramedMinLineEnds = 16;
constexpr unsigned kNotNaplpsFramedControlPercent = 2;

/// How the bytes of a record that arrived unharmed — delivered and passing
/// their parity, NUL aside — divide among the roles not_naplps_role() weighs.
struct DataCensus {
  size_t arrived = 0;
  /// Transmission and device controls (naplps_is_transparent_control()).
  size_t link_controls = 0;
  /// Codes only NAPLPS gives a meaning to (kNotNaplpsPlainBytesPerCode).
  size_t naplps_codes = 0;
  size_t line_feeds = 0;
  /// CR LF pairs.
  size_t line_ends = 0;
};

DataCensus data_census(const NabtsCataloguedRecord& record) {
  DataCensus census;
  bool after_carriage_return = false;
  for (size_t i = 0; i < record.data.size(); ++i) {
    const auto code = static_cast<uint8_t>(record.data[i] & 0x7F);
    if (!byte_arrived(record, i) || code == kNaplpsNul) {
      after_carriage_return = false;
      continue;
    }
    ++census.arrived;
    if (naplps_is_transparent_control(code)) {
      ++census.link_controls;
    }
    switch (code) {
      case kNaplpsSo:
      case kNaplpsSi:
      case kNaplpsSs2:
      case kNaplpsSs3:
      case kNaplpsEsc:
      case kNaplpsCs:
      case kNaplpsCan:
      case kNaplpsAps:
      case kNaplpsAph:
      case kNaplpsNsr:
        ++census.naplps_codes;
        break;
      default:
        break;
    }
    if (code == kNaplpsApd) {
      ++census.line_feeds;
      census.line_ends += after_carriage_return ? 1 : 0;
    }
    after_carriage_return = code == kNaplpsApr;
  }
  return census;
}

/// The bytes of |record| at |positions|, in that order, as text, for a record
/// that carries text: printable characters as themselves, a line end as a new
/// line, any other code as <XX>, a byte that failed its parity as U+FFFD, and
/// a stretch no copy delivered as a note of how long it was.
std::string data_as_text(const NabtsCataloguedRecord& record,
                         const std::vector<size_t>& positions) {
  std::string out;
  size_t lost = 0;
  const auto flush_lost = [&out, &lost]() {
    if (lost > 0) {
      out += "[" + plural(lost, "byte", "bytes") + " lost]";
      lost = 0;
    }
  };
  for (size_t k = 0; k < positions.size(); ++k) {
    const size_t i = positions[k];
    if (i < record.data_present.size() && record.data_present[i] == 0) {
      ++lost;
      continue;
    }
    flush_lost();
    const uint8_t byte = record.data[i];
    if (!teletext_odd_parity_valid(byte)) {
      out += "\xEF\xBF\xBD";
      continue;
    }
    const auto code = static_cast<uint8_t>(byte & 0x7F);
    if (code >= 0x20 && code < 0x7F) {
      out.push_back(static_cast<char>(code));
    } else if (code == kNaplpsApd) {
      out.push_back('\n');
    } else if (code == kNaplpsApr) {
      // A CR before a LF ends the line with it; on its own it ends one too.
      const bool line_feed_next =
          k + 1 < positions.size() &&
          (record.data[positions[k + 1]] & 0x7F) == kNaplpsApd &&
          teletext_odd_parity_valid(record.data[positions[k + 1]]);
      if (!line_feed_next) {
        out.push_back('\n');
      }
    } else if (code != kNaplpsNul) {
      char hex[8];
      std::snprintf(hex, sizeof(hex), "<%02X>", code);
      out += hex;
    }
  }
  flush_lost();
  return out;
}

/// |record|'s bytes as text (see the overload above), every one in order.
std::string data_as_text(const NabtsCataloguedRecord& record) {
  std::vector<size_t> positions(record.data.size());
  std::iota(positions.begin(), positions.end(), size_t{0});
  return data_as_text(record, positions);
}

/// |record|'s bytes as a hexadecimal listing, sixteen to a line with the
/// printable ones beside them; parity is stripped, a byte that failed it is
/// "??" and a byte no copy delivered "--".
std::string data_as_hex(const NabtsCataloguedRecord& record) {
  constexpr size_t kPerLine = 16;
  std::string out;
  for (size_t line = 0; line < record.data.size(); line += kPerLine) {
    char offset[24];
    std::snprintf(offset, sizeof(offset), "%06zX ", line);
    std::string hex = offset;
    std::string text;
    for (size_t i = line; i < line + kPerLine; ++i) {
      if (i >= record.data.size()) {
        hex += "   ";
        continue;
      }
      const uint8_t byte = record.data[i];
      if (i < record.data_present.size() && record.data_present[i] == 0) {
        hex += " --";
        text.push_back(' ');
      } else if (!teletext_odd_parity_valid(byte)) {
        hex += " ??";
        text.push_back('.');
      } else {
        const auto code = static_cast<uint8_t>(byte & 0x7F);
        char value[8];
        std::snprintf(value, sizeof(value), " %02X", code);
        hex += value;
        text.push_back(code >= 0x20 && code < 0x7F ? static_cast<char>(code)
                                                   : '.');
      }
    }
    out += hex + "  " + text + "\n";
  }
  return out;
}

/**
 * @brief A Dow Jones & Company test frame
 *
 * A service can carry a test news item of Dow Jones's — "This is a test
 * of the vertical blanking interval as a vehicle for delivery of news and
 * quotation information" — in eight framings, each protecting the same text
 * with check bytes of its own. The checks are what make it look like a
 * wire-service message framed by link controls: they are sums of character
 * codes, not the SOH, STX, ETX and EOT of a wire service's framing (ANPA
 * 1312), and none of those is in it. Taken apart, every copy is laid out the
 * same way:
 *
 * - five bytes that open every copy (7/13 2/1 1/4 0/0 4/0), then the frame's
 *   length in bytes, a sixteen-bit number sent low byte first with the
 *   parity bit in the eighth bit of each byte, so the low byte's own eighth
 *   bit is lost;
 * - the frame: blocks of kDowJonesBlockBytes, the first opening with the
 *   framing number five times and the block count in five decimal digits,
 *   each a Hamming 8/4 byte as CEA-516 §8.2 codes them, then the text in CR
 *   LF lines, filled out with NULs to whole blocks;
 * - an end-of-file code (SUB) and fill.
 *
 * A check is the fourteen-bit sum of the seven-bit codes it covers, sent as
 * its high seven bits and then its low seven, or (framing 3) their exclusive
 * OR; a check byte of 1 is sent as 7/15 rather than as SOH. Each framing
 * (kDowJonesFramings) checks the characters in runs within a block, the
 * columns of a run of blocks with check blocks after them, or both, and in
 * every copy on the tapes every check holds.
 */
constexpr size_t kDowJonesBlockBytes = 27;
constexpr size_t kDowJonesLengthAt = 5;
constexpr size_t kDowJonesFrameAt = 7;
constexpr size_t kDowJonesDigits = 5;

/// One of the eight framings: the runs of characters a block is checked in,
/// and the check blocks that follow a run of data blocks.
struct DowJonesFraming {
  /// Characters a block carries between its checks, each run followed by its
  /// two check bytes where |run_checks|; a block without them is all text.
  std::array<uint8_t, 4> runs;
  size_t run_count;
  bool run_checks;
  /// Data blocks before each set of check blocks, none where |check_blocks|
  /// is zero; one check block is their exclusive OR, two their sum.
  size_t data_blocks;
  size_t check_blocks;
};

constexpr std::array<DowJonesFraming, 8> kDowJonesFramings = {{
    {{27, 0, 0, 0}, 1, false, 0, 0},
    {{25, 0, 0, 0}, 1, true, 0, 0},
    {{27, 0, 0, 0}, 1, false, 3, 1},
    {{27, 0, 0, 0}, 1, false, 5, 2},
    {{25, 0, 0, 0}, 1, true, 5, 2},
    {{25, 0, 0, 0}, 1, true, 8, 2},
    {{25, 0, 0, 0}, 1, true, 13, 2},
    {{5, 5, 5, 4}, 4, true, 5, 2},
}};

/// A Dow Jones test frame, taken apart (see dow_jones_frame()).
struct DowJonesFrame {
  size_t framing = 0;
  size_t blocks = 0;
  /// Checks whose every byte arrived, and how many of them hold.
  size_t checks_judged = 0;
  size_t checks_held = 0;
  /// The positions in the record of the frame's text, checks left out.
  std::vector<size_t> text;
};

/// The low seven bits of |value| as a check byte is sent: 1 as 7/15 (see
/// dow_jones_frame()).
constexpr uint8_t dow_jones_check_byte(uint32_t value) {
  const auto seven = static_cast<uint8_t>(value & 0x7F);
  return seven == 1 ? 0x7F : seven;
}

/**
 * @brief |record| taken apart as a Dow Jones test frame, or nullopt
 *
 * Recognised by its own header rather than its look: the framing number
 * repeated (three of its five digits agreeing), the block count, and the
 * length the record opens with all naming the same frame, which nothing else
 * the services sent could do by chance.
 */
std::optional<DowJonesFrame> dow_jones_frame(
    const NabtsCataloguedRecord& record) {
  const std::vector<uint8_t>& data = record.data;
  if (data.size() < kDowJonesFrameAt + kDowJonesBlockBytes) {
    return std::nullopt;
  }
  const auto arrived = [&record](size_t i) { return byte_arrived(record, i); };
  const auto code = [&data](size_t i) {
    return static_cast<uint8_t>(data[i] & 0x7F);
  };

  std::array<size_t, 16> votes{};
  for (size_t i = 0; i < kDowJonesDigits; ++i) {
    const int digit = teletext_hamming84_decode(data[kDowJonesFrameAt + i]);
    if (digit >= 0) {
      ++votes[static_cast<size_t>(digit)];
    }
  }
  const auto most = std::max_element(votes.begin(), votes.end());
  const auto framing = static_cast<size_t>(most - votes.begin());
  if (*most < 3 || framing < 1 || framing > kDowJonesFramings.size()) {
    return std::nullopt;
  }
  const DowJonesFraming& layout = kDowJonesFramings[framing - 1];

  // The block count follows the framing number: after the number's own two
  // check bytes where the first run of a block is the number's five digits.
  size_t count_at = kDowJonesFrameAt + kDowJonesDigits;
  if (layout.run_checks && layout.runs[0] == kDowJonesDigits) {
    count_at += 2;
  }
  size_t blocks = 0;
  for (size_t i = 0; i < kDowJonesDigits; ++i) {
    const int digit = teletext_hamming84_decode(data[count_at + i]);
    if (digit < 0 || digit > 9) {
      return std::nullopt;
    }
    blocks = blocks * 10 + static_cast<size_t>(digit);
  }
  const size_t length = blocks * kDowJonesBlockBytes;
  const bool length_arrived =
      arrived(kDowJonesLengthAt) && arrived(kDowJonesLengthAt + 1);
  if (blocks == 0 || kDowJonesFrameAt + length > data.size() ||
      (length_arrived && (code(kDowJonesLengthAt) != (length & 0x7F) ||
                          code(kDowJonesLengthAt + 1) != (length >> 8)))) {
    return std::nullopt;
  }

  // Where a block's characters and its run checks lie.
  std::vector<size_t> characters;
  std::vector<std::pair<size_t, size_t>> runs;  // start, characters
  {
    size_t at = 0;
    for (size_t r = 0; r < layout.run_count; ++r) {
      runs.emplace_back(at, layout.runs[r]);
      for (size_t c = 0; c < layout.runs[r]; ++c) {
        characters.push_back(at + c);
      }
      at += layout.runs[r] + (layout.run_checks ? 2 : 0);
    }
  }

  DowJonesFrame frame;
  frame.framing = framing;
  frame.blocks = blocks;
  const auto judge = [&frame](bool held) {
    ++frame.checks_judged;
    frame.checks_held += held ? 1 : 0;
  };
  const size_t group = layout.data_blocks + layout.check_blocks;
  const auto block_at = [](size_t block) {
    return kDowJonesFrameAt + block * kDowJonesBlockBytes;
  };
  size_t header_left = 2 * kDowJonesDigits;
  for (size_t block = 0; block < blocks; ++block) {
    const size_t in_group = group == 0 ? 0 : block % group;
    const bool whole_group = group != 0 && block - in_group + group <= blocks;
    if (whole_group && in_group >= layout.data_blocks) {
      continue;  // a check block, judged with its group below
    }
    const size_t at = block_at(block);
    for (const size_t c : characters) {
      if (header_left > 0) {
        --header_left;
      } else {
        frame.text.push_back(at + c);
      }
    }
    if (layout.run_checks) {
      for (const auto& [start, count] : runs) {
        bool all_arrived =
            arrived(at + start + count) && arrived(at + start + count + 1);
        uint32_t sum = 0;
        for (size_t c = 0; c < count; ++c) {
          all_arrived = all_arrived && arrived(at + start + c);
          sum += code(at + start + c);
        }
        if (all_arrived) {
          judge(code(at + start + count) == dow_jones_check_byte(sum >> 7) &&
                code(at + start + count + 1) == dow_jones_check_byte(sum));
        }
      }
    }
    if (whole_group && in_group + 1 == layout.data_blocks) {
      // The group's check blocks, column by column.
      const size_t first = block + 1 - layout.data_blocks;
      const size_t checks = block_at(block + 1);
      for (const size_t c : characters) {
        bool all_arrived = true;
        uint32_t sum = 0;
        uint32_t exclusive = 0;
        for (size_t b = first; b <= block; ++b) {
          all_arrived = all_arrived && arrived(block_at(b) + c);
          sum += code(block_at(b) + c);
          exclusive ^= code(block_at(b) + c);
        }
        if (layout.check_blocks == 1) {
          if (all_arrived && arrived(checks + c)) {
            judge(code(checks + c) == dow_jones_check_byte(exclusive));
          }
        } else if (all_arrived && arrived(checks + c) &&
                   arrived(checks + kDowJonesBlockBytes + c)) {
          judge(code(checks + c) == dow_jones_check_byte(sum >> 7) &&
                code(checks + kDowJonesBlockBytes + c) ==
                    dow_jones_check_byte(sum));
        }
      }
    }
  }
  return frame;
}

/// What a Dow Jones test frame is, and how this framing checks it.
std::string dow_jones_note(const DowJonesFrame& frame) {
  const DowJonesFraming& layout = kDowJonesFramings[frame.framing - 1];
  std::string checks;
  if (layout.run_checks) {
    std::string runs = std::to_string(layout.runs[0]) + " characters";
    if (layout.run_count > 1) {
      std::vector<std::string> counts;
      for (size_t r = 0; r < layout.run_count; ++r) {
        counts.push_back(std::to_string(layout.runs[r]));
      }
      runs = "run of " + join_with_and(counts) + " characters";
    }
    checks = "two check bytes, the sum of the characters' codes, after every " +
             runs + " of a block";
  }
  if (layout.check_blocks > 0) {
    checks += std::string(checks.empty() ? "" : ", and ") +
              (layout.check_blocks == 1
                   ? "a check block, the exclusive OR of each column, after "
                     "every "
                   : "two check blocks, the sum of each column, after every ") +
              std::to_string(layout.data_blocks) + " blocks";
  }
  std::string note =
      "Its bytes are a test frame of Dow Jones & Company's, which tried the "
      "vertical blanking interval as a way to deliver news and quotations. "
      "It is framed not by a wire service's link controls — none of ANPA "
      "1312's SOH, STX, ETX and EOT is in it — but as " +
      plural(frame.blocks, "block", "blocks") + " of " +
      std::to_string(kDowJonesBlockBytes) +
      " bytes, the first opening with the framing number and the block "
      "count, each digit a Hamming 8/4 byte (CEA-516 §8.2). This is framing " +
      std::to_string(frame.framing) + " of eight, which ";
  if (checks.empty()) {
    note += "carries the text with no checks at all.";
  } else {
    const bool sums = layout.run_checks || layout.check_blocks == 2;
    note +=
        "checks the text with " + checks + "; " +
        (sums ? "a sum is sent as its high seven bits and then its low "
                "seven, and "
              : "") +
        "a check byte of 1 is sent as 7/15 rather than SOH. Those checks "
        "are what read as controls among the text, and " +
        (frame.checks_held == frame.checks_judged
             ? "all " + std::to_string(frame.checks_judged) + " of them hold"
             : std::to_string(frame.checks_held) + " of the " +
                   std::to_string(frame.checks_judged) +
                   " whose bytes arrived hold") +
        ".";
  }
  return note;
}

/**
 * @brief What a presentation record carries in place of NAPLPS, or nullopt
 *
 * CEA-516 §5.2.2 types a record's data as presentation or application, and
 * §6.1 makes presentation data NAPLPS; nothing stops a service sending
 * another kind of data under a presentation type — a news wire, a Dow Jones
 * test page sent in several framings, a text service's sample page, a block
 * of binary data. Read as NAPLPS they draw stray characters, shapes and colour
 * across the screen, which tells a reader nothing — so a record whose unharmed
 * bytes are one of these kinds is listed as data, with a note of why, rather
 * than drawn:
 *
 * - binary data: transmission and device controls make up
 *   kNotNaplpsBinaryControlPercent or more of them;
 * - plain text: it breaks text into lines with line feeds and sends almost
 *   none of the codes NAPLPS draws a page with
 *   (kNotNaplpsPlainBytesPerCode), and it is no continuation of a More chain;
 * - framed text: text in CR LF lines with link controls among it
 *   (kNotNaplpsFramedMinLineEnds).
 *
 * The thresholds are set so that such records are named and a NAPLPS page
 * never is.
 *
 * The Dow Jones test frame is named by its own header before any of that
 * (dow_jones_frame()), and listed as the text it carries with its check bytes
 * taken out and accounted for, since they are what makes the text look
 * garbled.
 */
std::optional<StructuralRole> not_naplps_role(
    const NabtsCataloguedRecord& record) {
  if (!nabts_type_is_presentation(record.record_type)) {
    return std::nullopt;
  }
  const std::string not_naplps =
      "This record is typed as presentation data (CEA-516 §5.2.2), which §6.1 "
      "makes NAPLPS, but it is not NAPLPS. ";
  const std::string listed =
      " A receiver would draw it as stray characters and shapes, so it is "
      "listed here as data rather than drawn, ";
  if (const auto frame = dow_jones_frame(record)) {
    StructuralRole role;
    role.badge = "Not NAPLPS";
    role.monospace = true;
    role.text = not_naplps + dow_jones_note(*frame) + listed +
                "as the text the frame carries, its checks taken out: a line "
                "end starts a new line.\n\n" +
                data_as_text(record, frame->text);
    return role;
  }
  const DataCensus census = data_census(record);
  if (census.arrived < kNotNaplpsMinBytes) {
    return std::nullopt;
  }
  const auto percent = [&census](size_t count) {
    return static_cast<unsigned>((count * 100) / census.arrived);
  };
  const bool binary = census.link_controls * 100 >=
                      census.arrived * kNotNaplpsBinaryControlPercent;
  const bool plain =
      census.naplps_codes * kNotNaplpsPlainBytesPerCode < census.arrived &&
      census.line_feeds >= kNotNaplpsPlainMinLineFeeds &&
      record.chain_position == 0;
  const bool framed = census.line_ends >= kNotNaplpsFramedMinLineEnds &&
                      census.link_controls * 100 >=
                          census.arrived * kNotNaplpsFramedControlPercent;
  if (!binary && !plain && !framed) {
    return std::nullopt;
  }

  StructuralRole role;
  role.badge = "Not NAPLPS";
  role.monospace = true;
  std::string why;
  if (binary && !(framed || plain)) {
    why = std::to_string(percent(census.link_controls)) +
          " % of the bytes that arrived are transmission and device "
          "controls — SOH to ACK, DLE, DC1 to DC4, NAK, SYN and ETB — which "
          "ANSI X3.110 §6.1.4-6.1.5 gives no effect on a page and a NAPLPS "
          "page has no use for. Random bytes hold one in nine: this is "
          "another service's binary data.";
  } else if (plain) {
    why = "Its bytes are plain text in " +
          plural(census.line_feeds + 1, "line", "lines") +
          ", with next to none of the codes a NAPLPS page is drawn with — "
          "shifts into the picture description set, designations, NAPLPS "
          "cursor controls (ANSI X3.110 §6.1) — at most one in a thousand: "
          "text for a printer or a terminal, not a page.";
  } else {
    why = "Its bytes are text in " +
          plural(census.line_ends, "CR LF line", "CR LF lines") + " with " +
          std::to_string(percent(census.link_controls)) +
          " % transmission and device controls among them, which ANSI "
          "X3.110 §6.1.4-6.1.5 gives no effect on a page: text framed for a "
          "data link, not a page.";
  }
  const bool as_text = !binary || framed || plain;
  role.text = not_naplps + why + listed +
              (as_text ? std::string("as text: a line end starts a new line "
                                     "and any other control is shown as "
                                     "<XX>.")
                       : std::string("in hexadecimal, parity stripped.")) +
              "\n\n" + (as_text ? data_as_text(record) : data_as_hex(record));
  return role;
}

/// Answers a page must offer to set a question: a choice is between two or
/// more, and a page that leads to one page drawn over it is only a page that
/// leads to another.
constexpr size_t kQuizMinAnswers = 2;

/**
 * @brief What a page of a quiz played by page number is to the quiz
 *
 * A quiz can be played by typing the numbers its pages offer, on a decoder
 * such as the Panasonic TU-2000x, which did not clear its screen when a page
 * was asked for: a board offers the numbers of its questions, each drawn over
 * the board, and each question offers its answers drawn over the question —
 * wrong ones and a right one.
 * What makes such pages a game is how they are put together rather than what
 * they say: a page drawn straight over a page that offers its number answers
 * that page (NabtsCataloguedRecord::presented_over, offered_pages), where the
 * page offers kQuizMinAnswers or more such answers; and a page that offers
 * that many answers sets a question — the board and the questions answer
 * their own, and copies of the questions that the board's NEXT leads
 * through offer the questions' answers.
 */
struct QuizPage {
  /// The page this one answers, as its identity; empty where it answers none.
  std::string answers;
  /// The answers it offers, where it sets a question; zero otherwise.
  size_t answers_offered = 0;
};

/// The pages of the quizzes |records| hold (QuizPage). Empty for a service
/// with none, which is every service that did not draw one page over another.
std::map<const NabtsCataloguedRecord*, QuizPage> quiz_pages(
    const std::vector<NabtsCataloguedRecord>& records) {
  std::map<std::string, const NabtsCataloguedRecord*> by_identity;
  for (const NabtsCataloguedRecord& record : records) {
    if (nabts_type_is_presentation(record.record_type)) {
      by_identity.emplace(nabts_record_identity(record), &record);
    }
  }
  // The pages drawn straight over a page that offers their numbers, by that
  // page: the last a page is presented over is the one it was asked from.
  std::map<const NabtsCataloguedRecord*,
           std::vector<const NabtsCataloguedRecord*>>
      drawn_over;
  for (const NabtsCataloguedRecord& record : records) {
    if (record.presented_over.empty()) {
      continue;
    }
    const auto under = by_identity.find(record.presented_over.back());
    if (under == by_identity.end() ||
        under->second->channel != record.channel) {
      continue;
    }
    const std::vector<uint64_t>& offered = under->second->offered_pages;
    if (std::find(offered.begin(), offered.end(), record.address) !=
        offered.end()) {
      drawn_over[under->second].push_back(&record);
    }
  }

  std::map<const NabtsCataloguedRecord*, QuizPage> quiz;
  std::set<std::pair<uint16_t, uint64_t>> answer_pages;
  for (const auto& [under, answers] : drawn_over) {
    std::set<uint64_t> numbers;
    for (const NabtsCataloguedRecord* answer : answers) {
      numbers.insert(answer->address);
    }
    if (numbers.size() < kQuizMinAnswers) {
      continue;
    }
    for (const NabtsCataloguedRecord* answer : answers) {
      quiz[answer].answers = nabts_record_identity(*under);
      answer_pages.emplace(answer->channel, answer->address);
    }
  }
  for (const NabtsCataloguedRecord& record : records) {
    if (!nabts_type_is_presentation(record.record_type)) {
      continue;
    }
    std::set<uint64_t> offered;
    for (const uint64_t address : record.offered_pages) {
      if (answer_pages.count({record.channel, address}) > 0) {
        offered.insert(address);
      }
    }
    if (offered.size() >= kQuizMinAnswers) {
      quiz[&record].answers_offered = offered.size();
    }
  }
  return quiz;
}

/// What a page of a quiz is to it, for the list's tooltip.
std::string quiz_tip(const QuizPage& page) {
  std::string tip = std::string("Marked ") + kNabtsQuizGameBadge +
                    ": a page of a quiz played by page number.";
  if (!page.answers.empty()) {
    tip += " It answers " + page.answers +
           ", which offers its number and over which it is drawn.";
  }
  if (page.answers_offered > 0) {
    tip += " It offers " + plural(page.answers_offered, "answer", "answers") +
           ", each a page drawn over the page that offers it.";
  }
  tip +=
      " A quiz is played so on a decoder that did not clear its screen for "
      "a new page: a board whose numbers bring up questions over it, each "
      "question's numbers its answers over the question.";
  return tip;
}

/// The number of a short page, 0 to 999 — the page a viewer typed three
/// digits for (nabts_keypad_number()) — or -1 for any other address.
int short_page_number(uint64_t address) {
  const std::string number = nabts_keypad_number(address);
  return number.size() == 3 ? std::stoi(number) : -1;
}

/// The long address of short page |number|, 0 to 999 (§5.2.5: 0000PQR00).
uint64_t short_page_address(int number) {
  uint64_t address = 0;
  char digits[8];
  std::snprintf(digits, sizeof(digits), "%03d", number);
  nabts_keypad_address(digits, address);
  return address;
}

/// Entries a Master Index must list to be taken for a list of sections; a
/// service's list runs to eight or nine.
constexpr size_t kMasterIndexMinSections = 4;

/**
 * @brief The sections a Master Index lists, by the number of each one's
 *        index, ascending
 *
 * A Master Index (000, which CEA-516 §7.1.5 reserves for it) can list the
 * service's sections by the number of each one's index page — " 1.News /
 * 10.Weather / ..." in one column or two. An entry is a number of one to
 * three digits, not part of a longer number or a price, followed by a full
 * stop and a capital, anywhere in the page's text as the text pane reads it
 * (nabts_page_text()), which reads a screen at a time the words headlines
 * wrote over one another around the list. A page listing at least
 * kMasterIndexMinSections such numbers, not all in a row — a ranking counts
 * 1, 2, 3 — lists sections; page 000 itself is no section of its own. Empty
 * where the text lists none.
 */
std::vector<int> master_index_sections(const std::string& text) {
  const auto digit = [](char c) { return c >= '0' && c <= '9'; };
  std::set<int> sections;
  size_t at = 0;
  while (at < text.size()) {
    if (!digit(text[at])) {
      ++at;
      continue;
    }
    size_t end = at;
    while (end < text.size() && digit(text[end])) {
      ++end;
    }
    const char before = at > 0 ? text[at - 1] : ' ';
    size_t letter = end + 1;
    if (letter < text.size() && text[letter] == ' ') {
      ++letter;
    }
    if (end - at <= 3 &&
        std::string(".,$:/").find(before) == std::string::npos &&
        end < text.size() && text[end] == '.' && letter < text.size() &&
        text[letter] >= 'A' && text[letter] <= 'Z') {
      const int number = std::stoi(text.substr(at, end - at));
      if (number > 0) {
        sections.insert(number);
      }
    }
    at = end;
  }
  bool in_a_row = true;
  for (auto it = sections.begin(); it != sections.end() && in_a_row; ++it) {
    in_a_row = std::next(it) == sections.end() || *std::next(it) == *it + 1;
  }
  if (sections.size() < kMasterIndexMinSections || in_a_row) {
    return {};
  }
  return {sections.begin(), sections.end()};
}

/// Unused page numbers that end a section's run of pages. A service can number
/// each section's pages on from its index and fill the numbers it does not use
/// with bumpers ("Stay tuned for the latest: Finance"); what is put between
/// two sections, such as a special cycle, stands well clear of the section
/// before.
constexpr int kSectionRunMaxGap = 5;

/// A record's address within its data channel: the key the list groups the
/// records of a page by.
using AddressKey = std::pair<uint16_t, uint64_t>;

/// The id of a More chain's page row. A record's id is always
/// "<channel>/<address> v<n>" (nabts_record_identity()), so the marker byte
/// that opens a row's id keeps it outside the records' identity space.
std::string chain_row_id(uint16_t channel, uint64_t chain_base_address) {
  return std::string(1, '\x02') + chain_base_label(channel, chain_base_address);
}

/// The id of a rolling address's page row (see chain_row_id()).
std::string rolling_row_id(const NabtsCataloguedRecord& record) {
  return std::string(1, '\x03') + to_upper(record.channel_text);
}

/// The id of the caption track's item (see chain_row_id()). Two literals, so
/// the escape ends before the name.
constexpr char kCaptionTrackId[] =
    "\x01"
    "captions";

/// The number a viewer typed for |address| (nabts_keypad_number()), as an
/// alias the find box takes for |item|, where the address has one.
void add_keypad_alias(CatalogueItem& item, uint64_t address) {
  const std::string number = nabts_keypad_number(address);
  if (!number.empty()) {
    item.find_aliases.push_back(number);
  }
}

/// What a record is listed as instead of a page, or nullopt for a page: named
/// for what it is where it was sent with nothing to draw (structural_role()),
/// and listed as the data it is where what it was sent with is not NAPLPS
/// (not_naplps_role()).
std::optional<StructuralRole> record_role(const NabtsCataloguedRecord& record) {
  std::optional<StructuralRole> role = structural_role(record);
  if (!role) {
    role = not_naplps_role(record);
  }
  return role;
}

/// How often the records a page row stands for were seen, over which frames,
/// and whether any was drawn from rebuilt bytes.
struct MemberSpan {
  uint64_t seen = 0;
  uint64_t first_frame = 0;
  uint64_t last_frame = 0;
  bool rebuilt = false;
};

/// The span of |members|, which is never empty.
MemberSpan span_of(const std::vector<const NabtsCataloguedRecord*>& members) {
  MemberSpan span;
  span.first_frame = members.front()->first_seen_frame;
  for (const NabtsCataloguedRecord* member : members) {
    span.seen += member->times_seen;
    span.first_frame = std::min(span.first_frame, member->first_seen_frame);
    span.last_frame = std::max(span.last_frame, member->last_seen_frame);
    span.rebuilt = span.rebuilt || reconstruction_touched(*member);
  }
  return span;
}

/**
 * @brief How the list groups the records into pages
 *
 * A More chain (§5.2.7.6) with more than one catalogued member is presented as
 * one page whose members are stepped through as sub-pages — which is what the
 * records are: §5.2.7.8 presents each More Record over the display its
 * predecessor left. So is an address that carried more than one presentation
 * record and is no part of a More chain: a carousel that gives each page in
 * turn the next version (§5.2.7.2 Y16) and uses the versions again when it
 * comes round with new pages, or a page revised as it was re-sent. Its records
 * are stepped through in the order they first arrived, which is how a receiver
 * asked for the address showed them, redrawing the page as each version came
 * round.
 *
 * Holds pointers into its own rolling pages, so it is not copied.
 */
struct PageGroups {
  explicit PageGroups(const std::vector<NabtsCataloguedRecord>& records);
  PageGroups(const PageGroups&) = delete;
  PageGroups& operator=(const PageGroups&) = delete;

  bool in_chain(const NabtsCataloguedRecord& record) const {
    return chains.count({record.channel, record.chain_base_address}) > 0;
  }

  /// The chain bases of the More chains with more than one member.
  std::set<AddressKey> chains;
  /// The presentation records at each chain base, by index, in catalogue
  /// order.
  std::map<AddressKey, std::vector<size_t>> chain_members;
  /// The versions each address was catalogued in. §5.2.1 makes the version
  /// part of a record's identity, so each is a record of its own; this is
  /// what lets their rows say they are revisions of one another.
  std::map<AddressKey, std::vector<const NabtsCataloguedRecord*>> versions;
  /// The rolling addresses, each with its presentation records in the order
  /// they first arrived.
  std::map<AddressKey, std::vector<const NabtsCataloguedRecord*>> rolling;
  /// Each rolling address's distinct pages (rolling_pages()). A carousel with
  /// fewer pages than versions comes round with the same page under the next
  /// version, which a receiver redrew as the same picture: it is one sub-page,
  /// shown once from the best of the records that sent it.
  std::map<AddressKey, std::vector<RollingPage>> rolling_pages_of;
  /// The rolling page each record a page is shown from shows.
  std::map<const NabtsCataloguedRecord*, const RollingPage*> rolling_page_shown;
};

PageGroups::PageGroups(const std::vector<NabtsCataloguedRecord>& records) {
  for (size_t i = 0; i < records.size(); ++i) {
    const NabtsCataloguedRecord& record = records[i];
    versions[{record.channel, record.address}].push_back(&record);
    if (!nabts_type_is_presentation(record.record_type)) {
      continue;
    }
    chain_members[{record.channel, record.chain_base_address}].push_back(i);
    if (record.chain_position > 0) {
      chains.insert({record.channel, record.chain_base_address});
    }
  }
  for (const auto& [address, held] : versions) {
    std::vector<const NabtsCataloguedRecord*> members;
    bool chained = false;
    for (const NabtsCataloguedRecord* member : held) {
      if (nabts_type_is_presentation(member->record_type)) {
        members.push_back(member);
        chained = chained || in_chain(*member);
      }
    }
    if (members.size() < 2 || chained) {
      continue;
    }
    std::stable_sort(
        members.begin(), members.end(),
        [](const NabtsCataloguedRecord* a, const NabtsCataloguedRecord* b) {
          return a->first_seen_frame < b->first_seen_frame;
        });
    rolling_pages_of[address] = rolling_pages(members);
    rolling[address] = std::move(members);
  }
  for (const auto& [address, pages] : rolling_pages_of) {
    for (const RollingPage& page : pages) {
      rolling_page_shown[page.shown] = &page;
    }
  }
}

/**
 * @brief Where each page of the service is in the list
 *
 * So a key pressed on a page or a number it offers can be followed: the
 * address's page row where its records are stepped through as sub-pages (a
 * carousel, the base of a More chain), and the record itself otherwise — the
 * latest to be sent, where the recording carried several.
 */
std::map<AddressKey, std::string> page_item_ids(
    const std::vector<NabtsCataloguedRecord>& records,
    const PageGroups& groups) {
  std::map<AddressKey, const NabtsCataloguedRecord*> latest_at;
  for (const auto& record : records) {
    if (!nabts_type_is_presentation(record.record_type)) {
      continue;
    }
    const NabtsCataloguedRecord*& held =
        latest_at[{record.channel, record.address}];
    if (held == nullptr || record.last_seen_frame > held->last_seen_frame) {
      held = &record;
    }
  }
  std::map<AddressKey, std::string> item_at;
  for (const auto& [key, record] : latest_at) {
    const bool chained = groups.in_chain(*record);
    if (chained && record->chain_position == 0) {
      item_at[key] = chain_row_id(record->channel, record->chain_base_address);
    } else if (!chained && groups.rolling.count(key) > 0) {
      item_at[key] = rolling_row_id(*record);
    } else {
      item_at[key] = nabts_record_identity(*record);
    }
  }
  return item_at;
}

/**
 * @brief The section of the service each page belongs to
 *
 * Where the service said: the page its header extension names as its Index
 * Record (§5.2.8.2 011), or — where the service flags its section indexes
 * instead (§5.2.7.2 Y13) — the flagged page at or below it in the same
 * hundred. A page others name as their index heads its own section.
 *
 * Where neither the header nor a flag says, the section the Master Index's
 * list puts a page in (master_index_sections()). A service may name a page's
 * index in the header on few of its pages, and its Master Index list where
 * each section starts: a page is in the section
 * whose start is the nearest at or below it — a start being an entry of the
 * list, or an index the service's pages name or flag — where the pages from
 * that start to it run on with no more than kSectionRunMaxGap numbers unused
 * between two of them. An entry no page names as its index takes the index
 * the pages of its range name, where they agree (a list's "2.News" taken for
 * the news pages' index 001), and is its own otherwise. The Master Index
 * starts no section of its own, even where section indexes name it as
 * theirs. Only the list's Section
 * column groups pages so: INDEX still leads where the decoder took it, to
 * the Master Index.
 */
class ServiceSections {
 public:
  explicit ServiceSections(const std::vector<NabtsCataloguedRecord>& records);

  /// The section the service names or flags for |record|, or nullopt.
  std::optional<uint64_t> named(const NabtsCataloguedRecord& record) const;

  /// The section the Master Index's list puts |record| in, or nullopt.
  std::optional<uint64_t> listed(const NabtsCataloguedRecord& record) const;

  /// The list's Section column: the section named, else listed, by number.
  std::string text(const NabtsCataloguedRecord& record) const;

 private:
  struct ListedSections {
    // Where a section starts: an entry of the list, or one of the indexes
    // the service's pages name in their headers or flag, which are named.
    std::set<int> starts;
    std::set<int> named;
    // The short pages the recording carried, and the index each one's header
    // names where it names one other than itself and the Master Index.
    std::set<int> pages;
    std::map<int, int> header;
  };

  std::set<AddressKey> named_as_index_;
  std::map<uint16_t, std::set<uint64_t>> index_pages_;
  std::map<uint16_t, ListedSections> listed_;
};

ServiceSections::ServiceSections(
    const std::vector<NabtsCataloguedRecord>& records) {
  for (const auto& record : records) {
    if (!nabts_type_is_presentation(record.record_type)) {
      continue;
    }
    if (record.has_index_address) {
      named_as_index_.insert({record.channel, record.index_address});
    }
    if (record.index) {
      index_pages_[record.channel].insert(record.address);
    }
    if (record.address == 0) {
      std::vector<int> entries =
          master_index_sections(nabts_page_text(record.page));
      ListedSections& sections = listed_[record.channel];
      if (entries.size() > sections.starts.size()) {
        sections.starts = {entries.begin(), entries.end()};
      }
    }
  }
  for (auto& [channel, sections] : listed_) {
    if (sections.starts.empty()) {
      continue;
    }
    for (const auto& record : records) {
      if (record.channel != channel ||
          !nabts_type_is_presentation(record.record_type)) {
        continue;
      }
      const int page = short_page_number(record.address);
      const int index = record.has_index_address
                            ? short_page_number(record.index_address)
                            : -1;
      if (page >= 0) {
        sections.pages.insert(page);
      }
      if (index > 0) {
        sections.named.insert(index);
        if (page >= 0 && index != page) {
          sections.header[page] = index;
        }
      }
      if (record.index && page > 0) {
        sections.named.insert(page);
      }
    }
    sections.starts.insert(sections.named.begin(), sections.named.end());
  }
}

std::optional<uint64_t> ServiceSections::named(
    const NabtsCataloguedRecord& record) const {
  if (named_as_index_.count({record.channel, record.address}) > 0) {
    return record.address;
  }
  if (record.has_index_address) {
    return record.index_address;
  }
  const auto flagged = index_pages_.find(record.channel);
  if (flagged != index_pages_.end()) {
    const auto above = flagged->second.upper_bound(record.address);
    if (above != flagged->second.begin()) {
      const uint64_t index = *std::prev(above);
      if ((index >> kPageHundredsShift) ==
          (record.address >> kPageHundredsShift)) {
        return index;
      }
    }
  }
  return std::nullopt;
}

std::optional<uint64_t> ServiceSections::listed(
    const NabtsCataloguedRecord& record) const {
  const auto found = listed_.find(record.channel);
  const int page = short_page_number(record.address);
  if (found == listed_.end() || page < 0) {
    return std::nullopt;
  }
  const ListedSections& sections = found->second;
  const auto above = sections.starts.upper_bound(page);
  if (above == sections.starts.begin()) {
    return std::nullopt;
  }
  const int start = *std::prev(above);
  int last = start;
  for (auto it = sections.pages.upper_bound(start);
       it != sections.pages.end() && *it <= page; ++it) {
    if (*it - last - 1 > kSectionRunMaxGap) {
      return std::nullopt;
    }
    last = *it;
  }
  if (sections.named.count(start) > 0) {
    return short_page_address(start);
  }
  const int end = above == sections.starts.end() ? start + 1 : *above;
  std::set<int> names;
  for (auto it = sections.header.lower_bound(start);
       it != sections.header.end() && it->first < end; ++it) {
    names.insert(it->second);
  }
  return short_page_address(names.size() == 1 ? *names.begin() : start);
}

std::string ServiceSections::text(const NabtsCataloguedRecord& record) const {
  std::optional<uint64_t> section = named(record);
  if (!section) {
    section = listed(record);
  }
  if (!section) {
    return std::string{};
  }
  const std::string number = nabts_keypad_number(*section);
  return number.empty() ? address_label(*section) : number;
}

/**
 * @brief The order the records are listed in
 *
 * Catalogue order, except that a chain's members are listed together behind
 * their page row and ordered by chain position — the order the stepper walks
 * them — rather than by address, which an explicitly-linked chain (§5.2.8.4)
 * need not follow; and that a rolling address lists one record for each of its
 * distinct pages, in the order the pages first arrived, the records that sent
 * a page again being its versions.
 */
std::vector<size_t> listing_order(
    const std::vector<NabtsCataloguedRecord>& records,
    const PageGroups& groups) {
  std::vector<size_t> order;
  order.reserve(records.size());
  std::vector<bool> scheduled(records.size(), false);
  for (size_t i = 0; i < records.size(); ++i) {
    if (scheduled[i]) {
      continue;
    }
    const auto& record = records[i];
    const bool presentation = nabts_type_is_presentation(record.record_type);
    if (presentation && groups.in_chain(record)) {
      std::vector<size_t> member_indices =
          groups.chain_members.at({record.channel, record.chain_base_address});
      // Stable, so versions of one position keep their ascending order.
      std::stable_sort(member_indices.begin(), member_indices.end(),
                       [&records](size_t a, size_t b) {
                         return records[a].chain_position <
                                records[b].chain_position;
                       });
      for (const size_t member : member_indices) {
        order.push_back(member);
        scheduled[member] = true;
      }
    } else if (presentation &&
               groups.rolling.count({record.channel, record.address}) > 0) {
      for (const RollingPage& page :
           groups.rolling_pages_of.at({record.channel, record.address})) {
        for (const NabtsCataloguedRecord* member : page.sent_as) {
          scheduled[static_cast<size_t>(member - records.data())] = true;
        }
        order.push_back(static_cast<size_t>(page.shown - records.data()));
      }
    } else {
      order.push_back(i);
      scheduled[i] = true;
    }
  }
  return order;
}

/**
 * @brief The catalogue's list, built a record at a time
 *
 * What the list says of a record depends on the records around it: the page
 * it belongs to (PageGroups), the section of the service it is in
 * (ServiceSections), where the pages its keys and numbers lead to are listed
 * (page_item_ids()), and whether it is a page of a game. That is worked out
 * once over every record; then each record is listed in turn, behind the row
 * of the page it is a sub-page of where it is one.
 */
class CatalogueListing {
 public:
  /// Lists |records|, interpreted, which must outlive the listing.
  CatalogueListing(const std::vector<NabtsCataloguedRecord>& records,
                   const NabtsRecoverySummary& summary, NaplpsRenderMode mode);

  /// Every record's item and payload, in listing_order(), each page row
  /// before its first sub-page.
  void append_to(CatalogueDataset& out) const;

 private:
  CatalogueItem chain_row(const NabtsCataloguedRecord& record) const;
  CatalogueItem rolling_row(const NabtsCataloguedRecord& record) const;
  CatalogueItem record_item(const NabtsCataloguedRecord& record, bool in_chain,
                            bool in_rolling,
                            const std::optional<StructuralRole>& role) const;
  std::string record_tooltip(
      const NabtsCataloguedRecord& record, const CatalogueItem& item,
      bool unconfirmed,
      const std::vector<const NabtsCataloguedRecord*>* revisions) const;
  CataloguePayload record_payload(
      const NabtsCataloguedRecord& record,
      const std::optional<StructuralRole>& role) const;
  std::vector<CatalogueLink> links_of(
      const NabtsCataloguedRecord& record) const;
  void mark_games(
      CatalogueItem& parent,
      const std::vector<const NabtsCataloguedRecord*>& members) const;

  const std::vector<NabtsCataloguedRecord>& records_;
  const NaplpsRenderMode mode_;
  // Whether the list keeps records whose address never arrived undamaged
  // (vbi_reconcile_identities()), which are then marked unconfirmed: kept as
  // recurring with content of their own, or kept because no address in the
  // recording arrived undamaged and there was nothing to judge them against.
  const bool keeps_unconfirmed_;
  const bool nothing_attested_;
  const PageGroups groups_;
  const ServiceSections sections_;
  const std::map<AddressKey, std::string> item_at_;
  // The pages of games, each with the mark the list gives it: a page played
  // on the Option keys, and the pages of a quiz played by page number
  // (quiz_pages()). A record named for its role or listed as data is no page
  // of either.
  const std::map<const NabtsCataloguedRecord*, QuizPage> quiz_;
  std::map<const NabtsCataloguedRecord*, std::string> games_;
};

CatalogueListing::CatalogueListing(
    const std::vector<NabtsCataloguedRecord>& records,
    const NabtsRecoverySummary& summary, NaplpsRenderMode mode)
    : records_(records),
      mode_(mode),
      keeps_unconfirmed_(summary.identity_reconciliation.identities_kept > 0 ||
                         summary.identity_reconciliation.withheld),
      nothing_attested_(summary.identity_reconciliation.withheld),
      groups_(records),
      sections_(records),
      item_at_(page_item_ids(records, groups_)),
      quiz_(quiz_pages(records)) {
  for (const auto& record : records) {
    const bool keys = record.linked_keys != 0;
    if (!nabts_type_is_presentation(record.record_type) ||
        (!keys && quiz_.count(&record) == 0) || record_role(record)) {
      continue;
    }
    games_.emplace(&record, keys ? kNabtsKeypadGameBadge : kNabtsQuizGameBadge);
  }
}

void CatalogueListing::append_to(CatalogueDataset& out) const {
  std::set<AddressKey> chain_rows;
  std::set<AddressKey> rolling_rows;
  for (const size_t record_index : listing_order(records_, groups_)) {
    const auto& record = records_[record_index];
    const bool presentation = nabts_type_is_presentation(record.record_type);
    const AddressKey chain_key{record.channel, record.chain_base_address};
    const bool in_chain = presentation && groups_.chains.count(chain_key) > 0;
    if (in_chain && chain_rows.insert(chain_key).second) {
      out.items.push_back(chain_row(record));
      out.payloads.emplace_back();
    }
    const AddressKey address_key{record.channel, record.address};
    const bool in_rolling =
        presentation && !in_chain && groups_.rolling.count(address_key) > 0;
    if (in_rolling && rolling_rows.insert(address_key).second) {
      out.items.push_back(rolling_row(record));
      out.payloads.emplace_back();
    }
    const std::optional<StructuralRole> role = record_role(record);
    out.items.push_back(record_item(record, in_chain, in_rolling, role));
    out.payloads.push_back(record_payload(record, role));
  }
}

/// A page row whose sub-pages hold a game carries its mark as well, since the
/// list shows the row and not the sub-pages.
void CatalogueListing::mark_games(
    CatalogueItem& parent,
    const std::vector<const NabtsCataloguedRecord*>& members) const {
  for (const char* mark : {kNabtsKeypadGameBadge, kNabtsQuizGameBadge}) {
    for (const NabtsCataloguedRecord* member : members) {
      const auto game = games_.find(member);
      if (game != games_.end() && game->second == mark) {
        parent.badges.emplace_back(mark);
        break;
      }
    }
  }
}

/// A More chain's page row, listed before its first member. It draws nothing
/// itself; the host shows the first sub-page when it is selected.
CatalogueItem CatalogueListing::chain_row(
    const NabtsCataloguedRecord& record) const {
  std::vector<const NabtsCataloguedRecord*> members;
  for (const size_t member :
       groups_.chain_members.at({record.channel, record.chain_base_address})) {
    members.push_back(&records_[member]);
  }
  const MemberSpan span = span_of(members);
  const std::string label =
      chain_base_label(record.channel, record.chain_base_address);

  CatalogueItem parent;
  parent.id = chain_row_id(record.channel, record.chain_base_address);
  parent.find_key = label;
  add_keypad_alias(parent, record.chain_base_address);
  parent.values = {
      label,
      short_type_name(record.record_type),
      std::to_string(span.seen),
      frame_range(span.first_frame, span.last_frame),
      sections_.text(record),
  };
  // Marked in the list itself: the members are reached through the sub-page
  // stepper rather than listed as rows, so without this nothing says the
  // page holds more than one screen.
  parent.badges.push_back(plural(members.size(), "sub-page", "sub-pages"));
  // And whether any member was revised during the recording, which puts more
  // than one version of it among them.
  size_t most_versions = 1;
  for (const NabtsCataloguedRecord* member : members) {
    const auto versions =
        groups_.versions.find({member->channel, member->address});
    if (versions != groups_.versions.end()) {
      most_versions = std::max(most_versions, versions->second.size());
    }
  }
  if (most_versions > 1) {
    parent.badges.push_back(plural(most_versions, "version", "versions"));
  }
  // A chain is presented as one page, so the mark belongs on it where any of
  // the records it is built from was altered.
  if (span.rebuilt) {
    parent.badges.emplace_back("~");
  }
  mark_games(parent, members);
  parent.tooltip =
      "This page is a chain of " + plural(members.size(), "record", "records") +
      " linked as More Records (CEA-516 §5.2.7.6, §5.2.8.4). Each is "
      "presented over the display its predecessor left (§5.2.7.8); step "
      "through them under the page display.";
  if (most_versions > 1) {
    parent.tooltip +=
        "\n\nSome of its records were revised during the recording and "
        "arrived in more than one version (CEA-516 §5.2.7.2 Y16); each "
        "version is a sub-page of its own, labelled with its version.";
  }
  return parent;
}

/// A rolling address's page row, listed before its first record. It draws
/// nothing itself; the host shows the first sub-page when it is selected.
CatalogueItem CatalogueListing::rolling_row(
    const NabtsCataloguedRecord& record) const {
  const AddressKey key{record.channel, record.address};
  const std::vector<const NabtsCataloguedRecord*>& members =
      groups_.rolling.at(key);
  const std::vector<RollingPage>& pages = groups_.rolling_pages_of.at(key);
  const MemberSpan span = span_of(members);
  std::set<uint8_t> versions;
  for (const NabtsCataloguedRecord* member : members) {
    versions.insert(member->version);
  }
  // The address as the record itself names it, "CCC/AAA", which is what its
  // sub-pages' identities begin with.
  const std::string label = to_upper(record.channel_text);

  CatalogueItem parent;
  parent.id = rolling_row_id(record);
  parent.find_key = label;
  add_keypad_alias(parent, record.address);
  parent.values = {
      label, short_type_name(record.record_type), std::to_string(span.seen),
      frame_range(span.first_frame, span.last_frame), sections_.text(record)};
  parent.badges.push_back(plural(versions.size(), "version", "versions"));
  if (pages.size() != versions.size()) {
    parent.badges.push_back(plural(pages.size(), "page", "pages"));
  }
  if (span.rebuilt) {
    parent.badges.emplace_back("~");
  }
  mark_games(parent, members);
  parent.tooltip =
      "This address carried " + plural(members.size(), "record", "records") +
      " over the recording: a carousel that gives each page in turn the "
      "next version (CEA-516 §5.2.7.2 Y16), or a page revised as it was "
      "re-sent. A receiver asked for the address redrew it as each "
      "arrived; step through them under the page display, in the order "
      "they first arrived.\n\n" +
      versions_tip(record, members);
  if (pages.size() < members.size()) {
    parent.tooltip +=
        "\n\nA carousel with fewer pages than versions comes round with "
        "the same page under the next version, and a receiver drew the "
        "same picture again. Records that carry the same bytes, but for "
        "what the recording damaged, are one page here: the " +
        plural(members.size(), "record is", "records are") + " " +
        plural(pages.size(), "page", "pages") +
        ", each a sub-page labelled with every version it was sent under.";
  }
  return parent;
}

/**
 * @brief Where the keys of a page and the numbers it offers lead
 *
 * As its decoder took them: NEXT the record the header names (§5.2.8.2 010),
 * else the next page by number (nabts_next_page_address()); MORE the record
 * the header names (001), else the More Flag's (§5.2.7.6); INDEX the record
 * the header names (011), else the page's section index, else the Master
 * Index (000) — the Panasonic TU-2000x's defaults. Only pages the recording
 * carried are linked.
 */
std::vector<CatalogueLink> CatalogueListing::links_of(
    const NabtsCataloguedRecord& record) const {
  std::vector<CatalogueLink> links;
  std::set<std::string> linked;
  const auto add = [&](CatalogueLink::Kind kind, const std::string& label,
                       uint64_t address) {
    if (address == record.address) {
      return false;
    }
    const auto target = item_at_.find({record.channel, address});
    if (target == item_at_.end()) {
      return false;
    }
    if (kind == CatalogueLink::Kind::kReference &&
        !linked.insert(target->second).second) {
      return false;
    }
    links.push_back(CatalogueLink{kind, label, target->second});
    return true;
  };
  uint64_t next = 0;
  if (record.has_next_address) {
    add(CatalogueLink::Kind::kNext, "NEXT", record.next_address);
  } else if (nabts_next_page_address(record.address, next)) {
    add(CatalogueLink::Kind::kNext, "NEXT", next);
  }
  uint64_t more = 0;
  if (record.more_redefined) {
    if (record.has_more_address && record.more_channel == record.channel) {
      add(CatalogueLink::Kind::kMore, "MORE", record.more_address);
    }
  } else if (record.more &&
             nabts_algorithmic_more_address(record.address, more)) {
    add(CatalogueLink::Kind::kMore, "MORE", more);
  }
  const std::optional<uint64_t> section = sections_.named(record);
  if (record.has_index_address) {
    add(CatalogueLink::Kind::kIndex, "INDEX", record.index_address);
  } else if (!(section && *section != record.address &&
               add(CatalogueLink::Kind::kIndex, "INDEX", *section))) {
    add(CatalogueLink::Kind::kIndex, "INDEX", 0);
  }
  for (const uint64_t offered : record.offered_pages) {
    const std::string number = nabts_keypad_number(offered);
    add(CatalogueLink::Kind::kReference, number, offered);
  }
  return links;
}

CatalogueItem CatalogueListing::record_item(
    const NabtsCataloguedRecord& record, bool in_chain, bool in_rolling,
    const std::optional<StructuralRole>& role) const {
  const bool presentation = nabts_type_is_presentation(record.record_type);
  const std::string identity = nabts_record_identity(record);

  CatalogueItem item;
  item.id = identity;
  item.find_key = identity;
  if (in_rolling) {
    item.parent_id = rolling_row_id(record);
    const auto page = groups_.rolling_page_shown.find(&record);
    item.variant_label = page != groups_.rolling_page_shown.end()
                             ? rolling_page_label(*page->second)
                             : version_label(record);
  }
  if (in_chain) {
    item.parent_id = chain_row_id(record.channel, record.chain_base_address);
    char suffix[16];
    // Positions count in decimal, as §7.3.4 has the address suffix do; the
    // version is the hex digit it is everywhere else — the record's own
    // label and the file export_records writes — so a saved image pairs
    // with its record file.
    std::snprintf(suffix, sizeof(suffix), "%02u v%X",
                  record.chain_position % 100, record.version & 0xFu);
    item.variant_label = suffix;
  }
  item.values = {
      identity,
      short_type_name(record.record_type),
      std::to_string(record.times_seen),
      frame_range(record.first_seen_frame, record.last_seen_frame),
      presentation ? sections_.text(record) : std::string{},
  };
  // The number a viewer typed for the page, and where its keys and the
  // numbers it offers lead (links_of()). A page salvaged without its record
  // header has no number, and nothing says where its keys led.
  if (presentation && !record.identity_lost) {
    add_keypad_alias(item, record.address);
    item.links = links_of(record);
  }

  // Marked in the list, so the rows of one address in different versions
  // read as the revisions they are rather than as unrelated records.
  const auto versions =
      groups_.versions.find(std::make_pair(record.channel, record.address));
  const std::vector<const NabtsCataloguedRecord*>* revisions =
      versions != groups_.versions.end() && versions->second.size() > 1
          ? &versions->second
          : nullptr;
  if (revisions != nullptr) {
    item.badges.push_back(plural(revisions->size(), "version", "versions"));
  }

  // Marked in the list, so a reader stepping through pages can see which of
  // them were drawn from rebuilt bytes without opening each one. What was
  // done is on the condition line under the page.
  if (reconstruction_touched(record)) {
    item.badges.emplace_back("~");
  }

  // Kept although its address never once arrived undamaged: it recurred
  // under that address with a page nothing else carries (see
  // vbi_reconcile_identities()).
  const bool unconfirmed =
      record.times_attested == 0 && keeps_unconfirmed_ && !record.identity_lost;
  if (unconfirmed) {
    item.badges.emplace_back("unconfirmed");
  }
  if (record.identity_lost) {
    item.badges.emplace_back("header lost");
  }

  if (role) {
    item.badges.push_back(role->badge);
  }

  // A page played on the keypad: the keys it links to its own macros, which
  // the viewer offers as a keypad of its own (CatalogueItem::keys).
  if (presentation && !role && record.linked_keys != 0) {
    item.keys = linked_key_ids(record);
  }
  const auto game = games_.find(&record);
  if (game != games_.end()) {
    item.badges.push_back(game->second);
  }
  item.tooltip = record_tooltip(record, item, unconfirmed, revisions);
  return item;
}

std::string CatalogueListing::record_tooltip(
    const NabtsCataloguedRecord& record, const CatalogueItem& item,
    bool unconfirmed,
    const std::vector<const NabtsCataloguedRecord*>* revisions) const {
  std::vector<std::string> tips;
  if (!item.keys.empty()) {
    tips.push_back(
        std::string("Marked ") + kNabtsKeypadGameBadge +
        ": this page links keys " + join(item.keys, " ") +
        " to macros it defines and never runs itself, which a decoder of "
        "the day ran as the viewer pressed them (X3.110 §5.5, Table D1 item "
        "7) — a game, played with the Option keys. Press them on the "
        "keypad under the page; Reset game starts it again.");
  }
  // A game page with no keys is a page of a quiz: that is why it is a game.
  if (games_.count(&record) > 0 && record.linked_keys == 0) {
    tips.push_back(quiz_tip(quiz_.at(&record)));
  }
  if (nabts_type_is_presentation(record.record_type) &&
      !sections_.named(record)) {
    const std::optional<uint64_t> section = sections_.listed(record);
    if (section) {
      tips.push_back(
          "Its header names no index, so its section, " +
          nabts_keypad_number(*section) +
          ", is the one the Master Index's list of sections puts it in: "
          "the section whose start is the nearest at or below its number, "
          "the pages from there to it running on.");
    }
  }
  if (!record.reserved_purpose.empty()) {
    // §7.1.5 reserves a handful of channel/address pairings; a reader has no
    // way of knowing which without being told.
    tips.push_back("CEA-516 §7.1.5 reserves this address for: " +
                   record.reserved_purpose);
  }
  if (record.identity_lost) {
    tips.push_back(
        "None of its copies arrived with the packet that starts its data "
        "group, which carries the group and record headers (CEA-516 §4.2, "
        "§5.2): the recording lost it every time, on a line it does not keep "
        "or with its header unreadable. The rest of each copy arrived, on a "
        "channel with no group open, and the copies were matched to one "
        "another by their bytes and voted into this page. Its channel is "
        "known; its address and version are not, so it has no page number, "
        "and the start of its presentation code is lost with the header.");
  }
  if (unconfirmed && nothing_attested_) {
    tips.push_back(
        "Its address never arrived undamaged — a Hamming 8/4 byte of it was "
        "corrected every time — and nor did any other address in this "
        "recording, so there was nothing to confirm it against: it is listed "
        "as it was recovered.");
  } else if (unconfirmed) {
    tips.push_back(
        "Its address never arrived undamaged — a Hamming 8/4 byte of it was "
        "corrected every time — but it recurred under this address with a "
        "page nothing else carries, so it is listed as a record of its own.");
  }
  const auto flags = classification_flags(record);
  if (!flags.empty()) {
    tips.push_back("Classification flags (§5.2.7.2): " + join(flags, ", "));
  }
  if (record.classification_pointers > 1) {
    tips.push_back(
        "Its classification sequence has " +
        plural(record.classification_pointers, "pointer byte",
               "pointer bytes") +
        " (CEA-516 §5.2.7.1). The Full Service Standard allows one, and has "
        "its receivers ignore a record announcing a second (§8.5.2.7.1): an "
        "FSS receiver would not have shown this page. It is read here all "
        "the same.");
  }
  if (!record.reserved_flags.empty()) {
    tips.push_back(
        "Flag bytes CEA-516 §5.2.7.2 reserves for future standardization, "
        "set by the service: " +
        join(record.reserved_flags, ", ") +
        ". What they meant was the service's own.");
  }
  const std::string more_tip = more_designation_tip(record);
  if (!more_tip.empty()) {
    tips.push_back(more_tip);
  }
  const std::string keys_tip = key_designation_tip(record);
  if (!keys_tip.empty()) {
    tips.push_back(keys_tip);
  }
  if (!record.presented_over.empty()) {
    tips.push_back(
        "It starts on the screen it was asked from — it neither clears the "
        "display nor fills it before it draws — so it is drawn over " +
        join_with_and(record.presented_over) +
        (record.presented_over.size() == 1 ? ", the page" : ", the pages") +
        " a viewer came to it from. The Panasonic TU-2000x did not clear its "
        "screen when a page was asked for, and pages were written to be "
        "drawn over others: the questions and answers of a game over its "
        "board, a card of headlines over the page around it. The Norpak TTX "
        "650 cleared it, and showed "
        "such a page on black.");
  }
  tips.push_back(std::to_string(record.times_intact) + " of " +
                 std::to_string(record.times_seen) +
                 " copies arrived whole with their suffix check passed");
  if (revisions != nullptr) {
    tips.push_back(versions_tip(record, *revisions));
  }
  return join(tips, "\n\n");
}

CataloguePayload CatalogueListing::record_payload(
    const NabtsCataloguedRecord& record,
    const std::optional<StructuralRole>& role) const {
  const bool presentation = nabts_type_is_presentation(record.record_type);
  CataloguePayload payload;
  payload.headline = record_headline(record);
  if (role) {
    payload.kind = CataloguePayload::Kind::kText;
    payload.document.text = role->text;
    payload.document.monospace = role->monospace;
  } else if (presentation) {
    payload.kind = CataloguePayload::Kind::kDisplayList;
    payload.display_list = nabts_page_display_list(record.page, mode_);
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
  // A record listed rather than drawn has no reading of a page to report:
  // its condition is how it arrived.
  payload.condition = record_condition(record, presentation && !role);
  return payload;
}

/**
 * @brief The caption service as one further item
 *
 * §7.3.10 carries captioning as records with the Caption Flag of §5.2.7.3
 * set, each new caption a new version of the same address, and reading them
 * one record at a time tells a viewer nothing about the service. They are the
 * dataset's caption stream — one entry per caption as it arrived — rather
 * than records of the catalogue, since the sixteen versions Y16 allows recur
 * throughout a recording (§7.3.10.1) and name a caption only while it is on
 * air. Nothing is added where the stream holds no caption.
 */
void append_caption_track(const NabtsAnalysisDataset& data,
                          NaplpsRenderMode mode, CatalogueDataset& out) {
  std::vector<NabtsCataloguedRecord> captions = data.captions;
  nabts_interpret_records(captions, naplps_render_grid(mode));
  const auto cues = nabts_caption_cues(captions);
  if (cues.empty()) {
    return;
  }
  CatalogueItem item;
  item.id = kCaptionTrackId;
  item.values = {"Caption track", "Captions", std::to_string(cues.size()),
                 frame_range(cues.front().start_frame, cues.back().end_frame)};
  item.tooltip =
      "These records were transmitted with the Caption Flag set (CEA-516 "
      "§5.2.7.3), which is what makes a record part of the captioning "
      "service of §7.3.10. Each caption is a new version of its address, and "
      "the sixteen versions are used over and over, so the captions are "
      "listed as they arrived — each read from its own copies — rather than "
      "as records. Selecting this reads them in order.";

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
    if (std::find(channels.begin(), channels.end(), label) == channels.end()) {
      channels.push_back(label);
    }
    // A cue is often two lines; the table shows it on one so the list stays
    // scannable.
    std::string text = cue.text;
    std::replace(text.begin(), text.end(), '\n', ' ');
    payload.table.rows.push_back({frame_range(cue.start_frame, cue.end_frame),
                                  nabts_caption_time(cue.start_frame, '.') +
                                      " -> " +
                                      nabts_caption_time(cue.end_frame, '.'),
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

/// The id of a datacast's item and of a file it carried (see chain_row_id()).
std::string datacast_item_id(uint16_t channel) {
  char id[16];
  std::snprintf(id, sizeof(id), "\x04%03X", channel & 0xFFFU);
  return id;
}

std::string carried_file_id(const NabtsCarriedFile& file, size_t index) {
  char channel[8];
  std::snprintf(channel, sizeof(channel), "%03X", file.channel & 0xFFFU);
  return std::string(1, '\x05') + channel + "/" + file.name + "/" + file.time +
         "/" + std::to_string(index);
}

/// What the list calls a file of |format| (NabtsCarriedFile::format).
std::string file_kind_name(const std::string& format) {
  if (format == "gif") return "GIF image";
  if (format == "png") return "PNG image";
  if (format == "jpeg") return "JPEG image";
  if (format == "html") return "HTML page";
  if (format == "text") return "Text file";
  return "Binary file";
}

std::string channel_label(uint16_t channel) {
  char label[8];
  std::snprintf(label, sizeof(label), "%03X", channel & 0xFFFU);
  return label;
}

/// The address a file came on, or the first and last of the family a framed
/// service spread it over.
std::string file_addresses(const NabtsCarriedFile& file) {
  return file.last_channel > file.channel ? channel_label(file.channel) + "-" +
                                                channel_label(file.last_channel)
                                          : channel_label(file.channel);
}

/// A picture as one raster: the whole image a run of colours deposited
/// across a field the image's shape, one pixel a pel, from the top left
/// (CatalogueDrawKind::kColourRun), its pixels square.
CatalogueDisplayList image_display_list(const GifImage& image) {
  CatalogueDisplayList list;
  const double width = image.width;
  const double height = image.height;
  list.aspect_height = height / width;
  list.nominal_width = image.width;
  list.nominal_height = image.height;
  list.pixel_aligned = true;
  CatalogueDrawOp op;
  op.kind = CatalogueDrawKind::kColourRun;
  op.origin = CataloguePoint{0.0, 0.0};
  op.size = CatalogueSize{1.0, height / width};
  op.pen_size = CatalogueSize{1.0 / width, 1.0 / width};
  op.points.push_back(op.origin);
  op.colour_run.reserve(image.pixels.size());
  for (const uint8_t pixel : image.pixels) {
    CatalogueColour colour;
    if (pixel < image.palette.size()) {
      colour.red = image.palette[pixel][0];
      colour.green = image.palette[pixel][1];
      colour.blue = image.palette[pixel][2];
    }
    colour.transparent = pixel == image.transparent_index;
    op.colour_run.push_back(colour);
  }
  list.ops.push_back(std::move(op));
  return list;
}

/// Up to |limit| bytes of |data| as a hex dump, sixteen to a line with their
/// offset and the text they read as; '?' marks a byte no copy delivered.
std::string hex_dump(const NabtsCarriedFile& file, size_t limit) {
  std::string out;
  const size_t count = std::min(file.data.size(), limit);
  for (size_t row = 0; row < count; row += 16) {
    char line[16];
    std::snprintf(line, sizeof(line), "%08zx ", row);
    out += line;
    std::string text;
    for (size_t i = row; i < row + 16; ++i) {
      if (i >= count) {
        out += "   ";
        continue;
      }
      const bool lost = !file.data_present.empty() && file.data_present[i] == 0;
      if (lost) {
        out += " ??";
        text.push_back('?');
        continue;
      }
      char byte[4];
      std::snprintf(byte, sizeof(byte), " %02x", file.data[i]);
      out += byte;
      const uint8_t c = file.data[i];
      text.push_back(c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.');
    }
    out += "  " + text + "\n";
  }
  if (file.data.size() > count) {
    out += "... " + std::to_string(file.data.size() - count) +
           " more bytes in the exported file\n";
  }
  return out;
}

// The shortest run of text a binary file's text is taken from — words among
// it, its letters at least half of it — and how much of its text is shown.
constexpr size_t kBinaryTextMinRun = 20;
constexpr size_t kBinaryTextShown = size_t{64} * 1024;

/// The runs of text a binary file holds — a document's text, a program's
/// messages — each on a line of its own, a carriage return in one starting a
/// line; empty where it holds none.
std::string binary_text(const NabtsCarriedFile& file) {
  std::string out;
  std::string run;
  size_t letters = 0;
  size_t spaces = 0;
  const auto close = [&]() {
    if (run.size() >= kBinaryTextMinRun && spaces > 0 &&
        letters * 2 >= run.size()) {
      out += run + "\n";
    }
    run.clear();
    letters = 0;
    spaces = 0;
  };
  for (size_t i = 0; i < file.data.size() && out.size() < kBinaryTextShown;
       ++i) {
    const uint8_t c = file.data[i];
    const bool arrived = file.data_present.empty() || file.data_present[i] != 0;
    if (arrived && c >= 0x20 && c < 0x7F) {
      run.push_back(static_cast<char>(c));
      letters += std::isalpha(c) != 0 ? 1 : 0;
      spaces += c == ' ' ? 1 : 0;
    } else if (arrived && (c == '\r' || c == '\n' || c == '\t')) {
      run.push_back(c == '\t' ? '\t' : '\n');
    } else {
      close();
    }
  }
  close();
  return out;
}

/// |file|'s bytes as text, a byte no copy delivered as '?'.
std::string file_text(const NabtsCarriedFile& file) {
  std::string text(file.data.begin(), file.data.end());
  for (size_t i = 0; i < file.data_present.size() && i < text.size(); ++i) {
    if (file.data_present[i] == 0) {
      text[i] = '?';
    }
  }
  return text;
}

/// What the list shows for a file a datacast carried: a picture drawn, a
/// page of HTML as the text it shows, a text file as it reads, anything
/// else as a dump of its bytes and the runs of text it holds.
CataloguePayload carried_file_payload(const NabtsCarriedFile& file) {
  CataloguePayload payload;
  const size_t lost = file.bytes_lost();
  payload.headline =
      file.name + (file.source.empty() ? "" : " (from " + file.source + ")") +
      (file.joined_parts > 0
           ? " (joined from its " + std::to_string(file.joined_parts) +
                 " numbered parts)"
           : "") +
      (file.time.empty() ? "" : ", " + file.time) + ": " +
      plural(file.data.size(), "byte", "bytes") + ", " +
      plural(file.copies, "copy", "copies") + " (" + file.transfer + " on " +
      file_addresses(file) + ")";
  if (lost > 0) {
    payload.condition = plural(lost, "byte", "bytes") +
                        " no copy delivered, written as zero in the exported "
                        "file";
  } else if (file.deciphered) {
    payload.condition =
        "Read through the byte table the service hid its "
        "files' text behind";
  } else if (file.cut_short) {
    payload.condition =
        "Unpacked from its service's container as far as the container's "
        "bytes went: its end did not come out";
  }
  // The service's own check: the sum of the file's bytes a carousel's
  // directory gives, or the check a container keeps of what it holds.
  if (file.checksum_holds.has_value()) {
    const bool container = !file.source.empty();
    payload.condition +=
        std::string(payload.condition.empty() ? "" : "; ") +
        (*file.checksum_holds
             ? (container ? "unpacked from its service's container, the "
                            "container's check of it holding"
                          : "its bytes add up to the sum its directory gives")
             : (container ? "unpacked from its service's container, the "
                            "container's check of it failing: some of its "
                            "bytes were misread"
                          : "its bytes do not add up to the sum its directory "
                            "gives: some were misread"));
  }
  if (file.format == "gif") {
    if (const std::optional<GifImage> image = decode_gif(file.data)) {
      payload.kind = CataloguePayload::Kind::kDisplayList;
      payload.display_list = image_display_list(*image);
      payload.companion_text = file_kind_name(file.format) + ", " +
                               std::to_string(image->width) + " x " +
                               std::to_string(image->height) +
                               (image->truncated ? ", its data cut short" : "");
      return payload;
    }
  }
  payload.kind = CataloguePayload::Kind::kText;
  payload.document.monospace = true;
  if (file.format == "html") {
    std::vector<uint8_t> shown(file.data);
    if (!file.data_present.empty()) {
      const std::string text = file_text(file);
      shown.assign(text.begin(), text.end());
    }
    payload.document.text = nabts_html_text(shown);
  } else if (file.format == "text") {
    payload.document.text = file_text(file);
  } else {
    payload.document.text = hex_dump(file, 4096);
    const std::string text = binary_text(file);
    if (!text.empty()) {
      payload.document.text +=
          "\nThe text it holds — every run of " +
          std::to_string(kBinaryTextMinRun) +
          " characters or more that reads as words, in the order it holds "
          "them:\n\n" +
          text;
    }
  }
  return payload;
}

/// The datacasts the recording carried, each an item of its own, and the
/// files they carried after them. A datacast's meaning is its service's
/// own, so its item says how it decoded and shows the text it holds; a file
/// is shown as what it is.
void append_datacasts(const NabtsAnalysisDataset& data, CatalogueDataset& out) {
  for (const NabtsDatacastListing& datacast : data.datacasts) {
    CatalogueItem item;
    item.id = datacast_item_id(datacast.channel);
    item.values = {channel_label(datacast.channel),
                   datacast.raw ? "Raw stream" : "Datacast", "", "",
                   "Datacasts"};
    item.tooltip =
        datacast.raw
            ? "A stream of data blocks this channel sent in no data groups "
              "and with no row code, joined in the order its continuity "
              "index gives them. What it means is its service's own; the "
              "text it holds is shown."
            : "An FEC datacast (RFC 2728): rows of 26 bytes protected by a "
              "row code and, down each column of a bundle, by check rows, "
              "decoded and put right where the code could. What it means is "
              "its service's own; the text it holds is shown, and the files "
              "it carried in a framing the stage knows are listed after it.";
    CataloguePayload payload;
    payload.kind = CataloguePayload::Kind::kText;
    payload.document.monospace = true;
    payload.document.text =
        datacast.summary + "\n\n" +
        (datacast.text.empty()
             ? std::string("The payload holds no run of text: its bytes are "
                           "its service's own coding.")
             : datacast.text);
    payload.headline = std::string(datacast.raw ? "Raw stream" : "Datacast") +
                       " on " + channel_label(datacast.channel) + ": " +
                       plural(datacast.payload_bytes, "byte", "bytes") +
                       " of payload";
    out.items.push_back(std::move(item));
    out.payloads.push_back(std::move(payload));
  }
  for (size_t i = 0; i < data.carried_files.size(); ++i) {
    const NabtsCarriedFile& file = data.carried_files[i];
    CatalogueItem item;
    item.id = carried_file_id(file, i);
    item.values = {file_addresses(file) + "/" + file.name,
                   file_kind_name(file.format), std::to_string(file.copies), "",
                   "Datacast files"};
    item.find_key = file.source.empty() ? file.name : file.source;
    if (file.bytes_lost() > 0 || file.cut_short) {
      item.badges.emplace_back("incomplete");
    }
    item.tooltip =
        "A file the datacast on " + file_addresses(file) +
        " carried, taken out of its " + file.transfer +
        (file.transfer == "framed service"
             ? ", its frames read under the pad they were sent under"
             : "") +
        (file.source.empty() ? ""
                             : " and unpacked from the container the "
                               "service sent it in") +
        (file.copies > 1 ? " and combined from the copies the recording held"
                         : "") +
        (file.deciphered ? ", and read through the byte table its service "
                           "hid its files' text behind"
                         : "") +
        ". With export_records it is written beside the packet stream.";
    out.items.push_back(std::move(item));
    out.payloads.push_back(carried_file_payload(file));
  }
  if (!data.carried_files.empty()) {
    out.summary.notices.push_back(
        plural(data.carried_files.size(), "file", "files") +
        " taken out of the datacasts");
  }
  for (const NabtsFramedService& service : data.framed_services) {
    if (service.channels.empty()) {
      continue;
    }
    char identifier[16];
    std::snprintf(identifier, sizeof(identifier), "%08x", service.identifier);
    const std::string addresses = channel_label(service.channels.front()) +
                                  "-" + channel_label(service.channels.back());
    CatalogueItem item;
    item.id = std::string(1, '\x06') + identifier;
    item.values = {addresses, "Framed service", std::to_string(service.frames),
                   "", "Datacasts"};
    item.find_key = identifier;
    item.tooltip =
        "A datacast service that sends its data in counted frames, each the "
        "sixteen data rows of a bundle opened by a header, spread over a "
        "family of addresses that take turns, every frame under one fixed "
        "pad. The pad is taken off, the frames of every address that carried "
        "it are put back in the order their count gives, and the files they "
        "hold are listed with the datacast files; with export_records the "
        "frames are written beside the stream as one file.";
    CataloguePayload payload;
    payload.kind = CataloguePayload::Kind::kText;
    payload.document.monospace = true;
    payload.document.text =
        "Service " + std::string(identifier) + ", " + service.summary +
        ".\n\nThe first frames' headers, the pad taken off: the frame's "
        "count, the object it is part of and where in it, the frame of the "
        "recording it arrived in and its address, then the header's "
        "bytes:\n" +
        service.headers;
    payload.headline = "Framed service " + std::string(identifier) + " on " +
                       addresses + ": " +
                       plural(service.frames, "frame", "frames");
    out.items.push_back(std::move(item));
    out.payloads.push_back(std::move(payload));
  }
  if (!data.framed_services.empty()) {
    out.summary.notices.push_back(plural(data.framed_services.size(),
                                         "framed datacast service",
                                         "framed datacast services"));
  }
}

/// The channels of data groups of a type the standard reserves, an item each:
/// what their groups carried, as text where they read as text and otherwise as
/// a dump of their bytes (NabtsPrivateTextLog).
void append_private_data(const NabtsAnalysisDataset& data,
                         CatalogueDataset& out) {
  for (const NabtsPrivateData& channel : data.private_data) {
    char id[16];
    std::snprintf(id, sizeof(id), "\x07%03X/%u", channel.channel & 0xFFFU,
                  static_cast<unsigned>(channel.type));
    const std::string type =
        channel.type == kNabtsPrivateGroupType
            ? "Private data (type 15)"
            : "Type " + std::to_string(channel.type) + " data";
    CatalogueItem item;
    item.id = id;
    item.values = {channel_label(channel.channel), type,
                   std::to_string(channel.groups), "", "Other services"};
    item.tooltip =
        "Data groups of a type CEA-516 reserves — another service's data in a "
        "format of its own — shown as they arrived: a group that reads as "
        "seven-bit text as its text, any other as a dump of its bytes.";
    CataloguePayload payload;
    payload.kind = CataloguePayload::Kind::kText;
    payload.document.monospace = true;
    payload.headline = "Channel " + channel_label(channel.channel) + ": " +
                       plural(channel.groups, "data group", "data groups") +
                       " of type " + std::to_string(channel.type) + ", " +
                       std::to_string(channel.text_groups) + " of them text";
    payload.document.text = channel.shown;
    if (channel.not_shown > 0) {
      payload.document.text +=
          plural(channel.not_shown, "more group", "more groups") +
          " not shown\n";
    }
    out.items.push_back(std::move(item));
    out.payloads.push_back(std::move(payload));
  }
}

/// What the list says of the run as a whole beside the caption track: what a
/// reconstruction did, and the other services' data it does not list.
void append_notices(const NabtsAnalysisDataset& data,
                    const std::vector<NabtsCataloguedRecord>& records,
                    bool reconstruct, CatalogueDataset& out) {
  if (!records.empty() && reconstruct) {
    out.summary.notices.push_back(
        reconstruction_notice(records, data.references.size()));
  }
  for (const NabtsForeignGroups& foreign : data.summary.foreign_groups) {
    out.summary.notices.push_back(foreign_notice(foreign));
  }
  for (const NabtsOtherService& service : data.summary.other_services) {
    out.summary.notices.push_back(other_service_notice(service));
  }
  if (records.empty() && (!data.summary.foreign_groups.empty() ||
                          !data.summary.other_services.empty())) {
    out.schema.empty_message =
        "No teletext records: the NABTS this recording carries is another "
        "service's data";
  }
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
          "256 x 200 (vector)",
          "The reference receiver's page drawn as shapes rather than pixels: "
          "every stroke as heavy as on that receiver and on the pixels it "
          "would light, but smooth at any size, and the clearest reading of "
          "what the page describes."},
  };
}

CatalogueViewToggle naplps_reconstruct_toggle(bool active) {
  return CatalogueViewToggle{
      kNabtsReconstructToggleId, "Reconstruct lost data",
      "A recording that lost a packet lost a stretch of every record it "
      "carried, and the page reads the stretch as nothing — so whatever came "
      "after it is drawn wrongly. With this on, a lost stretch is filled where "
      "another record, of this recording or of the reference recordings the "
      "stage names, holds the same bytes on both sides of it; what is left is "
      "read as a receiver with those bytes gone would read it. A byte that "
      "arrived failing its "
      "parity — known to be wrong — is replaced the same way, where another "
      "record agrees with this one all round it. Pages drawn from rebuilt "
      "bytes are marked ~ and say what was rebuilt and from where. Only what "
      "is drawn "
      "here changes — the exported stream and record files are the "
      "recording's own bytes.",
      active};
}

CatalogueViewToggle naplps_receiver_pace_toggle(bool active) {
  return CatalogueViewToggle{
      kNabtsReceiverPaceToggleId, "Receiver pace",
      "Play each page at the pace a receiver of the day drew it. A set drew a "
      "page as it arrived, a few milliseconds a character or a line and a "
      "filled shape by its size, so a page was seen being built up and a busy "
      "one took seconds, a filled shape or a clear sweeping across the screen "
      "a row at a time. Off, a page appears whole and moves on only at the "
      "pauses its author wrote into it (WAIT). The timings are the Norpak TTX "
      "650's, measured from its firmware, unless TU-2000x pace is ticked; "
      "what is drawn is the same either way.",
      active};
}

CatalogueViewToggle naplps_tu2000x_pace_toggle(bool active) {
  return CatalogueViewToggle{
      kNabtsTu2000xPaceToggleId, "TU-2000x pace",
      "Play each page at the pace the Panasonic TU-2000x drew it, whether or "
      "not Receiver pace is ticked: a home decoder of the day. It was slower "
      "than the Norpak TTX 650 over large lettering and filled polygons and "
      "quicker over arcs and clears, waited for the next field to change a "
      "colour, and swept fills and clears down the screen where the TTX 650 "
      "swept them up. Its timings were measured from its firmware too; what "
      "is drawn is the same either way.",
      active};
}

NaplpsReceiverPace naplps_receiver_pace_for(bool receiver_pace,
                                            bool tu2000x_pace) {
  if (tu2000x_pace) {
    return NaplpsReceiverPace::kPanasonicTu2000x;
  }
  return receiver_pace ? NaplpsReceiverPace::kNorpakTtx650
                       : NaplpsReceiverPace::kOff;
}

CatalogueDataset build_nabts_catalogue(const NabtsAnalysisDataset& data,
                                       NaplpsRenderMode mode, bool reconstruct,
                                       bool receiver_pace, bool tu2000x_pace) {
  // The pages are built here rather than by recovery, because what a page looks
  // like depends on the receiver it is drawn for and recovery has no business
  // fixing that: X3.110 §6.2.3 sizes a DRCS character's storage buffer from the
  // physical resolution its character field covers. Interpreting on the way to
  // the screen is what lets the render resolution be changed without reading
  // the recording again.
  std::vector<NabtsCataloguedRecord> records = data.records;
  nabts_interpret_records(
      records, naplps_render_grid(mode),
      NabtsReconstructionInput{reconstruct, &data.references},
      naplps_receiver_pace_for(receiver_pace, tu2000x_pace));

  CatalogueDataset out;
  out.schema.columns = {
      CatalogueColumn{"address", "Address", false},
      CatalogueColumn{"type", "Type", false},
      CatalogueColumn{"seen", "Seen", true},
      CatalogueColumn{"frames", "Frames", true},
      CatalogueColumn{"section", "Section", false},
  };
  out.schema.item_noun = "Record";
  out.schema.variant_noun = "Sub-page";
  out.schema.highlight_label = "Show display area";
  out.schema.empty_message =
      data.summary.not_nabts
          ? "No NABTS: the lines read carry another signal behind NABTS's "
            "run-in and framing code"
          : "No NABTS records were recovered";
  // A page is reached the way a viewer reached it: by its number, typed as the
  // decoder's keypad took it (nabts_keypad_number()) — each item's alias.
  out.schema.find_label = "Page:";
  out.schema.find_placeholder = "e.g. 100, 1985, game";

  // Which receiver the pages are drawn against is offered to the reader, not
  // only to the project: it changes nothing the recovery found and everything
  // about how a page looks, and the reason to have four of them is to put one
  // beside another.
  out.schema.view_label = "Receiver";
  out.schema.view_options = naplps_view_options();
  out.schema.view_option = naplps_render_mode_name(mode);

  // Whether a damaged page is presented with its lost bytes rebuilt or as
  // recovered is the reader's to decide while looking, for the same reason the
  // receiver is: it changes nothing the recovery found, and having the two side
  // by side is how anyone judges whether a rebuild restored the page or
  // invented it.
  out.schema.toggles = {naplps_reconstruct_toggle(reconstruct),
                        naplps_receiver_pace_toggle(receiver_pace),
                        naplps_tu2000x_pace_toggle(tu2000x_pace)};

  // The decoder's keypad, on every recording: its digits ask for a page by
  // number, as the find box does, and play a page that links keys to macros.
  out.schema.keypad_label = kNabtsKeypadLabel;

  CatalogueListing(records, data.summary, mode).append_to(out);
  append_caption_track(data, mode, out);
  append_datacasts(data, out);
  append_private_data(data, out);
  append_notices(data, records, reconstruct, out);
  out.summary.headline = run_headline(data.summary, records.size());
  return out;
}

CataloguePayload build_nabts_played_payload(
    const NabtsAnalysisDataset& data, const std::string& item_id,
    const std::vector<std::string>& keys, NaplpsRenderMode mode,
    bool reconstruct, bool receiver_pace, bool tu2000x_pace) {
  std::vector<NabtsCataloguedRecord> records = data.records;
  NabtsKeyPlay play;
  play.record = records.size();
  for (size_t i = 0; i < records.size(); ++i) {
    if (nabts_type_is_presentation(records[i].record_type) &&
        nabts_record_identity(records[i]) == item_id) {
      play.record = i;
      break;
    }
  }
  if (play.record == records.size()) {
    return {};
  }
  for (const std::string& key : keys) {
    if (key.size() == 1 && key[0] >= '0' && key[0] <= '9') {
      play.keys.push_back(static_cast<uint8_t>(key[0] - '0'));
    }
  }

  // The page is presented exactly as the catalogue presents it — the same
  // receiver, reconstruction and pace — and only then played.
  nabts_interpret_records(
      records, naplps_render_grid(mode),
      NabtsReconstructionInput{reconstruct, &data.references},
      naplps_receiver_pace_for(receiver_pace, tu2000x_pace), &play);

  const NabtsCataloguedRecord& record = records[play.record];
  CataloguePayload payload;
  payload.kind = CataloguePayload::Kind::kDisplayList;
  payload.display_list = nabts_page_display_list(record.page, mode);
  payload.companion_text = nabts_page_text(record.page);
  payload.headline = record_headline(record);
  if (!keys.empty()) {
    payload.headline += ", played: " + join(keys, " ");
  }
  payload.condition = record_condition(record, true);
  return payload;
}

}  // namespace orc
