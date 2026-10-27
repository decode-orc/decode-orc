/*
 * File:        parameter_value_rules_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests for the cross-parameter value rules
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "parameter_value_rules.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace gui_unit_test {
namespace {

using orc::gui::crossParameterErrors;
using orc::gui::decoderScheduleError;

const std::vector<std::string> kPalDecoders = {"pal2d", "transform2d",
                                               "transform3d", "secam", "mono"};

std::map<std::string, orc::ParameterValue> videoParamsValues(
    int32_t active_video_start, int32_t active_video_end,
    int32_t first_active_frame_line, int32_t last_active_frame_line,
    int32_t black_level, int32_t white_level) {
  return {{"activeVideoStart", active_video_start},
          {"activeVideoEnd", active_video_end},
          {"firstActiveFrameLine", first_active_frame_line},
          {"lastActiveFrameLine", last_active_frame_line},
          {"blackLevel", black_level},
          {"whiteLevel", white_level}};
}

}  // namespace

TEST(ParameterValueRulesTest, ReportsNothing_WhenThePalStandardValuesAreUsed) {
  // EBU Tech. 3280-E §1.2 / §1.1.1 Table 1: the standard PAL active window
  // and levels must not be reported as inconsistent.
  EXPECT_TRUE(
      crossParameterErrors(videoParamsValues(157, 1105, 44, 620, 256, 844))
          .empty());
}

TEST(ParameterValueRulesTest, ReportsNothing_WhenEveryValueIsInherited) {
  EXPECT_TRUE(
      crossParameterErrors(videoParamsValues(-1, -1, -1, -1, -1, -1)).empty());
}

TEST(ParameterValueRulesTest,
     ReportsNothing_WhenOnlyOneEndOfAPairIsGivenAndTheOtherIsInherited) {
  // The inherited end is the source's, which this cannot see: a value that
  // looks inverted against the sentinel is not an error.
  EXPECT_TRUE(crossParameterErrors(videoParamsValues(900, -1, -1, 620, -1, 844))
                  .empty());
}

TEST(ParameterValueRulesTest,
     ReportsTheActiveWindow_WhenItsStartIsNotBeforeItsEnd) {
  const auto after_the_end =
      crossParameterErrors(videoParamsValues(1105, 157, 44, 620, 256, 844));
  ASSERT_EQ(after_the_end.size(), 1u);
  EXPECT_EQ(after_the_end.front(),
            "Active Video Start must be less than Active Video End.");

  // An empty window describes no picture either.
  const auto equal =
      crossParameterErrors(videoParamsValues(157, 157, 44, 620, 256, 844));
  ASSERT_EQ(equal.size(), 1u);
  EXPECT_EQ(equal.front(),
            "Active Video Start must be less than Active Video End.");
}

TEST(ParameterValueRulesTest,
     ReportsTheActiveLines_WhenTheFirstIsNotBeforeTheLast) {
  const auto errors =
      crossParameterErrors(videoParamsValues(157, 1105, 620, 44, 256, 844));
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors.front(),
            "First Active Frame Line must be less than Last Active Frame "
            "Line.");
}

TEST(ParameterValueRulesTest, ReportsTheLevels_WhenBlackIsNotBelowWhite) {
  const auto errors =
      crossParameterErrors(videoParamsValues(157, 1105, 44, 620, 844, 256));
  ASSERT_EQ(errors.size(), 1u);
  EXPECT_EQ(errors.front(), "Black Level must be less than White Level.");
}

TEST(ParameterValueRulesTest, ReportsEveryBrokenPair_WhenSeveralAreWrong) {
  EXPECT_EQ(
      crossParameterErrors(videoParamsValues(1105, 157, 620, 44, 844, 256))
          .size(),
      3u);
}

TEST(ParameterValueRulesTest, ReportsNothing_WhenTheStageHasNoSuchParameters) {
  // Every other stage's parameters pass through untouched: a rule whose
  // parameters are absent is not applied.
  const std::map<std::string, orc::ParameterValue> other_stage = {
      {"input_path", std::string("/captures/disc.tbc")},
      {"threshold", static_cast<int32_t>(12)},
      {"enabled", true}};
  EXPECT_TRUE(crossParameterErrors(other_stage).empty());
}

TEST(ParameterValueRulesTest,
     ReportsNothing_WhenAParameterIsNotAWholeNumberAtAll) {
  // A value of the wrong type is the type check's business, not this one's;
  // it must not be read as a number that happens to compare badly.
  const std::map<std::string, orc::ParameterValue> wrong_types = {
      {"activeVideoStart", std::string("157")}, {"activeVideoEnd", 1105.0}};
  EXPECT_TRUE(crossParameterErrors(wrong_types).empty());
}

TEST(ParameterValueRulesTest, ReadsAPairCarriedAsUnsignedValues) {
  const std::map<std::string, orc::ParameterValue> unsigned_values = {
      {"activeVideoStart", static_cast<uint32_t>(1105)},
      {"activeVideoEnd", static_cast<uint32_t>(157)}};
  ASSERT_EQ(crossParameterErrors(unsigned_values).size(), 1u);
}

TEST(ParameterValueRulesTest, DecoderSchedule_AcceptsRangesAndADefault) {
  EXPECT_EQ(decoderScheduleError(
                "transform3d, 1001-1500:transform2d, 1600:mono", kPalDecoders),
            "");
  EXPECT_EQ(decoderScheduleError("1-10:pal2d", kPalDecoders), "");
}

TEST(ParameterValueRulesTest, DecoderSchedule_ReportsFrameZeroAsInvalid) {
  // Frame numbers in the dialog are 1-based, so frame 0 does not exist.
  EXPECT_NE(decoderScheduleError("0-10:pal2d", kPalDecoders), "");
}

TEST(ParameterValueRulesTest, DecoderSchedule_ReportsAnEmptySchedule) {
  EXPECT_NE(decoderScheduleError("", kPalDecoders), "");
  EXPECT_NE(decoderScheduleError(" , ", kPalDecoders), "");
}

TEST(ParameterValueRulesTest, DecoderSchedule_ReportsADecoderTheProjectLacks) {
  const std::string error = decoderScheduleError("1-5:ntsc3d", kPalDecoders);
  EXPECT_NE(error.find("'ntsc3d'"), std::string::npos) << error;
  EXPECT_NE(decoderScheduleError("multiple", kPalDecoders), "");
}

TEST(ParameterValueRulesTest, DecoderSchedule_ReportsTwoDefaults) {
  EXPECT_NE(decoderScheduleError("pal2d, transform3d", kPalDecoders), "");
}

TEST(ParameterValueRulesTest, DecoderSchedule_ReportsReversedRange) {
  const std::string error = decoderScheduleError("10-5:pal2d", kPalDecoders);
  EXPECT_NE(error.find("10-5"), std::string::npos) << error;
}

TEST(ParameterValueRulesTest, DecoderSchedule_ReportsOverlapIn1BasedFrames) {
  const std::string error =
      decoderScheduleError("1-10:pal2d, 10-20:mono", kPalDecoders);
  EXPECT_NE(error.find("frame 10"), std::string::npos) << error;
}

TEST(ParameterValueRulesTest, DecoderSchedule_AcceptsAHeldDecoder) {
  EXPECT_EQ(decoderScheduleError("transform2d, held:transform3d", kPalDecoders),
            "");
  EXPECT_EQ(
      decoderScheduleError(
          "transform2d, held:transform3d, 1001-1500:transform3d", kPalDecoders),
      "");
}

TEST(ParameterValueRulesTest, DecoderSchedule_ReportsAHeldDecoderAlone) {
  // The frames that are not held need a default.
  EXPECT_NE(decoderScheduleError("held:transform3d", kPalDecoders), "");
  EXPECT_NE(decoderScheduleError("1-10:pal2d, held:transform3d", kPalDecoders),
            "");
}

TEST(ParameterValueRulesTest, DecoderSchedule_ReportsTwoHeldDecoders) {
  EXPECT_NE(decoderScheduleError("pal2d, held:transform3d, held:transform2d",
                                 kPalDecoders),
            "");
}

TEST(ParameterValueRulesTest,
     DecoderSchedule_ReportsAHeldDecoderTheProjectLacks) {
  const std::string error =
      decoderScheduleError("pal2d, held:ntsc3d", kPalDecoders);
  EXPECT_NE(error.find("'ntsc3d'"), std::string::npos) << error;
}

}  // namespace gui_unit_test
