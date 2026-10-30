/*
 * File:        baselines_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the videosynth suite's baselines: parsing
 *              baselines.json and judging a value against a baseline
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <baselines.h>
#include <gtest/gtest.h>

#include <stdexcept>

namespace orc_unit_test {
namespace {

using orc_videosynth::Baseline;
using orc_videosynth::BaselineFile;
using orc_videosynth::Better;
using orc_videosynth::judge;
using orc_videosynth::Verdict;

TEST(VideosynthBaselines, lowerIsBetterJudgesBothWays) {
  const Baseline b{2.0, 0.1, Better::Lower};
  EXPECT_EQ(judge(b, 2.0), Verdict::Pass);
  EXPECT_EQ(judge(b, 2.09), Verdict::Pass);
  EXPECT_EQ(judge(b, 1.91), Verdict::Pass);
  EXPECT_EQ(judge(b, 2.11), Verdict::Regressed);
  EXPECT_EQ(judge(b, 1.89), Verdict::Improved);
}

TEST(VideosynthBaselines, higherIsBetterJudgesBothWays) {
  const Baseline b{20.0, 0.5, Better::Higher};
  EXPECT_EQ(judge(b, 19.6), Verdict::Pass);
  EXPECT_EQ(judge(b, 19.4), Verdict::Regressed);
  EXPECT_EQ(judge(b, 20.6), Verdict::Improved);
}

TEST(VideosynthBaselines, zeroToleranceIsBitExact) {
  const Baseline b{4.0, 0.0, Better::Higher};
  EXPECT_EQ(judge(b, 4.0), Verdict::Pass);
  EXPECT_EQ(judge(b, 3.999), Verdict::Regressed);
}

TEST(VideosynthBaselines, noBaselineIsMissing) {
  EXPECT_EQ(judge(std::nullopt, 1.0), Verdict::Missing);
}

TEST(VideosynthBaselines, parsesTheJsonShape) {
  const auto file = BaselineFile::parse(R"({
    "_comment": ["ignored"],
    "tests": {
      "Suite.Test": {
        "error": {"value": 1.5, "tolerance": 0.05, "better": "lower"},
        "gain": {"value": 12, "tolerance": 0.2, "better": "higher"}
      }
    }
  })",
                                        "baselines.json");
  const auto error = file.find("Suite.Test", "error");
  ASSERT_TRUE(error.has_value());
  EXPECT_DOUBLE_EQ(error->value, 1.5);
  EXPECT_DOUBLE_EQ(error->tolerance, 0.05);
  EXPECT_EQ(error->better, Better::Lower);
  const auto gain = file.find("Suite.Test", "gain");
  ASSERT_TRUE(gain.has_value());
  EXPECT_EQ(gain->better, Better::Higher);
  EXPECT_FALSE(file.find("Suite.Test", "other").has_value());
  EXPECT_FALSE(file.find("Suite.Other", "error").has_value());
}

TEST(VideosynthBaselines, emptyTestsObjectIsValid) {
  const auto file = BaselineFile::parse(R"({"tests": {}})", "baselines.json");
  EXPECT_FALSE(file.find("Suite.Test", "error").has_value());
}

TEST(VideosynthBaselines, rejectsMalformedFiles) {
  EXPECT_THROW(BaselineFile::parse(R"({"other": {}})", "b"),
               std::runtime_error);
  EXPECT_THROW(
      BaselineFile::parse(
          R"({"tests": {"S.T": {"m": {"value": 1, "tolerance": 0}}}})", "b"),
      std::runtime_error);
  EXPECT_THROW(
      BaselineFile::parse(
          R"({"tests": {"S.T": {"m": {"value": 1, "tolerance": 0, "better": "up"}}}})",
          "b"),
      std::runtime_error);
  EXPECT_THROW(BaselineFile::parse("{ not json", "b"), std::runtime_error);
}

}  // namespace
}  // namespace orc_unit_test
