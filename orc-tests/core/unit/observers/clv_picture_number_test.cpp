/*
 * File:        clv_picture_number_test.cpp
 * Module:      observers
 * Purpose:     Unit tests for shared IEC 60856/60857 CLV picture number
 *              decoding
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <clv_picture_number.h>
#include <gtest/gtest.h>

#include <cstdint>

namespace {

using orc::decode_clv_picture_number;
using orc::decode_clv_seconds_picture;
using orc::decode_clv_time_code;
using orc::decode_clv_time_code_line;
using orc::max_legal_clv_picture_number;

constexpr int32_t kPalFps = 25;
constexpr int32_t kNtscFps = 30;

// A VBI line that failed to decode is published as -1 by the biphase
// observer.
constexpr int32_t kUnreadableLine = -1;

// IEC 60856/60857-1986 - 10.1.6 Programme time code: "FX1DDX2X3", X1 the
// hours and X2X3 the minutes in BCD.
int32_t clv_time_code_word(int32_t hours, int32_t minutes) {
  return 0xF00000 | (hours << 16) | 0x00DD00 | ((minutes / 10) << 4) |
         (minutes % 10);
}

// IEC 60856/60857-1986 - 10.1.10 CLV picture number: "8X1EX3X4X5", X1 running
// A through F for the tens of seconds, X3 the units, X4X5 the picture within
// the second.
int32_t clv_picture_word(int32_t seconds, int32_t picture) {
  return 0x800000 | ((0xA + (seconds / 10)) << 16) | 0x00E000 |
         ((seconds % 10) << 8) | ((picture / 10) << 4) | (picture % 10);
}

int32_t picture_number_of(int32_t h, int32_t m, int32_t s, int32_t f,
                          int32_t fps) {
  return (((h * 3600) + (m * 60) + s) * fps) + f;
}

// ---------------------------------------------------------------------------
// Programme time code (10.1.6)
// ---------------------------------------------------------------------------

TEST(ClvPictureNumber, DecodesLineCarryingAProgrammeTimeCode) {
  const auto tc = decode_clv_time_code_line(clv_time_code_word(0, 34));
  ASSERT_TRUE(tc.has_value());
  EXPECT_EQ(tc->hours, 0);
  EXPECT_EQ(tc->minutes, 34);
}

TEST(ClvPictureNumber, RejectsLineWithoutTheProgrammeTimeCodeMarker) {
  // IEC 60857-1986 - 10.1.2 Lead-out code, not a programme time code.
  EXPECT_FALSE(decode_clv_time_code_line(0x80EEEE).has_value());
}

// A line that failed to decode arrives as -1 (all bits set). The marker test
// alone accepts it, so the BCD digit check is what has to reject it.
TEST(ClvPictureNumber, RejectsUnreadableLine) {
  EXPECT_FALSE(decode_clv_time_code_line(kUnreadableLine).has_value());
}

TEST(ClvPictureNumber, RejectsMinutesOutsideTheLegalRange) {
  // 0x6D minutes decodes as BCD 6-and-13, which is not a decimal digit pair.
  EXPECT_FALSE(decode_clv_time_code_line(0xF0DD6D).has_value());
  // 72 minutes is legal BCD but not a legal time.
  EXPECT_FALSE(
      decode_clv_time_code_line(clv_time_code_word(0, 72)).has_value());
}

TEST(ClvPictureNumber, CrossValidatesAgreeingLines) {
  const int32_t word = clv_time_code_word(0, 34);
  bool cross_validated = false;
  const auto tc = decode_clv_time_code(word, word, cross_validated);
  ASSERT_TRUE(tc.has_value());
  EXPECT_EQ(tc->minutes, 34);
  EXPECT_TRUE(cross_validated);
}

// The failure taken from a real Domesday capture: line 17's hours digit sits
// outside the programme time code's signature mask, so a flipped bit there
// still decodes cleanly and moves the picture by an hour of running time.
// Line 18 carries the same code and disagrees, which is what catches it.
TEST(ClvPictureNumber, RejectsLinesThatDisagreeOnTheHours) {
  bool cross_validated = false;
  const auto tc = decode_clv_time_code(
      clv_time_code_word(7, 12), clv_time_code_word(0, 12), cross_validated);
  EXPECT_FALSE(tc.has_value());
  EXPECT_FALSE(cross_validated);
}

TEST(ClvPictureNumber, AcceptsASingleReadableLineWithoutCrossValidation) {
  bool cross_validated = true;
  auto tc = decode_clv_time_code(clv_time_code_word(0, 34), kUnreadableLine,
                                 cross_validated);
  ASSERT_TRUE(tc.has_value());
  EXPECT_EQ(tc->minutes, 34);
  EXPECT_FALSE(cross_validated);

  cross_validated = true;
  tc = decode_clv_time_code(kUnreadableLine, clv_time_code_word(0, 34),
                            cross_validated);
  ASSERT_TRUE(tc.has_value());
  EXPECT_EQ(tc->minutes, 34);
  EXPECT_FALSE(cross_validated);
}

// ---------------------------------------------------------------------------
// Seconds and picture within the second (10.1.10)
// ---------------------------------------------------------------------------

TEST(ClvPictureNumber, DecodesSecondsAndPictureFromLine16) {
  int32_t seconds = -1;
  int32_t picture = -1;
  ASSERT_TRUE(decode_clv_seconds_picture(clv_picture_word(43, 15), kPalFps,
                                         seconds, picture));
  EXPECT_EQ(seconds, 43);
  EXPECT_EQ(picture, 15);
}

// 10.1.10 writes the picture within the second as X4 = 0..2, X5 = 0..9 in the
// PAL standard as well as the NTSC one, but a 25 Hz disc never reaches
// picture 25, so anything at or above the frame rate is a misread.
TEST(ClvPictureNumber, RejectsAPictureAtOrAboveTheFrameRate) {
  int32_t seconds = -1;
  int32_t picture = -1;
  EXPECT_FALSE(decode_clv_seconds_picture(clv_picture_word(43, 27), kPalFps,
                                          seconds, picture));
  // The same word is a legal reading on a 30 Hz disc.
  EXPECT_TRUE(decode_clv_seconds_picture(clv_picture_word(43, 27), kNtscFps,
                                         seconds, picture));
  EXPECT_EQ(picture, 27);
}

TEST(ClvPictureNumber, RejectsSecondsOutsideTheLegalRange) {
  int32_t seconds = -1;
  int32_t picture = -1;
  // Tens-of-seconds nibble below the A..F range the standard specifies.
  EXPECT_FALSE(decode_clv_seconds_picture(0x89E015, kPalFps, seconds, picture));
}

// ---------------------------------------------------------------------------
// The assembled picture number
// ---------------------------------------------------------------------------

TEST(ClvPictureNumber, AssemblesARunningTimeIntoAPictureNumber) {
  const auto pn = decode_clv_picture_number(clv_picture_word(43, 15),
                                            clv_time_code_word(0, 46),
                                            clv_time_code_word(0, 46), kPalFps);
  ASSERT_TRUE(pn.has_value());
  EXPECT_EQ(pn->value, picture_number_of(0, 46, 43, 15, kPalFps));
  EXPECT_TRUE(pn->cross_validated);
}

TEST(ClvPictureNumber, RejectsThePictureNumberWhenTheTimeLinesDisagree) {
  EXPECT_FALSE(decode_clv_picture_number(clv_picture_word(43, 15),
                                         clv_time_code_word(4, 46),
                                         clv_time_code_word(0, 46), kPalFps)
                   .has_value());
}

TEST(ClvPictureNumber, FlagsASingleLineReadingAsNotCrossValidated) {
  const auto pn = decode_clv_picture_number(clv_picture_word(43, 15),
                                            clv_time_code_word(4, 46),
                                            kUnreadableLine, kPalFps);
  ASSERT_TRUE(pn.has_value());
  EXPECT_EQ(pn->value, picture_number_of(4, 46, 43, 15, kPalFps));
  EXPECT_FALSE(pn->cross_validated);
}

TEST(ClvPictureNumber, RejectsAPictureNumberOfZero) {
  // The first picture of the programme is read from the lead-in code, so a
  // running time of zero is reported as "no picture number".
  EXPECT_FALSE(decode_clv_picture_number(clv_picture_word(0, 0),
                                         clv_time_code_word(0, 0),
                                         clv_time_code_word(0, 0), kPalFps)
                   .has_value());
}

// ---------------------------------------------------------------------------
// The standard's ceiling
// ---------------------------------------------------------------------------

// 10.1.6 gives the hours a single 4-bit group, so the largest running time the
// code can express is 9:59:59 plus the last picture of that second.
TEST(ClvPictureNumber, LegalMaximumMatchesTheStandardsLargestTimeCode) {
  EXPECT_EQ(max_legal_clv_picture_number(kPalFps),
            picture_number_of(9, 59, 59, kPalFps - 1, kPalFps));
  EXPECT_EQ(max_legal_clv_picture_number(kNtscFps),
            picture_number_of(9, 59, 59, kNtscFps - 1, kNtscFps));
  EXPECT_EQ(max_legal_clv_picture_number(kPalFps), 899999);
  EXPECT_EQ(max_legal_clv_picture_number(kNtscFps), 1079999);
}

// Every value the decoder can return is inside the standard's ceiling,
// because each field is range checked as it is decoded.
TEST(ClvPictureNumber, NeverReturnsAValueAboveTheLegalMaximum) {
  const auto pn = decode_clv_picture_number(clv_picture_word(59, kPalFps - 1),
                                            clv_time_code_word(9, 59),
                                            clv_time_code_word(9, 59), kPalFps);
  ASSERT_TRUE(pn.has_value());
  EXPECT_EQ(pn->value, max_legal_clv_picture_number(kPalFps));

  // An hours digit of 0xA is not legal BCD, so the line is rejected outright
  // rather than producing a time beyond the ceiling.
  EXPECT_FALSE(decode_clv_time_code_line(0xFADD59).has_value());
}

}  // namespace
