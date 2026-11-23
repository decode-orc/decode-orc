/*
 * File:        mame_export_grid_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the MAME LaserDisc export frame geometry
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/sinks/common/mame_export_grid.h"

#include <gtest/gtest.h>

#include <string>

namespace orc_unit_test {

namespace {

// The source frame-flat line written to output row `row`, or -1 for a black
// pad row.
int source_line_for_row(const orc::MameExportGrid& grid, int row) {
  if (row < grid.top_pad_rows) return -1;
  return grid.first_frame_line + row - grid.top_pad_rows;
}

// MAME chdman splits a frame into fields by row parity (even rows first) and
// vbiparse reads field row n as field line n: row 2n + field must carry
// field line n of that field.  Frame-flat line L is field L % 2, 0-based
// field line index L / 2, i.e. field line L / 2 + 1.
void expect_chdman_reads_field_line(const orc::MameExportGrid& grid, int field,
                                    int field_line) {
  const int row = 2 * field_line + field;
  const int line = source_line_for_row(grid, row);
  ASSERT_GE(line, 0) << "row " << row << " is a pad row";
  EXPECT_EQ(line % 2, field) << "row " << row;
  EXPECT_EQ(line / 2 + 1, field_line) << "row " << row;
}

}  // namespace

TEST(MameExportGridTest, RecognisesOnlyTheMameFormat) {
  EXPECT_TRUE(orc::is_mame_export_format("avi-huffyuv-mame"));
  EXPECT_FALSE(orc::is_mame_export_format("mkv-ffv1-bt601"));
  EXPECT_FALSE(orc::is_mame_export_format("avi-huffyuv"));
}

// chdman stores VBI metadata only for 262-row fields on 525-line material.
TEST(MameExportGridTest, NtscFrameIs720x524) {
  const auto grid = orc::resolve_mame_export_grid(orc::VideoSystem::NTSC);

  EXPECT_EQ(grid.frame_width, 720);
  EXPECT_EQ(grid.frame_height, 524);
  EXPECT_EQ(grid.top_pad_rows, 2);
  EXPECT_EQ(grid.first_frame_line, 0);
  EXPECT_EQ(grid.last_frame_line, 522);
  EXPECT_EQ(grid.bt601.frame_height, 522);
}

// The 625-line field height chdman accepts is 312 rows.
TEST(MameExportGridTest, PalFrameIs720x624) {
  const auto grid = orc::resolve_mame_export_grid(orc::VideoSystem::PAL);

  EXPECT_EQ(grid.frame_width, 720);
  EXPECT_EQ(grid.frame_height, 624);
  EXPECT_EQ(grid.last_frame_line, 622);
}

// PAL-M is a 525-line raster and SECAM a 625-line one.
TEST(MameExportGridTest, FrameHeightFollowsTheLineStandard) {
  EXPECT_EQ(orc::resolve_mame_export_grid(orc::VideoSystem::PAL_M).frame_height,
            524);
  EXPECT_EQ(orc::resolve_mame_export_grid(orc::VideoSystem::SECAM).frame_height,
            624);
}

// IEC 60857 §10.2.4 white flag on line 11; Philips codes on lines 16-18 of
// each field.  These are the rows MAME's vbiparse reads them from.
TEST(MameExportGridTest, NtscVbiLinesSitOnTheRowsChdmanReads) {
  const auto grid = orc::resolve_mame_export_grid(orc::VideoSystem::NTSC);
  for (int field = 0; field < 2; ++field) {
    for (int field_line : {11, 16, 17, 18}) {
      expect_chdman_reads_field_line(grid, field, field_line);
    }
  }
}

TEST(MameExportGridTest, PalVbiLinesSitOnTheRowsChdmanReads) {
  const auto grid = orc::resolve_mame_export_grid(orc::VideoSystem::PAL);
  for (int field = 0; field < 2; ++field) {
    for (int field_line : {16, 17, 18}) {
      expect_chdman_reads_field_line(grid, field, field_line);
    }
  }
}

// Every row below the pad follows the same rule, so the picture is one
// continuous raster with the VBI: row 44, where MAME starts showing the
// picture, is field line 22.
TEST(MameExportGridTest, EveryRowIsFieldLineHalfRowOfFieldRowParity) {
  for (const auto system : {orc::VideoSystem::NTSC, orc::VideoSystem::PAL}) {
    const auto grid = orc::resolve_mame_export_grid(system);
    EXPECT_EQ(source_line_for_row(grid, 0), -1);
    EXPECT_EQ(source_line_for_row(grid, 1), -1);
    for (int row = grid.top_pad_rows; row < grid.frame_height; ++row) {
      expect_chdman_reads_field_line(grid, row % 2, row / 2);
    }
  }
}

// The pixels are those of the VP415e formats, so is the pixel shape.
TEST(MameExportGridTest, SampleAspectRatioMatchesTheVp415eFormats) {
  const auto ntsc = orc::resolve_mame_export_grid(orc::VideoSystem::NTSC);
  const auto ntsc_bt601 =
      orc::resolve_bt601_export_grid(orc::VideoSystem::NTSC, 486);
  EXPECT_EQ(ntsc.sar_num, ntsc_bt601.sar_num);
  EXPECT_EQ(ntsc.sar_den, ntsc_bt601.sar_den);

  const auto pal = orc::resolve_mame_export_grid(orc::VideoSystem::PAL);
  EXPECT_EQ(pal.sar_num, 128);
  EXPECT_EQ(pal.sar_den, 117);
}

// The chain resamples the decoded lines horizontally, then pads the black
// rows on top to reach chdman's frame height.
TEST(MameExportGridTest, FilterChainResamplesThenPadsTheTopRows) {
  const auto grid = orc::resolve_mame_export_grid(orc::VideoSystem::NTSC);
  const std::string chain = orc::build_mame_filter_chain(grid, 764, 764);

  EXPECT_NE(chain.find("scale=720:522:"), std::string::npos) << chain;
  const auto pad = chain.find("pad=720:524:0:2:black");
  ASSERT_NE(pad, std::string::npos) << chain;
  EXPECT_LT(chain.find("scale="), pad) << chain;
}

}  // namespace orc_unit_test
