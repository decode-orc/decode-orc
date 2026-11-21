/*
 * File:        fm_beat_calibration_tool_test.cpp
 * Module:      orc-core-tests
 * Purpose:     FM Beat Cancel's analysis tool: how the stage offers it, and
 *              what a run on a fake job context reports and would apply
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../../orc/plugins/stages/fm_beat_cancel/fm_beat_calibration_tool.h"

#include <gtest/gtest.h>
#include <orc/stage/tooling/stage_analysis_tool.h>

#include <cmath>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "../../../../../orc/plugins/stages/fm_beat_cancel/fm_beat_cancel_stage.h"
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

std::shared_ptr<PalColourBarsSource> bars(bool colour) {
  PalColourBarsSource::Options options;
  options.colour = colour;
  return std::make_shared<PalColourBarsSource>(options);
}

orc::StageAnalysisReport run(FakeJobContext& context) {
  orc::FmBeatCancelStage stage;
  return stage.run_analysis_tool(orc::fm_beat_calibration_tool::kToolId, {},
                                 context);
}

TEST(FmBeatCalibrationToolTest, TheStageOffersItAsAnAnalysisTool) {
  orc::FmBeatCancelStage stage;
  const auto tools = stage.get_stage_tools();
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools[0].tool_id, "fm_beat_calibration");
  EXPECT_EQ(tools[0].display_name, "FM Beat Calibration");
  EXPECT_EQ(tools[0].kind, orc::StageToolKind::Analysis);
  EXPECT_EQ(tools[0].contract_id,
            "decode-orc.stage-tools.fm-beat-calibration.v1");
  EXPECT_TRUE(stage.analysis_tool_parameters("fm_beat_calibration").empty());
}

TEST(FmBeatCalibrationToolTest, WithoutAnInputItSaysToConnectOne) {
  FakeJobContext context({});
  const auto report = run(context);
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary,
            "Connect a source to the FM Beat Cancel node first.");
  EXPECT_TRUE(report.parameter_changes.empty());
}

TEST(FmBeatCalibrationToolTest, AnInputThatIsNotVideoFails) {
  FakeJobContext context({std::make_shared<NotVideo>()});
  const auto report = run(context);
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(report.summary, "The input produces no video.");
}

TEST(FmBeatCalibrationToolTest, AnUnknownToolFails) {
  orc::FmBeatCancelStage stage;
  FakeJobContext context({bars(true)});
  const auto report = stage.run_analysis_tool("no_such_tool", {}, context);
  EXPECT_EQ(report.status, Status::Failed);
  EXPECT_EQ(context.reports, 0);
}

TEST(FmBeatCalibrationToolTest, WithoutFlatColourThereIsNothingToSet) {
  FakeJobContext context({bars(false)});
  const auto report = run(context);
  ASSERT_EQ(report.status, Status::Success) << report.summary;
  EXPECT_TRUE(report.parameter_changes.empty());
  EXPECT_EQ(std::get<int64_t>(report.statistics.at("probedFrames")), 1);
  EXPECT_EQ(std::get<int64_t>(report.statistics.at("fieldsSearched")), 0);
  EXPECT_NE(report.summary.find("nothing to do on this source"),
            std::string::npos);
  EXPECT_GT(context.reports, 0);
}

TEST(FmBeatCalibrationToolTest, ACancelledRunReportsCancelled) {
  FakeJobContext context({bars(true)}, /*cancelled=*/true);
  const auto report = run(context);
  EXPECT_EQ(report.status, Status::Cancelled);
  EXPECT_TRUE(report.parameter_changes.empty());
}

TEST(FmBeatCalibrationToolTest, TheCarrierFoundIsWhatApplyWrites) {
  PalColourBarsSource::Options options;
  options.beat_carrier_hz = 7.0915e6;
  options.noise_codes = 4;
  FakeJobContext context({std::make_shared<PalColourBarsSource>(options)});

  const auto report = run(context);

  ASSERT_EQ(report.status, Status::Success) << report.summary;
  ASSERT_EQ(report.parameter_changes.size(), 1u);
  const double carrier =
      std::get<double>(report.parameter_changes.at("carrier_black_mhz"));
  EXPECT_NEAR(carrier, 7.0915, 0.0005);
  // Written as the report rounds it, to four decimals.
  EXPECT_DOUBLE_EQ(carrier, std::round(carrier * 1.0e4) / 1.0e4);
  // The statistic keeps the digits the rounding drops.
  EXPECT_NEAR(std::get<double>(report.statistics.at("carrierBlackMHz")),
              carrier, 0.00005);
  ASSERT_FALSE(report.items.empty());
  EXPECT_EQ(report.items.front().type, "sweep");
  EXPECT_TRUE(report.items.front().data.count("carrierMHz"));
  EXPECT_NE(report.summary.find("Apply to Stage sets carrier_black_mhz"),
            std::string::npos);
  EXPECT_DOUBLE_EQ(context.last_fraction, 1.0);
}

}  // namespace
}  // namespace orc_unit_test
