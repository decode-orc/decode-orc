/*
 * File:        naplps_interpreter.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     NAPLPS interpreter implementation (X3.110 §5, §6)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_interpreter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <utility>
#include <vector>

#include "nabts_raster_view.h"
#include "naplps_font.h"
#include "naplps_incremental.h"
#include "naplps_proportional.h"
#include "naplps_raster.h"
#include "naplps_receiver_pace.h"
#include "vbi-services/teletext_page_decoder.h"

namespace orc {

namespace {

/// §3.3 of CEA-516 puts odd parity in b8 of every data byte of a type-zero
/// group, so a NAPLPS byte is the low seven bits.
constexpr uint8_t kSevenBits = 0x7F;

/// A character field of no stated height still names one row, so a movement
/// across it is measured against at least this much.
constexpr double kRowHeightFloor = 1e-9;

/// §5.3.2.7.3 and §5.3.2.8.2 count their intervals in tenths of a second.
constexpr int64_t kTenthMs = 100;

/// §5.3.2.8.2: a zero interval is "between zero and 1/10 of a second
/// (inclusive) that is implementation-dependent", and Table D1 item 5(6)
/// narrows that for teletext to "between two complete TV fields (approximately
/// 1/30 s) and three complete TV fields" — so two fields of 525-line NTSC.
constexpr int64_t kMinimumWaitMs = 33;

/// Where the presentation clock stops. Fifteen minutes is well past anything a
/// service paced a page at (the longest in the recordings is about nine), and
/// a clock that stops keeps a damaged WAIT from pushing the rest of a page out
/// of reach.
constexpr int64_t kMaxPresentationMs = int64_t{15} * 60 * 1000;

/// Whether every one of |points| lies on the unit screen (§5.3.1).
bool within_unit_screen(const std::vector<NabtsPoint>& points) {
  return std::all_of(points.begin(), points.end(), [](const NabtsPoint& p) {
    return naplps_in_unit_screen(p.x) && naplps_in_unit_screen(p.y);
  });
}

/// §6.2.8.1 leaves BLINK START's intervals to the implementation ("the on and
/// off intervals are implementation-dependent"). These are the Norpak TTX
/// 650's: a second of the blink-to colour (the ON interval, shown first), then
/// a second of the drawing colour. The Panasonic TU-2000x uses 1.1 s of each.
constexpr uint32_t kBlinkStartOnMs = 1000;
constexpr uint32_t kBlinkStartOffMs = 1000;

/// A comparison tolerance for character fields meeting an edge: a field ending
/// exactly at the edge must not count as over it, and a character width like
/// the default 1/40 is not an exact binary fraction, so a line of them lands
/// within an ulp or two of the edge rather than on it.
constexpr double kFieldEdgeTolerance = 1e-9;

/// A rectangle of the unit screen a cursor movement is confined to.
struct CursorArea {
  double left = 0.0;
  double right = 1.0;
  double bottom = 0.0;
  double top = 1.0;
};

/// §5.3.2.3.6's unit screen, and §6.1.2's display area: the lower
/// kNabtsDisplayAreaHeight of it (§4.2.2).
constexpr CursorArea kUnitScreenArea{0.0, 1.0, 0.0, 1.0};
constexpr CursorArea kDisplayArea{0.0, 1.0, 0.0, kNabtsDisplayAreaHeight};

/// The character field about its origin as offsets from the cursor, turned by
/// the character rotation — §5.3.2.3.2 Table 6 turns it counterclockwise
/// about its origin. The field's size is taken as magnitudes: a negative
/// dimension mirrors the character inside the field rather than moving it.
struct FieldExtent {
  double x0 = 0.0;
  double x1 = 0.0;
  double y0 = 0.0;
  double y1 = 0.0;
};

FieldExtent field_extent(const NaplpsTextState& text) {
  const double w = std::fabs(text.character_field.dx);
  const double h = std::fabs(text.character_field.dy);
  switch (text.rotation) {
    case NabtsCharRotation::k90:
      return {-h, 0.0, 0.0, w};
    case NabtsCharRotation::k180:
      return {-w, 0.0, -h, 0.0};
    case NabtsCharRotation::k270:
      return {0.0, h, -w, 0.0};
    case NabtsCharRotation::kNone:
      break;
  }
  return {0.0, w, 0.0, h};
}

/// Whether a movement of the cursor to |next|, in the direction (|step_x|,
/// |step_y|), keeps the character field inside |area| on the axis it moves
/// along. The edge rules are about what "such a movement would cause"
/// (§6.1.2): a field already partly outside across its direction of travel
/// has not been put there by the move.
bool move_fits(const CursorArea& area, const NabtsPoint& next,
               const FieldExtent& extent, double step_x, double step_y) {
  const bool x_fits = next.x + extent.x0 >= area.left - kFieldEdgeTolerance &&
                      next.x + extent.x1 <= area.right + kFieldEdgeTolerance;
  const bool y_fits = next.y + extent.y0 >= area.bottom - kFieldEdgeTolerance &&
                      next.y + extent.y1 <= area.top + kFieldEdgeTolerance;
  return (step_x == 0.0 || x_fits) && (step_y == 0.0 || y_fits);
}

/// Whether the character field with its origin at |cursor| lies wholly in
/// |area|.
bool field_fits(const CursorArea& area, const NabtsPoint& cursor,
                const FieldExtent& extent) {
  return move_fits(area, cursor, extent, 1.0, 1.0);
}

/// One pixel of the reference receiver (Table D1 item 10).
constexpr double kReferencePixel = 1.0 / 256.0;

/**
 * @brief The active field FIELD set, as an area, or false for the default one
 *
 * The default field, the whole unit screen, confines nothing the unit screen
 * and the display area do not already (§5.3.3.6.2). The origin may be any
 * corner, dx and dy being signed. A negative size is counted from the origin's
 * own pixel, as both receivers of the day count a logical pel of negative size
 * from the drawing point's pixel: FIELD (0.4, 0.66) (-0.23, -0.12) covers the
 * pixels from the origin's down and to the left, so a character field whose
 * top is on the origin's row lies within it — pages are sent with their text
 * cursor exactly there.
 */
bool active_field_area(const NaplpsState& state, CursorArea& area) {
  const NabtsPoint origin = state.field_origin;
  const NabtsSize size = state.field_size;
  if (origin.x == 0.0 && origin.y == 0.0 && size.dx == 1.0 && size.dy == 1.0) {
    return false;
  }
  const double x_shift = size.dx < 0.0 ? kReferencePixel : 0.0;
  const double y_shift = size.dy < 0.0 ? kReferencePixel : 0.0;
  area.left = std::min(origin.x, origin.x + size.dx) + x_shift;
  area.right = std::max(origin.x, origin.x + size.dx) + x_shift;
  area.bottom = std::min(origin.y, origin.y + size.dy) + y_shift;
  area.top = std::max(origin.y, origin.y + size.dy) + y_shift;
  return true;
}

/**
 * @brief The area a movement of the cursor is confined to: the active field
 *        where the character field at the cursor lies within it, |outside|
 *        otherwise
 *
 * X3.110 confines text to the active field "if the character field was
 * entirely within the active field immediately before the movement" — the
 * automatic APR APD (§5.3.2.3.6), APB, APF, APD, APU and APR (§6.1.2.1-7),
 * REPEAT TO EOL (§6.2.7.3) and scrolling (§6.2.7.13-14) — "the active field
 * used for columnated text" (§5.3.3.6.2). Both receivers of the day whose
 * firmware the recordings were checked against confine; this is the Norpak
 * TTX 650's reading, which is the clause's.
 */
CursorArea confining_area(const NaplpsState& state, const CursorArea& outside,
                          const FieldExtent& extent) {
  CursorArea field;
  return active_field_area(state, field) &&
                 field_fits(field, state.cursor, extent)
             ? field
             : outside;
}

/// The character path as a unit vector (§5.3.2.3.3 Table 7).
NabtsPoint path_vector(NabtsCharPath path) {
  switch (path) {
    case NabtsCharPath::kRight:
      return NabtsPoint{1.0, 0.0};
    case NabtsCharPath::kLeft:
      return NabtsPoint{-1.0, 0.0};
    case NabtsCharPath::kUp:
      return NabtsPoint{0.0, 1.0};
    case NabtsCharPath::kDown:
      return NabtsPoint{0.0, -1.0};
  }
  return NabtsPoint{0.0, 0.0};
}

/// |at| moved along the axis of the direction (|dx|, |dy|), one of them zero,
/// to put the character field against |area|'s edge that way: its left edge
/// on the area's left for a negative |dx|, and so on.
NabtsPoint against_edge(NabtsPoint at, const CursorArea& area,
                        const FieldExtent& extent, double dx, double dy) {
  if (dx < 0.0) {
    at.x = area.left - extent.x0;
  } else if (dx > 0.0) {
    at.x = area.right - extent.x1;
  }
  if (dy < 0.0) {
    at.y = area.bottom - extent.y0;
  } else if (dy > 0.0) {
    at.y = area.top - extent.y1;
  }
  return at;
}

/**
 * @brief Collapse map writes nothing can tell apart
 *
 * Within one instant the record's writes all land before anything is shown
 * (a pixel is resolved through the map at display time), so of a run of
 * writes with no blink event between them only the last to each entry
 * matters. A page that loads its palette entry by entry and redraws it
 * several times over leaves a long run otherwise.
 */
void compact_map_writes(std::vector<NabtsPaletteEvent>& events) {
  std::vector<NabtsPaletteEvent> out;
  out.reserve(events.size());
  size_t i = 0;
  while (i < events.size()) {
    if (events[i].kind != NabtsPaletteEvent::Kind::kMapWrite) {
      out.push_back(events[i++]);
      continue;
    }
    size_t end = i;
    while (end < events.size() &&
           events[end].kind == NabtsPaletteEvent::Kind::kMapWrite &&
           events[end].time_ms == events[i].time_ms) {
      ++end;
    }
    for (size_t j = i; j < end; ++j) {
      bool overwritten = false;
      for (size_t k = j + 1; k < end && !overwritten; ++k) {
        overwritten = events[k].address == events[j].address;
      }
      if (!overwritten) {
        out.push_back(events[j]);
      }
    }
    i = end;
  }
  events.swap(out);
}

/**
 * @brief The start of the ON interval of |process| under way at |time|, or of
 *        the next one where it is between them
 *
 * §5.3.2.7.2 measures a new process's start delay from "the start of the ON
 * interval of the most recently defined active blink process". A process in
 * its ON interval has one started; one in its OFF interval, or not yet
 * started, has the next one to come.
 */
int64_t current_or_next_on_start(const NabtsBlinkProcess& process,
                                 int64_t time) {
  const int64_t period = process.period_ms();
  if (time <= process.first_on_ms || period <= 0) {
    return process.first_on_ms;
  }
  const int64_t start =
      process.first_on_ms + ((time - process.first_on_ms) / period) * period;
  return time - start < static_cast<int64_t>(process.on_ms) ? start
                                                            : start + period;
}

/**
 * @brief When a process defined at |time| with a start delay of |delay_ms|
 *        begins its first ON interval, |newest| being the most recently
 *        defined process still running
 *
 * The delay is counted from "the start of the ON interval of the most recently
 * defined active blink process" (§5.3.2.7.2): the one under way, or the next.
 * Where the delay counted from an ON interval already under way has gone by
 * when the process is defined, both receivers of the day — the Norpak TTX 650
 * and the Panasonic TU-2000x, run as firmware — count it from that process's
 * next ON interval instead, so a process never starts part way through an ON
 * interval it was not there for.
 */
int64_t first_on_after(const NabtsBlinkProcess& newest, int64_t time,
                       uint32_t delay_ms) {
  int64_t start = current_or_next_on_start(newest, time) + delay_ms;
  const int64_t period = newest.period_ms();
  if (start < time && period > 0) {
    start += (time - start + period - 1) / period * period;
  }
  return start;
}

/// Both receivers of the day step their blink processes on a 10 Hz tick, so a
/// process with none running to keep in step with starts on the next tick of
/// the page's clock, and processes defined within one tick start together.
constexpr int64_t kBlinkTickMs = 100;

int64_t next_blink_tick(int64_t time) {
  return (time + kBlinkTickMs - 1) / kBlinkTickMs * kBlinkTickMs;
}

/**
 * @brief Whether text is spaced proportionally on the path it runs
 *
 * Inter-character spacing code 3 (§5.3.2.3.4), with the path running the way
 * the character reads: right at 0 degrees, up at 90, left at 180, down at 270.
 * T.101 Annex D's Addendum (clause 11) has its displacement apply "if the
 * character field width is parallel to the character path"; both receivers of
 * the day — the Norpak TTX 650 and the Panasonic TU-2000x, driven through
 * their firmware — apply it only where the path also runs the way the
 * character reads, and step a path against it by the field.
 */
bool spaced_proportionally(const NaplpsTextState& text) {
  if (text.intercharacter_spacing != 3) {
    return false;
  }
  switch (text.rotation) {
    case NabtsCharRotation::kNone:
      return text.path == NabtsCharPath::kRight;
    case NabtsCharRotation::k90:
      return text.path == NabtsCharPath::kUp;
    case NabtsCharRotation::k180:
      return text.path == NabtsCharPath::kLeft;
    case NabtsCharRotation::k270:
      return text.path == NabtsCharPath::kDown;
  }
  return false;
}

/**
 * @brief How far into its field a proportionally spaced character's ink
 *        starts, in units of the unit screen
 *
 * Both receivers of the day lay proportional text with each character's ink
 * starting at the cursor: the space a fixed pitch leaves beside a narrow
 * letter is what proportional spacing takes away. The field is moved back by
 * the columns its pattern leaves blank before the ink, measured where the
 * receivers drew it — on the 256 x 200 grid, in the face and at the place the
 * raster gives that field there (a whole multiple of the cell, centred, the
 * spare column to the right). Zero for a character with no ink.
 */
double ink_offset(char32_t code, const NabtsSize& field) {
  const int columns = std::max(1, naplps_reference_pixels(field.dx));
  const int rows = std::max(1, naplps_reference_pixels(field.dy));
  const NaplpsFontFace& face =
      naplps_font_face_for_field(kNaplpsGridReference, columns, rows);
  const uint16_t* pattern = naplps_face_pattern(face, code);
  if (pattern == nullptr) {
    return 0.0;
  }
  int first_lit = face.width;
  for (int row = 0; row < face.height; ++row) {
    for (int column = 0; column < first_lit; ++column) {
      if (((pattern[row] >> (face.width - 1 - column)) & 1u) != 0u) {
        first_lit = column;
        break;
      }
    }
  }
  if (first_lit >= face.width) {
    return 0.0;
  }
  const int scale = columns >= face.width ? columns / face.width : 1;
  const int first_column =
      columns >= face.width ? (columns - scale * face.width) / 2 : 0;
  return (first_column + first_lit * scale) / kNaplpsReferencePixelsPerUnit;
}

/// Graphic bytes of a 7-bit in-use table: columns 2 to 7.
constexpr bool is_graphic(uint8_t byte) { return byte >= 0x20; }

/// C0 controls: columns 0 and 1.
constexpr bool is_c0(uint8_t byte) { return byte < 0x20; }

