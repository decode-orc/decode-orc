/*
 * File:        analysis_sink_tool_results_test.cpp
 * Module:      orc-core-tests
 * Purpose:     What the dropout, SNR and burst level analysis sinks answer
 *              their views and report to orc-cli, and the wire format the
 *              views read
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/tooling/stage_results_report.h>
#include <orc/stage/tooling/stage_tool_requests.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "../../../orc/plugins/stages/burst_level_analysis_sink/burst_level_analysis_sink_stage.h"
#include "../../../orc/plugins/stages/common/analysis-results/analysis_series_wire.h"
#include "../../../orc/plugins/stages/dropout_analysis_sink/dropout_analysis_sink_stage.h"
#include "../../../orc/plugins/stages/snr_analysis_sink/snr_analysis_sink_stage.h"
#include "../include/video_frame_representation_artifact_mock.h"

namespace orc_unit_test {
namespace {

using testing::_;
using testing::NiceMock;
using testing::Return;

namespace wire = orc::analysis_wire;

class FakeDropoutDeps : public orc::IDropoutAnalysisSinkStageDeps {
 public:
  explicit FakeDropoutDeps(orc::DropoutAnalysisComputeResult result)
      : result_(std::move(result)) {}
  void init(orc::TriggerProgressCallback, std::atomic<bool>*) override {}
  orc::DropoutAnalysisComputeResult compute_and_analyze(
      orc::VideoFrameRepresentation*, orc::IObservationContext&,
      orc::DropoutAnalysisComputeOptions) override {
    return result_;
  }
  bool write_csv(const std::string&,
                 const std::vector<orc::FrameDropoutStats>&) override {
    return true;
  }
  std::unique_ptr<orc::IDropoutReportWriter> open_report(
      const std::string&, orc::DropoutReportFormat) override {
    return nullptr;
  }

 private:
  orc::DropoutAnalysisComputeResult result_;
};

class FakeSnrDeps : public orc::ISNRAnalysisSinkStageDeps {
 public:
  explicit FakeSnrDeps(orc::SNRAnalysisComputeResult result)
      : result_(std::move(result)) {}
  void init(orc::TriggerProgressCallback, std::atomic<bool>*) override {}
  orc::SNRAnalysisComputeResult compute_and_analyze(
      orc::VideoFrameRepresentation*, orc::IObservationContext&,
      orc::SNRAnalysisComputeOptions) override {
    return result_;
  }
  bool write_csv(const std::string&,
                 const std::vector<orc::FrameSNRStats>&) override {
    return true;
  }

 private:
  orc::SNRAnalysisComputeResult result_;
};

class FakeBurstDeps : public orc::IBurstLevelAnalysisSinkStageDeps {
 public:
  explicit FakeBurstDeps(orc::BurstAnalysisComputeResult result)
      : result_(std::move(result)) {}
  void init(orc::TriggerProgressCallback, std::atomic<bool>*) override {}
  orc::BurstAnalysisComputeResult compute_and_analyze(
      orc::VideoFrameRepresentation*, orc::IObservationContext&,
      orc::BurstAnalysisComputeOptions) override {
    return result_;
  }
  bool write_csv(const std::string&,
                 const std::vector<orc::FrameBurstLevelStats>&) override {
    return true;
  }

 private:
  orc::BurstAnalysisComputeResult result_;
};

orc::FrameDropoutStats dropout(int32_t frame, int32_t count, int64_t length) {
  orc::FrameDropoutStats f;
  f.frame_number = frame;
  f.dropout_count = count;
  f.dropout_length_samples = length;
  f.has_data = count > 0;
  return f;
}

orc::FrameSNRStats snr(int32_t frame, bool has_white, double white,
                       bool has_black, double black) {
  orc::FrameSNRStats f;
  f.frame_number = frame;
  f.has_white_snr = has_white;
  f.white_snr = white;
  f.has_black_psnr = has_black;
  f.black_psnr = black;
  f.has_data = has_white || has_black;
  return f;
}

orc::FrameBurstLevelStats burst(int32_t frame, bool has_data, double level) {
  orc::FrameBurstLevelStats f;
  f.frame_number = frame;
  f.has_data = has_data;
  f.median_burst_10bit = level;
  return f;
}

template <typename Stage>
void trigger(Stage& stage,
             std::shared_ptr<MockVideoFrameRepresentationArtifact> input) {
  orc::ObservationContext context;
  ASSERT_TRUE(stage.trigger({input}, {}, context));
}

std::shared_ptr<NiceMock<MockVideoFrameRepresentationArtifact>> input() {
  return std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();
}

double real(const orc::StageReportValue& v) {
  EXPECT_EQ(v.type, orc::StageReportValue::Type::Real);
  return v.real;
}

int64_t integer(const orc::StageReportValue& v) {
  EXPECT_EQ(v.type, orc::StageReportValue::Type::Integer);
  return v.integer;
}

}  // namespace

// --- Before a trigger -------------------------------------------------------

TEST(AnalysisSinkToolResultsTest, UntriggeredStagesHaveNothingToShowOrReport) {
  orc::DropoutAnalysisSinkStage dropout_stage;
  orc::SNRAnalysisSinkStage snr_stage;
  orc::BurstLevelAnalysisSinkStage burst_stage;

  const auto a = dropout_stage.handle_tool_request(wire::kDropoutContractId,
                                                   wire::kResultsVerb, {});
  const auto b = snr_stage.handle_tool_request(wire::kSnrContractId,
                                               wire::kResultsVerb, {});
  const auto c = burst_stage.handle_tool_request(wire::kBurstLevelContractId,
                                                 wire::kResultsVerb, {});
  for (const auto& reply : {a, b, c}) {
    EXPECT_FALSE(reply.ok);
    EXPECT_EQ(reply.error, wire::kNotTriggeredMessage);
  }

  orc::StageResultsReport report;
  EXPECT_FALSE(dropout_stage.results_report(&report));
  EXPECT_FALSE(snr_stage.results_report(&report));
  EXPECT_FALSE(burst_stage.results_report(&report));
}

TEST(AnalysisSinkToolResultsTest, RefusesAnotherContractOrVerb) {
  orc::SNRAnalysisSinkStage stage;
  EXPECT_FALSE(
      stage
          .handle_tool_request(wire::kDropoutContractId, wire::kResultsVerb, {})
          .ok);
  const auto reply =
      stage.handle_tool_request(wire::kSnrContractId, "export", {});
  EXPECT_FALSE(reply.ok);
  EXPECT_NE(reply.error.find("export"), std::string::npos);
}

// The tool descriptors and the wire name the same contracts, or the host
// would open no view for them.
TEST(AnalysisSinkToolResultsTest, ToolsAdvertiseTheWireContracts) {
  EXPECT_EQ(orc::DropoutAnalysisSinkStage().get_stage_tools().at(0).contract_id,
            wire::kDropoutContractId);
  EXPECT_EQ(orc::SNRAnalysisSinkStage().get_stage_tools().at(0).contract_id,
            wire::kSnrContractId);
  EXPECT_EQ(
      orc::BurstLevelAnalysisSinkStage().get_stage_tools().at(0).contract_id,
      wire::kBurstLevelContractId);
}

// --- Dropout ----------------------------------------------------------------

TEST(AnalysisSinkToolResultsTest, DropoutResultsReachTheViewAndTheReport) {
  orc::DropoutAnalysisSinkStage stage;
  orc::DropoutAnalysisComputeResult computed;
  computed.success = true;
  computed.frame_stats = {dropout(1, 0, 0), dropout(2, 3, 120),
                          dropout(3, 1, 7)};
  computed.total_frames = 3;
  stage.set_deps_override(std::make_shared<FakeDropoutDeps>(computed));
  trigger(stage, input());

  const auto reply = stage.handle_tool_request(wire::kDropoutContractId,
                                               wire::kResultsVerb, {});
  ASSERT_TRUE(reply.ok) << reply.error;
  wire::DropoutResults results;
  std::string error;
  ASSERT_TRUE(wire::decode(reply.data, &results, &error)) << error;
  EXPECT_EQ(results.total_frames, 3);
  ASSERT_EQ(results.frames.size(), 3u);
  EXPECT_EQ(results.frames[1].dropout_count, 3);
  EXPECT_EQ(results.frames[1].dropout_length_samples, 120);
  EXPECT_FALSE(results.frames[0].has_data);

  orc::StageResultsReport report;
  ASSERT_TRUE(stage.results_report(&report));
  EXPECT_EQ(report.kind, "dropout");
  EXPECT_EQ(report.total_frames, 3);
  EXPECT_EQ(report.columns,
            (std::vector<std::string>{"frame", "dropout_count",
                                      "dropout_length_samples"}));
  ASSERT_EQ(report.rows.size(), 3u);
  // Every frame is a row, with a count even when it had no dropouts.
  EXPECT_EQ(integer(report.rows[0][1]), 0);
  EXPECT_EQ(integer(report.rows[1][2]), 120);
  EXPECT_EQ(integer(report.statistics.at("frames_analysed")), 3);
  EXPECT_EQ(integer(report.statistics.at("frames_with_dropouts")), 2);
  EXPECT_EQ(integer(report.statistics.at("total_dropouts")), 4);
  EXPECT_EQ(integer(report.statistics.at("total_dropout_samples")), 127);
  EXPECT_EQ(integer(report.statistics.at("total_frames")), 3);
}

// --- SNR --------------------------------------------------------------------

TEST(AnalysisSinkToolResultsTest, SnrReportLeavesMissingValuesAbsent) {
  orc::SNRAnalysisSinkStage stage;
  orc::SNRAnalysisComputeResult computed;
  computed.success = true;
  computed.frame_stats = {snr(1, true, 30.0, true, 40.0),
                          snr(2, false, 0.0, true, 42.0),
                          snr(3, true, 34.0, false, 0.0)};
  computed.total_frames = 5;
  stage.set_deps_override(std::make_shared<FakeSnrDeps>(computed));
  trigger(stage, input());

  const auto reply =
      stage.handle_tool_request(wire::kSnrContractId, wire::kResultsVerb, {});
  ASSERT_TRUE(reply.ok) << reply.error;
  wire::SnrResults results;
  std::string error;
  ASSERT_TRUE(wire::decode(reply.data, &results, &error)) << error;
  EXPECT_EQ(results.total_frames, 5);
  ASSERT_EQ(results.frames.size(), 3u);
  EXPECT_FALSE(results.frames[1].has_white_snr);
  EXPECT_DOUBLE_EQ(results.frames[2].white_snr, 34.0);

  orc::StageResultsReport report;
  ASSERT_TRUE(stage.results_report(&report));
  EXPECT_EQ(report.kind, "snr");
  ASSERT_EQ(report.rows.size(), 3u);
  EXPECT_EQ(report.rows[1][1].type, orc::StageReportValue::Type::Absent);
  EXPECT_EQ(report.rows[2][2].type, orc::StageReportValue::Type::Absent);
  EXPECT_DOUBLE_EQ(real(report.statistics.at("mean_white_snr")), 32.0);
  EXPECT_DOUBLE_EQ(real(report.statistics.at("min_white_snr")), 30.0);
  EXPECT_DOUBLE_EQ(real(report.statistics.at("max_black_psnr")), 42.0);
  EXPECT_EQ(integer(report.statistics.at("frames_analysed")), 3);
}

TEST(AnalysisSinkToolResultsTest, SnrWithNoValuesReportsNoMeans) {
  orc::SNRAnalysisSinkStage stage;
  orc::SNRAnalysisComputeResult computed;
  computed.success = true;
  computed.frame_stats = {snr(1, false, 0.0, false, 0.0)};
  computed.total_frames = 1;
  stage.set_deps_override(std::make_shared<FakeSnrDeps>(computed));
  trigger(stage, input());

  orc::StageResultsReport report;
  ASSERT_TRUE(stage.results_report(&report));
  EXPECT_EQ(report.statistics.count("mean_white_snr"), 0u);
  EXPECT_EQ(report.statistics.count("mean_black_psnr"), 0u);
}

// --- Burst level ------------------------------------------------------------

TEST(AnalysisSinkToolResultsTest, BurstResultsCarryTheSourceLevels) {
  orc::BurstLevelAnalysisSinkStage stage;
  orc::BurstAnalysisComputeResult computed;
  computed.success = true;
  computed.frame_stats = {burst(1, true, 118.0), burst(2, false, 0.0),
                          burst(3, true, 122.0)};
  computed.total_frames = 3;
  stage.set_deps_override(std::make_shared<FakeBurstDeps>(computed));

  auto source = input();
  orc::SourceParameters params;
  params.system = orc::VideoSystem::NTSC;
  params.blanking_level = 240;
  params.white_level = 800;
  ON_CALL(*source, get_video_parameters()).WillByDefault(Return(params));
  trigger(stage, source);

  const auto reply = stage.handle_tool_request(wire::kBurstLevelContractId,
                                               wire::kResultsVerb, {});
  ASSERT_TRUE(reply.ok) << reply.error;
  wire::BurstLevelResults results;
  std::string error;
  ASSERT_TRUE(wire::decode(reply.data, &results, &error)) << error;
  EXPECT_EQ(results.levels.system, wire::System::NTSC);
  EXPECT_EQ(results.levels.blanking_level, 240);
  EXPECT_EQ(results.levels.white_level, 800);
  ASSERT_EQ(results.frames.size(), 3u);
  EXPECT_DOUBLE_EQ(results.frames[2].median_burst_10bit, 122.0);

  orc::StageResultsReport report;
  ASSERT_TRUE(stage.results_report(&report));
  EXPECT_EQ(report.kind, "burst_level");
  EXPECT_EQ(report.rows[1][1].type, orc::StageReportValue::Type::Absent);
  EXPECT_DOUBLE_EQ(real(report.statistics.at("mean_median_burst_10bit")),
                   120.0);
}

TEST(AnalysisSinkToolResultsTest, BurstLevelsUnknownWithoutVideoParameters) {
  orc::BurstLevelAnalysisSinkStage stage;
  orc::BurstAnalysisComputeResult computed;
  computed.success = true;
  computed.frame_stats = {burst(1, true, 118.0)};
  computed.total_frames = 1;
  stage.set_deps_override(std::make_shared<FakeBurstDeps>(computed));
  trigger(stage, input());

  const auto reply = stage.handle_tool_request(wire::kBurstLevelContractId,
                                               wire::kResultsVerb, {});
  ASSERT_TRUE(reply.ok);
  wire::BurstLevelResults results;
  std::string error;
  ASSERT_TRUE(wire::decode(reply.data, &results, &error));
  EXPECT_EQ(results.levels.blanking_level, -1);
  EXPECT_EQ(results.levels.white_level, -1);
}

// --- The wire ---------------------------------------------------------------

TEST(AnalysisSeriesWireTest, RefusesBytesThatAreNotItsResults) {
  wire::DropoutResults dropout_results;
  std::string error;
  EXPECT_FALSE(wire::decode({}, &dropout_results, &error));
  EXPECT_FALSE(error.empty());

  // The SNR results are not dropout results.
  wire::SnrFrame snr_frame;
  snr_frame.frame_number = 1;
  snr_frame.has_white_snr = true;
  snr_frame.white_snr = 1.0;
  snr_frame.has_data = true;
  const auto snr_bytes = wire::encode(wire::SnrResults{2, {snr_frame}});
  EXPECT_FALSE(wire::decode(snr_bytes, &dropout_results, &error));
  EXPECT_NE(error.find("another analysis"), std::string::npos) << error;
}

TEST(AnalysisSeriesWireTest, RefusesCutShortOrOverlongBytes) {
  auto bytes = wire::encode(wire::DropoutResults{
      2,
      {wire::DropoutFrame{1, 1, 4, true}, wire::DropoutFrame{2, 2, 8, true}}});
  wire::DropoutResults out;
  std::string error;

  auto short_bytes = bytes;
  short_bytes.pop_back();
  EXPECT_FALSE(wire::decode(short_bytes, &out, &error));

  auto long_bytes = bytes;
  long_bytes.push_back(0);
  EXPECT_FALSE(wire::decode(long_bytes, &out, &error));

  // A count larger than what follows is refused before anything is sized.
  auto huge = wire::encode(wire::DropoutResults{0, {}});
  huge[huge.size() - 1] = 0xff;
  huge[huge.size() - 2] = 0xff;
  EXPECT_FALSE(wire::decode(huge, &out, &error));

  EXPECT_TRUE(wire::decode(bytes, &out, &error)) << error;
  EXPECT_EQ(out.frames.size(), 2u);
}

}  // namespace orc_unit_test
