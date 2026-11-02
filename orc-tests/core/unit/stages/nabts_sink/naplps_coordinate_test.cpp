/*
 * File:        naplps_coordinate_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NAPLPS coordinate codebook: how the bytes of
 *              a multi-value or single-value operand become a point, a map
 *              address and a pixel
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <vector>

#include "nabts_raster_view.h"
#include "naplps_interpreter.h"
#include "naplps_raster.h"
#include "naplps_render_grid.h"

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

/// The opcode byte of |pdi|.
constexpr uint8_t op(NaplpsPdi pdi) { return static_cast<uint8_t>(pdi); }

/// DOMAIN byte 1 (§5.3.2.2, Figure 14): b2-b1 the single-value length less
/// one, b5-b3 the multi-value length less one, b6 the dimensionality.
constexpr uint8_t domain_byte1(int multi_value_bytes, int single_value_bytes,
                               bool three_dimensional) {
  return numeric(static_cast<uint8_t>((three_dimensional ? 0x20 : 0x00) |
                                      ((multi_value_bytes - 1) << 2) |
                                      (single_value_bytes - 1)));
}

/// A two-dimensional word of |bytes| bytes (Figure 11): each byte carries three
/// bits of X in b6-b4 and three of Y in b3-b1, most significant first. |x| and
/// |y| are the components' raw 3*|bytes|-bit two's-complement fields.
std::vector<uint8_t> word2(int bytes, uint32_t x, uint32_t y) {
  const int n = 3 * bytes;
  std::vector<uint8_t> out;
  for (int i = 0; i < bytes; ++i) {
    const int shift = n - 3 * (i + 1);
    out.push_back(numeric(static_cast<uint8_t>((((x >> shift) & 0x7) << 3) |
                                               ((y >> shift) & 0x7))));
  }
  return out;
}

/// A three-dimensional word (Figure 11): X in b6-b5, Y in b4-b3, Z in b2-b1,
/// each a 2*|bytes|-bit two's-complement field, most significant first.
std::vector<uint8_t> word3(int bytes, uint32_t x, uint32_t y, uint32_t z) {
  const int n = 2 * bytes;
  std::vector<uint8_t> out;
  for (int i = 0; i < bytes; ++i) {
    const int shift = n - 2 * (i + 1);
    out.push_back(numeric(static_cast<uint8_t>((((x >> shift) & 0x3) << 4) |
                                               (((y >> shift) & 0x3) << 2) |
                                               ((z >> shift) & 0x3))));
  }
  return out;
}

/// The raw |bits|-bit two's-complement field of |value| in 1/2^(|bits|-1)
/// units, which is how §5.3.1 codes a coordinate: the MSB is the sign and
/// "the digit just to the right of the decimal point" follows it.
uint32_t field(double value, int bits) {
  const double scaled = std::ldexp(value, bits - 1);
  const auto integer = static_cast<int64_t>(std::llround(scaled));
  return static_cast<uint32_t>(integer) & ((uint32_t{1} << bits) - 1);
}

/// What the component bit |index| (0 being the sign) of a word is worth: the
/// sign bit -1, then 1/2, 1/4, ... (§5.3.1).
double weight(int index) { return index == 0 ? -1.0 : std::ldexp(1.0, -index); }

/**
 * @brief Builds a presentation record byte by byte
 *
 * Every record starts by invoking the PDI set (SO puts G1, the PDI set by
 * default, in GL) and every test ends its last PDI with a code, so that no
 * operand run is cut by the end of the record.
 */
class Record {
 public:
  /// SO — invoke G1, which by default holds the PDI set.
  Record& pdi() { return byte(code(0, 14)); }
  /// SI — invoke G0, the primary character set; it also ends a PDI.
  Record& text_mode() { return byte(code(0, 15)); }

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
  Record& add(const std::vector<uint8_t>& values) {
    bytes_.insert(bytes_.end(), values.begin(), values.end());
    return *this;
  }

  const std::vector<uint8_t>& data() const { return bytes_; }

 private:
  std::vector<uint8_t> bytes_;
};

NabtsPageSnapshot run(const Record& record) {
  NaplpsInterpreter interpreter;
  return interpreter.run(record.data());
}

/// The point every visible POINT of |snapshot| was drawn at, in order.
std::vector<NabtsPoint> points_of(const NabtsPageSnapshot& snapshot) {
  std::vector<NabtsPoint> out;
  for (const NabtsPrimitive& primitive : snapshot.primitives) {
    if (primitive.kind == NabtsPrimitiveKind::kPoint &&
        !primitive.points.empty()) {
      out.push_back(primitive.points.front());
    }
  }
  return out;
}

