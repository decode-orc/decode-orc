/*
 * File:        naplps_interpreter_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NAPLPS interpreter: PDI execution, attribute
 *              inheritance, macros, DRCS and the storage budget
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_interpreter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace orc {
namespace {

/// A code position in the standard's column/row notation.
constexpr uint8_t code(int column, int row) {
  return static_cast<uint8_t>((column << 4) | row);
}

/// A numeric-data byte of the PDI set carrying |payload| in b6-b1 (§5.3.1).
constexpr uint8_t numeric(uint8_t payload) {
  return static_cast<uint8_t>(0x40 | (payload & 0x3F));
}

/// A one-byte coordinate word: three bits of X then three of Y (Figure 11).
constexpr uint8_t coord1(uint8_t x, uint8_t y) {
  return numeric(static_cast<uint8_t>(((x & 0x7) << 3) | (y & 0x7)));
}

/// A three-byte coordinate word: three bits of X then three of Y per byte, most
/// significant first (Figure 11), which is nine bits an axis of two's
/// complement, so any 1/256 of the unit screen from -1 to 255/256 can be named
/// exactly. A value past the top is taken as 255/256.
std::vector<uint8_t> coord3(double x, double y) {
  const auto encode = [](double value) {
    const auto steps =
        std::clamp(static_cast<int32_t>(value * 256.0), -256, 255);
    return static_cast<uint32_t>(steps) & 0x1FFu;
  };
  const uint32_t xb = encode(x);
  const uint32_t yb = encode(y);
  std::vector<uint8_t> out;
  for (int i = 0; i < 3; ++i) {
    const int shift = 6 - 3 * i;
    out.push_back(numeric(static_cast<uint8_t>((((xb >> shift) & 0x7) << 3) |
                                               ((yb >> shift) & 0x7))));
  }
  return out;
}

/**
 * @brief Builds a presentation record byte by byte
 *
 * Every record starts by invoking the PDI set, because that is what a record
 * drawing anything does: §4.3.1.3 designates PDI into G1, and SO invokes G1.
 */
class Record {
 public:
  /// SO — invoke G1, which by default holds the PDI set.
  Record& pdi() { return byte(0x0E); }
  /// SI — invoke G0, the primary character set.
  Record& text_mode() { return byte(0x0F); }

  Record& byte(uint8_t value) {
    bytes_.push_back(value);
    return *this;
  }
  Record& add(std::initializer_list<uint8_t> values) {
    for (const uint8_t value : values) {
      bytes_.push_back(value);
    }
    return *this;
  }
  /// A coordinate word or any other run built elsewhere, such as coord3().
  Record& add(const std::vector<uint8_t>& values) {
    bytes_.insert(bytes_.end(), values.begin(), values.end());
    return *this;
  }
  /// Characters of the G-set in GL, as they are coded: one byte each.
  Record& text(const char* characters) {
    for (; *characters != '\0'; ++characters) {
      bytes_.push_back(static_cast<uint8_t>(*characters));
    }
    return *this;
  }
  /// ESC followed by |rest| — a designation, locking shift or C1 control.
  Record& escape(std::initializer_list<uint8_t> rest) {
    bytes_.push_back(0x1B);
    return add(rest);
  }

  const std::vector<uint8_t>& data() const { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
};

NabtsPageSnapshot run(const Record& record) {
  NaplpsInterpreter interpreter;
  return interpreter.run(record.data());
}

/// The blink process still running on map entry |from| at the end of the
/// record, or nullptr.
const NabtsBlinkProcess* running_on(const NabtsPageSnapshot& snapshot,
                                    uint8_t from) {
  for (const NabtsBlinkProcess& process : snapshot.timeline.blinks) {
    if (process.from == from && process.ended_ms == kNabtsNeverMs) {
      return &process;
    }
  }
  return nullptr;
}

/// WAIT with the fixed format byte of Figure 28 (5/12) and |tenths|.
std::vector<uint8_t> wait(std::initializer_list<uint8_t> tenths) {
  std::vector<uint8_t> out{static_cast<uint8_t>(NaplpsPdi::kWait),
                           numeric(0x1C)};
  for (const uint8_t value : tenths) {
    out.push_back(numeric(value));
  }
  return out;
}

////////////////////////////////////////////////////////////////////////////////////////////
// The reset state (T.101 Table II-3)
////////////////////////////////////////////////////////////////////////////////////////////

// Table II-3's default presentation parameters, field by field, for the ones a
// display list can carry: a freshly reset interpreter matches Table II-3 field
// for field.
TEST(NaplpsInterpreter, AFreshInterpreterMatchesTableII3) {
  NaplpsInterpreter interpreter;
  interpreter.run({});
  const NaplpsState& state = interpreter.state();

  // current-text-position: Table II-3 says "lower left corner", which is the
  // corner of the *character field* rather than of the screen. X3.110 says so
  // three times over — §5.3.2.9.3 sends a reset cursor to "its home position
  // (top left character position in the display area)", §6.1.2.6 and §6.1.2.8
  // home CS and APH to "the upper left character position in the display area,
  // in which the top of the character field coincides with the top boundary",
  // and §6.1.6.5(6) numbers NSR's rows from "the upper leftmost character
  // position" — and Table II-3 itself gives the other two data syntaxes an
  // "upper left corner". Reading it as the bottom of the screen puts every
  // record that opens with text and line feeds, which is how the reference
  // ExtraVision service writes, on the bottom row with every line feed clamped.
  EXPECT_DOUBLE_EQ(state.cursor.x, 0.0);
  EXPECT_DOUBLE_EQ(state.cursor.y,
                   kNabtsDisplayAreaHeight - state.text.character_field.dy);

  // current-foreground-colour: colour = white, mode = direct.
  EXPECT_EQ(state.colour.mode(), NabtsColourMode::kDirect);
  EXPECT_EQ(state.colour.drawing_colour(), kNabtsNominalWhite);

  // flash-blink-state: off — on no colour map entry, not merely on the one
  // being drawn in.
  EXPECT_TRUE(state.blinks.empty());

  // basic-char-size-state: dx = 1/40, dy = 1/128 ... the table's own figure for
  // dy is 1/128, but X3.110 §5.3.2.3.9 and §6.2.7.8 both give 5/128 for the
  // default and for NORMAL TEXT, and Table II-3's geometric-control-1-state
  // gives the texture mask — "the default character field size" per §5.3.2.4.5
  // — as 1/40, 5/128. So 5/128 is the value two independent statements agree on
  // and 1/128 is a typo in the table.
  EXPECT_DOUBLE_EQ(state.text.character_field.dx, 1.0 / 40.0);
  EXPECT_DOUBLE_EQ(state.text.character_field.dy, 5.0 / 128.0);

  // geometric-control-1-state: line texture solid, texture pattern solid,
  // texture mask 1/40 by 5/128, highlight off, logical pel 0,0.
  EXPECT_EQ(state.texture.line_texture, NabtsLineTexture::kSolid);
  EXPECT_EQ(state.texture.pattern, NabtsTexturePattern::kSolid);
  EXPECT_DOUBLE_EQ(state.texture.mask_size.dx, 1.0 / 40.0);
  EXPECT_DOUBLE_EQ(state.texture.mask_size.dy, 5.0 / 128.0);
  EXPECT_FALSE(state.texture.highlight);
  EXPECT_DOUBLE_EQ(state.domain.logical_pel.dx, 0.0);
  EXPECT_DOUBLE_EQ(state.domain.logical_pel.dy, 0.0);

  // general-text-state: char rotation 0, char path right, char spacing 1,
  // cursor underscore, interrow single space.
  EXPECT_EQ(state.text.rotation, NabtsCharRotation::kNone);
  EXPECT_EQ(state.text.path, NabtsCharPath::kRight);
  EXPECT_EQ(state.text.intercharacter_spacing, 0u);
  EXPECT_EQ(state.text.interrow_spacing, 0u);
  EXPECT_EQ(state.text.cursor_style, NabtsCursorStyle::kUnderscore);

  // DRCS-definition-state and macro-definition-state: none defined.
  EXPECT_EQ(state.storage_used(), 0u);

  // Macro-Seg-Memory-Limit and DRCS-Memory-Limit: 3072 bytes, shared.
  EXPECT_EQ(kNaplpsSharedStorageBytes, 3072u);

  // Default operand lengths: §5.3.2.2.2 one byte, §5.3.2.2.3 three bytes,
  // §5.3.2.2.4 two-dimensional.
  EXPECT_EQ(state.domain.format.single_value_bytes, 1u);
  EXPECT_EQ(state.domain.format.multi_value_bytes, 3u);
  EXPECT_FALSE(state.domain.format.three_dimensional);
}

TEST(NaplpsInterpreter, AnEmptyRecordDrawsNothing) {
  const NabtsPageSnapshot snapshot = run(Record{});
  EXPECT_TRUE(snapshot.empty());
}

////////////////////////////////////////////////////////////////////////////////////////////
// Geometric primitives
////////////////////////////////////////////////////////////////////////////////////////////

// §5.3.3.1.4: POINT (Absolute, Visible) "sets the drawing point to the absolute
// coordinates specified and draws a point".
TEST(NaplpsInterpreter, DrawsAnAbsolutePoint) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      // One-byte multi-value operands, so a coordinate is one byte.
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b010, 0b011));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const NabtsPrimitive& point = snapshot.primitives[0];
  EXPECT_EQ(point.kind, NabtsPrimitiveKind::kPoint);
  EXPECT_DOUBLE_EQ(point.origin.x, 0.5);   // 010 over three bits
  EXPECT_DOUBLE_EQ(point.origin.y, 0.75);  // 011
}

// §5.3.3.1.2: POINT SET (Absolute, Invisible) "sets the drawing point ... A
// point is not drawn."
TEST(NaplpsInterpreter, PointSetMovesTheDrawingPointWithoutDrawing) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .byte(coord1(0b010, 0b010));

  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot snapshot = interpreter.run(record.data());
  EXPECT_TRUE(snapshot.primitives.empty());
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.x, 0.5);
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.y, 0.5);
}

// §5.3.2.2.5 zero-fills a short operand, and a POINT with no data at all is
// the shortest: the absolute forms go to the origin, the relative forms stay
// where they are, and the visible forms draw there. An encoder can open every
// PDI section with an empty POINT SET (Absolute) and draw on from the origin;
// the TTX 650 and TU-2000x firmware both read all four forms so.
TEST(NaplpsInterpreter, APointWithNoDataIsZeroFilled) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .byte(coord1(0b010, 0b010))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointRel))     // a dot, here
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetRel))  // no move
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))     // a dot, origin
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .byte(coord1(0b010, 0b010))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))  // back to origin
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b001, 0b001));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  EXPECT_EQ(snapshot.primitives[0].kind, NabtsPrimitiveKind::kPoint);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x, 0.5);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.y, 0.5);
  EXPECT_EQ(snapshot.primitives[1].kind, NabtsPrimitiveKind::kPoint);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x, 0.0);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.y, 0.0);
  EXPECT_EQ(snapshot.primitives[2].kind, NabtsPrimitiveKind::kRectangle);
  EXPECT_DOUBLE_EQ(snapshot.primitives[2].origin.x, 0.0);
  EXPECT_DOUBLE_EQ(snapshot.primitives[2].origin.y, 0.0);
}

// LINE (Absolute) with no data, zero-filled the same way, is a line to the
// origin, which both receivers of the day draw. LINE (Relative) and the SET
// forms with none stay null operations: the TU-2000x draws nothing for them.
TEST(NaplpsInterpreter, AnAbsoluteLineWithNoDataRunsToTheOrigin) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .byte(coord1(0b010, 0b010))
      .byte(static_cast<uint8_t>(NaplpsPdi::kLineRel))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetLineAbs))
      .byte(static_cast<uint8_t>(NaplpsPdi::kLineAbs));

  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot snapshot = interpreter.run(record.data());
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const NabtsPrimitive& line = snapshot.primitives[0];
  EXPECT_EQ(line.kind, NabtsPrimitiveKind::kLine);
  ASSERT_EQ(line.points.size(), 2u);
  EXPECT_DOUBLE_EQ(line.points[0].x, 0.5);
  EXPECT_DOUBLE_EQ(line.points[0].y, 0.5);
  EXPECT_DOUBLE_EQ(line.points[1].x, 0.0);
  EXPECT_DOUBLE_EQ(line.points[1].y, 0.0);
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.x, 0.0);
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.y, 0.0);
}

// A SET form whose start point has no end after it has the end zero-filled,
// as the 1993 Addendum to T.101 Annex D reads every absent operand: SET & LINE
// (ABSOLUTE) with its start alone is a line from it to the origin, which both
// receivers of the day draw, and the relative form a line of no length that
// leaves the drawing point on the start.
TEST(NaplpsInterpreter, ASetLineWithItsStartAloneHasItsEndZeroFilled) {
  Record absolute;
  absolute.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetLineAbs))
      .byte(coord1(0b010, 0b001))
      .text_mode();
  NaplpsInterpreter interpreter;
  NabtsPageSnapshot snapshot = interpreter.run(absolute.data());
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  ASSERT_EQ(snapshot.primitives[0].points.size(), 2u);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].points[0].x, 0.5);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].points[0].y, 0.25);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].points[1].x, 0.0);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].points[1].y, 0.0);
  EXPECT_EQ(snapshot.diagnostics.truncated_pdis, 0u);

  Record relative;
  relative.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetLineRel))
      .byte(coord1(0b010, 0b001))
      .text_mode();
  snapshot = interpreter.run(relative.data());
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].points[1].x, 0.5);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].points[1].y, 0.25);
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.x, 0.5);
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.y, 0.25);
}

// §6.1.2.6 clears to nominal black in colour mode 1, which X3.110 defines as
// "the color that is at color map address zero in color modes 1 and 2" — so the
// cleared ground is drawn through entry 0, and a later write to entry 0
// recolours it, as both receivers of the day show. Mode 0 clears to plain
// black.
TEST(NaplpsInterpreter, AClearInColourModeOneIsMapEntryZero) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))  // mode 1, entry 3
      .byte(0x0C);              // CS

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].kind, NabtsPrimitiveKind::kRectangle);
  EXPECT_EQ(snapshot.primitives[0].colour_map_address, 0);
  EXPECT_EQ(snapshot.primitives[0].colour, snapshot.colour_map[0]);

  Record mode_zero;
  mode_zero.pdi().byte(0x0C);
  EXPECT_TRUE(run(mode_zero).primitives.empty());
}

// A polygon the page record ends inside, with no code after it to end the
// command, is not drawn: both receivers of the day wait for that code before
// closing a figure only its last word closes. Ended by a code, it is drawn.
TEST(NaplpsInterpreter, APolygonTheRecordEndsInsideIsNotDrawn) {
  const auto polygon = [](bool ended) {
    Record record;
    record.pdi()
        .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
        .byte(numeric(0b000000))
        .byte(static_cast<uint8_t>(NaplpsPdi::kPolyFilled))
        .add(
            {coord1(0b001, 0b000), coord1(0b000, 0b001), coord1(0b111, 0b000)});
    if (ended) {
      record.text_mode();
    }
    return run(record);
  };

  const NabtsPageSnapshot cut = polygon(false);
  EXPECT_TRUE(cut.primitives.empty());
  EXPECT_EQ(cut.diagnostics.truncated_pdis, 1u);
  EXPECT_EQ(polygon(true).primitives.size(), 1u);
}

// A partial word at the very end of the page record is not drawn — both
// receivers of the day draw the words that arrived whole — while a partial
// word a code ends is zero-filled (§5.3.2.2.5), as they zero-fill it too.
TEST(NaplpsInterpreter, APartialWordTheRecordEndsInIsNotDrawn) {
  const auto points = [](bool ended) {
    Record record;
    record.pdi()
        .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
        .byte(numeric(0b000100))  // two-byte coordinate words
        .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
        .add({numeric(0b010010), numeric(0b000000), numeric(0b001001)});
    if (ended) {
      record.text_mode();
    }
    return run(record);
  };

  EXPECT_EQ(points(false).primitives.size(), 1u);
  EXPECT_EQ(points(true).primitives.size(), 2u);
}

// §5.3.3.2.2: LINE (Absolute) — "The start point is the current drawing point.
// The end point is specified in absolute coordinates." And §5.3.3.2.1: "At the
// completion of drawing a line, the drawing point is coincident with the end
// point."
TEST(NaplpsInterpreter, DrawsALineFromTheDrawingPointAndLeavesItAtTheEnd) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .byte(coord1(0b001, 0b001))
      .byte(static_cast<uint8_t>(NaplpsPdi::kLineAbs))
      .byte(coord1(0b011, 0b010));

  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot snapshot = interpreter.run(record.data());
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const NabtsPrimitive& line = snapshot.primitives[0];
  EXPECT_EQ(line.kind, NabtsPrimitiveKind::kLine);
  ASSERT_EQ(line.points.size(), 2u);
  EXPECT_DOUBLE_EQ(line.points[0].x, 0.25);
  EXPECT_DOUBLE_EQ(line.points[1].x, 0.75);
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.x, 0.75);
}

// §5.3.3.4.2: RECTANGLE (Outlined) takes the width and height as a coordinate
// word; §5.3.3.4.1 leaves the drawing point "altered in x only".
TEST(NaplpsInterpreter, DrawsARectangleAndAdvancesTheDrawingPointInXOnly) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b010, 0b001));

  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot snapshot = interpreter.run(record.data());
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const NabtsPrimitive& rect = snapshot.primitives[0];
  EXPECT_EQ(rect.kind, NabtsPrimitiveKind::kRectangle);
  EXPECT_TRUE(rect.filled);
  EXPECT_DOUBLE_EQ(rect.size.dx, 0.5);
  EXPECT_DOUBLE_EQ(rect.size.dy, 0.25);
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.x, 0.5);
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.y, 0.0);
}

// §5.3.3.5.1: a polygon's vertices are relative displacements from the previous
// vertex, with "implicit closure between the start point and the last vertex".
TEST(NaplpsInterpreter, DrawsAPolygonFromRelativeVertices) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPolyFilled))
      .byte(coord1(0b010, 0b000))  // +0,5 in x
      .byte(coord1(0b000, 0b010))  // +0,5 in y
      .byte(coord1(0b110, 0b000))  // -0,5 in x
      .text_mode();                // the code that ends the command

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const NabtsPrimitive& poly = snapshot.primitives[0];
  EXPECT_EQ(poly.kind, NabtsPrimitiveKind::kPolygon);
  EXPECT_TRUE(poly.filled);
  // Start plus three vertices; the closure back to the start is implicit and so
  // is not a fourth point.
  ASSERT_EQ(poly.points.size(), 4u);
  EXPECT_DOUBLE_EQ(poly.points[1].x, 0.5);
  EXPECT_DOUBLE_EQ(poly.points[2].y, 0.5);
  EXPECT_DOUBLE_EQ(poly.points[3].x, 0.0);
}

// A polygon of one displacement encloses nothing, but §5.3.2.2.6's pel covers
// at least a pixel, so it is still drawn — as the line out and back — and the
// drawing point still returns to the start (§5.3.3.5.1).
TEST(NaplpsInterpreter, APolygonOfOneDisplacementIsStillDrawn) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetPolyOutlined))
      .add(coord3(0.25, 0.25))
      .add(coord3(0.125, 0.0))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointRel))
      .add(coord3(0.0, 0.0));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_EQ(snapshot.primitives[0].kind, NabtsPrimitiveKind::kPolygon);
  ASSERT_EQ(snapshot.primitives[0].points.size(), 2u);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].points[1].x, 0.375);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x, 0.25);
  EXPECT_EQ(snapshot.diagnostics.truncated_pdis, 0u);
}

// §5.3.3.6.4, Table 16: after the step size, each byte carries three two-bit
// moves read b6 to b1 — 01 steps dx, 10 steps dy, 11 steps both — and every
// step with the draw flag on (as it starts) extends the line. The drawing point
// is left where the last step took it.
TEST(NaplpsInterpreter, IncrementalLineStepsAsTable16Says) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .add(coord3(0.25, 0.25))
      .byte(static_cast<uint8_t>(NaplpsPdi::kIncrLine))
      .add(coord3(1.0 / 64, 1.0 / 64))
      .byte(numeric(0b010110))  // +dx, +dx, +dy
      // A visible POINT with no displacement marks the drawing point.
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointRel))
      .add(coord3(0.0, 0.0));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  const NabtsPrimitive& line = snapshot.primitives[0];
  EXPECT_EQ(line.kind, NabtsPrimitiveKind::kLine);
  ASSERT_EQ(line.points.size(), 4u);
  EXPECT_DOUBLE_EQ(line.points[0].x, 0.25);
  EXPECT_DOUBLE_EQ(line.points[1].x, 0.25 + 1.0 / 64);
  EXPECT_DOUBLE_EQ(line.points[2].x, 0.25 + 2.0 / 64);
  EXPECT_DOUBLE_EQ(line.points[2].y, 0.25);
  EXPECT_DOUBLE_EQ(line.points[3].y, 0.25 + 1.0 / 64);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x, 0.25 + 2.0 / 64);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.y, 0.25 + 1.0 / 64);
}