/// The repertoire a G-set maps to in the display list.
NabtsPrimitive::Repertoire repertoire_of(NaplpsGSet set) {
  switch (set) {
    case NaplpsGSet::kSupplementary:
      return NabtsPrimitive::Repertoire::kSupplementary;
    case NaplpsGSet::kMosaic:
      return NabtsPrimitive::Repertoire::kMosaic;
    case NaplpsGSet::kDrcs:
      return NabtsPrimitive::Repertoire::kDrcs;
    case NaplpsGSet::kPrimary:
    case NaplpsGSet::kPdi:
    case NaplpsGSet::kMacro:
    case NaplpsGSet::kNull:
      break;
  }
  return NabtsPrimitive::Repertoire::kPrimary;
}

}  // namespace

NaplpsInterpreter::NaplpsInterpreter()
    : NaplpsInterpreter(kNaplpsGridReference) {}

NaplpsInterpreter::NaplpsInterpreter(NaplpsRenderGrid grid) {
  state_.set_render_grid(grid);
}

// ---------------------------------------------------------------------------
// The main loop
// ---------------------------------------------------------------------------

void NaplpsInterpreter::reset_decoder() {
  // CEA-516 §8.5: "A general reset is equivalent to a NSR, followed by a RESET
  // command with two operands consisting of all 1s" — so everything, storage
  // included, goes back to Table II-3's defaults.
  state_.reset_all();
  env_.reset();
  previous_end_ms_ = 0;
}

void NaplpsInterpreter::apply_caption_state() {
  // CEA-516 §5.2.7.3: the caption process "first executes a Non-Selective
  // Reset (NSR)" — which leaves macros, DRCS, masks and map alone — and then
  // establishes the stated state.
  apply_nsr_reset();
  state_.cursor = NabtsPoint{0.0, 0.0};
  state_.drawing_point = NabtsPoint{0.0, 0.0};
  // "color map entry 0/16 (0000) set to transparent, ... 1/16 (0001) set to
  // black, ... 7/16 (0111) set to white".
  state_.colour.write_map_entry(0, kNabtsTransparent);
  state_.colour.write_map_entry(1, kNabtsNominalBlack);
  state_.colour.write_map_entry(7, kNabtsNominalWhite);
  // "color mode set to 2, drawing color set to entry 7/16 (0111) (white),
  // background color set to entry 1/16 (0001) (black)".
  state_.colour.select_mapped_background_mode(7, 1);
}

NabtsPageSnapshot NaplpsInterpreter::run(const std::vector<uint8_t>& record,
                                         bool keep_display) {
  // Only the per-record transients start fresh. The presentation state — the
  // attributes, macros, DRCS, texture masks and colour map — carries over from
  // whatever ran before, because that is what a receiver does: CEA-516
  // §8.7.2.3.1's per-page NSR is recommended precisely because nothing resets
  // between records by itself.
  //
  // A continuation additionally starts from the display the previous record
  // left: a More Record repaints only what changes, and running it over a
  // blank canvas leaves holes where the page's standing artwork should be.
  std::vector<NabtsPrimitive> carried;
  if (keep_display) {
    carried = std::move(snapshot_.primitives);
  }
  snapshot_ = NabtsPageSnapshot{};
  snapshot_.primitives = std::move(carried);
  // The standing display is on screen from the moment this record starts.
  for (NabtsPrimitive& primitive : snapshot_.primitives) {
    primitive.appear_ms = 0;
    primitive.draw_ms = 0;
  }

  // The record's clock starts at zero, with the colour map as it stands —
  // whatever ran before, a Support Record or the caption preset, is part of
  // where the record starts from rather than something it does.
  clock_ms_ = 0;
  pace_us_ = 0;
  snapshot_.timeline.fill_sweep = naplps_pace_fill_sweep(pace_);
  (void)state_.colour.take_writes();
  state_.colour.copy_map_to(snapshot_.timeline.initial_map);

  // A blink process nothing has terminated is still running (§6.1.6.5 leaves
  // them out of NSR, and CEA-516 §5.2.7.8 presents a More Record straight
  // after its predecessor), so it carries into this record. The previous
  // record's clock stopped where this one's starts, so the process's times
  // move back by that much, which keeps its phase.
  for (NaplpsState::Blink& blink : state_.blinks) {
    blink.process.defined_ms -= previous_end_ms_;
    blink.process.first_on_ms -= previous_end_ms_;
    blink.timeline_index =
        static_cast<uint32_t>(snapshot_.timeline.blinks.size());
    snapshot_.timeline.blinks.push_back(blink.process);
    NabtsPaletteEvent event;
    event.kind = NabtsPaletteEvent::Kind::kBlinkStart;
    event.time_ms = blink.process.defined_ms;
    event.process = blink.timeline_index;
    snapshot_.timeline.events.push_back(event);
  }

  frames_.clear();
  collecting_ = Collecting::kNothing;
  definition_body_.clear();
  drcs_target_ = nullptr;
  mask_target_ = nullptr;
  have_last_drcs_code_ = false;
  last_byte_graphic_ = false;
  escape_follows_graphic_ = false;
  pending_mark_class_ = -1;
  defining_frame_index_ = 0;
  definition_had_code_ = false;
  wrap_suppress_ = WrapSuppress::kNone;
  macros_defined_.reset();
  macros_invoked_.reset();

  // §6.1.4, §6.1.5, §6.1.6.1: the transmission and device controls and NUL
  // "have no effect on the presentation layer ... They may be embedded within
  // any presentation layer sequence without affecting that sequence" — an
  // escape sequence, an operand run, the bytes a control takes after itself —
  // so they are dropped as the record is read, as both receivers of the day
  // drop them at fetch.
  if (trace_ != nullptr) {
    trace_->assign(record.size(), NaplpsByteRole::kUnread);
  }
  record_.clear();
  record_offsets_.clear();
  record_parity_failed_.clear();
  record_.reserve(record.size());
  record_offsets_.reserve(record.size());
  record_parity_failed_.reserve(record.size());
  // CEA-516 §3.3 gives every data byte of a teletext record odd parity, so a
  // byte that arrived failing it — the vote had no copy with a clean value for
  // it — is known to be wrong. Neither receiver of the day executes such a
  // byte as a control or a drawing command: the Panasonic TU-2000x flags the
  // failure as it fetches the byte and
  // skips a C0 or C1 control or a PDI opcode carrying the flag, and the Norpak
  // TTX 650 discards the whole block the byte came in. Run as what it reads
  // as, a byte that was a shift or an operand clears the page or reads its
  // drawing as text: a page can lose its whole picture to a CS failing parity
  // where the page it copies sends SO. A record passed without its parity (no
  // byte with b8 set) has nothing to test.
  const bool carries_parity =
      std::any_of(record.begin(), record.end(),
                  [](uint8_t byte) { return (byte & 0x80) != 0; });
  for (size_t i = 0; i < record.size(); ++i) {
    const auto byte = static_cast<uint8_t>(record[i] & kSevenBits);
    if (naplps_is_transparent_control(byte)) {
      if (trace_ != nullptr) {
        (*trace_)[i] = NaplpsByteRole::kTransparent;
      }
      continue;
    }
    const bool parity_failed =
        carries_parity && !teletext_odd_parity_valid(record[i]);
    if (parity_failed && is_c0(byte)) {
      ++snapshot_.diagnostics.parity_skipped;
      if (trace_ != nullptr) {
        (*trace_)[i] = NaplpsByteRole::kTransparent;
      }
      continue;
    }
    record_.push_back(byte);
    record_offsets_.push_back(i);
    record_parity_failed_.push_back(parity_failed ? 1 : 0);
  }

  // Room for the deepest nesting allowed, so no push moves a frame that a
  // handler further down the stack still holds a reference to.
  frames_.reserve(kNaplpsMaxMacroDepth + 2);
  frames_.push_back(Frame{record_.data(), record_.size(), 0, nullptr});
  while (!frames_.empty()) {
    if (!step()) {
      frames_.pop_back();
      // A definition opened inside a macro expansion keeps collecting from the
      // frame below once the expansion runs out.
      if (!frames_.empty() && defining_frame_index_ >= frames_.size()) {
        defining_frame_index_ = frames_.size() - 1;
      }
    }
  }

  // A definition left open by a truncated record is closed rather than dropped:
  // the bytes that did arrive defined something.
  end_definition();

  // The digits this record leaves linked (linked_keys()): the macros 2/0 to 2/9
  // it defined and kept, and never ran itself.
  linked_keys_ = 0;
  for (uint8_t key = 0; key < kNaplpsLinkedKeyCount; ++key) {
    const auto code = static_cast<uint8_t>(kNaplpsFirstLinkedMacro + key);
    const size_t index = code - kNaplpsFirstCode;
    const NaplpsMacro* entry = state_.macro(code);
    if (macros_defined_[index] && !macros_invoked_[index] && entry != nullptr &&
        entry->defined() && !entry->transmit) {
      linked_keys_ = static_cast<uint16_t>(linked_keys_ | (1u << key));
    }
  }

  flush_map_writes();
  compact_map_writes(snapshot_.timeline.events);
  snapshot_.timeline.end_ms = static_cast<uint32_t>(clock_ms_);
  previous_end_ms_ = clock_ms_;

  // §5.3.2.5: a colour map write is retroactive — "A change in the color map
  // will immediately be reflected in the color of all pixels whose associated
  // color map address points to the color map entry that has been changed". A
  // real display stores addresses per pixel and resolves them through the map
  // at display time, so what is on screen at the end is everything resolved
  // through the *final* map. The display list stores each primitive's address
  // and resolves it here; how the entry changed on the way is the timeline's.
  const NabtsColour* final_map = state_.colour.map();
  const auto resolve_colours = [final_map](NabtsPrimitive& primitive) {
    if (primitive.colour_map_address >= 0) {
      primitive.colour =
          final_map[primitive.colour_map_address % kNabtsColourMapEntries];
    }
    if (primitive.background_map_address >= 0) {
      primitive.background =
          final_map[primitive.background_map_address % kNabtsColourMapEntries];
    }
    for (size_t i = 0; i < primitive.incremental_addresses.size(); ++i) {
      if (primitive.incremental_addresses[i] >= 0) {
        primitive.incremental_colours[i] =
            final_map[primitive.incremental_addresses[i] %
                      kNabtsColourMapEntries];
      }
    }
  };
  for (NabtsPrimitive& primitive : snapshot_.primitives) {
    resolve_colours(primitive);
  }
  for (NabtsPrimitive& primitive : snapshot_.timeline.superseded) {
    resolve_colours(primitive);
  }

  state_.colour.copy_map_to(snapshot_.colour_map);
  snapshot_.drcs = state_.defined_drcs();
  for (size_t i = 0; i < kNabtsTextureMaskCount; ++i) {
    snapshot_.texture_masks[i] = state_.texture_masks[i];
  }
  return snapshot_;
}

bool NaplpsInterpreter::step() {
  if (frames_.back().position >= frames_.back().length) {
    return false;
  }
  // Everything the step consumes from the record itself — a control and the
  // bytes it takes after itself, an opcode and its operands — is read the way
  // the code executing it settles.
  const bool traced = trace_ != nullptr && frames_.size() == 1;
  const size_t start = frames_.back().position;
  role_ = NaplpsByteRole::kControl;
  step_byte();
  if (traced) {
    for (size_t i = start; i < frames_.front().position; ++i) {
      (*trace_)[record_offsets_[i]] = role_;
    }
  }
  return true;
}

void NaplpsInterpreter::step_byte() {
  Frame& frame = frames_.back();
  // Parity is known only for the record's own bytes, not for a macro's.
  executing_parity_failure_ = frames_.size() == 1 &&
                              frame.position < record_parity_failed_.size() &&
                              record_parity_failed_[frame.position] != 0;
  const uint8_t byte = frame.bytes[frame.position++];

  // Only the frame the definition was opened in is scanned for terminators and
  // stored from. Bytes from deeper frames are a macro expanding inside the
  // definition, and §6.2.2.1 stores "that reference only, not the expansion".
  const bool from_defining_frame = frames_.size() - 1 == defining_frame_index_;
  // Whether this byte is the ESC of a control function that ends the
  // definition under way.
  NaplpsEscape escape;
  bool ends_definition = false;
  if (collecting_ != Collecting::kNothing && from_defining_frame &&
      byte == kNaplpsEsc) {
    escape = naplps_parse_escape(frame.bytes + frame.position,
                                 frame.length - frame.position);
    ends_definition = escape.kind == NaplpsEscapeKind::kControl &&
                      naplps_terminates_definition(escape.c1);
  }

  // A macro definition stores its bytes rather than executing them (§6.2.2.1),
  // so the only thing that matters while collecting is whether this byte ends
  // the definition. DEFP MACRO is the exception: §6.2.2.2 has it
  // "simultaneously executed and stored".
  if (collecting_ == Collecting::kMacro ||
      collecting_ == Collecting::kMacroExecuting) {
    if (from_defining_frame) {
      if (ends_definition) {
        // §6.2.2.1: "Neither the terminating control character nor its
        // preceding ESC character in a 7-bit environment is stored as part of
        // the macro."
        frame.position += escape.length - 1;
        end_definition();
        execute_c1(escape.c1);
        return;
      }
      if (collecting_ == Collecting::kMacro) {
        definition_body_.push_back(byte);
        role_ = NaplpsByteRole::kDefinition;
        pace(naplps_pace_definition_byte_us(pace_));
        return;
      }
      // DEFP MACRO: execute the byte, then store it together with whatever
      // lookahead its execution consumed from this frame — escape sequences,
      // PDI operands, cursor addresses. Storing the lead byte alone left the
      // body a different program from the one that just ran.
      const size_t start = frame.position - 1;
      execute_byte(byte);
      const Frame& defining = frames_[defining_frame_index_];
      definition_body_.insert(definition_body_.end(), defining.bytes + start,
                              defining.bytes + defining.position);
      return;
    }
    // A macro expanding inside a DEFP MACRO body: executed, not stored.
    execute_byte(byte);
    return;
  }

  // §6.2.3: a DRCS definition terminated "with no intervening presentation
  // layer code" frees the character, so note whether this byte is code within
  // the definition or the sequence that terminates it.
  if ((collecting_ == Collecting::kDrcs ||
       collecting_ == Collecting::kTextureMask) &&
      from_defining_frame && !ends_definition) {
    definition_had_code_ = true;
  }

  execute_byte(byte);
}

void NaplpsInterpreter::execute_byte(uint8_t byte) {
  // §6.2.7.2 repeats "the immediately preceding byte", so what REPEAT may
  // repeat lasts one byte.
  const bool follows_graphic = last_byte_graphic_;
  last_byte_graphic_ = false;
  if (is_c0(byte)) {
    if (byte == kNaplpsEsc) {
      escape_follows_graphic_ = follows_graphic;
    }
    execute_c0(byte);
    return;
  }
  execute_graphic(byte);
}