// ---------------------------------------------------------------------------
// Figure 11: every payload bit's weight
// ---------------------------------------------------------------------------

// §5.3.2.2.3 Table 5 and §5.3.1 Figure 11, two-dimensional mode: a word is
// 1 to 8 bytes, each carrying three bits of X (b6-b4) and three of Y (b3-b1),
// most significant byte first, and each component is "signed, two's complement
// ... binary decimals where the MSB represents the digit just to the right of
// the decimal point" — so the first bit weighs -1, the next 1/2, then 1/4 on.
// One POINT per bit of each component, for every length. Both receivers of the
// day place every bit of the lengths they keep the same way.
TEST(NaplpsCoordinates, EveryBitOfATwoDimensionalWordHasItsFigure11Weight) {
  for (int length = 1; length <= 8; ++length) {
    const int bits = 3 * length;
    Record record;
    record.pdi().add({op(NaplpsPdi::kDomain), domain_byte1(length, 1, false)});
    std::vector<NabtsPoint> expected;
    for (int index = 0; index < bits; ++index) {
      const uint32_t one_bit = uint32_t{1} << (bits - 1 - index);
      record.byte(op(NaplpsPdi::kPointAbs)).add(word2(length, one_bit, 0));
      expected.push_back(NabtsPoint{weight(index), 0.0});
      record.byte(op(NaplpsPdi::kPointAbs)).add(word2(length, 0, one_bit));
      expected.push_back(NabtsPoint{0.0, weight(index)});
    }
    record.text_mode();

    const std::vector<NabtsPoint> points = points_of(run(record));
    ASSERT_EQ(points.size(), expected.size()) << "length " << length;
    for (size_t i = 0; i < points.size(); ++i) {
      EXPECT_DOUBLE_EQ(points[i].x, expected[i].x)
          << "length " << length << " point " << i;
      EXPECT_DOUBLE_EQ(points[i].y, expected[i].y)
          << "length " << length << " point " << i;
    }
  }
}

// §5.3.2.2.4 and Figure 11, three-dimensional mode: each byte carries two bits
// of X (b6-b5), two of Y (b4-b3) and two of Z (b2-b1). "If three-dimensional
// coordinates are received, the Z coordinate is to be ignored" (and Table D1
// item 5(1)(c)), so every X and Y bit keeps its weight with Z all ones beside
// it. The Norpak TTX 650 reads every length so; the Panasonic TU-2000x keeps 7
// bits and has quirks of its own, which this does not follow.
TEST(NaplpsCoordinates,
     EveryBitOfAThreeDimensionalWordHasItsWeightAndZIsIgnored) {
  for (int length = 1; length <= 8; ++length) {
    const int bits = 2 * length;
    const uint32_t all_ones = (uint32_t{1} << bits) - 1;
    Record record;
    record.pdi().add({op(NaplpsPdi::kDomain), domain_byte1(length, 1, true)});
    std::vector<NabtsPoint> expected;
    for (int index = 0; index < bits; ++index) {
      const uint32_t one_bit = uint32_t{1} << (bits - 1 - index);
      record.byte(op(NaplpsPdi::kPointAbs))
          .add(word3(length, one_bit, 0, all_ones));
      expected.push_back(NabtsPoint{weight(index), 0.0});
      record.byte(op(NaplpsPdi::kPointAbs))
          .add(word3(length, 0, one_bit, all_ones));
      expected.push_back(NabtsPoint{0.0, weight(index)});
    }
    record.text_mode();

    const std::vector<NabtsPoint> points = points_of(run(record));
    ASSERT_EQ(points.size(), expected.size()) << "length " << length;
    for (size_t i = 0; i < points.size(); ++i) {
      EXPECT_DOUBLE_EQ(points[i].x, expected[i].x)
          << "length " << length << " point " << i;
      EXPECT_DOUBLE_EQ(points[i].y, expected[i].y)
          << "length " << length << " point " << i;
    }
  }
}