// Table 16's 00 makes the next move a Table 17 instruction; 00 there toggles
// the draw flag, and the move after it is a step again. Steps taken with the
// flag off move the drawing point without drawing, so the line breaks into
// runs.
TEST(NaplpsInterpreter, IncrementalLineBreaksWhereTheDrawFlagIsOff) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .add(coord3(0.25, 0.25))
      .byte(static_cast<uint8_t>(NaplpsPdi::kIncrLine))
      .add(coord3(1.0 / 64, 1.0 / 64))
      .byte(numeric(0b010000))   // +dx drawn, then toggle off
      .byte(numeric(0b010000))   // +dx undrawn, then toggle on
      .byte(numeric(0b010101));  // three drawn +dx steps

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  ASSERT_EQ(snapshot.primitives[0].points.size(), 2u);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].points[1].x, 0.25 + 1.0 / 64);
  ASSERT_EQ(snapshot.primitives[1].points.size(), 4u);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].points[0].x, 0.25 + 2.0 / 64);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].points[3].x, 0.25 + 5.0 / 64);
}

// Table 17's 01 changes the sign of dx for the steps that follow.
TEST(NaplpsInterpreter, IncrementalLineChangesTheSignOfItsStep) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .add(coord3(0.25, 0.25))
      .byte(static_cast<uint8_t>(NaplpsPdi::kIncrLine))
      .add(coord3(1.0 / 64, 1.0 / 64))
      .byte(numeric(0b000101));  // modify: negate dx; then step dx

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  ASSERT_EQ(snapshot.primitives[0].points.size(), 2u);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].points[1].x, 0.25 - 1.0 / 64);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].points[1].y, 0.25);
}

// §5.3.3.6.5: the polygon's draw flag is always on — a toggle is a null and the
// move after it is still a modify instruction — the outline closes back on the
// start, and the drawing point is left there.
TEST(NaplpsInterpreter,
     IncrementalPolygonIgnoresTheToggleAndReturnsToItsStart) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .add(coord3(0.25, 0.25))
      .byte(static_cast<uint8_t>(NaplpsPdi::kIncrPolyFilled))
      .add(coord3(1.0 / 64, 1.0 / 64))
      .byte(numeric(0b000001))  // modify: toggle (a null), negate dx
      .byte(numeric(0b011001))  // -dx, +dy, -dx
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointRel))
      .add(coord3(0.0, 0.0));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  const NabtsPrimitive& poly = snapshot.primitives[0];
  EXPECT_EQ(poly.kind, NabtsPrimitiveKind::kPolygon);
  EXPECT_TRUE(poly.filled);
  ASSERT_EQ(poly.points.size(), 4u);
  EXPECT_DOUBLE_EQ(poly.points[1].x, 0.25 - 1.0 / 64);
  EXPECT_DOUBLE_EQ(poly.points[2].y, 0.25 + 1.0 / 64);
  EXPECT_DOUBLE_EQ(poly.points[3].x, 0.25 - 2.0 / 64);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x, 0.25);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.y, 0.25);
}

// "The limit of the number of coordinates for an INCREMENTAL POLYGON command is
// the same as for the POLYGON command, and is 256" (T.101 Annex D Addendum,
// clause 8; Table D1 item 4). One with more keeps its first 256, as the Norpak
// TTX 650 does and as POLYGON does here.
TEST(NaplpsInterpreter, IncrementalPolygonKeepsItsFirst256Vertices) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .add(coord3(0.25, 0.25))
      .byte(static_cast<uint8_t>(NaplpsPdi::kIncrPolyFilled))
      .add(coord3(1.0 / 256, 1.0 / 256));
  for (int i = 0; i < 100; ++i) {
    record.byte(numeric(0b011001));  // +dx, +dy, +dx: 300 steps in all
  }
  record.text_mode();

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].kind, NabtsPrimitiveKind::kPolygon);
  EXPECT_EQ(snapshot.primitives[0].points.size(), 256u);
}

// §5.3.3.3.1: "an arc is drawn from a start point to an end point through an
// intermediate point on the arc", the intermediate relative to the start and
// the end relative to the intermediate.
TEST(NaplpsInterpreter, DrawsAnArcThroughThreeControlPoints) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kArcOutlined))
      .byte(coord1(0b001, 0b001))
      .byte(coord1(0b001, 0b111))
      .text_mode();  // the code that ends the command

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const NabtsPrimitive& arc = snapshot.primitives[0];
  EXPECT_EQ(arc.kind, NabtsPrimitiveKind::kArc);
  EXPECT_FALSE(arc.filled);
  ASSERT_EQ(arc.points.size(), 3u) << "start, intermediate, end";
}

// §5.3.2.2.5: "If an operand following an opcode is longer than the length
// previously specified ... it is taken as an indication to repeat the execution
// of the opcode with the subsequent numeric data taken as new operands."
TEST(NaplpsInterpreter, RepeatsAnOpcodeWhoseOperandRunIsLong) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      .byte(coord1(0b010, 0b010))
      .byte(coord1(0b011, 0b011));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x, 0.25);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x, 0.5);
  EXPECT_DOUBLE_EQ(snapshot.primitives[2].origin.x, 0.75);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Attributes
////////////////////////////////////////////////////////////////////////////////////////////

// §5.3.2.2.2 Table 4 and §5.3.2.2.3 Table 5, and the note in §5.3.2.2.6 that
// the new multi-value length "applies to the multi-value logical pel size
// operand of that DOMAIN command".
TEST(NaplpsInterpreter, DomainSetsTheOperandLengthsAndItsOwnPelUsesTheNewOne) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      // b2 b1 = 01 → two-byte single values; b5 b4 b3 = 000 → one-byte
      // multi-values; b6 = 0 → two-dimensional.
      .byte(numeric(0b000001))
      // Read on the new one-byte length: dx = 001 = +0,25, dy = 001.
      .byte(coord1(0b001, 0b001));

  NaplpsInterpreter interpreter;
  interpreter.run(record.data());
  const NaplpsState& state = interpreter.state();
  EXPECT_EQ(state.domain.format.single_value_bytes, 2u);
  EXPECT_EQ(state.domain.format.multi_value_bytes, 1u);
  EXPECT_DOUBLE_EQ(state.domain.logical_pel.dx, 0.25);
  EXPECT_DOUBLE_EQ(state.domain.logical_pel.dy, 0.25);
}

// Game pages send DOMAIN 4/0 — one-byte multi-values by Table 5 — with a
// three-byte pel, and go on in three-byte coordinates: Go's star points land on
// its board's 4-4 points only read so. The pel's own length is the one meant.
TEST(NaplpsInterpreter, ADomainWhosePelArrivesInThreeBytesKeepsThreeByteWords) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .add(coord3(3.0 / 256.0, 3.0 / 256.0))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .add(coord3(40.0 / 256.0, 40.0 / 256.0));

  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot snapshot = interpreter.run(record.data());
  EXPECT_EQ(interpreter.state().domain.format.multi_value_bytes, 3u);
  EXPECT_DOUBLE_EQ(interpreter.state().domain.logical_pel.dx, 3.0 / 256.0);
  ASSERT_EQ(snapshot.primitives.size(), 1u)
      << "one point, not three one-byte ones";
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x, 40.0 / 256.0);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.y, 40.0 / 256.0);
}

// §5.3.2.4: the texture attributes are inherited by every subsequent primitive,
// which is what makes the display list self-contained.
TEST(NaplpsInterpreter, APrimitiveCarriesTheAttributesInForceWhenItRan) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kTexture))
      // b2 b1 = 10 dashed; b3 = 1 highlight; b6 b5 b4 = 011 cross-hatching.
      .byte(numeric(0b011110))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b001, 0b001))
      // Then change the texture and draw again.
      .byte(static_cast<uint8_t>(NaplpsPdi::kTexture))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b001, 0b001));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_EQ(snapshot.primitives[0].line_texture, NabtsLineTexture::kDashed);
  EXPECT_TRUE(snapshot.primitives[0].highlighted);
  EXPECT_EQ(snapshot.primitives[0].texture_pattern,
            NabtsTexturePattern::kCrossHatch);
  // The second inherited the new state, not the old — so the first kept its
  // own.
  EXPECT_EQ(snapshot.primitives[1].line_texture, NabtsLineTexture::kSolid);
  EXPECT_FALSE(snapshot.primitives[1].highlighted);
}

// §5.3.2.6: no operand selects colour mode 0, one selects mode 1, two select
// mode 2 — and in modes 1 and 2 a SET COLOR writes the map at the selected
// address, which a later primitive then draws in.
TEST(NaplpsInterpreter, AColourMapWriteIsVisibleToTheNextPrimitive) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      // SELECT COLOR with one operand: mode 1, drawing address from the high
      // four bits of the one-byte operand — 0011xx → address 3.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      // SET COLOR writes entry 3. One byte gives two bits per gun: GRB = 100,
      // GRB = 000 → green 10, red 10, blue 00, i.e. 2 of the 3 a two-bit
      // operand can express.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b110000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].colour_mode, NabtsColourMode::kMapped);
  // §5.3.2.5.1: "For each primary, the maximum color fraction attainable, given
  // the number of bits specified in the color value operand, shall be
  // interpreted as full intensity and intermediate values shall be equally
  // distributed between zero and full intensity." Over two bits at four bits
  // a gun that is 0, 5, 10, 15 — so 2 of 3 is 10 of 15, not the 8 a
  // shift-and-zero-fill would give. The clause matters: zero-filling makes
  // white unreachable in a one-byte operand, and services set white with
  // exactly one byte.
  EXPECT_EQ(snapshot.primitives[0].colour.green, 10u);
  EXPECT_EQ(snapshot.primitives[0].colour.red, 10u);
  EXPECT_EQ(snapshot.primitives[0].colour.blue, 0u);
  // And the map the renderer is handed carries it, because §5.3.2.5 makes a map
  // change retroactive for every pixel already pointing at that entry.
  EXPECT_EQ(snapshot.colour_map[3].green, 10u);
}

// The regression the ExtraVision logo exposed. DOMAIN may declare a multi-value
// length of three bytes and the service still send SET COLOR one byte at a
// time; §5.3.2.5.1 zero-fills the rest. Requiring the declared length dropped
// the write silently, so the CBS eye was drawn in whatever colour came before
// it — the page background — and vanished.
TEST(NaplpsInterpreter, AShortColourOperandStillSetsTheColour) {
  Record record;
  record
      .pdi()
      // DOMAIN with a three-byte multi-value length.
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000010))
      // A one-byte SET COLOR: all six payload bits set is full intensity.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b111111))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].colour, kNabtsNominalWhite)
      << "a one-byte SET COLOR was dropped, so the primitive kept the previous "
         "colour";
}

// The same clause from the other end: a colour operand longer than the map can
// hold "is truncated and only the most significant bits are used".
TEST(NaplpsInterpreter, ALongColourOperandIsTruncatedToTheMostSignificantBits) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000010))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      // Three bytes: green takes b6 and b3 of each, so 1,0 1,0 1,0 = 0b101010
      // over six bits, truncated to its top four = 0b1010 = 10.
      .byte(numeric(0b100000))
      .byte(numeric(0b100000))
      .byte(numeric(0b100000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].colour.green, 10u);
  EXPECT_EQ(snapshot.primitives[0].colour.red, 0u);
  EXPECT_EQ(snapshot.primitives[0].colour.blue, 0u);
}

// §5.3.2.2.5 completes a short operand with trailing zero bits, so a SELECT
// COLOR whose one address arrived short of a whole single-value word is still
// one operand: colour mode 1, at the address the bits that did arrive name.
TEST(NaplpsInterpreter, SelectColourCountsAShortWordAsAnOperand) {
  Record record;
  record
      .pdi()
      // DOMAIN: two-byte single values, three-byte multi-values.
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b001001))
      // One byte of a two-byte address: 1010 leads, so address 10 once the
      // missing byte is zero-filled and the address read from the top bits.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b101000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .add(coord3(0.25, 0.25));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].colour_mode, NabtsColourMode::kMapped);
  EXPECT_EQ(snapshot.primitives[0].colour_map_address, 10);
}

// The same with one whole word and half of a second: two operands, mode 2.
TEST(NaplpsInterpreter, SelectColourCountsAShortSecondWordAsAnOperand) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b001001))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b101000))
      .byte(numeric(0b000000))
      .byte(numeric(0b011000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .add(coord3(0.25, 0.25));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].colour_mode,
            NabtsColourMode::kMappedWithBackground);
  EXPECT_EQ(snapshot.primitives[0].colour_map_address, 10);
  EXPECT_EQ(snapshot.primitives[0].background_map_address, 6);
}

// §5.3.2.5.1: "If no operand follows a SET COLOR opcode, the transparent color
// is set", which in a captioning application is what lets the video through.
TEST(NaplpsInterpreter, SetColourWithNoOperandSelectsTransparent) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_TRUE(snapshot.primitives[0].colour.transparent);
}

// §5.3.2.9: RESET is selective, and byte 2 b6 is the bit that clears the DRCS
// set. A RESET with no operands is "as if it had been sent with bits b6 to b1
// in both bytes set equal to 0" — that is, it does nothing.
TEST(NaplpsInterpreter, ResetWithNoOperandsChangesNothing) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000001))  // two-byte single values
      .byte(static_cast<uint8_t>(NaplpsPdi::kReset));

  NaplpsInterpreter interpreter;
  interpreter.run(record.data());
  // The DOMAIN change survived, because RESET was told to reset nothing.
  EXPECT_EQ(interpreter.state().domain.format.single_value_bytes, 2u);
}

TEST(NaplpsInterpreter, ResetByte1Bit1RestoresTheDomainDefaults) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000001))
      .byte(static_cast<uint8_t>(NaplpsPdi::kReset))
      .byte(numeric(0b000001));  // byte 1 b1 = 1

  NaplpsInterpreter interpreter;
  interpreter.run(record.data());
  EXPECT_EQ(interpreter.state().domain.format.single_value_bytes, 1u);
}

// §5.3.2.9.2 Table 14: "set color map to default colors, and set the in-use
// drawing color to white". The default map already holds white, so the drawing
// colour points at it; writing white over entry 0 instead would lose the
// nominal black the reset has just restored. NBC Teletext opens every page with
// this RESET and draws its outlines, faces and shadows in entry 0, all of which
// came out white (issue #313).
TEST(NaplpsInterpreter, ResetIntoColourMode1KeepsTheDefaultMapsBlack) {
  for (const uint8_t colour_action : {0b000100, 0b000110}) {
    Record record;
    record.pdi()
        .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
        .byte(numeric(0b000000))
        // Byte 1 b3 b2 = 10 from colour mode 0, which is the same as 11.
        .byte(static_cast<uint8_t>(NaplpsPdi::kReset))
        .byte(numeric(colour_action))
        .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
        .byte(coord1(0b001, 0b001))
        .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
        .byte(numeric(0b000000))  // address 0
        .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
        .byte(coord1(0b010, 0b010));

    const NabtsPageSnapshot snapshot = run(record);
    ASSERT_EQ(snapshot.primitives.size(), 2u);
    EXPECT_EQ(snapshot.colour_map[0], kNabtsNominalBlack)
        << "the reset wrote over the default map";
    EXPECT_EQ(snapshot.primitives[0].colour_mode, NabtsColourMode::kMapped);
    EXPECT_EQ(snapshot.primitives[0].colour, kNabtsNominalWhite)
        << "the in-use drawing colour is white";
    EXPECT_NE(snapshot.primitives[0].colour_map_address, 0);
    EXPECT_EQ(snapshot.primitives[1].colour, kNabtsNominalBlack);
  }
}

// §5.3.2.9.2 Table 15: clearing the display area means everything drawn before
// it is gone, which for a display list means the primitives are dropped.
TEST(NaplpsInterpreter, ResetClearingTheDisplayAreaDropsWhatWasDrawn) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      .byte(static_cast<uint8_t>(NaplpsPdi::kReset))
      // Byte 1 b6 b5 b4 = 001: display area to nominal black.
      .byte(numeric(0b001000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b010, 0b010));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u) << "the pre-clear point survived";
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x, 0.5);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Text
////////////////////////////////////////////////////////////////////////////////////////////

// A character from the primary set is emitted at the cursor in the current
// character field, and the cursor advances along the character path
// (§5.3.2.3.3).
TEST(NaplpsInterpreter, EmitsCharactersAndAdvancesAlongTheCharacterPath) {
  Record record;
  record.text_mode().add({'A', 'B', 'C'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(snapshot.primitives[i].kind, NabtsPrimitiveKind::kCharacter);
    EXPECT_EQ(snapshot.primitives[i].repertoire,
              NabtsPrimitive::Repertoire::kPrimary);
    EXPECT_DOUBLE_EQ(snapshot.primitives[i].size.dx, 1.0 / 40.0);
  }
  EXPECT_EQ(snapshot.primitives[0].character, 'A');
  EXPECT_EQ(snapshot.primitives[2].character, 'C');
  // Default intercharacter spacing is 1, so the fields abut (§5.3.2.3.4).
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x, 1.0 / 40.0);
  EXPECT_DOUBLE_EQ(snapshot.primitives[2].origin.x, 2.0 / 40.0);
}