void NaplpsInterpreter::execute_c0(uint8_t byte) {
  pace(naplps_pace_c0_us(pace_, byte));
  switch (byte) {
    case kNaplpsEsc:
      execute_escape();
      return;

    // §6.1.3: the locking and non-locking invocations.
    case kNaplpsSi:
      env_.invoke_locking(NaplpsGSlot::kG0);
      return;
    case kNaplpsSo:
      env_.invoke_locking(NaplpsGSlot::kG1);
      return;
    case kNaplpsSs2:
      env_.invoke_single_shift(NaplpsGSlot::kG2);
      return;
    case kNaplpsSs3:
      env_.invoke_single_shift(NaplpsGSlot::kG3);
      return;

    // §6.1.2: the format effectors. The cursor moves; nothing is drawn. All
    // four are defined relative to the character path, and they are four
    // different movements — a line feed that stepped along the path instead of
    // across it would draw every row of a page on top of the last.
    case kNaplpsApb:
      move_cursor_by(CursorMove::kBackward);
      return;
    case kNaplpsApf:
      move_cursor_by(CursorMove::kForward);
      return;
    case kNaplpsApd:
      // §5.3.2.3.6: the explicit APR APD (or APD APR) after an automatic one
      // is a null operation — the wrap already did its work.
      if (wrap_suppress_ == WrapSuppress::kArmed) {
        wrap_suppress_ = WrapSuppress::kSawApd;
        return;
      }
      if (wrap_suppress_ == WrapSuppress::kSawApr) {
        wrap_suppress_ = WrapSuppress::kNone;
        return;
      }
      move_cursor_by(CursorMove::kDown);
      return;
    case kNaplpsApu:
      move_cursor_by(CursorMove::kUp);
      return;
    case kNaplpsApr:
      if (wrap_suppress_ == WrapSuppress::kArmed) {
        wrap_suppress_ = WrapSuppress::kSawApr;
        return;
      }
      if (wrap_suppress_ == WrapSuppress::kSawApd) {
        wrap_suppress_ = WrapSuppress::kNone;
        return;
      }
      // §6.1.2.7: "to the first character position within the display area
      // (or within the active field ...) along the character path".
      move_cursor(line_start());
      return;
    case kNaplpsCs:
      // §6.1.2.6: clears the display area and homes the cursor. "In color
      // modes 0 and 1, it clears the display area to nominal black. In color
      // mode 2, it clears the display area to the background color."
      if (state_.colour.mode() == NabtsColourMode::kMappedWithBackground) {
        clear_display(state_.colour.background_colour(),
                      static_cast<int>(state_.colour.map_background_address()));
      } else {
        clear_display(state_.colour.nominal_black(),
                      state_.colour.nominal_black_address());
      }
      move_cursor(state_.home_position());
      return;
    case kNaplpsAph:
      move_cursor(state_.home_position());
      return;

    case kNaplpsNsr: {
      apply_nsr_reset();

      // §6.1.6.5(6): NSR "can be used as an alternative means to position the
      // cursor". Two bytes from columns 4 to 7 are a row and a column address;
      // two from columns 2 and 3 "are ignored"; a control among them
      // "terminate[s] the NSR sequence" and is executed. A pair mixing the
      // columns the clause leaves open, and it is consumed without moving the
      // cursor, as the Norpak TTX 650 does. Note the origin: "row 0, column 0
      // in the upper leftmost character position in the display area" — the
      // opposite end from APS's own numbering (§6.1.2.4).
      Frame& frame = frames_.back();
      if (frame.position + 1 < frame.length &&
          !is_c0(frame.bytes[frame.position]) &&
          !is_c0(frame.bytes[frame.position + 1])) {
        const uint8_t row_byte = frame.bytes[frame.position];
        const uint8_t column_byte = frame.bytes[frame.position + 1];
        frame.position += 2;
        const auto is_address = [](uint8_t byte) {
          return byte >= 0x40;  // columns 4 to 7
        };
        if (is_address(row_byte) && is_address(column_byte)) {
          const int row = row_byte & 0x3F;
          const int column = column_byte & 0x3F;
          const double dx = std::fabs(state_.text.character_field.dx);
          const double dy = std::fabs(state_.text.character_field.dy);
          // Row 0's character field has its top on the top of the display area,
          // and the cursor is that field's lower left corner (§5.3.2.3.2).
          move_cursor(NabtsPoint{
              static_cast<double>(column) * dx,
              kNabtsDisplayAreaHeight - static_cast<double>(row + 1) * dy});
          return;
        }
      }
      // No address: the reset alone. "If either of the two bytes following
      // the NSR is not from columns 4 through 7 ..., the non-selective reset
      // function of NSR is executed and the cursor is not repositioned" — it
      // stays where it was, as both receivers of the day leave it.
      return;
    }

    case kNaplpsCan:
      // §6.1.6.3: terminate every executing macro. Execution resumes after the
      // outermost one, which is frame 0.
      while (frames_.size() > 1) {
        frames_.pop_back();
      }
      return;

    case kNaplpsAps: {
      // §6.1.2.4: the two bytes following are a row and column address in the
      // nominal screen format the current character field establishes.
      Frame& frame = frames_.back();
      if (frame.position + 1 >= frame.length) {
        ++snapshot_.diagnostics.truncated_pdis;
        return;
      }
      const uint8_t row_byte = frame.bytes[frame.position];
      const uint8_t column_byte = frame.bytes[frame.position + 1];
      if (is_c0(row_byte) || is_c0(column_byte)) {
        // "If either of the characters following the APS character is a C0 or
        // C1 control, the APS is ignored and the C0 or C1 control is executed."
        return;
      }
      frame.position += 2;
      const int row = (row_byte & 0x7F) - 32;
      const int column = (column_byte & 0x7F) - 32;
      const double dx = std::fabs(state_.text.character_field.dx);
      const double dy = std::fabs(state_.text.character_field.dy);
      // "Rows and columns are numbered starting with row 0, column 0, in the
      // lower leftmost character position of the display area."
      move_cursor(NabtsPoint{static_cast<double>(column) * dx,
                             static_cast<double>(row) * dy});
      return;
    }

    // BEL (§6.1.6.2) is a transient indication and SDC (§6.1.6.4) a null
    // operation at this layer, and anything left is a C0 position this standard
    // does not define. None of them draws.
    case kNaplpsBel:
    case kNaplpsSdc:
    default:
      return;
  }
}

void NaplpsInterpreter::execute_escape() {
  Frame& frame = frames_.back();
  const NaplpsEscape escape = naplps_parse_escape(
      frame.bytes + frame.position, frame.length - frame.position);

  // |length| counts the ESC, which has already been consumed.
  frame.position += escape.length - 1;

  switch (escape.kind) {
    case NaplpsEscapeKind::kDesignation:
      env_.designate(escape.slot, escape.set);
      return;
    case NaplpsEscapeKind::kLockingShift:
      env_.invoke_locking(escape.slot);
      return;
    case NaplpsEscapeKind::kControl:
      execute_c1(escape.c1);
      return;
    // A sequence naming a set this does not implement, and one broken by a byte
    // outside the syntax, come to the same thing here: nothing is designated
    // and nothing is invoked. They differ only in what was consumed, which
    // naplps_parse_escape() has already decided — §4.3.2 leaves the offending
    // byte of a malformed sequence to be executed in its own right.
    case NaplpsEscapeKind::kUnsupported:
    case NaplpsEscapeKind::kMalformed:
      return;
    case NaplpsEscapeKind::kTruncated:
      ++snapshot_.diagnostics.truncated_pdis;
      return;
  }
}

void NaplpsInterpreter::apply_nsr_reset() {
  // §6.1.6.5, items (1) to (5). What is *not* here is the point: the colour
  // map ("The color map is not changed"), the programmable masks ("The
  // programmable masks are not cleared"), macros and DRCS — only the selective
  // RESET of §5.3.2.9 clears those, which is what lets a Support Record's
  // definitions survive the NSR every page is recommended to open with
  // (CEA-516 §8.7.2.3.1). Blink processes are likewise not in the list, so
  // they are left running.
  env_.reset();
  state_.domain.reset();
  // (3) covers "the text parameters (from the TEXT opcode, from the C1 set
  // and the active field)" — §5.3.2.9.3's wording, which includes the active
  // field itself.
  state_.text.reset();
  state_.reset_active_field();
  state_.texture.reset();
  // (5): "The color mode is set to color mode 0 and the drawing color is set
  // to nominal white."
  state_.colour.reset_drawing_to_white();
}

void NaplpsInterpreter::clear_display(const NabtsColour& colour,
                                      int map_address) {
  // The clear takes the receiver its time, the same in every colour mode, and
  // what was on screen goes when it is done: swept off a row at a time
  // (NabtsPresentationTimeline::fill_sweep) over the time it took.
  const int64_t started_ms = clock_ms_;
  pace(naplps_pace_clear_us(pace_));
  const auto took_ms = static_cast<uint32_t>(clock_ms_ - started_ms);
  // What was on screen goes, but it was on screen: kept, in paint order, so a
  // page that draws, waits and clears can be shown doing so.
  std::vector<NabtsPrimitive>& superseded = snapshot_.timeline.superseded;
  superseded.insert(superseded.end(),
                    std::make_move_iterator(snapshot_.primitives.begin()),
                    std::make_move_iterator(snapshot_.primitives.end()));
  snapshot_.primitives.clear();
  snapshot_.timeline.clears.push_back(NabtsDisplayClear{
      static_cast<uint32_t>(clock_ms_), superseded.size(), took_ms});

  // Nominal black with no map address behind it is what a renderer shows for
  // an empty display list anyway.
  const bool black = colour.red == 0 && colour.green == 0 && colour.blue == 0 &&
                     !colour.transparent;
  if (black && map_address < 0) {
    return;
  }

  // The clear is recorded as a full-display-area filled rectangle, drawn
  // plainly whatever texture attributes happen to be in force — a clear is a
  // flood, not a patterned fill.
  NabtsPrimitive primitive;
  primitive.kind = NabtsPrimitiveKind::kRectangle;
  primitive.filled = true;
  primitive.origin = NabtsPoint{0.0, 0.0};
  primitive.size = NabtsSize{1.0, kNabtsDisplayAreaHeight};
  primitive.points.push_back(NabtsPoint{0.0, 0.0});
  primitive.points.push_back(NabtsPoint{1.0, kNabtsDisplayAreaHeight});
  primitive.colour_mode = state_.colour.mode();
  primitive.colour = colour;
  primitive.colour_map_address =
      map_address < 0 ? int16_t{-1} : static_cast<int16_t>(map_address);
  primitive.appear_ms = static_cast<uint32_t>(clock_ms_);
  // The colour floods the display as the clear sweeps it.
  primitive.draw_ms = took_ms;
  snapshot_.primitives.push_back(std::move(primitive));
}

// ---------------------------------------------------------------------------
// Time
// ---------------------------------------------------------------------------

void NaplpsInterpreter::flush_map_writes() {
  for (const NaplpsColourState::MapWrite& write : state_.colour.take_writes()) {
    NabtsPaletteEvent event;
    event.kind = NabtsPaletteEvent::Kind::kMapWrite;
    event.time_ms = clock_ms_;
    event.address = write.address;
    event.colour = write.colour;
    snapshot_.timeline.events.push_back(event);
  }
}

void NaplpsInterpreter::define_blink(uint32_t from, int16_t to_address,
                                     const NabtsColour& to_colour,
                                     uint32_t on_ms, uint32_t off_ms,
                                     uint32_t delay_ms) {
  // §5.3.2.7.4: "Defining a blink process on a pair of blink-to and
  // blink-from colors automatically terminates any previously defined blink
  // process operating on the same pair of colors."
  end_blinks(BlinkSelection::kPair, from, to_address);
  if (state_.blinks.size() >= kNaplpsMaxBlinkProcesses) {
    // Table D1 item 5(5) guarantees sixteen and no more, so a service asking
    // for a seventeenth is asking for something no receiver promised.
    return;
  }

  NabtsBlinkProcess process;
  process.from = static_cast<uint8_t>(from % kNabtsColourMapEntries);
  process.to_address = to_address;
  process.to_colour = to_colour;
  process.on_ms = on_ms;
  process.off_ms = off_ms;
  process.defined_ms = clock_ms_;
  // §5.3.2.7.2: "a delay in the start of the ON interval referenced to the
  // start of the ON interval of the most recently defined active blink
  // process. A start delay specification when there are no active blink
  // processes has no effect." A delay of zero with one running is still
  // measured from it, which is what keeps the processes of one command in
  // step with each other.
  process.first_on_ms =
      state_.blinks.empty()
          ? next_blink_tick(clock_ms_)
          : first_on_after(state_.blinks.back().process, clock_ms_, delay_ms);

  NaplpsState::Blink blink;
  blink.process = process;
  blink.timeline_index =
      static_cast<uint32_t>(snapshot_.timeline.blinks.size());
  snapshot_.timeline.blinks.push_back(process);
  state_.blinks.push_back(blink);

  NabtsPaletteEvent event;
  event.kind = NabtsPaletteEvent::Kind::kBlinkStart;
  event.time_ms = clock_ms_;
  event.process = blink.timeline_index;
  snapshot_.timeline.events.push_back(event);
}

void NaplpsInterpreter::end_blinks(BlinkSelection selection, uint32_t from,
                                   int16_t to_address) {
  // Map writes made before this belong before it on the clock.
  flush_map_writes();
  const uint8_t entry = static_cast<uint8_t>(from % kNabtsColourMapEntries);
  auto& running = state_.blinks;
  for (auto it = running.begin(); it != running.end();) {
    const NabtsBlinkProcess& process = it->process;
    const bool selected =
        selection == BlinkSelection::kAll ||
        (process.from == entry && (selection == BlinkSelection::kFrom ||
                                   process.to_address == to_address));
    if (!selected) {
      ++it;
      continue;
    }
    snapshot_.timeline.blinks[it->timeline_index].ended_ms = clock_ms_;
    NabtsPaletteEvent event;
    event.kind = NabtsPaletteEvent::Kind::kBlinkEnd;
    event.time_ms = clock_ms_;
    event.process = it->timeline_index;
    snapshot_.timeline.events.push_back(event);
    it = running.erase(it);
  }
}