// Worked examples, byte for byte: what a reader of a record dump sees and
// where the point lands, in 1/256 of the unit screen.
// A negative absolute coordinate is kept — §5.3.1 makes it an error a receiver
// may execute and clip — which is also where the TTX 650 leaves it.
TEST(NaplpsCoordinates, TheCodebookExamplesDecodeAsTabulated) {
  struct Example {
    int length;
    bool three_dimensional;
    std::vector<uint8_t> word;
    double x;  // in 1/256
    double y;
  };
  const Example examples[] = {
      {1, false, {0x49}, 64.0, 64.0},
      {1, false, {0x51}, 128.0, 64.0},
      {2, false, {0x41, 0x4C}, 8.0, 96.0},
      {3, false, {0x40, 0x40, 0x49}, 1.0, 1.0},
      {3, false, {0x51, 0x44, 0x44}, 128.0, 100.0},
      {3, false, {0x79, 0x44, 0x44}, -64.0, 100.0},
      {3, false, {0x7F, 0x7F, 0x7F}, -1.0, -1.0},
      {4, false, {0x49, 0x44, 0x44, 0x60}, 64.5, 100.0},
      {2, true, {0x40, 0x6C}, 64.0, 96.0},
      {4, true, {0x53, 0x4F, 0x43, 0x4B}, 128.0, 100.0},
      {5, true, {0x50, 0x4C, 0x40, 0x48, 0x60}, 129.0, 100.0},
  };
  for (const Example& example : examples) {
    Record record;
    record.pdi()
        .add({op(NaplpsPdi::kDomain),
              domain_byte1(example.length, 1, example.three_dimensional),
              op(NaplpsPdi::kPointAbs)})
        .add(example.word)
        .text_mode();
    const std::vector<NabtsPoint> points = points_of(run(record));
    ASSERT_EQ(points.size(), 1u);
    EXPECT_DOUBLE_EQ(points[0].x * 256.0, example.x);
    EXPECT_DOUBLE_EQ(points[0].y * 256.0, example.y);
  }
}

// ---------------------------------------------------------------------------
// The pixel
// ---------------------------------------------------------------------------

// Appendix B: the internal representation maps to the 256-pixel physical one
// by "a truncation of the low order bits" (BYTE, February 1983: shift the
// integer right) — the pixel is floor(256 x), not the nearest. So x = 1/2 less
// one least significant bit lights column 127, and x = 128.5/256 column 128.
// Both receivers of the day do exactly this. Drawn through the renderer's own
// deposit onto the Table D1 receiver's 256 x 200 grid.
TEST(NaplpsCoordinates, HalfLessOneLeastSignificantBitLandsOnPixel127) {
  for (int length = 4; length <= 8; ++length) {
    const int bits = 3 * length;
    const double lsb = std::ldexp(1.0, 1 - bits);
    for (const double x : {0.5 - lsb, 128.5 / 256.0}) {
      Record record;
      record.pdi()
          .add({op(NaplpsPdi::kDomain), domain_byte1(length, 1, false),
                op(NaplpsPdi::kPointAbs)})
          .add(word2(length, field(x, bits), field(100.0 / 256.0, bits)))
          .text_mode();
      const NabtsPageSnapshot page = run(record);
      ASSERT_EQ(page.primitives.size(), 1u);
      EXPECT_DOUBLE_EQ(page.primitives[0].points.front().x, x);

      NaplpsCellSurface surface(kNaplpsGridReference);
      NaplpsRasteriser raster(surface, NaplpsGridMapping{kNaplpsGridReference});
      naplps_deposit_primitive(page.primitives[0], NaplpsDepositContext{},
                               raster);
      const int column = x < 0.5 ? 127 : 128;
      EXPECT_TRUE(surface.at(column, 100).painted)
          << "length " << length << " x " << x;
      EXPECT_EQ(surface.painted_count(), 1u)
          << "length " << length << " x " << x;
    }
  }
}

// ---------------------------------------------------------------------------
// Relative accumulation
// ---------------------------------------------------------------------------

// §5.3.3.1.3: POINT SET (Relative) adds its displacement to the drawing point.
// §5.3.1 makes a drawing point outside the unit screen an error a receiver may
// "execute and clip", so the point is kept where the record put it (counted),
// and a displacement back brings it home exactly. The Norpak TTX 650 does the
// same; the Panasonic TU-2000x clamps every step to 0..255 and ends at 63,
// which this deliberately does not follow.
TEST(NaplpsCoordinates, ARelativeExcursionPastTheUnitScreenReturnsExactly) {
  Record record;
  record.pdi()
      .byte(op(NaplpsPdi::kPointSetAbs))
      .add(word2(3, field(0.5, 9), field(100.0 / 256.0, 9)))
      .byte(op(NaplpsPdi::kPointSetRel))
      .add(word2(3, field(0.75, 9), 0))
      .byte(op(NaplpsPdi::kPointSetRel))
      .add(word2(3, field(-0.75, 9), 0))
      .byte(op(NaplpsPdi::kPointRel))
      .add(word2(3, 0, 0))
      .text_mode();

  const NabtsPageSnapshot page = run(record);
  const std::vector<NabtsPoint> points = points_of(page);
  ASSERT_EQ(points.size(), 1u);
  EXPECT_DOUBLE_EQ(points[0].x, 0.5);
  // x = 1.25 on the way out, off the unit screen, was kept rather than
  // clamped, so the way back lands where the record meant.
  EXPECT_DOUBLE_EQ(points[0].y, 100.0 / 256.0);
}

