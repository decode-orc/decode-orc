/*
 * File:        naplps_pdi.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     The NAPLPS picture description instruction set: opcodes and
 *              operand decoding (X3.110 §5.3)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NAPLPS_PDI_H
#define ORC_NAPLPS_PDI_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "vbi-services/nabts_page.h"

namespace orc {

/**
 * @brief The PDI set of X3.110 Figure 13
 *
 * Columns 2 and 3 of the 96-position set are opcodes and columns 4 to 7 are
 * numeric data, which §5.3.1 makes distinguishable by b7 alone: "If b7 is set
 * to 0, an opcode is indicated. If b7 is set to 1, numeric data (ie, an
 * operand) is indicated."
 *
 * The values are the code positions themselves, so the parser dispatches on the
 * byte rather than through a lookup table. Taken from Figure 13 directly.
 */
enum class NaplpsPdi : uint8_t {
  // Column 2, rows 0-3: the first four control codes.
  kReset = 0x20,    // 2/0  §5.3.2.9
  kDomain = 0x21,   // 2/1  §5.3.2.2
  kText = 0x22,     // 2/2  §5.3.2.3
  kTexture = 0x23,  // 2/3  §5.3.2.4

  // Column 2, rows 4-7: POINT (§5.3.3.1). "SET" forms move the drawing point
  // without drawing; the visible forms draw a point of the logical pel's size.
  kPointSetAbs = 0x24,  // 2/4
  kPointSetRel = 0x25,  // 2/5
  kPointAbs = 0x26,     // 2/6
  kPointRel = 0x27,     // 2/7

  // Column 2, rows 8-11: LINE (§5.3.3.2). The plain forms start at the current
  // drawing point; the SET forms carry their own start point.
  kLineAbs = 0x28,     // 2/8
  kLineRel = 0x29,     // 2/9
  kSetLineAbs = 0x2A,  // 2/10
  kSetLineRel = 0x2B,  // 2/11

  // Column 2, rows 12-15: ARC (§5.3.3.3).
  kArcOutlined = 0x2C,     // 2/12
  kArcFilled = 0x2D,       // 2/13
  kSetArcOutlined = 0x2E,  // 2/14
  kSetArcFilled = 0x2F,    // 2/15

  // Column 3, rows 0-3: RECTANGLE (§5.3.3.4).
  kRectOutlined = 0x30,     // 3/0
  kRectFilled = 0x31,       // 3/1
  kSetRectOutlined = 0x32,  // 3/2
  kSetRectFilled = 0x33,    // 3/3

  // Column 3, rows 4-7: POLYGON (§5.3.3.5).
  kPolyOutlined = 0x34,     // 3/4
  kPolyFilled = 0x35,       // 3/5
  kSetPolyOutlined = 0x36,  // 3/6
  kSetPolyFilled = 0x37,    // 3/7

  // Column 3, rows 8-11: the INCREMENTAL group (§5.3.3.6).
  kField = 0x38,           // 3/8  §5.3.3.6.2
  kIncrPoint = 0x39,       // 3/9  §5.3.3.6.3
  kIncrLine = 0x3A,        // 3/10 §5.3.3.6.4
  kIncrPolyFilled = 0x3B,  // 3/11 §5.3.3.6.5

  // Column 3, rows 12-15: the remaining four control codes.
  kSetColour = 0x3C,     // 3/12 §5.3.2.5
  kWait = 0x3D,          // 3/13 §5.3.2.8
  kSelectColour = 0x3E,  // 3/14 §5.3.2.6
  kBlink = 0x3F,         // 3/15 §5.3.2.7
};

/// §5.3.1: b7 clear is an opcode, b7 set is numeric data. In the 96-position
/// set that is columns 2-3 against columns 4-7.
constexpr bool naplps_is_pdi_opcode(uint8_t byte) {
  return byte >= 0x20 && byte <= 0x3F;
}
constexpr bool naplps_is_pdi_numeric(uint8_t byte) {
  return byte >= 0x40 && byte <= 0x7F;
}