void NaplpsInterpreter::execute_c1(NaplpsC1 control) {
  pace(naplps_pace_c1_us(pace_, static_cast<uint8_t>(control)));
  switch (control) {
    // §6.2.2, §6.2.3, §6.2.4: the definition openers. Each takes the code of
    // what is being defined from the next byte.
    case NaplpsC1::kDefMacro:
    case NaplpsC1::kDefpMacro:
    case NaplpsC1::kDeftMacro:
    case NaplpsC1::kDefDrcs:
    case NaplpsC1::kDefTexture: {
      const Collecting terminated = collecting_;
      end_definition();

      // §6.2.3's one exception: a DEF DRCS that terminated a previous DEF DRCS
      // is *not* followed by a code — "the next character of the DRCS G-set
      // (ie, in the circular sequence 2/0, 2/1, ... 7/15, 2/0 ...) is defined
      // by the presentation layer code immediately following this new DEF DRCS
      // command. (This is the only time DEF DRCS is not followed by the DRCS
      // character to be defined.)" Consuming a code byte here would both
      // define the wrong character and eat the first byte of its definition.
      if (control == NaplpsC1::kDefDrcs && terminated == Collecting::kDrcs &&
          have_last_drcs_code_) {
        const uint8_t next = last_drcs_code_ >= kNaplpsLastCode
                                 ? kNaplpsFirstCode
                                 : static_cast<uint8_t>(last_drcs_code_ + 1);
        begin_definition(Collecting::kDrcs, next);
        return;
      }

      Frame& frame = frames_.back();
      uint8_t code = 0;
      if (frame.position < frame.length) {
        code = frame.bytes[frame.position];
        if (is_graphic(code)) {
          ++frame.position;
        } else {
          // §6.2.2.1: "If the character following the DEF MACRO control is not
          // in this range, the entire command ... is in error and is executed
          // as a null operation." The offending byte is left to be executed.
          return;
        }
      }

      switch (control) {
        case NaplpsC1::kDefMacro:
          definition_is_transmit_ = false;
          begin_definition(Collecting::kMacro, code);
          return;
        case NaplpsC1::kDefpMacro:
          definition_is_transmit_ = false;
          begin_definition(Collecting::kMacroExecuting, code);
          return;
        case NaplpsC1::kDeftMacro:
          definition_is_transmit_ = true;
          begin_definition(Collecting::kMacro, code);
          return;
        case NaplpsC1::kDefDrcs:
          begin_definition(Collecting::kDrcs, code);
          return;
        default:
          begin_definition(Collecting::kTextureMask, code);
          return;
      }
    }

    case NaplpsC1::kEnd:
      end_definition();
      return;

    // §6.2.7.2: "This command causes the immediately preceding byte to be
    // repeated a specific number of additional times if the byte is SPACE or
    // any spacing character from the primary, supplementary, DRCS, or mosaic
    // sets. Otherwise, the command is in error and shall be executed as a null
    // operation." The count byte "shall be in the range 4/0 through 7/15;
    // otherwise the command is in error and shall be executed as a null
    // operation, and the count character is executed" in its own right.
    case NaplpsC1::kRepeat: {
      Frame& frame = frames_.back();
      if (frame.position >= frame.length) {
        return;
      }
      const uint8_t count_byte = frame.bytes[frame.position];
      if (count_byte < 0x40) {
        return;
      }
      ++frame.position;
      if (!escape_follows_graphic_) {
        return;
      }
      const auto count = static_cast<uint8_t>(count_byte & 0x3F);
      for (uint8_t i = 0; i < count; ++i) {
        draw_character(last_graphic_, last_graphic_set_);
      }
      return;
    }
    // §6.2.7.3: "the immediately preceding byte to be repeated until the last
    // character position along the current character path is reached" — along
    // whichever path, and "only up to the last character position along the
    // current character path within the active field" where the cursor's field
    // lies within one. The last position is the one whose advance would take
    // the automatic APR APD, which ends the repetition as it would a line.
    case NaplpsC1::kRepeatToEol: {
      if (!escape_follows_graphic_) {
        return;
      }
      // Bounded, so a damaged character field cannot make this run away.
      constexpr size_t kMaxRepeats = 1024;
      const FieldExtent extent = field_extent(state_.text);
      for (size_t i = 0; i < kMaxRepeats; ++i) {
        if (!field_fits(confining_area(state_, kUnitScreenArea, extent),
                        state_.cursor, extent)) {
          break;
        }
        draw_character(last_graphic_, last_graphic_set_);
        if (line_wrapped_) {
          break;
        }
      }
      return;
    }

    // §6.2.7.4-5.
    case NaplpsC1::kReverseVideo:
      state_.text.reverse_video = true;
      return;
    case NaplpsC1::kNormalVideo:
      state_.text.reverse_video = false;
      return;

    // §6.2.7.6-10: each sets the character field to a stated size.
    case NaplpsC1::kSmallText:
      state_.text.character_field = NaplpsTextState::small_field();
      return;
    case NaplpsC1::kMediumText:
      state_.text.character_field = NaplpsTextState::medium_field();
      return;
    case NaplpsC1::kNormalText:
      state_.text.character_field = NaplpsTextState::normal_field();
      return;
    case NaplpsC1::kDoubleHeight:
      state_.text.character_field = NaplpsTextState::double_height_field();
      return;
    case NaplpsC1::kDoubleSize:
      state_.text.character_field = NaplpsTextState::double_size_field();
      return;

    // §6.2.8.1: "creates a blink process in which: the blink-from color is the
    // drawing color; the blink-to color is nominal black in color modes 0 and 1
    // or the background color in color mode 2; the on and off intervals are
    // implementation-dependent; and the phase delay is 0."
    case NaplpsC1::kBlinkStart:
      if (state_.colour.mode() == NabtsColourMode::kDirect) {
        // "objects drawn with the previous drawing color will remain not
        // blinking, but objects subsequently drawn with the new drawing color
        // will blink" — so mode 0 gives the colour an entry of its own first.
        state_.colour.claim_entry_for_drawing_colour();
        flush_map_writes();
      }
      if (state_.colour.mode() == NabtsColourMode::kMappedWithBackground) {
        // The background has a map address of its own, so the process follows
        // a later write to it the way the drawing colour does.
        define_blink(
            state_.colour.drawing_address(),
            static_cast<int16_t>(state_.colour.map_background_address()),
            NabtsColour{}, kBlinkStartOnMs, kBlinkStartOffMs, 0);
      } else {
        define_blink(state_.colour.drawing_address(),
                     state_.colour.nominal_black_address(),
                     state_.colour.nominal_black(), kBlinkStartOnMs,
                     kBlinkStartOffMs, 0);
      }
      return;
    // §6.2.8.2: "turns off any currently active blink processes utilizing the
    // drawing color as the blink-from color" — the drawing colour's, not every
    // process running.
    case NaplpsC1::kBlinkStop:
      end_blinks(BlinkSelection::kFrom, state_.colour.drawing_address());
      return;

    // §6.2.7.13-16.
    case NaplpsC1::kScrollOn:
      state_.text.scroll = true;
      return;
    case NaplpsC1::kScrollOff:
      state_.text.scroll = false;
      return;
    case NaplpsC1::kUnderlineStart:
      state_.text.underlined = true;
      return;
    case NaplpsC1::kUnderlineStop:
      state_.text.underlined = false;
      return;

    // §6.2.6: Table D1 item 12(1) makes these no-ops for the teletext service —
    // "The execution of the PROTECT and UNPROTECT commands shall have no
    // effect" — and §6.2.8.3's extended device controls are outside this layer.
    // Word wrap (§6.2.7.11-12) and the cursor (§6.2.7.17-19) change nothing a
    // page draws: no receiver of the day wrapped words, and a page shown for
    // reading has no cursor on it.
    case NaplpsC1::kProtect:
    case NaplpsC1::kUnprotect:
    case NaplpsC1::kEdc1:
    case NaplpsC1::kEdc2:
    case NaplpsC1::kEdc3:
    case NaplpsC1::kEdc4:
    case NaplpsC1::kWordWrapOn:
    case NaplpsC1::kWordWrapOff:
    case NaplpsC1::kFlashCursor:
    case NaplpsC1::kSteadyCursor:
    case NaplpsC1::kCursorOff:
      return;
  }
}

void NaplpsInterpreter::execute_graphic(uint8_t byte) {
  const NaplpsGSet set = env_.in_use();
  env_.consume_character();

  switch (set) {
    case NaplpsGSet::kPdi:
      // §5.3.1: b7 clear is an opcode, b7 set is numeric data. Numeric data
      // with no opcode in front of it has nothing to be an operand of.
      role_ = NaplpsByteRole::kPdi;
      if (naplps_is_pdi_opcode(byte)) {
        // An opcode failing parity is skipped, as the TU-2000x skips it, and
        // its operands follow it as data with no opcode in front of them.
        if (executing_parity_failure_) {
          ++snapshot_.diagnostics.parity_skipped;
          return;
        }
        execute_pdi(byte);
      }
      return;

    case NaplpsGSet::kMacro:
      role_ = NaplpsByteRole::kOther;
      invoke_macro(byte);
      return;

    case NaplpsGSet::kNull:
      // §4.3.2: "A null set is a set in which all code positions are executed
      // as null operations."
      role_ = NaplpsByteRole::kOther;
      return;

    case NaplpsGSet::kPrimary:
    case NaplpsGSet::kSupplementary:
      // §4.4: with a 94-position set invoked, 7/15 is DELETE, which "is
      // executed as a null operation" — nothing drawn, the cursor not moved.
      // The 96-position sets below give 7/15 a character of its own.
      if (byte == kNaplpsDelete) {
        role_ = NaplpsByteRole::kText;
        return;
      }
      [[fallthrough]];
    case NaplpsGSet::kMosaic:
    case NaplpsGSet::kDrcs:
      role_ = NaplpsByteRole::kText;
      draw_character(byte, set);
      // REPEAT repeats a spacing character only (§6.2.7.2).
      if (set != NaplpsGSet::kSupplementary ||
          !nabts_supplementary_is_nonspacing(byte)) {
        last_graphic_ = byte;
        last_graphic_set_ = set;
        last_byte_graphic_ = true;
      }
      return;
  }
}

void NaplpsInterpreter::draw_character(uint8_t byte, NaplpsGSet set) {
  NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kCharacter);
  primitive.character = byte;
  primitive.repertoire = repertoire_of(set);
  if (set == NaplpsGSet::kDrcs) {
    primitive.drcs_index = static_cast<uint8_t>(byte - kNaplpsFirstCode);
  }
  primitive.origin = state_.cursor;
  primitive.points.push_back(state_.cursor);
  primitive.size = state_.text.character_field;
  primitive.rotation = state_.text.rotation;
  primitive.path = state_.text.path;
  primitive.reverse_video = state_.text.reverse_video;
  primitive.underlined = state_.text.underlined;

  // §7.2: a composite character is transmitted as a non-spacing mark from the
  // supplementary set followed by the letter it applies to, and Table 27's
  // note has the mark's coded representation "precede those of the
  // characters" it modifies. The pair occupies one character field — that is
  // what makes it one character of the repertoire (§7.1) — so the mark itself
  // must not move the cursor, or every accented letter would come out with a
  // gap in front of it.
  const bool mark = set == NaplpsGSet::kSupplementary &&
                    nabts_supplementary_is_nonspacing(byte);
  const double advance = proportional_advance(byte, set, mark, primitive);
  emit(std::move(primitive));

  line_wrapped_ = false;
  if (!mark) {
    line_wrapped_ = advance_cursor(advance);
  }
}

double NaplpsInterpreter::proportional_advance(uint8_t byte, NaplpsGSet set,
                                               bool mark,
                                               NabtsPrimitive& primitive) {
  // T.101 Annex D's Addendum (clause 11) spaces the characters of the
  // repertoire; mosaics and DRCS keep the field (clause 11's function "applies
  // only to the character repertoire as defined in clause D.7").
  const bool repertoire =
      set == NaplpsGSet::kPrimary || set == NaplpsGSet::kSupplementary;
  if (!repertoire || !spaced_proportionally(state_.text)) {
    pending_mark_class_ = -1;
    return 0.0;
  }
  const bool supplementary = set == NaplpsGSet::kSupplementary;
  int width_class = naplps_width_class(byte, supplementary);
  if (mark) {
    // Its class waits for the letter it is put on (clause 11.1).
    const bool same_field = pending_mark_class_ >= 0 &&
                            pending_mark_cursor_.x == state_.cursor.x &&
                            pending_mark_cursor_.y == state_.cursor.y;
    pending_mark_class_ =
        same_field ? std::max(pending_mark_class_, width_class) : width_class;
    pending_mark_cursor_ = state_.cursor;
    return 0.0;
  }
  const bool composite = pending_mark_class_ >= 0 &&
                         pending_mark_cursor_.x == state_.cursor.x &&
                         pending_mark_cursor_.y == state_.cursor.y;
  if (composite) {
    width_class = std::max(width_class, pending_mark_class_);
  }
  pending_mark_class_ = -1;

  const NabtsSize& field = state_.text.character_field;
  // A field too narrow to present a character in is spaced at the full width,
  // as the Norpak TTX 650 spaces the dots it draws there.
  if (naplps_reference_pixels(field.dx) < kNaplpsSmallestFieldColumns) {
    width_class = kNaplpsFullWidthClass;
  }

  // The ink starts at the cursor, along the field's width. A composite stays
  // in the field its marks were drawn in, where the raster composes it, and a
  // reflected field keeps its ink where the reflection puts it.
  if (!composite && field.dx > 0.0) {
    const double offset =
        ink_offset(supplementary ? nabts_supplementary_to_unicode(byte)
                                 : nabts_primary_to_unicode(byte),
                   field);
    switch (state_.text.rotation) {
      case NabtsCharRotation::kNone:
        primitive.origin.x -= offset;
        break;
      case NabtsCharRotation::k90:
        primitive.origin.y -= offset;
        break;
      case NabtsCharRotation::k180:
        primitive.origin.x += offset;
        break;
      case NabtsCharRotation::k270:
        primitive.origin.y += offset;
        break;
    }
  }
  return naplps_proportional_displacement(width_class, field.dx);
}

// ---------------------------------------------------------------------------
// PDI
// ---------------------------------------------------------------------------

std::vector<uint8_t> NaplpsInterpreter::gather_operands() {
  std::vector<uint8_t> operands;
  Frame& frame = frames_.back();
  while (frame.position < frame.length) {
    const uint8_t byte = frame.bytes[frame.position];
    if (naplps_is_pdi_numeric(byte)) {
      operands.push_back(byte);
      ++frame.position;
      continue;
    }
    // Anything else terminates the sequence and is left to be executed.
    //
    // §5.3.1 also allows a PDI to continue into the operand data of a macro:
    // "The invocation of a macro either from the in-use table or by single
    // shift will not by itself terminate a PDI". That is not followed here —
    // gathering stops at the end of the frame it started in — because a PDI
    // whose operands span a macro boundary would need the operand reader to
    // walk the frame stack, and no observed service does it. A record that did
    // would lose that one PDI's tail rather than desynchronise, since the
    // macro's bytes are then executed in their own right.
    break;
  }
  return operands;
}