// The same excursion made of many steps, and the other way: a long run of
// relative words accumulates exactly (the TTX 650 keeps 14 fraction bits and
// wraps only past +-2).
TEST(NaplpsCoordinates, ALongRelativeRunAccumulatesExactly) {
  Record record;
  record.pdi()
      .byte(op(NaplpsPdi::kPointSetAbs))
      .add(word2(3, field(0.5, 9), field(100.0 / 256.0, 9)))
      .byte(op(NaplpsPdi::kPointSetRel));
  for (int i = 0; i < 20; ++i) {
    record.add(word2(3, field(0.125, 9), 0));
  }
  for (int i = 0; i < 20; ++i) {
    record.add(word2(3, field(-0.125, 9), field(1.0 / 256.0, 9)));
  }
  record.byte(op(NaplpsPdi::kPointRel)).add(word2(3, 0, 0)).text_mode();

  const std::vector<NabtsPoint> points = points_of(run(record));
  ASSERT_EQ(points.size(), 1u);
  EXPECT_DOUBLE_EQ(points[0].x, 0.5);
  EXPECT_DOUBLE_EQ(points[0].y, 120.0 / 256.0);
}

// ---------------------------------------------------------------------------
// Operand length
// ---------------------------------------------------------------------------

// §5.3.2.2.5: "If an operand following an opcode is shorter than the length
// previously specified by the DOMAIN command ... trailing zero bits are
// supplied by the receiving presentation process." A word a code ends — SI,
// or the next opcode — after one or two of its three bytes is completed with
// zeros, absolute or relative. Both receivers of the day zero-fill so. (A word
// the record itself ends inside is another matter, pinned in
// naplps_interpreter_test: APartialWordTheRecordEndsInIsNotDrawn.)
TEST(NaplpsCoordinates, AShortWordEndedByACodeIsZeroFilled) {
  // X 010 011 (000) = 152/256, Y 001 010 (000) = 80/256.
  const std::vector<uint8_t> two_bytes{numeric(0b010001), numeric(0b011010)};
  {
    Record record;
    record.pdi().byte(op(NaplpsPdi::kPointAbs)).add(two_bytes).text_mode();
    const std::vector<NabtsPoint> points = points_of(run(record));
    ASSERT_EQ(points.size(), 1u);
    EXPECT_DOUBLE_EQ(points[0].x, 152.0 / 256.0);
    EXPECT_DOUBLE_EQ(points[0].y, 80.0 / 256.0);
  }
  {
    // Ended by the next opcode rather than by SI.
    Record record;
    record.pdi()
        .byte(op(NaplpsPdi::kPointAbs))
        .add(two_bytes)
        .byte(op(NaplpsPdi::kPointSetAbs))
        .add(word2(3, field(0.75, 9), field(0.75, 9)))
        .text_mode();
    const std::vector<NabtsPoint> points = points_of(run(record));
    ASSERT_EQ(points.size(), 1u);
    EXPECT_DOUBLE_EQ(points[0].x, 152.0 / 256.0);
    EXPECT_DOUBLE_EQ(points[0].y, 80.0 / 256.0);
  }
  {
    // One byte: X 010 = 1/2, Y 011 = 3/4.
    Record record;
    record.pdi()
        .byte(op(NaplpsPdi::kPointAbs))
        .byte(numeric(0b010011))
        .text_mode();
    const std::vector<NabtsPoint> points = points_of(run(record));
    ASSERT_EQ(points.size(), 1u);
    EXPECT_DOUBLE_EQ(points[0].x, 0.5);
    EXPECT_DOUBLE_EQ(points[0].y, 0.75);
  }
  {
    // A relative word of one byte: X 001 = +1/4, Y 111 = -1/4.
    Record record;
    record.pdi()
        .byte(op(NaplpsPdi::kPointSetAbs))
        .add(word2(3, field(0.5, 9), field(0.5, 9)))
        .byte(op(NaplpsPdi::kPointRel))
        .byte(numeric(0b001111))
        .text_mode();
    const std::vector<NabtsPoint> points = points_of(run(record));
    ASSERT_EQ(points.size(), 1u);
    EXPECT_DOUBLE_EQ(points[0].x, 0.75);
    EXPECT_DOUBLE_EQ(points[0].y, 0.25);
  }
}

// ---------------------------------------------------------------------------
// DOMAIN
// ---------------------------------------------------------------------------

