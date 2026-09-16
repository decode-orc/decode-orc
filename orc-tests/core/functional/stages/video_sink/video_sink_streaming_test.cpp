/*
 * File:        video_sink_streaming_test.cpp
 * Module:      orc-core functional tests
 * Purpose:     End-to-end regression test for VideoSinkStage's streaming
 *              (unbounded-source) export path
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * Functional (not unit): writes a real video file to disk through the real
 * RawOutputBackend, which unit tests may not touch (AGENTS.md §4.2).
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/observation/observation_context.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "video_sink_stage.h"

namespace orc {
namespace {

// A hand-rolled (non-mock) VideoFrameRepresentation standing in for a piped,
// unbounded (frame_count=0) cvbs_stream_source/tbc_stream_source: it serves
// exactly `real_frame_count` real frames, reports a huge placeholder
// frame_range() the way the real stream sources do, and becomes
// is_exhausted() only once a read past the real data is actually attempted
// — matching ThrottledRingReader's own behaviour. This lets the test drive a
// real, stateful streaming sequence (something gmock's canned EXPECT_CALL
// returns can't easily express) through VideoSinkStage's actual trigger()
// and a real (non-FFmpeg) RawOutputBackend, to confirm the streaming export
// path added for the "FATAL ERROR: vector::reserve" crash stops at the real
// frame count rather than the huge placeholder.
class FakeStreamingVfr : public orc::VideoFrameRepresentation,
                         public orc::Artifact {
 public:
  using sample_type = orc::VideoFrameRepresentation::sample_type;

  explicit FakeStreamingVfr(size_t real_frame_count)
      : orc::Artifact(orc::ArtifactID("fake_streaming_vfr"), orc::Provenance{}),
        real_frame_count_(real_frame_count),
        frame_samples_(static_cast<size_t>(orc::kNtscSamplesPerLine) *
                       static_cast<size_t>(orc::kNtscFrameLines)),
        buffer_(frame_samples_, static_cast<sample_type>(orc::kNtscBlanking)) {
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
  }

  std::string type_name() const override { return "FakeStreamingVfr"; }

  orc::FrameIDRange frame_range() const override {
    return orc::FrameIDRange{0, kPlaceholderLast};
  }
  size_t frame_count() const override { return kPlaceholderLast + 1; }
  bool has_frame(orc::FrameID id) const override {
    return id <= kPlaceholderLast;
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
    if (id >= real_frame_count_) {
      exhausted_ = true;
      return nullptr;
    }
    return buffer_.data();
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    const sample_type* ptr = get_frame(id);
    if (!ptr) return {};
    return buffer_;
  }
  std::optional<orc::SourceParameters> get_video_parameters() const override {
    return params_;
  }
  bool has_unbounded_frame_range() const override { return true; }
  bool is_exhausted() const override { return exhausted_; }

 private:
  // Matches kUnboundedFrameCount (UINT32_MAX) in cvbs_stream_source_stage.cpp
  // / tbc_stream_source_stage.cpp exactly. A smaller placeholder here once
  // let a real bug slip past this test: frameInfoList.reserve() sized off
  // this value only throws std::length_error at the real sentinel's
  // magnitude (~4.3 billion x sizeof(FrameInfo)) — a placeholder of a
  // million or so is a harmless few bytes and does not exercise the crash
  // at all.
  static constexpr orc::FrameID kPlaceholderLast =
      static_cast<orc::FrameID>(0xFFFFFFFFu) - 1;

  size_t real_frame_count_;
  size_t frame_samples_;
  std::vector<sample_type> buffer_;
  orc::SourceParameters params_;
  mutable bool exhausted_ = false;
};

}  // namespace

TEST(VideoSinkStreamingTest, StopsAtTheRealFrameCountNotThePlaceholder) {
  constexpr size_t kRealFrames = 5;
  auto vfr = std::make_shared<FakeStreamingVfr>(kRealFrames);

  const auto out_dir =
      std::filesystem::temp_directory_path() / "orc-video-sink-streaming-test";
  std::filesystem::remove_all(out_dir);
  std::filesystem::create_directories(out_dir);
  const std::string out_path = (out_dir / "out.y4m").string();

  orc::VideoSinkStage stage;
  ObservationContext observation_context;

  const bool result = stage.trigger({vfr},
                                    {{"output_path", out_path},
                                     {"decoder_type", std::string("mono")},
                                     {"output_mode", std::string("raw")},
                                     {"raw_format", std::string("y4m")}},
                                    observation_context);

  EXPECT_TRUE(result) << stage.get_trigger_status();
  EXPECT_THAT(stage.get_trigger_status(), testing::HasSubstr("Streaming"));

  ASSERT_TRUE(std::filesystem::exists(out_path));
  // A non-empty but bounded output size is enough evidence the export
  // stopped at kRealFrames rather than looping toward the placeholder's
  // ~1,000,000 frames (which would produce gigabytes, not bytes).
  const auto file_size = std::filesystem::file_size(out_path);
  EXPECT_GT(file_size, 0u);
  EXPECT_LT(file_size, 50u * 1024 * 1024);

  std::error_code ec;
  std::filesystem::remove_all(out_dir, ec);
}

TEST(VideoSinkStreamingTest, ImmediateEofProducesNoFrames) {
  auto vfr = std::make_shared<FakeStreamingVfr>(0);

  const auto out_dir = std::filesystem::temp_directory_path() /
                       "orc-video-sink-streaming-eof-test";
  std::filesystem::remove_all(out_dir);
  std::filesystem::create_directories(out_dir);
  const std::string out_path = (out_dir / "out.y4m").string();

  orc::VideoSinkStage stage;
  ObservationContext observation_context;

  const bool result = stage.trigger({vfr},
                                    {{"output_path", out_path},
                                     {"decoder_type", std::string("mono")},
                                     {"output_mode", std::string("raw")},
                                     {"raw_format", std::string("y4m")}},
                                    observation_context);

  EXPECT_TRUE(result) << stage.get_trigger_status();
  EXPECT_THAT(stage.get_trigger_status(), testing::HasSubstr("0 frames"));

  std::error_code ec;
  std::filesystem::remove_all(out_dir, ec);
}

}  // namespace orc
