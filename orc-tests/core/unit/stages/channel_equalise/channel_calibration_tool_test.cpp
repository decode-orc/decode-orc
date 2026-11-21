/*
 * File:        channel_calibration_tool_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Channel Equalise's analysis tool: how the stage offers it,
 *              what a run on a fake job context reports, and what each
 *              verdict would apply
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../../orc/plugins/stages/channel_equalise/channel_calibration_tool.h"

#include <gtest/gtest.h>
#include <orc/stage/tooling/stage_analysis_tool.h>

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../../../../../orc/plugins/stages/channel_equalise/channel_equalise_stage.h"
#include "../../include/pal_colour_bars_source.h"

namespace orc_unit_test {
namespace {

using Status = orc::StageAnalysisReport::Status;

// The inputs and cancel flag a run is given; progress is counted.
class FakeJobContext final : public orc::IToolJobContext {
 public:
  explicit FakeJobContext(std::vector<orc::ArtifactPtr> inputs,
                          bool cancelled = false)
      : inputs_(std::move(inputs)), cancelled_(cancelled) {}
  const std::vector<orc::ArtifactPtr>& inputs() const override {
    return inputs_;
  }
  void report_progress(double fraction, const std::string&) override {
    ++reports;
    last_fraction = fraction;
  }
  bool is_cancelled() const override { return cancelled_; }

  int reports = 0;
  double last_fraction = -1.0;

 private:
  std::vector<orc::ArtifactPtr> inputs_;
  bool cancelled_;
};

// An artifact that is not video.
class NotVideo : public orc::Artifact {
 public:
  NotVideo() : orc::Artifact(orc::ArtifactID("not_video"), orc::Provenance{}) {}
  std::string type_name() const override { return "not_video"; }
};

std::shared_ptr<PalColourBarsSource> bars() {
  return std::make_shared<PalColourBarsSource>(PalColourBarsSource::Options{});
}

orc::StageAnalysisReport run(FakeJobContext& context) {
  orc::ChannelEqualiseStage stage;
  return stage.run_analysis_tool(orc::channel_calibration_tool::kToolId, {},
                                 context);
}

orc::ChannelWindow window(orc::SmearVerdict verdict, double fraction,
                          double tau) {
  orc::ChannelWindow w;
  w.first = 100;
  w.frames = 48;
  w.frames_used = 40;
  w.fit = orc::SmearFit{fraction, tau, 0.5, 1.0};
  w.verdict = verdict;
  return w;
}

TEST(ChannelCalibrationToolTest, TheStageOffersItAsAnAnalysisTool) {
  orc::ChannelEqualiseStage stage;
  const auto tools = stage.get_stage_tools();
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools[0].tool_id, "channel_calibration");
  EXPECT_EQ(tools[0].display_name, "Channel Calibration");
  EXPECT_EQ(tools[0].kind, orc::StageToolKind::Analysis);
  EXPECT_EQ(tools[0].contract_id,
            "decode-orc.stage-tools.channel-calibration.v1");
  EXPECT_TRUE(stage.analysis_tool_parameters("channel_calibration").empty());
}

TEST(ChannelCalibrationToolTest, WithoutAnInputItSaysToConnectOne) {
  FakeJobContext context({});
  const auto report = run(context);
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary,
            "Connect a source to the Channel Equalise node first.");
  EXPECT_TRUE(report.parameter_changes.empty());
}

TEST(ChannelCalibrationToolTest, AnInputThatIsNotVideoFails) {
  FakeJobContext context({std::make_shared<NotVideo>()});
  const auto report = run(context);
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary, "The input produces no video.");
}

TEST(ChannelCalibrationToolTest, AnUnknownToolFails) {
  orc::ChannelEqualiseStage stage;
  FakeJobContext context({bars()});
  const auto report = stage.run_analysis_tool("no_such_tool", {}, context);
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(context.reports, 0);
}

TEST(ChannelCalibrationToolTest, ACancelledRunReportsCancelled) {
  FakeJobContext context({bars()}, /*cancelled=*/true);
  const auto report = run(context);
  EXPECT_EQ(report.status, Status::Cancelled);
  EXPECT_TRUE(report.parameter_changes.empty());
}

// One frame is a single window over the whole source.
TEST(ChannelCalibrationToolTest, ARunReportsItsWindows) {
  FakeJobContext context({bars()});
  const auto report = run(context);
  ASSERT_EQ(report.status, Status::Success) << report.summary;
  ASSERT_EQ(report.items.size(), 1u);
  EXPECT_EQ(report.items[0].type, "window");
  EXPECT_EQ(report.items[0].start_frame, 0);
  EXPECT_EQ(report.items[0].end_frame, 0);
  EXPECT_EQ(std::get<int64_t>(report.statistics.at("windows")), 1);
  EXPECT_DOUBLE_EQ(context.last_fraction, 1.0);
}

TEST(ChannelCalibrationToolTest, ASmearIsWhatApplyWrites) {
  const auto report =
      orc::channel_calibration_tool::describe(orc::combine_channel_windows(
          {window(orc::SmearVerdict::Smear, 0.243216, 6.12345)}));
  ASSERT_EQ(report.status, Status::Success);
  ASSERT_EQ(report.parameter_changes.size(), 2u);
  // Written to four and three decimals; the statistics keep the rest.
  EXPECT_DOUBLE_EQ(
      std::get<double>(report.parameter_changes.at("smear_fraction")), 0.2432);
  EXPECT_DOUBLE_EQ(
      std::get<double>(report.parameter_changes.at("smear_tau_samples")),
      6.123);
  EXPECT_DOUBLE_EQ(std::get<double>(report.statistics.at("smearFraction")),
                   0.243216);
  EXPECT_EQ(std::get<int64_t>(report.statistics.at("framesUsed")), 40);
  ASSERT_EQ(report.items.size(), 1u);
  EXPECT_EQ(report.items[0].start_frame, 100);
  EXPECT_EQ(report.items[0].end_frame, 147);
  EXPECT_EQ(report.items[0].message, "smear: fraction 0.243, tau 6.12 samples");
  EXPECT_NE(report.summary.find("Apply to Stage sets smear_fraction and "
                                "smear_tau_samples"),
            std::string::npos);
}

TEST(ChannelCalibrationToolTest, NoSmearSetsTheFractionToZero) {
  const auto report =
      orc::channel_calibration_tool::describe(orc::combine_channel_windows(
          {window(orc::SmearVerdict::NoSmear, 0.0, 1.0)}));
  ASSERT_EQ(report.status, Status::Success);
  ASSERT_EQ(report.parameter_changes.size(), 1u);
  EXPECT_EQ(std::get<double>(report.parameter_changes.at("smear_fraction")),
            0.0);
}

TEST(ChannelCalibrationToolTest, CannotTellLeavesNothingToApply) {
  const auto report =
      orc::channel_calibration_tool::describe(orc::ChannelCalibration{});
  EXPECT_EQ(report.status, Status::Success);
  EXPECT_TRUE(report.parameter_changes.empty());
  EXPECT_NE(report.summary.find("No smear could be measured"),
            std::string::npos);
}

}  // namespace
}  // namespace orc_unit_test