// §4.4: with a 94-position set invoked, 7/15 is DELETE, which "is executed as
// a null operation": nothing drawn, and the next character takes the field
// DELETE did not use. (The TTX 650 does the same.)
TEST(NaplpsInterpreter, DeleteIsANullOperation) {
  Record record;
  record.text_mode().add({'A', 0x7F, 'B'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_EQ(snapshot.primitives[0].character, 'A');
  EXPECT_EQ(snapshot.primitives[1].character, 'B');
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x, 1.0 / 40.0);
}

////////////////////////////////////////////////////////////////////////////////////////////
// The format effectors (§6.1.2)
////////////////////////////////////////////////////////////////////////////////////////////

// The regression this section exists for. §6.1.2 defines four *different*
// movements relative to the character path, and an implementation that treats
// them all as "advance along the path" draws every row of a page on top of the
// last: the ExtraVision news pages came out with each line overprinting the one
// before it, one character out of step.
TEST(NaplpsInterpreter, APRThenAPDStartsANewRowRatherThanOverprinting) {
  // CS first: the reset cursor is at the lower left corner (T.101 Table II-3),
  // where there is no room below to move into. A record that draws rows homes
  // the cursor first, which is what CS does (§6.1.2.6).
  Record record;
  record.text_mode()
      .byte(0x0C)  // CS — home to the top left of the display area
      .add({'A', 'B'})
      .byte(0x0D)  // APR — back to the first character position of the row
      .byte(0x0A)  // APD — down one row
      .add({'C'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  const NabtsPoint& first = snapshot.primitives[0].origin;
  const NabtsPoint& third = snapshot.primitives[2].origin;
  // Back to the left edge...
  EXPECT_DOUBLE_EQ(third.x, first.x);
  // ...and one row down, which is the whole point.
  EXPECT_LT(third.y, first.y);
  EXPECT_DOUBLE_EQ(third.y, first.y - 5.0 / 128.0);
}

// §6.1.2.3: APD moves "a distance equal to the interrow space lying
// perpendicular to the character path in a direction perpendicular to the
// character path (-90 degrees)". Default interrow spacing is 1, so that is one
// character field height.
TEST(NaplpsInterpreter, APDMovesDownOneRowAndLeavesTheColumnAlone) {
  Record record;
  record.text_mode().byte(0x0C).add({'A'}).byte(0x0A).add({'B'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  // The character before it advanced one field along the path; APD adds no
  // horizontal movement of its own.
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x, 1.0 / 40.0);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.y,
                   snapshot.primitives[0].origin.y - 5.0 / 128.0);
}

// §6.1.2.5: APU is the same movement 180 degrees round.
TEST(NaplpsInterpreter, APUMovesUpOneRow) {
  Record record;
  record.text_mode()
      .byte(0x0C)
      .add({'A'})
      .byte(0x0A)  // down, so there is room to come back up
      .byte(0x0B)  // APU
      .add({'B'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.y,
                   snapshot.primitives[0].origin.y);
}

// §6.1.2.1: APB moves back along the character path, so the character after it
// lands on the one before — which is how a record overstrikes.
TEST(NaplpsInterpreter, APBMovesBackAlongTheCharacterPath) {
  Record record;
  record.text_mode().byte(0x0C).add({'A', 'B'}).byte(0x08).add({'C'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  // 'B' was drawn at one field in and the cursor left at two; APB puts it back
  // to one, so 'C' lands on 'B'.
  EXPECT_DOUBLE_EQ(snapshot.primitives[2].origin.x,
                   snapshot.primitives[1].origin.x);
  EXPECT_DOUBLE_EQ(snapshot.primitives[2].origin.y,
                   snapshot.primitives[1].origin.y);
}

// The regression the ExtraVision service exposed. A record that simply writes
// text with CR and LF — no CS, no APS, no NSR — must run *down* the screen from
// the top. Homing the cursor at the bottom left clamps every line feed, and the
// whole page piles onto one row.
TEST(NaplpsInterpreter, ARecordThatOpensWithTextStartsAtTheTopOfTheScreen) {
  Record record;
  record.text_mode()
      .add({'A'})
      .add({0x0D, 0x0A})  // APR APD — carriage return, line feed
      .add({'B'})
      .add({0x0D, 0x0A})
      .add({'C'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  const double field = snapshot.primitives[0].size.dy;
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.y,
                   kNabtsDisplayAreaHeight - field);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.y,
                   kNabtsDisplayAreaHeight - 2.0 * field);
  EXPECT_DOUBLE_EQ(snapshot.primitives[2].origin.y,
                   kNabtsDisplayAreaHeight - 3.0 * field);
}

// §5.3.2.3.6: "If an explicit APR APD (or APD APR) sequence is received after
// an automatic APR APD is executed but before the character field origin is
// moved, aligned, or set by any other received command or sequence, the
// explicit APR APD (or APD APR) sequence shall be executed as a null
// operation." A service writes each line flush to its field and ends it with
// CR LF; without the suppression every exactly-full line gains a blank row and
// the rest of the page lands on top of its positioned elements — which is how
// the NBC "BASIC FITNESS" page rendered before this test existed.
TEST(NaplpsInterpreter, AnExplicitCrLfAfterAnAutomaticWrapIsANullOperation) {
  Record record;
  record.text_mode().byte(0x0C);  // CS — home, top of the display area
  // Exactly fill the row: 40 characters of the default 1/40 field.
  for (int i = 0; i < 40; ++i) {
    record.byte('A');
  }
  record.add({0x0D, 0x0A});  // explicit APR APD, straight after the wrap
  record.byte('B');

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 41u);
  const NabtsPrimitive& first = snapshot.primitives[0];
  const NabtsPrimitive& next_line = snapshot.primitives[40];
  EXPECT_EQ(next_line.character, 'B');
  EXPECT_DOUBLE_EQ(next_line.origin.x, 0.0);
  // One row below the first line, not two.
  EXPECT_DOUBLE_EQ(next_line.origin.y, first.origin.y - 5.0 / 128.0);
}

// The services in hand end a line by carriage return, setting the indent, then
// line feed — so the explicit APR APD §5.3.2.3.6 makes null has a POINT SET
// between its halves. Closing the window on that, as the clause's "moved,
// aligned, or set by any other received command" reads on its face, double
// spaces every line a service indents: over the reference ExtraVision
// recording it displaced 108 lines of 135 wrapped ones, most of them out of the
// panel they belonged in. A command setting the margin of the row the cursor is
// already on has not taken the row advance the window stands for.
TEST(NaplpsInterpreter, AnIndentBetweenTheCrAndTheLfDoesNotEndTheSuppression) {
  // CS homes the cursor a character's height below the top of the display area,
  // and the automatic wrap takes it one row further down.
  constexpr double kHomeRow = kNabtsDisplayAreaHeight - 5.0 / 128.0;
  constexpr double kWrappedRow = kHomeRow - 5.0 / 128.0;

  Record record;
  record.text_mode().byte(0x0C);
  for (int i = 0; i < 40; ++i) {
    record.byte('A');
  }
  record.byte(0x0D);  // APR, the first half of the explicit pair
  // POINT SET absolute to the line's indent, on the row the wrap moved to —
  // which is one row below the top of the display area.
  record.pdi().byte(0x24);
  for (const uint8_t operand : coord3(0.125, kWrappedRow)) {
    record.byte(operand);
  }
  record.text_mode();
  record.byte(0x0A);  // APD, the second half
  record.byte('B');

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 41u);
  const NabtsPrimitive& first = snapshot.primitives[0];
  const NabtsPrimitive& next_line = snapshot.primitives[40];
  EXPECT_DOUBLE_EQ(first.origin.y, kHomeRow);
  EXPECT_EQ(next_line.character, 'B');
  // At the indent, one row below the first line rather than two.
  EXPECT_DOUBLE_EQ(next_line.origin.x, 0.125);
  EXPECT_DOUBLE_EQ(next_line.origin.y, kWrappedRow);
}

// The suppression window closes the moment the cursor moves by anything else:
// a CR LF later in the stream is a real one.
TEST(NaplpsInterpreter, TheWrapSuppressionEndsOnceTheCursorMovesAgain) {
  Record record;
  record.text_mode().byte(0x0C);
  for (int i = 0; i < 40; ++i) {
    record.byte('A');
  }
  // A character on the wrapped line moves the cursor, so the CR LF after it is
  // explicit and real.
  record.byte('B').add({0x0D, 0x0A}).byte('C');

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 42u);
  const NabtsPrimitive& b = snapshot.primitives[40];
  const NabtsPrimitive& c = snapshot.primitives[41];
  EXPECT_EQ(c.character, 'C');
  EXPECT_DOUBLE_EQ(c.origin.y, b.origin.y - 5.0 / 128.0);
}

// §6.1.6.5(6): NSR "can be used as an alternative means to position the
// cursor" — the two bytes after it are a row and a column when both come from
// columns 4 to 7, and they are consumed rather than drawn. The reference
// ExtraVision records open with exactly this, and executing the pair as text
// left the cursor wherever it was and put two stray glyphs on the page.
TEST(NaplpsInterpreter, NSRConsumesItsRowAndColumnAddress) {
  Record record;
  // NSR, row 2, column 3 — both bytes from column 4 of the in-use table.
  record.text_mode().byte(0x1F).byte(0x42).byte(0x43).add({'X'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u) << "the address bytes were drawn";
  EXPECT_EQ(snapshot.primitives[0].character, 'X');
  const double dx = snapshot.primitives[0].size.dx;
  const double dy = snapshot.primitives[0].size.dy;
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x, 3.0 * dx);
  // Row 0 is the *upper* leftmost character position here — the opposite end
  // from APS's own numbering in §6.1.2.4.
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.y,
                   kNabtsDisplayAreaHeight - 3.0 * dy);
}

// §6.1.6.5(6): "If the two bytes are from columns 2 and 3 (or columns 10 and
// 11), they are ignored" — consumed and discarded, so nothing is drawn for
// them and the cursor stays where the reset put it.
TEST(NaplpsInterpreter, NSRIgnoresAnAddressFromColumnsTwoAndThree) {
  Record record;
  record.text_mode().byte(0x1F).add({'!', '"'}).add({'X'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u) << "the ignored bytes were drawn";
  EXPECT_EQ(snapshot.primitives[0].character, 'X');
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x, 0.0);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.y,
                   kNabtsDisplayAreaHeight - snapshot.primitives[0].size.dy);
}

// A pair mixing columns 2-3 and 4-7 is one §6.1.6.5(6) does not settle: it is
// not an address, so the cursor stays, and the Norpak TTX 650 consumes it all
// the same.
TEST(NaplpsInterpreter, NSRConsumesAMixedPairWithoutMovingTheCursor) {
  Record record;
  record.text_mode().add({'A'}).byte(0x1F).add({'!', 'B'}).add({'X'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u) << "the pair was drawn";
  EXPECT_EQ(snapshot.primitives[1].character, 'X');
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x, 1.0 / 40.0);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.y,
                   snapshot.primitives[0].origin.y);
}

// §6.1.4, §6.1.5, §6.1.6.1: the transmission and device controls and NUL "may
// be embedded within any presentation layer sequence without affecting that
// sequence" — an escape sequence and the bytes a control takes after itself
// as much as a PDI's operands. A lost packet's filler lands anywhere.
TEST(NaplpsInterpreter, ATransparentControlInsideASequenceLeavesItWhole) {
  Record repeat;
  // 'A', ESC NUL 4/6 (REPEAT), DLE, count 3.
  repeat.text_mode().add({'A', 0x1B, 0x00, code(4, 6), 0x10, 0x43});
  EXPECT_EQ(run(repeat).primitives.size(), 4u);

  Record nsr;
  // NSR, NUL, row 2, SYN, column 3, 'X'.
  nsr.text_mode().add({0x1F, 0x00, 0x42, 0x16, 0x43, 'X'});
  const NabtsPageSnapshot snapshot = run(nsr);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x,
                   3.0 * snapshot.primitives[0].size.dx);
}

// A C0 control after NSR "terminates the NSR sequence" and is executed, so it
// must not be swallowed as half an address.
TEST(NaplpsInterpreter, NSRLeavesAFollowingControlToBeExecuted) {
  Record record;
  // NSR, then APD (line feed), then a character: the line feed must move the
  // cursor down a row rather than being read as a row address.
  record.text_mode().byte(0x1F).byte(0x0A).add({'X'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_DOUBLE_EQ(
      snapshot.primitives[0].origin.y,
      kNabtsDisplayAreaHeight - 2.0 * snapshot.primitives[0].size.dy);
}

// §6.1.2.2: APF is the forward movement, i.e. a tab of one character position.
TEST(NaplpsInterpreter, APFSkipsOneCharacterPosition) {
  Record record;
  record.text_mode().byte(0x0C).add({'A'}).byte(0x09).add({'B'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  // One field for the character, one for the APF.
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x, 2.0 / 40.0);
}

// All four are defined *relative to the character path*, so a record that
// turned the path through 90 degrees gets its rows turned with it (§5.3.2.3.3
// Table 7 and §5.3.2.3.5). With the path running down the screen, -90 degrees
// from it is to the left.
TEST(NaplpsInterpreter, TheFormatEffectorsFollowTheCharacterPath) {
  // TEXT byte 1 carries the character path in b4-b3 (§5.3.2.3.1); Table 7
  // code 2 is "up". Started from the reset cursor at the top left, the advance
  // takes the cursor up into the border strip above the display area — still
  // on the unit screen, which is what bounds a character's advance
  // (§5.3.2.3.6) — and the APD then moves across, which is no movement out of
  // the display area in the direction it travels.
  Record record;
  record.pdi()
      .byte(code(2, 2))                             // TEXT (§5.3.2.3, Fig. 13)
      .byte(numeric(static_cast<uint8_t>(2 << 2)))  // path = up
      .byte(numeric(0))
      .text_mode()
      .add({'A'})
      .byte(0x0A)  // APD: -90 degrees from an upward path, i.e. to the right
      .add({'B'});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  // 'A' advanced the cursor up the screen — the path — by the field height,
  // and APD then stepped across the path by the field width rather than
  // further along it.
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.y,
                   snapshot.primitives[0].origin.y + 5.0 / 128.0);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].origin.x,
                   snapshot.primitives[0].origin.x + 1.0 / 40.0);
}

// §6.2.7.6-10: each C1 text control sets the character field to a stated size.
TEST(NaplpsInterpreter, TheC1TextControlsSetTheStatedCharacterFieldSizes) {
  struct Case {
    uint8_t final_byte;
    double dx;
    double dy;
  };
  const Case cases[] = {
      {code(4, 10), 1.0 / 80.0, 5.0 / 128.0},  // SMALL TEXT §6.2.7.6
      {code(4, 11), 1.0 / 32.0, 3.0 / 64.0},   // MEDIUM TEXT §6.2.7.7
      {code(4, 12), 1.0 / 40.0, 5.0 / 128.0},  // NORMAL TEXT §6.2.7.8
      {code(4, 13), 1.0 / 40.0, 5.0 / 64.0},   // DOUBLE HEIGHT §6.2.7.9
      {code(4, 15), 1.0 / 20.0, 5.0 / 64.0},   // DOUBLE SIZE §6.2.7.10
  };

  for (const Case& test_case : cases) {
    Record record;
    record.text_mode().escape({test_case.final_byte}).byte('X');
    const NabtsPageSnapshot snapshot = run(record);
    ASSERT_EQ(snapshot.primitives.size(), 1u) << +test_case.final_byte;
    EXPECT_DOUBLE_EQ(snapshot.primitives[0].size.dx, test_case.dx)
        << +test_case.final_byte;
    EXPECT_DOUBLE_EQ(snapshot.primitives[0].size.dy, test_case.dy)
        << +test_case.final_byte;
  }
}

// §6.2.7.4 and §6.2.7.15: reverse video and underline are text attributes a
// character inherits.
TEST(NaplpsInterpreter, ReverseVideoAndUnderlineAreCarriedOnTheCharacter) {
  Record record;
  record.text_mode()
      .escape({code(4, 8)})  // REVERSE VIDEO
      .escape({code(5, 9)})  // UNDERLINE START
      .byte('A')
      .escape({code(4, 9)})   // NORMAL VIDEO
      .escape({code(5, 10)})  // UNDERLINE STOP
      .byte('B');

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_TRUE(snapshot.primitives[0].reverse_video);
  EXPECT_TRUE(snapshot.primitives[0].underlined);
  EXPECT_FALSE(snapshot.primitives[1].reverse_video);
  EXPECT_FALSE(snapshot.primitives[1].underlined);
}

// §6.2.7.2: REPEAT repeats the preceding character a stated number of
// additional times.
TEST(NaplpsInterpreter, RepeatEmitsTheStatedNumberOfAdditionalCharacters) {
  Record record;
  record.text_mode()
      .byte('X')
      .escape({code(4, 6)})  // REPEAT
      .byte(numeric(3));     // three more

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 4u) << "one plus three repeats";
  for (const NabtsPrimitive& primitive : snapshot.primitives) {
    EXPECT_EQ(primitive.character, 'X');
  }
}

// §4.3.2: a single shift lasts one character, so the character after it comes
// from the locked set again.
TEST(NaplpsInterpreter, ASingleShiftAppliesToExactlyOneCharacter) {
  Record record;
  record.text_mode()
      .byte('A')
      .byte(0x19)  // SS2 — G2, the supplementary set
      .byte('B')
      .byte('C');

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  EXPECT_EQ(snapshot.primitives[0].repertoire,
            NabtsPrimitive::Repertoire::kPrimary);
  EXPECT_EQ(snapshot.primitives[1].repertoire,
            NabtsPrimitive::Repertoire::kSupplementary);
  EXPECT_EQ(snapshot.primitives[2].repertoire,
            NabtsPrimitive::Repertoire::kPrimary);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Macros (§6.2.2)
////////////////////////////////////////////////////////////////////////////////////////////

// A macro defined and invoked expands to the same display list as its inline
// form.
TEST(NaplpsInterpreter, AnInvokedMacroExpandsToItsInlineForm) {
  // The body: switch to text and draw two characters.
  const std::vector<uint8_t> body = {0x0F, 'H', 'i'};

  Record inline_form;
  inline_form.add({body[0], body[1], body[2]});
  const NabtsPageSnapshot expected = run(inline_form);

  Record macro_form;
  macro_form
      // DEF MACRO at name 2/0, body, END.
      .escape({code(4, 0)})
      .byte(code(2, 0))
      .add({body[0], body[1], body[2]})
      .escape({code(4, 5)})
      // Designate the macro set into G1 and invoke it, then name 2/0.
      .escape({code(2, 9), code(7, 10)})
      .byte(0x0E)
      .byte(code(2, 0));
  const NabtsPageSnapshot actual = run(macro_form);

  ASSERT_EQ(actual.primitives.size(), expected.primitives.size());
  for (size_t i = 0; i < expected.primitives.size(); ++i) {
    EXPECT_EQ(actual.primitives[i].character, expected.primitives[i].character)
        << "primitive " << i;
    EXPECT_DOUBLE_EQ(actual.primitives[i].origin.x,
                     expected.primitives[i].origin.x)
        << "primitive " << i;
  }
  EXPECT_EQ(actual.diagnostics.unresolved_macros, 0u);
}

// §6.2.2.1: "A null macro definition ... causes that macro to be deleted."
TEST(NaplpsInterpreter, ANullMacroDefinitionDeletesTheMacro) {
  Record record;
  record.escape({code(4, 0)})
      .byte(code(2, 0))
      .add({0x0F, 'X'})
      .escape({code(4, 5)})
      // Redefine it with nothing between the name and END.
      .escape({code(4, 0)})
      .byte(code(2, 0))
      .escape({code(4, 5)})
      // Invoke it: there is nothing there now.
      .escape({code(2, 9), code(7, 10)})
      .byte(0x0E)
      .byte(code(2, 0));

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_TRUE(snapshot.primitives.empty());
  EXPECT_EQ(snapshot.diagnostics.unresolved_macros, 1u);
}

// §6.2.2.1 lists DEF MACRO among the controls that terminate a definition, so
// one definition can follow another with no END between them.
// The second SO before the second macro name is not decoration. A macro's
// effects on the presentation state persist past its expansion — §6.2.3 says so
// of a DRCS definition in as many words, "Any changes to the state of the
// receiving device, such as character field size, in-use G-sets, etc, shall
// persist", and a macro is no different — so the SI inside the first macro's
// body leaves the primary set invoked. Naming the second macro without
// re-invoking the macro set draws '!' instead, which is what an earlier version
// of this test did.
TEST(NaplpsInterpreter, OneMacroDefinitionTerminatesThePrevious) {
  Record record;
  record.escape({code(4, 0)})
      .byte(code(2, 0))
      .add({0x0F, 'A'})
      .escape({code(4, 0)})  // terminates the first and opens the second
      .byte(code(2, 1))
      .add({0x0F, 'B'})
      .escape({code(4, 5)})
      .escape({code(2, 9), code(7, 10)})
      .byte(0x0E)
      .byte(code(2, 0))
      .byte(0x0E)  // the first macro left G0 invoked; go back to the macro set
      .byte(code(2, 1));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_EQ(snapshot.primitives[0].character, 'A');
  EXPECT_EQ(snapshot.primitives[1].character, 'B');
}

// The property the test above depends on, stated on its own: a macro's changes
// to the in-use table outlive its expansion.
TEST(NaplpsInterpreter, AMacrosStateChangesPersistAfterItReturns) {
  Record record;
  record
      // A macro whose whole body is "invoke G0".
      .escape({code(4, 0)})
      .byte(code(2, 0))
      .byte(0x0F)
      .escape({code(4, 5)})
      // Invoke the macro set, run the macro, then send a graphic byte with no
      // invocation of its own.
      .escape({code(2, 9), code(7, 10)})
      .byte(0x0E)
      .byte(code(2, 0))
      .byte('Z');

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u)
      << "the macro's SI did not outlive its expansion";
  EXPECT_EQ(snapshot.primitives[0].character, 'Z');
  EXPECT_EQ(snapshot.primitives[0].repertoire,
            NabtsPrimitive::Repertoire::kPrimary);
}

// §6.2.2.1: the definition is stored and *not* executed, so a DEF MACRO body
// draws nothing at definition time.
TEST(NaplpsInterpreter, DefMacroStoresWithoutExecuting) {
  Record record;
  record.escape({code(4, 0)})
      .byte(code(2, 0))
      .add({0x0F, 'X'})
      .escape({code(4, 5)});

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_TRUE(snapshot.primitives.empty());
}

// §6.2.2.2: DEFP MACRO is the exception — its body is "simultaneously executed
// and stored".
TEST(NaplpsInterpreter, DefpMacroExecutesAsItStores) {
  Record record;
  record.escape({code(4, 1)})
      .byte(code(2, 0))
      .add({0x0F, 'X'})
      .escape({code(4, 5)});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].character, 'X');
}

// §6.2.2.3: a transmit macro "when called, [is] not executed, but [is]
// transmitted in their entirety to the host". There is no host here.
TEST(NaplpsInterpreter, ATransmitMacroDrawsNothingWhenInvoked) {
  Record record;
  record
      .escape({code(4, 2)})  // DEFT MACRO
      .byte(code(2, 0))
      .add({0x0F, 'X'})
      .escape({code(4, 5)})
      .escape({code(2, 9), code(7, 10)})
      .byte(0x0E)
      .byte(code(2, 0));

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_TRUE(snapshot.primitives.empty());
}

