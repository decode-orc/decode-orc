/*
 * File:        naplps_receiver_pace.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     How long a receiver of the day took over each part of a NAPLPS
 *              page
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "naplps_receiver_pace.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "naplps_code_env.h"
#include "naplps_font.h"
#include "naplps_pdi.h"
#include "naplps_raster.h"

namespace orc {

namespace {

/// Both receivers' pixels to the unit screen, on both axes: 256 across, and
/// the display area's 0.78125 of the unit screen 200 up.
constexpr double kPixelsPerUnit = kNaplpsReferencePixelsPerUnit;

/// The tallest cell is the display area's height.
constexpr double kTallestCellPx = 200.0;

/// The tolerance arcs are measured at: the curve as a polyline within half a
/// receiver pixel.
constexpr double kArcTolerance = 1.0 / 512.0;

/// How a receiver chose the face it drew a character field in, and when it
/// drew that face through its slower scaling routine rather than copying it
/// from the ROM.
enum class FaceChoice : uint8_t {
  /// The face that leaves the fewest columns and rows of the field over, the
  /// larger on a tie (naplps_font_face_for_field()); a face drawn at twice its
  /// size or more on either axis is scaled.
  kColumnsAndRows,
  /// The face that leaves the fewest rows over, the larger on a tie; only a
  /// face drawn at twice its width or more is scaled, and one stretched down
  /// the field alone is still copied a row at a time from the ROM.
  kRowsAlone,
};

/**
 * @brief One receiver's times, in microseconds
 *
 * Each is what the receiver's firmware took over blocks of identical commands,
 * the difference of two block lengths over the difference of their counts.
 */
struct PaceTable {
  FaceChoice face_choice = FaceChoice::kColumnsAndRows;
  /// A character copied from the font ROM at its own size — the normal face's,
  /// the larger face's, SPACE — and either in colour mode 2, where the
  /// background is drawn as well (§5.3.2.5.3).
  double character_us = 0.0;
  double larger_character_us = 0.0;
  double space_us = 0.0;
  double character_with_background_us = 0.0;
  /// A scaled character: its own cost, a cost per row of the cell, and a cost
  /// per pixel of the cell up to a cap past which the receiver did no more.
  double scaled_character_us = 0.0;
  double scaled_character_row_us = 0.0;
  double scaled_character_pixel_us = 0.0;
  double scaled_character_pixel_cap = 0.0;
  /// A scaled SPACE, and per pixel of its cell up to the same cap.
  double scaled_space_us = 0.0;
  double scaled_space_pixel_us = 0.0;

  /// A point (§5.3.3.1).
  double point_us = 0.0;
  /// A line (§5.3.3.2), and each pixel of it by how it was drawn: in a
  /// horizontal run, down a column, or stepped along a sloping line's longer
  /// axis.
  double line_us = 0.0;
  double line_horizontal_pixel_us = 0.0;
  double line_vertical_pixel_us = 0.0;
  double line_sloping_pixel_us = 0.0;
  /// An outlined rectangle (§5.3.3.4), by the pixels of its horizontal and of
  /// its vertical sides.
  double rect_outline_us = 0.0;
  double rect_outline_horizontal_pixel_us = 0.0;
  double rect_outline_vertical_pixel_us = 0.0;
  /// A filled rectangle: a run per row, and the pixels.
  double rect_fill_us = 0.0;
  double rect_fill_row_us = 0.0;
  double rect_fill_pixel_us = 0.0;
  /// An outlined polygon (§5.3.3.5): each vertex, then its edges as lines.
  double poly_outline_vertex_us = 0.0;
  double poly_outline_horizontal_pixel_us = 0.0;
  double poly_outline_vertical_pixel_us = 0.0;
  double poly_outline_sloping_pixel_us = 0.0;
  /// A filled polygon: each vertex, an edge scan per row, and the pixels.
  double poly_fill_vertex_us = 0.0;
  double poly_fill_row_us = 0.0;
  double poly_fill_pixel_us = 0.0;
  /// An arc (§5.3.3.3), by its length; filled, by its rows and area as well.
  double arc_outline_us = 0.0;
  double arc_filled_us = 0.0;
  double arc_length_pixel_us = 0.0;
  double arc_fill_row_us = 0.0;
  double arc_fill_pixel_us = 0.0;
  /// INCREMENTAL POINT (§5.3.3.6.3), by the pels it deposits.
  double incremental_points_us = 0.0;
  double incremental_pel_us = 0.0;
  /// A scroll of the active field.
  double scroll_us = 0.0;

