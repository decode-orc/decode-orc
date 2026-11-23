/*
 * File:        mame_export_grid.h
 * Module:      orc-stage-plugin-video-sink
 * Purpose:     Frame geometry of the MAME LaserDisc export (avi-huffyuv-mame),
 *              the AVI that MAME's chdman createld turns into a LaserDisc CHD
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_PLUGIN_VIDEO_SINK_MAME_EXPORT_GRID_H
#define ORC_STAGE_PLUGIN_VIDEO_SINK_MAME_EXPORT_GRID_H

#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <string>

#include "bt601_export_grid.h"

namespace orc {

// MAME plays a LaserDisc from a CHD, which its chdman tool builds from an AVI
// ("chdman createld -i side.avi -o side.chd").  chdman does not take the
// disc's VBI from a sidecar: it reads the white flag and the Philips codes out
// of the picture rows of the AVI, once, and stores them in the CHD, which is
// what MAME seeks with at play time.  The AVI therefore has to carry the lines
// above the picture, on exactly the rows chdman reads, and this header is the
// geometry that puts them there.  The format is specified in
// docs/technical/laserdisc-emulation-formats.md.
//
// What MAME (0.287) requires, from src/tools/chdman.cpp,
// src/lib/util/vbiparse.cpp and src/devices/machine/laserdsc.cpp:
//  - chdman splits each frame into fields by row parity, even rows first,
//    and stores VBI metadata only when a field is 262 rows (525-line) or
//    312 rows (625-line) tall; without it MAME refuses to play the disc
//  - vbiparse reads field row 11 for the white flag and rows 16, 17 and 18
//    for the Philips codes, so field row n must hold field line n
//  - laserdsc weaves the two fields back into the same rows and shows rows
//    44 onwards, 8 pixels in from each side
//
// So row R of the frame is field line R / 2 of field R % 2, field 0 being
// the first field (the one carrying the CAV picture number).  Field line 0
// does not exist, so rows 0 and 1 are black.  The horizontal grid is the
// BT.601 720-sample digital active line of the VP415e formats, which is also
// the 720 pixels MAME's laserdisc device assumes.
//
// The 525-line geometry is what MAME's NTSC games use.  The 625-line one
// follows the same rules, which chdman and vbiparse apply unchanged to
// 312-row fields, but no MAME driver plays a PAL disc yet, so it has not been
// checked against MAME's display.
inline constexpr const char* kMameFormat = "avi-huffyuv-mame";

// True for the MAME LaserDisc export.
inline bool is_mame_export_format(const std::string& format) {
  return format == kMameFormat;
}

// MAME chdman: the field heights for which VBI metadata is stored.
inline constexpr int kMameFieldRows525 = 262;
inline constexpr int kMameFieldRows625 = 312;

// Field line 0 does not exist, so the two rows that would carry it (one per
// field) are written black.
inline constexpr int kMameTopPadRows = 2;

// Picture lines used for the sample aspect ratio, as the VP415e formats use
// them (SMPTE 125M 486 lines on 525-line, ITU-R BT.601 576 on 625-line), so
// the two exports signal the same pixel shape.
inline constexpr int kMameAspectLines525 = 486;
inline constexpr int kMameAspectLines625 = 576;

// Geometry of one MAME export frame.
struct MameExportGrid {
  int frame_width = kBt601FrameWidth;  // Always 720
  int frame_height = 0;                // 524 (525-line) or 624 (625-line)
  int top_pad_rows = kMameTopPadRows;  // Black rows above field line 1
  // Frame-flat source lines decoded into rows top_pad_rows onwards:
  // [first_frame_line, last_frame_line).  Frame-flat line L is 0-based field
  // line index L / 2 of field L % 2, i.e. field line L / 2 + 1.
  int first_frame_line = 0;
  int last_frame_line = 0;
  // BT.601 grid of the decoded lines, for the horizontal resample.
  Bt601ExportGrid bt601;
  int sar_num = 0;
  int sar_den = 1;
};

inline MameExportGrid resolve_mame_export_grid(VideoSystem system) {
  // PAL-M shares the 525-line raster; PAL and SECAM use the 625-line one.
  const bool line_625 = is_625_line(system);
  const int field_rows = line_625 ? kMameFieldRows625 : kMameFieldRows525;

  MameExportGrid grid;
  grid.frame_height = 2 * field_rows;
  grid.first_frame_line = 0;
  grid.last_frame_line = grid.frame_height - grid.top_pad_rows;
  grid.bt601 = resolve_bt601_export_grid(
      system, grid.last_frame_line - grid.first_frame_line);

  const Bt601ExportGrid aspect = resolve_bt601_export_grid(
      system, line_625 ? kMameAspectLines625 : kMameAspectLines525);
  grid.sar_num = aspect.sar_num;
  grid.sar_den = aspect.sar_den;
  return grid;
}

// Builds the FFmpeg filter chain for the MAME export: the BT.601 horizontal
// resample of the decoded lines (see build_bt601_filter_chain()), then the
// black rows above field line 1.  Nothing is filtered vertically, so the
// field structure, and with it the rows chdman reads the VBI from, survives.
inline std::string build_mame_filter_chain(const MameExportGrid& grid,
                                           int src_width, int padded_width) {
  std::string chain =
      build_bt601_filter_chain(grid.bt601, src_width, padded_width);
  chain += ",pad=" + std::to_string(grid.frame_width) + ":" +
           std::to_string(grid.frame_height) +
           ":0:" + std::to_string(grid.top_pad_rows) + ":black";
  return chain;
}

}  // namespace orc

#endif  // ORC_STAGE_PLUGIN_VIDEO_SINK_MAME_EXPORT_GRID_H
