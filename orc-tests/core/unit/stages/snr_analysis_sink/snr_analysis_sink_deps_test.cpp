/*
 * File:        snr_analysis_sink_deps_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for SNRAnalysisSinkStageDeps per-frame capture
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/snr_analysis_sink/snr_analysis_sink_deps.h"

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

// Observation value keyed off the field id so each analysed frame gets a
// distinct, predictable metric: white SNR == frame_number, black PSNR ==
// frame_number + 100. field_id == frame_id * 2, frame_number == frame_id + 1.
std::optional<orc::ObservationValue> white_for(orc::FieldID fid,
                                               const std::string&,
                                               const std::string&) {
  const double frame_number = static_cast<double>(fid.value() / 2 + 1);
  return orc::ObservationValue(frame_number);
}
std::optional<orc::ObservationValue> black_for(orc::FieldID fid,
                                               const std::string&,
                                               const std::string&) {
  const double frame_number = static_cast<double>(fid.value() / 2 + 1);
  return orc::ObservationValue(frame_number + 100.0);
}

// Builds a deps instance wired to a service that hands out no-op observer
// handles, plus a context that answers white/black queries via the helpers
// above. Ownership of the mocks is returned to the caller.
struct Harness {
  std::shared_ptr<NiceMock<MockObservationService>> service;
  std::shared_ptr<NiceMock<MockObservationContext>> context;
  std::shared_ptr<NiceMock<MockVideoFrameRepresentationArtifact>> vfr;
  std::unique_ptr<orc::SNRAnalysisSinkStageDeps> deps;
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
  ON_CALL(*h->context, get(_, "white_snr", "snr_db")).WillByDefault(white_for);
  ON_CALL(*h->context, get(_, "black_psnr", "psnr_db"))
      .WillByDefault(black_for);
  ON_CALL(*h->vfr, frame_range()).WillByDefault(Return(range));

  h->deps = std::make_unique<orc::SNRAnalysisSinkStageDeps>(h->service.get());
  h->deps->init(nullptr, &h->cancel);
  return h;
}

orc::SNRAnalysisComputeOptions default_options() {
  orc::SNRAnalysisComputeOptions opts;
  opts.snr_mode = orc::SNRAnalysisMode::BOTH;
  return opts;
}

}  // namespace

TEST(SNRAnalysisSinkDepsTest, AnalysesEveryFrame) {
  auto h = make_harness(orc::FrameIDRange{0, 4});  // 5 frames
  const auto result = h->deps->compute_and_analyze(h->vfr.get(), *h->context,
                                                   default_options());

  ASSERT_TRUE(result.success);
  ASSERT_EQ(result.frame_stats.size(), 5u);
  EXPECT_EQ(result.total_frames, 5);
  for (size_t i = 0; i < result.frame_stats.size(); ++i) {
    const auto& fs = result.frame_stats[i];
    EXPECT_EQ(fs.frame_number, static_cast<int32_t>(i) + 1);
    EXPECT_TRUE(fs.has_white_snr);
    EXPECT_TRUE(fs.has_black_psnr);
    EXPECT_DOUBLE_EQ(fs.white_snr, static_cast<double>(fs.frame_number));
    EXPECT_DOUBLE_EQ(fs.black_psnr,
                     static_cast<double>(fs.frame_number) + 100.0);
  }
}

TEST(SNRAnalysisSinkDepsTest, NonZeroFirstFramePreservesTrueFrameNumbers) {
  auto h = make_harness(orc::FrameIDRange{10, 12});  // frames at ids 10,11,12
  const auto result = h->deps->compute_and_analyze(h->vfr.get(), *h->context,
                                                   default_options());

  ASSERT_TRUE(result.success);
  ASSERT_EQ(result.frame_stats.size(), 3u);
  EXPECT_EQ(result.frame_stats[0].frame_number, 11);
  EXPECT_EQ(result.frame_stats[1].frame_number, 12);
  EXPECT_EQ(result.frame_stats[2].frame_number, 13);
}

TEST(SNRAnalysisSinkDepsTest, EmptyRangeYieldsNoRecords) {
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
TEST(SNRAnalysisSinkDepsTest, CancelledRunReportsFailureAndWritesNothing) {
  auto h = make_harness(orc::FrameIDRange{0, 4});  // 5 frames
  h->cancel.store(true);

  const auto result = h->deps->compute_and_analyze(h->vfr.get(), *h->context,
                                                   default_options());

  EXPECT_FALSE(result.success);
  EXPECT_EQ(result.message, "Cancelled by user");
}

// ----- CSV writer (stream formatter) -----

TEST(SNRAnalysisSinkCsvTest, HeaderIsSelfDescribing) {
  orc::SNRAnalysisSinkStageDeps deps(nullptr);
  std::ostringstream os;
  deps.write_csv(os, {});
  EXPECT_EQ(os.str(), "frame_number,white_snr_db,black_psnr_db\n");
}

TEST(SNRAnalysisSinkCsvTest, WritesOneRowPerAnalysedFrameWithBothMetrics) {
  orc::SNRAnalysisSinkStageDeps deps(nullptr);
  std::vector<orc::FrameSNRStats> stats;
  orc::FrameSNRStats a{};
  a.frame_number = 1;
  a.white_snr = 42.5;
  a.has_white_snr = true;
  a.black_psnr = 38.0;
  a.has_black_psnr = true;
  a.has_data = true;
  stats.push_back(a);

  std::ostringstream os;
  deps.write_csv(os, stats);
  EXPECT_EQ(os.str(),
            "frame_number,white_snr_db,black_psnr_db\n"
            "1,42.5,38\n");
}

TEST(SNRAnalysisSinkCsvTest, AbsentMetricsSerialiseAsEmptyFields) {
  orc::SNRAnalysisSinkStageDeps deps(nullptr);
  std::vector<orc::FrameSNRStats> stats;
  // White only.
  orc::FrameSNRStats white_only{};
  white_only.frame_number = 2;
  white_only.white_snr = 40.0;
  white_only.has_white_snr = true;
  white_only.has_data = true;
  stats.push_back(white_only);
  // Black only.
  orc::FrameSNRStats black_only{};
  black_only.frame_number = 3;
  black_only.black_psnr = 30.0;
  black_only.has_black_psnr = true;
  black_only.has_data = true;
  stats.push_back(black_only);
  // Neither metric captured for this analysed frame.
  orc::FrameSNRStats neither{};
  neither.frame_number = 4;
  neither.has_data = false;
  stats.push_back(neither);

  std::ostringstream os;
  deps.write_csv(os, stats);
  EXPECT_EQ(os.str(),
            "frame_number,white_snr_db,black_psnr_db\n"
            "2,40,\n"
            "3,,30\n"
            "4,,\n");
  EXPECT_EQ(os.str().find("nan"), std::string::npos);
}

// ----- Phase 5.3: reuse of pre-loaded observations -----

using testing::ByMove;
using testing::StrictMock;

namespace {

// A deps instance whose observer handles are captured so a test can assert the
// exact number of process_frame() calls. The context's has() answers control
// which frames are treated as already covered by the store pre-load.
struct ReuseHarness {
  std::shared_ptr<NiceMock<MockObservationService>> service;
  std::shared_ptr<NiceMock<MockObservationContext>> context;
  std::shared_ptr<NiceMock<MockVideoFrameRepresentationArtifact>> vfr;
  StrictMock<MockObserverHandle>* white_handle = nullptr;
  StrictMock<MockObserverHandle>* black_handle = nullptr;
  std::unique_ptr<orc::SNRAnalysisSinkStageDeps> deps;
  std::atomic<bool> cancel{false};
};

std::unique_ptr<ReuseHarness> make_reuse_harness(orc::FrameIDRange range) {
  auto h = std::make_unique<ReuseHarness>();
  h->service = std::make_shared<NiceMock<MockObservationService>>();
  h->context = std::make_shared<NiceMock<MockObservationContext>>();
  h->vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  auto white = std::make_unique<StrictMock<MockObserverHandle>>();
  auto black = std::make_unique<StrictMock<MockObserverHandle>>();
  h->white_handle = white.get();
  h->black_handle = black.get();

  EXPECT_CALL(*h->service, create_observer("white_snr"))
      .WillOnce(Return(ByMove(std::move(white))));
  EXPECT_CALL(*h->service, create_observer("black_psnr"))
      .WillOnce(Return(ByMove(std::move(black))));

  ON_CALL(*h->context, get(_, "white_snr", "snr_db")).WillByDefault(white_for);
  ON_CALL(*h->context, get(_, "black_psnr", "psnr_db"))
      .WillByDefault(black_for);
  ON_CALL(*h->vfr, frame_range()).WillByDefault(Return(range));

  h->deps = std::make_unique<orc::SNRAnalysisSinkStageDeps>(h->service.get());
  h->deps->init(nullptr, &h->cancel);
  return h;
}

}  // namespace

// Fully covered store: every frame's value is already present, so the sink runs
// the observers zero times yet still produces a full result set.
TEST(SNRAnalysisSinkDepsReuseTest, FullyCoveredContextRunsZeroObserverFrames) {
  auto h = make_reuse_harness(orc::FrameIDRange{0, 4});  // 5 frames
  ON_CALL(*h->context, has(_, "white_snr", "snr_db"))
      .WillByDefault(Return(true));
  ON_CALL(*h->context, has(_, "black_psnr", "psnr_db"))
      .WillByDefault(Return(true));

  EXPECT_CALL(*h->white_handle, process_frame(_, _, _)).Times(0);
  EXPECT_CALL(*h->black_handle, process_frame(_, _, _)).Times(0);

  const auto result = h->deps->compute_and_analyze(h->vfr.get(), *h->context,
                                                   default_options());

  ASSERT_TRUE(result.success);
  ASSERT_EQ(result.frame_stats.size(), 5u);
  for (const auto& fs : result.frame_stats) {
    EXPECT_TRUE(fs.has_white_snr);  // values come from the pre-loaded context
    EXPECT_TRUE(fs.has_black_psnr);
  }
}

// Partial coverage: only the uncovered frames are observed. Frames 0..2 are
// pre-loaded (has() == true); frames 3,4 are missing and must be observed.
TEST(SNRAnalysisSinkDepsReuseTest, PartialCoverageObservesOnlyMissingFrames) {
  auto h = make_reuse_harness(orc::FrameIDRange{0, 4});  // 5 frames

  // field id == frame_id * 2, so covered frames 0,1,2 -> fields 0,2,4.
  const auto covered = [](orc::FieldID fid, const std::string&,
                          const std::string&) { return fid.value() <= 4; };
  ON_CALL(*h->context, has(_, "white_snr", "snr_db")).WillByDefault(covered);
  ON_CALL(*h->context, has(_, "black_psnr", "psnr_db")).WillByDefault(covered);

  // Only frames 3 and 4 are re-observed by each stateless handle.
  EXPECT_CALL(*h->white_handle, process_frame(_, _, _)).Times(2);
  EXPECT_CALL(*h->black_handle, process_frame(_, _, _)).Times(2);

  const auto result = h->deps->compute_and_analyze(h->vfr.get(), *h->context,
                                                   default_options());

  ASSERT_TRUE(result.success);
  EXPECT_EQ(result.frame_stats.size(), 5u);
}

}  // namespace orc_unit_test
