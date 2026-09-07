/*
 * File:        cav_picture_number_test.cpp
 * Module:      observers
 * Purpose:     Unit tests for shared IEC 60857 CAV picture number decoding
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <cav_picture_number.h>
#include <gtest/gtest.h>

#include <cstdint>

namespace {

using orc::decode_cav_picture_number;
using orc::decode_cav_picture_number_line;
using orc::decode_vbi_bcd;

// IEC 60857-1986 - 10.1.3 Picture numbers (CAV discs): the 0xF00000 marker
// followed by the picture number in BCD.
int32_t cav_picture_word(int32_t picture_number) {
  int32_t bcd = 0;
  int32_t shift = 0;
  for (int32_t remaining = picture_number; remaining > 0; remaining /= 10) {
    bcd |= (remaining % 10) << shift;
    shift += 4;
  }
  return 0xF00000 | bcd;
}

TEST(CavPictureNumber, DecodesLineCarryingAPictureNumber) {
  const auto pn = decode_cav_picture_number_line(cav_picture_word(48912));
  ASSERT_TRUE(pn.has_value());
  EXPECT_EQ(*pn, 48912);
}

TEST(CavPictureNumber, RejectsLineWithoutThePictureNumberMarker) {
  // IEC 60857-1986 - 10.1.2 Lead-out code, not a picture number.
  EXPECT_FALSE(decode_cav_picture_number_line(0x80EEEE).has_value());
}

// A line that failed to decode arrives as -1 (all bits set). The marker test
// alone accepts it, so the BCD digit check is what has to reject it.
TEST(CavPictureNumber, RejectsUnreadableLine) {
  EXPECT_FALSE(decode_cav_picture_number_line(-1).has_value());
}

TEST(CavPictureNumber, RejectsLineWithANonDecimalDigit) {
  // 0x00F4B066: the hundreds digit came through as 0xB.
  EXPECT_FALSE(decode_cav_picture_number_line(0x00F4B066).has_value());
}

TEST(CavPictureNumber, MarksValueCrossValidated_WhenBothLinesAgree) {
  const int32_t word = cav_picture_word(1234);
  const auto pn = decode_cav_picture_number(word, word);
  ASSERT_TRUE(pn.has_value());
  EXPECT_EQ(pn->value, 1234);
  EXPECT_TRUE(pn->cross_validated);
}

// Both lines carry the number, so a disagreement means one of them is
// corrupt and neither can be trusted on its own.
TEST(CavPictureNumber, ReturnsNothing_WhenBothLinesDecodeButDisagree) {
  EXPECT_FALSE(
      decode_cav_picture_number(cav_picture_word(1234), cav_picture_word(1235))
          .has_value());
}

TEST(CavPictureNumber, MarksValueNotCrossValidated_WhenOnlyLine17Decodes) {
  const auto pn = decode_cav_picture_number(cav_picture_word(1234), -1);
  ASSERT_TRUE(pn.has_value());
  EXPECT_EQ(pn->value, 1234);
  EXPECT_FALSE(pn->cross_validated);
}

TEST(CavPictureNumber, MarksValueNotCrossValidated_WhenOnlyLine18Decodes) {
  const auto pn = decode_cav_picture_number(-1, cav_picture_word(1234));
  ASSERT_TRUE(pn.has_value());
  EXPECT_EQ(pn->value, 1234);
  EXPECT_FALSE(pn->cross_validated);
}

TEST(CavPictureNumber, ReturnsNothing_WhenNeitherLineCarriesAPictureNumber) {
  EXPECT_FALSE(decode_cav_picture_number(0, 0).has_value());
}

// The top bit of the marker nibble can carry the stop code, so the picture
// number field is masked to 0x07FFFF: the standard's range is 0-79999.
TEST(CavPictureNumber, DecodesTheTopOfTheStandardsRange) {
  const auto pn = decode_cav_picture_number_line(cav_picture_word(79999));
  ASSERT_TRUE(pn.has_value());
  EXPECT_EQ(*pn, 79999);
}

TEST(VbiBcd, DecodesEachNibbleAsADecimalDigit) {
  int32_t value = -1;
  ASSERT_TRUE(decode_vbi_bcd(0x48912, value));
  EXPECT_EQ(value, 48912);
}

TEST(VbiBcd, RejectsANibbleAboveNine) {
  int32_t value = -1;
  EXPECT_FALSE(decode_vbi_bcd(0x4A912, value));
}

TEST(VbiBcd, DecodesZero) {
  int32_t value = -1;
  ASSERT_TRUE(decode_vbi_bcd(0, value));
  EXPECT_EQ(value, 0);
}

}  // namespace