void NaplpsInterpreter::execute_pdi(uint8_t opcode) {
  std::vector<uint8_t> operands = gather_operands();
  pace(naplps_pace_pdi_us(pace_, opcode));
  const NaplpsPdi pdi = static_cast<NaplpsPdi>(opcode);
  const NaplpsOperandFormat format = state_.domain.format;

  // The page record ended inside this command, rather than a code ending it.
  // Both receivers of the day keep a drawing command open until the code after
  // it arrives, so the words that arrived whole are drawn and a partial last
  // one is not — and a figure only its last word closes (a polygon, an arc, an
  // incremental polygon) is not drawn at all. A short word a code ends is still
  // zero-filled (§5.3.2.2.5): they do that too.
  if (frames_.size() == 1 && frames_.back().position >= frames_.back().length &&
      !operands.empty()) {
    if (naplps_pdi_closes_on_its_last_word(pdi)) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
    if (naplps_pdi_takes_coordinate_words(pdi)) {
      const size_t whole =
          operands.size() - operands.size() % format.multi_value_bytes;
      if (whole == 0) {
        ++snapshot_.diagnostics.truncated_pdis;
        return;
      }
      operands.resize(whole);
    }
  }

  NaplpsOperandReader reader(operands.data(), operands.size(), format);

  switch (pdi) {
    case NaplpsPdi::kReset:
      pdi_reset(reader);
      return;
    case NaplpsPdi::kDomain:
      pdi_domain(reader);
      return;
    case NaplpsPdi::kText:
      pdi_text(reader);
      return;
    case NaplpsPdi::kTexture:
      pdi_texture(reader);
      return;
    case NaplpsPdi::kSetColour:
      pdi_set_colour(reader);
      return;
    case NaplpsPdi::kSelectColour:
      pdi_select_colour(reader, format.single_value_bytes);
      return;
    case NaplpsPdi::kBlink:
      pdi_blink(reader, format.single_value_bytes);
      return;
    case NaplpsPdi::kWait:
      pdi_wait(reader);
      return;

    case NaplpsPdi::kPointSetAbs:
    case NaplpsPdi::kPointSetRel:
    case NaplpsPdi::kPointAbs:
    case NaplpsPdi::kPointRel:
      pdi_point(pdi, reader);
      return;

    case NaplpsPdi::kLineAbs:
    case NaplpsPdi::kLineRel:
    case NaplpsPdi::kSetLineAbs:
    case NaplpsPdi::kSetLineRel:
      pdi_line(pdi, reader);
      return;

    case NaplpsPdi::kArcOutlined:
    case NaplpsPdi::kArcFilled:
    case NaplpsPdi::kSetArcOutlined:
    case NaplpsPdi::kSetArcFilled:
      pdi_arc(pdi, reader);
      return;

    case NaplpsPdi::kRectOutlined:
    case NaplpsPdi::kRectFilled:
    case NaplpsPdi::kSetRectOutlined:
    case NaplpsPdi::kSetRectFilled:
      pdi_rect(pdi, reader);
      return;

    case NaplpsPdi::kPolyOutlined:
    case NaplpsPdi::kPolyFilled:
    case NaplpsPdi::kSetPolyOutlined:
    case NaplpsPdi::kSetPolyFilled:
      pdi_poly(pdi, reader);
      return;

    case NaplpsPdi::kField:
      pdi_field(reader);
      return;

    case NaplpsPdi::kIncrPoint:
    case NaplpsPdi::kIncrLine:
    case NaplpsPdi::kIncrPolyFilled:
      pdi_incremental(pdi, reader, operands);
      return;
  }
}

void NaplpsInterpreter::pdi_reset(NaplpsOperandReader& reader) {
  // §5.3.2.9.1: a two-byte fixed operand, executed b1 to b6 of byte 1 then b1
  // to b6 of byte 2. §5.3.2.9.3: no operands is every bit zero, i.e. no action.
  const uint8_t byte1 = reader.empty() ? 0 : reader.read_fixed_byte();
  const uint8_t byte2 = reader.empty() ? 0 : reader.read_fixed_byte();

  const auto bit = [](uint8_t value, int index) {
    // §5.3.2.9's bits are b1 to b6, and the numeric byte's payload is b6 down
    // to b1 in bits 5 down to 0, so b<n> is bit n-1.
    return ((value >> (index - 1)) & 0x1u) != 0;
  };

  // Byte 1, b1: DOMAIN.
  if (bit(byte1, 1)) {
    state_.domain.reset();
  }

  // Byte 1, b3 b2: Table 14's colour-mode reset.
  const int colour_action = (bit(byte1, 3) ? 2 : 0) | (bit(byte1, 2) ? 1 : 0);
  switch (colour_action) {
    case 0:
      break;  // No action.
    case 1:
      state_.colour.select_direct_mode();
      state_.colour.reset_map();
      state_.colour.set_colour(kNabtsNominalWhite);
      break;
    case 2:
      // "Select color mode 1 and set color map to default colors. If this is
      // executed while in color mode 0, then it has the same effect as 11."
      state_.colour.reset_to_mapped_mode(state_.colour.mode() ==
                                         NabtsColourMode::kDirect);
      break;
    default:
      // "Select color mode 1, set color map to default colors, and set the
      // in-use drawing color to white" — white from the default map, not
      // written over its entry 0 (see reset_to_mapped_mode()).
      state_.colour.reset_to_mapped_mode(true);
      break;
  }

  // Byte 1, b6 b5 b4: Table 15's screen and border clear. Only the display-area
  // cases mean anything to a display list; the border has nowhere to appear.
  // Actions 1 and 7 clear the display to nominal black, 2, 5 and 6 to the
  // current drawing colour — recorded so a page that opens on a coloured
  // ground shows it.
  const int screen_action = (bit(byte1, 6) ? 4 : 0) | (bit(byte1, 5) ? 2 : 0) |
                            (bit(byte1, 4) ? 1 : 0);
  if (screen_action == 1 || screen_action == 7) {
    clear_display(state_.colour.nominal_black(),
                  state_.colour.nominal_black_address());
  } else if (screen_action == 2 || screen_action == 5 || screen_action == 6) {
    clear_display(state_.colour.drawing_colour(),
                  state_.colour.drawing_map_address());
  }

  // Byte 2.
  if (bit(byte2, 1)) {
    // §5.3.2.9.3: home the cursor and reset every text parameter.
    state_.text.reset();
    state_.reset_active_field();
    move_cursor(state_.home_position());
  }
  if (bit(byte2, 2)) {
    // §5.3.2.9.3: "all blink processes are terminated" — every one, not just
    // the drawing colour's.
    end_blinks(BlinkSelection::kAll);
  }
  // b3 protects unprotected fields, which Table D1 item 12(1) makes a no-op for
  // the teletext service.
  if (bit(byte2, 4)) {
    // "all texture attributes are set to their default values. The four
    // programmable texture masks are not cleared."
    state_.texture.reset();
  }
  if (bit(byte2, 5)) {
    state_.clear_macros();
  }
  if (bit(byte2, 6)) {
    state_.clear_drcs();
  }
}

void NaplpsInterpreter::pdi_domain(NaplpsOperandReader& reader) {
  if (reader.empty()) {
    return;
  }
  const uint8_t byte1 = reader.read_fixed_byte();

  // §5.3.2.2.2 Table 4: b2 b1 give the single-value length, 1 to 4 bytes.
  state_.domain.format.single_value_bytes =
      static_cast<size_t>(byte1 & 0x03) + 1;
  // §5.3.2.2.3 Table 5: b5 b4 b3 give the multi-value length, 1 to 8 bytes.
  state_.domain.format.multi_value_bytes =
      static_cast<size_t>((byte1 >> 2) & 0x07) + 1;
  // §5.3.2.2.4: b6 is the dimensionality.
  state_.domain.format.three_dimensional = ((byte1 >> 5) & 0x01) != 0;

  // A DOMAIN declaring one- or two-byte multi-value operands whose own logical
  // pel arrives in three bytes was written by a sender still on three, and
  // three is what its coordinates go on being sent in. Services do this: game
  // pages send DOMAIN 4/0 4/0 4/0 5/11 — by Table 5 a one-byte length — before
  // three-byte coordinates that land exactly where they belong, Go's star
  // points on the 4-4 points of its board and a golf game's text field in its
  // score box, and other pages send DOMAIN 4/4 with a three-byte pel before
  // three-byte polygons; read at the declared length, every coordinate after
  // it is noise. The receivers they were written for read three whatever
  // byte 1 said. A sender that means a shorter length sends its pel
  // in it (§5.3.2.2.6), so no DOMAIN that follows Table 5 is read any other
  // way — and the repetition §5.3.2.2.5 would otherwise make of the surplus
  // bytes, a second DOMAIN taking a coordinate byte as its byte 1, is no
  // command a sender writes.
  if (state_.domain.format.multi_value_bytes < kNaplpsMultiValueDefaultBytes &&
      reader.remaining() == kNaplpsMultiValueDefaultBytes) {
    state_.domain.format.multi_value_bytes = kNaplpsMultiValueDefaultBytes;
  }

  // §5.3.2.2.6: "Note that the new length of the multi-value operands, as set
  // in byte 1, applies to the multi-value logical pel size operand of that
  // DOMAIN command." So the reader has to be rebuilt on the new format.
  if (reader.empty()) {
    // "If the logical pel size operand is omitted, the size of the logical pel
    // shall not be changed."
    return;
  }
  reader.set_format(state_.domain.format);
  const NabtsPoint pel = reader.read_coordinate();
  state_.domain.logical_pel = NabtsSize{pel.x, pel.y};
}

void NaplpsInterpreter::pdi_text(NaplpsOperandReader& reader) {
  // §5.3.2.3.1: a two-byte fixed operand, then a multi-value character field.
  if (reader.empty()) {
    return;
  }
  const uint8_t byte1 = reader.read_fixed_byte();
  state_.text.rotation = static_cast<NabtsCharRotation>(byte1 & 0x03);
  state_.text.path = static_cast<NabtsCharPath>((byte1 >> 2) & 0x03);
  state_.text.intercharacter_spacing =
      static_cast<uint8_t>((byte1 >> 4) & 0x03);

  if (!reader.empty()) {
    const uint8_t byte2 = reader.read_fixed_byte();
    state_.text.interrow_spacing = static_cast<uint8_t>(byte2 & 0x03);
    state_.text.move_attribute = static_cast<uint8_t>((byte2 >> 2) & 0x03);
    state_.text.cursor_style =
        static_cast<NabtsCursorStyle>((byte2 >> 4) & 0x03);
  }

  // §5.3.2.3.9: "If the character field dimensions are omitted from the
  // operand, then the current character field dimensions remain unchanged."
  // A field with no width or no height — which is what a field word cut short
  // comes to once §5.3.2.2.5 has supplied its trailing zero bits — would draw
  // nothing, and both receivers of the day keep the field they had instead.
  if (!reader.empty()) {
    const NabtsPoint field = reader.read_coordinate();
    if (field.x != 0.0 && field.y != 0.0) {
      state_.text.character_field = NabtsSize{field.x, field.y};
    }
  }

  // §5.3.2.3.7: "The execution of a TEXT command shall have no effect on the
  // position of the character field origin, except if it would cause any part
  // of the character field to fall outside the unit screen", when it moves as
  // little as it must; and "shall cause alignment of the drawing point if the
  // "move together" or "cursor leads" move attribute is in effect after
  // execution".
  state_.cursor = fit_cursor(state_.cursor);
  if (state_.text.move_attribute == 0 || state_.text.move_attribute == 1) {
    const NabtsPoint alignment = cursor_alignment();
    state_.drawing_point = NabtsPoint{state_.cursor.x + alignment.x,
                                      state_.cursor.y + alignment.y};
  }
}

void NaplpsInterpreter::pdi_texture(NaplpsOperandReader& reader) {
  if (reader.empty()) {
    return;
  }
  const uint8_t byte1 = reader.read_fixed_byte();
  // §5.3.2.4.2: b2 b1 line texture; §5.3.2.4.3: b3 highlight; §5.3.2.4.4:
  // b6 b5 b4 texture pattern.
  state_.texture.line_texture = static_cast<NabtsLineTexture>(byte1 & 0x03);
  state_.texture.highlight = ((byte1 >> 2) & 0x01) != 0;
  state_.texture.pattern =
      static_cast<NabtsTexturePattern>((byte1 >> 3) & 0x07);

  // §5.3.2.4.5: "If the mask size operand is not present within the TEXTURE
  // PDI, then the current mask size is not changed."
  if (!reader.empty()) {
    const NabtsPoint mask = reader.read_coordinate();
    state_.texture.mask_size = NabtsSize{mask.x, mask.y};
  }
}

void NaplpsInterpreter::pdi_set_colour(NaplpsOperandReader& reader) {
  // §5.3.2.5.1: "If no operand follows a SET COLOR opcode, the transparent
  // color is set."
  if (reader.empty()) {
    state_.colour.set_transparent();
    return;
  }

  // Each colour word sets a colour; §5.3.2.5.1 has additional data repeat the
  // opcode "with the map address incremented prior to the execution of the new
  // opcode", which is how a service loads a whole palette in one command. "This
  // incrementing does not affect the color map address associated with the
  // drawing color", so the walk is held locally rather than in the state, and
  // "subsequent operand data are ignored when the physical limit (all ones) of
  // the implemented color map is reached".
  bool first = true;
  uint32_t repeat_address = 0;
  while (!reader.empty()) {
    const NabtsColour colour = reader.read_colour();
    if (first) {
      state_.colour.set_colour(colour);
      repeat_address = state_.colour.drawing_address();
      first = false;
      continue;
    }
    if (state_.colour.mode() == NabtsColourMode::kDirect) {
      // In mode 0 a repeat is simply SET COLOR again: the last word is the
      // drawing colour, each finding or claiming its map entry as §5.3.2.5.1
      // has it.
      state_.colour.set_colour(colour);
      continue;
    }
    if (!NaplpsColourState::increment_map_address(repeat_address)) {
      break;
    }
    state_.colour.write_map_entry(repeat_address, colour);
  }
}

void NaplpsInterpreter::pdi_select_colour(NaplpsOperandReader& reader,
                                          size_t operand_bytes) {
  // §5.3.2.6: zero operands selects mode 0, one selects mode 1, two select
  // mode 2. Additional data is "reserved for future standardization and shall
  // be ignored". A short final word still counts as an operand: §5.3.2.2.5
  // completes it with trailing zero bits.
  const size_t words = (reader.remaining() + operand_bytes - 1) / operand_bytes;
  if (words == 0) {
    state_.colour.select_direct_mode();
    return;
  }
  const size_t operand_bits = operand_bytes * kNaplpsNumericBits;
  const uint32_t first =
      NaplpsColourState::map_address(reader.read_single_value(), operand_bits);
  if (words == 1) {
    state_.colour.select_mapped_mode(first);
    return;
  }
  const uint32_t second =
      NaplpsColourState::map_address(reader.read_single_value(), operand_bits);
  state_.colour.select_mapped_background_mode(first, second);
}

void NaplpsInterpreter::pdi_blink(NaplpsOperandReader& reader,
                                  size_t operand_bytes) {
  // §5.3.2.7.4: "If no operands follow the blink opcode, then all blink
  // processes utilizing the current drawing color as the blink-from color will
  // be terminated."
  if (reader.empty()) {
    end_blinks(BlinkSelection::kFrom, state_.colour.drawing_address());
    return;
  }

  // §5.3.2.7.2: the blink-from colour is the one in use for drawing, and the
  // process runs on that colour map entry. §5.3.2.7.5 then allows the command
  // to be implicitly repeated for as long as operands keep arriving, "with the
  // address of the blink-from color being automatically incremented (as in
  // 5.3.2.5.1) ... The drawing color is not affected by this incrementing."
  uint32_t blink_from = state_.colour.drawing_address();
  while (true) {
    // §5.3.2.7.3: a blink-to map address, then the ON and OFF intervals and
    // the start delay, "in units of 1/10 of a second" with "only bits b6
    // through b1" used. An omitted start delay is zero.
    const uint32_t to_address = NaplpsColourState::map_address(
        reader.read_single_value(), operand_bytes * kNaplpsNumericBits);
    const uint8_t on_interval = reader.empty() ? 0 : reader.read_fixed_byte();
    const uint8_t off_interval = reader.empty() ? 0 : reader.read_fixed_byte();
    const uint8_t start_delay = reader.empty() ? 0 : reader.read_fixed_byte();
    const auto to = static_cast<int16_t>(to_address % kNabtsColourMapEntries);
    if (on_interval == 0 || off_interval == 0) {
      // "An ON or OFF interval of 0 is taken to mean termination of any
      // active blink process on the blink-from/blink-to color pair."
      end_blinks(BlinkSelection::kPair, blink_from, to);
    } else {
      define_blink(blink_from, to, NabtsColour{},
                   static_cast<uint32_t>(on_interval * kTenthMs),
                   static_cast<uint32_t>(off_interval * kTenthMs),
                   static_cast<uint32_t>(start_delay * kTenthMs));
    }

    if (reader.empty() ||
        !NaplpsColourState::increment_map_address(blink_from)) {
      return;
    }
  }
}