  /// A clear of the display, a macro call, and a byte collected into a
  /// definition.
  int64_t clear_us = 0;
  int64_t macro_us = 0;
  int64_t definition_byte_us = 0;

  /// The PDIs, once their operands are in (§5.3.2), and every drawing command
  /// before its figure (2/6 to 3/7 and 3/9 to 3/11).
  int64_t reset_us = 0;
  int64_t domain_us = 0;
  int64_t text_us = 0;
  int64_t texture_us = 0;
  int64_t point_set_us = 0;
  int64_t field_us = 0;
  int64_t set_colour_us = 0;
  int64_t wait_us = 0;
  int64_t select_colour_us = 0;
  int64_t blink_us = 0;
  int64_t drawing_command_us = 0;

  /// The C0 controls (§6.1): the shifts, the cursor moves, APS, NSR, and the
  /// rest.
  int64_t shift_us = 0;
  int64_t cursor_move_us = 0;
  int64_t aps_us = 0;
  int64_t nsr_us = 0;
  int64_t other_c0_us = 0;

  /// The C1 controls (§6.2): the size controls, BLINK START and STOP, and the
  /// rest.
  int64_t size_control_us = 0;
  int64_t blink_control_us = 0;
  int64_t other_c1_us = 0;

  /// Which way a fill or a clear was swept across the screen.
  NabtsFillSweep fill_sweep = NabtsFillSweep::kTopDown;
};

/**
 * @brief The Norpak TTX 650 (1.79 MHz, 28.636 MHz over 16)
 *
 * Against the TTX 650's own drawing of the recorded pages this predicts a
 * median 0.95 of each stage's drawing time, with 77 % of stages within a
 * quarter or 0.1 s, and 70 % of pages within a quarter or 0.25 s.
 */
