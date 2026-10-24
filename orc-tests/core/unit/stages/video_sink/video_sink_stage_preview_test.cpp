/*
 * File:        video_sink_stage_preview_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for VideoSinkStage's colour preview with temporal
 *              noise reduction
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "../../../../orc/plugins/stages/sinks/common/decoders/noise_reducer.h"
#include "../../../../orc/plugins/stages/sinks/common/video_sink_stage.h"

namespace orc_unit_test {
namespace {

constexpr size_t kFrameCount = 12;
constexpr double kNoiseSigma = 6.0;  // composite codes
constexpr double kStrengthDb = 6.0;

// An in-memory NTSC source with a still mid-grey picture and independent
// Gaussian noise on every frame, so consecutive frames differ only by noise.
// Bounded frame range, nothing on disk.
class StillNoisyNtscVfr : public orc::VideoFrameRepresentation,
                          public orc::Artifact {
 public:
  using sample_type = orc::VideoFrameRepresentation::sample_type;

  StillNoisyNtscVfr()
      : orc::Artifact(orc::ArtifactID("still_noisy_ntsc_vfr"),
                      orc::Provenance{}),
        frame_samples_(static_cast<size_t>(orc::kNtscSamplesPerLine) *
                       static_cast<size_t>(orc::kNtscFrameLines)) {
    params_.system = orc::VideoSystem::NTSC;
    params_.frame_width_nominal = orc::kNtscSamplesPerLine;
    params_.frame_height = orc::kNtscFrameLines;
    params_.sync_tip_level = orc::kNtscSyncTip;
    params_.blanking_level = orc::kNtscBlanking;
    params_.black_level = orc::kNtscBlack;
    params_.white_level = orc::kNtscWhite;
    params_.peak_level = orc::kNtscPeak;
    params_.active_video_start = orc::kNtscActiveVideoStart;
    params_.active_video_end = orc::kNtscActiveVideoEnd;
    params_.first_active_frame_line = orc::kNtscFirstActiveFrameLine;
    params_.last_active_frame_line = orc::kNtscLastActiveFrameLine;

    std::mt19937 rng(20261023u);
    std::normal_distribution<double> noise(0.0, kNoiseSigma);
    const double grey = 0.5 * (orc::kNtscBlack + orc::kNtscWhite);
    frames_.reserve(kFrameCount);
    for (size_t n = 0; n < kFrameCount; ++n) {
      std::vector<sample_type> frame(
          frame_samples_, static_cast<sample_type>(orc::kNtscBlanking));
      for (int32_t line = params_.first_active_frame_line;
           line < params_.last_active_frame_line; ++line) {
        sample_type* row =
            frame.data() + static_cast<size_t>(line) *
                               static_cast<size_t>(orc::kNtscSamplesPerLine);
        for (int32_t x = params_.active_video_start;
             x < params_.active_video_end; ++x) {
          row[x] = static_cast<sample_type>(std::lround(grey + noise(rng)));
        }
      }
      frames_.push_back(std::move(frame));
    }
  }

  std::string type_name() const override { return "StillNoisyNtscVfr"; }
  orc::FrameIDRange frame_range() const override {
    return orc::FrameIDRange{0, kFrameCount - 1};
  }
  size_t frame_count() const override { return kFrameCount; }
  bool has_frame(orc::FrameID id) const override { return id < kFrameCount; }
  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    orc::FrameDescriptor d;
    d.frame_id = id;
    d.system = orc::VideoSystem::NTSC;
    d.height = static_cast<size_t>(orc::kNtscFrameLines);
    d.samples_total = frame_samples_;
    d.samples_per_line_nominal = static_cast<size_t>(orc::kNtscSamplesPerLine);
    return d;
  }
  const sample_type* get_frame(orc::FrameID id) const override {
    return has_frame(id) ? frames_[id].data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return has_frame(id) ? frames_[id] : std::vector<sample_type>{};
  }
  std::optional<orc::SourceParameters> get_video_parameters() const override {
    return params_;
  }

 private:
  size_t frame_samples_;
  std::vector<std::vector<sample_type>> frames_;
  orc::SourceParameters params_;
};

class VideoSinkStagePreviewTest : public ::testing::Test {
 protected:
  void SetUp() override {
    source_ = std::make_shared<StillNoisyNtscVfr>();
    orc::ObservationContext context;
    stage_.execute({source_}, {}, context);
    stage_.set_parameters({{"decoder_type", std::string("ntsc2d")}});
  }

  void set_strength(double strength_db) {
    ASSERT_TRUE(stage_.set_parameters({{"noise_reduction", strength_db}}));
  }

  orc::ColourFrameCarrier preview(uint64_t index) {
    auto carrier = stage_.get_colour_preview_carrier(
        index, orc::PreviewNavigationHint::Random);
    EXPECT_TRUE(carrier.has_value()) << "frame " << index;
    return carrier.value_or(orc::ColourFrameCarrier{});
  }

  // Root mean square difference of the active luma of two carriers.
  static double rms_luma_difference(const orc::ColourFrameCarrier& a,
                                    const orc::ColourFrameCarrier& b) {
    EXPECT_EQ(a.width, b.width);
    EXPECT_EQ(a.height, b.height);
    double sum = 0.0;
    size_t count = 0;
    for (uint32_t y = a.active_y_start; y < a.active_y_end; ++y) {
      for (uint32_t x = a.active_x_start; x < a.active_x_end; ++x) {
        const size_t i = static_cast<size_t>(y) * a.width + x;
        const double d = a.y_plane[i] - b.y_plane[i];
        sum += d * d;
        ++count;
      }
    }
    return count > 0 ? std::sqrt(sum / static_cast<double>(count)) : 0.0;
  }

  // Standard deviation of the active luma of a carrier about its mean: on
  // the still grey picture, its noise.
  static double active_luma_deviation(const orc::ColourFrameCarrier& a) {
    double sum = 0.0;
    double sum_squares = 0.0;
    size_t count = 0;
    for (uint32_t y = a.active_y_start; y < a.active_y_end; ++y) {
      for (uint32_t x = a.active_x_start; x < a.active_x_end; ++x) {
        const double v = a.y_plane[static_cast<size_t>(y) * a.width + x];
        sum += v;
        sum_squares += v * v;
        ++count;
      }
    }
    if (count == 0) return 0.0;
    const double mean = sum / static_cast<double>(count);
    return std::sqrt(
        std::max(0.0, sum_squares / static_cast<double>(count) - mean * mean));
  }

  static bool same_planes(const orc::ColourFrameCarrier& a,
                          const orc::ColourFrameCarrier& b) {
    return a.y_plane == b.y_plane && a.u_plane == b.u_plane &&
           a.v_plane == b.v_plane;
  }

  std::shared_ptr<StillNoisyNtscVfr> source_;
  orc::VideoSinkStage stage_;
};

TEST_F(VideoSinkStagePreviewTest,
       RepeatsTheSameFilteredFrame_AndFollowsTemporalNrChanges) {
  set_strength(kStrengthDb);
  const auto first = preview(5);
  const auto again = preview(5);
  EXPECT_TRUE(same_planes(first, again));

  set_strength(0.0);
  const auto unfiltered = preview(5);
  EXPECT_FALSE(same_planes(first, unfiltered));
  EXPECT_GT(rms_luma_difference(first, unfiltered), 0.0);

  set_strength(9.0);
  const auto stronger = preview(5);
  EXPECT_FALSE(same_planes(first, stronger));
  EXPECT_FALSE(same_planes(unfiltered, stronger));
}

TEST_F(VideoSinkStagePreviewTest,
       ColdRequestForFrameZero_IsFilteredWithinTheFrame) {
  // Frame 0 has no earlier frame to average with, so it comes out smoothed
  // within itself, as the first frame of an export does: changed from the
  // plain decode, no noisier, and the same on every request. How much the
  // window removes depends on how the decoder has shaped the noise, so the
  // amount is checked on the filter itself, not here.
  set_strength(kStrengthDb);
  preview(3);  // so the request below is a seek, not a continuation
  const auto filtered = preview(0);
  preview(3);
  const auto again = preview(0);
  EXPECT_TRUE(same_planes(filtered, again));

  set_strength(0.0);
  const auto unfiltered = preview(0);
  EXPECT_FALSE(same_planes(filtered, unfiltered));
  EXPECT_LE(active_luma_deviation(filtered), active_luma_deviation(unfiltered));
}

TEST_F(VideoSinkStagePreviewTest,
       SeekWarmsUpToWithinTheColdStartToleranceOfPlayback) {
  // Noise level of the decoded luma, from two unfiltered frames that differ
  // only by their noise.
  set_strength(0.0);
  const auto unfiltered_7 = preview(7);
  const auto unfiltered_8 = preview(8);
  const double sigma =
      rms_luma_difference(unfiltered_7, unfiltered_8) / std::sqrt(2.0);
  ASSERT_GT(sigma, 0.5);

  // Playback: frame 8 reached through 6 and 7, with 6 itself warmed up, so
  // its history runs from frame 2.
  set_strength(kStrengthDb);
  preview(6);
  preview(7);
  const auto played = preview(8);

  // Seek: frame 8 cold, warmed up from frame 4.
  preview(0);
  const auto sought = preview(8);

  // The two histories differ only in frames 2 and 3, whose contribution to
  // frame 8 is attenuated by five blend steps (w^5 = 0.08 at 6 dB).
  const double w = NoiseReducer::blend_weight_for_strength_db(kStrengthDb);
  const double tolerance = 2.0 * std::pow(w, 5) * std::sqrt(2.0) * sigma;
  const double seek_error = rms_luma_difference(played, sought);
  EXPECT_LT(seek_error, tolerance);
  // The continuation carries more history than the warm-up, so the two are
  // close but not the same frame.
  EXPECT_GT(seek_error, 0.0);

  // Both are filtered: well away from the unfiltered decode.
  EXPECT_GT(rms_luma_difference(sought, unfiltered_8), 0.3 * sigma);
  EXPECT_GT(rms_luma_difference(played, unfiltered_8), 0.3 * sigma);
}

TEST_F(VideoSinkStagePreviewTest,
       DecoderSchedule_PreviewsEachFrameWithItsScheduledDecoder) {
  set_strength(0.0);
  ASSERT_TRUE(stage_.set_parameters({{"decoder_type", std::string("mono")}}));
  const auto mono_4 = preview(4);
  ASSERT_TRUE(stage_.set_parameters({{"decoder_type", std::string("ntsc2d")}}));
  const auto colour_2 = preview(2);
  const auto colour_7 = preview(7);
  ASSERT_FALSE(same_planes(mono_4, preview(4)));

  // Stored 0-based: frames 3-5 mono, everything else ntsc2d.
  ASSERT_TRUE(stage_.set_parameters(
      {{"decoder_type", std::string("multiple")},
       {"decoder_schedule", std::string("ntsc2d,3-5:mono")}}));
  EXPECT_TRUE(same_planes(preview(2), colour_2));
  EXPECT_TRUE(same_planes(preview(4), mono_4));
  EXPECT_TRUE(same_planes(preview(7), colour_7));
}

TEST_F(VideoSinkStagePreviewTest, DecoderSchedule_UncoveredFrameHasNoPreview) {
  ASSERT_TRUE(
      stage_.set_parameters({{"decoder_type", std::string("multiple")},
                             {"decoder_schedule", std::string("0-3:ntsc2d")}}));

  EXPECT_TRUE(
      stage_.get_colour_preview_carrier(2, orc::PreviewNavigationHint::Random)
          .has_value());
  EXPECT_FALSE(
      stage_.get_colour_preview_carrier(6, orc::PreviewNavigationHint::Random)
          .has_value());
}

}  // namespace
}  // namespace orc_unit_test
