/*
 * File:        naplps_code_env_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NAPLPS code environment and PDI operand
 *              decoding (X3.110 §4.3, §5.3.1)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_code_env.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "naplps_pdi.h"

namespace orc {
namespace {

/// A code position in the standard's column/row notation.
constexpr uint8_t code(int column, int row) {
  return static_cast<uint8_t>((column << 4) | row);
}

NaplpsEscape parse(const std::vector<uint8_t>& after_esc) {
  return naplps_parse_escape(after_esc.data(), after_esc.size());
}

////////////////////////////////////////////////////////////////////////////////////////////
// Designation (§4.3.2 Table 1)
////////////////////////////////////////////////////////////////////////////////////////////

// §4.3.1.3: "In the default state G0 contains the primary character set, G1 the
// PDI set, G2 the supplementary character set, and G3 the mosaic set", with G0
// invoked.
TEST(NaplpsCodeEnvironment, StartsInTheDefaultDesignationAndInvocation) {
  const NaplpsCodeEnvironment env;
  EXPECT_EQ(env.designated(NaplpsGSlot::kG0), NaplpsGSet::kPrimary);
  EXPECT_EQ(env.designated(NaplpsGSlot::kG1), NaplpsGSet::kPdi);
  EXPECT_EQ(env.designated(NaplpsGSlot::kG2), NaplpsGSet::kSupplementary);
  EXPECT_EQ(env.designated(NaplpsGSlot::kG3), NaplpsGSet::kMosaic);
  EXPECT_EQ(env.invoked_slot(), NaplpsGSlot::kG0);
  EXPECT_EQ(env.in_use(), NaplpsGSet::kPrimary);
}

// Table 1's worked example: "The F character for the primary character set, for
// example, is 4/2 and for G0 the I character is 2/8. The three character escape
// sequence ESC 2/8 4/2, therefore, designates the primary character set as the
// current G0 set."
TEST(NaplpsParseEscape, DesignatesThePrimarySetIntoG0) {
  const NaplpsEscape escape = parse({code(2, 8), code(4, 2)});
  EXPECT_EQ(escape.kind, NaplpsEscapeKind::kDesignation);
  EXPECT_EQ(escape.slot, NaplpsGSlot::kG0);
  EXPECT_EQ(escape.set, NaplpsGSet::kPrimary);
  EXPECT_EQ(escape.length, 3u);  // ESC + intermediate + final
}

TEST(NaplpsParseEscape, DesignatesEvery94CharacterSetIntoEverySlot) {
  struct Case {
    uint8_t intermediate;
    NaplpsGSlot slot;
  };
  const Case slots[] = {{code(2, 8), NaplpsGSlot::kG0},
                        {code(2, 9), NaplpsGSlot::kG1},
                        {code(2, 10), NaplpsGSlot::kG2},
                        {code(2, 11), NaplpsGSlot::kG3}};

  for (const Case& slot : slots) {
    const NaplpsEscape primary = parse({slot.intermediate, code(4, 2)});
    EXPECT_EQ(primary.kind, NaplpsEscapeKind::kDesignation);
    EXPECT_EQ(primary.slot, slot.slot);
    EXPECT_EQ(primary.set, NaplpsGSet::kPrimary);

    const NaplpsEscape supplementary = parse({slot.intermediate, code(7, 12)});
    EXPECT_EQ(supplementary.set, NaplpsGSet::kSupplementary);
  }
}

// Table 1's 96-character sets take 2/9 to 2/11 for G1 to G3, "and 2/13, 2/14,
// 2/15 for G1, G2, G3, respectively" — the dual coding the standard's own
// footnote calls out.
TEST(NaplpsParseEscape, AcceptsBothIntermediateFormsForThe96CharacterSets) {
  struct Case {
    uint8_t primary_form;
    uint8_t alternate_form;
    NaplpsGSlot slot;
  };
  const Case slots[] = {{code(2, 9), code(2, 13), NaplpsGSlot::kG1},
                        {code(2, 10), code(2, 14), NaplpsGSlot::kG2},
                        {code(2, 11), code(2, 15), NaplpsGSlot::kG3}};
  struct SetCase {
    uint8_t final_byte;
    NaplpsGSet set;
  };
  const SetCase sets[] = {{code(5, 7), NaplpsGSet::kPdi},
                          {code(7, 13), NaplpsGSet::kMosaic},
                          {code(7, 10), NaplpsGSet::kMacro},
                          {code(7, 11), NaplpsGSet::kDrcs}};

  for (const Case& slot : slots) {
    for (const SetCase& set : sets) {
      for (const uint8_t intermediate :
           {slot.primary_form, slot.alternate_form}) {
        const NaplpsEscape escape = parse({intermediate, set.final_byte});
        EXPECT_EQ(escape.kind, NaplpsEscapeKind::kDesignation);
        EXPECT_EQ(escape.slot, slot.slot);
        EXPECT_EQ(escape.set, set.set);
      }
    }
  }
}

// §4.3.1.2 makes G0 a 94-position set, and Table 1 lists no G0 intermediate for
// the 96-position sets. Such a sequence is well-formed and designates nothing
// usable, which §4.3.2 makes the null set.
TEST(NaplpsParseEscape, RefusesA96CharacterSetIntoG0) {
  const NaplpsEscape escape = parse({code(2, 8), code(5, 7)});
  EXPECT_EQ(escape.kind, NaplpsEscapeKind::kDesignation);
  EXPECT_EQ(escape.slot, NaplpsGSlot::kG0);
  EXPECT_EQ(escape.set, NaplpsGSet::kNull);
}

// §4.3.2: "All other designation sequences shall designate either a null G- or
// a null C1-set."
TEST(NaplpsParseEscape, DesignatesTheNullSetForAnUnknownFinalCharacter) {
  const NaplpsEscape escape = parse({code(2, 9), code(6, 5)});
  EXPECT_EQ(escape.kind, NaplpsEscapeKind::kDesignation);
  EXPECT_EQ(escape.set, NaplpsGSet::kNull);
}

// §4.3.2: "The redesignation of the C0 set is not permitted in the context of
// this standard and such redesignating escape sequences shall be ignored."
TEST(NaplpsParseEscape, SkipsAControlSetDesignation) {
  for (const uint8_t intermediate : {code(2, 1), code(2, 2)}) {
    const NaplpsEscape escape = parse({intermediate, code(4, 0)});
    EXPECT_EQ(escape.kind, NaplpsEscapeKind::kUnsupported);
    EXPECT_EQ(escape.length, 3u);
  }
}

////////////////////////////////////////////////////////////////////////////////////////////
// Invocation (§4.3.2 Table 2)
////////////////////////////////////////////////////////////////////////////////////////////

TEST(NaplpsParseEscape, ReadsTheLockingShiftsThe7BitEnvironmentHas) {
  const NaplpsEscape ls2 = parse({code(6, 14)});
  EXPECT_EQ(ls2.kind, NaplpsEscapeKind::kLockingShift);
  EXPECT_EQ(ls2.slot, NaplpsGSlot::kG2);
  EXPECT_EQ(ls2.length, 2u);

  const NaplpsEscape ls3 = parse({code(6, 15)});
  EXPECT_EQ(ls3.kind, NaplpsEscapeKind::kLockingShift);
  EXPECT_EQ(ls3.slot, NaplpsGSlot::kG3);
}

// Table 2's right-hand locking shifts exist only in the 8-bit environment,
// which CEA-516 §6.1 does not use.
TEST(NaplpsParseEscape, DoesNotImplementTheRightHandLockingShifts) {
  for (const uint8_t final_byte : {code(7, 12), code(7, 13), code(7, 14)}) {
    const NaplpsEscape escape = parse({final_byte});
    EXPECT_EQ(escape.kind, NaplpsEscapeKind::kUnsupported) << +final_byte;
  }
}

// §4.3.2: SI invokes G0 and SO invokes G1, both locking, so they hold until
// something else acts.
TEST(NaplpsCodeEnvironment, LockingShiftsHoldAcrossCharacters) {
  NaplpsCodeEnvironment env;
  env.invoke_locking(NaplpsGSlot::kG1);
  EXPECT_EQ(env.in_use(), NaplpsGSet::kPdi);
  env.consume_character();
  EXPECT_EQ(env.in_use(), NaplpsGSet::kPdi);
  env.consume_character();
  EXPECT_EQ(env.in_use(), NaplpsGSet::kPdi);
}

// §6.1.3.3: SS2 invokes G2 "in a nonlocking manner", so it lasts one character
// and then the last locking invocation comes back.
TEST(NaplpsCodeEnvironment, ASingleShiftRevertsAfterOneCharacter) {
  NaplpsCodeEnvironment env;
  env.invoke_locking(NaplpsGSlot::kG1);  // SO: PDI

  env.invoke_single_shift(NaplpsGSlot::kG2);
  EXPECT_TRUE(env.single_shift_pending());
  EXPECT_EQ(env.in_use(), NaplpsGSet::kSupplementary);

  env.consume_character();
  EXPECT_FALSE(env.single_shift_pending());
  EXPECT_EQ(env.in_use(), NaplpsGSet::kPdi)
      << "the single shift did not revert";
}

TEST(NaplpsCodeEnvironment, ASingleShiftRevertsToG0WhenThatIsWhatWasLocked) {
  NaplpsCodeEnvironment env;
  env.invoke_single_shift(NaplpsGSlot::kG3);
  EXPECT_EQ(env.in_use(), NaplpsGSet::kMosaic);
  env.consume_character();
  EXPECT_EQ(env.in_use(), NaplpsGSet::kPrimary);
}

TEST(NaplpsCodeEnvironment, ResetReturnsToTheDefaultState) {
  NaplpsCodeEnvironment env;
  env.designate(NaplpsGSlot::kG0, NaplpsGSet::kNull);
  env.invoke_locking(NaplpsGSlot::kG3);
  env.invoke_single_shift(NaplpsGSlot::kG2);

  env.reset();
  EXPECT_EQ(env.designated(NaplpsGSlot::kG0), NaplpsGSet::kPrimary);
  EXPECT_EQ(env.invoked_slot(), NaplpsGSlot::kG0);
  EXPECT_FALSE(env.single_shift_pending());
}

////////////////////////////////////////////////////////////////////////////////////////////
// C1 and malformed sequences
////////////////////////////////////////////////////////////////////////////////////////////

// Figure 65's "Definition of Columns A and B": in a 7-bit code a C1 control is
// a two-character escape sequence whose final character is taken with A = 4 and
// B = 5.
TEST(NaplpsParseEscape, ReadsTheC1SetAsTwoCharacterEscapeSequences) {
  struct Case {
    uint8_t final_byte;
    NaplpsC1 control;
  };
  const Case cases[] = {
      {code(4, 0), NaplpsC1::kDefMacro},
      {code(4, 3), NaplpsC1::kDefDrcs},
      {code(4, 5), NaplpsC1::kEnd},
      {code(4, 12), NaplpsC1::kNormalText},
      {code(4, 15), NaplpsC1::kDoubleSize},
      {code(5, 0), NaplpsC1::kProtect},
      {code(5, 9), NaplpsC1::kUnderlineStart},
      {code(5, 15), NaplpsC1::kUnprotect},
  };

  for (const Case& test_case : cases) {
    const NaplpsEscape escape = parse({test_case.final_byte});
    EXPECT_EQ(escape.kind, NaplpsEscapeKind::kControl) << +test_case.final_byte;
    EXPECT_EQ(escape.c1, test_case.control) << +test_case.final_byte;
    EXPECT_EQ(escape.length, 2u);
  }
}

// §6.2.2.1, §6.2.3 and §6.2.4 each list the same six controls as the
// terminators of a definition in progress.
TEST(NaplpsC1Set, NamesTheControlsThatTerminateADefinition) {
  for (const NaplpsC1 control :
       {NaplpsC1::kDefMacro, NaplpsC1::kDefpMacro, NaplpsC1::kDeftMacro,
        NaplpsC1::kDefDrcs, NaplpsC1::kDefTexture, NaplpsC1::kEnd}) {
    EXPECT_TRUE(naplps_terminates_definition(control));
  }
  for (const NaplpsC1 control :
       {NaplpsC1::kRepeat, NaplpsC1::kNormalText, NaplpsC1::kBlinkStart,
        NaplpsC1::kUnderlineStart, NaplpsC1::kProtect}) {
    EXPECT_FALSE(naplps_terminates_definition(control));
  }
}

// §4.3.2: "The occurrence of any other bit combination in an escape sequence
// shall cause the partial escape sequence to be terminated and ignored, and
// that bit combination shall be executed." So the offending byte must be left
// unconsumed — the length stops before it.
TEST(NaplpsParseEscape, LeavesTheByteThatBrokeTheSequenceToBeExecuted) {
  const NaplpsEscape escape = parse({code(2, 9), 0x0D, code(4, 2)});
  EXPECT_EQ(escape.kind, NaplpsEscapeKind::kMalformed);
  // ESC + the one intermediate, and not the 0/13 that broke it.
  EXPECT_EQ(escape.length, 2u);
}

TEST(NaplpsParseEscape, ReportsASequenceTheRecordRanOutOf) {
  EXPECT_EQ(parse({}).kind, NaplpsEscapeKind::kTruncated);
  EXPECT_EQ(parse({code(2, 9)}).kind, NaplpsEscapeKind::kTruncated);
}

// §4.3.2 permits "zero or more occurrences of intermediate characters", so two
// is well-formed — and names nothing this standard defines.
TEST(NaplpsParseEscape, SkipsAWellFormedSequenceWithTooManyIntermediates) {
  const NaplpsEscape escape = parse({code(2, 9), code(2, 9), code(4, 2)});
  EXPECT_EQ(escape.kind, NaplpsEscapeKind::kUnsupported);
  EXPECT_EQ(escape.length, 4u) << "the whole sequence must be skipped";
}

////////////////////////////////////////////////////////////////////////////////////////////
// Transparent controls (§6.1.4, §6.1.5, §6.1.6.1)
////////////////////////////////////////////////////////////////////////////////////////////

// The transmission controls (0/1-0/6, 1/0, 1/5-1/7), the device controls
// (1/1-1/4) and NUL (0/0) "have no effect on the presentation layer ... They
// may be embedded within any presentation layer sequence without affecting that
// sequence."
TEST(NaplpsTransparentControls, CoverExactlyTheCodesTheStandardLists) {
  const uint8_t transparent[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x10,
                                 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17};
  for (const uint8_t byte : transparent) {
    EXPECT_TRUE(naplps_is_transparent_control(byte)) << +byte;
  }

  // Everything else in columns 0 and 1 does have an effect.
  const uint8_t effective[] = {0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
                               0x0D, 0x0E, 0x0F, 0x18, 0x19, 0x1A,
                               0x1B, 0x1C, 0x1D, 0x1E, 0x1F};
  for (const uint8_t byte : effective) {
    EXPECT_FALSE(naplps_is_transparent_control(byte)) << +byte;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////
// PDI opcodes and operands (§5.3.1)
////////////////////////////////////////////////////////////////////////////////////////////

// §5.3.1: "If b7 is set to 0, an opcode is indicated. If b7 is set to 1,
// numeric data (ie, an operand) is indicated." In the 96-position set that is
// columns 2-3 against 4-7, which is what Figure 13 draws.
TEST(NaplpsPdiSet, SeparatesOpcodesFromNumericDataByB7) {
  for (int column = 2; column <= 3; ++column) {
    for (int row = 0; row < 16; ++row) {
      EXPECT_TRUE(naplps_is_pdi_opcode(code(column, row)));
      EXPECT_FALSE(naplps_is_pdi_numeric(code(column, row)));
    }
  }
  for (int column = 4; column <= 7; ++column) {
    for (int row = 0; row < 16; ++row) {
      EXPECT_FALSE(naplps_is_pdi_opcode(code(column, row)));
      EXPECT_TRUE(naplps_is_pdi_numeric(code(column, row)));
    }
  }
}

// Figure 13's layout, opcode by opcode. Transcribed from the figure so that a
// mistranscription shows up here rather than as a page that draws nonsense.
TEST(NaplpsPdiSet, MatchesFigure13) {
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kReset), code(2, 0));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kDomain), code(2, 1));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kText), code(2, 2));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kTexture), code(2, 3));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kPointSetAbs), code(2, 4));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kPointSetRel), code(2, 5));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kPointAbs), code(2, 6));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kPointRel), code(2, 7));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kLineAbs), code(2, 8));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kLineRel), code(2, 9));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kSetLineAbs), code(2, 10));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kSetLineRel), code(2, 11));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kArcOutlined), code(2, 12));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kArcFilled), code(2, 13));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kSetArcOutlined), code(2, 14));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kSetArcFilled), code(2, 15));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kRectOutlined), code(3, 0));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kRectFilled), code(3, 1));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kSetRectOutlined), code(3, 2));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kSetRectFilled), code(3, 3));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kPolyOutlined), code(3, 4));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kPolyFilled), code(3, 5));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kSetPolyOutlined), code(3, 6));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kSetPolyFilled), code(3, 7));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kField), code(3, 8));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kIncrPoint), code(3, 9));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kIncrLine), code(3, 10));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kIncrPolyFilled), code(3, 11));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kSetColour), code(3, 12));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kWait), code(3, 13));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kSelectColour), code(3, 14));
  EXPECT_EQ(static_cast<uint8_t>(NaplpsPdi::kBlink), code(3, 15));
}

/// A numeric-data byte carrying |payload| in b6-b1.
constexpr uint8_t numeric(uint8_t payload) {
  return static_cast<uint8_t>(0x40 | (payload & 0x3F));
}

// §5.3.1: a single-value operand is "unsigned integers (ordinal numbers)
// composed of the sequence of concatenated bits taken consecutively (high order
// bit or b6 to low order bit or b1) from the numeric data bytes".
TEST(NaplpsOperandReader, ReadsSingleValuesAsConcatenatedSixBitGroups) {
  NaplpsOperandFormat format;
  format.single_value_bytes = 2;
  const std::vector<uint8_t> bytes = {numeric(0b101010), numeric(0b010101)};
  NaplpsOperandReader reader(bytes.data(), bytes.size(), format);
  EXPECT_EQ(reader.read_single_value(), 0b101010010101u);
}

// §5.3.2.2.5: "If an operand following an opcode is shorter than the length
// previously specified ... trailing zero bits are supplied by the receiving
// presentation process."
TEST(NaplpsOperandReader, ZeroExtendsAShortSingleValue) {
  NaplpsOperandFormat format;
  format.single_value_bytes = 2;
  const std::vector<uint8_t> bytes = {numeric(0b111111)};
  NaplpsOperandReader reader(bytes.data(), bytes.size(), format);
  EXPECT_EQ(reader.read_single_value(), 0b111111000000u);
  EXPECT_TRUE(reader.truncated());
}

// Figure 11, two-dimensional mode: X in b6-b4 and Y in b3-b1 of every byte, the
// first byte carrying the most significant bits. §5.3.1 makes them two's
// complement binary fractions, so the MSB is the digit just right of the point.
TEST(NaplpsOperandReader, ReadsTwoDimensionalCoordinatesFromContiguousFields) {
  NaplpsOperandFormat format;
  format.multi_value_bytes = 1;

  // One byte gives three bits per component. §5.3.1 puts the point just left of
  // the sign bit — "the MSB represents the digit just to the right of the
  // decimal point" — so the weights are -1, 1/2, 1/4 and the range is [-1, 1).
  // That is what an absolute coordinate needs: §5.3.1 also calls for positions
  // "from 0 (inclusive) to 1 (noninclusive)", which a sign bit weighted -1/2
  // could not reach.
  //
  // X = 010 is therefore +1/2, and Y = 110 is -1 + 1/2 = -1/2.
  const std::vector<uint8_t> bytes = {numeric(0b010110)};
  NaplpsOperandReader reader(bytes.data(), bytes.size(), format);
  const NabtsPoint point = reader.read_coordinate();
  EXPECT_DOUBLE_EQ(point.x, 0.5);
  EXPECT_DOUBLE_EQ(point.y, -0.5);
}

TEST(NaplpsOperandReader, ReadsTheDefaultThreeByteCoordinateWord) {
  const NaplpsOperandFormat format;  // three bytes, two dimensions
  ASSERT_EQ(format.multi_value_bytes, 3u);

  // Nine bits per component. X = 0 1000 0000 = +0,5; Y = 1 0000 0000 = -1,0.
  const std::vector<uint8_t> bytes = {numeric(0b010100), numeric(0b000000),
                                      numeric(0b000000)};
  NaplpsOperandReader reader(bytes.data(), bytes.size(), format);
  const NabtsPoint point = reader.read_coordinate();
  EXPECT_DOUBLE_EQ(point.x, 0.5);
  EXPECT_DOUBLE_EQ(point.y, -1.0);
}

// Figure 11, three-dimensional mode: X in b6-b5, Y in b4-b3, Z in b2-b1. The Z
// is read past — §5.3.2.2.4 has the receiver ignore it — so the same bytes give
// a different X and Y than they would in two-dimensional mode.
TEST(NaplpsOperandReader,
     SplitsThreeDimensionalCoordinatesTwoBitsPerComponent) {
  NaplpsOperandFormat format;
  format.multi_value_bytes = 1;
  format.three_dimensional = true;
  ASSERT_EQ(format.coordinate_components(), 3u);

  // X = 01 = +0,5; Y = 10 = -1,0; Z = 11 ignored.
  const std::vector<uint8_t> bytes = {numeric(0b011011)};
  NaplpsOperandReader reader(bytes.data(), bytes.size(), format);
  const NabtsPoint point = reader.read_coordinate();
  EXPECT_DOUBLE_EQ(point.x, 0.5);
  EXPECT_DOUBLE_EQ(point.y, -1.0);
}

// Figure 12: "Each byte contains two three-tuples ... specified in the order
// green, red, blue", and a gun's value is its bits taken one per three-tuple.
TEST(NaplpsOperandReader, ReadsColourWordsAsGreenRedBlueTuples) {
  NaplpsOperandFormat format;
  format.multi_value_bytes = 2;

  // Byte 1 tuples: GRB = 100, GRB = 100. Byte 2: 100, 100. So green gets
  // 1111 and red and blue get 0000 — full green at four bits, the map's own
  // width.
  const std::vector<uint8_t> bytes = {numeric(0b100100), numeric(0b100100)};
  NaplpsOperandReader reader(bytes.data(), bytes.size(), format);
  const NabtsColour colour = reader.read_colour();
  EXPECT_EQ(colour.green, 15u);
  EXPECT_EQ(colour.red, 0u);
  EXPECT_EQ(colour.blue, 0u);
}

// §5.3.2.5.1 ends with the rule that decides how a short colour word is read:
// "For each primary, the maximum color fraction attainable, given the number of
// bits specified in the color value operand, shall be interpreted as full
// intensity and intermediate values shall be equally distributed between zero
// and full intensity." Over two bits at four bits a gun that is 0, 5, 10, 15,
// as the Norpak TTX 650 reads it.
//
// The earlier sentence about "trailing zero bits" would instead give 0, 4, 8,
// 12 and make white unreachable in a one-byte operand — which the reference
// ExtraVision service disproves, since it sets white with exactly one byte.
TEST(NaplpsOperandReader, ScalesAColourWordNarrowerThanFourBitsToFullRange) {
  NaplpsOperandFormat format;
  format.multi_value_bytes = 1;

  // One byte gives two bits per gun. GRB = 110, GRB = 000 → green 10, red 10,
  // blue 00 — 2 of 3, which is 10 of 15.
  const std::vector<uint8_t> bytes = {numeric(0b110000)};
  NaplpsOperandReader reader(bytes.data(), bytes.size(), format);
  const NabtsColour colour = reader.read_colour();
  EXPECT_EQ(colour.green, 10u);
  EXPECT_EQ(colour.red, 10u);
  EXPECT_EQ(colour.blue, 0u);

  // The brightest a two-bit operand can ask for is full intensity, or a service
  // could not send white in one byte.
  const std::vector<uint8_t> brightest = {numeric(0b111111)};
  NaplpsOperandReader white(brightest.data(), brightest.size(), format);
  EXPECT_EQ(white.read_colour(), kNabtsNominalWhite);
}

// A colour operand shorter than the DOMAIN-declared multi-value length is read
// for what it carries rather than refused: §5.3.2.5.1 supplies the missing bits
// itself, and the declared length is a maximum.
TEST(NaplpsOperandReader, ReadsAColourWordShorterThanTheDeclaredLength) {
  NaplpsOperandFormat format;
  format.multi_value_bytes = 3;

  const std::vector<uint8_t> bytes = {numeric(0b111111)};
  NaplpsOperandReader reader(bytes.data(), bytes.size(), format);
  EXPECT_EQ(reader.read_colour(), kNabtsNominalWhite);
}

// The cursor is kept on the unit screen, which §5.3.1 makes noninclusive at 1.
TEST(NaplpsOnUnitScreen, ClampsACoordinateOffTheScreen) {
  const NabtsPoint inside = naplps_on_unit_screen(NabtsPoint{0.5, 0.5});
  EXPECT_DOUBLE_EQ(inside.x, 0.5);
  EXPECT_DOUBLE_EQ(inside.y, 0.5);

  const NabtsPoint negative = naplps_on_unit_screen(NabtsPoint{-0.25, 0.5});
  EXPECT_DOUBLE_EQ(negative.x, 0.0);
  EXPECT_DOUBLE_EQ(negative.y, 0.5);

  const NabtsPoint past_the_end = naplps_on_unit_screen(NabtsPoint{0.5, 1.5});
  EXPECT_LT(past_the_end.y, 1.0) << "the unit screen is noninclusive at 1";
  EXPECT_GT(past_the_end.y, 0.99);
}

}  // namespace
}  // namespace orc