constexpr PaceTable norpak_ttx650() {
  PaceTable t;
  t.face_choice = FaceChoice::kColumnsAndRows;
  // A face the field holds only once is copied from the ROM at the ROM's own
  // speed, however much of the field it leaves over: twenty characters in 6
  // by 13 to 7 by 18 fields took 3.5 ms each, in 8 by 13 to 11 by 16 fields
  // 4.0 ms. Only a face drawn at twice its size or more goes through the
  // slower scaling routine, row by row: 16 by 24 took 19.8 ms.
  t.character_us = 3400.0;
  t.larger_character_us = 4000.0;
  t.space_us = 1550.0;
  t.character_with_background_us = 6500.0;
  t.scaled_character_us = 10000.0;
  t.scaled_character_row_us = 400.0;
  t.scaled_space_us = 2000.0;

  t.point_us = 300.0;
  // A horizontal run is a fill and nearly free; every other pixel is plotted.
  t.line_us = 3000.0;
  t.line_horizontal_pixel_us = 3.3;
  t.line_vertical_pixel_us = 290.0;
  t.line_sloping_pixel_us = 300.0;
  t.rect_outline_us = 6500.0;
  t.rect_outline_horizontal_pixel_us = 3.0;
  t.rect_outline_vertical_pixel_us = 290.0;
  t.rect_fill_us = 3500.0;
  t.rect_fill_row_us = 320.0;
  t.rect_fill_pixel_us = 2.8;
  t.poly_outline_vertex_us = 2800.0;
  t.poly_outline_horizontal_pixel_us = 3.0;
  t.poly_outline_vertical_pixel_us = 290.0;
  t.poly_outline_sloping_pixel_us = 300.0;
  t.poly_fill_vertex_us = 4100.0;
  t.poly_fill_row_us = 600.0;
  t.poly_fill_pixel_us = 3.0;
  // An arc is plotted a pixel at a time along its length: a circle of radius
  // 90 took 809 ms.
  t.arc_outline_us = 9200.0;
  t.arc_filled_us = 12000.0;
  t.arc_length_pixel_us = 1410.0;
  t.arc_fill_row_us = 500.0;
  t.arc_fill_pixel_us = 1.0;
  t.incremental_points_us = 3000.0;
  t.incremental_pel_us = 50.0;
  t.scroll_us = 50000.0;

  t.clear_us = 169000;
  t.macro_us = 1000;
  t.definition_byte_us = 200;

  t.reset_us = 2200;
  t.domain_us = 3200;
  t.text_us = 10800;
  t.texture_us = 1000;
  t.point_set_us = 3600;
  t.field_us = 5900;
  t.set_colour_us = 3100;
  t.wait_us = 2000;
  t.select_colour_us = 1100;
  t.blink_us = 2800;
  t.drawing_command_us = 3500;

  t.shift_us = 300;
  t.cursor_move_us = 900;
  t.aps_us = 1000;
  t.nsr_us = 9800;
  t.other_c0_us = 500;

  // The size controls set up a TEXT of their own (§6.2.7.6 to §6.2.7.10).
  t.size_control_us = 9500;
  t.blink_control_us = 1800;
  t.other_c1_us = 1000;

  // Filmed a field at a time through its firmware, the TTX 650 swept a
  // full-width fill and a clear up its screen from the bottom row.
  t.fill_sweep = NabtsFillSweep::kBottomUp;
  return t;
}

/**
 * @brief The Panasonic TU-2000x (its 6809 at 1.5 MHz)
 *
 * Against the TU-2000x's own drawing of the recorded pages this predicts a
 * median 0.99 of each stage's drawing time, with 85 % of stages within a
 * quarter or 0.1 s, and 88 % of pages within a quarter or 0.25 s; the TTX
 * 650's table puts only 65 % of its stages there. On the pages both drew, the
 * TU-2000x took a median 1.12 times as long.
 */
