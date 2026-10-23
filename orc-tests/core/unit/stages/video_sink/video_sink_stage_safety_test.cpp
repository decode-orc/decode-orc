/*
 * File:        video_sink_stage_safety_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for video sink invalid metadata hardening
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "../../../../orc/plugins/stages/sinks/common/video_sink_stage.h"
#include "../../include/observation_context_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"

namespace orc_unit_test {
using testing::HasSubstr;
using testing::NiceMock;
using testing::Return;

namespace {
orc::SourceParameters make_too_narrow_ntsc_params() {
  orc::SourceParameters params;
  params.system = orc::VideoSystem::NTSC;
  params.frame_width_nominal = 8;
  params.active_video_start = 0;
  params.active_video_end = 8;
  params.first_active_frame_line = 0;
  params.last_active_frame_line = 6;
  return params;
}

orc::SourceParameters make_too_narrow_pal_params() {
  auto params = make_too_narrow_ntsc_params();
  params.system = orc::VideoSystem::PAL;
  return params;
}
}  // namespace

TEST(VideoSinkStageSafetyTest, RawModeTrigger_RejectsInvalidNtscGeometry) {
  orc::VideoSinkStage stage;
  MockObservationContext observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  EXPECT_CALL(*vfr, get_video_parameters())
      .WillRepeatedly(Return(make_too_narrow_ntsc_params()));

  const bool result =
      stage.trigger({vfr},
                    {{"output_path", std::string("ignored.y4m")},
                     {"decoder_type", std::string("ntsc2d")},
                     {"output_mode", std::string("raw")},
                     {"raw_format", std::string("y4m")}},
                    observation_context);

  EXPECT_FALSE(result);
  EXPECT_THAT(stage.get_trigger_status(),
              HasSubstr("Invalid video parameters"));
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

TEST(VideoSinkStageSafetyTest, FfmpegModeTrigger_RejectsInvalidPalGeometry) {
  orc::VideoSinkStage stage;
  MockObservationContext observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  EXPECT_CALL(*vfr, get_video_parameters())
      .WillRepeatedly(Return(make_too_narrow_pal_params()));

  const bool result =
      stage.trigger({vfr},
                    {{"output_path", std::string("ignored.mp4")},
                     {"decoder_type", std::string("pal2d")},
                     {"output_mode", std::string("ffmpeg")},
                     {"ffmpeg_format", std::string("mp4-h264")}},
                    observation_context);

  EXPECT_FALSE(result);
  EXPECT_THAT(stage.get_trigger_status(),
              HasSubstr("Invalid video parameters"));
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

// An unbounded (frame_count=0) source used to be refused outright here; it
// is now genuinely supported via a streaming export path — see
// video_sink_streaming_test.cpp for that behaviour, exercised end-to-end
// against a real (non-mock) representation. A canned mock is a poor fit for
// that test: has_unbounded_frame_range() and is_exhausted() on this file's
// NiceMock default to false regardless of each other, so nothing here can
// simulate "the source eventually ends" without effectively re-implementing
// the fake representation the dedicated test already uses — and scripting
// it wrong risks an infinite streaming loop (is_exhausted() never true)
// rather than a clean test failure.

////////////////////////////////////////////////////////////////////////////////////////////
// VideoSinkStage::supports_streaming_execution() — the ffmpeg-mode container
// check must mirror FFmpegOutputBackend::initialize()'s own fallback (see its
// comment there): a container needing to seek back and rewrite its header is
// only refused here when ffmpeg_format was explicitly requested. When it's
// only the constructor's own default (mp4-h264, never touched by the
// caller), initialize() silently substitutes nut-ffv1 instead of failing —
// so this pre-flight check must agree, or ProjectPresenter::
// validatePipeExecution() would refuse the majority case of "pipe to a
// video_sink with no --ffmpeg_format override" even though it actually
// works at runtime (this is the exact shape of a crash this project once
// shipped: the check and the runtime behaviour had silently drifted apart).
////////////////////////////////////////////////////////////////////////////////////////////

TEST(VideoSinkStageStreamingCapabilityTest, RawMode_IsAlwaysStreamable) {
  orc::VideoSinkStage stage;
  stage.set_parameters({{"output_path", std::string("-")},
                        {"output_mode", std::string("raw")},
                        {"raw_format", std::string("y4m")}});

  EXPECT_TRUE(stage.supports_streaming_execution());
}

TEST(VideoSinkStageStreamingCapabilityTest, NoOutputPath_IsNeverStreamable) {
  orc::VideoSinkStage stage;
  stage.set_parameters({{"output_mode", std::string("raw")},
                        {"raw_format", std::string("y4m")}});

  EXPECT_FALSE(stage.supports_streaming_execution());
}

// The exact scenario this project's own crash report exercised: a
// filtergraph piping to a video_sink with no --ffmpeg_format override at
// all. mp4-h264 only landed here as the constructor default, not a
// deliberate choice, so this must stay streamable.
TEST(VideoSinkStageStreamingCapabilityTest,
     FfmpegMode_UntouchedDefaultFormat_IsStreamable) {
  orc::VideoSinkStage stage;
  stage.set_parameters({{"output_path", std::string("-")},
                        {"output_mode", std::string("ffmpeg")}});

  EXPECT_TRUE(stage.supports_streaming_execution());
}

// project_to_dag() (project_to_dag.cpp) pre-fills EVERY declared parameter's
// default value — including "ffmpeg_format": "mp4-h264" — into the map
// passed to set_parameters() before overlaying whatever a project file/CLI
// graph actually specified, so the key IS present on essentially every real
// node, piped or not; this is what actually broke the scenario above the
// first time it was fixed, since a naive "was the key present" check for
// ffmpeg_format_explicit_ makes it true unconditionally. Simulates that
// exact pre-fill (the key present, holding the untouched default value) to
// pin the fix: it must be indistinguishable from the case above.
TEST(VideoSinkStageStreamingCapabilityTest,
     FfmpegMode_DefaultFormatSuppliedExplicitlyByPreFill_IsStreamable) {
  orc::VideoSinkStage stage;
  stage.set_parameters({{"output_path", std::string("-")},
                        {"output_mode", std::string("ffmpeg")},
                        {"ffmpeg_format", std::string("mp4-h264")}});

  EXPECT_TRUE(stage.supports_streaming_execution());
}

// Once the caller asks for a non-pipe-safe container OTHER than the
// constructor's own default, initialize() hard-refuses rather than silently
// substituting a different one — this must be caught here, before any real
// I/O is attempted. mov-h264 (rather than mp4-h264) is used so this is
// unambiguously a deliberate choice, not the default landing here unasked.
TEST(VideoSinkStageStreamingCapabilityTest,
     FfmpegMode_ExplicitNonPipeSafeFormat_IsNotStreamable) {
  orc::VideoSinkStage stage;
  stage.set_parameters({{"output_path", std::string("-")},
                        {"output_mode", std::string("ffmpeg")},
                        {"ffmpeg_format", std::string("mov-h264")}});

  EXPECT_FALSE(stage.supports_streaming_execution());
}

TEST(VideoSinkStageStreamingCapabilityTest,
     FfmpegMode_ExplicitPipeSafeFormat_IsStreamable) {
  orc::VideoSinkStage stage;
  stage.set_parameters({{"output_path", std::string("-")},
                        {"output_mode", std::string("ffmpeg")},
                        {"ffmpeg_format", std::string("mkv-ffv1")}});

  EXPECT_TRUE(stage.supports_streaming_execution());
}

// Chapter/disc-metadata/closed-caption embedding all gather their data
// before the first frame is written, regardless of how pipe-safe the
// container otherwise is — this overrides even an explicit pipe-safe format.
TEST(VideoSinkStageStreamingCapabilityTest,
     FfmpegMode_EmbedChapterMetadata_IsNotStreamableEvenWithSafeContainer) {
  orc::VideoSinkStage stage;
  stage.set_parameters({{"output_path", std::string("-")},
                        {"output_mode", std::string("ffmpeg")},
                        {"ffmpeg_format", std::string("mkv-ffv1")},
                        {"embed_chapter_metadata", true}});

  EXPECT_FALSE(stage.supports_streaming_execution());
}

}  // namespace orc_unit_test