/// The figures only their last word closes — an arc through its points, a
/// polygon round its vertices, an incremental polygon to its last step
/// (§5.3.3.3, §5.3.3.5, §5.3.3.6.5) — which a receiver cannot draw until a code
/// has ended the command.
constexpr bool naplps_pdi_closes_on_its_last_word(NaplpsPdi pdi) {
  return (pdi >= NaplpsPdi::kArcOutlined && pdi <= NaplpsPdi::kSetArcFilled) ||
         (pdi >= NaplpsPdi::kPolyOutlined &&
          pdi <= NaplpsPdi::kSetPolyFilled) ||
         pdi == NaplpsPdi::kIncrPolyFilled;
}

/// The commands whose operands are coordinate words and nothing else (§5.3.3.1
/// to §5.3.3.6.2), so that a run of them divides into whole words.
constexpr bool naplps_pdi_takes_coordinate_words(NaplpsPdi pdi) {
  return pdi >= NaplpsPdi::kPointSetAbs && pdi <= NaplpsPdi::kField;
}

/// Six bits of payload per numeric byte, b6 to b1 (§5.3.1, Figure 10).
constexpr int kNaplpsNumericBits = 6;
constexpr uint8_t kNaplpsNumericMask = 0x3F;

/// §5.3.2.2.2 Table 4: one to four bytes, defaulting to one.
constexpr size_t kNaplpsSingleValueDefaultBytes = 1;
/// §5.3.2.2.3 Table 5: one to eight bytes, defaulting to three.
constexpr size_t kNaplpsMultiValueDefaultBytes = 3;

/// §5.3.3.6.3: "The range of values of the packing counter shall be 1 to 48,
/// inclusive" — the bits one INCREMENTAL POINT colour specification takes.
constexpr size_t kNaplpsMaxPackingCount = 48;

/// §5.3.2.8.2, Figure 28: the fixed format byte a WAIT's operand opens with,
/// 5/12, as the six payload bits a numeric byte carries.
constexpr uint8_t kNaplpsWaitFormat = 0x1C;

/// Vertices Table D1 item 4 requires of a polygon and of a spline. A bound
/// rather than a limit of the format: it is what an information provider may
/// assume a receiver will hold.
constexpr size_t kNaplpsMaxVertices = 256;

/**
 * @brief How the operand words of a PDI are read
 *
 * §5.3.2.2 makes both lengths a property of the presentation state rather than
 * of the opcode, so they travel together and are passed to every decode.
 */
struct NaplpsOperandFormat {
  size_t single_value_bytes = kNaplpsSingleValueDefaultBytes;
  size_t multi_value_bytes = kNaplpsMultiValueDefaultBytes;
  /// §5.3.2.2.4: three-dimensional mode, in which each coordinate word carries
  /// a Z the receiver "is to ignore, thereby projecting the image into the
  /// two-dimensional (X,Y) plane".
  bool three_dimensional = false;

  /// Components a coordinate word carries: X and Y, or X, Y and Z.
  size_t coordinate_components() const { return three_dimensional ? 3 : 2; }
};

/**
 * @brief A run of numeric data bytes, read as the operands of one opcode
 *
 * The bytes are collected first and interpreted after, because §5.3.2.2.5 makes
 * the count decide the meaning: an operand shorter than the stated length is
 * zero-extended, and one longer "is taken as an indication to repeat the
 * execution of the opcode with the subsequent numeric data taken as new
 * operands". Neither can be known while still reading.
 */
class NaplpsOperandReader {
 public:
  NaplpsOperandReader(const uint8_t* bytes, size_t length,
                      NaplpsOperandFormat format)
      : bytes_(bytes), length_(length), format_(format) {}

  /// Numeric bytes available.
  size_t remaining() const { return length_ - position_; }
  bool empty() const { return remaining() == 0; }

