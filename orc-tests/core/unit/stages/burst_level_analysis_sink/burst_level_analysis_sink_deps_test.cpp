/*
 * File:        burst_level_analysis_sink_deps_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for BurstLevelAnalysisSinkStageDeps per-frame
 *              capture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/burst_level_analysis_sink/burst_level_analysis_sink_deps.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <sstream>
#include <vector>

#include "../../include/observation_context_interface_mock.h"
#include "../../include/observation_service_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"

namespace orc_unit_test {
using testing::_;
using testing::NiceMock;
using testing::Return;

namespace {

// median burst == frame_number * 10, keyed off the field id so each analysed
// frame gets a distinct, predictable value. field_id == frame_id * 2,
// frame_number == frame_id + 1.
std::optional<orc::ObservationValue> burst_for(orc::FieldID fid,
                                               const std::string&,
                                               const std::string&) {
  const double frame_number = static_cast<double>(fid.value() / 2 + 1);
  return orc::ObservationValue(frame_number * 10.0);
}

struct Harness {
  std::shared_ptr<NiceMock<MockObservationService>> service;
  std::shared_ptr<NiceMock<MockObservationContext>> context;
  std::shared_ptr<NiceMock<MockVideoFrameRepresentationArtifact>> vfr;
  std::unique_ptr<orc::BurstLevelAnalysisSinkStageDeps> deps;
  std::atomic<bool> cancel{false};
};

std::unique_ptr<Harness> make_harness(orc::FrameIDRange range) {
  auto h = std::make_unique<Harness>();
  h->service = std::make_shared<NiceMock<MockObservationService>>();
  h->context = std::make_shared<NiceMock<MockObservationContext>>();
  h->vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  ON_CALL(*h->service, create_observer(_))
      .WillByDefault([](const std::string&) {
        return std::make_unique<NiceMock<MockObserverHandle>>();
      });
  ON_CALL(*h->context, get(_, "burst_level", "median_burst_10bit"))
      .WillByDefault(burst_for);
  ON_CALL(*h->vfr, frame_range()).WillByDefault(Return(range));

  h->deps =
      std::make_unique<orc::BurstLevelAnalysisSinkStageDeps>(h->service.get());
  h->deps->init(nullptr, &h->cancel);
  return h;
}

orc::BurstAnalysisComputeOptions default_options() {
  return orc::BurstAnalysisComputeOptions{};
}

}  // namespace

TEST(BurstLevelAnalysisSinkDepsTest, AnalysesEveryFrame) {
  auto h = make_harness(orc::FrameIDRange{0, 3});  // 4 frames
  const auto result = h->deps->compute_and_analyze(h->vfr.get(), *h->context,
                                                   default_options());

  ASSERT_TRUE(result.success);
  ASSERT_EQ(result.frame_stats.size(), 4u);
  EXPECT_EQ(result.total_frames, 4);
  for (size_t i = 0; i < result.frame_stats.size(); ++i) {
    const auto& fs = result.frame_stats[i];
    EXPECT_EQ(fs.frame_number, static_cast<int32_t>(i) + 1);
    EXPECT_TRUE(fs.has_data);
    EXPECT_DOUBLE_EQ(fs.median_burst_10bit,
                     static_cast<double>(fs.frame_number) * 10.0);
  }
}

TEST(BurstLevelAnalysisSinkDepsTest,
     NonZeroFirstFramePreservesTrueFrameNumbers) {
  auto h = make_harness(orc::FrameIDRange{10, 12});  // frames at ids 10,11,12
  const auto result = h->deps->compute_and_analyze(h->vfr.get(), *h->context,
                                                   default_options());

  ASSERT_TRUE(result.success);
  ASSERT_EQ(result.frame_stats.size(), 3u);
  EXPECT_EQ(result.frame_stats[0].frame_number, 11);
  EXPECT_EQ(result.frame_stats[1].frame_number, 12);
  EXPECT_EQ(result.frame_stats[2].frame_number, 13);
}

TEST(BurstLevelAnalysisSinkDepsTest, EmptyRangeYieldsNoRecords) {
  auto h = make_harness(orc::FrameIDRange{1, 0});  // count() == 0
  const auto result = h->deps->compute_and_analyze(h->vfr.get(), *h->context,
                                                   default_options());

  ASSERT_TRUE(result.success);
  EXPECT_TRUE(result.frame_stats.empty());
  EXPECT_EQ(result.total_frames, 0);
}

// When cancellation is requested, analysis reports failure so the stage never
// proceeds to write a (partial) CSV. This is the deps-level guard behind the
// "cancelling leaves no truncated output file" requirement.
TEST(BurstLevelAnalysisSinkDepsTest,
     CancelledRunReportsFailureAndWritesNothing) {
  auto h = make_harness(orc::FrameIDRange{0, 3});  // 4 frames
  h->cancel.store(true);

  const auto result = h->deps->compute_and_analyze(h->vfr.get(), *h->context,
                                                   default_options());

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "Cancelled by user");
}

// ----- CSV writer (stream formatter) -----

TEST(BurstLevelAnalysisSinkCsvTest, HeaderIsSelfDescribing) {
  orc::BurstLevelAnalysisSinkStageDeps deps(nullptr);
  std::ostringstream os;
  deps.write_csv(os, {});
  EXPECT_EQ(os.str(), "frame_number,median_burst_10bit\n");
}

TEST(BurstLevelAnalysisSinkCsvTest, WritesOneRowPerAnalysedFrame) {
  orc::BurstLevelAnalysisSinkStageDeps deps(nullptr);
  std::vector<orc::FrameBurstLevelStats> stats;
  orc::FrameBurstLevelStats a{};
  a.frame_number = 1;
  a.median_burst_10bit = 200.0;
  a.has_data = true;
  orc::FrameBurstLevelStats b{};
  b.frame_number = 7;
  b.median_burst_10bit = 205.5;
  b.has_data = true;
  stats.push_back(a);
  stats.push_back(b);

  std::ostringstream os;
  deps.write_csv(os, stats);
  EXPECT_EQ(os.str(),
            "frame_number,median_burst_10bit\n"
            "1,200\n"
            "7,205.5\n");
}

TEST(BurstLevelAnalysisSinkCsvTest, AbsentValueSerialisesAsEmptyField) {
  orc::BurstLevelAnalysisSinkStageDeps deps(nullptr);
  std::vector<orc::FrameBurstLevelStats> stats;
  orc::FrameBurstLevelStats missing{};
  missing.frame_number = 3;
  missing.has_data = false;  // no burst level captured
  stats.push_back(missing);

  std::ostringstream os;
  deps.write_csv(os, stats);
  EXPECT_EQ(os.str(),
            "frame_number,median_burst_10bit\n"
            "3,\n");
  EXPECT_EQ(os.str().find("nan"), std::string::npos);
}

}  // namespace orc_unit_test