// The standard bounds macro nesting only by memory, so a self-invoking macro is
// expressible. It is capped and counted rather than followed.
TEST(NaplpsInterpreter, BoundsMacroRecursionRatherThanFollowingIt) {
  Record record;
  record.escape({code(4, 0)})
      .byte(code(2, 0))
      // Body: designate and invoke the macro set, then invoke itself.
      .escape({code(2, 9), code(7, 10)})
      .byte(0x0E)
      .byte(code(2, 0))
      .escape({code(4, 5)})
      .escape({code(2, 9), code(7, 10)})
      .byte(0x0E)
      .byte(code(2, 0));

  const NabtsPageSnapshot snapshot = run(record);
  // It stopped, which is the point of the test, and said so.
  EXPECT_GT(snapshot.diagnostics.unresolved_macros, 0u);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Keys linked to macros (§5.5, Table D1 item 7)
////////////////////////////////////////////////////////////////////////////////////////////

/// DEF MACRO |name| with |body|, closed by END, appended to |record|.
void define_macro(Record& record, uint8_t name,
                  const std::vector<uint8_t>& body, uint8_t opener = 0x40) {
  record.escape({opener}).byte(name).add(body).escape({code(4, 5)});
}

/// A one-pixel walk the way a game moves its cursor: a page that
/// sets the drawing point and defines 2/9 "a point one step right" and 2/7 "a
/// point one step left", and draws nothing itself.
Record walking_page() {
  std::vector<uint8_t> right{static_cast<uint8_t>(NaplpsPdi::kPointRel)};
  const std::vector<uint8_t> step_right = coord3(0.0625, 0.0);
  right.insert(right.end(), step_right.begin(), step_right.end());
  std::vector<uint8_t> left{static_cast<uint8_t>(NaplpsPdi::kPointRel)};
  const std::vector<uint8_t> step_left = coord3(-0.0625, 0.0);
  left.insert(left.end(), step_left.begin(), step_left.end());

  Record page;
  page.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .add(coord3(0.5, 0.25));
  define_macro(page, code(2, 9), right);
  define_macro(page, code(2, 7), left);
  return page;
}

// Table D1 item 7: "Macros may be accessed by user input mechanisms, for
// example, keys", from 2/0 up. A page that defines a digit's macro and never
// runs it leaves it for the key; one it runs itself, a transmit macro and a
// macro past 2/9 are not the keys'.
TEST(NaplpsInterpreter, TheDigitMacrosAPageDefinesAndLeavesAloneAreItsKeys) {
  const std::vector<uint8_t> dot{static_cast<uint8_t>(NaplpsPdi::kPointAbs)};
  Record record;
  record.pdi();
  define_macro(record, code(2, 1), dot);
  define_macro(record, code(2, 2), dot);
  define_macro(record, code(2, 3), dot);
  define_macro(record, code(2, 4), dot, code(4, 2));  // DEFT MACRO
  define_macro(record, code(2, 10), dot);
  // Run 2/3 itself: designate the macro set into G1 and name it.
  record.escape({code(2, 9), code(7, 10)}).byte(0x0E).byte(code(2, 3));

  NaplpsInterpreter interpreter;
  interpreter.run(record.data());
  EXPECT_EQ(interpreter.linked_keys(), (1u << 1) | (1u << 2));

  // A record that defines none links none, whatever an earlier one did.
  Record plain;
  plain.text_mode().add({'A'});
  interpreter.run(plain.data());
  EXPECT_EQ(interpreter.linked_keys(), 0u);
}

// A key runs its macro from the state the page — and every key before it —
// left: the drawing point a move left behind is where the next one starts, so
// 9, 9, 7 steps right, right and back.
TEST(NaplpsInterpreter, PressingADigitRunsItsMacroFromThePagesDrawingPoint) {
  NaplpsInterpreter interpreter;
  interpreter.reset_decoder();
  const NabtsPageSnapshot page = interpreter.run(walking_page().data());
  EXPECT_TRUE(page.primitives.empty());
  EXPECT_EQ(interpreter.linked_keys(), (1u << 7) | (1u << 9));

  interpreter.press_key(9);
  interpreter.press_key(9);
  const NabtsPageSnapshot played = interpreter.press_key(7);

  ASSERT_EQ(played.primitives.size(), 3u)
      << "each key's drawing stays on screen under the next";
  const double expected_x[] = {0.5625, 0.625, 0.5625};
  for (size_t i = 0; i < played.primitives.size(); ++i) {
    EXPECT_EQ(played.primitives[i].kind, NabtsPrimitiveKind::kPoint);
    EXPECT_DOUBLE_EQ(played.primitives[i].origin.x, expected_x[i])
        << "point " << i;
    EXPECT_DOUBLE_EQ(played.primitives[i].origin.y, 0.25) << "point " << i;
  }
  // What earlier keys drew is on screen from the moment this one is pressed.
  EXPECT_EQ(played.primitives[0].appear_ms, 0u);
  EXPECT_EQ(played.primitives[1].appear_ms, 0u);
  EXPECT_EQ(played.diagnostics.unresolved_macros, 0u);
}

// Table D1 item 7 leaves the state a key's macro ends in "implementation-
// dependent", the service's to restore. A game can have a key that writes the
// colour map entry the key before it selected — "one key that is somewhat
// unpredictable" — so a key acts on what the last one left.
TEST(NaplpsInterpreter, AKeyActsOnTheColourTheKeyBeforeItSelected) {
  const auto select = [](uint8_t entry) {
    // One single-value byte, the address left-justified in its six bits.
    return std::vector<uint8_t>{static_cast<uint8_t>(NaplpsPdi::kSelectColour),
                                numeric(static_cast<uint8_t>(entry << 2))};
  };
  const std::vector<uint8_t> black{static_cast<uint8_t>(NaplpsPdi::kSetColour),
                                   numeric(0), numeric(0), numeric(0)};
  std::vector<uint8_t> square{static_cast<uint8_t>(NaplpsPdi::kRectFilled)};
  const std::vector<uint8_t> extent = coord3(0.0625, 0.0625);
  square.insert(square.end(), extent.begin(), extent.end());

  Record page;
  page.pdi()
      .add(select(1))
      .add({static_cast<uint8_t>(NaplpsPdi::kSetColour), numeric(0x3F),
            numeric(0x3F), numeric(0x3F)})
      .add(square)
      .add(select(2))
      .add({static_cast<uint8_t>(NaplpsPdi::kSetColour), numeric(0x3F),
            numeric(0x3F), numeric(0x3F)})
      .add(square);
  define_macro(page, code(2, 1), select(1));
  define_macro(page, code(2, 2), select(2));
  define_macro(page, code(2, 6), black);

  NaplpsInterpreter first;
  first.reset_decoder();
  first.run(page.data());
  first.press_key(1);
  const NabtsPageSnapshot after_one = first.press_key(6);
  ASSERT_EQ(after_one.primitives.size(), 2u);
  EXPECT_EQ(after_one.primitives[0].colour, kNabtsNominalBlack);
  EXPECT_EQ(after_one.primitives[1].colour, kNabtsNominalWhite);

  NaplpsInterpreter second;
  second.reset_decoder();
  second.run(page.data());
  second.press_key(2);
  const NabtsPageSnapshot after_two = second.press_key(6);
  ASSERT_EQ(after_two.primitives.size(), 2u);
  EXPECT_EQ(after_two.primitives[0].colour, kNabtsNominalWhite);
  EXPECT_EQ(after_two.primitives[1].colour, kNabtsNominalBlack);
}

// A key the page linked nothing to — or a transmit macro, which is a host's
// business (§6.2.2.3) — leaves the page as it stands.
TEST(NaplpsInterpreter, AKeyWithNoMacroLeavesThePageAsItStands) {
  NaplpsInterpreter interpreter;
  interpreter.reset_decoder();
  interpreter.run(walking_page().data());
  interpreter.press_key(9);
  const NabtsPageSnapshot unchanged = interpreter.press_key(3);
  ASSERT_EQ(unchanged.primitives.size(), 1u);
  EXPECT_DOUBLE_EQ(unchanged.primitives[0].origin.x, 0.5625);
  EXPECT_EQ(unchanged.diagnostics.unresolved_macros, 0u);

  // And the next key carries on from where the last real one left off.
  const NabtsPageSnapshot next = interpreter.press_key(9);
  ASSERT_EQ(next.primitives.size(), 2u);
  EXPECT_DOUBLE_EQ(next.primitives[1].origin.x, 0.625);
}

////////////////////////////////////////////////////////////////////////////////////////////
// DRCS and the storage budget
////////////////////////////////////////////////////////////////////////////////////////////

// §6.2.3: the presentation code following DEF DRCS is executed into a storage
// buffer rather than onto the screen, and every element it writes comes on.
TEST(NaplpsInterpreter, DefDrcsDrawsIntoABufferRatherThanTheDisplay) {
  Record record;
  record
      .escape({code(4, 3)})  // DEF DRCS
      .byte(code(2, 0))
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b011, 0b011))
      .escape({code(4, 5)});  // END

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_TRUE(snapshot.primitives.empty())
      << "a DRCS definition must not draw on the display";
  ASSERT_EQ(snapshot.drcs.size(), 1u);
  EXPECT_EQ(snapshot.drcs[0].code, code(2, 0));
  EXPECT_TRUE(snapshot.drcs[0].defined());
  // Something was switched on.
  const auto on = std::count(snapshot.drcs[0].elements.begin(),
                             snapshot.drcs[0].elements.end(), true);
  EXPECT_GT(on, 0);
}

// §6.2.3 sizes the storage buffer at "the minimum physical resolution ...
// covered by the character field", and Table D1 item 8 gives the worked
// example: "if the character field size is dx = 6/256 and dy = 10/256, then the
// storage buffer shall be an array of at least 6 elements horizontal by at
// least 10 elements vertical".
//
// Both axes are counted in the receiver's pixels, and a unit-y height covers
// more rows than the grid has: the grid's 200 rows span only the visible
// 0,78125 of unit y, so 10/256 of unit y is 10 pixels rather than the 8 that
// scaling by the row count alone would give.
TEST(NaplpsInterpreter, ADrcsBufferIsSizedInTheReceiversPixels) {
  NaplpsState state;
  state.text.character_field = NabtsSize{6.0 / 256.0, 10.0 / 256.0};

  uint16_t width = 0;
  uint16_t height = 0;
  state.drcs_buffer_size(width, height);
  EXPECT_EQ(width, 6);
  EXPECT_EQ(height, 10);
}

// A finer receiver stores the same character field at its own resolution, which
// is what §6.2.3's "minimum physical resolution covered by the character field"
// means once the receiver is not the reference one.
TEST(NaplpsInterpreter, ADrcsBufferFollowsTheReceiverResolution) {
  NaplpsState state;
  state.set_render_grid(kNaplpsGridTwice);
  state.text.character_field = NabtsSize{6.0 / 256.0, 10.0 / 256.0};

  uint16_t width = 0;
  uint16_t height = 0;
  state.drcs_buffer_size(width, height);
  EXPECT_EQ(width, 12);
  EXPECT_EQ(height, 20);
}

// The grid is a property of the receiver rather than of the presentation, so
// the resets that return the presentation state to its defaults leave it alone.
TEST(NaplpsInterpreter, AResetLeavesTheReceiverResolutionAlone) {
  NaplpsState state;
  state.set_render_grid(kNaplpsGridTwice);
  state.reset_all();
  EXPECT_EQ(state.render_grid().width, kNaplpsGridTwice.width);
}

// A definition is executed "in the same manner as if it were being displayed"
// (§6.2.3), so what lands in the buffer is the figure the code drew. An arc is
// the case that shows it: capturing only the box around its control points
// would fill the buffer solid, and the middle of a stroked arc is empty.
TEST(NaplpsInterpreter, ADrcsDefinitionCapturesTheFigureNotItsBoundingBox) {
  Record record;
  record
      .escape({code(4, 3)})  // DEF DRCS
      .byte(code(2, 0))
      .pdi()
      // A large character field, so the buffer has room for the figure to have
      // an inside worth checking.
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      // ARC (outlined): a start point, an intermediate point and an end point
      // — a half circle over (0.5, 0.5), within the unit screen.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetArcOutlined))
      .byte(coord1(0b001, 0b010))
      .byte(coord1(0b001, 0b001))
      .byte(coord1(0b001, 0b111))
      .escape({code(4, 5)});  // END

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.drcs.size(), 1u);
  const NabtsDrcsCharacter& glyph = snapshot.drcs[0];
  ASSERT_TRUE(glyph.defined());

  const auto on =
      std::count(glyph.elements.begin(), glyph.elements.end(), true);
  EXPECT_GT(on, 0) << "the arc captured nothing at all";
  EXPECT_LT(on, static_cast<long>(glyph.elements.size()))
      << "the arc filled its whole buffer, which is the bounding box, not the "
         "figure";
}

// §6.2.3: "When the current DRCS downloading operation is terminated by another
// DEF DRCS command, the next character of the DRCS G-set (ie, in the circular
// sequence 2/0, 2/1, ... 7/15, 2/0 ...) is defined by the presentation layer
// code immediately following this new DEF DRCS command. (This is the only time
// DEF DRCS is not followed by the DRCS character to be defined.)" — so the
// chained form carries no code byte, and the byte after it is the next
// character's defining code, not a name.
TEST(NaplpsInterpreter, ADefDrcsTerminatingAnotherAdvancesThroughTheGSet) {
  Record record;
  record.escape({code(4, 3)})
      .byte(code(2, 0))
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b011, 0b011))
      // A second DEF DRCS with no code: the definition that follows is 2/1's.
      .escape({code(4, 3)})
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b011, 0b011))
      .escape({code(4, 5)});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.drcs.size(), 2u);
  EXPECT_EQ(snapshot.drcs[0].code, code(2, 0));
  EXPECT_EQ(snapshot.drcs[1].code, code(2, 1));
  // The chained definition's first byte was executed as defining code rather
  // than consumed as a name, so 2/1 has elements switched on.
  const auto on = std::count(snapshot.drcs[1].elements.begin(),
                             snapshot.drcs[1].elements.end(), true);
  EXPECT_GT(on, 0);
}

// §6.2.3: "If a DRCS definition is immediately terminated with no intervening
// presentation layer code, the buffer space allocated to that character is
// freed."
TEST(NaplpsInterpreter, AnImmediatelyTerminatedDrcsDefinitionIsFreed) {
  Record record;
  record.escape({code(4, 3)}).byte(code(2, 0)).escape({code(4, 5)});

  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot snapshot = interpreter.run(record.data());
  EXPECT_TRUE(snapshot.drcs.empty());
  EXPECT_EQ(interpreter.state().storage_used(), 0u);
}

// Table D1 item 5(3)(b): the four programmable texture masks are 16 by 16
// stored elements, and item 11 keeps that storage outside the shared budget.
TEST(NaplpsInterpreter, DefTextureDefinesA16By16Mask) {
  Record record;
  record
      .escape({code(4, 4)})  // DEF TEXTURE
      .byte(code(4, 1))      // mask A
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b011, 0b011))
      .escape({code(4, 5)});

  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot snapshot = interpreter.run(record.data());
  ASSERT_TRUE(snapshot.texture_masks[0].defined());
  EXPECT_EQ(snapshot.texture_masks[0].width, 16u);
  EXPECT_EQ(snapshot.texture_masks[0].height, 16u);
  // The mask storage is not part of the shared budget.
  EXPECT_EQ(interpreter.state().storage_used(), 0u);
}

// §6.2.4: "If bits b7 to b1 of the character following the DEF TEXTURE control
// are not in the range 4/1 to 4/4, the entire command ... is executed as a null
// operation."
TEST(NaplpsInterpreter, DefTextureRefusesAMaskNameOutsideTheRange) {
  Record record;
  record.escape({code(4, 4)}).byte(code(5, 0)).escape({code(4, 5)});

  const NabtsPageSnapshot snapshot = run(record);
  for (const NabtsTextureMask& mask : snapshot.texture_masks) {
    EXPECT_FALSE(mask.defined());
  }
}

// The storage budget is enforced rather than exceeded.
// CEA-516 §8.6.1 and T.101 Table II-3 both give 3072 bytes, shared between
// macro definitions and DRCS.
TEST(NaplpsInterpreter, EnforcesTheSharedStorageBudget) {
  // A macro body just under the whole budget, then another that cannot fit.
  const size_t big = kNaplpsSharedStorageBytes - 8;

  Record record;
  record.escape({code(4, 0)}).byte(code(2, 0));
  for (size_t i = 0; i < big; ++i) {
    record.byte('A');
  }
  record.escape({code(4, 0)}).byte(code(2, 1));
  for (size_t i = 0; i < 64; ++i) {
    record.byte('B');
  }
  record.escape({code(4, 5)});

  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot snapshot = interpreter.run(record.data());
  EXPECT_LE(interpreter.state().storage_used(), kNaplpsSharedStorageBytes)
      << "the budget was exceeded rather than enforced";
  EXPECT_GT(snapshot.diagnostics.storage_refusals, 0u)
      << "the overrun was accepted silently";
}

////////////////////////////////////////////////////////////////////////////////////////////
// Robustness on a recovered record
////////////////////////////////////////////////////////////////////////////////////////////

// §5.3.1: the transmission and device controls and NUL "may be embedded within
// any presentation layer sequence without affecting that sequence" — so one in
// the middle of a PDI's operands must not terminate it.
TEST(NaplpsInterpreter, AnEmbeddedNullDoesNotTerminateAnOpenPdi) {
  Record with_null;
  with_null.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      .byte(0x00)  // NUL
      .byte(coord1(0b010, 0b010))
      .byte(0x11)  // DC1
      .byte(coord1(0b011, 0b011));

  const NabtsPageSnapshot snapshot = run(with_null);
  EXPECT_EQ(snapshot.primitives.size(), 3u)
      << "an embedded transparent control broke the operand run";
}

// A record that ends mid-sequence yields what it managed. This is the normal
// case for a recovered record: the group lost its last packet.
TEST(NaplpsInterpreter, ATruncatedRecordYieldsWhatItManaged) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      // An arc with no coordinate data at all behind it.
      .byte(static_cast<uint8_t>(NaplpsPdi::kArcOutlined));

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_EQ(snapshot.primitives.size(), 1u) << "the point before it survived";
  EXPECT_GT(snapshot.diagnostics.truncated_pdis, 0u);
}

// §5.3.3.3.1: "If the end point is omitted, it is taken to be coincident with
// the start point and a circle is drawn." A single coordinate block after ARC
// is the compact encoding of a circle, not a record that ran out — reading it
// as a truncation drops every circle a service draws that way.
TEST(NaplpsInterpreter, AnArcWithNoEndPointIsACircle) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .byte(coord1(0b001, 0b001))
      // Out to (0.5, 0.5) and back: a circle within the unit screen.
      .byte(static_cast<uint8_t>(NaplpsPdi::kArcOutlined))
      .byte(coord1(0b001, 0b001))
      .text_mode();  // the code that ends the command

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const NabtsPrimitive& arc = snapshot.primitives[0];
  EXPECT_EQ(arc.kind, NabtsPrimitiveKind::kArc);
  // Start, the intermediate point that gives the diameter, and an end point
  // supplied as coincident with the start — which is what makes it a circle.
  ASSERT_EQ(arc.points.size(), 3u);
  EXPECT_DOUBLE_EQ(arc.points[2].x, arc.points[0].x);
  EXPECT_DOUBLE_EQ(arc.points[2].y, arc.points[0].y);
  EXPECT_NE(arc.points[1].x, arc.points[0].x);
  EXPECT_EQ(snapshot.diagnostics.truncated_pdis, 0u);
}

// CEA-516 §3.3 puts odd parity in b8 of every data byte, so the interpreter
// strips it — and a caller that stripped it already gets the same answer.
TEST(NaplpsInterpreter, StripsByteParityAndIsIdempotentAboutIt) {
  Record record;
  record.text_mode().add({'A', 'B'});

  std::vector<uint8_t> with_parity;
  for (const uint8_t byte : record.data()) {
    // Set b8 on every byte, as a type-zero group's odd parity may.
    with_parity.push_back(static_cast<uint8_t>(byte | 0x80));
  }

  NaplpsInterpreter stripped;
  NaplpsInterpreter unstripped;
  const NabtsPageSnapshot a = stripped.run(record.data());
  const NabtsPageSnapshot b = unstripped.run(with_parity);

  ASSERT_EQ(a.primitives.size(), b.primitives.size());
  for (size_t i = 0; i < a.primitives.size(); ++i) {
    EXPECT_EQ(a.primitives[i].character, b.primitives[i].character);
  }
}

// §5.3.1 makes an out-of-screen coordinate an error, which may be "executed and
// clipped within the unit screen": the point is kept where the record put it,
// and what is drawn there is clipped by the renderer — not moved onto the edge,
// and not wrapped round to the other side.
TEST(NaplpsInterpreter, KeepsACoordinateOffTheUnitScreen) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .byte(coord1(0b001, 0b001))
      // A relative move of -0,5 in x from +0,25, which lands off the screen.
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointRel))
      .byte(coord1(0b110, 0b000));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x, -0.25)
      << "kept, not moved onto the edge or wrapped";
}

// §4.3.2: a null set "is a set in which all code positions are executed as null
// operations", so a record that designates one draws nothing from it.
TEST(NaplpsInterpreter, ANullSetDrawsNothing) {
  Record record;
  record
      // Designate an unlisted final character into G0, then invoke it.
      .escape({code(2, 8), code(6, 5)})
      .text_mode()
      .add({'A', 'B', 'C'});

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_TRUE(snapshot.primitives.empty());
}

////////////////////////////////////////////////////////////////////////////////////////////
// What persists, and what NSR and the resets do to it
////////////////////////////////////////////////////////////////////////////////////////////

/// A macro definition at name 2/0 whose body draws 'X' from the primary set.
Record& define_macro_x(Record& record) {
  return record.escape({code(4, 0)})
      .byte(code(2, 0))
      .add({0x0F, 'X'})
      .escape({code(4, 5)});
}

/// Designate the macro set into G1, invoke it, and call the macro at 2/0.
Record& invoke_macro_20(Record& record) {
  return record.escape({code(2, 9), code(7, 10)}).byte(0x0E).byte(code(2, 0));
}

// A macro whose body clears the macros (RESET, byte 2 b5, §5.3.2.9.3) runs to
// its end regardless: the expansion is of the definition as it stood when the
// macro was invoked. Before, this was a read of freed storage that drew such a
// page differently from one run to the next.
TEST(NaplpsInterpreter, AMacroThatClearsTheMacrosRunsToItsEnd) {
  Record record;
  record.escape({code(4, 0)})
      .byte(code(2, 0))
      .add({0x0F, 'X'})
      .escape({code(2, 9), code(5, 7)})  // PDI into G1
      .byte(0x0E)
      .add({static_cast<uint8_t>(NaplpsPdi::kReset), numeric(0),
            numeric(0b010000)})
      .add({0x0F, 'Y'})
      .escape({code(4, 5)});
  invoke_macro_20(record);

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_EQ(snapshot.primitives[0].character, 'X');
  EXPECT_EQ(snapshot.primitives[1].character, 'Y');
}