constexpr PaceTable panasonic_tu2000x() {
  PaceTable t;
  // The TU-2000x picks the face whose cell leaves the fewest rows of the field
  // over (naplps_font.h). It copies a face stretched down the field from the
  // ROM, as fast as one at its own size — 6 by 20 at 6.1 ms a character
  // against 4.6 — but runs one at twice its width or more through a scaling
  // routine paid by the cell's area, up to the 9,600 pixels past which it
  // took no longer: 12 by 20 took 20 to 28 ms, 64 by 96 467 ms, a
  // display-sized cell 826 ms.
  t.face_choice = FaceChoice::kRowsAlone;
  t.character_us = 4700.0;
  t.larger_character_us = 4500.0;
  t.space_us = 3500.0;
  t.character_with_background_us = 5500.0;
  t.scaled_character_us = 4600.0;
  t.scaled_character_pixel_us = 80.0;
  t.scaled_character_pixel_cap = 9600.0;
  t.scaled_space_us = 3500.0;
  t.scaled_space_pixel_us = 4.0;

  t.point_us = 100.0;
  // Every pixel of a line is plotted, a sloping one at three and a half
  // times the cost of one on an axis.
  t.line_us = 1600.0;
  t.line_horizontal_pixel_us = 180.0;
  t.line_vertical_pixel_us = 180.0;
  t.line_sloping_pixel_us = 650.0;
  t.rect_outline_us = 1700.0;
  t.rect_outline_horizontal_pixel_us = 180.0;
  t.rect_outline_vertical_pixel_us = 180.0;
  t.rect_fill_us = 3700.0;
  t.rect_fill_row_us = 770.0;
  t.rect_fill_pixel_us = 2.0;
  t.poly_outline_vertex_us = 3500.0;
  t.poly_outline_horizontal_pixel_us = 180.0;
  t.poly_outline_vertical_pixel_us = 180.0;
  t.poly_outline_sloping_pixel_us = 650.0;
  // Polygon fills two to three times the TTX 650's: a triangle of radius 90
  // took 527 ms against its 242.
  t.poly_fill_vertex_us = 10000.0;
  t.poly_fill_row_us = 1000.0;
  t.poly_fill_pixel_us = 8.0;
  // Arcs at a little over half the TTX 650's: radius 90 in 454 ms.
  t.arc_outline_us = 5300.0;
  t.arc_filled_us = 7600.0;
  t.arc_length_pixel_us = 790.0;
  t.arc_fill_row_us = 350.0;
  t.arc_fill_pixel_us = 0.5;
  t.incremental_points_us = 3000.0;
  t.incremental_pel_us = 50.0;
  t.scroll_us = 50000.0;

  t.clear_us = 70000;
  t.macro_us = 1000;
  t.definition_byte_us = 200;

  t.reset_us = 2600;
  t.domain_us = 2500;
  t.text_us = 3900;
  t.texture_us = 1400;
  t.point_set_us = 2300;
  t.field_us = 3100;
  // The TU-2000x loads its palette in the vertical blank, so SET COLOR, BLINK
  // and BLINK START wait for the next field: a whole one in a block of them,
  // half of one on average where other drawing goes between, which is what
  // its pages do (2.3 ms for the command itself and 8.4 ms waiting).
  t.set_colour_us = 8400;
  t.wait_us = 2000;
  t.select_colour_us = 1100;
  t.blink_us = 8500;
  t.drawing_command_us = 2300;

  t.shift_us = 360;
  t.cursor_move_us = 600;
  t.aps_us = 1000;
  t.nsr_us = 3700;
  t.other_c0_us = 500;

  t.size_control_us = 2100;
  t.blink_control_us = 8400;
  t.other_c1_us = 1000;

  // Filmed a field at a time through its firmware, the TU-2000x swept a
  // full-width fill and a clear down its screen from the top row, as a
  // recorded headend decoder did.
  t.fill_sweep = NabtsFillSweep::kTopDown;
  return t;
}

constexpr PaceTable kNorpakTtx650 = norpak_ttx650();
constexpr PaceTable kPanasonicTu2000x = panasonic_tu2000x();

/// |pace|'s table, or nullptr with the pace off.
const PaceTable* table_for(NaplpsReceiverPace pace) {
  switch (pace) {
    case NaplpsReceiverPace::kNorpakTtx650:
      return &kNorpakTtx650;
    case NaplpsReceiverPace::kPanasonicTu2000x:
      return &kPanasonicTu2000x;
    case NaplpsReceiverPace::kOff:
      break;
  }
  return nullptr;
}

/// An outline's length in receiver pixels, by how the receiver drew it.
struct OutlineLengths {
  double horizontal = 0.0;
  double vertical = 0.0;
  double sloping = 0.0;
};

/// The edges through |points| in order, and back to the first where
/// |closed|. An edge less than half a pixel off the horizontal is a run; one
/// less than half a pixel off the vertical is a column; anything else is
/// plotted a pixel per step along its longer axis.
OutlineLengths outline_lengths(const std::vector<NabtsPoint>& points,
                               bool closed) {
  OutlineLengths lengths;
  const size_t count = points.size();
  if (count < 2) {
    return lengths;
  }
  const size_t edges = closed ? count : count - 1;
  for (size_t i = 0; i < edges; ++i) {
    const NabtsPoint& from = points[i];
    const NabtsPoint& to = points[(i + 1) % count];
    const double dx = std::fabs(to.x - from.x) * kPixelsPerUnit;
    const double dy = std::fabs(to.y - from.y) * kPixelsPerUnit;
    if (dy < 0.5) {
      lengths.horizontal += dx;
    } else if (dx < 0.5) {
      lengths.vertical += dy;
    } else {
      lengths.sloping += std::max(dx, dy);
    }
  }
  return lengths;
}