// §5.3.2.2.6: "If the logical pel size operand is omitted, the size of the
// logical pel shall not be changed." A DOMAIN of byte 1 alone keeps the pel an
// earlier one set, even as it changes the multi-value length. The TTX 650
// keeps it (an 8 x 8 point); the TU-2000x clears the pel before reading an
// operand that is not there and draws one pixel, which this deliberately does
// not follow.
TEST(NaplpsCoordinates, ADomainWithoutAPelKeepsThePel) {
  const double eight = 8.0 / 256.0;
  for (const int second_length : {3, 4}) {
    Record record;
    record.pdi()
        .add({op(NaplpsPdi::kDomain), domain_byte1(3, 1, false)})
        .add(word2(3, field(eight, 9), field(eight, 9)))
        .add({op(NaplpsPdi::kDomain), domain_byte1(second_length, 1, false)})
        .byte(op(NaplpsPdi::kPointAbs))
        .add(word2(second_length, field(0.5, 3 * second_length),
                   field(100.0 / 256.0, 3 * second_length)))
        .text_mode();
    const NabtsPageSnapshot page = run(record);
    ASSERT_EQ(page.primitives.size(), 1u);
    EXPECT_DOUBLE_EQ(page.primitives[0].logical_pel.dx, eight);
    EXPECT_DOUBLE_EQ(page.primitives[0].logical_pel.dy, eight);
    EXPECT_DOUBLE_EQ(page.primitives[0].points.front().x, 0.5);
  }
}

// ---------------------------------------------------------------------------
// Single-value operands
// ---------------------------------------------------------------------------

// §5.3.2.2.2 Table 4: DOMAIN b2-b1 set the single-value length, and §5.3.1
// Figure 10 reads the bytes as one unsigned integer, most significant first.
// §5.3.2.6.1: a colour map address shorter than the operand is "left justified
// within the single-value operand", so with two-byte single values SELECT
// COLOR 70 4C is one 12-bit operand whose top four bits are 1100: entry 12, in
// colour mode 1 — and the second byte's bits are low-order bits, not a second
// operand. The TTX 650 reads it so; the TU-2000x reads
// single values a byte at a time whatever DOMAIN says and takes this as colour
// mode 2, which this deliberately does not follow.
TEST(NaplpsCoordinates, ATwoByteSingleValueIsOneTwelveBitAddress) {
  const auto select = [](std::initializer_list<uint8_t> operand) {
    Record record;
    record.pdi()
        .add({op(NaplpsPdi::kDomain), domain_byte1(3, 2, false),
              op(NaplpsPdi::kSelectColour)})
        .add(operand)
        .byte(op(NaplpsPdi::kPointAbs))
        .add(word2(3, field(0.5, 9), field(0.25, 9)))
        .text_mode();
    return run(record);
  };

  {
    const NabtsPageSnapshot page = select({numeric(12 << 2), numeric(3 << 2)});
    ASSERT_EQ(page.primitives.size(), 1u);
    EXPECT_EQ(page.primitives[0].colour_mode, NabtsColourMode::kMapped);
    EXPECT_EQ(page.primitives[0].colour_map_address, 12);
  }
  {
    // The low bits of the first byte and all of the second are below the
    // address's four bits.
    const NabtsPageSnapshot page =
        select({numeric((12 << 2) | 0x3), numeric(0x3F)});
    ASSERT_EQ(page.primitives.size(), 1u);
    EXPECT_EQ(page.primitives[0].colour_mode, NabtsColourMode::kMapped);
    EXPECT_EQ(page.primitives[0].colour_map_address, 12);
  }
  {
    // An address carried in the second byte alone is entry 0.
    const NabtsPageSnapshot page = select({numeric(0), numeric(12 << 2)});
    ASSERT_EQ(page.primitives.size(), 1u);
    EXPECT_EQ(page.primitives[0].colour_mode, NabtsColourMode::kMapped);
    EXPECT_EQ(page.primitives[0].colour_map_address, 0);
  }
  {
    // Two two-byte operands: colour mode 2 (§5.3.2.6.3), drawing 12 on 3.
    const NabtsPageSnapshot page =
        select({numeric(12 << 2), numeric(0), numeric(3 << 2), numeric(0)});
    ASSERT_EQ(page.primitives.size(), 1u);
    EXPECT_EQ(page.primitives[0].colour_mode,
              NabtsColourMode::kMappedWithBackground);
    EXPECT_EQ(page.primitives[0].colour_map_address, 12);
    EXPECT_EQ(page.primitives[0].background_map_address, 3);
  }
}

}  // namespace
}  // namespace orc