  /**
   * @brief Read one single-value operand as an unsigned integer (Figure 10)
   *
   * §5.3.1: "interpreted as unsigned integers (ordinal numbers) composed of the
   * sequence of concatenated bits taken consecutively (high order bit or b6 to
   * low order bit or b1) from the numeric data bytes". A short read is
   * zero-extended per §5.3.2.2.5.
   */
  uint32_t read_single_value();

  /**
   * @brief Read one coordinate word (Figure 11)
   *
   * Each byte carries one bit of X, one of Y and — in three-dimensional mode —
   * one of Z, most significant first. §5.3.1 makes them "signed, two's
   * complement numbers, ie, binary decimals where the MSB represents the digit
   * just to the right of the decimal point", so the value is in [-1, 1).
   *
   * The Z component is parsed and discarded, which §5.3.2.2.4 requires and
   * Table D1 item 5(1)(c) confirms: "the third dimension shall be ignored when
   * in the three-dimensional mode".
   */
  NabtsPoint read_coordinate();

  /**
   * @brief Read one colour word (Figure 12)
   *
   * "Each byte contains two three-tuples. Each three-tuple contains one bit for
   * each of the three primary colors. These are specified in the order green,
   * red, blue, which is the order of decreasing luminance." So a byte's six
   * bits are G R B G R B, and the components are the concatenated bits taken
   * one from each three-tuple.
   *
   * Truncated to three bits per gun, which is what Table D1 item 5(4) requires
   * a receiver to have and §5.3.2.5 says to do with more: "the operand is
   * truncated and only the most significant bits are used".
   */
  NabtsColour read_colour();

  /// Read one fixed-format byte: the six payload bits of the next numeric byte,
  /// or 0 if there is none.
  uint8_t read_fixed_byte();

  /// Whether the last read ran out of bytes, which §5.3.2.2.5 makes legal
  /// (zero-extended) but which a caller may want to count.
  bool truncated() const { return truncated_; }

  /// Change the format for the reads still to come.
  ///
  /// DOMAIN needs this and nothing else does: §5.3.2.2.6 notes that "the new
  /// length of the multi-value operands, as set in byte 1, applies to the
  /// multi-value logical pel size operand of that DOMAIN command" — so the
  /// command's own second operand is read on the length its first operand just
  /// established.
  void set_format(NaplpsOperandFormat format) { format_ = format; }

 private:
  /// Next numeric byte's six payload bits, or 0 with |truncated_| set.
  uint8_t next();

  const uint8_t* bytes_ = nullptr;
  size_t length_ = 0;
  size_t position_ = 0;
  NaplpsOperandFormat format_;
  bool truncated_ = false;
};

/**
 * @brief A colour given as a string of bits (§5.3.2.5.1, §5.3.3.6.3)
 *
 * The |count| low bits of |bits|, most significant first, are three-tuples in
 * the order green, red, blue — the arrangement Figure 12 gives a colour word
 * and §5.3.3.6.3 gives an INCREMENTAL POINT colour specification — so each gun
 * takes every third bit. A count that is not a multiple of three gives the
 * guns unequal accuracy ("GRBG" for four), which §5.3.3.6.3 allows. Each gun is
 * then scaled as §5.3.2.5.1 has it: more than three bits truncate to the most
 * significant three, and fewer are spread so their maximum is full intensity.
 */
NabtsColour naplps_colour_from_bits(uint64_t bits, size_t count);

/// Whether |value| lies within the unit screen, 0 <= v < 1 (§5.3.1).
constexpr bool naplps_in_unit_screen(double value) {
  return value >= 0.0 && value < 1.0;
}

/**
 * @brief |point| clamped into the unit screen, which is where the cursor is
 *        kept
 *
 * Just under 1 at the top, since §5.3.1 makes the range noninclusive there.
 * Only the cursor is moved so: text written at a cursor pushed off the screen
 * would simply vanish. A drawing keeps the points the record gave it (see
 * NaplpsInterpreter::reject_off_screen()).
 */
NabtsPoint naplps_on_unit_screen(NabtsPoint point);

}  // namespace orc

#endif  // ORC_NAPLPS_PDI_H
