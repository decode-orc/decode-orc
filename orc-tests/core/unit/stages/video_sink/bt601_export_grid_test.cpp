/*
 * File:        bt601_export_grid_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the BT.601 13.5 MHz export grid geometry
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/sinks/common/bt601_export_grid.h"

#include <gtest/gtest.h>

namespace orc_unit_test {

// ITU-R BT.656: for 625-line, 0H sits at sample 732 of 864, so BT.601 output
// column 0 is 132 samples (9.778 us) after 0H; the 720-sample window is
// 53.333 us long. On the PAL 4fsc grid that is 173 .. 173 + 946.
TEST(Bt601ExportGridTest, SourceWindowFor625Line) {
  const auto window = orc::resolve_bt601_source_window(orc::VideoSystem::PAL);

  EXPECT_EQ(window.active_video_start, 173);
  EXPECT_EQ(window.active_video_end - window.active_video_start, 946);
}

// 525-line: 0H at sample 736 of 858, so column 0 is 122 samples (9.037 us)
// after 0H; 53.333 us is 764 samples on the NTSC 4fsc grid.
TEST(Bt601ExportGridTest, SourceWindowFor525Line) {
  const auto window = orc::resolve_bt601_source_window(orc::VideoSystem::NTSC);

  EXPECT_EQ(window.active_video_start, 129);
  EXPECT_EQ(window.active_video_end - window.active_video_start, 764);
}

// PAL-M shares the 525-line raster and line period, but not the sample rate.
TEST(Bt601ExportGridTest, SourceWindowForPalM) {
  const auto window = orc::resolve_bt601_source_window(orc::VideoSystem::PAL_M);

  EXPECT_EQ(window.active_video_start, 129);
  EXPECT_EQ(window.active_video_end - window.active_video_start, 763);
}

// Every window must fit inside its line, with room for the decoder's filter
// margin at each end (video_parameter_safety.h allows 7 samples for PAL).
TEST(Bt601ExportGridTest, SourceWindowsFitInsideTheLine) {
  const auto pal = orc::resolve_bt601_source_window(orc::VideoSystem::PAL);
  EXPECT_GE(pal.active_video_start, 7);
  EXPECT_LE(pal.active_video_end, orc::kPalSamplesPerLineNominal - 7);

  const auto ntsc = orc::resolve_bt601_source_window(orc::VideoSystem::NTSC);
  EXPECT_GT(ntsc.active_video_start, 0);
  EXPECT_LE(ntsc.active_video_end, orc::kNtscSamplesPerLine);

  const auto palm = orc::resolve_bt601_source_window(orc::VideoSystem::PAL_M);
  EXPECT_GT(palm.active_video_start, 0);
  EXPECT_LE(palm.active_video_end, orc::kPalMSamplesPerLine);
}

// ITU-R BT.1700: the 625-line analogue active line is 64 - 12 = 52 us, which
// is 702 pixels at 13.5 MHz; the 525-line one is 52.6556 us, or 711 pixels.
TEST(Bt601ExportGridTest, NominalActiveLineIs702For625Line) {
  const auto grid = orc::resolve_bt601_export_grid(orc::VideoSystem::PAL, 576);

  EXPECT_EQ(grid.frame_width, 720);
  EXPECT_EQ(grid.frame_height, 576);
  EXPECT_EQ(grid.nominal_active_width, 702);
}

TEST(Bt601ExportGridTest, NominalActiveLineIs711For525Line) {
  const auto grid = orc::resolve_bt601_export_grid(orc::VideoSystem::NTSC, 486);

  EXPECT_EQ(grid.nominal_active_width, 711);
}

// SAR = (4/3) x (height / active line): 702 pixels over 576 lines -> 128:117,
// within 0.2% of the conventional 59:54.
TEST(Bt601ExportGridTest, SampleAspectRatioIsDerivedFromTheActiveLine) {
  const auto grid = orc::resolve_bt601_export_grid(orc::VideoSystem::PAL, 576);

  EXPECT_EQ(grid.sar_num, 128);
  EXPECT_EQ(grid.sar_den, 117);
}

// The chain must be horizontal-only: the scale keeps the source height, and
// there is nothing else in it that could touch a field.
TEST(Bt601ExportGridTest, FilterChainIsHorizontalOnly) {
  const auto grid = orc::resolve_bt601_export_grid(orc::VideoSystem::PAL, 576);
  const std::string chain = orc::build_bt601_filter_chain(grid, 946, 946);

  EXPECT_EQ(chain,
            "scale=720:576:flags=lanczos+full_chroma_int+accurate_rnd"
            ",format=yuv444p16le");
}

// An odd source window is rounded up to even before the filter graph, and
// that blanking column must be cropped rather than resampled into the picture.
TEST(Bt601ExportGridTest, FilterChainCropsTheEvenWidthPadding) {
  const auto grid =
      orc::resolve_bt601_export_grid(orc::VideoSystem::PAL_M, 486);
  const std::string chain = orc::build_bt601_filter_chain(grid, 763, 764);

  EXPECT_EQ(chain.find("crop=763:486:0:0,"), 0u);
}

TEST(Bt601ExportGridTest, FormatPredicateMatchesOnlyTheBt601Format) {
  EXPECT_TRUE(orc::is_bt601_export_format("mkv-ffv1-bt601"));
  EXPECT_FALSE(orc::is_bt601_export_format("mkv-ffv1"));
  EXPECT_FALSE(orc::is_bt601_export_format("mp4-h264"));
}

}  // namespace orc_unit_test