/// The steps a pixel at a time along |points|: each edge's longer axis.
double stepped_length(const std::vector<NabtsPoint>& points, bool closed) {
  double length = 0.0;
  for (size_t i = 0; i + 1 < points.size(); ++i) {
    length += std::max(std::fabs(points[i + 1].x - points[i].x),
                       std::fabs(points[i + 1].y - points[i].y));
  }
  if (closed && points.size() > 2) {
    length += std::max(std::fabs(points.front().x - points.back().x),
                       std::fabs(points.front().y - points.back().y));
  }
  return length * kPixelsPerUnit;
}

/// The display rows a figure through |points| spans, as far as they are on
/// the display.
double rows_spanned(const std::vector<NabtsPoint>& points) {
  if (points.empty()) {
    return 0.0;
  }
  double low = points.front().y;
  double high = points.front().y;
  for (const NabtsPoint& point : points) {
    low = std::min(low, point.y);
    high = std::max(high, point.y);
  }
  low = std::max(low, 0.0);
  high = std::min(high, kNabtsDisplayAreaHeight);
  return std::max(high - low, 0.0) * kPixelsPerUnit;
}

/// The pixels of the rectangle at |origin| of |size| that are on the display.
double on_screen_pixels(const NabtsPoint& origin, const NabtsSize& size) {
  const auto overlap = [](double from, double extent, double limit) {
    const double low = std::max(std::min(from, from + extent), 0.0);
    const double high = std::min(std::max(from, from + extent), limit);
    return std::max(high - low, 0.0);
  };
  return overlap(origin.x, size.dx, 1.0) *
         overlap(origin.y, size.dy, kNabtsDisplayAreaHeight) * kPixelsPerUnit *
         kPixelsPerUnit;
}

/// The pixels |points| enclose (the shoelace formula), at most the unit
/// screen's.
double enclosed_pixels(const std::vector<NabtsPoint>& points) {
  double twice = 0.0;
  for (size_t i = 0; i < points.size(); ++i) {
    const NabtsPoint& a = points[i];
    const NabtsPoint& b = points[(i + 1) % points.size()];
    twice += a.x * b.y - b.x * a.y;
  }
  return std::min(std::fabs(twice) / 2.0, 1.0) * kPixelsPerUnit *
         kPixelsPerUnit;
}

/// The face |table|'s receiver drew a |columns| by |rows| field in, and
/// whether it scaled that face to do so.
struct FaceAtField {
  const NaplpsFontFace* face = nullptr;
  bool scaled = false;
};

FaceAtField face_at_field(const PaceTable& table, int columns, int rows) {
  FaceAtField out;
  if (table.face_choice == FaceChoice::kColumnsAndRows) {
    out.face = &naplps_font_face_for_field(kNaplpsGridReference, columns, rows);
    out.scaled = columns >= 2 * out.face->width || rows >= 2 * out.face->height;
    return out;
  }
  // The rows each of the two generators' faces leaves over, where the field
  // holds it once; the larger face where they leave the same.
  const NaplpsFontFace& normal = naplps_font_face(0);
  const NaplpsFontFace* larger = naplps_font_larger_face();
  out.face = &normal;
  if (larger != nullptr && larger->width <= columns && larger->height <= rows) {
    const bool normal_fits = normal.width <= columns && normal.height <= rows;
    if (!normal_fits || rows % larger->height <= rows % normal.height) {
      out.face = larger;
    }
  }
  out.scaled = columns >= 2 * out.face->width;
  return out;
}