// §6.1.6.5 resets selectively: "The programmable masks are not cleared", "The
// color map is not changed", and macros and DRCS are not in its list at all —
// only the RESET PDI of §5.3.2.9 clears those. CEA-516 §8.7.2.3.1 restates it
// for the teletext service: a page's opening NSR establishes "a known
// presentation state without affecting macros, DRCS, texture definitions,
// color map or the currently displayed image".
TEST(NaplpsInterpreter, NSRPreservesMacrosAndTheColourMap) {
  Record record;
  define_macro_x(record);
  record
      .pdi()
      // Write map entry 3, as the colour-map test above does.
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b110000))
      // NSR, then invoke the macro.
      .byte(0x1F);
  invoke_macro_20(record);

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u)
      << "the macro did not survive the NSR";
  EXPECT_EQ(snapshot.primitives[0].character, 'X');
  EXPECT_EQ(snapshot.colour_map[3].green, 10u)
      << "the colour map did not survive the NSR";
  EXPECT_EQ(snapshot.diagnostics.unresolved_macros, 0u);
}

// §6.1.6.5(5): "The color mode is set to color mode 0 and the drawing color is
// set to nominal white." And (3) resets the text parameters *and the active
// field* (§5.3.2.9.3's wording, which NSR references).
TEST(NaplpsInterpreter, NSRRestoresWhiteInModeZeroAndTheFullActiveField) {
  Record record;
  record
      .pdi()
      // Mode 1 drawing a mapped colour, and an active field smaller than the
      // screen.
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      .byte(static_cast<uint8_t>(NaplpsPdi::kField))
      .byte(coord1(0b001, 0b001))
      .byte(0x1F);  // NSR

  NaplpsInterpreter interpreter;
  interpreter.run(record.data());
  EXPECT_EQ(interpreter.state().colour.mode(), NabtsColourMode::kDirect);
  EXPECT_EQ(interpreter.state().colour.drawing_colour(), kNabtsNominalWhite);
  EXPECT_DOUBLE_EQ(interpreter.state().field_size.dx, 1.0);
  EXPECT_DOUBLE_EQ(interpreter.state().field_size.dy, 1.0);
}

/// A coordinate word as X3.110 Figure 11 codes it: two's complement, three
/// bytes of the default DOMAIN, 1/256 of the unit screen a step.
std::vector<uint8_t> coord3_signed(double x, double y) {
  const auto encode = [](double value) {
    return static_cast<uint32_t>(std::lround(value * 256.0)) & 0x1FFu;
  };
  const uint32_t xb = encode(x);
  const uint32_t yb = encode(y);
  std::vector<uint8_t> out;
  for (int i = 0; i < 3; ++i) {
    const int shift = 6 - 3 * i;
    out.push_back(numeric(static_cast<uint8_t>((((xb >> shift) & 0x7) << 3) |
                                               ((yb >> shift) & 0x7))));
  }
  return out;
}

std::vector<const NabtsPrimitive*> characters_of(
    const NabtsPageSnapshot& page) {
  std::vector<const NabtsPrimitive*> out;
  for (const NabtsPrimitive& primitive : page.primitives) {
    if (primitive.kind == NabtsPrimitiveKind::kCharacter) {
      out.push_back(&primitive);
    }
  }
  return out;
}

// §6.1.2.3 and §6.1.2.7: while the cursor's character field lies within the
// active field FIELD sets, a carriage return comes back to the field's edge
// and a line feed past its bottom goes to its top row — "the active field used
// for columnated text" (§5.3.3.6.2). A field three rows high holds three lines;
// the fifth line is back on the top row, where the second was.
TEST(NaplpsInterpreter, TextIsKeptToTheActiveFieldItLiesIn) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kField))
      .add(coord3_signed(0.125, 0.625))
      .add(coord3_signed(0.75, 0.125))
      .text_mode();
  for (const char* line : {"AB", "CD", "EF", "GH", "IJ"}) {
    record.text(line).byte(0x0D).byte(0x0A);  // APR APD
  }

  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot page = interpreter.run(record.data());
  const auto chars = characters_of(page);
  ASSERT_EQ(chars.size(), 10u);
  const double row = 5.0 / 128.0;
  const double top = 0.75 - row;
  const double rows[] = {0.625, top, top - row, top - 2.0 * row, top};
  for (size_t line = 0; line < 5; ++line) {
    EXPECT_DOUBLE_EQ(chars[2 * line]->origin.x, 0.125) << "line " << line;
    EXPECT_DOUBLE_EQ(chars[2 * line]->origin.y, rows[line]) << "line " << line;
  }
}

// A field of negative size is counted from its origin's own pixel (see
// active_field_area()), so a field (0.25, 0.5) (-0.25, 0.25) does not hold
// the character field at its origin: the line runs its full length, and a
// carriage return goes to the display area's edge rather than the field's.
TEST(NaplpsInterpreter, TextOutsideTheActiveFieldRunsFree) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kField))
      .add(coord3_signed(0.25, 0.5))
      .add(coord3_signed(-0.25, 0.25))
      .text_mode()
      .text("ABCDEFGHIJ")
      .byte(0x0D)
      .byte(0x0A)
      .text("K");

  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot page = interpreter.run(record.data());
  const auto chars = characters_of(page);
  ASSERT_EQ(chars.size(), 11u);
  EXPECT_DOUBLE_EQ(chars[0]->origin.x, 0.25);
  for (size_t i = 1; i < 10; ++i) {
    EXPECT_DOUBLE_EQ(chars[i]->origin.y, chars[0]->origin.y)
        << "character " << i << " was wrapped inside the field";
    EXPECT_GT(chars[i]->origin.x, chars[i - 1]->origin.x);
  }
  EXPECT_DOUBLE_EQ(chars[10]->origin.x, 0.0);
  EXPECT_DOUBLE_EQ(chars[10]->origin.y, chars[0]->origin.y - 5.0 / 128.0);
}

// The origin-pixel rule decides a cursor on the edge: FIELD (40, 170)
// (60, -30) in 1/256ths covers y 141 to 171, which holds a character field
// from 161 to 171 but not one from 140 to 150. Probed on the TTX 650, with
// pages that put the cursor's cell top on a downward field's origin row.
TEST(NaplpsInterpreter, AFieldOfNegativeSizeCountsFromItsOriginsPixel) {
  const auto carriage_return_from = [](double y) {
    Record record;
    record.pdi()
        .byte(static_cast<uint8_t>(NaplpsPdi::kField))
        .add(coord3_signed(40.0 / 256.0, 170.0 / 256.0))
        .add(coord3_signed(60.0 / 256.0, -30.0 / 256.0))
        .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
        .add(coord3(64.0 / 256.0, y))
        .text_mode()
        .text("AB")
        .byte(0x0D)  // APR
        .text("X");
    const NabtsPageSnapshot page = run(record);
    const auto chars = characters_of(page);
    return chars.size() == 3 ? chars[2]->origin.x : -1.0;
  };
  EXPECT_DOUBLE_EQ(carriage_return_from(161.0 / 256.0), 40.0 / 256.0)
      << "the field's edge";
  EXPECT_DOUBLE_EQ(carriage_return_from(140.0 / 256.0), 0.0)
      << "the display area's edge";
}

// §5.3.2.3.4: the intercharacter distance is a multiple "of the character
// field width (dx) or height (dy), whichever lies parallel to the character
// path, depending on the character path and character rotation" — and
// §5.3.2.3.5 the same for rows. Turned 90 degrees, a 6 by 10 field steps 10
// along a path to the right and 6 a row.
TEST(NaplpsInterpreter, ARotatedFieldStepsByItsSideAlongThePath) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .add(coord3(0.5, 100.0 / 256.0))
      .byte(static_cast<uint8_t>(NaplpsPdi::kText))
      .byte(numeric(0b000001))  // rotation 90
      .byte(numeric(0))
      .add(coord3(6.0 / 256.0, 10.0 / 256.0))
      .text_mode()
      .text("ABC")
      .byte(0x0D)
      .byte(0x0A)
      .text("D");
  const auto chars = characters_of(run(record));
  ASSERT_EQ(chars.size(), 4u);
  for (size_t i = 0; i < 3; ++i) {
    EXPECT_DOUBLE_EQ(chars[i]->origin.x, (128.0 + 10.0 * i) / 256.0);
    EXPECT_DOUBLE_EQ(chars[i]->origin.y, 100.0 / 256.0);
  }
  // The turned field reaches ten to the left of its origin.
  EXPECT_DOUBLE_EQ(chars[3]->origin.x, 10.0 / 256.0);
  EXPECT_DOUBLE_EQ(chars[3]->origin.y, 94.0 / 256.0);
}

// §6.1.6.5(6): "If either of the two bytes following the NSR is not from
// columns 4 through 7 ..., the non-selective reset function of NSR is executed
// and the cursor is not repositioned."
TEST(NaplpsInterpreter, AnNsrWithNoAddressLeavesTheCursor) {
  Record record;
  record.text_mode()
      .text("A")
      .byte(0x1F)  // NSR
      .byte(0x0A)  // APD, which is not an address
      .text("X");
  const auto chars = characters_of(run(record));
  ASSERT_EQ(chars.size(), 2u);
  EXPECT_DOUBLE_EQ(chars[1]->origin.x, 1.0 / 40.0);
  EXPECT_DOUBLE_EQ(chars[1]->origin.y, chars[0]->origin.y - 5.0 / 128.0);
}

// §5.3.2.3.9 keeps the field when its dimensions are omitted. A field word cut
// short comes to a field with no width or no height once §5.3.2.2.5 has
// supplied its trailing zero bits, and both receivers of the day keep the
// field they had for one: here the 12 by 20 field stays.
TEST(NaplpsInterpreter, ATextFieldWithNoSizeKeepsTheFieldThereWas) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kText))
      .byte(numeric(0))
      .byte(numeric(0))
      .add(coord3(12.0 / 256.0, 20.0 / 256.0))
      .text_mode()
      .text("AB")
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kText))
      .byte(numeric(0))
      .byte(numeric(0))
      .add(coord3(0.0, 0.0))
      .text_mode()
      .text("C");
  const auto chars = characters_of(run(record));
  ASSERT_EQ(chars.size(), 3u);
  EXPECT_DOUBLE_EQ(chars[2]->origin.x, 24.0 / 256.0);
  EXPECT_DOUBLE_EQ(chars[2]->origin.y, chars[0]->origin.y);
  EXPECT_DOUBLE_EQ(chars[2]->size.dx, 12.0 / 256.0);
  EXPECT_DOUBLE_EQ(chars[2]->size.dy, 20.0 / 256.0);
}

// The persistence the service model is built on: macros, DRCS and the colour
// map carry from one record to the next in the same interpreter, and
// reset_decoder() — the receiver's general reset (CEA-516 §8.5) — is what
// clears them.
TEST(NaplpsInterpreter, StateCarriesAcrossRecordsUntilTheDecoderIsReset) {
  Record defining;
  define_macro_x(defining);
  Record invoking;
  invoke_macro_20(invoking);

  NaplpsInterpreter interpreter;
  interpreter.run(defining.data());

  const NabtsPageSnapshot second = interpreter.run(invoking.data());
  ASSERT_EQ(second.primitives.size(), 1u)
      << "a macro defined by an earlier record was lost";
  EXPECT_EQ(second.primitives[0].character, 'X');

  interpreter.reset_decoder();
  const NabtsPageSnapshot third = interpreter.run(invoking.data());
  EXPECT_TRUE(third.primitives.empty());
  EXPECT_EQ(third.diagnostics.unresolved_macros, 1u);
}

// CEA-516 §5.2.7.8: a More Record is "presented after the completion of the
// presentation of the current Record" — over the standing display, not over a
// cleared one. A continuation run therefore starts from the previous run's
// display list.
TEST(NaplpsInterpreter, AContinuationRunDrawsOverTheStandingDisplay) {
  Record base;
  base.text_mode().add({'X'});
  Record continuation;
  continuation.text_mode().add({'Y'});

  NaplpsInterpreter interpreter;
  interpreter.run(base.data());
  const NabtsPageSnapshot page = interpreter.run(continuation.data(), true);
  ASSERT_EQ(page.primitives.size(), 2u)
      << "the base record's drawing was not carried into the continuation";
  EXPECT_EQ(page.primitives[0].character, 'X');
  EXPECT_EQ(page.primitives[1].character, 'Y');
}

// The carried primitives keep their colour map addresses, so a map write in
// the continuation retro-colours them — one screen, one map (§5.3.2.5).
TEST(NaplpsInterpreter, AMapWriteInAContinuationRecoloursTheCarriedDisplay) {
  Record base;
  base.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b000000))  // mode 1, address 0
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001));
  Record continuation;
  // The colour state persists between chain members, so this writes entry 0.
  continuation.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b100000));

  NaplpsInterpreter interpreter;
  interpreter.run(base.data());
  const NabtsPageSnapshot page = interpreter.run(continuation.data(), true);
  ASSERT_EQ(page.primitives.size(), 1u);
  EXPECT_EQ(page.primitives[0].colour.green, 10u)
      << "the carried point kept the colour entry 0 held before the "
         "continuation rewrote it";
}

// A continuation that clears the screen clears the carried display with it —
// carrying the canvas must not make CS forget what it wipes.
TEST(NaplpsInterpreter, AClearInAContinuationWipesTheCarriedDisplay) {
  Record base;
  base.text_mode().add({'X'});
  Record continuation;
  continuation.text_mode().byte(0x0C).add({'Y'});  // CS, then draw

  NaplpsInterpreter interpreter;
  interpreter.run(base.data());
  const NabtsPageSnapshot page = interpreter.run(continuation.data(), true);
  ASSERT_EQ(page.primitives.size(), 1u);
  EXPECT_EQ(page.primitives[0].character, 'Y');
}

// CEA-516 §5.2.7.3: the caption preset — colour mode 2, drawing white over
// black out of the stated map entries, cursor and drawing point at (0,0).
TEST(NaplpsInterpreter, TheCaptionPresetDrawsWhiteOverBlackFromTheLowerLeft) {
  Record record;
  record.text_mode().add({'A'});

  NaplpsInterpreter interpreter;
  interpreter.reset_decoder();
  interpreter.apply_caption_state();
  const NabtsPageSnapshot snapshot = interpreter.run(record.data());

  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const NabtsPrimitive& character = snapshot.primitives[0];
  EXPECT_EQ(character.colour_mode, NabtsColourMode::kMappedWithBackground);
  EXPECT_EQ(character.colour, kNabtsNominalWhite);
  EXPECT_EQ(character.background, kNabtsNominalBlack);
  EXPECT_DOUBLE_EQ(character.origin.x, 0.0);
  EXPECT_DOUBLE_EQ(character.origin.y, 0.0);
}

////////////////////////////////////////////////////////////////////////////////////////////
// The colour map: implicit repeat and retroactivity (§5.3.2.5)
////////////////////////////////////////////////////////////////////////////////////////////

// §5.3.2.5.1: additional colour words repeat SET COLOR "with the map address
// incremented prior to the execution of the new opcode. The algorithm for
// incrementing is to change the most significant zero to a one and to change
// all ones to the left of it to zero" — from 0000 that walks 1000, then 0100.
// This is how a service loads its palette in one command.
TEST(NaplpsInterpreter, SetColourRepeatsWalkTheMapByTheIncrementAlgorithm) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      // Mode 1 at address 0.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b000000))
      // Three one-byte colour words: green, red, blue.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b100000))
      .byte(numeric(0b010000))
      .byte(numeric(0b001000));

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_EQ(snapshot.colour_map[0].green, 10u);
  EXPECT_EQ(snapshot.colour_map[8].red, 10u)
      << "the first repeat lands at 1000";
  EXPECT_EQ(snapshot.colour_map[4].blue, 10u)
      << "the second repeat lands at 0100";
}

// §5.3.2.5: "A change in the color map will immediately be reflected in the
// color of all pixels whose associated color map address points to the color
// map entry that has been changed" — a map write recolours what was already
// drawn with that address, so the display list resolves mapped colours through
// the map as it finally stood.
TEST(NaplpsInterpreter, AMapWriteRecoloursWhatWasAlreadyDrawnWithItsAddress) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      // Mode 1 at address 0, draw a point, then rewrite entry 0.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b100000));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].colour_map_address, 0);
  EXPECT_EQ(snapshot.primitives[0].colour.green, 10u)
      << "the point kept the colour the entry held when it was drawn";
}

////////////////////////////////////////////////////////////////////////////////////////////
// Screen clears carry their colour (§6.1.2.6, §5.3.2.9.2 Table 15)
////////////////////////////////////////////////////////////////////////////////////////////

// §6.1.2.6: "In color mode 2, it clears the display area to the background
// color." The clear is recorded as a full-display-area rectangle so a renderer
// shows the ground the page was drawn on.
TEST(NaplpsInterpreter, ClearScreenInModeTwoRecordsTheBackgroundGround) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      // Mode 2: drawing address 0, background address 1.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b000000))
      .byte(numeric(0b000100))
      .byte(0x0C);  // CS

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const NabtsPrimitive& ground = snapshot.primitives[0];
  EXPECT_EQ(ground.kind, NabtsPrimitiveKind::kRectangle);
  EXPECT_TRUE(ground.filled);
  EXPECT_DOUBLE_EQ(ground.size.dx, 1.0);
  EXPECT_EQ(ground.colour, snapshot.colour_map[1]);
}

// Table 15 action 010: "Display area to current drawing color" — the ground a
// page that opens on a coloured background stands on.
TEST(NaplpsInterpreter, ResetClearingToTheDrawingColourRecordsThatGround) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kReset))
      .byte(numeric(0b010000));  // byte 1 b5 = 1: display to drawing colour

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].kind, NabtsPrimitiveKind::kRectangle);
  EXPECT_EQ(snapshot.primitives[0].colour, kNabtsNominalWhite)
      << "the default drawing colour is white";
}

////////////////////////////////////////////////////////////////////////////////////////////
// DEFP MACRO stores the program that ran (§6.2.2.2)
////////////////////////////////////////////////////////////////////////////////////////////

// §6.2.2.2: the body is "simultaneously executed and stored" — the *same*
// body. A PDI's operand bytes are consumed by the execution, and an
// implementation that stored only the lead bytes kept a different program from
// the one that just drew.
TEST(NaplpsInterpreter, ADefpMacroBodyReplaysExactlyWhenInvoked) {
  Record record;
  record
      .escape({code(4, 1)})  // DEFP MACRO
      .byte(code(2, 0))
      // The body designates the PDI set itself, so it draws the same whatever
      // the in-use table held when it was invoked — and the designation
      // sequence is itself lookahead the execution consumes, which the stored
      // body must keep.
      .escape({code(2, 9), code(5, 7)})  // G1 <- PDI
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b010, 0b011))
      .escape({code(4, 5)});  // END
  invoke_macro_20(record);

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u)
      << "once at definition, once at invocation";
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x,
                   snapshot.primitives[1].origin.x);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.y,
                   snapshot.primitives[1].origin.y);
  EXPECT_EQ(snapshot.diagnostics.unresolved_macros, 0u);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Blink processes (§5.3.2.7, §6.2.8)
////////////////////////////////////////////////////////////////////////////////////////////

// The regression the "TELETEXT is Animation!" record exposed. §5.3.2.7.2 runs a
// blink process on a colour map *entry* — it "periodically overwrites the
// contents of the current in-use drawing color" — so it is not a mode that
// everything drawn after the command inherits. That record opens with one BLINK
// and then draws a whole picture through 104 SELECT COLOR changes; read as a
// sticky flag, every one of its 159 primitives blinks and the display flashes
// whole.
TEST(NaplpsInterpreter, BlinkFollowsTheColourMapEntryNotTheDrawingOrder) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      // Mode 1 at address 3, and start a blink process on it: blink-to
      // address 6, ON 4 and OFF 6 tenths of a second, no start delay.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
      .byte(numeric(0b011000))
      .byte(numeric(4))
      .byte(numeric(6))
      .byte(numeric(0))
      // Drawn in entry 3, so it blinks.
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      // Move to entry 7 and draw again: §6.2.8.1 — "If the drawing color is
      // changed, the old color remains blinking and the new drawing color does
      // not blink."
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b011100))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b010, 0b010))
      // Back to entry 3: the process is still running there.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b011, 0b011));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  // The primitives name the entries they were drawn in; the process is on
  // the entry, not on them.
  EXPECT_EQ(snapshot.primitives[0].colour_map_address, 3);
  EXPECT_EQ(snapshot.primitives[1].colour_map_address, 7);
  EXPECT_EQ(snapshot.primitives[2].colour_map_address, 3);
  ASSERT_EQ(snapshot.timeline.blinks.size(), 1u);
  const NabtsBlinkProcess* process = running_on(snapshot, 3);
  ASSERT_NE(process, nullptr);
  EXPECT_EQ(running_on(snapshot, 7), nullptr)
      << "a primitive drawn in another map entry inherited the blink";
  // §5.3.2.7.3's blink-to address and intervals, in tenths of a second.
  EXPECT_EQ(process->to_address, 6);
  EXPECT_EQ(process->on_ms, 400u);
  EXPECT_EQ(process->off_ms, 600u);
}