void NaplpsInterpreter::pdi_wait(NaplpsOperandReader& reader) {
  // §5.3.2.8.2: "The first byte of operand data following the WAIT opcode
  // shall follow the format given in Figure 28. If any other bit combination
  // follows the WAIT opcode, the entire command is reserved for future
  // standardization and shall be executed as a null operation." Both
  // receivers of the day do exactly that, a format byte the tape damaged
  // included; putting such a byte back from another recording is the
  // reconstruction's business, not the grammar's.
  if (reader.empty() || reader.read_fixed_byte() != kNaplpsWaitFormat) {
    return;
  }
  // Whatever the record changed in the colour map before the pause happened
  // before it.
  flush_map_writes();
  // "The next byte of operand data gives the time delay in units of 1/10 of a
  // second ... If any additional data bytes follow, they are treated as
  // additional periods of waiting time".
  while (!reader.empty()) {
    const uint8_t tenths = reader.read_fixed_byte();
    clock_ms_ += tenths == 0 ? kMinimumWaitMs : tenths * kTenthMs;
  }
  clock_ms_ = std::min(clock_ms_, kMaxPresentationMs);
}

// ---------------------------------------------------------------------------
// Geometric primitives
// ---------------------------------------------------------------------------

namespace {

/// |base| displaced by |delta|: where a relative coordinate leads (§5.3.1).
NabtsPoint displaced(const NabtsPoint& base, const NabtsPoint& delta) {
  return NabtsPoint{base.x + delta.x, base.y + delta.y};
}

}  // namespace

bool NaplpsInterpreter::reject_off_screen(const NabtsPrimitive& figure) {
  // §5.3.1: "If a coordinate specification or a drawing operation would cause
  // the drawing point or any portion of the resulting drawing to be outside the
  // unit screen, then the PDI is considered to be in error. The handling of
  // this error condition is implementation-dependent. For example, this PDI
  // may either be rejected (ie, executed as a null operation) or executed and
  // clipped within the unit screen." A point, line or rectangle keeps the
  // points the record gave it and is executed and clipped: the renderer clips
  // what is drawn to the screen, as both receivers of the day do. Moving a
  // point onto the screen's edge instead would move the geometry — a rectangle
  // starting left of x = 0 collapsing to a line down the edge — rather than
  // clip it.
  //
  // For polygons, arcs and splines this takes the other way: the figure is not
  // drawn, though the drawing
  // point moves as the command would have moved it. That is what the Panasonic
  // TU-2000x does — so what its viewers saw — and the X3.110 note on arcs has
  // it that "the arc may not be specified so that any portion of it lies
  // outside the unit screen", which a service composing to the standard never
  // does (§8.2.1 item 4). On the recordings a vertex off the screen is nearly
  // always an operand read across a lost stretch, or damaged: the bytes after
  // the hole belong to commands whose opcodes were lost, and a figure that
  // takes them in as vertices runs off the screen and, drawn and clipped,
  // floods the page with a shape its author never drew. Points, lines and
  // rectangles are clipped instead, as both receivers of the day clip them.
  if (within_unit_screen(figure.points)) {
    return false;
  }
  ++snapshot_.diagnostics.rejected_figures;
  return true;
}

void NaplpsInterpreter::pdi_point(NaplpsPdi opcode,
                                  NaplpsOperandReader& reader) {
  const bool relative =
      opcode == NaplpsPdi::kPointSetRel || opcode == NaplpsPdi::kPointRel;
  const bool visible =
      opcode == NaplpsPdi::kPointAbs || opcode == NaplpsPdi::kPointRel;

  const auto execute = [&](const NabtsPoint& word) {
    const NabtsPoint target =
        relative ? displaced(state_.drawing_point, word) : word;
    move_drawing_point(target);
    if (visible) {
      NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kPoint);
      primitive.origin = target;
      primitive.points.push_back(target);
      emit(std::move(primitive));
    }
  };

  // §5.3.2.2.5 supplies trailing zero bits to an operand shorter than DOMAIN
  // declares, and one that carries no data at all is the shortest there is:
  // the origin for the absolute forms, no displacement for the relative ones.
  // An encoder can open and close every PDI section with an empty POINT SET
  // (Absolute) and draw on from the origin it leaves; the Norpak TTX 650 and
  // Panasonic TU-2000x firmware both read all four forms so.
  if (reader.empty()) {
    execute(NabtsPoint{0.0, 0.0});
    return;
  }

  // §5.3.2.2.5 has a longer operand repeat the opcode, which for POINT means a
  // run of points.
  while (!reader.empty()) {
    execute(reader.read_coordinate());
    if (reader.truncated()) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
  }
}

void NaplpsInterpreter::pdi_line(NaplpsPdi opcode,
                                 NaplpsOperandReader& reader) {
  const bool relative_end =
      opcode == NaplpsPdi::kLineRel || opcode == NaplpsPdi::kSetLineRel;
  const bool has_start =
      opcode == NaplpsPdi::kSetLineAbs || opcode == NaplpsPdi::kSetLineRel;

  // LINE (Absolute) with no data at all, zero-filled as POINT's is
  // (§5.3.2.2.5): a line from the drawing point to the origin, which both
  // receivers of the day draw. The other forms stay null operations — the
  // TU-2000x draws nothing for them and the TTX 650 a single pel, and a
  // relative end or a SET start of zero is nothing a service would mean.
  if (reader.empty() && opcode == NaplpsPdi::kLineAbs) {
    const NabtsPoint start = state_.drawing_point;
    const NabtsPoint end = NabtsPoint{0.0, 0.0};
    NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kLine);
    primitive.origin = start;
    primitive.points.push_back(start);
    primitive.points.push_back(end);
    emit(std::move(primitive));
    move_drawing_point(end);
    return;
  }

  while (!reader.empty()) {
    NabtsPoint start = state_.drawing_point;
    if (has_start) {
      // §5.3.3.2.4: the start point is absolute in both SET forms.
      start = reader.read_coordinate();
    }
    // A start with no end point after it has the end zero-filled — "for all
    // PDIs, when not otherwise stated, non-existent operands are assumed to be
    // zero" (the 1993 Addendum to T.101 Annex D, reading §5.3.2.2.5): the
    // origin for the absolute form, no displacement for the relative one. Both
    // receivers of the day draw SET & LINE (ABSOLUTE) with its start alone as a
    // line from it to the origin.
    const NabtsPoint word =
        reader.empty() ? NabtsPoint{0.0, 0.0} : reader.read_coordinate();
    const NabtsPoint end = relative_end ? displaced(start, word) : word;

    NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kLine);
    primitive.origin = start;
    primitive.points.push_back(start);
    primitive.points.push_back(end);
    emit(std::move(primitive));

    // §5.3.3.2.1: "At the completion of drawing a line, the drawing point is
    // coincident with the end point."
    move_drawing_point(end);
    if (reader.truncated()) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
  }
}

void NaplpsInterpreter::pdi_arc(NaplpsPdi opcode, NaplpsOperandReader& reader) {
  const bool filled =
      opcode == NaplpsPdi::kArcFilled || opcode == NaplpsPdi::kSetArcFilled;
  const bool has_start = opcode == NaplpsPdi::kSetArcOutlined ||
                         opcode == NaplpsPdi::kSetArcFilled;

  NabtsPoint start = state_.drawing_point;
  if (has_start) {
    if (reader.empty()) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
    start = reader.read_coordinate();
  }
  if (reader.empty()) {
    ++snapshot_.diagnostics.truncated_pdis;
    return;
  }

  // §5.3.3.3.2: the intermediate point is relative to the start, and the end
  // point relative to the intermediate. §5.3.3.3.1 adds that "If more points
  // are given, they define a higher level arc, a curvilinear line defined by a
  // spline function", so the run is collected as a control-point list and the
  // renderer decides between circle and spline by its length.
  NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kArc);
  primitive.filled = filled;
  primitive.origin = start;
  primitive.points.push_back(start);

  NabtsPoint previous = start;
  while (!reader.empty() && primitive.points.size() < kNaplpsMaxVertices) {
    const NabtsPoint word = reader.read_coordinate();
    previous = displaced(previous, word);
    primitive.points.push_back(previous);
    if (reader.truncated()) {
      ++snapshot_.diagnostics.truncated_pdis;
      break;
    }
  }

  if (primitive.points.size() < 2) {
    // An arc needs a start and at least the intermediate point (§5.3.3.3.1).
    ++snapshot_.diagnostics.truncated_pdis;
    return;
  }
  if (primitive.points.size() == 2) {
    // §5.3.3.3.1: "If the end point is omitted, it is taken to be coincident
    // with the start point and a circle is drawn." A record drawing a circle
    // saves a whole coordinate block this way, so it is a normal encoding
    // rather than a truncation.
    primitive.points.push_back(start);
  }
  const NabtsPoint end = primitive.points.back();
  if (!reject_off_screen(primitive)) {
    emit(std::move(primitive));
  }
  move_drawing_point(end);
}

void NaplpsInterpreter::pdi_rect(NaplpsPdi opcode,
                                 NaplpsOperandReader& reader) {
  const bool filled =
      opcode == NaplpsPdi::kRectFilled || opcode == NaplpsPdi::kSetRectFilled;
  const bool has_start = opcode == NaplpsPdi::kSetRectOutlined ||
                         opcode == NaplpsPdi::kSetRectFilled;

  while (!reader.empty()) {
    NabtsPoint start = state_.drawing_point;
    if (has_start) {
      start = reader.read_coordinate();
      if (reader.empty()) {
        ++snapshot_.diagnostics.truncated_pdis;
        return;
      }
    }
    // §5.3.3.4.2: the width and height are a coordinate word, and may be
    // negative — which is what puts the origin in any of the four corners.
    const NabtsPoint extent = reader.read_coordinate();

    // The far corner may leave the unit screen: the rectangle keeps its whole
    // extent and is clipped where it is drawn. Real service records do this:
    // rectangles whose corner lands at x = 1 and just past it.
    const NabtsPoint corner{start.x + extent.x, start.y + extent.y};

    NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kRectangle);
    primitive.filled = filled;
    primitive.origin = start;
    primitive.size = NabtsSize{corner.x - start.x, corner.y - start.y};
    primitive.points.push_back(start);
    primitive.points.push_back(corner);
    emit(std::move(primitive));

    // §5.3.3.4.1: "At the completion of drawing a rectangle, the drawing point
    // is the start point altered in x only, by the amount of the dx
    // displacement."
    move_drawing_point(NabtsPoint{start.x + extent.x, start.y});
    if (reader.truncated()) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
  }
}

void NaplpsInterpreter::pdi_poly(NaplpsPdi opcode,
                                 NaplpsOperandReader& reader) {
  const bool filled =
      opcode == NaplpsPdi::kPolyFilled || opcode == NaplpsPdi::kSetPolyFilled;
  const bool has_start = opcode == NaplpsPdi::kSetPolyOutlined ||
                         opcode == NaplpsPdi::kSetPolyFilled;

  NabtsPoint start = state_.drawing_point;
  if (has_start) {
    if (reader.empty()) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
    start = reader.read_coordinate();
  }

  NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kPolygon);
  primitive.filled = filled;
  primitive.origin = start;
  primitive.points.push_back(start);

  // §5.3.3.5.1: "Each (dx, dy) coordinate pair represents a relative
  // displacement from the last vertex (a relative displacement of magnitude 0
  // is ignored). There is implicit closure between the start point and the last
  // vertex".
  NabtsPoint previous = start;
  while (!reader.empty() && primitive.points.size() < kNaplpsMaxVertices) {
    const NabtsPoint word = reader.read_coordinate();
    if (word.x == 0.0 && word.y == 0.0) {
      continue;
    }
    previous = displaced(previous, word);
    primitive.points.push_back(previous);
    if (reader.truncated()) {
      ++snapshot_.diagnostics.truncated_pdis;
      break;
    }
  }

  // A polygon of one or two points encloses nothing, but it is still drawn:
  // §5.3.2.2.6 has the logical pel "always map to at least one and possibly
  // many display pixels", so the outline the pel traces is there even where
  // the area is not — a start point alone is one pel, two points a line.
  if (!reject_off_screen(primitive)) {
    emit(std::move(primitive));
  }
  // "at the completion of drawing a polygon, the drawing point is coincident
  // with the start point."
  move_drawing_point(start);
}

void NaplpsInterpreter::pdi_field(NaplpsOperandReader& reader) {
  // §5.3.3.6.2: no operands sets the field to the full unit screen with origin
  // (0,0); one operand gives the dimensions with the current drawing point as
  // origin; two give the origin and then the dimensions.
  if (reader.empty()) {
    state_.reset_active_field();
    move_drawing_point(state_.field_origin);
    return;
  }

  const NabtsPoint first = reader.read_coordinate();
  if (reader.empty()) {
    state_.field_origin = state_.drawing_point;
    state_.field_size = NabtsSize{first.x, first.y};
    return;
  }
  const NabtsPoint extent = reader.read_coordinate();
  state_.field_origin = first;
  state_.field_size = NabtsSize{extent.x, extent.y};
  // "The drawing point is set to the origin of the field after FIELD has been
  // executed."
  move_drawing_point(state_.field_origin);
}