double character_us(const PaceTable& table, const NabtsPrimitive& figure) {
  const double width = std::fabs(figure.size.dx) * kPixelsPerUnit;
  const double height = std::fabs(figure.size.dy) * kPixelsPerUnit;
  const bool with_background =
      figure.colour_mode == NabtsColourMode::kMappedWithBackground;
  // SPACE draws nothing unless something is drawn behind it.
  const bool space =
      figure.character == 0x20 &&
      figure.repertoire == NabtsPrimitive::Repertoire::kPrimary &&
      !with_background && !figure.reverse_video;
  const int columns = std::max(1, naplps_reference_pixels(figure.size.dx));
  const int rows = std::max(1, naplps_reference_pixels(figure.size.dy));
  const FaceAtField drawn = face_at_field(table, columns, rows);
  if (!drawn.scaled) {
    if (space) {
      return table.space_us;
    }
    if (with_background) {
      return table.character_with_background_us;
    }
    return drawn.face->larger ? table.larger_character_us : table.character_us;
  }
  const double pixels = std::min(width * std::min(height, kTallestCellPx),
                                 table.scaled_character_pixel_cap);
  if (space) {
    return table.scaled_space_us + table.scaled_space_pixel_us * pixels;
  }
  return table.scaled_character_us +
         table.scaled_character_row_us * std::min(height, kTallestCellPx) +
         table.scaled_character_pixel_us * pixels;
}

}  // namespace

int64_t naplps_pace_clear_us(NaplpsReceiverPace pace) {
  const PaceTable* table = table_for(pace);
  return table == nullptr ? 0 : table->clear_us;
}

int64_t naplps_pace_macro_us(NaplpsReceiverPace pace) {
  const PaceTable* table = table_for(pace);
  return table == nullptr ? 0 : table->macro_us;
}

int64_t naplps_pace_definition_byte_us(NaplpsReceiverPace pace) {
  const PaceTable* table = table_for(pace);
  return table == nullptr ? 0 : table->definition_byte_us;
}

int64_t naplps_pace_pdi_us(NaplpsReceiverPace pace, uint8_t opcode) {
  const PaceTable* table = table_for(pace);
  if (table == nullptr || !naplps_is_pdi_opcode(opcode)) {
    return 0;
  }
  switch (static_cast<NaplpsPdi>(opcode)) {
    case NaplpsPdi::kReset:
      return table->reset_us;
    case NaplpsPdi::kDomain:
      return table->domain_us;
    case NaplpsPdi::kText:
      return table->text_us;
    case NaplpsPdi::kTexture:
      return table->texture_us;
    case NaplpsPdi::kPointSetAbs:
    case NaplpsPdi::kPointSetRel:
      return table->point_set_us;
    case NaplpsPdi::kField:
      return table->field_us;
    case NaplpsPdi::kSetColour:
      return table->set_colour_us;
    case NaplpsPdi::kWait:
      return table->wait_us;
    case NaplpsPdi::kSelectColour:
      return table->select_colour_us;
    case NaplpsPdi::kBlink:
      return table->blink_us;
    default:
      // Every drawing command, 2/6 to 3/7 and 3/9 to 3/11, before its figures.
      return table->drawing_command_us;
  }
}

int64_t naplps_pace_c0_us(NaplpsReceiverPace pace, uint8_t code) {
  const PaceTable* table = table_for(pace);
  if (table == nullptr) {
    return 0;
  }
  switch (code) {
    case kNaplpsEsc:
    case kNaplpsCs:
      return 0;
    case kNaplpsSo:
    case kNaplpsSi:
    case kNaplpsSs2:
    case kNaplpsSs3:
      return table->shift_us;
    case kNaplpsApb:
    case kNaplpsApf:
    case kNaplpsApd:
    case kNaplpsApu:
    case kNaplpsApr:
    case kNaplpsAph:
      return table->cursor_move_us;
    case kNaplpsAps:
      return table->aps_us;
    case kNaplpsNsr:
      return table->nsr_us;
    default:
      return table->other_c0_us;
  }
}

