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
/// significant first (Figure 11), which is nine bits an axis — a sign and eight
/// of magnitude, so any 1/256 of the unit screen can be named exactly.
std::vector<uint8_t> coord3(double x, double y) {
  const auto encode = [](double value) {
    const uint32_t magnitude = std::min<uint32_t>(
        static_cast<uint32_t>(value < 0.0 ? -value * 256.0 : value * 256.0),
        0xFF);
    return value < 0.0 ? (0x100u | magnitude) : magnitude;
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

////////////////////////////////////////////////////////////////////////////////////////////
// The reset state (T.101 Table II-3)
////////////////////////////////////////////////////////////////////////////////////////////

// Table II-3's default presentation parameters, field by field, for the ones a
// display list can carry. This is the test the plan asked for: "a freshly reset
// interpreter matches Table II-3 field for field".
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
  EXPECT_FALSE(state.blinking());
  EXPECT_TRUE(std::none_of(
      state.blink_from.begin(), state.blink_from.end(),
      [](const NaplpsState::BlinkProcess& p) { return p.active; }));

  // basic-char-size-state: dx = 1/40, dy = 1/128 ... the table's own figure for
  // dy is 1/128, but X3.110 §5.3.2.3.9 and §6.2.7.8 both give 5/128 for the
  // default and for NORMAL TEXT, and Table II-3's geometric-control-1-state
  // gives the texture mask — "the default character field size" per §5.3.2.4.5
  // — as 1/40, 5/128. So 5/128 is the value two independent statements agree on
  // and 1/128 is a typo in the table.
  EXPECT_DOUBLE_EQ(state.text.character_field.dx, 1.0 / 40.0);
  EXPECT_DOUBLE_EQ(state.text.character_field.dy, 5.0 / 128.0);

  // cursor-control-state: off (invisible).
  EXPECT_FALSE(state.text.cursor_visible);

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
  EXPECT_EQ(snapshot.diagnostics.bytes_read, 0u);
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
      .byte(coord1(0b010, 0b000))   // +0,5 in x
      .byte(coord1(0b000, 0b010))   // +0,5 in y
      .byte(coord1(0b110, 0b000));  // -0,5 in x

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
      .byte(coord1(0b001, 0b111));

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
  // distributed between zero and full intensity." Over two bits that is
  // 0, 7/3, 14/3, 7 — so 2 of 3 is 5 of 7, not the 4 a shift-and-zero-fill
  // would give. The clause matters: zero-filling makes white unreachable in a
  // one-byte operand, and the reference ExtraVision service sets white with
  // exactly one byte.
  EXPECT_EQ(snapshot.primitives[0].colour.green, 5u);
  EXPECT_EQ(snapshot.primitives[0].colour.red, 5u);
  EXPECT_EQ(snapshot.primitives[0].colour.blue, 0u);
  // And the map the renderer is handed carries it, because §5.3.2.5 makes a map
  // change retroactive for every pixel already pointing at that entry.
  EXPECT_EQ(snapshot.colour_map[3].green, 5u);
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
      // over six bits, truncated to its top three = 0b101 = 5.
      .byte(numeric(0b100000))
      .byte(numeric(0b100000))
      .byte(numeric(0b100000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_EQ(snapshot.primitives[0].colour.green, 5u);
  EXPECT_EQ(snapshot.primitives[0].colour.red, 0u);
  EXPECT_EQ(snapshot.primitives[0].colour.blue, 0u);
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
  // Nothing was clamped back into the screen on the way.
  EXPECT_EQ(snapshot.diagnostics.out_of_range_coordinates, 0u);
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
  // code 2 is "up". Started from the reset cursor at the lower left, so both
  // the advance and the APD move away from the edges rather than into them.
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

// The plan's acceptance: "a macro defined and invoked expands to the same
// display list as its inline form".
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
      // ARC (outlined): a start point, an intermediate point and an end point.
      .byte(static_cast<uint8_t>(NaplpsPdi::kArcOutlined))
      .byte(coord1(0b001, 0b011))
      .byte(coord1(0b011, 0b111))
      .byte(coord1(0b111, 0b011))
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

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_TRUE(snapshot.drcs.empty());
  EXPECT_EQ(snapshot.diagnostics.storage_used, 0u);
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

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_TRUE(snapshot.texture_masks[0].defined());
  EXPECT_EQ(snapshot.texture_masks[0].width, 16u);
  EXPECT_EQ(snapshot.texture_masks[0].height, 16u);
  // The mask storage is not part of the shared budget.
  EXPECT_EQ(snapshot.diagnostics.storage_used, 0u);
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

// The plan's acceptance: "the storage budget is enforced rather than exceeded".
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

  const NabtsPageSnapshot snapshot = run(record);
  EXPECT_LE(snapshot.diagnostics.storage_used, kNaplpsSharedStorageBytes)
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
  EXPECT_GT(snapshot.diagnostics.ignored_controls, 0u);
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
      .byte(static_cast<uint8_t>(NaplpsPdi::kArcOutlined))
      .byte(coord1(0b011, 0b011));

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

// §5.3.1 makes an out-of-screen coordinate an error; this clips it and counts
// it rather than wrapping or rejecting the PDI.
TEST(NaplpsInterpreter, ClipsACoordinateOffTheUnitScreenAndCountsIt) {
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
  EXPECT_DOUBLE_EQ(snapshot.primitives[0].origin.x, 0.0)
      << "clipped, not wrapped";
  EXPECT_GT(snapshot.diagnostics.out_of_range_coordinates, 0u);
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
  EXPECT_GT(snapshot.diagnostics.unknown_designations, 0u);
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
  EXPECT_EQ(snapshot.colour_map[3].green, 5u)
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
  EXPECT_EQ(page.primitives[0].colour.green, 5u)
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
  EXPECT_EQ(snapshot.colour_map[0].green, 5u);
  EXPECT_EQ(snapshot.colour_map[8].red, 5u) << "the first repeat lands at 1000";
  EXPECT_EQ(snapshot.colour_map[4].blue, 5u)
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
  EXPECT_EQ(snapshot.primitives[0].colour.green, 5u)
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
  EXPECT_TRUE(snapshot.primitives[0].blinking);
  EXPECT_FALSE(snapshot.primitives[1].blinking)
      << "a primitive drawn in another map entry inherited the blink";
  EXPECT_TRUE(snapshot.primitives[2].blinking);
  // §5.3.2.7.3's blink-to address, resolved through the map: entry 6 of the
  // default colour map (§5.3.2.5.2).
  EXPECT_EQ(snapshot.primitives[0].blink_to_map_address, 6);
  EXPECT_EQ(snapshot.primitives[0].blink_to, snapshot.colour_map[6]);
}

// The blink-to colour is what makes a blink more than an appearance and a
// disappearance. This is the "TELETEXT is Animation!" record's own shape: a
// figure in one map entry alternating with another entry's colour, so the
// baubles twinkle blue to yellow rather than vanishing.
TEST(NaplpsInterpreter, BlinkCarriesTheColourTheEntryAlternatesTo) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      // Entry 6 is loaded with a colour of its own first, so the assertion is
      // about this record's map rather than the default one.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b011000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b110000))
      // Draw in entry 3, blinking to entry 6.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
      .byte(numeric(0b011000))
      .byte(numeric(4))
      .byte(numeric(6))
      .byte(numeric(0))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  ASSERT_TRUE(snapshot.primitives[0].blinking);
  EXPECT_EQ(snapshot.primitives[0].blink_to.green, 5u);
  EXPECT_EQ(snapshot.primitives[0].blink_to.red, 5u);
  EXPECT_EQ(snapshot.primitives[0].blink_to.blue, 0u);
}

// §5.3.2.5 makes a colour map write retroactive, and a blink process reads the
// map rather than a copy of what the entry held when it started — so rewriting
// the blink-to entry after the fact changes what the figure alternates to.
TEST(NaplpsInterpreter, ARewrittenBlinkToEntryChangesWhatTheBlinkShows) {
  Record record;
  record.pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kDomain))
      .byte(numeric(0b000000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
      .byte(numeric(0b011000))  // blink-to entry 6
      .byte(numeric(4))
      .byte(numeric(6))
      .byte(numeric(0))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      // Only now is entry 6 given its colour.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b011000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSetColour))
      .byte(numeric(0b001100));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  ASSERT_TRUE(snapshot.primitives[0].blinking);
  // The operand's six bits are two GRB triples, so 001100 is G = 01, R = 00,
  // B = 10 — a third and two thirds of full intensity, which §5.3.2.5.1's equal
  // distribution puts at 2 and 5 of 7.
  EXPECT_EQ(snapshot.primitives[0].blink_to.green, 2u);
  EXPECT_EQ(snapshot.primitives[0].blink_to.red, 0u);
  EXPECT_EQ(snapshot.primitives[0].blink_to.blue, 5u);
  EXPECT_EQ(snapshot.primitives[0].blink_to, snapshot.colour_map[6]);
  // §5.3.2.5.2's default entry 6 is grey level 6, so this could not have come
  // from reading the map as it stood when the process started.
  EXPECT_FALSE(snapshot.primitives[0].blink_to ==
               (NabtsColour{6, 6, 6, false}));
}

// §6.2.8.1: the C1 form names no entry — "the blink-to color is nominal black
// in color modes 0 and 1 or the background color in color mode 2".
TEST(NaplpsInterpreter, C1BlinkStartBlinksToBlackOutsideColourModeTwo) {
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
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  ASSERT_TRUE(snapshot.primitives[0].blinking);
  EXPECT_EQ(snapshot.primitives[0].blink_to_map_address, -1)
      << "the C1 form names no map entry, so nothing should resolve one";
  EXPECT_EQ(snapshot.primitives[0].blink_to, kNabtsNominalBlack);
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
        .byte(static_cast<uint8_t>(NaplpsPdi::kBlink))
        .byte(numeric(0b011000))
        .byte(numeric(intervals.first))
        .byte(numeric(intervals.second))
        .byte(numeric(0))
        .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
        .byte(coord1(0b001, 0b001));

    const NabtsPageSnapshot snapshot = run(record);
    ASSERT_EQ(snapshot.primitives.size(), 1u);
    EXPECT_FALSE(snapshot.primitives[0].blinking)
        << "ON=" << static_cast<int>(intervals.first)
        << " OFF=" << static_cast<int>(intervals.second);
  }
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
      .pdi()
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001))
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b001100))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b010, 0b010));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 2u);
  EXPECT_FALSE(snapshot.primitives[0].blinking) << "entry 7 was stopped";
  EXPECT_TRUE(snapshot.primitives[1].blinking) << "entry 3 was never stopped";
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
      .byte(numeric(0))
      // Drawn in entry 8, which only the repeat can have started.
      .byte(static_cast<uint8_t>(NaplpsPdi::kSelectColour))
      .byte(numeric(0b100000))
      .byte(static_cast<uint8_t>(NaplpsPdi::kPointAbs))
      .byte(coord1(0b001, 0b001));

  const NabtsPageSnapshot snapshot = run(record);
  ASSERT_EQ(snapshot.primitives.size(), 1u);
  EXPECT_TRUE(snapshot.primitives[0].blinking);
}

}  // namespace
}  // namespace orc