// §6.2.8.1: "the blink-to color is nominal black in color modes 0 and 1 or the
// background color in color mode 2" — and nominal black is "the color that is
// at color map address zero in color modes 1 and 2", so in mode 1 the process
// blinks to entry 0. "The on and off intervals are implementation-dependent":
// a second of each, as the TTX 650 has them.
TEST(NaplpsInterpreter, C1BlinkStartBlinksToNominalBlackOutsideColourModeTwo) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      .escape({code(4, 14)})  // BLINK START
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001));

  const NabtsPageSnapshot snapshot = run(record);
  const NabtsBlinkProcess* process = running_on(snapshot, 3);
  ASSERT_NE(process, nullptr);
  EXPECT_EQ(process->to_address, 0) << "mode 1's nominal black is entry 0";
  EXPECT_EQ(process->to_colour, snapshot.timeline.initial_map[0]);
  EXPECT_EQ(process->on_ms, 1000u);
  EXPECT_EQ(process->off_ms, 1000u);
}

// In colour mode 2 the C1 form blinks to the background, which is an entry of
// its own.
TEST(NaplpsInterpreter, C1BlinkStartBlinksToTheBackgroundInColourModeTwo) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))  // drawing entry 3
      .byte(numeric(0b010100))  // background entry 5
      .escape({code(4, 14)});   // BLINK START

  const NabtsPageSnapshot snapshot = run(record);
  const NabtsBlinkProcess* process = running_on(snapshot, 3);
  ASSERT_NE(process, nullptr);
  EXPECT_EQ(process->to_address, 5);
}

// §6.2.8.1 on colour mode 0: "objects drawn with the previous drawing color
// will remain not blinking, but objects subsequently drawn with the new drawing
// color will blink" — so BLINK START gives the colour an entry of its own, and
// what was drawn before keeps the entry that holds still.
TEST(NaplpsInterpreter, ModeZeroBlinkStartBlinksAFreshEntry) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      // Mode 0, a colour the default map does not hold: GRB 2,0,0 in the
      // top bits of each gun, which mode 0 claims an entry for.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b100000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      .escape({code(4, 14)})  // BLINK START
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b010, 0b010));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  const int16_t before = snapshot.primitives[0].colour_map_address;
  const int16_t after = snapshot.primitives[1].colour_map_address;
  ASSERT_GE(before, 0) << "mode 0 found no entry for its colour";
  ASSERT_GE(after, 0);
  EXPECT_NE(before, after) << "the blink went onto the steady figure's entry";
  // Both show the same colour; only the second entry blinks.
  EXPECT_EQ(snapshot.primitives[0].colour, snapshot.primitives[1].colour);
  EXPECT_EQ(running_on(snapshot, static_cast<uint8_t>(before)), nullptr);
  EXPECT_NE(running_on(snapshot, static_cast<uint8_t>(after)), nullptr);
}

// §5.3.2.7.3: "An ON or OFF interval of 0 is taken to mean termination of any
// active blink process on the blink-from/blink-to color pair." Both intervals
// count — reading only the ON interval leaves a terminated process running.
TEST(NaplpsInterpreter, AZeroBlinkIntervalTerminatesTheProcess) {
  for (const auto& intervals :
       std::vector<std::pair<uint8_t, uint8_t>>{{0, 6}, {4, 0}}) {
    Record record;
    record.pdi()
        .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
        .byte(numeric(0b000000))
        .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
        .byte(numeric(0b001100))
        // A process first, so there is one for the zero interval to end.
        .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
        .byte(numeric(0b011000))
        .byte(numeric(4))
        .byte(numeric(6))
        .byte(numeric(0))
        .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
        .byte(numeric(0b011000))
        .byte(numeric(intervals.first))
        .byte(numeric(intervals.second))
        .byte(numeric(0));

    const NabtsPageSnapshot snapshot = run(record);
    EXPECT_EQ(running_on(snapshot, 3), nullptr)
        << "ON=" << static_cast<int>(intervals.first)
        << " OFF=" << static_cast<int>(intervals.second);
    ASSERT_EQ(snapshot.timeline.blinks.size(), 1u)
        << "a zero interval defined a process of its own";
  }
}

// §5.3.2.7.4: "Defining a blink process on a pair of blink-to and blink-from
// colors automatically terminates any previously defined blink process
// operating on the same pair of colors" — the same pair, so a second process
// on the entry to another colour runs beside it.
TEST(NaplpsInterpreter, ABlinkReplacesOnlyTheProcessOnItsOwnPair) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
      .add({numeric(0b011000), numeric(4), numeric(6), numeric(0)})
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
      .add({numeric(0b010100), numeric(4), numeric(6), numeric(0)})
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
      .add({numeric(0b011000), numeric(2), numeric(2), numeric(0)});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.timeline.blinks.size(), 3u);
  EXPECT_NE(snapshot.timeline.blinks[0].ended_ms, kNabtsNeverMs)
      << "the redefinition of 3 -> 6 left the old process running";
  EXPECT_EQ(snapshot.timeline.blinks[1].ended_ms, kNabtsNeverMs)
      << "3 -> 5 is another pair and should have been left alone";
  EXPECT_EQ(snapshot.timeline.blinks[2].ended_ms, kNabtsNeverMs);
  EXPECT_EQ(snapshot.timeline.blinks[2].on_ms, 200u);
}

// §6.2.8.2: BLINK STOP "turns off any currently active blink processes
// utilizing the drawing color as the blink-from color" — the drawing colour's
// alone, so a process on another entry survives it.
TEST(NaplpsInterpreter, BlinkStopEndsOnlyTheDrawingColoursProcess) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      .escape({code(4, 14)})  // BLINK START on entry 3
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b011100))
      .escape({code(4, 14)})  // BLINK START on entry 7
      .pdi()
      .escape({code(5, 14)})  // BLINK STOP, drawing colour still entry 7
      .pdi();

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_EQ(running_on(snapshot, 7), nullptr) << "entry 7 was stopped";
  EXPECT_NE(running_on(snapshot, 3), nullptr) << "entry 3 was never stopped";
}

// §5.3.2.7.4: "If no operands follow the blink opcode, then all blink
// processes utilizing the current drawing color as the blink-from color will
// be terminated" — every one on that entry, whatever it blinks to.
TEST(NaplpsInterpreter, BlinkWithNoOperandsEndsEveryProcessOnTheEntry) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
      .add({numeric(0b011000), numeric(4), numeric(6), numeric(0)})
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
      .add({numeric(0b010100), numeric(4), numeric(6), numeric(0)})
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.timeline.blinks.size(), 2u);
  EXPECT_EQ(running_on(snapshot, 3), nullptr);
}

// §5.3.2.9.3: RESET byte 2 b2 — "all blink processes are terminated".
TEST(NaplpsInterpreter, ResetEndsEveryBlinkProcess) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      .escape({code(4, 14)})
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b011100))
      .escape({code(4, 14)})
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kReset))
      .byte(numeric(0))
      .byte(numeric(0b000010));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.timeline.blinks.size(), 2u);
  EXPECT_EQ(running_on(snapshot, 3), nullptr);
  EXPECT_EQ(running_on(snapshot, 7), nullptr);
}

// §5.3.2.7.5: operands beyond one complete process repeat the command "with the
// address of the blink-from color being automatically incremented (as in
// 5.3.2.5.1) ... The drawing color is not affected by this incrementing."
// §5.3.2.5.1's increment turns the most significant zero into a one, so from
// entry 0 the next blink-from is entry 8.
TEST(NaplpsInterpreter, BlinkRepeatsOntoTheIncrementedBlinkFromAddress) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b000000))  // mode 1 at entry 0
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
      // First process: blink-to 6, ON 4, OFF 6, delay 0.
      .byte(numeric(0b011000))
      .byte(numeric(4))
      .byte(numeric(6))
      .byte(numeric(0))
      // Second, implicitly on the incremented blink-from address.
      .byte(numeric(0b011000))
      .byte(numeric(4))
      .byte(numeric(6))
      .byte(numeric(0));

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_NE(running_on(snapshot, 0), nullptr);
  EXPECT_NE(running_on(snapshot, 8), nullptr);
  // "The drawing color is not affected by this incrementing."
  NaplpsInterpreter interpreter;
  (void)interpreter.run(record.data());
  EXPECT_EQ(interpreter.state().colour.drawing_address(), 0u);
}

// §5.3.2.7.2: a start delay is "a delay in the start of the ON interval
// referenced to the start of the ON interval of the most recently defined
// active blink process". Measured from the ON interval under way where the
// newest process is in one, and from its next where it is between them.
TEST(NaplpsInterpreter, AStartDelayIsMeasuredFromTheNewestProcess) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b000100))  // entry 1
      // A: ON 5, OFF 5, starting now.
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
      .add({numeric(0b011000), numeric(5), numeric(5), numeric(0)})
      .add(wait({2}))
      // B, 0.2 s in — A is in its ON interval, which started at 0, so B's
      // start a tenth after that has already gone by.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001000))  // entry 2
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
      .add({numeric(0b011000), numeric(5), numeric(5), numeric(1)})
      .add(wait({5}))
      // C, 0.7 s in — B, the newest, has not started yet, so its first ON
      // interval, at 1.1 s, is the reference.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))  // entry 3
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
      .add({numeric(0b011000), numeric(5), numeric(5), numeric(2)});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.timeline.blinks.size(), 3u);
  const auto& blinks = snapshot.timeline.blinks;
  EXPECT_EQ(blinks[0].first_on_ms, 0);
  EXPECT_EQ(blinks[1].defined_ms, 200);
  // Both receivers of the day wait for A's next ON interval, at 1 s, rather
  // than start B part way through an ON interval it was not there for.
  EXPECT_EQ(blinks[1].first_on_ms, 1100)
      << "B's ON interval is in step with A's, a tenth behind";
  EXPECT_EQ(blinks[2].defined_ms, 700);
  EXPECT_EQ(blinks[2].first_on_ms, 1300);
}

// The Norpak TTX 650 and the Panasonic TU-2000x, run as firmware: entry 12
// blinks to entry 8 a second on and a second off; 0.3 s later entry 13 blinks
// to 8 with a start delay of 0.2 s. The first process's ON interval began at
// 0, so the second's start, 0.2 s, has gone by; both receivers turn entry 13
// on at the first process's next ON interval and the delay, 2.2 s (2.23 and
// 2.43 s on their own ticks), where a start part way through the first ON
// interval would have it on at 0.3 s.
TEST(NaplpsInterpreter, ABlinkWhoseStartHasGoneByWaitsForTheNextOnInterval) {
  for (const uint8_t delay : {uint8_t{2}, uint8_t{0}}) {
    Record record;
    record.pdi()
        .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
        .byte(numeric(0b110000))  // mode 1, entry 12
        .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
        .add({numeric(0b100000), numeric(10), numeric(10), numeric(0)})
        .add(wait({3}))
        .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
        .byte(numeric(0b110100))  // entry 13
        .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
        .add({numeric(0b100000), numeric(10), numeric(10), numeric(delay)});

    const NabtsPageSnapshot snapshot = run(record);
    ASSERT_EQ(snapshot.timeline.blinks.size(), 2u);
    EXPECT_EQ(snapshot.timeline.blinks[1].first_on_ms, 2000 + delay * 100)
        << int{delay};
  }
}

// Both receivers step their blink processes on a 10 Hz tick, so one defined
// with nothing running starts on the page clock's next tick: here after the
// shortest WAIT, two fields (33 ms) in, at 100 ms.
TEST(NaplpsInterpreter, ABlinkWithNothingRunningStartsOnTheNextTick) {
  Record record;
  record.pdi()
      .add(wait({0}))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
      .add({numeric(0b011000), numeric(5), numeric(5), numeric(0)});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.timeline.blinks.size(), 1u);
  EXPECT_EQ(snapshot.timeline.blinks[0].defined_ms, 33);
  EXPECT_EQ(snapshot.timeline.blinks[0].first_on_ms, 100);
}

// "A start delay specification when there are no active blink processes has
// no effect."
TEST(NaplpsInterpreter, AStartDelayWithNothingRunningHasNoEffect) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
      .add({numeric(0b011000), numeric(5), numeric(5), numeric(9)});

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.timeline.blinks.size(), 1u);
  EXPECT_EQ(snapshot.timeline.blinks[0].first_on_ms, 0);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Presentation time (§5.3.2.8)
////////////////////////////////////////////////////////////////////////////////////////////

// §5.3.2.8.2: after the fixed format byte, "The next byte of operand data gives
// the time delay in units of 1/10 of a second ... If any additional data bytes
// follow, they are treated as additional periods of waiting time".
TEST(NaplpsInterpreter, WaitStagesTheDrawing) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      .add(wait({2, 3}))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b010, 0b010))
      .text_mode();  // the code that ends the command

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_EQ(snapshot.primitives[0].appear_ms, 0u);
  EXPECT_EQ(snapshot.primitives[1].appear_ms, 500u);
  EXPECT_EQ(snapshot.timeline.end_ms, 500u);
}

// §5.3.1 leaves a drawing that would leave the unit screen to the receiver —
// "rejected (ie, executed as a null operation) or executed and clipped" — and a
// polygon, arc or spline is rejected, as the Panasonic TU-2000x rejects them:
// a vertex off the screen is almost always an operand read across a lost
// stretch, and the figure it makes would flood the page. The drawing point
// still ends where the command leaves it — a polygon's start point.
TEST(NaplpsInterpreter, APolygonWithAVertexOffTheScreenIsNotDrawn) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetPolyFilled))
      .add(coord3_signed(0.25, 0.25))
      .add(coord3_signed(0.9, 0.0))  // to x = 1.15
      .add(coord3_signed(-0.5, 0.25))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .text_mode();

  const NabtsPageSnapshot snapshot = run(record);
  for (const NabtsPrimitive& primitive : snapshot.primitives) {
    EXPECT_NE(primitive.kind, NabtsPrimitiveKind::kPolygon);
  }
  EXPECT_EQ(snapshot.diagnostics.rejected_figures, 1u);
}

TEST(NaplpsInterpreter, AnArcThroughAPointOffTheScreenIsNotDrawn) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetArcFilled))
      .add(coord3_signed(0.6, 0.3))
      .add(coord3_signed(0.3, 0.2))   // to x = 0.9
      .add(coord3_signed(0.3, -0.2))  // to x = 1.2
      .text_mode();

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_TRUE(snapshot.primitives.empty());
  EXPECT_EQ(snapshot.diagnostics.rejected_figures, 1u);
}

// Rectangles, lines and points are clipped instead, as both receivers of the
// day clip them; and the unit screen runs above the display area (§4.2.2), so
// a vertex up there is on it.
TEST(NaplpsInterpreter, OtherFiguresOffTheScreenAreClipped) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetRectFilled))
      .add(coord3_signed(0.75, 0.25))
      .add(coord3_signed(0.5, 0.25))  // to x = 1.25
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetPolyFilled))
      .add(coord3_signed(0.25, 0.25))
      .add(coord3_signed(0.25, 0.65))  // to y = 0.9
      .add(coord3_signed(0.1, -0.5))
      .text_mode();

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_EQ(snapshot.primitives[0].kind, NabtsPrimitiveKind::kRectangle);
  EXPECT_EQ(snapshot.primitives[1].kind, NabtsPrimitiveKind::kPolygon);
  EXPECT_EQ(snapshot.diagnostics.rejected_figures, 0u);
}

/// |text| run in TEXT's inter-character spacing code 3, with |text_byte1|'s
/// rotation and path, from the middle of the screen, and where the cursor
/// started and ended.
struct ProportionalRun {
  NabtsPageSnapshot snapshot;
  NabtsPoint start;
  NabtsPoint end;
};

ProportionalRun run_proportional(const char* text,
                                 uint8_t text_byte1 = 0b110000) {
  const auto cursor_after = [&](const char* characters,
                                NabtsPageSnapshot* snapshot) {
    Record record;
    // Mid-screen, clear of every edge the path could wrap at.
    record.pdi()
        .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
        .add(coord3(0.5, 0.5))
        .byte(static_cast<uint8_t>(NaplpsPdi::kText))
        .byte(numeric(text_byte1))
        .byte(numeric(0))
        .text_mode()
        .text(characters);
    NaplpsInterpreter interpreter;
    NabtsPageSnapshot page = interpreter.run(record.data());
    if (snapshot != nullptr) {
      *snapshot = std::move(page);
    }
    return interpreter.state().cursor;
  };
  ProportionalRun run;
  run.start = cursor_after("", nullptr);
  run.end = cursor_after(text, &run.snapshot);
  return run;
}

// TEXT's inter-character spacing code 3 is proportional spacing, which T.101
// Annex D's Addendum lays down (clause 11) and both receivers of the day
// follow: each character moves the cursor by its width class. In the default
// field, 6.4 pixels wide, an i moves it 2 pixels, an a 5 and an M 6. The
// receivers also put each character's ink at the cursor, so the i, whose stem
// is a column into its 6 by 10 cell, has its field a pixel back.
TEST(NaplpsInterpreter, ProportionalTextMovesEachCharacterByItsWidthClass) {
  const ProportionalRun run = run_proportional("iaM");
  ASSERT_EQ(run.snapshot.primitives.size(), 3u);
  EXPECT_NEAR(run.end.x - run.start.x, (2.0 + 5.0 + 6.0) / 256.0, 1e-9);
  EXPECT_NEAR(run.end.y, run.start.y, 1e-12);
  EXPECT_NEAR(run.snapshot.primitives[0].origin.x, run.start.x - 1.0 / 256.0,
              1e-9);
  EXPECT_NEAR(run.snapshot.primitives[1].origin.x, run.start.x + 2.0 / 256.0,
              1e-9);
  EXPECT_NEAR(run.snapshot.primitives[2].origin.x, run.start.x + 7.0 / 256.0,
              1e-9);
}

// Both receivers space a path that runs against the character's reading — to
// the left at 0 degrees — by the field, proportional or not.
TEST(NaplpsInterpreter, ProportionalTextAgainstItsReadingStepsTheField) {
  const ProportionalRun run = run_proportional("ii", 0b110100);  // path left
  EXPECT_NEAR(run.end.x - run.start.x, -2.0 / 40.0, 1e-9);
  EXPECT_NEAR(run.snapshot.primitives[0].origin.x, run.start.x, 1e-12)
      << "nothing moved back for its ink";
}

// T.101 Annex D Addendum, clause 11.1: "The format effector characters APB and
// APF in proportional space mode are considered to have a width class of 9
// (full width)", so APF moves 6 pixels in the default field, not 6.4.
TEST(NaplpsInterpreter, ProportionalApfMovesTheFullWidthClass) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kText))
      .byte(numeric(0b110000))
      .byte(numeric(0))
      .text_mode();
  NaplpsInterpreter before;
  (void)before.run(record.data());
  record.byte(kNaplpsApf);
  NaplpsInterpreter after;
  (void)after.run(record.data());
  EXPECT_NEAR(after.state().cursor.x - before.state().cursor.x, 6.0 / 256.0,
              1e-9);
}

// Clause 11.1: "The width class of a composite character created by the use of
// non-spacing character together with a spacing character, is determined by
// the maximum width class of its components" — an i under a grave (class 2)
// moves 4 pixels, not the i's 2, and stays in the field its mark was drawn
// in, where the two are composed.
TEST(NaplpsInterpreter, ProportionalCompositeTakesTheWiderClass) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kText))
      .byte(numeric(0b110000))
      .byte(numeric(0))
      .text_mode();
  NaplpsInterpreter before;
  (void)before.run(record.data());
  record.byte(kNaplpsSs2).byte(0x41).text("i");
  NaplpsInterpreter after;
  const NabtsPageSnapshot snapshot = after.run(record.data());
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_NEAR(after.state().cursor.x - before.state().cursor.x, 4.0 / 256.0,
              1e-9);
  EXPECT_NEAR(snapshot.primitives[1].origin.x, snapshot.primitives[0].origin.x,
              1e-12);
}

NabtsPageSnapshot run_at_receiver_pace(const Record& record) {
  NaplpsInterpreter interpreter;
  interpreter.set_receiver_pace(NaplpsReceiverPace::kNorpakTtx650);
  return interpreter.run(record.data());
}

// At the receiver's pace a character takes the 3.4 ms the TTX 650 took, and
// the part of a millisecond left over is carried to the next, so four
// characters are on the screen by 13 ms rather than 12.
TEST(NaplpsInterpreter, ReceiverPaceDrawsACharacterIn3Point4Ms) {
  Record record;
  record.text("ABCD");

  const NabtsPageSnapshot snapshot = run_at_receiver_pace(record);
  ASSERT_EQ(snapshot.primitives.size(), 4u);
  EXPECT_EQ(snapshot.primitives[0].appear_ms, 3u);
  EXPECT_EQ(snapshot.primitives[1].appear_ms, 6u);
  EXPECT_EQ(snapshot.primitives[2].appear_ms, 10u);
  EXPECT_EQ(snapshot.primitives[3].appear_ms, 13u);
  EXPECT_EQ(snapshot.timeline.end_ms, 13u);

  // Off, the same characters appear at once.
  for (const NabtsPrimitive& primitive : run(record).primitives) {
    EXPECT_EQ(primitive.appear_ms, 0u);
  }
}