int64_t naplps_pace_c1_us(NaplpsReceiverPace pace, uint8_t control) {
  const PaceTable* table = table_for(pace);
  if (table == nullptr) {
    return 0;
  }
  switch (static_cast<NaplpsC1>(control)) {
    case NaplpsC1::kSmallText:
    case NaplpsC1::kMediumText:
    case NaplpsC1::kNormalText:
    case NaplpsC1::kDoubleHeight:
    case NaplpsC1::kDoubleSize:
      return table->size_control_us;
    case NaplpsC1::kBlinkStart:
    case NaplpsC1::kBlinkStop:
      return table->blink_control_us;
    default:
      return table->other_c1_us;
  }
}

int64_t naplps_pace_figure_us(NaplpsReceiverPace pace,
                              const NabtsPrimitive& figure) {
  const PaceTable* table = table_for(pace);
  if (table == nullptr) {
    return 0;
  }
  const PaceTable& t = *table;
  double us = 0.0;
  switch (figure.kind) {
    case NabtsPrimitiveKind::kCharacter:
      us = character_us(t, figure);
      break;
    case NabtsPrimitiveKind::kPoint:
      us = t.point_us;
      break;
    case NabtsPrimitiveKind::kLine: {
      const OutlineLengths lengths = outline_lengths(figure.points, false);
      us = t.line_us + t.line_horizontal_pixel_us * lengths.horizontal +
           t.line_vertical_pixel_us * lengths.vertical +
           t.line_sloping_pixel_us * lengths.sloping;
      break;
    }
    case NabtsPrimitiveKind::kRectangle: {
      const double width = std::fabs(figure.size.dx) * kPixelsPerUnit;
      const double height = std::fabs(figure.size.dy) * kPixelsPerUnit;
      if (!figure.filled) {
        us = t.rect_outline_us +
             t.rect_outline_horizontal_pixel_us * 2.0 * width +
             t.rect_outline_vertical_pixel_us * 2.0 * height;
        break;
      }
      const NabtsPoint far{figure.origin.x + figure.size.dx,
                           figure.origin.y + figure.size.dy};
      us = t.rect_fill_us +
           t.rect_fill_row_us * rows_spanned({figure.origin, far}) +
           t.rect_fill_pixel_us * on_screen_pixels(figure.origin, figure.size);
      break;
    }
    case NabtsPrimitiveKind::kPolygon: {
      const double vertices = static_cast<double>(figure.points.size());
      if (!figure.filled) {
        const OutlineLengths lengths = outline_lengths(figure.points, true);
        us = t.poly_outline_vertex_us * vertices +
             t.poly_outline_horizontal_pixel_us * lengths.horizontal +
             t.poly_outline_vertical_pixel_us * lengths.vertical +
             t.poly_outline_sloping_pixel_us * lengths.sloping;
        break;
      }
      us = t.poly_fill_vertex_us * vertices +
           t.poly_fill_row_us * rows_spanned(figure.points) +
           t.poly_fill_pixel_us * enclosed_pixels(figure.points);
      break;
    }
    case NabtsPrimitiveKind::kArc: {
      const std::vector<NabtsPoint> curve =
          naplps_arc_polyline(figure.points, kArcTolerance);
      if (!figure.filled) {
        us = t.arc_outline_us +
             t.arc_length_pixel_us * stepped_length(curve, false);
        break;
      }
      us = t.arc_filled_us +
           t.arc_length_pixel_us * stepped_length(curve, true) +
           t.arc_fill_row_us * rows_spanned(curve) +
           t.arc_fill_pixel_us * enclosed_pixels(curve);
      break;
    }
    case NabtsPrimitiveKind::kIncrementalPoints:
      us = t.incremental_points_us +
           t.incremental_pel_us *
               static_cast<double>(figure.incremental_colours.size());
      break;
    case NabtsPrimitiveKind::kScroll:
      us = t.scroll_us;
      break;
  }
  return static_cast<int64_t>(us);
}

NabtsFillSweep naplps_pace_fill_sweep(NaplpsReceiverPace pace) {
  const PaceTable* table = table_for(pace);
  return table == nullptr ? NabtsFillSweep::kTopDown : table->fill_sweep;
}

}  // namespace orc
