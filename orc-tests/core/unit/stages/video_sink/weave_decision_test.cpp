/*
 * File:        weave_decision_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the video sink's deinterlace_mode: the comb
 *              score, the per-frame weave decision, the deinterlacing filter
 *              and the parameter
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/plugins/stages/sinks/common/weave_decision.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "../../../../orc/plugins/stages/sinks/common/video_sink_stage.h"

namespace orc_unit_test {
namespace {

using orc::chroma_sink::comb_score;
using orc::chroma_sink::CombScore;
using orc::chroma_sink::deinterlace_filter;
using orc::chroma_sink::DeinterlaceMode;
using orc::chroma_sink::kWeaveCombThreshold;
using orc::chroma_sink::kWeaveLocalCombThreshold;
using orc::chroma_sink::parse_deinterlace_mode;
using orc::chroma_sink::weave_frame;

constexpr int32_t kWidth = 360;
constexpr int32_t kHeight = 288;

// A woven frame of diagonal bars: frame line y is the picture at height y.
// The second field's lines (the odd ones) show the bars moved
// field_motion samples along the line, as interlaced motion leaves them;
// noise is Gaussian of sigma noise_sigma.
std::vector<double> bars_frame(double field_motion, double noise_sigma,
                               uint32_t seed = 1) {
  std::mt19937 random(seed);
  std::normal_distribution<double> noise(0.0, noise_sigma);
  std::vector<double> frame(static_cast<size_t>(kWidth) * kHeight);
  for (int32_t y = 0; y < kHeight; ++y) {
    const double shift = (y % 2 == 1) ? field_motion : 0.0;
    for (int32_t x = 0; x < kWidth; ++x) {
      const double phase = (x - shift - (0.5 * y)) / 24.0;
      const double bars = std::tanh(4.0 * std::sin(phase));
      frame[(static_cast<size_t>(y) * kWidth) + x] =
          1000.0 + (300.0 * bars) + noise(random);
    }
  }
  return frame;
}

CombScore score_of(const std::vector<double>& frame) {
  return comb_score(frame.data(), kWidth, 0, kWidth, 0, kHeight);
}

bool one_instant(const CombScore& score) { return weave_frame(false, score); }

TEST(WeaveDecisionTest, ParsesTheTwoModes) {
  EXPECT_EQ(parse_deinterlace_mode("all"), DeinterlaceMode::All);
  EXPECT_EQ(parse_deinterlace_mode("motion_only"), DeinterlaceMode::MotionOnly);
  EXPECT_FALSE(parse_deinterlace_mode("weave").has_value());
  EXPECT_FALSE(parse_deinterlace_mode("").has_value());
}

TEST(WeaveDecisionTest, AllKeepsTheBareBwdif) {
  for (const auto system : {orc::VideoSystem::PAL, orc::VideoSystem::NTSC,
                            orc::VideoSystem::PAL_M}) {
    EXPECT_EQ(deinterlace_filter(DeinterlaceMode::All, system), "bwdif");
  }
}

TEST(WeaveDecisionTest, MotionOnlyDeinterlacesFlaggedFramesAtTheFieldRate) {
  EXPECT_EQ(
      deinterlace_filter(DeinterlaceMode::MotionOnly, orc::VideoSystem::PAL),
      "bwdif=deint=interlaced,fps=50");
  EXPECT_EQ(
      deinterlace_filter(DeinterlaceMode::MotionOnly, orc::VideoSystem::NTSC),
      "bwdif=deint=interlaced,fps=60000/1001");
  EXPECT_EQ(
      deinterlace_filter(DeinterlaceMode::MotionOnly, orc::VideoSystem::PAL_M),
      "bwdif=deint=interlaced,fps=60000/1001");
}

TEST(WeaveDecisionTest, WeavesHeldFramesWhateverTheirScore) {
  EXPECT_TRUE(weave_frame(true, {5.0, 9.0}));
  EXPECT_TRUE(weave_frame(
      false, {kWeaveCombThreshold - 0.01, kWeaveLocalCombThreshold - 0.01}));
  EXPECT_FALSE(weave_frame(false, {kWeaveCombThreshold, 0.5}));
  EXPECT_FALSE(weave_frame(false, {0.3, kWeaveLocalCombThreshold}));
}

TEST(WeaveDecisionTest, OneInstantScoresBelowTheThresholds) {
  const CombScore score = score_of(bars_frame(0.0, 3.0));
  EXPECT_LT(score.whole, kWeaveCombThreshold) << score.whole;
  EXPECT_LT(score.local, kWeaveLocalCombThreshold) << score.local;
  EXPECT_GT(score.local, 0.0);
}

TEST(WeaveDecisionTest, InterlacedMotionScoresAboveTheThreshold) {
  for (const double motion : {2.0, 4.0, 8.0}) {
    const CombScore score = score_of(bars_frame(motion, 3.0));
    EXPECT_GT(score.whole, kWeaveCombThreshold) << motion;
    EXPECT_FALSE(one_instant(score)) << motion;
  }
}

TEST(WeaveDecisionTest, MotionInPartOfThePictureIsFoundByTheLocalScore) {
  // A still with something moving between the fields in one corner: the
  // whole-picture score stays low, the local one does not.
  std::vector<double> frame = bars_frame(0.0, 3.0);
  const std::vector<double> moving = bars_frame(1.0, 3.0, 2);
  for (int32_t y = 0; y < 40; ++y) {
    for (int32_t x = 0; x < 70; ++x) {
      frame[(static_cast<size_t>(y) * kWidth) + x] =
          moving[(static_cast<size_t>(y) * kWidth) + x];
    }
  }
  const CombScore score = score_of(frame);
  EXPECT_LT(score.whole, kWeaveCombThreshold)
      << score.whole << " " << score.local;
  EXPECT_GE(score.local, kWeaveLocalCombThreshold) << score.local;
  EXPECT_FALSE(one_instant(score));
}

TEST(WeaveDecisionTest, OneDefectiveLineIsNotMotion) {
  // A streak on one line (an unflagged dropout) within one block: the local
  // score is the second-worst block's.
  std::vector<double> frame = bars_frame(0.0, 3.0);
  for (int32_t x = 70; x < 120; ++x) {
    frame[(static_cast<size_t>(150) * kWidth) + x] += 60.0;
  }
  const CombScore score = score_of(frame);
  EXPECT_TRUE(one_instant(score)) << score.whole << " " << score.local;
}

TEST(WeaveDecisionTest, FlatNoiseReadsAsOneInstant) {
  // No vertical detail: both predictions are noise against noise, and the
  // noise floor keeps the score from reading that as motion.
  std::mt19937 random(7);
  std::normal_distribution<double> noise(0.0, 5.0);
  std::vector<double> frame(static_cast<size_t>(kWidth) * kHeight);
  for (double& sample : frame) sample = 1000.0 + noise(random);
  EXPECT_TRUE(one_instant(score_of(frame)));
}

TEST(WeaveDecisionTest, ChromaResidueIsNotMotion) {
  // Luma residue of a saturated colour at the subcarrier (four samples a
  // cycle at 4fsc), its phase opposite in the two fields.
  std::vector<double> frame = bars_frame(0.0, 3.0);
  constexpr double kPi = 3.14159265358979323846;
  for (int32_t y = 0; y < kHeight; ++y) {
    const double field_phase = (y % 2 == 1) ? kPi : 0.0;
    for (int32_t x = 0; x < kWidth; ++x) {
      frame[(static_cast<size_t>(y) * kWidth) + x] +=
          60.0 * std::sin((kPi / 2.0 * x) + field_phase);
    }
  }
  EXPECT_TRUE(one_instant(score_of(frame)));
}

TEST(WeaveDecisionTest, ScoreDoesNotDependOnTheSignalScale) {
  const std::vector<double> frame = bars_frame(3.0, 3.0);
  std::vector<double> scaled = frame;
  for (double& sample : scaled) sample *= 64.0;
  EXPECT_NEAR(score_of(frame).whole, score_of(scaled).whole, 1e-4);
  EXPECT_NEAR(score_of(frame).local, score_of(scaled).local, 1e-4);
}

TEST(WeaveDecisionTest, ScoresOnlyTheActiveArea) {
  // Motion in the blanking either side of the active area does not count.
  std::vector<double> frame = bars_frame(0.0, 3.0);
  const std::vector<double> moving = bars_frame(8.0, 3.0, 2);
  for (int32_t y = 0; y < kHeight; ++y) {
    for (int32_t x = 0; x < 40; ++x) {
      frame[(static_cast<size_t>(y) * kWidth) + x] =
          moving[(static_cast<size_t>(y) * kWidth) + x];
    }
  }
  EXPECT_GT(score_of(frame).whole, score_of(bars_frame(0.0, 3.0)).whole);
  EXPECT_TRUE(
      one_instant(comb_score(frame.data(), kWidth, 40, kWidth, 0, kHeight)));
}

TEST(WeaveDecisionTest, TooSmallAnAreaScoresZero) {
  const std::vector<double> frame = bars_frame(8.0, 3.0);
  EXPECT_EQ(comb_score(frame.data(), kWidth, 0, kWidth, 0, 4).whole, 0.0);
  EXPECT_EQ(comb_score(frame.data(), kWidth, 0, 2, 0, kHeight).whole, 0.0);
  // An area of one block has no second block.
  EXPECT_EQ(comb_score(frame.data(), kWidth, 0, 100, 0, 40).local, 0.0);
}

const orc::ParameterDescriptor* find_parameter(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::string& name) {
  for (const auto& descriptor : descriptors) {
    if (descriptor.name == name) return &descriptor;
  }
  return nullptr;
}

TEST(WeaveDecisionTest, ModeParameterFollowsApplyDeinterlace) {
  orc::VideoSinkStage stage;
  const auto descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::PAL, orc::SourceType::Composite);
  const auto* mode = find_parameter(descriptors, "deinterlace_mode");
  ASSERT_NE(mode, nullptr);
  EXPECT_EQ(mode->type, orc::ParameterType::STRING);
  ASSERT_TRUE(mode->constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*mode->constraints.default_value),
            "motion_only");
  EXPECT_EQ(mode->constraints.allowed_strings,
            (std::vector<std::string>{"all", "motion_only"}));
  ASSERT_TRUE(mode->constraints.depends_on.has_value());
  EXPECT_EQ(mode->constraints.depends_on->parameter_name, "apply_deinterlace");
  EXPECT_EQ(mode->constraints.depends_on->required_values,
            (std::vector<std::string>{"true"}));
}

TEST(WeaveDecisionTest, ModeParameterRoundTripsAndRejectsUnknownValues) {
  orc::VideoSinkStage stage;
  EXPECT_EQ(
      std::get<std::string>(stage.get_parameters().at("deinterlace_mode")),
      "motion_only");
  ASSERT_TRUE(stage.set_parameters({{"deinterlace_mode", std::string("all")}}));
  EXPECT_EQ(
      std::get<std::string>(stage.get_parameters().at("deinterlace_mode")),
      "all");
  EXPECT_FALSE(
      stage.set_parameters({{"deinterlace_mode", std::string("weave")}}));
  EXPECT_FALSE(stage.set_parameters({{"deinterlace_mode", true}}));
  EXPECT_EQ(
      std::get<std::string>(stage.get_parameters().at("deinterlace_mode")),
      "all");
}

}  // namespace
}  // namespace orc_unit_test