// Every command takes its own time and a filled figure its rows and pixels
// besides, as far as they are on the display (256 pixels to the unit screen):
// after SO's 0.3 ms, the first RECT FILLED takes 3.5 ms for itself and 46.9 ms
// for 64 rows and 8,192 pixels, so it is on the screen at 50 ms. POINT SET
// takes 3.6 ms; the second rectangle, from (0.75, 0.625), shows only 40 rows
// and 2,560 pixels of itself, 3.5 ms and 23.5 ms more, so 81 ms.
TEST(NaplpsInterpreter, ReceiverPaceCostsAFilledFigureByItsPixelsOnScreen) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .add(coord3(0.5, 0.25))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .add(coord3(0.75, 0.625))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .add(coord3(0.5, 0.25))
      .text_mode();

  const NabtsPageSnapshot snapshot = run_at_receiver_pace(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_EQ(snapshot.primitives[0].appear_ms, 50u);
  EXPECT_EQ(snapshot.primitives[1].appear_ms, 81u);
}

// The pace adds to a WAIT rather than replacing it: the pause starts once what
// came before it is drawn. "A" at 3.4 ms, SO 0.3 ms and the WAIT command 2 ms
// for itself before its 200 ms pause, SI 0.3 ms, and "B" 3.4 ms more.
TEST(NaplpsInterpreter, ReceiverPaceRunsBeforeAWait) {
  Record record;
  record.text("A").pdi().add(wait({2})).text_mode().text("B");

  const NabtsPageSnapshot snapshot = run_at_receiver_pace(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_EQ(snapshot.primitives[0].appear_ms, 3u);
  EXPECT_EQ(snapshot.primitives[1].appear_ms, 209u);
}

// A clear takes the receiver 169 ms in every colour mode, and nothing more for
// the colour it floods the display with.
TEST(NaplpsInterpreter, ReceiverPaceTakesAClearOnce) {
  for (const bool mapped : {false, true}) {
    Record record;
    if (mapped) {
      record.pdi()
          .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
          .byte(numeric(0b001100))  // mode 1, entry 3
          .text_mode();
    }
    record.text("A").byte(kNaplpsCs).text("B");

    const NabtsPageSnapshot snapshot = run_at_receiver_pace(record);
    ASSERT_EQ(snapshot.timeline.clears.size(), 1u) << mapped;
    const uint32_t cleared = snapshot.timeline.clears[0].time_ms;
    const NabtsPrimitive& after = snapshot.primitives.back();
    EXPECT_EQ(after.kind, NabtsPrimitiveKind::kCharacter) << mapped;
    // Drawn 169 ms after "A", give or take the carried fraction, and 3.4 ms
    // after the clear.
    EXPECT_EQ(cleared - snapshot.timeline.superseded.back().appear_ms, 169u)
        << mapped;
    EXPECT_EQ(after.appear_ms - cleared, 3u) << mapped;
  }
}

// Both receivers of the day reset the colour map before they clear the screen:
// a RESET that does both shows the old picture in the default colours for as
// long as the clear takes, 169 ms on the TTX 650 — which a More Record or a
// rolling sub-page drawn over its predecessor shows.
TEST(NaplpsInterpreter, ReceiverPaceResetsTheMapBeforeItClears) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))  // mode 1, entry 3
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b110000))  // entry 3 loaded
      .byte(static_cast<uint8_t>(NaplpsPdi::kReset))
      .byte(numeric(0b001110))  // the default map in mode 1; clear to black
      .byte(numeric(0))
      .text_mode();

  const NabtsPageSnapshot snapshot = run_at_receiver_pace(record);
  ASSERT_EQ(snapshot.timeline.clears.size(), 1u);
  const int64_t cleared = snapshot.timeline.clears[0].time_ms;
  const auto reset = std::find_if(
      snapshot.timeline.events.begin(), snapshot.timeline.events.end(),
      [&](const NabtsPaletteEvent& event) {
        return event.kind == NabtsPaletteEvent::Kind::kMapWrite &&
               event.address == 3 &&
               event.colour == snapshot.timeline.initial_map[3];
      });
  ASSERT_NE(reset, snapshot.timeline.events.end());
  EXPECT_EQ(cleared - reset->time_ms, 169);
}

// Both receivers of the day show SET COLOR's colour the moment it runs, so at
// their pace a map write is on the clock where it was made — before what is
// drawn after it in the same stage — not at the stage's end.
TEST(NaplpsInterpreter, ReceiverPaceShowsAColourWhenItIsSet) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))  // mode 1, entry 3
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b110000))
      .text_mode()
      .text("ABCDEFGH");

  const NabtsPageSnapshot snapshot = run_at_receiver_pace(record);
  const auto write = std::find_if(
      snapshot.timeline.events.begin(), snapshot.timeline.events.end(),
      [](const NabtsPaletteEvent& event) {
        return event.kind == NabtsPaletteEvent::Kind::kMapWrite &&
               event.address == 3;
      });
  ASSERT_NE(write, snapshot.timeline.events.end());
  ASSERT_EQ(snapshot.primitives.size(), 8u);
  EXPECT_LE(write->time_ms, snapshot.primitives.front().appear_ms);
  EXPECT_LT(write->time_ms, snapshot.timeline.end_ms);
}

NabtsPageSnapshot run_at_tu2000x_pace(const Record& record) {
  NaplpsInterpreter interpreter;
  interpreter.set_receiver_pace(NaplpsReceiverPace::kPanasonicTu2000x);
  return interpreter.run(record.data());
}

// At the Panasonic TU-2000x's pace a character takes the 4.7 ms its firmware
// took, the part of a millisecond left over carried to the next.
TEST(NaplpsInterpreter, TheTu2000xPaceDrawsACharacterIn4Point7Ms) {
  Record record;
  record.text("ABCD");

  const NabtsPageSnapshot snapshot = run_at_tu2000x_pace(record);
  ASSERT_EQ(snapshot.primitives.size(), 4u);
  EXPECT_EQ(snapshot.primitives[0].appear_ms, 4u);
  EXPECT_EQ(snapshot.primitives[1].appear_ms, 9u);
  EXPECT_EQ(snapshot.primitives[2].appear_ms, 14u);
  EXPECT_EQ(snapshot.primitives[3].appear_ms, 18u);
}

// The TU-2000x loads a colour in the next vertical blank: SET COLOR takes
// 8.4 ms, half a field on average, before what follows it is drawn.
TEST(NaplpsInterpreter, TheTu2000xPaceWaitsForTheFieldToLoadAColour) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b111111))
      .text_mode()
      .text("A");

  // SO and SI 0.36 ms each, SET COLOR 8.4, "A" 4.7: 13.82 ms.
  const NabtsPageSnapshot snapshot = run_at_tu2000x_pace(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].appear_ms, 13u);
}

// A figure records how long the receiver took over it, ending where it
// appears, which is the time a fill is swept onto the screen over: the first
// rectangle of ReceiverPaceCostsAFilledFigureByItsPixelsOnScreen takes its
// 46.9 ms from 3 ms to 50, the second its 23.5 from 57 to 81.
TEST(NaplpsInterpreter, ReceiverPaceRecordsHowLongAFigureTook) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .add(coord3(0.5, 0.25))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .add(coord3(0.75, 0.625))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .add(coord3(0.5, 0.25))
      .text_mode();

  const NabtsPageSnapshot snapshot = run_at_receiver_pace(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_EQ(snapshot.primitives[0].draw_ms, 47u);
  EXPECT_EQ(snapshot.primitives[1].draw_ms, 24u);
  EXPECT_EQ(snapshot.timeline.fill_sweep, NabtsFillSweep::kBottomUp)
      << "the Norpak TTX 650 swept up its screen";
  EXPECT_EQ(run_at_tu2000x_pace(record).timeline.fill_sweep,
            NabtsFillSweep::kTopDown)
      << "the Panasonic TU-2000x swept down its screen";
}

// A clear records how long it took the receiver — 169 ms on the TTX 650, 70
// on the TU-2000x — and so does the colour it floods the display with.
TEST(NaplpsInterpreter, ReceiverPaceRecordsHowLongAClearTook) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))  // mode 1, entry 3
      .text_mode();
  record.text("A").byte(kNaplpsCs).text("B");

  const NabtsPageSnapshot ttx = run_at_receiver_pace(record);
  ASSERT_EQ(ttx.timeline.clears.size(), 1u);
  EXPECT_EQ(ttx.timeline.clears[0].draw_ms, 169u);
  const NabtsPageSnapshot tu = run_at_tu2000x_pace(record);
  ASSERT_EQ(tu.timeline.clears.size(), 1u);
  EXPECT_EQ(tu.timeline.clears[0].draw_ms, 70u);
  for (const NabtsPrimitive& primitive : tu.primitives) {
    if (primitive.kind == NabtsPrimitiveKind::kRectangle && primitive.filled) {
      EXPECT_EQ(primitive.draw_ms, 70u) << "the flood is the clear";
    }
  }
}

// Without a receiver's pace everything appears at once: nothing records a
// drawing time.
TEST(NaplpsInterpreter, WithoutAPaceNothingTakesTimeToDraw) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .add(coord3(0.5, 0.25))
      .text_mode()
      .text("A")
      .byte(kNaplpsCs)
      .text("B");

  const NabtsPageSnapshot snapshot = run(record);
  for (const NabtsPrimitive& primitive : snapshot.timeline.superseded) {
    EXPECT_EQ(primitive.draw_ms, 0u);
  }
  for (const NabtsPrimitive& primitive : snapshot.primitives) {
    EXPECT_EQ(primitive.draw_ms, 0u);
  }
  ASSERT_EQ(snapshot.timeline.clears.size(), 1u);
  EXPECT_EQ(snapshot.timeline.clears[0].draw_ms, 0u);
}

// §5.3.2.8.2: "If any other bit combination follows the WAIT opcode, the
// entire command is reserved for future standardization and shall be executed
// as a null operation" — whatever shape the rest of it has. 6/12 is the
// commonest way the tapes damage 5/12, and both receivers of the day drew
// straight through it.
TEST(NaplpsInterpreter, AWaitWithoutItsFormatByteIsNothing) {
  for (const std::vector<uint8_t>& operands :
       {std::vector<uint8_t>{numeric(0x2C), numeric(20)},
        std::vector<uint8_t>{numeric(0x2C), numeric(20), numeric(20)},
        std::vector<uint8_t>{numeric(0)}}) {
    Record record;
    record.pdi().byte(static_cast<uint8_t>(NaplpsPdi::kWait)).add(operands);
    record.byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
        .byte(coord1(0b001, 0b001))
        .text_mode();  // the code that ends the command

    const NabtsPageSnapshot snapshot = run(record);
    ASSERT_EQ(snapshot.primitives.size(), 1u);
    EXPECT_EQ(snapshot.primitives[0].appear_ms, 0u);
    EXPECT_EQ(snapshot.timeline.end_ms, 0u);
  }
}

// "An operand of zero indicates a wait interval between zero and 1/10 of a
// second (inclusive) that is implementation-dependent", and Table D1 item 5(6)
// puts it for teletext "between two complete TV fields (approximately 1/30 s)
// and three".
TEST(NaplpsInterpreter, AZeroWaitIsTwoFields) {
  Record record;
  record.pdi().add(wait({0}));
  EXPECT_EQ(run(record).timeline.end_ms, 33u);
}

// The clock stops at fifteen minutes, so a damaged WAIT cannot push the rest of
// a page out of reach.
TEST(NaplpsInterpreter, ThePresentationClockStops) {
  Record record;
  record.pdi();
  for (int i = 0; i < 20; ++i) {
    record.add(wait({63, 63, 63, 63, 63, 63, 63, 63, 63, 63}));
  }
  EXPECT_EQ(run(record).timeline.end_ms, 15u * 60u * 1000u);
}

// A clear removes what was drawn, but it was on screen until then: the
// timeline keeps it, in paint order, with where the clear fell.
TEST(NaplpsInterpreter, AClearKeepsWhatItRemovedInTheTimeline) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      .add(wait({10}))
      .byte(0x0C)  // CS
      .add(wait({5}))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b010, 0b010))
      .text_mode();  // the code that ends the command

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.timeline.superseded.size(), 1u);
  EXPECT_EQ(snapshot.timeline.superseded[0].appear_ms, 0u);
  ASSERT_EQ(snapshot.timeline.clears.size(), 1u);
  EXPECT_EQ(snapshot.timeline.clears[0].time_ms, 1000u);
  EXPECT_EQ(snapshot.timeline.clears[0].stream_index, 1u);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].appear_ms, 1500u);
}

// A map write is on the clock too: §5.3.2.5 has it "immediately reflected" on
// screen, so a page that loads an entry after a pause changes colour then.
TEST(NaplpsInterpreter, MapWritesAreStampedWithTheirTime) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))  // mode 1, entry 3
      .add(wait({10}))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b110000));

  const NabtsPageSnapshot snapshot = run(record);
  const auto write = std::find_if(
      snapshot.timeline.events.begin(), snapshot.timeline.events.end(),
      [](const NabtsPaletteEvent& event) {
        return event.kind == NabtsPaletteEvent::Kind::kMapWrite &&
               event.address == 3;
      });
  ASSERT_NE(write, snapshot.timeline.events.end());
  EXPECT_EQ(write->time_ms, 1000);
  EXPECT_EQ(write->colour, snapshot.colour_map[3]);
}

// §5.3.2.5.1: "Color mode 0 also uses the color map" — a mode-0 colour is
// found in the map or given an entry, and that entry is what the pixel stores,
// so a later write to it reaches what mode 0 drew.
TEST(NaplpsInterpreter, ModeZeroDrawsThroughTheEntryItFoundOrClaimed) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b100000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001));

  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot snapshot = interpreter.run(record.data());
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const int16_t entry = snapshot.primitives[0].colour_map_address;
  ASSERT_GE(entry, 0);
  EXPECT_EQ(snapshot.colour_map[entry], snapshot.primitives[0].colour);

  // Rewriting that entry in mode 1 recolours the mode-0 point.
  Record rewrite;
  rewrite.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(static_cast<uint8_t>(entry << 2)))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b000011));
  const NabtsPageSnapshot after = interpreter.run(rewrite.data(), true);
  ASSERT_EQ(after.primitives.size(), 1u);
  EXPECT_EQ(after.primitives[0].colour, after.colour_map[entry]);
  EXPECT_FALSE(after.primitives[0].colour == snapshot.primitives[0].colour);
}

// §5.3.2.5.1: a mode-0 colour the map already holds is drawn through the
// lowest entry holding it, and that entry has then "been used (via a color mode
// 0 ... SET COLOR command)" — so the next colour mode 0 has to find room for
// takes another entry, and what was drawn in the found one keeps its colour.
// (Pages draw their backgrounds in a default-map grey, then bring in new
// colours; both receivers of the day keep the grey.)
TEST(NaplpsInterpreter, ModeZeroNeverClaimsAnEntryItFoundItsColourIn) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b000100))  // a green the default map lacks: claims 1
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b000111))  // grey, 2 of 7: the default map's entry 2
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b010, 0b010))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b000010))  // a red the map lacks
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b011, 0b011));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  const NabtsPrimitive& grey = snapshot.primitives[1];
  ASSERT_EQ(grey.colour_map_address, 2);
  EXPECT_EQ(snapshot.colour_map[2], grey.colour);
  EXPECT_EQ(snapshot.primitives[0].colour_map_address, 1);
  EXPECT_EQ(snapshot.primitives[2].colour_map_address, 3);
}

// §6.1.6.5(5): NSR sets "the drawing color ... to nominal white" in mode 0,
// and mode 0 draws through the entry holding the colour — so the drawing
// address has to follow it to white rather than stay where it was.
TEST(NaplpsInterpreter, NsrPointsTheDrawingAddressAtWhite) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))  // mode 1, entry 3
      .byte(0x1F)               // NSR
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      .text_mode();  // the code that ends the command

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].colour, kNabtsNominalWhite);
  const int16_t entry = snapshot.primitives[0].colour_map_address;
  ASSERT_GE(entry, 0);
  EXPECT_EQ(snapshot.colour_map[entry], kNabtsNominalWhite);
}

// A blink process nothing terminated is still running when the next record
// starts — NSR leaves processes alone (§6.1.6.5) and a More Record follows its
// predecessor straight on (CEA-516 §5.2.7.8) — so it carries in, moved back by
// the length of the record that defined it.
TEST(NaplpsInterpreter, ABlinkProcessCarriesIntoTheNextRecord) {
  Record first;
  first.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
      .add({numeric(0b011000), numeric(5), numeric(5), numeric(0)})
      .add(wait({12}));
  Record second;
  second.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001));

  NaplpsInterpreter interpreter;
  (void)interpreter.run(first.data());
  const NabtsPageSnapshot snapshot = interpreter.run(second.data(), true);
  const NabtsBlinkProcess* process = running_on(snapshot, 3);
  ASSERT_NE(process, nullptr);
  EXPECT_EQ(process->defined_ms, -1200);
  EXPECT_EQ(process->first_on_ms, -1200);
  ASSERT_FALSE(snapshot.timeline.events.empty());
  EXPECT_EQ(snapshot.timeline.events.front().kind,
            NabtsPaletteEvent::Kind::kBlinkStart);
  EXPECT_EQ(snapshot.timeline.events.front().time_ms, -1200);

  // The general reset ends it.
  interpreter.reset_decoder();
  EXPECT_TRUE(interpreter.run(second.data()).timeline.blinks.empty());
}

// A More Record is drawn over the display its predecessor left, which is on
// screen from the moment it starts.
TEST(NaplpsInterpreter, TheStandingDisplayIsThereFromTheStart) {
  Record first;
  first.pdi()
      .add(wait({10}))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      .text_mode();  // the code that ends the command
  Record second;
  second.pdi()
      .add(wait({3}))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b010, 0b010))
      .text_mode();  // the code that ends the command

  NaplpsInterpreter interpreter;
  (void)interpreter.run(first.data());
  const NabtsPageSnapshot snapshot = interpreter.run(second.data(), true);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_EQ(snapshot.primitives[0].appear_ms, 0u);
  EXPECT_EQ(snapshot.primitives[1].appear_ms, 300u);
}

////////////////////////////////////////////////////////////////////////////////////////////
// INCREMENTAL POINT (§5.3.3.6.3)
////////////////////////////////////////////////////////////////////////////////////////////

/// A record opening in the default three-byte domain with a logical pel of
/// |pel_x| by |pel_y| 256ths, the drawing point at (|x|, |y|), colour mode 1 at
/// entry 1.
Record incremental_setup(double pel_x, double pel_y, double x, double y) {
  Record record;
  record
      .pdi()
      // DOMAIN: single-value one byte, multi-value three bytes, then the pel.
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b001000))
      .add(coord3(pel_x / 256.0, pel_y / 256.0))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b000100))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .add(coord3(x, y));
  return record;
}

std::vector<const NabtsPrimitive*> of_kind(const NabtsPageSnapshot& snapshot,
                                           NabtsPrimitiveKind kind) {
  std::vector<const NabtsPrimitive*> out;
  for (const NabtsPrimitive& primitive : snapshot.primitives) {
    if (primitive.kind == kind) {
      out.push_back(&primitive);
    }
  }
  return out;
}

// "The range of values of the packing counter shall be 1 to 48, inclusive.
// Values of 0 or greater than 48 are reserved for future standardization and
// the entire INCREMENTAL POINT command containing packing counters with such
// values shall be executed as a null operation."
TEST(NaplpsInterpreter, AReservedPackingCountIsANullOperation) {
  for (const uint8_t packing : {uint8_t{0}, uint8_t{49}, uint8_t{63}}) {
    Record record = incremental_setup(8, 8, 0.5, 0.5);
    record.byte(static_cast<uint8_t>(NaplpsPdi::kIncrPoint))
        .byte(numeric(packing))
        .byte(numeric(0b111111));
    const NabtsPageSnapshot snapshot = run(record);
    EXPECT_TRUE(
        of_kind(snapshot, NabtsPrimitiveKind::kIncrementalPoints).empty())
        << "packing " << static_cast<int>(packing);
  }
}

