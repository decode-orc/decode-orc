/*
 * File:        source_alignment_tool_test.cpp
 * Module:      orc-core-tests
 * Purpose:     The Source Align's alignment analysis as the stage offers it,
 *              and its runs on a fake job context over captures carrying
 *              picture numbers: each mode's map, failures, cancel
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../../orc/plugins/stages/source_align/source_alignment_tool.h"

#include <gtest/gtest.h>
#include <orc/plugin/orc_stage_tooling.h>
#include <orc/stage/tooling/stage_analysis_tool.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "../../../../../orc/plugins/stages/source_align/source_align_stage.h"
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

orc::ArtifactPtr capture(std::vector<int32_t> picture_numbers) {
  return std::make_shared<PalPictureNumberSource>(std::move(picture_numbers));
}

orc::StageAnalysisReport align(FakeToolJobContext& context,
                               const std::string& mode) {
  return orc::source_alignment_tool::run(
      {{"alignmentMode", orc::ParameterValue{mode}}}, context);
}

std::string change(const orc::StageAnalysisReport& report,
                   const std::string& name) {
  const auto it = report.parameter_changes.find(name);
  return it == report.parameter_changes.end()
             ? std::string("(none)")
             : std::get<std::string>(it->second);
}

}  // namespace

TEST(SourceAlignTools, TheStageOffersTheAlignmentAnalysis) {
  orc::SourceAlignStage stage;
  const auto tools = stage.get_stage_tools();
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools[0].tool_id, "source_alignment");
  EXPECT_EQ(tools[0].display_name, "Source Alignment Analysis");
  EXPECT_EQ(tools[0].contract_id, "decode-orc.stage-tools.source-alignment.v1");
  EXPECT_EQ(tools[0].kind, orc::StageToolKind::Analysis);

  const auto params = stage.analysis_tool_parameters("source_alignment");
  ASSERT_EQ(params.size(), 1u);
  EXPECT_EQ(params[0].name, "alignmentMode");
  EXPECT_EQ(std::get<std::string>(*params[0].constraints.default_value),
            "pad_for_alignment");
  EXPECT_TRUE(stage.analysis_tool_parameters("no_such_tool").empty());

  FakeToolJobContext context({});
  const auto report = stage.run_analysis_tool("no_such_tool", {}, context);
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary, "Source Align has no tool 'no_such_tool'");
}

TEST(SourceAlignmentTool, NeedsVideoInputs) {
  FakeToolJobContext none({});
  auto report = align(none, "pad_for_alignment");
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary.rfind(
                "Source Alignment Analysis requires at least one input", 0),
            0u)
      << report.summary;

  FakeToolJobContext not_video(
      {capture(pictures(1, 5)), std::make_shared<NotVideoArtifact>()});
  report = align(not_video, "pad_for_alignment");
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary,
            "Input node 2 did not produce VideoFrameRepresentation");
}

TEST(SourceAlignmentTool, TheSameCaptureTwiceIsAMistake) {
  const auto same = capture(pictures(1, 10));
  FakeToolJobContext context({same, same});
  const auto report = align(context, "pad_for_alignment");
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary, "ERROR: All 2 inputs are the SAME source object");
  ASSERT_EQ(report.items.size(), 1u);
  EXPECT_EQ(report.items[0].type, "error");
  EXPECT_TRUE(report.parameter_changes.empty());
}

TEST(SourceAlignmentTool, PadsEachCaptureToTheEarliestPicture) {
  FakeToolJobContext context(
      {capture(pictures(1, 30)), capture(pictures(6, 40))});
  const auto report = align(context, "pad_for_alignment");
  ASSERT_EQ(report.status, Status::Success) << report.summary;
  EXPECT_EQ(change(report, "alignmentMap"), "1+0, 2+5");
  EXPECT_EQ(change(report, "alignmentMode"), "pad_for_alignment");
  EXPECT_EQ(std::get<int64_t>(report.statistics.at("sourceCount")), 2);
  EXPECT_EQ(std::get<int64_t>(report.statistics.at("globalFirstVBIFrame")), 1);
  EXPECT_NE(report.summary.find("Alignment Map: 1+0, 2+5"), std::string::npos)
      << report.summary;
  EXPECT_DOUBLE_EQ(context.fractions.back(), 1.0);
  EXPECT_EQ(context.texts.back(), "Analysis complete");
}

TEST(SourceAlignmentTool, TrimsEachCaptureToTheFirstCommonPicture) {
  FakeToolJobContext context(
      {capture(pictures(1, 30)), capture(pictures(6, 40))});
  const auto report = align(context, "first_common_frame");
  ASSERT_EQ(report.status, Status::Success) << report.summary;
  EXPECT_EQ(change(report, "alignmentMap"), "1+5, 2+0");
  EXPECT_EQ(change(report, "alignmentMode"), "first_common_frame");
  EXPECT_EQ(std::get<int64_t>(report.statistics.at("firstCommonVBIFrame")), 6);
  EXPECT_EQ(std::get<int64_t>(report.statistics.at("participatingSourceCount")),
            2);
}

TEST(SourceAlignmentTool, HasNothingToApplyWithoutPictureNumbers) {
  for (const char* mode : {"pad_for_alignment", "first_common_frame"}) {
    FakeToolJobContext context({capture(std::vector<int32_t>(6, 0)),
                                capture(std::vector<int32_t>(6, 0))});
    const auto report = align(context, mode);
    EXPECT_EQ(report.status, Status::Failed) << mode;
    EXPECT_EQ(report.summary, "No VBI frames found in any sources") << mode;
    EXPECT_TRUE(report.parameter_changes.empty()) << mode;
  }
}

TEST(SourceAlignmentTool, ACancelledRunIsCancelled) {
  FakeToolJobContext context(
      {capture(pictures(1, 30)), capture(pictures(6, 40))});
  context.cancelled = true;
  const auto report = align(context, "pad_for_alignment");
  EXPECT_EQ(report.status, Status::Cancelled);
  EXPECT_TRUE(report.parameter_changes.empty());
}

}  // namespace orc_unit_test
