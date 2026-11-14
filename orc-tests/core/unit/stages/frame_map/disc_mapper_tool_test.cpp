/*
 * File:        disc_mapper_tool_test.cpp
 * Module:      orc-core-tests
 * Purpose:     The Frame Map's analysis tools as the stage offers them, and
 *              the Disc Mapper's runs on a fake job context: the frames it
 *              maps, its readers on private copies of the inputs, cancel
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../../orc/plugins/stages/frame_map/disc_mapper_tool.h"

#include <gtest/gtest.h>
#include <orc/plugin/orc_stage_tooling.h>
#include <orc/stage/tooling/stage_analysis_tool.h>

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "../../../../../orc/plugins/stages/frame_map/frame_corruption_tool.h"
#include "../../../../../orc/plugins/stages/frame_map/frame_map_range_tool.h"
#include "../../../../../orc/plugins/stages/frame_map/frame_map_stage.h"
#include "../../include/fake_tool_job_context.h"
#include "../../include/pal_picture_number_source.h"

namespace orc_unit_test {
namespace {

using Status = orc::StageAnalysisReport::Status;

std::vector<int32_t> pictures(int32_t first, int32_t last) {
  std::vector<int32_t> out;
  for (int32_t pn = first; pn <= last; ++pn) out.push_back(pn);
  return out;
}

orc::ArtifactPtr disc(std::vector<int32_t> picture_numbers) {
  return std::make_shared<PalPictureNumberSource>(std::move(picture_numbers));
}

orc::StageAnalysisReport map(FakeToolJobContext& context,
                             const orc::ObservationPassOptions& options) {
  return orc::disc_mapper_tool::run(
      {{"include_lead_in_out", orc::ParameterValue{true}}}, context, options);
}

// A sweep on the calling thread alone, taking no shared readers.
orc::ObservationPassOptions one_reader() {
  orc::ObservationPassOptions options;
  options.max_workers = 1;
  options.readers = nullptr;
  return options;
}

std::string ranges_of(const orc::StageAnalysisReport& report) {
  const auto it = report.parameter_changes.find("ranges");
  return it == report.parameter_changes.end()
             ? std::string("(none)")
             : std::get<std::string>(it->second);
}

}  // namespace

TEST(FrameMapTools, TheStageOffersItsThreeAnalysisTools) {
  orc::FrameMapStage stage;
  const auto tools = stage.get_stage_tools();
  ASSERT_EQ(tools.size(), 3u);
  EXPECT_EQ(tools[0].tool_id, "field_mapping");
  EXPECT_EQ(tools[0].display_name, "Disc Mapper");
  EXPECT_EQ(tools[0].contract_id, "decode-orc.stage-tools.disc-mapper.v1");
  EXPECT_EQ(tools[1].tool_id, "frame_map_range");
  EXPECT_EQ(tools[1].contract_id, "decode-orc.stage-tools.frame-map-range.v1");
  EXPECT_EQ(tools[2].tool_id, "frame_corruption");
  EXPECT_EQ(tools[2].contract_id, "decode-orc.stage-tools.frame-corruption.v1");
  for (const auto& tool : tools) {
    EXPECT_EQ(tool.kind, orc::StageToolKind::Analysis) << tool.tool_id;
    EXPECT_FALSE(tool.description.empty()) << tool.tool_id;
  }

  const auto mapper = stage.analysis_tool_parameters("field_mapping");
  ASSERT_EQ(mapper.size(), 1u);
  EXPECT_EQ(mapper[0].name, "include_lead_in_out");
  EXPECT_EQ(std::get<bool>(*mapper[0].constraints.default_value), true);
  EXPECT_EQ(stage.analysis_tool_parameters("frame_map_range").size(), 2u);
  EXPECT_EQ(stage.analysis_tool_parameters("frame_corruption").size(), 2u);

  // A refresh maps a stack's discs together; the other tools run alone.
  EXPECT_TRUE(stage.analysis_tool_runs_alongside("field_mapping"));
  EXPECT_FALSE(stage.analysis_tool_runs_alongside("frame_map_range"));
  EXPECT_FALSE(stage.analysis_tool_runs_alongside("frame_corruption"));
  EXPECT_TRUE(stage.analysis_tool_parameters("no_such_tool").empty());
}

TEST(FrameMapTools, ARunOfAToolTheStageLacksFails) {
  orc::FrameMapStage stage;
  FakeToolJobContext context({});
  const auto report = stage.run_analysis_tool("no_such_tool", {}, context);
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary, "Frame Map has no tool 'no_such_tool'");
}

TEST(FrameMapTools, TheStageKeepsTheSeedACorruptionPatternWrote) {
  orc::FrameMapStage stage;
  EXPECT_TRUE(
      stage.set_parameters({{"seed", orc::ParameterValue{int32_t{7}}}}));
  FakeToolJobContext context({disc(pictures(1, 20))});
  const auto report =
      stage.run_analysis_tool(orc::frame_corruption_tool::kToolId, {}, context);
  EXPECT_EQ(std::get<int32_t>(report.parameter_changes.at("seed")), 7);
  EXPECT_FALSE(stage.set_parameters({{"seed", orc::ParameterValue{1.5}}}));
}

TEST(DiscMapperTool, NeedsAVideoInput) {
  FakeToolJobContext none({});
  auto report = map(none, one_reader());
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary, "Frame map node has no input connected");

  FakeToolJobContext not_video({std::make_shared<NotVideoArtifact>()});
  report = map(not_video, one_reader());
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary,
            "Input node did not produce VideoFrameRepresentation");
}

TEST(DiscMapperTool, MapsAnUnbrokenRunOfPictures) {
  FakeToolJobContext context({disc(pictures(1, 12))});
  const auto report = map(context, one_reader());
  ASSERT_EQ(report.status, Status::Success) << report.summary;
  EXPECT_EQ(ranges_of(report), "0-11");
  EXPECT_EQ(std::get<std::string>(report.statistics.at("discType")), "CAV");
  EXPECT_EQ(std::get<std::string>(report.statistics.at("videoFormat")), "PAL");
  EXPECT_EQ(std::get<int64_t>(report.statistics.at("finalFrames")), 12);
  EXPECT_EQ(std::get<int64_t>(report.statistics.at("firstPicture")), 1);
  EXPECT_EQ(std::get<int64_t>(report.statistics.at("lastPicture")), 12);
  EXPECT_NE(report.summary.find("Generated Frame Mapping:\n  1-12"),
            std::string::npos)
      << "the spec is shown 1-based:\n"
      << report.summary;
  ASSERT_FALSE(context.fractions.empty());
  EXPECT_DOUBLE_EQ(context.fractions.back(), 1.0);
  EXPECT_EQ(context.texts.back(), "Analysis complete");
}

TEST(DiscMapperTool, ReadsWithAReaderPerPrivateCopy) {
  if (std::thread::hardware_concurrency() < 2) {
    GTEST_SKIP() << "needs two hardware threads";
  }
  const auto numbers = pictures(1, 40);
  FakeToolJobContext sequential({disc(numbers)});
  const auto expected = map(sequential, one_reader());
  ASSERT_EQ(expected.status, Status::Success) << expected.summary;

  auto shared = std::make_shared<PalPictureNumberSource>(numbers);
  std::vector<std::shared_ptr<PalPictureNumberSource>> copies;
  FakeToolJobContext context({shared});
  context.make_private = [&copies, &numbers] {
    copies.push_back(std::make_shared<PalPictureNumberSource>(numbers));
    return std::vector<orc::ArtifactPtr>{copies.back()};
  };
  orc::ObservationPassOptions options;
  options.max_workers = 2;
  options.chunk_frames = 4;
  options.readers = nullptr;

  const auto report = map(context, options);
  ASSERT_EQ(report.status, Status::Success) << report.summary;
  EXPECT_EQ(context.private_copies, 2);
  ASSERT_EQ(copies.size(), 2u);
  EXPECT_GT(copies[0]->reads.load() + copies[1]->reads.load(), 0);
  EXPECT_EQ(ranges_of(report), ranges_of(expected));
  EXPECT_EQ(report.summary, expected.summary);
}

TEST(DiscMapperTool, ReadsAloneWhenTheHostCannotCopyTheInputs) {
  if (std::thread::hardware_concurrency() < 2) {
    GTEST_SKIP() << "needs two hardware threads";
  }
  FakeToolJobContext context({disc(pictures(1, 40))});
  orc::ObservationPassOptions options;
  options.max_workers = 2;
  options.chunk_frames = 4;
  options.readers = nullptr;
  const auto report = map(context, options);
  ASSERT_EQ(report.status, Status::Success) << report.summary;
  EXPECT_GE(context.private_copies, 1);
  EXPECT_EQ(ranges_of(report), "0-39");
}

TEST(DiscMapperTool, HasNothingToApplyWithoutPictureNumbers) {
  FakeToolJobContext context({disc(std::vector<int32_t>(8, 0))});
  const auto report = map(context, one_reader());
  EXPECT_TRUE(report.parameter_changes.empty()) << report.summary;
}

TEST(DiscMapperTool, ACancelledRunIsCancelled) {
  FakeToolJobContext context({disc(pictures(1, 300))});
  context.cancelled = true;
  const auto report = map(context, one_reader());
  EXPECT_EQ(report.status, Status::Cancelled);
  EXPECT_TRUE(report.parameter_changes.empty());
}

}  // namespace orc_unit_test