void NaplpsInterpreter::incremental_point(
    const std::vector<uint8_t>& operands) {
  // §5.3.3.6.3: "The INCREMENTAL POINT command takes two operands. The first is
  // a single byte, fixed format operand that describes the packing counter ...
  // The range of values of the packing counter shall be 1 to 48, inclusive.
  // Values of 0 or greater than 48 are reserved for future standardization and
  // the entire INCREMENTAL POINT command containing packing counters with such
  // values shall be executed as a null operation."
  if (operands.empty()) {
    ++snapshot_.diagnostics.truncated_pdis;
    return;
  }
  const size_t packing = operands.front() & kNaplpsNumericMask;
  if (packing == 0 || packing > kNaplpsMaxPackingCount) {
    return;
  }

  // "If INCREMENTAL POINT is received and either or both dimensions of the
  // logical pel are equal to 0, then these dimensions are set, for the
  // duration of the command only, to the smallest positive value specifiable
  // within the current domain" — one step of a multi-value coordinate, which
  // spends one bit of each axis on its sign.
  constexpr auto kPayloadBits = static_cast<size_t>(kNaplpsNumericBits);
  const NaplpsOperandFormat& format = state_.domain.format;
  const size_t axis_bits = format.multi_value_bytes *
                           (kPayloadBits / format.coordinate_components());
  const double smallest = std::ldexp(1.0, 1 - static_cast<int>(axis_bits));
  NabtsSize pel = state_.domain.logical_pel;
  if (pel.dx == 0.0) {
    pel.dx = smallest;
  }
  if (pel.dy == 0.0) {
    pel.dy = smallest;
  }

  // The active field, whichever corner its origin is (§5.3.3.6.2).
  const NabtsPoint field_origin = state_.field_origin;
  const NabtsSize field_size = state_.field_size;
  const double left = std::min(field_origin.x, field_origin.x + field_size.dx);
  const double right = std::max(field_origin.x, field_origin.x + field_size.dx);
  const double bottom =
      std::min(field_origin.y, field_origin.y + field_size.dy);
  const double top = std::max(field_origin.y, field_origin.y + field_size.dy);
  // Whether the pel with its drawing point at (x, y) lies wholly in the field.
  // The slack absorbs the arithmetic of stepping, not a whole pel.
  constexpr double kSlack = 1e-9;
  const auto fits = [&](double x, double y) {
    return std::min(x, x + pel.dx) >= left - kSlack &&
           std::max(x, x + pel.dx) <= right + kSlack &&
           std::min(y, y + pel.dy) >= bottom - kSlack &&
           std::max(y, y + pel.dy) <= top + kSlack;
  };

  // "If INCREMENTAL POINT is received and if any portion of the logical pel
  // indicated by the initial drawing point lies outside the active field, the
  // entire command is considered to be in error and is executed as a null
  // operation."
  const NabtsPoint start = state_.drawing_point;
  if (!fits(start.x, start.y)) {
    return;
  }

  // The colour specifications, "stored sequentially without regard to byte
  // boundaries, high order bit (b6) to low order bit (b1)".
  const size_t string_bytes = operands.size() - 1;
  size_t byte = 0;
  size_t bit = 0;  // bits of |byte| already used
  const auto available = [&] {
    return (string_bytes - byte) * kPayloadBits - bit;
  };
  const auto read_bits = [&](size_t count) {
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) {
      const uint8_t payload = operands[1 + byte] & kNaplpsNumericMask;
      value = (value << 1) | ((payload >> (kPayloadBits - 1 - bit)) & 0x1u);
      if (++bit == kPayloadBits) {
        bit = 0;
        ++byte;
      }
    }
    return value;
  };

  const bool direct = state_.colour.mode() == NabtsColourMode::kDirect;
  NabtsPrimitive row;
  const auto begin_row = [&](const NabtsPoint& at) {
    row = make_primitive(NabtsPrimitiveKind::kIncrementalPoints);
    row.origin = at;
    row.logical_pel = pel;
    row.points = {field_origin};
    row.size = field_size;
  };
  const auto end_row = [&] {
    if (!row.incremental_colours.empty()) {
      emit(std::move(row));
    }
    row = NabtsPrimitive{};
  };

  begin_row(start);
  double row_x = start.x;
  double y = start.y;
  size_t column = 0;
  while (true) {
    const double x = row_x + static_cast<double>(column) * pel.dx;
    if (fits(x, y)) {
      // (2): the next colour, deposited under the pel; then the drawing point
      // moves a pel along x.
      if (available() < packing) {
        break;  // the string is spent
      }
      const uint64_t specification = read_bits(packing);
      if (direct) {
        // "In color mode 0, these will be interpreted as actual color values",
        // three-tuples G R B as in a colour word.
        row.incremental_colours.push_back(
            naplps_colour_from_bits(specification, packing));
        row.incremental_addresses.push_back(-1);
      } else {
        // In modes 1 and 2 "ordinal numbers, ie, addresses in the color map",
        // left-justified in their bits as every map address is (§5.3.2.6.1).
        const size_t entry =
            NaplpsColourState::map_address(specification, packing) %
            kNabtsColourMapEntries;
        row.incremental_colours.push_back(state_.colour.map()[entry]);
        row.incremental_addresses.push_back(static_cast<int16_t>(entry));
      }
      ++column;
      continue;
    }

    // (3): the pel has left the field. "Any remaining bits in the byte of the
    // string operand currently being interpreted are discarded ...
    // Interpretation resumes at the first bit (ie, b6) in the next complete
    // byte. If there are no further numeric data bytes, then the operation is
    // terminated; otherwise the drawing point is repositioned to the opposite
    // boundary."
    end_row();
    if (bit != 0) {
      bit = 0;
      ++byte;
    }
    if (byte >= string_bytes) {
      break;
    }
    row_x = pel.dx > 0.0 ? left : right;
    column = 0;
    if (fits(row_x, y + pel.dy)) {
      y += pel.dy;
    } else {
      // "the Y value is left constant and the entire display image lying
      // within the area of the screen defined by the active field is scrolled
      // in the opposite direction, ie, a distance equivalent to -dy".
      NabtsPrimitive scroll = make_primitive(NabtsPrimitiveKind::kScroll);
      scroll.origin = field_origin;
      scroll.size = field_size;
      scroll.points = {NabtsPoint{0.0, -pel.dy}};
      scroll.logical_pel = pel;
      emit(std::move(scroll));
    }
    if (!fits(row_x, y)) {
      break;  // a field narrower than the pel has no row to resume on
    }
    begin_row(NabtsPoint{row_x, y});
  }
  end_row();

  // "If the operation has terminated, the drawing point is then set to the
  // origin of the active field."
  move_drawing_point(field_origin);
}

void NaplpsInterpreter::pdi_incremental(NaplpsPdi opcode,
                                        NaplpsOperandReader& reader,
                                        const std::vector<uint8_t>& operands) {
  if (opcode == NaplpsPdi::kIncrPoint) {
    incremental_point(operands);
    return;
  }

  // §5.3.3.6.4 and §5.3.3.6.5: a multi-value operand giving the step size, then
  // a string of move nibbles, three to a byte (Tables 16 and 17).
  if (reader.empty()) {
    ++snapshot_.diagnostics.truncated_pdis;
    return;
  }
  const NabtsPoint step = reader.read_coordinate();
  std::vector<uint8_t> moves;
  moves.reserve(reader.remaining());
  while (!reader.empty()) {
    moves.push_back(reader.read_fixed_byte());
  }
  const bool polygon = opcode == NaplpsPdi::kIncrPolyFilled;
  const NaplpsIncrementalPath path =
      naplps_incremental_path(state_.drawing_point, step, moves, polygon);

  if (polygon) {
    // §5.3.3.6.5: "The final drawing point is implicitly taken as the initial
    // drawing point", and the area the outline encloses is filled.
    if (path.points.size() >= 2) {
      NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kPolygon);
      primitive.filled = true;
      // The vertex limit is "the same as for the POLYGON command, and is 256"
      // (T.101 Annex D Addendum, clause 8; Table D1 item 4), and the Norpak
      // TTX 650 keeps the first 256 as POLYGON does here.
      const size_t vertices = std::min(path.points.size(), kNaplpsMaxVertices);
      for (size_t i = 0; i < vertices; ++i) {
        primitive.points.push_back(path.points[i]);
      }
      primitive.origin = primitive.points.front();
      if (!reject_off_screen(primitive)) {
        emit(std::move(primitive));
      }
    }
    move_drawing_point(path.points.front());
    return;
  }

  // §5.3.3.6.4: each step with the draw flag on joins the point before it to
  // the point after it, so every run of drawn steps is one line through all of
  // its vertices — each of which §5.3.2.4.2 has plotted whatever the texture.
  NabtsPrimitive line;
  const auto finish_line = [&] {
    if (!line.points.empty()) {
      emit(std::move(line));
      line = NabtsPrimitive{};
    }
  };
  for (size_t i = 0; i + 1 < path.points.size(); ++i) {
    if (!path.drawn[i]) {
      finish_line();
      continue;
    }
    if (line.points.empty()) {
      line = make_primitive(NabtsPrimitiveKind::kLine);
      line.points.push_back(path.points[i]);
      line.origin = line.points.front();
    }
    line.points.push_back(path.points[i + 1]);
  }
  finish_line();
  move_drawing_point(path.points.back());
}

// ---------------------------------------------------------------------------
// Emission and movement
// ---------------------------------------------------------------------------

NabtsPrimitive NaplpsInterpreter::make_primitive(
    NabtsPrimitiveKind kind) const {
  NabtsPrimitive primitive;
  primitive.kind = kind;
  primitive.logical_pel = state_.domain.logical_pel;
  primitive.line_texture = state_.texture.line_texture;
  primitive.texture_pattern = state_.texture.pattern;
  primitive.texture_mask_size = state_.texture.mask_size;
  primitive.highlighted = state_.texture.highlight;
  primitive.colour_mode = state_.colour.mode();
  primitive.colour = state_.colour.drawing_colour();
  primitive.background = state_.colour.background_colour();
  // The pixel a receiver stores is the address, not the value — in colour
  // mode 0 too, which finds or claims an entry for its colour (§5.3.2.5.1) —
  // so the address is kept for run() to resolve through the map as it finally
  // stood (§5.3.2.5's retroactivity), and for a blink process on the entry to
  // reach.
  primitive.colour_map_address = state_.colour.drawing_map_address();
  if (state_.colour.mode() == NabtsColourMode::kMappedWithBackground) {
    primitive.background_map_address =
        static_cast<int16_t>(state_.colour.map_background_address());
  }
  return primitive;
}

void NaplpsInterpreter::emit(NabtsPrimitive primitive) {
  // §6.2.3 and §6.2.4: while a DRCS character or a texture mask is being
  // defined, "all drawing operations affect the DRCS storage buffer rather than
  // the display area".
  if (collecting_ == Collecting::kDrcs ||
      collecting_ == Collecting::kTextureMask) {
    draw_into_definition(primitive);
    return;
  }
  // The figure is on the screen once the receiver has drawn it, which it was
  // seen doing for as long as that took.
  const int64_t started_ms = clock_ms_;
  pace(naplps_pace_figure_us(pace_, primitive));
  primitive.appear_ms = static_cast<uint32_t>(clock_ms_);
  primitive.draw_ms = static_cast<uint32_t>(clock_ms_ - started_ms);
  snapshot_.primitives.push_back(std::move(primitive));
}

void NaplpsInterpreter::pace(int64_t us) {
  // With the receiver's pace, a command is done — its figure on the screen —
  // only once the receiver has taken its time over it, and the next starts
  // then.
  if (pace_ == NaplpsReceiverPace::kOff || us <= 0) {
    return;
  }
  // Colour map writes made before this belong before it on the clock: both
  // receivers of the day show SET COLOR's colour the moment it runs, so what a
  // stage draws is seen in its own colours as it is drawn, not in the old ones
  // until the stage ends.
  flush_map_writes();
  pace_us_ += us;
  clock_ms_ = std::min(clock_ms_ + pace_us_ / 1000, kMaxPresentationMs);
  pace_us_ %= 1000;
}

bool NaplpsInterpreter::stays_on_row(const NabtsPoint& point) const {
  const bool path_is_horizontal = state_.text.path == NabtsCharPath::kRight ||
                                  state_.text.path == NabtsCharPath::kLeft;
  const double across = path_is_horizontal ? point.y - state_.cursor.y
                                           : point.x - state_.cursor.x;
  const double row =
      std::fabs(path_is_horizontal ? state_.text.character_field.dy
                                   : state_.text.character_field.dx);
  // Half a row: nearer than that to where it already is and the origin has not
  // been put on another row, however the coordinate was resolved.
  return std::fabs(across) < std::max(row, kRowHeightFloor) * 0.5;
}

NabtsPoint NaplpsInterpreter::cursor_alignment() const {
  // §5.3.2.3.7: "The alignment of the drawing point corresponds to the
  // character field origin for the underscore cursor and block cursor, and the
  // center of the character field for the cross-hair cursor and custom
  // cursor."
  const NabtsCursorStyle style = state_.text.cursor_style;
  if (style != NabtsCursorStyle::kCrossHair &&
      style != NabtsCursorStyle::kCustom) {
    return NabtsPoint{0.0, 0.0};
  }
  const FieldExtent extent = field_extent(state_.text);
  return NabtsPoint{(extent.x0 + extent.x1) / 2.0,
                    (extent.y0 + extent.y1) / 2.0};
}

NabtsPoint NaplpsInterpreter::fit_cursor(NabtsPoint cursor) const {
  const FieldExtent extent = field_extent(state_.text);
  // As close as possible: the least movement that brings each side of the
  // field inside, the lower edge winning where the field is larger than the
  // screen.
  cursor.x = std::max(std::min(cursor.x, 1.0 - extent.x1), -extent.x0);
  cursor.y = std::max(std::min(cursor.y, 1.0 - extent.y1), -extent.y0);
  return cursor;
}

void NaplpsInterpreter::move_drawing_point(const NabtsPoint& point) {
  state_.drawing_point = point;
  // §5.3.2.3.7 Table 10: move together (0) or drawing point leads (2) both take
  // the cursor with it, keeping their alignment, and never so far that the
  // character field leaves the unit screen.
  if (state_.text.move_attribute == 0 || state_.text.move_attribute == 2) {
    const NabtsPoint alignment = cursor_alignment();
    const NabtsPoint cursor =
        fit_cursor(NabtsPoint{point.x - alignment.x, point.y - alignment.y});
    // The character field origin moved to another row, so §5.3.2.3.6's
    // suppression window closes.
    if (!stays_on_row(cursor)) {
      wrap_suppress_ = WrapSuppress::kNone;
    }
    state_.cursor = cursor;
  }
}

void NaplpsInterpreter::move_cursor(const NabtsPoint& point) {
  // §5.3.2.3.6: any movement of the character field origin ends the window in
  // which an explicit APR APD would be suppressed — including the one a
  // displayed character makes, which is what tells a service's own line ending
  // from a real one further down the line. The automatic wrap itself also
  // passes through here; move_cursor_by() re-arms after it.
  wrap_suppress_ = WrapSuppress::kNone;
  // The cursor, unlike a drawing, stays on the screen: text written at a
  // cursor pushed off it would simply vanish.
  state_.cursor = naplps_on_unit_screen(point);
  // Move together (0) or cursor leads (1), keeping the alignment (§5.3.2.3.7).
  if (state_.text.move_attribute == 0 || state_.text.move_attribute == 1) {
    const NabtsPoint alignment = cursor_alignment();
    state_.drawing_point = NabtsPoint{state_.cursor.x + alignment.x,
                                      state_.cursor.y + alignment.y};
  }
}

NabtsPoint NaplpsInterpreter::line_start() const {
  // §6.1.2.7: "the first character position within the display area (or
  // within the active field, should the full character field corresponding to
  // the cursor lie entirely within the active field before the movement) along
  // the character path" — the field's own edge, whichever corner its origin
  // is, only while the cursor is in it (confining_area()).
  const FieldExtent extent = field_extent(state_.text);
  const NabtsPoint path = path_vector(state_.text.path);
  return against_edge(state_.cursor,
                      confining_area(state_, kDisplayArea, extent), extent,
                      -path.x, -path.y);
}

