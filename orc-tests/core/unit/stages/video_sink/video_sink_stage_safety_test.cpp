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

}  // namespace orc_unit_test