// "stored sequentially without regard to byte boundaries, high order bit (b6)
// to low order bit (b1)", a packing counter's worth of bits apiece — and in
// colour modes 1 and 2 an address, left-justified as every map address is.
TEST(NaplpsInterpreter, IncrementalPointPacksColoursAcrossBytes) {
  Record record = incremental_setup(8, 8, 0.5, 0.5);
  record.byte(static_cast<uint8_t>(NaplpsPdi::kIncrPoint))
      .byte(numeric(4))
      .byte(numeric(0b000100))   // 0001 00...
      .byte(numeric(0b100011));  // ...10 0011
  const NabtsPageSnapshot snapshot = run(record);
  const auto rows = of_kind(snapshot, NabtsPrimitiveKind::kIncrementalPoints);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0]->incremental_addresses, (std::vector<int16_t>{1, 2, 3}));
  // "The operation starts at the current drawing point."
  EXPECT_DOUBLE_EQ(rows[0]->origin.x, 0.5);
  EXPECT_DOUBLE_EQ(rows[0]->origin.y, 0.5);
  EXPECT_DOUBLE_EQ(rows[0]->logical_pel.dx, 8.0 / 256.0);
  // And the colours are the entries', resolved through the final map.
  EXPECT_EQ(rows[0]->incremental_colours[1], snapshot.colour_map[2]);
}

// In colour mode 0 a specification is a colour value: three-tuples G R B, the
// first holding the most significant bits.
TEST(NaplpsInterpreter, IncrementalPointInModeZeroCarriesValues) {
  Record record = incremental_setup(8, 8, 0.5, 0.5);
  record
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))  // mode 0
      .byte(static_cast<uint8_t>(NaplpsPdi::kIncrPoint))
      .byte(numeric(6))
      .byte(numeric(0b111111))   // white
      .byte(numeric(0b010010));  // G 00, R 11, B 00
  const NabtsPageSnapshot snapshot = run(record);
  const auto rows = of_kind(snapshot, NabtsPrimitiveKind::kIncrementalPoints);
  ASSERT_EQ(rows.size(), 1u);
  ASSERT_EQ(rows[0]->incremental_colours.size(), 2u);
  EXPECT_EQ(rows[0]->incremental_colours[0], kNabtsNominalWhite);
  EXPECT_EQ(rows[0]->incremental_addresses[0], -1);
  // G = 00, R = 11, B = 00: red at full intensity.
  EXPECT_EQ(rows[0]->incremental_colours[1], (NabtsColour{0, 15, 0, false}));
}

// (3): at the field's edge "any remaining bits in the byte of the string
// operand currently being interpreted are discarded ... Interpretation resumes
// at the first bit (ie, b6) in the next complete byte", and the next row starts
// at "the opposite boundary", a pel up or down.
TEST(NaplpsInterpreter, IncrementalPointStartsEachRowOnANewByte) {
  Record record = incremental_setup(8, 8, 0.5, 0.5);
  record
      // A field two pels wide and two high, from the drawing point.
      .byte(static_cast<uint8_t>(NaplpsPdi::kField))
      .add(coord3(0.5, 0.5))
      .add(coord3(16.0 / 256.0, 16.0 / 256.0))
      .byte(static_cast<uint8_t>(NaplpsPdi::kIncrPoint))
      .byte(numeric(2))
      .byte(numeric(0b011011))   // 01 10 [11 dropped at the edge]
      .byte(numeric(0b100000));  // 10 00 [00 dropped]
  const NabtsPageSnapshot snapshot = run(record);
  const auto rows = of_kind(snapshot, NabtsPrimitiveKind::kIncrementalPoints);
  ASSERT_EQ(rows.size(), 2u);
  // Two-bit addresses are left-justified in four: 01 is entry 4, 10 entry 8.
  EXPECT_EQ(rows[0]->incremental_addresses, (std::vector<int16_t>{4, 8}));
  EXPECT_EQ(rows[1]->incremental_addresses, (std::vector<int16_t>{8, 0}));
  EXPECT_DOUBLE_EQ(rows[1]->origin.x, 0.5) << "back at the left boundary";
  EXPECT_DOUBLE_EQ(rows[1]->origin.y, 0.5 + 8.0 / 256.0) << "a pel up";
  // "If the operation has terminated, the drawing point is then set to the
  // origin of the active field."
  NaplpsInterpreter interpreter;
  (void)interpreter.run(record.data());
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.x, 0.5);
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.y, 0.5);
}

// "If moving the drawing point in the Y direction a distance equal to the
// height (dy) of the logical pel would cause any portion of the logical pel to
// exceed the active field, then the Y value is left constant and the entire
// display image lying within the area of the screen defined by the active
// field is scrolled in the opposite direction".
TEST(NaplpsInterpreter, IncrementalPointScrollsAFieldItHasFilled) {
  Record record = incremental_setup(8, 8, 0.5, 0.5);
  record.byte(static_cast<uint8_t>(NaplpsPdi::kField))
      .add(coord3(0.5, 0.5))
      .add(coord3(16.0 / 256.0, 8.0 / 256.0))  // one row high
      .byte(static_cast<uint8_t>(NaplpsPdi::kIncrPoint))
      .byte(numeric(3))
      .byte(numeric(0b001010))
      .byte(numeric(0b011100));
  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  EXPECT_EQ(snapshot.primitives[0].kind,
            NabtsPrimitiveKind::kIncrementalPoints);
  EXPECT_EQ(snapshot.primitives[1].kind, NabtsPrimitiveKind::kScroll);
  EXPECT_DOUBLE_EQ(snapshot.primitives[1].points[0].y, -8.0 / 256.0);
  EXPECT_EQ(snapshot.primitives[2].kind,
            NabtsPrimitiveKind::kIncrementalPoints);
  EXPECT_DOUBLE_EQ(snapshot.primitives[2].origin.y, 0.5) << "Y left constant";
}

// "If INCREMENTAL POINT is received and if any portion of the logical pel
// indicated by the initial drawing point lies outside the active field, the
// entire command is considered to be in error and is executed as a null
// operation."
TEST(NaplpsInterpreter, IncrementalPointOutsideItsFieldIsANullOperation) {
  Record record = incremental_setup(8, 8, 0.1, 0.1);
  record.byte(static_cast<uint8_t>(NaplpsPdi::kField))
      .add(coord3(0.5, 0.5))
      .add(coord3(0.25, 0.25))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .add(coord3(0.1, 0.1))
      .byte(static_cast<uint8_t>(NaplpsPdi::kIncrPoint))
      .byte(numeric(6))
      .byte(numeric(0b111111));
  EXPECT_TRUE(
      of_kind(run(record), NabtsPrimitiveKind::kIncrementalPoints).empty());
}

// "If INCREMENTAL POINT is received and either or both dimensions of the
// logical pel are equal to 0, then these dimensions are set, for the duration
// of the command only, to the smallest positive value specifiable within the
// current domain (For example, if the current multi-value operand length is 3
// bytes, then the smallest specifiable value would be ... 1/256.)"
TEST(NaplpsInterpreter, AZeroPelIsTheDomainsSmallestStep) {
  Record record = incremental_setup(0, 0, 0.5, 0.5);
  record.byte(static_cast<uint8_t>(NaplpsPdi::kIncrPoint))
      .byte(numeric(6))
      .byte(numeric(0b111111));
  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot snapshot = interpreter.run(record.data());
  const auto rows = of_kind(snapshot, NabtsPrimitiveKind::kIncrementalPoints);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_DOUBLE_EQ(rows[0]->logical_pel.dx, 1.0 / 256.0);
  EXPECT_DOUBLE_EQ(rows[0]->logical_pel.dy, 1.0 / 256.0);
  // "for the duration of the command only"
  EXPECT_DOUBLE_EQ(interpreter.state().domain.logical_pel.dx, 0.0);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Definition buffers (§6.2.3, §6.2.4)
////////////////////////////////////////////////////////////////////////////////////////////

// "The larger buffer dimension (dx or dy) shall coincide with the entire
// corresponding axis of the unit screen. For example, a character field that
// is 6/256 (of the unit screen) wide and 10/256 high would cause the DRCS
// character shape to be defined only by code that writes into that portion of
// the unit screen in the range 0.0 to 0.6 in the X axis".
TEST(NaplpsInterpreter, ADrcsBufferCoversItsShareOfTheNarrowerAxis) {
  const auto define_with_rectangle_at = [](double x) {
    Record record;
    record
        .pdi()
        // A field 6/256 wide and 10/256 high.
        .byte(static_cast<uint8_t>(NaplpsPdi::kText))
        .byte(numeric(0))
        .byte(numeric(0))
        .add(coord3(6.0 / 256.0, 10.0 / 256.0))
        .escape({code(4, 3)})  // DEF DRCS
        .byte(0x41)            // character 'A'
        .pdi()
        .byte(static_cast<uint8_t>(NaplpsPdi::kSetRectFilled))
        .add(coord3(x, 0.0))
        .add(coord3(0.1, 0.99))
        .escape({code(4, 5)});  // END
    NaplpsInterpreter interpreter;
    const NabtsPageSnapshot snapshot = interpreter.run(record.data());
    size_t on = 0;
    for (const NabtsDrcsCharacter& glyph : snapshot.drcs) {
      for (const bool element : glyph.elements) {
        on += element ? 1 : 0;
      }
    }
    return on;
  };
  EXPECT_GT(define_with_rectangle_at(0.2), 0u) << "inside 0.0 to 0.6";
  EXPECT_EQ(define_with_rectangle_at(0.7), 0u) << "past 0.6, off the buffer";
}

// "Each element that is affected shall be set to the on state, unless it is
// written into with nominal black, in which case it is set to the off state"
// — each element by what was written into it, so a highlighted figure's black
// outline turns its elements off while the fill inside turns them on.
TEST(NaplpsInterpreter, EachDefinitionElementTakesWhatWasWrittenIntoIt) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kTexture))
      .byte(numeric(0b000100))  // highlight on
      .escape({code(4, 3)})     // DEF DRCS
      .byte(0x41)
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetRectFilled))
      .add(coord3(0.0, 0.0))
      .add(coord3(0.99, 0.99))
      .escape({code(4, 5)});
  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.drcs.size(), 1u);
  const NabtsDrcsCharacter& glyph = snapshot.drcs.front();
  ASSERT_TRUE(glyph.defined());
  const auto element = [&](int column, int row) {
    return glyph.elements[static_cast<size_t>(row) * glyph.width +
                          static_cast<size_t>(column)];
  };
  EXPECT_FALSE(element(0, 0)) << "the black highlight on the outline";
  EXPECT_TRUE(element(glyph.width / 2, glyph.height / 2)) << "the fill";
}

////////////////////////////////////////////////////////////////////////////////////////////
// Clipping and text layout (§5.3.1, §5.3.2.3, §6.1.2, §6.2.7)
////////////////////////////////////////////////////////////////////////////////////////////

// §5.3.1 lets a drawing that leaves the unit screen be "executed and clipped",
// which keeps its geometry: a rectangle starting left of the screen is still a
// rectangle, not a line pressed against the edge.
TEST(NaplpsInterpreter, AnOffScreenFigureKeepsItsGeometry) {
  // From x = 0.75, half a screen wide: its far side is past the right edge.
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetRectFilled))
      .add(coord3(0.75, 0.25))
      .add(coord3(0.5, 0.25));
  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x, 0.75);
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].size.dx, 0.5)
      << "the far side moved onto the screen's edge";
}

// §6.2.7.14 SCROLL OFF (the default): a line feed past the bottom of the
// display area puts the cursor "to the opposite edge of the area or field such
// that the character field so defined lies entirely within" it.
TEST(NaplpsInterpreter, ALineFeedPastTheBottomWrapsToTheTop) {
  // The display area is exactly twenty rows of the default field (0.78125 /
  // (5/128)), so from the top row (APH) nineteen line feeds reach the bottom
  // one and the twentieth wraps.
  Record deep;
  deep.text_mode().byte(0x1E);
  for (int i = 0; i < 20; ++i) {
    deep.byte(0x0A);
  }
  deep.byte(0x41);
  const NabtsPageSnapshot snapshot = run(deep);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  const double field = 5.0 / 128.0;
  EXPECT_NEAR(snapshot.primitives[0].origin.y, kNabtsDisplayAreaHeight - field,
              1e-9)
      << "wrapped to the top row";
}

// §6.2.7.13 SCROLL ON: the same line feed scrolls the display area up instead,
// "far enough to bring the next intended character location just into" it,
// and the cursor stays on the bottom row.
TEST(NaplpsInterpreter, ALineFeedPastTheBottomScrollsUnderScrollOn) {
  Record record;
  record.text_mode().escape({code(5, 7)});  // SCROLL ON
  record.byte(0x1E);
  for (int i = 0; i < 20; ++i) {
    record.byte(0x0A);
  }
  record.byte(0x41);
  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_EQ(snapshot.primitives[0].kind, NabtsPrimitiveKind::kScroll);
  EXPECT_NEAR(snapshot.primitives[0].points[0].y, 5.0 / 128.0, 1e-9)
      << "one row up";
  EXPECT_NEAR(snapshot.primitives[1].origin.y, 0.0, 1e-9) << "bottom row";
}

// §5.3.2.3.6 on every path: text written downwards wraps when the next field
// would leave the unit screen — back to the first position along the path,
// which going down is the top of the display area, and a line on, which is
// -90 degrees from the path: to the left.
TEST(NaplpsInterpreter, TheAutomaticLineBreakFollowsTheCharacterPath) {
  Record record;
  record
      .pdi()
      // TEXT: path down (b4 b3 = 11), fields 1/40 by 5/128.
      .byte(static_cast<uint8_t>(NaplpsPdi::kText))
      .byte(numeric(0b001100))
      .byte(numeric(0))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .add(coord3(0.5, 5.0 / 128.0))  // one field above the bottom
      .text_mode()
      .add({0x41, 0x42, 0x43});
  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  EXPECT_NEAR(snapshot.primitives[1].origin.y, 0.0, 1e-9) << "B at the bottom";
  // C: back to the first position along the (downward) path — the top of the
  // display area — and a line down, which on a downward path is -90 degrees
  // from it: to the left.
  EXPECT_NEAR(snapshot.primitives[2].origin.y,
              kNabtsDisplayAreaHeight - 5.0 / 128.0, 1e-9);
  EXPECT_LT(snapshot.primitives[2].origin.x, 0.5);
}

// §6.1.2.1: an APB past the start of the line goes "to the opposite edge
// (along the character path) of the display area ... and an automatic APU is
// executed" — the end of the line above.
TEST(NaplpsInterpreter, ABackspaceAtTheStartOfALineGoesToTheEndOfTheOneAbove) {
  Record record;
  record.text_mode()
      .byte(0x1E)  // APH
      .byte(0x0A)  // APD: the second row
      .byte(0x08)  // APB at the left edge
      .byte(0x41);
  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_NEAR(snapshot.primitives[0].origin.x, 1.0 - 1.0 / 40.0, 1e-9);
  EXPECT_NEAR(snapshot.primitives[0].origin.y,
              kNabtsDisplayAreaHeight - 5.0 / 128.0, 1e-9)
      << "the top row";
}

// §6.2.7.2: REPEAT repeats "the immediately preceding byte". Anything else in
// between — a carriage return here — leaves nothing to repeat, and the command
// is a null operation.
TEST(NaplpsInterpreter, RepeatRepeatsOnlyTheImmediatelyPrecedingByte) {
  Record repeated;
  repeated.text_mode().byte(0x41).escape({code(4, 6)}).byte(0x43);  // 3 more
  EXPECT_EQ(run(repeated).primitives.size(), 4u);

  Record interrupted;
  interrupted.text_mode()
      .byte(0x41)
      .byte(0x0D)  // APR
      .escape({code(4, 6)})
      .byte(0x43);
  EXPECT_EQ(run(interrupted).primitives.size(), 1u);
}

// A character single-shifted in from G2 is repeated as itself — from the
// supplementary set — not as whatever set is in use once the shift has passed.
TEST(NaplpsInterpreter, RepeatRepeatsASingleShiftedCharacterFromItsOwnSet) {
  Record record;
  record.text_mode()
      .byte(0x19)  // SS2
      .byte(0x23)
      .escape({code(4, 6)})
      .byte(0x42);  // 2 more
  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 3u);
  for (const NabtsPrimitive& primitive : snapshot.primitives) {
    EXPECT_EQ(primitive.repertoire, NabtsPrimitive::Repertoire::kSupplementary);
    EXPECT_EQ(primitive.character, 0x23);
  }
}

// §6.2.7.3: REPEAT TO EOL repeats "until the last character position along the
// current character path is reached" — here the last of forty, whatever the
// path.
TEST(NaplpsInterpreter, RepeatToEndOfLineFillsTheLineAndNoFurther) {
  Record record;
  record.text_mode()
      .byte(0x1E)  // APH
      .byte(0x2D)  // '-'
      .escape({code(4, 7)});
  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_EQ(snapshot.primitives.size(), 40u);
  for (const NabtsPrimitive& primitive : snapshot.primitives) {
    EXPECT_NEAR(primitive.origin.y, kNabtsDisplayAreaHeight - 5.0 / 128.0,
                1e-9);
  }
}

// §5.3.2.3.7: "The alignment of the drawing point corresponds to ... the
// center of the character field for the cross-hair cursor and custom cursor",
// and TEXT "shall cause alignment of the drawing point if the "move together"
// or "cursor leads" move attribute is in effect".
TEST(NaplpsInterpreter, ACrossHairCursorAlignsTheDrawingPointAtTheCentre) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kText))
      .byte(numeric(0))
      .byte(numeric(0b100000));  // cursor style 2: cross-hair
  NaplpsInterpreter interpreter;
  (void)interpreter.run(record.data());
  const NaplpsState& state = interpreter.state();
  EXPECT_NEAR(state.drawing_point.x - state.cursor.x, 1.0 / 80.0, 1e-12);
  EXPECT_NEAR(state.drawing_point.y - state.cursor.y, 5.0 / 256.0, 1e-12);
}

// §5.3.2.3.7: "Movement of the drawing point should never cause the cursor to
// be located such that any part of the character field indicated by the
// cursor would fall outside the unit screen", the cursor being "adjusted as
// close as possible to the drawing point".
TEST(NaplpsInterpreter, TheCursorKeepsItsFieldOnTheScreen) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs))
      .add(coord3(254.0 / 256.0, 0.5));
  NaplpsInterpreter interpreter;
  (void)interpreter.run(record.data());
  EXPECT_DOUBLE_EQ(interpreter.state().drawing_point.x, 254.0 / 256.0);
  EXPECT_NEAR(interpreter.state().cursor.x, 1.0 - 1.0 / 40.0, 1e-12);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Bytes known to be wrong
////////////////////////////////////////////////////////////////////////////////////////////

/// |data| as it arrives: b8 of every byte set to give the odd parity CEA-516
/// §3.3 requires of every data byte of a teletext record.
std::vector<uint8_t> with_parity(std::vector<uint8_t> data) {
  for (uint8_t& byte : data) {
    int bits = 0;
    for (int bit = 0; bit < 7; ++bit) {
      bits += (byte >> bit) & 1;
    }
    byte = static_cast<uint8_t>((byte & 0x7F) | (bits % 2 == 0 ? 0x80 : 0x00));
  }
  return data;
}

/// |data| with its byte |index| received failing its parity.
std::vector<uint8_t> failing_at(std::vector<uint8_t> data, size_t index) {
  data[index] = static_cast<uint8_t>(data[index] ^ 0x80);
  return data;
}

NabtsPageSnapshot run_bytes(const std::vector<uint8_t>& data) {
  NaplpsInterpreter interpreter;
  return interpreter.run(data);
}

// CEA-516 §3.3: a byte failing its parity is known to be wrong. A CS that
// arrived so is not run, as the Panasonic TU-2000x does not run it, so what was
// drawn before it stays on the page.
TEST(NaplpsInterpreter, AClearFailingParityIsNotRun) {
  Record record;
  record.text_mode().add({'X'}).byte(0x0C).add({'Y'});
  const std::vector<uint8_t> sent = with_parity(record.data());

  const NabtsPageSnapshot clean = run_bytes(sent);
  ASSERT_EQ(clean.primitives.size(), 1u);
  EXPECT_EQ(clean.primitives[0].character, 'Y');

  const NabtsPageSnapshot damaged = run_bytes(failing_at(sent, 2));
  ASSERT_EQ(damaged.primitives.size(), 2u);
  EXPECT_EQ(damaged.primitives[0].character, 'X');
  EXPECT_EQ(damaged.primitives[1].character, 'Y');
  EXPECT_EQ(damaged.diagnostics.parity_skipped, 1u);
}

// An opcode failing its parity is skipped too, and the operands after it are
// data with no opcode in front of them: nothing is drawn.
TEST(NaplpsInterpreter, AnOpcodeFailingParityIsSkippedWithItsOperands) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kRectFilled))
      .byte(coord1(0b010, 0b001));
  const std::vector<uint8_t> sent = with_parity(record.data());
  EXPECT_EQ(run_bytes(sent).primitives.size(), 1u);

  const NabtsPageSnapshot damaged = run_bytes(failing_at(sent, 3));
  EXPECT_TRUE(damaged.primitives.empty());
  EXPECT_EQ(damaged.diagnostics.parity_skipped, 1u);
}

// A record handed over with its parity already stripped — no byte has b8 set —
// has nothing to test, and is run as it is.
TEST(NaplpsInterpreter, ARecordWithoutItsParityIsRunAsItIs) {
  Record record;
  record.text_mode().add({'X'}).byte(0x0C).add({'Y'});
  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].character, 'Y');
  EXPECT_EQ(snapshot.diagnostics.parity_skipped, 0u);
}

}  // namespace
}  // namespace orc
