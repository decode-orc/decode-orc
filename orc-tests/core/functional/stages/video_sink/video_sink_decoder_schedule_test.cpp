/*
 * File:        video_sink_decoder_schedule_test.cpp
 * Module:      orc-core functional tests
 * Purpose:     End-to-end tests for VideoSinkStage's per-frame-range decoder
 *              schedule in the batch and streaming export paths
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * Functional (not unit): writes real video files to disk through the real
 * RawOutputBackend, which unit tests may not touch (AGENTS.md §4.2).
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "video_sink_stage.h"

namespace orc {
namespace {

constexpr size_t kFrameCount = 6;

// Codes the active picture of frame n rises by, so every frame differs.
constexpr int32_t kLumaStep = 24;

// Amplitude, in composite codes, of the 4fsc subcarrier pattern laid over
// the picture. A comb filter takes it out of luma as chroma; the mono
// decoder leaves it in luma, so the two decoders' output differs visibly.
constexpr int32_t kSubcarrierAmplitude = 60;

// An in-memory NTSC source: a flat picture per frame carrying a subcarrier
// pattern. Bounded, or standing in for a piped source of unknown length (a
// placeholder frame_range() that only becomes exhausted once a read past the
// real frames is attempted, as the stream sources behave).
class PatternedNtscVfr : public orc::VideoFrameRepresentation,
                         public orc::Artifact {
 public:
  using sample_type = orc::VideoFrameRepresentation::sample_type;

  explicit PatternedNtscVfr(bool unbounded)
      : orc::Artifact(orc::ArtifactID("patterned_ntsc_vfr"), orc::Provenance{}),
        unbounded_(unbounded),
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

    // One subcarrier cycle is four samples at 4fsc.
    static constexpr int32_t kCycle[4] = {1, 0, -1, 0};
    for (size_t n = 0; n < kFrameCount; ++n) {
      std::vector<sample_type> frame(
          frame_samples_, static_cast<sample_type>(orc::kNtscBlanking));
      const int32_t level =
          orc::kNtscBlack + 200 + static_cast<int32_t>(n) * kLumaStep;
      for (int32_t line = params_.first_active_frame_line;
           line < params_.last_active_frame_line; ++line) {
        sample_type* row =
            frame.data() + static_cast<size_t>(line) *
                               static_cast<size_t>(orc::kNtscSamplesPerLine);
        for (int32_t x = params_.active_video_start;
             x < params_.active_video_end; ++x) {
          row[x] = static_cast<sample_type>(
              level + kSubcarrierAmplitude * kCycle[(x + line * 2) % 4]);
        }
      }
      frames_.push_back(std::move(frame));
    }
  }

  std::string type_name() const override { return "PatternedNtscVfr"; }
  orc::FrameIDRange frame_range() const override {
    return orc::FrameIDRange{0,
                             unbounded_ ? kPlaceholderLast : kFrameCount - 1};
  }
  size_t frame_count() const override {
    return unbounded_ ? kPlaceholderLast + 1 : kFrameCount;
  }
  bool has_frame(orc::FrameID id) const override {
    return id <= frame_range().last;
  }
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
    if (id >= kFrameCount) {
      exhausted_ = true;
      return nullptr;
    }
    return frames_[id].data();
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    const sample_type* ptr = get_frame(id);
    return ptr ? frames_[id] : std::vector<sample_type>{};
  }
  std::optional<orc::SourceParameters> get_video_parameters() const override {
    return params_;
  }
  bool has_unbounded_frame_range() const override { return unbounded_; }
  bool is_exhausted() const override { return unbounded_ && exhausted_; }

 private:
  static constexpr orc::FrameID kPlaceholderLast =
      static_cast<orc::FrameID>(0xFFFFFFFFu) - 1;

  bool unbounded_;
  size_t frame_samples_;
  std::vector<std::vector<sample_type>> frames_;
  orc::SourceParameters params_;
  mutable bool exhausted_ = false;
};

struct ExportResult {
  bool ok = false;
  std::string status;
  std::vector<std::string> frames;  // Raw YUV444P16 bytes of each frame
};

class VideoSinkDecoderScheduleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    out_dir_ = std::filesystem::temp_directory_path() /
               "orc-video-sink-decoder-schedule-test";
    std::filesystem::remove_all(out_dir_);
    std::filesystem::create_directories(out_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(out_dir_, ec);
  }

  // Export the whole source with decoder_type (and decoder_schedule when
  // given) and split the raw output into frames.
  ExportResult run_export(bool unbounded, const std::string& decoder_type,
                          const std::string& schedule = {}) {
    const std::string out_path =
        (out_dir_ / ("out" + std::to_string(export_count_++) + ".yuv"))
            .string();
    orc::VideoSinkStage stage;
    ObservationContext observation_context;
    std::map<std::string, ParameterValue> params = {
        {"output_path", out_path},
        {"decoder_type", decoder_type},
        {"output_mode", std::string("raw")},
        {"raw_format", std::string("yuv")}};
    if (!schedule.empty()) params["decoder_schedule"] = schedule;

    ExportResult result;
    result.ok = stage.trigger({std::make_shared<PatternedNtscVfr>(unbounded)},
                              params, observation_context);
    result.status = stage.get_trigger_status();
    if (!result.ok || !std::filesystem::exists(out_path)) return result;

    std::ifstream in(out_path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(in)),
                            std::istreambuf_iterator<char>());
    EXPECT_EQ(bytes.size() % kFrameCount, 0U);
    const size_t frame_bytes = bytes.size() / kFrameCount;
    for (size_t n = 0; n < kFrameCount; ++n) {
      result.frames.push_back(bytes.substr(n * frame_bytes, frame_bytes));
    }
    return result;
  }

  // Each frame of `scheduled` must be the same frame of `inside` for frames
  // first..last and of `outside` for the rest.
  static void expect_frames_from(const ExportResult& scheduled,
                                 const ExportResult& outside,
                                 const ExportResult& inside, size_t first,
                                 size_t last) {
    ASSERT_EQ(scheduled.frames.size(), kFrameCount) << scheduled.status;
    ASSERT_EQ(outside.frames.size(), kFrameCount) << outside.status;
    ASSERT_EQ(inside.frames.size(), kFrameCount) << inside.status;
    for (size_t n = 0; n < kFrameCount; ++n) {
      const bool in_range = n >= first && n <= last;
      ASSERT_NE(outside.frames[n], inside.frames[n])
          << "the two decoders must differ on frame " << n;
      EXPECT_EQ(scheduled.frames[n],
                in_range ? inside.frames[n] : outside.frames[n])
          << "frame " << n;
    }
  }

  std::filesystem::path out_dir_;
  int export_count_ = 0;
};

TEST_F(VideoSinkDecoderScheduleTest, Batch_DecodesEachFrameWithItsDecoder) {
  const auto colour = run_export(false, "ntsc2d");
  const auto mono = run_export(false, "mono");
  const auto scheduled = run_export(false, "multiple", "ntsc2d,2-3:mono");

  EXPECT_TRUE(scheduled.ok) << scheduled.status;
  expect_frames_from(scheduled, colour, mono, 2, 3);
}

TEST_F(VideoSinkDecoderScheduleTest, Batch_GivesEachDecoderItsOwnLookAround) {
  // ntsc3d reads a frame either side; ntsc2d reads none. The 3D frames must
  // come out as they do in an all-3D export, the 2D ones as in an all-2D one.
  const auto three_d = run_export(false, "ntsc3d");
  const auto two_d = run_export(false, "ntsc2d");
  const auto scheduled = run_export(false, "multiple", "ntsc3d,1-3:ntsc2d");

  EXPECT_TRUE(scheduled.ok) << scheduled.status;
  expect_frames_from(scheduled, three_d, two_d, 1, 3);
}

TEST_F(VideoSinkDecoderScheduleTest, Streaming_DecodesEachFrameWithItsDecoder) {
  const auto colour = run_export(true, "ntsc2d");
  const auto mono = run_export(true, "mono");
  const auto scheduled = run_export(true, "multiple", "ntsc2d,2-3:mono");

  EXPECT_TRUE(scheduled.ok) << scheduled.status;
  EXPECT_THAT(scheduled.status, testing::HasSubstr("Streaming"));
  expect_frames_from(scheduled, colour, mono, 2, 3);
}

TEST_F(VideoSinkDecoderScheduleTest, Batch_UncoveredFrameIsRefusedUpFront) {
  const auto result = run_export(false, "multiple", "0-2:ntsc2d");

  EXPECT_FALSE(result.ok);
  // Frame 3 (0-based) is the first without a decoder; users count from 1.
  EXPECT_THAT(result.status, testing::HasSubstr("no decoder for frame 4"));
}

TEST_F(VideoSinkDecoderScheduleTest, Streaming_UncoveredFrameFailsTheExport) {
  const auto result = run_export(true, "multiple", "0-2:ntsc2d");

  EXPECT_FALSE(result.ok);
  EXPECT_THAT(result.status, testing::HasSubstr("no decoder for frame 4"));
}

TEST_F(VideoSinkDecoderScheduleTest,
       Streaming_ScheduleCoveringTheWholeStreamNeedsNoDefault) {
  // Frames past the end of the stream are never decoded, so ranges that
  // cover every real frame are enough.
  const auto result = run_export(true, "multiple", "0-2:ntsc2d,3-5:mono");

  EXPECT_TRUE(result.ok) << result.status;
  EXPECT_EQ(result.frames.size(), kFrameCount);
}

TEST_F(VideoSinkDecoderScheduleTest, Batch_DecoderUnsuitedToSourceIsRefused) {
  const auto result = run_export(false, "multiple", "ntsc2d,1-2:pal2d");

  EXPECT_FALSE(result.ok);
  EXPECT_THAT(result.status, testing::HasSubstr("pal2d"));
}

}  // namespace
}  // namespace orc
