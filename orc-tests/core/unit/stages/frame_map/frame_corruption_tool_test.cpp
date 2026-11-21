/*
 * File:        frame_corruption_tool_test.cpp
 * Module:      orc-core-tests
 * Purpose:     The Frame Map's Frame Corruption Generator on a fake job
 *              context: the seed it uses, the frame map it proposes, failures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../../orc/plugins/stages/frame_map/frame_corruption_tool.h"

#include <gtest/gtest.h>
#include <orc/stage/tooling/stage_analysis_tool.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "../../include/fake_tool_job_context.h"
#include "../../include/pal_picture_number_source.h"

namespace orc_unit_test {
namespace {

using Status = orc::StageAnalysisReport::Status;
using Params = std::map<std::string, orc::ParameterValue>;

FakeToolJobContext with_frames(size_t frames) {
  return FakeToolJobContext({std::make_shared<PalPictureNumberSource>(
      std::vector<int32_t>(frames, 0))});
}

// Counts the new seeds asked for, and gives |seed| each time.
struct SeedSource {
  uint32_t seed = 1234;
  int asked = 0;
  uint32_t operator()() {
    ++asked;
    return seed;
  }
};

orc::StageAnalysisReport generate(FakeToolJobContext& context,
                                  const Params& params, int32_t stored,
                                  SeedSource& seeds) {
  return orc::frame_corruption_tool::run(params, context, stored,
                                         [&seeds] { return seeds(); });
}

}  // namespace

TEST(FrameCorruptionTool, NeedsAVideoInput) {
  FakeToolJobContext context({std::make_shared<NotVideoArtifact>()});
  SeedSource seeds;
  const auto report = generate(context, {}, 0, seeds);
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary.rfind("Cannot determine input frame count.", 0), 0u);
  EXPECT_TRUE(report.parameter_changes.empty());
}

TEST(FrameCorruptionTool, ANodeWithoutASeedGetsANewOne) {
  auto context = with_frames(200);
  SeedSource seeds;
  const auto report = generate(context, {}, 0, seeds);
  ASSERT_EQ(report.status, Status::Success) << report.summary;
  EXPECT_EQ(seeds.asked, 1);
  EXPECT_NE(report.summary.find("Seed: 1234 (new)"), std::string::npos)
      << report.summary;
  EXPECT_NE(report.summary.find("Pattern: simple-skip"), std::string::npos);
  EXPECT_EQ(std::get<int32_t>(report.parameter_changes.at("seed")), 1234);
  EXPECT_FALSE(
      std::get<std::string>(report.parameter_changes.at("ranges")).empty());
  EXPECT_EQ(std::get<std::string>(report.statistics.at("patternName")),
            "simple-skip");
  EXPECT_EQ(std::get<int64_t>(report.statistics.at("seed")), 1234);
  ASSERT_FALSE(report.items.empty());
  EXPECT_EQ(report.items.front().type, "info");
  EXPECT_DOUBLE_EQ(context.fractions.back(), 1.0);
}

TEST(FrameCorruptionTool, TheStoredSeedMakesTheSamePatternAgain) {
  auto first_context = with_frames(200);
  SeedSource seeds;
  const auto first = generate(first_context, {}, 0, seeds);

  auto again_context = with_frames(200);
  SeedSource unused{999};
  const auto again = generate(again_context, {}, 1234, unused);
  EXPECT_EQ(unused.asked, 0);
  EXPECT_NE(again.summary.find("Seed: 1234 (reused)"), std::string::npos);
  EXPECT_EQ(again.parameter_changes.at("ranges"),
            first.parameter_changes.at("ranges"));
}

TEST(FrameCorruptionTool, RegeneratingTakesANewSeed) {
  auto context = with_frames(200);
  SeedSource seeds{55};
  const auto report = generate(
      context, {{"regenerate_seed", orc::ParameterValue{true}}}, 1234, seeds);
  EXPECT_EQ(seeds.asked, 1);
  EXPECT_NE(report.summary.find("Seed: 55 (regenerated)"), std::string::npos);
  EXPECT_EQ(std::get<int32_t>(report.parameter_changes.at("seed")), 55);
}

TEST(FrameCorruptionTool, AnUnknownPatternFails) {
  auto context = with_frames(50);
  SeedSource seeds;
  const auto report = generate(
      context, {{"pattern", orc::ParameterValue{std::string("wobbly")}}}, 0,
      seeds);
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary, "Unknown corruption pattern: wobbly");
}

TEST(FrameCorruptionTool, EveryOfferedPatternGenerates) {
  const auto params = orc::frame_corruption_tool::parameters();
  ASSERT_FALSE(params.empty());
  for (const auto& pattern : params[0].constraints.allowed_strings) {
    auto context = with_frames(500);
    SeedSource seeds;
    const auto report = generate(
        context, {{"pattern", orc::ParameterValue{pattern}}}, 0, seeds);
    EXPECT_EQ(report.status, Status::Success) << pattern;
    EXPECT_EQ(report.parameter_changes.count("ranges"), 1u) << pattern;
  }
}

}  // namespace orc_unit_test
