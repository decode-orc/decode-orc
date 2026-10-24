/*
 * File:        eia608_line21_pairs_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for which line 21 byte pairs a receiver takes: their
 *              parity, the EIA-608 code table, the field order and the run
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/support/eia608_line21_pairs.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace orc_unit_test {
namespace {

using orc::eia608_pair_is_defined;
using orc::EIA608FieldOrder;
using orc::EIA608FieldReading;
using orc::EIA608Line21Run;
using orc::EIA608Pair;

/// A line that carried a pair, with each byte's parity as given.
EIA608FieldReading reading(uint8_t data0, uint8_t data1, bool parity0 = true,
                           bool parity1 = true) {
  EIA608FieldReading field;
  field.present = true;
  field.data0 = data0;
  field.data1 = data1;
  field.parity0_valid = parity0;
  field.parity1_valid = parity1;
  return field;
}

/// The pairs a run hands back, in order.
std::vector<EIA608Line21Run::Pair> pairs_of(const EIA608Line21Run& run) {
  std::vector<EIA608Line21Run::Pair> pairs;
  run.for_each(
      [&pairs](const EIA608Line21Run::Pair& pair) { pairs.push_back(pair); });
  return pairs;
}

////////////////////////////////////////////////////////////////////////////////
// Parity
////////////////////////////////////////////////////////////////////////////////

TEST(EIA608PairByParity, TakesAPairWhoseParitiesBothHeld) {
  const auto pair = orc::eia608_pair_by_parity(reading('H', 'i'));
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(pair->data0, 'H');
  EXPECT_EQ(pair->data1, 'i');
  EXPECT_FALSE(pair->first_only);
}

// A first byte that failed may have been a control code: the pair is dropped
// whole, and its encoder's second copy stands in [CTA-608-E §D.2].
TEST(EIA608PairByParity, DropsAPairWhoseFirstByteFailed) {
  EXPECT_FALSE(orc::eia608_pair_by_parity(reading('H', 'i', false, true)));
  EXPECT_FALSE(orc::eia608_pair_by_parity(reading(0x14, 0x2C, false, true)));
}

// A character whose partner failed is taken alone, as a receiver displays the
// character it did receive; a control code needs both bytes.
TEST(EIA608PairByParity, TakesACharacterWhosePartnerFailedAlone) {
  const auto pair = orc::eia608_pair_by_parity(reading('H', 'i', true, false));
  ASSERT_TRUE(pair.has_value());
  EXPECT_EQ(pair->data0, 'H');
  EXPECT_EQ(pair->data1, 0x00);
  EXPECT_TRUE(pair->first_only);

  EXPECT_FALSE(orc::eia608_pair_by_parity(reading(0x14, 0x2C, true, false)));
}

TEST(EIA608PairByParity, TakesNothingFromALineWithoutTheWaveform) {
  EIA608FieldReading empty;
  empty.parity0_valid = true;
  empty.parity1_valid = true;
  EXPECT_FALSE(orc::eia608_pair_by_parity(empty));
}

////////////////////////////////////////////////////////////////////////////////
// The bytes with no function on a field
////////////////////////////////////////////////////////////////////////////////

EIA608Pair pair_of(uint8_t data0, uint8_t data1) {
  EIA608Pair pair;
  pair.data0 = data0;
  pair.data1 = data1;
  return pair;
}

// A first byte 00h-0Fh is ignored and the second treated normally
// [47 CFR 15.119(i)(1)] — but on field 2, 01h-0Fh is the Extended Data
// Service's.
TEST(EIA608PairForField, IgnoresAFirstByteWithNoFunction) {
  auto pair = orc::eia608_pair_for_field(0, pair_of(0x05, 'A'));
  EXPECT_EQ(pair.data0, 0x00);
  EXPECT_EQ(pair.data1, 'A');

  pair = orc::eia608_pair_for_field(1, pair_of(0x05, 0x01));
  EXPECT_EQ(pair.data0, 0x05);
  EXPECT_EQ(pair.data1, 0x01);
}

// A text service can end a row's last character with 01h: the character is
// kept, the 01h ignored.
TEST(EIA608PairForField, KeepsACharacterWhosePartnerHasNoFunction) {
  for (const int field : {0, 1}) {
    const auto pair = orc::eia608_pair_for_field(field, pair_of('C', 0x01));
    EXPECT_EQ(pair.data0, 'C');
    EXPECT_EQ(pair.data1, 0x00);
  }
  const auto null_pair = orc::eia608_pair_for_field(0, pair_of(0x00, 0x1C));
  EXPECT_EQ(null_pair.data1, 0x00);
}

// Control codes and characters pass as they are.
TEST(EIA608PairForField, LeavesControlCodesAndCharactersAlone) {
  for (const auto& [data0, data1] : std::vector<std::pair<uint8_t, uint8_t>>{
           {0x14, 0x2C}, {0x11, 0x37}, {'H', 'i'}, {0x00, 0x00}}) {
    const auto pair = orc::eia608_pair_for_field(0, pair_of(data0, data1));
    EXPECT_EQ(pair.data0, data0);
    EXPECT_EQ(pair.data1, data1);
  }
}

////////////////////////////////////////////////////////////////////////////////
// The EIA-608 code table
////////////////////////////////////////////////////////////////////////////////

// Characters and nulls, on either field.
TEST(EIA608PairIsDefined, TakesCharactersAndNulls) {
  EXPECT_TRUE(eia608_pair_is_defined(0, 'H', 'i'));
  EXPECT_TRUE(eia608_pair_is_defined(0, 'A', 0x00));
  EXPECT_TRUE(eia608_pair_is_defined(1, 0x00, 0x00));
  EXPECT_TRUE(eia608_pair_is_defined(0, 0x7F, 0x20));
  // A second byte 01h-1Fh is neither a character nor a null.
  EXPECT_FALSE(eia608_pair_is_defined(0, 'A', 0x05));
  EXPECT_FALSE(eia608_pair_is_defined(0, 0x00, 0x1C));
}

// The control codes of CTA-608-E's tables, on both data channels; the code
// space between them is not taken.
TEST(EIA608PairIsDefined, TakesTheControlCodesOfTheTables) {
  EXPECT_TRUE(eia608_pair_is_defined(0, 0x14, 0x2C));  // EDM, channel 1
  EXPECT_TRUE(eia608_pair_is_defined(0, 0x1C, 0x20));  // RCL, channel 2
  EXPECT_TRUE(eia608_pair_is_defined(1, 0x15, 0x2F));  // EOC, field 2
  EXPECT_TRUE(eia608_pair_is_defined(0, 0x11, 0x2E));  // mid-row italics
  EXPECT_TRUE(eia608_pair_is_defined(0, 0x11, 0x37));  // music note
  EXPECT_TRUE(eia608_pair_is_defined(0, 0x12, 0x30));  // extended character
  EXPECT_TRUE(eia608_pair_is_defined(0, 0x17, 0x21));  // tab offset 1
  EXPECT_TRUE(eia608_pair_is_defined(0, 0x10, 0x2E));  // background
  EXPECT_TRUE(eia608_pair_is_defined(0, 0x14, 0x70));  // PAC row 15
  EXPECT_TRUE(eia608_pair_is_defined(0, 0x10, 0x50));  // PAC row 11

  EXPECT_FALSE(eia608_pair_is_defined(0, 0x14, 0x0C));  // second byte < 20h
  EXPECT_FALSE(eia608_pair_is_defined(0, 0x14, 0x30));
  EXPECT_FALSE(eia608_pair_is_defined(0, 0x16, 0x25));
  EXPECT_FALSE(eia608_pair_is_defined(0, 0x17, 0x2B));
  EXPECT_FALSE(eia608_pair_is_defined(0, 0x10, 0x60));  // row 11 has no 60h
}

// XDS control codes belong to field 2: on field 1 they have "no valid
// purpose" [CTA-608-E §D.2].
TEST(EIA608PairIsDefined, TakesXdsCodesOnTheSecondFieldOnly) {
  EXPECT_TRUE(eia608_pair_is_defined(1, 0x05, 0x01));
  EXPECT_TRUE(eia608_pair_is_defined(1, 0x0F, 0x1D));
  EXPECT_FALSE(eia608_pair_is_defined(0, 0x05, 0x01));
  EXPECT_FALSE(eia608_pair_is_defined(0, 0x0E, 0x50));
}

////////////////////////////////////////////////////////////////////////////////
// Field order
////////////////////////////////////////////////////////////////////////////////

// Captions on the first stored field spelled as field 1 spells them, XDS on
// the second: the normal order.
TEST(EIA608FieldOrder, KeepsTheNormalOrder) {
  EIA608FieldOrder order;
  for (int i = 0; i < 20; ++i) {
    order.add(0, 0x14, 0x2C);
    order.add(1, 0x05, 0x01);
  }
  EXPECT_FALSE(order.reversed());
}

// Some card dumps start a frame late: the captions are on the second stored
// field and spelled as field 1 spells them;
// the capture starts on television field 2.
TEST(EIA608FieldOrder, FindsACaptureThatStartsOnTheSecondField) {
  EIA608FieldOrder order;
  for (int i = 0; i < 10; ++i) {
    order.add(1, 0x14, 0x20);
  }
  EXPECT_TRUE(order.reversed());

  // XDS on the first stored field says the same.
  EIA608FieldOrder xds_first;
  for (int i = 0; i < 10; ++i) {
    xds_first.add(0, 0x01, 0x03);
  }
  EXPECT_TRUE(xds_first.reversed());
}

// A handful of signs, or signs both ways, change nothing.
TEST(EIA608FieldOrder, NeedsClearSigns) {
  EIA608FieldOrder few;
  for (int i = 0; i < 4; ++i) {
    few.add(1, 0x14, 0x2C);
  }
  EXPECT_FALSE(few.reversed());

  EIA608FieldOrder mixed;
  for (int i = 0; i < 20; ++i) {
    mixed.add(1, 0x14, 0x2C);
    mixed.add(0, 0x14, 0x2C);
  }
  EXPECT_FALSE(mixed.reversed());

  // Characters and other codes are no sign of either.
  EIA608FieldOrder none;
  for (int i = 0; i < 20; ++i) {
    none.add(1, 'H', 'I');
    none.add(1, 0x14, 0x50);
  }
  EXPECT_FALSE(none.reversed());
}

////////////////////////////////////////////////////////////////////////////////
// The observations
////////////////////////////////////////////////////////////////////////////////

TEST(EIA608FieldReading, ReadsTheClosedCaptionObservations) {
  orc::ObservationContext context;
  const orc::FieldID field(4);
  context.set(field, "closed_caption", "present", true);
  context.set(field, "closed_caption", "data0", int32_t{0x14});
  context.set(field, "closed_caption", "data1", int32_t{0x2C});
  context.set(field, "closed_caption", "parity0_valid", true);
  context.set(field, "closed_caption", "parity1_valid", false);

  const EIA608FieldReading read = orc::eia608_field_reading(context, field);
  EXPECT_TRUE(read.present);
  EXPECT_EQ(read.data0, 0x14);
  EXPECT_EQ(read.data1, 0x2C);
  EXPECT_TRUE(read.parity0_valid);
  EXPECT_FALSE(read.parity1_valid);

  // A field the observer read nothing on, or never read, carries nothing.
  context.set(orc::FieldID(5), "closed_caption", "present", false);
  EXPECT_FALSE(orc::eia608_field_reading(context, orc::FieldID(5)).present);
  EXPECT_FALSE(orc::eia608_field_reading(context, orc::FieldID(6)).present);
}

// Where line 21 carried no pair, the observer says which line next to it did
// and what it read there.
TEST(EIA608FieldReading, ReadsTheLineNextToLine21) {
  orc::ObservationContext context;
  const orc::FieldID field(4);
  context.set(field, "closed_caption", "present", false);
  context.set(field, "closed_caption", "other_line", int32_t{22});
  context.set(field, "closed_caption", "other_data0", int32_t{0x14});
  context.set(field, "closed_caption", "other_data1", int32_t{0x2C});
  context.set(field, "closed_caption", "other_parity0_valid", true);
  context.set(field, "closed_caption", "other_parity1_valid", true);

  EXPECT_FALSE(orc::eia608_field_reading(context, field).present);
  const EIA608FieldReading other =
      orc::eia608_other_line_reading(context, field);
  EXPECT_TRUE(other.present);
  EXPECT_EQ(other.line, 22);
  EXPECT_EQ(other.data0, 0x14);
  EXPECT_EQ(other.data1, 0x2C);
  EXPECT_TRUE(other.parity1_valid);
  EXPECT_FALSE(
      orc::eia608_other_line_reading(context, orc::FieldID(5)).present);
}

////////////////////////////////////////////////////////////////////////////////
// The run
////////////////////////////////////////////////////////////////////////////////

// A captioned field carries a pair on every field; the run counts the lines
// read and those that carried the waveform, and hands back the pairs in the
// order they were transmitted.
TEST(EIA608Line21Run, HandsBackThePairsInTransmissionOrder) {
  EIA608Line21Run run;
  for (uint32_t frame = 0; frame < 4; ++frame) {
    run.add(frame, 0, reading(static_cast<uint8_t>('A' + frame), 'b'));
    run.add(frame, 1, EIA608FieldReading{});
  }
  EXPECT_EQ(run.lines(0), 4u);
  EXPECT_EQ(run.carried(0), 4u);
  EXPECT_EQ(run.lines(1), 4u);
  EXPECT_EQ(run.carried(1), 0u);

  const auto pairs = pairs_of(run);
  ASSERT_EQ(pairs.size(), 4u);
  for (uint32_t frame = 0; frame < 4; ++frame) {
    EXPECT_EQ(pairs[frame].frame_offset, frame);
    EXPECT_EQ(pairs[frame].stored_field, 0);
    EXPECT_EQ(pairs[frame].television_field, 0);
    EXPECT_EQ(pairs[frame].data0, 'A' + frame);
    EXPECT_TRUE(pairs[frame].taken());
  }
}

// A line of noise that reads as a pair stands alone: no line 21 data within
// thirty frames either side of it.
TEST(EIA608Line21Run, RefusesAPairStandingAlone) {
  EIA608Line21Run run;
  run.add(0, 0, reading('X', 'Y'));
  for (uint32_t frame = 100; frame < 103; ++frame) {
    run.add(frame, 0, reading('O', 'K'));
  }
  const auto pairs = pairs_of(run);
  ASSERT_EQ(pairs.size(), 4u);
  EXPECT_TRUE(pairs[0].defined);
  EXPECT_TRUE(pairs[0].isolated);
  EXPECT_FALSE(pairs[0].taken());
  EXPECT_TRUE(pairs[1].taken());
  EXPECT_TRUE(pairs[3].taken());
}

// A pair no table defines for its field is handed back refused.
TEST(EIA608Line21Run, RefusesAPairNoTableDefines) {
  EIA608Line21Run run;
  for (uint32_t frame = 0; frame < 3; ++frame) {
    run.add(frame, 0, reading('O', 'K'));
  }
  run.add(3, 0, reading(0x14, 0x30));  // no miscellaneous code 30h
  const auto pairs = pairs_of(run);
  ASSERT_EQ(pairs.size(), 4u);
  EXPECT_FALSE(pairs[3].defined);
  EXPECT_FALSE(pairs[3].taken());
}

// The pairs come back as a receiver on their field acts on them: XDS's first
// byte is ignored on field 1, a character's 01h partner everywhere.
TEST(EIA608Line21Run, HandsBackThePairsAsTheirFieldTakesThem) {
  EIA608Line21Run run;
  for (uint32_t frame = 0; frame < 3; ++frame) {
    run.add(frame, 0, reading('O', 'K'));
  }
  run.add(3, 0, reading(0x05, 'A'));
  run.add(4, 0, reading('C', 0x01));
  const auto pairs = pairs_of(run);
  ASSERT_EQ(pairs.size(), 5u);
  EXPECT_EQ(pairs[3].data0, 0x00);
  EXPECT_EQ(pairs[3].data1, 'A');
  EXPECT_TRUE(pairs[3].taken());
  EXPECT_EQ(pairs[4].data0, 'C');
  EXPECT_EQ(pairs[4].data1, 0x00);
  EXPECT_TRUE(pairs[4].taken());
}

// Pairs whose parity a receiver would not take are not handed back; a
// character whose partner failed is, alone, but is no evidence of line 21
// data around a pair.
TEST(EIA608Line21Run, TakesPairsByTheirParity) {
  EIA608Line21Run run;
  run.add(0, 0, reading('H', 'i', false, true));
  run.add(1, 0, reading('H', 'i', true, false));
  run.add(2, 0, reading('O', 'K'));
  run.add(3, 0, reading('O', 'K'));
  const auto pairs = pairs_of(run);
  ASSERT_EQ(pairs.size(), 3u);
  EXPECT_TRUE(pairs[0].first_only);
  EXPECT_EQ(pairs[0].data1, 0x00);
  // Two pairs with both parities good: not yet a run of line 21 data.
  EXPECT_TRUE(pairs[0].isolated);
  EXPECT_TRUE(pairs[2].isolated);
  EXPECT_EQ(run.carried(0), 4u);
}

// A capture whose first stored field is television field 2: its captions are
// on the second stored field, spelled as field 1 spells them, and come back on
// television field 1.
TEST(EIA608Line21Run, ReadsAReversedCaptureTheOtherWayRound) {
  EIA608Line21Run run;
  for (uint32_t frame = 0; frame < 12; ++frame) {
    run.add(frame, 0, reading(0x00, 0x00));
    run.add(frame, 1, reading(0x14, 0x2C));
  }
  EXPECT_TRUE(run.fields_reversed());
  const auto pairs = pairs_of(run);
  ASSERT_EQ(pairs.size(), 24u);
  EXPECT_EQ(pairs[0].stored_field, 0);
  EXPECT_EQ(pairs[0].television_field, 1);
  EXPECT_EQ(pairs[1].stored_field, 1);
  EXPECT_EQ(pairs[1].television_field, 0);
  EXPECT_TRUE(pairs[1].taken());
}

/// The line next to line 21, |line|, read with a pair.
EIA608FieldReading neighbour(int line, uint8_t data0, uint8_t data1,
                             bool parity0 = true, bool parity1 = true) {
  EIA608FieldReading field = reading(data0, data1, parity0, parity1);
  field.line = line;
  return field;
}

// A recording whose time base slipped a line carries its captions on line 22:
// where line 21 carries nothing, the run takes line 22's pairs once that line
// reads as a caption line — a caption stream, control codes among its
// characters, both parities good on nearly every field.
TEST(EIA608Line21Run, TakesCaptionsFromTheLineNextToLine21) {
  EIA608Line21Run run;
  for (uint32_t frame = 0; frame < 40; ++frame) {
    const bool code = frame % 4 == 0;
    run.add(frame, 0, EIA608FieldReading{},
            neighbour(22, code ? 0x14 : 'O', code ? 0x2C : 'K'));
    run.add(frame, 1, EIA608FieldReading{});
  }
  EXPECT_EQ(run.taken_from(0, 22), 40u);
  EXPECT_EQ(run.taken_from(0, 20), 0u);
  const auto pairs = pairs_of(run);
  ASSERT_EQ(pairs.size(), 40u);
  EXPECT_EQ(pairs[1].data0, 'O');
  EXPECT_TRUE(pairs[1].taken());
  EXPECT_EQ(run.carried(0), 0u) << "line 21 carried nothing";
}

// A line next to line 21 is read where the caption line has moved there, not
// where line 21 lost a field amid its own pairs: only fifteen frames or more
// from any line 21 waveform.
TEST(EIA608Line21Run, ReadsTheLineNextToLine21OnlyWhereLine21IsQuiet) {
  EIA608Line21Run run;
  for (uint32_t frame = 0; frame < 120; ++frame) {
    const bool code = frame % 4 == 0;
    const EIA608FieldReading other =
        neighbour(22, code ? 0x14 : 'N', code ? 0x2C : 'B');
    if (frame < 50 && frame != 25) {
      run.add(frame, 0, reading('O', 'K'));
    } else {
      run.add(frame, 0, EIA608FieldReading{}, other);
    }
  }
  // Frame 25 lies amid line 21's pairs, and frames 50-64 within fifteen of
  // its last: frames 65-119 are line 22's.
  EXPECT_EQ(run.taken_from(0, 22), 55u);
  const auto pairs = pairs_of(run);
  ASSERT_EQ(pairs.size(), 49u + 55u);
  EXPECT_EQ(pairs[48].frame_offset, 49u);
  EXPECT_EQ(pairs[49].frame_offset, 65u);
}

// A line next to line 21 that does not read as a caption line is left out: a
// test signal slicing into one fixed pair carries no control code, and a line
// of noise passes parity at chance.
TEST(EIA608Line21Run, LeavesOutANeighbourThatIsNoCaptionLine) {
  EIA608Line21Run fixed;
  EIA608Line21Run noise;
  for (uint32_t frame = 0; frame < 40; ++frame) {
    fixed.add(frame, 0, EIA608FieldReading{}, neighbour(20, 'O', 'K'));
    noise.add(frame, 0, EIA608FieldReading{},
              neighbour(20, 0x14, 0x2C, frame % 2 == 0, frame % 3 == 0));
  }
  EXPECT_TRUE(pairs_of(fixed).empty());
  EXPECT_EQ(fixed.taken_from(0, 20), 0u);
  EXPECT_TRUE(pairs_of(noise).empty());
}

// Where line 21 carried its pair the line next to it is never taken, and a run
// with no neighbour carrying captions hands back exactly line 21's pairs.
TEST(EIA608Line21Run, Line21WinsWhereItCarriedAPair) {
  EIA608Line21Run run;
  for (uint32_t frame = 0; frame < 40; ++frame) {
    run.add(frame, 0, reading('O', 'K'), neighbour(22, 0x14, 0x2C));
  }
  EXPECT_EQ(run.taken_from(0, 22), 0u);
  const auto pairs = pairs_of(run);
  ASSERT_EQ(pairs.size(), 40u);
  for (const auto& pair : pairs) {
    EXPECT_EQ(pair.data0, 'O');
  }
}

}  // namespace
}  // namespace orc_unit_test
