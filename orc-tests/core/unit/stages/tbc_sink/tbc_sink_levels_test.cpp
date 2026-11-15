/*
 * File:        tbc_sink_levels_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the TBC sink CVBS → ld-decode level mapping
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "tbc_sink_levels.h"

#include <gtest/gtest.h>

#include "tbc_capture_system.h"
#include "tbc_level_scale.h"

namespace orc_unit_test {
namespace {

using orc::VideoSystem;

// The full range of CVBS_U10_4FSC sample values, plus the headroom either side
// that the source-side map deliberately preserves.
constexpr int16_t kCvbsSweepMin = 0;
constexpr int16_t kCvbsSweepMax = 1023;

}  // namespace

// The ld-decode 16-bit domain is defined as CVBS_U10_4FSC x 64
// (cvbs_signal_constants.h), so the export must be a plain widening: any other
// factor would rescale the picture on the way out.
TEST(TbcSinkLevelsTest, CvbsToTbc_IsExactlyTimes64_ForEverySystem) {
  for (const VideoSystem sys :
       {VideoSystem::PAL, VideoSystem::NTSC, VideoSystem::PAL_M}) {
    const orc::TbcSinkLevelScale scale = orc::make_tbc_sink_level_scale(sys);
    EXPECT_DOUBLE_EQ(scale.scale, 64.0) << "system " << static_cast<int>(sys);
    EXPECT_DOUBLE_EQ(scale.offset, 0.0) << "system " << static_cast<int>(sys);

    for (int32_t cvbs = kCvbsSweepMin; cvbs <= kCvbsSweepMax; ++cvbs) {
      EXPECT_EQ(scale.to_tbc(static_cast<int16_t>(cvbs)),
                static_cast<uint16_t>(cvbs * 64))
          << "system " << static_cast<int>(sys) << " sample " << cvbs;
    }
  }
}

// tbc_source maps .tbc samples into CVBS_U10_4FSC through tbc_level_scale.h;
// re-exporting them must land back on the value that was read, so a
// source -> sink round trip leaves the video bit-identical.
TEST(TbcSinkLevelsTest, CvbsToTbc_RoundTripsTbcSourceMapping_Pal) {
  const orc::TbcLevelScale to_cvbs =
      orc::make_tbc_level_scale(orc::kTbcPalBlanking, orc::kTbcPalWhite,
                                orc::kPalBlanking, orc::kPalWhite);
  const orc::TbcSinkLevelScale to_tbc =
      orc::make_tbc_sink_level_scale(VideoSystem::PAL);

  for (int32_t cvbs = kCvbsSweepMin; cvbs <= kCvbsSweepMax; ++cvbs) {
    const auto tbc_sample = static_cast<uint16_t>(cvbs * 64);
    EXPECT_EQ(to_tbc.to_tbc(to_cvbs.map(tbc_sample)), tbc_sample)
        << "sample " << cvbs;
  }
}

TEST(TbcSinkLevelsTest, CvbsToTbc_RoundTripsTbcSourceMapping_Ntsc) {
  const orc::TbcLevelScale to_cvbs =
      orc::make_tbc_level_scale(orc::kTbcNtscBlanking, orc::kTbcNtscWhite,
                                orc::kNtscBlanking, orc::kNtscWhite);
  const orc::TbcSinkLevelScale to_tbc =
      orc::make_tbc_sink_level_scale(VideoSystem::NTSC);

  for (int32_t cvbs = kCvbsSweepMin; cvbs <= kCvbsSweepMax; ++cvbs) {
    const auto tbc_sample = static_cast<uint16_t>(cvbs * 64);
    EXPECT_EQ(to_tbc.to_tbc(to_cvbs.map(tbc_sample)), tbc_sample)
        << "sample " << cvbs;
  }
}

TEST(TbcSinkLevelsTest, CvbsToTbc_ClampsToUint16Range) {
  const orc::TbcSinkLevelScale scale =
      orc::make_tbc_sink_level_scale(VideoSystem::NTSC);
  EXPECT_EQ(scale.to_tbc(-1), 0u);
  EXPECT_EQ(scale.to_tbc(-32768), 0u);
  EXPECT_EQ(scale.to_tbc(2000), 65535u);
}

TEST(TbcSinkLevelsTest, CaptureLevels_StandardNtscKeepsSetupPedestal) {
  orc::SourceParameters params;
  params.system = VideoSystem::NTSC;
  params.blanking_level = orc::kNtscBlanking;
  params.black_level = orc::kNtscBlack;
  params.white_level = orc::kNtscWhite;

  const orc::TbcCaptureLevels levels = orc::tbc_capture_levels(params);
  EXPECT_EQ(levels.blanking_16b, orc::kTbcNtscBlanking);
  EXPECT_EQ(levels.black_16b, orc::kTbcNtscBlack);
  EXPECT_EQ(levels.white_16b, orc::kTbcNtscWhite);
}

// NTSC-J has no setup pedestal: picture black sits at the 0 IRE blanking
// level. Writing the spec pedestal instead re-labelled the export as standard
// NTSC and shifted its black level on the next read (issue #257).
TEST(TbcSinkLevelsTest, CaptureLevels_NtscJBlackLevelSurvives) {
  orc::SourceParameters params;
  params.system = VideoSystem::NTSC;
  params.blanking_level = orc::kNtscBlanking;
  params.black_level = orc::kNtscBlanking;  // 0 IRE picture black
  params.white_level = orc::kNtscWhite;
  params.has_nonstandard_values = true;

  const orc::TbcCaptureLevels levels = orc::tbc_capture_levels(params);
  EXPECT_EQ(levels.blanking_16b, orc::kTbcNtscBlanking);
  EXPECT_EQ(levels.black_16b, orc::kTbcNtscBlanking);
  EXPECT_NE(levels.black_16b, orc::kTbcNtscBlack);
  EXPECT_EQ(levels.white_16b, orc::kTbcNtscWhite);
}

TEST(TbcSinkLevelsTest, CaptureLevels_PalHasNoPedestal) {
  orc::SourceParameters params;
  params.system = VideoSystem::PAL;
  params.blanking_level = orc::kPalBlanking;
  params.black_level = orc::kPalBlack;
  params.white_level = orc::kPalWhite;

  const orc::TbcCaptureLevels levels = orc::tbc_capture_levels(params);
  EXPECT_EQ(levels.blanking_16b, orc::kTbcPalBlanking);
  EXPECT_EQ(levels.black_16b, orc::kTbcPalBlanking);
  EXPECT_EQ(levels.white_16b, orc::kTbcPalWhite);
}

TEST(TbcSinkLevelsTest, CaptureLevels_UnsetLevelsFallBackToSpecConstants) {
  orc::SourceParameters ntsc;
  ntsc.system = VideoSystem::NTSC;  // levels left at their -1 default
  const orc::TbcCaptureLevels ntsc_levels = orc::tbc_capture_levels(ntsc);
  EXPECT_EQ(ntsc_levels.blanking_16b, orc::kTbcNtscBlanking);
  EXPECT_EQ(ntsc_levels.black_16b, orc::kTbcNtscBlack);
  EXPECT_EQ(ntsc_levels.white_16b, orc::kTbcNtscWhite);

  orc::SourceParameters pal;
  pal.system = VideoSystem::PAL;
  const orc::TbcCaptureLevels pal_levels = orc::tbc_capture_levels(pal);
  EXPECT_EQ(pal_levels.blanking_16b, orc::kTbcPalBlanking);
  EXPECT_EQ(pal_levels.black_16b, orc::kTbcPalBlanking);
  EXPECT_EQ(pal_levels.white_16b, orc::kTbcPalWhite);
}

// A video_params override that moves white away from the spec level must be
// described in the capture row rather than normalised out of the samples.
TEST(TbcSinkLevelsTest, CaptureLevels_NonSpecWhiteIsCarriedThrough) {
  orc::SourceParameters params;
  params.system = VideoSystem::PAL;
  params.blanking_level = orc::kPalBlanking;
  params.black_level = orc::kPalBlack;
  params.white_level = 900;

  const orc::TbcCaptureLevels levels = orc::tbc_capture_levels(params);
  EXPECT_EQ(levels.white_16b, 900 * 64);
}

// capture.system takes ld-decode's spelling: its CHECK admits only 'NTSC',
// 'PAL' and 'PAL_M', so the display name "PAL-M" fails every PAL_M export.
TEST(TbcCaptureSystemTest, UsesLdDecodeSpelling) {
  EXPECT_STREQ(orc::tbc_capture_system_name(VideoSystem::PAL), "PAL");
  EXPECT_STREQ(orc::tbc_capture_system_name(VideoSystem::NTSC), "NTSC");
  EXPECT_STREQ(orc::tbc_capture_system_name(VideoSystem::PAL_M), "PAL_M");
  EXPECT_EQ(orc::tbc_capture_system_name(VideoSystem::Unknown), nullptr);
}

}  // namespace orc_unit_test