bool NaplpsInterpreter::move_cursor_by(CursorMove move, double along) {
  // §5.3.2.3.4 Table 8 and §5.3.2.3.5 Table 9: both spacings are multiples of
  // the character field dimension lying along the direction of travel — the one
  // parallel to the character path for a movement along it, the one
  // perpendicular for a movement across it. Proportional inter-character
  // spacing (code 3) is font-dependent and §5.3.2.3.4 makes the algorithm
  // implementation-dependent; a character of the repertoire is given its own
  // displacement by T.101 Annex D's Addendum (proportional_advance()), and
  // anything else steps one field: the floor the standard guarantees, "at
  // least as many characters per line as would be allowed by the current
  // character field dimensions".
  static constexpr double kCharacterSpacing[4] = {1.0, 1.25, 1.5, 1.0};
  static constexpr double kRowSpacing[4] = {1.0, 1.25, 1.5, 2.0};

  const bool along_path = move == CursorMove::kAdvance ||
                          move == CursorMove::kForward ||
                          move == CursorMove::kBackward;
  const bool path_is_horizontal = state_.text.path == NabtsCharPath::kRight ||
                                  state_.text.path == NabtsCharPath::kLeft;

  // "depending on the character path and character rotation" (§5.3.2.3.4):
  // turned 90 or 270 degrees, the field's height lies across the screen and
  // its width up it, so those are what a movement along x or y is measured by.
  const bool turned = state_.text.rotation == NabtsCharRotation::k90 ||
                      state_.text.rotation == NabtsCharRotation::k270;
  const double across = std::fabs(turned ? state_.text.character_field.dy
                                         : state_.text.character_field.dx);
  const double up = std::fabs(turned ? state_.text.character_field.dx
                                     : state_.text.character_field.dy);
  const double character_distance =
      (path_is_horizontal ? across : up) *
      kCharacterSpacing[state_.text.intercharacter_spacing & 0x03];
  const double row_distance = (path_is_horizontal ? up : across) *
                              kRowSpacing[state_.text.interrow_spacing & 0x03];
  double distance = along_path ? character_distance : row_distance;
  if (along_path && along > 0.0) {
    distance = along;
  } else if ((move == CursorMove::kForward || move == CursorMove::kBackward) &&
             spaced_proportionally(state_.text)) {
    // T.101 Annex D Addendum, clause 11.1: "The format effector characters
    // APB and APF in proportional space mode are considered to have a width
    // class of 9 (full width)."
    distance = naplps_proportional_displacement(kNaplpsFullWidthClass,
                                                state_.text.character_field.dx);
  }

  // Forward along the path, backward 180 degrees from it, down -90 and up +90
  // — rotating the path vector, so all four follow the path wherever TEXT put
  // it. On the default rightward path -90 degrees is straight down, which is
  // what makes APD the line feed.
  const NabtsPoint path = path_vector(state_.text.path);
  double step_x = 0.0;
  double step_y = 0.0;
  switch (move) {
    case CursorMove::kAdvance:
    case CursorMove::kForward:
      step_x = path.x;
      step_y = path.y;
      break;
    case CursorMove::kBackward:
      step_x = -path.x;
      step_y = -path.y;
      break;
    case CursorMove::kDown:
      step_x = path.y;
      step_y = -path.x;
      break;
    case CursorMove::kUp:
      step_x = -path.y;
      step_y = path.x;
      break;
  }

  const NabtsPoint next{state_.cursor.x + step_x * distance,
                        state_.cursor.y + step_y * distance};
  const FieldExtent extent = field_extent(state_.text);

  switch (move) {
    case CursorMove::kAdvance:
    case CursorMove::kForward: {
      // §5.3.2.3.6 for a character's advance, against the unit screen, and
      // §6.1.2.2 for APF, against the display area — either way the active
      // field instead where the cursor's field lies in it.
      const CursorArea limit = confining_area(
          state_, move == CursorMove::kAdvance ? kUnitScreenArea : kDisplayArea,
          extent);
      if (move_fits(limit, next, extent, step_x, step_y)) {
        move_cursor(next);
        return false;
      }
      // "an automatic APR (carriage return) and APD (line feed) are
      // immediately executed" (§5.3.2.3.6); APF's "cursor is instead moved to
      // the opposite edge (along the character path) ... and an automatic APD
      // is executed" (§6.1.2.2) comes to the same.
      move_cursor(line_start());
      (void)move_cursor_by(CursorMove::kDown);
      // §5.3.2.3.6: the explicit APR APD that a service sends after writing a
      // line flush to its field must now execute as a null operation. Armed
      // after the moves, which clear it.
      wrap_suppress_ = WrapSuppress::kArmed;
      return true;
    }

    case CursorMove::kBackward: {
      // §6.1.2.1: past the edge, "the cursor is instead moved to the opposite
      // edge (along the character path) of the display area (or active field)
      // and an automatic APU is executed".
      const CursorArea limit = confining_area(state_, kDisplayArea, extent);
      if (move_fits(limit, next, extent, step_x, step_y)) {
        move_cursor(next);
        return false;
      }
      move_cursor(against_edge(state_.cursor, limit, extent, path.x, path.y));
      (void)move_cursor_by(CursorMove::kUp);
      return true;
    }

    case CursorMove::kDown:
    case CursorMove::kUp: {
      const CursorArea limit = confining_area(state_, kDisplayArea, extent);
      if (move_fits(limit, next, extent, step_x, step_y)) {
        move_cursor(next);
        return false;
      }
      if (!state_.text.scroll) {
        // §6.2.7.14 SCROLL OFF: "the cursor to be repositioned to the opposite
        // edge of the area or field such that the character field so defined
        // lies entirely within the area or field".
        move_cursor(against_edge(next, limit, extent, -step_x, -step_y));
        return true;
      }
      // §6.2.7.13 SCROLL ON: "the entire display within the area or field to
      // be scrolled ... far enough to bring the next intended character
      // location just into the area or field", the cursor at that location.
      const NabtsPoint at = against_edge(next, limit, extent, step_x, step_y);
      NabtsPoint displacement{0.0, 0.0};
      if (step_y < 0.0) {
        displacement.y = limit.bottom - (next.y + extent.y0);
      } else if (step_y > 0.0) {
        displacement.y = limit.top - (next.y + extent.y1);
      } else if (step_x < 0.0) {
        displacement.x = limit.left - (next.x + extent.x0);
      } else if (step_x > 0.0) {
        displacement.x = limit.right - (next.x + extent.x1);
      }
      NabtsPrimitive scroll = make_primitive(NabtsPrimitiveKind::kScroll);
      scroll.origin = NabtsPoint{limit.left, limit.bottom};
      scroll.size =
          NabtsSize{limit.right - limit.left, limit.top - limit.bottom};
      scroll.points = {displacement};
      scroll.logical_pel = NabtsSize{0.0, 0.0};
      emit(std::move(scroll));
      move_cursor(at);
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// Definitions
// ---------------------------------------------------------------------------

void NaplpsInterpreter::begin_definition(Collecting what, uint8_t code) {
  collecting_ = what;
  definition_body_.clear();
  definition_code_ = code;
  drcs_target_ = nullptr;
  mask_target_ = nullptr;
  defining_frame_index_ = frames_.empty() ? 0 : frames_.size() - 1;
  definition_had_code_ = false;

  // §6.2.3 and §6.2.4: the buffer takes the aspect of the character field (a
  // DRCS character) or the texture mask size (a mask) when the definition
  // starts, and keeps it whatever the definition then does to either.
  const NabtsSize shape = what == Collecting::kTextureMask
                              ? state_.texture.mask_size
                              : state_.text.character_field;
  const double across = std::fabs(shape.dx);
  const double up = std::fabs(shape.dy);
  definition_extent_ = NabtsSize{1.0, 1.0};
  if (across > 0.0 && up > 0.0) {
    definition_extent_ = across >= up ? NabtsSize{1.0, up / across}
                                      : NabtsSize{across / up, 1.0};
  }

  if (what == Collecting::kDrcs) {
    uint16_t width = 0;
    uint16_t height = 0;
    state_.drcs_buffer_size(width, height);
    drcs_target_ = state_.begin_drcs(code, width, height);
    if (drcs_target_ == nullptr) {
      // Over the storage budget. The definition is still collected — its
      // drawing redirected into nothing — because falling back to normal
      // execution would paint the character's defining code onto the display,
      // which no receiver refusing the download would show.
      ++snapshot_.diagnostics.storage_refusals;
    }
    // The circular sequence of §6.2.3 continues from here even when the
    // character itself was refused.
    last_drcs_code_ = code;
    have_last_drcs_code_ = true;
    return;
  }

  if (what == Collecting::kTextureMask) {
    // §6.2.4: the code must be 4/1 to 4/4 for mask A to D.
    if (code < 0x41 || code > 0x44) {
      collecting_ = Collecting::kNothing;
      return;
    }
    const size_t index = static_cast<size_t>(code - 0x41);
    mask_target_ = &state_.texture_masks[index];
    // Table D1 item 5(3)(b): 16 by 16 stored elements.
    constexpr uint16_t kMaskSide = 16;
    mask_target_->width = kMaskSide;
    mask_target_->height = kMaskSide;
    mask_target_->elements.assign(static_cast<size_t>(kMaskSide) * kMaskSide,
                                  false);
  }
}

void NaplpsInterpreter::end_definition() {
  switch (collecting_) {
    case Collecting::kNothing:
      return;
    case Collecting::kMacro:
    case Collecting::kMacroExecuting: {
      const bool stores_code = !definition_body_.empty();
      if (!state_.define_macro(definition_code_, std::move(definition_body_),
                               definition_is_transmit_)) {
        ++snapshot_.diagnostics.storage_refusals;
      } else if (stores_code && definition_code_ >= kNaplpsFirstCode) {
        macros_defined_.set(definition_code_ - kNaplpsFirstCode);
      }
      break;
    }
    case Collecting::kDrcs:
      // §6.2.3: "If a DRCS definition is immediately terminated with no
      // intervening presentation layer code, the buffer space allocated to
      // that character is freed."
      if (!definition_had_code_ && drcs_target_ != nullptr) {
        state_.free_drcs(definition_code_);
      }
      [[fallthrough]];
    case Collecting::kTextureMask:
      // §6.2.3: "After the downloading sequence has been terminated, the
      // receiving device reverts to the normal procedure of mapping the unit
      // screen to the physical display screen, with the drawing point reset to
      // (0,0)."
      state_.drawing_point = NabtsPoint{0.0, 0.0};
      break;
  }
  collecting_ = Collecting::kNothing;
  definition_body_.clear();
  drcs_target_ = nullptr;
  mask_target_ = nullptr;
  definition_is_transmit_ = false;
}

void NaplpsInterpreter::draw_into_definition(const NabtsPrimitive& primitive) {
  uint16_t width = 0;
  uint16_t height = 0;
  std::vector<bool>* elements = nullptr;
  if (drcs_target_ != nullptr) {
    width = drcs_target_->width;
    height = drcs_target_->height;
    elements = &drcs_target_->elements;
  } else if (mask_target_ != nullptr) {
    width = mask_target_->width;
    height = mask_target_->height;
    elements = &mask_target_->elements;
  }
  if (elements == nullptr || width == 0 || height == 0) {
    return;
  }

  // The definition is drawn by exactly the rules the display is drawn by:
  // §6.2.3 executes the defining code "in the same manner as if it were being
  // displayed", into the buffer rather than onto the screen. So the buffer is
  // a receiver of its own size and the shared deposit fills it — textures,
  // masks, highlights, colour mode 2's background and all.
  //
  // §6.2.3 places the buffer at the unit screen's lower left with "the larger
  // buffer dimension (dx or dy) ... coincide with the entire corresponding
  // axis of the unit screen", so the other dimension covers only its share of
  // its axis — a field 6 wide by 10 high is defined by code "in the range 0.0
  // to 0.6 in the X axis".
  const NaplpsRenderGrid buffer_grid{static_cast<int>(width),
                                     static_cast<int>(height)};
  NaplpsCellSurface surface(buffer_grid);
  NaplpsGridMapping mapping(buffer_grid);
  mapping.columns_per_unit = width / definition_extent_.dx;
  mapping.rows_per_unit = height / definition_extent_.dy;
  NaplpsRasteriser raster(surface, mapping);

  // The masks and DRCS characters as they stand now, which a nested DRCS
  // character or a masked fill inside the definition draws with.
  const std::vector<NabtsDrcsCharacter> drcs = state_.defined_drcs();
  NaplpsDepositContext context;
  context.masks = state_.texture_masks;
  context.drcs = &drcs;
  naplps_deposit_primitive(primitive, context, raster);

  // "Each element that is affected shall be set to the on state, unless it is
  // written into with nominal black, in which case it is set to the off
  // state" — element by element, by what was written into it.
  for (int row = 0; row < surface.height(); ++row) {
    for (int column = 0; column < surface.width(); ++column) {
      const NaplpsCell& cell = surface.at(column, row);
      if (!cell.painted) {
        continue;
      }
      (*elements)[static_cast<size_t>(row) * width +
                  static_cast<size_t>(column)] =
          !(cell.colour == kNabtsNominalBlack);
    }
  }
}

void NaplpsInterpreter::invoke_macro(uint8_t code) {
  pace(naplps_pace_macro_us(pace_));
  // Run by the page itself, so not a macro the page leaves for a key
  // (linked_keys()) — whether or not it resolves.
  if (code >= kNaplpsFirstCode) {
    macros_invoked_.set(code - kNaplpsFirstCode);
  }
  // §6.2.2.2: "A macro is considered to be undefined during definition until
  // the definition is terminated. Therefore, if a DEFP MACRO command contains
  // a reference to itself, or if it references another macro which references
  // the one being defined, the reference to the macro being defined is
  // executed as a null operation."
  if (collecting_ == Collecting::kMacroExecuting && code == definition_code_) {
    return;
  }
  const NaplpsMacro* entry = state_.macro(code);
  if (entry == nullptr || !entry->defined()) {
    ++snapshot_.diagnostics.unresolved_macros;
    return;
  }
  // §6.2.2.3: a transmit macro "when called, [is] not executed, but [is]
  // transmitted in their entirety to the host computer or to a local
  // application process". There is no host here, so it draws nothing.
  if (entry->transmit) {
    return;
  }
  // The standard bounds nesting only by memory. A recovered record can contain
  // a macro that invokes itself — §6.2.2.2 rules that out for DEFP MACRO alone
  // — so the depth is capped and the overrun counted.
  if (frames_.size() > kNaplpsMaxMacroDepth) {
    ++snapshot_.diagnostics.unresolved_macros;
    return;
  }
  // The expansion runs the definition as it stood when invoked, to its end.
  // A macro can clear the macros (RESET, §5.3.2.9.3) or redefine itself while
  // it runs, and the standard says nothing of the expansion under way; one
  // that stopped part way would draw part of what the invocation asked for.
  auto body = std::make_shared<const std::vector<uint8_t>>(entry->code);
  frames_.push_back(Frame{body->data(), body->size(), 0, body});
}

NabtsPageSnapshot NaplpsInterpreter::press_key(uint8_t key) {
  // The definition as it stands, copied: an expansion may clear the macros or
  // redefine the one running (see invoke_macro()), and runs to its end either
  // way. Its bytes are presentation code as the record carried it, already
  // read down to seven bits, so it runs as a record of its own would — over the
  // standing display, from the state the page left.
  std::vector<uint8_t> body;
  if (key < kNaplpsLinkedKeyCount) {
    const NaplpsMacro* entry =
        state_.macro(static_cast<uint8_t>(kNaplpsFirstLinkedMacro + key));
    if (entry != nullptr && entry->defined() && !entry->transmit) {
      body = entry->code;
    }
  }
  return run(body, /*keep_display=*/true);
}

std::vector<NaplpsByteRole> naplps_read_roles(
    const std::vector<uint8_t>& record) {
  std::vector<NaplpsByteRole> roles;
  NaplpsInterpreter interpreter;
  interpreter.set_trace(&roles);
  (void)interpreter.run(record);
  return roles;
}

}  // namespace orc
