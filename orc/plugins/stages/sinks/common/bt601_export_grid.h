/*
 * File:        bt601_export_grid.h
 * Module:      orc-stage-plugin-video-sink
 * Purpose:     ITU-R BT.601 13.5 MHz export grid geometry for the
 *              "FFV1 for VP415e" preset
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_PLUGIN_VIDEO_SINK_BT601_EXPORT_GRID_H
#define ORC_STAGE_PLUGIN_VIDEO_SINK_BT601_EXPORT_GRID_H

#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <cmath>
#include <numeric>
#include <string>

namespace orc {

// The "FFV1 for VP415e" preset writes lossless FFV1 on the ITU-R BT.601
// 13.5 MHz sampling grid instead of the 4fsc grid every other preset uses.
// It exists for players that drive real BT.601 hardware (the Philips VP415
// LaserDisc player emulator being the motivating case): the 4fsc -> 13.5 MHz
// resample is done once here, at export, rather than on every frame of every
// playback.
//
// Everything below is derived from ITU-R BT.601 and BT.656 rather than
// hard-coded per standard, so the 625- and 525-line paths cannot drift apart.
// The header is pure and free of FFmpeg types so it can be unit tested.
inline constexpr const char* kBt601FFV1Format = "mkv-ffv1-bt601";

// True for the export format that targets the BT.601 grid.
inline bool is_bt601_export_format(const std::string& format) {
  return format == kBt601FFV1Format;
}

// ITU-R BT.601 §1: 13.5 MHz luma sampling for both line standards.
inline constexpr double kBt601SampleRateHz = 13500000.0;

// ITU-R BT.601: the digital active line is 720 samples for both standards.
inline constexpr int kBt601FrameWidth = 720;

// ITU-R BT.656: position of the 0H timing datum within the digital line, from
// which the offset of output column 0 from 0H follows.
//   625-line: 864 samples per line, 0H at sample 732 -> column 0 is 132
//             samples (9.778 us) after 0H
//   525-line: 858 samples per line, 0H at sample 736 -> column 0 is 122
//             samples (9.037 us) after 0H
inline constexpr int kBt601ZeroToActive625 = 132;
inline constexpr int kBt601ZeroToActive525 = 122;

// ITU-R BT.1700 Table 1: the analogue active line, used only to derive the
// sample aspect ratio.
//   625-line: 64 us line, 12 us blanking      -> 52.0000 us -> 702 pixels
//   525-line: 63.5556 us line, 10.9 us blank  -> 52.6556 us -> 711 pixels
inline constexpr double kActiveLineNs625 = 52000.0;
inline constexpr double kActiveLineNs525 = 52655.6;

// The 4fsc window that carries the BT.601 720-sample digital active line.
struct Bt601SourceWindow {
  int active_video_start = 0;  // 4fsc sample under BT.601 output column 0
  int active_video_end = 0;    // One past the last, i.e. under column 720
};

// Resolves that window from the video system alone: both grids are anchored
// to the same physical thing, the 0H timing datum, so the mapping is defined
// in time.  Column 0 sits kBt601ZeroToActive* samples after 0H at 13.5 MHz,
// and the window is 720 samples (53.333 us) long.
//
// The export takes the whole 720 from source rather than isolating the 702-
// pixel analogue active line and padding the rest with digital black, which
// is what Rec. 601 expects of an analogue-sourced picture: the margins carry
// the source's own blanking, nothing is cropped, and a source whose timing is
// slightly off neither loses picture nor rings against a synthetic edge.
inline Bt601SourceWindow resolve_bt601_source_window(VideoSystem system) {
  // PAL-M shares the 525-line raster and line period; only true 625-line PAL
  // uses the 64 us line.
  const bool is_625_line = (system == VideoSystem::PAL);
  const double source_rate_hz =
      is_625_line
          ? kPalSampleRate
          : (system == VideoSystem::PAL_M ? kPalMSampleRate : kNtscSampleRate);
  const double samples_per_output_pixel = source_rate_hz / kBt601SampleRateHz;
  const double zero_to_column0 =
      (is_625_line ? kBt601ZeroToActive625 : kBt601ZeroToActive525) *
      samples_per_output_pixel;

  Bt601SourceWindow window;
  window.active_video_start = static_cast<int>(std::lround(zero_to_column0));
  window.active_video_end = window.active_video_start +
                            static_cast<int>(std::lround(
                                kBt601FrameWidth * samples_per_output_pixel));
  return window;
}

// Geometry of one BT.601 export frame.
struct Bt601ExportGrid {
  int frame_width = kBt601FrameWidth;  // Always 720
  int frame_height = 0;          // Unchanged: the conversion is horizontal only
  int nominal_active_width = 0;  // The analogue active line on the 13.5 MHz
                                 // grid (702 for 625-line, 711 for 525-line)
  int sar_num = 0;               // Sample aspect ratio for a 4:3 picture
  int sar_den = 1;
};

inline Bt601ExportGrid resolve_bt601_export_grid(VideoSystem system,
                                                 int frame_height) {
  const bool is_625_line = (system == VideoSystem::PAL);
  const double active_line_ns =
      is_625_line ? kActiveLineNs625 : kActiveLineNs525;

  Bt601ExportGrid grid;
  grid.frame_height = frame_height;
  grid.nominal_active_width =
      static_cast<int>(std::lround(active_line_ns * kBt601SampleRateHz / 1e9));

  // The analogue active line is 4:3 (BT.1700 §1), so the sample aspect ratio
  // that makes it display correctly is
  //   SAR = (4/3) x (height / active line in pixels)
  // giving 128:117 for 625-line, within 0.2% of the conventional 59:54.  It
  // follows the active line rather than the whole 720, which is what makes
  // the blanking margins display as margins instead of squeezing the picture.
  const long long num = 4LL * frame_height;
  const long long den = 3LL * grid.nominal_active_width;
  const long long divisor = std::gcd(num, den);
  if (divisor > 0) {
    grid.sar_num = static_cast<int>(num / divisor);
    grid.sar_den = static_cast<int>(den / divisor);
  }
  return grid;
}

// Builds the FFmpeg filter chain that resamples the source window onto the
// BT.601 grid.  The chain is deliberately horizontal-only:
//
//   crop   drops the blanking column the encoder path adds when the source
//          window is an odd number of samples wide, so the resample sees the
//          window and nothing else
//   scale  maps the window - a time window, not a whole number of 4fsc
//          samples - onto 720 pixels, band-limited (Lanczos-3).  The vertical
//          ratio is 1:1, which swscale resolves to a unit tap, so no vertical
//          filtering happens and the interlaced field structure survives
//   format pins the resample to 4:4:4 16 bit, so the reduction to the
//          encoder's 4:2:2 pixel format (appended by the caller) happens
//          after it rather than inside it
//
// src_width is the source window; padded_width is the width of the frame
// actually handed to the filter graph, which the encoder path rounds up to
// even.
inline std::string build_bt601_filter_chain(const Bt601ExportGrid& grid,
                                            int src_width, int padded_width) {
  std::string chain;
  if (padded_width > src_width) {
    chain += "crop=" + std::to_string(src_width) + ":" +
             std::to_string(grid.frame_height) + ":0:0,";
  }
  chain += "scale=" + std::to_string(grid.frame_width) + ":" +
           std::to_string(grid.frame_height) +
           ":flags=lanczos+full_chroma_int+accurate_rnd";
  chain += ",format=yuv444p16le";
  return chain;
}

}  // namespace orc

#endif  // ORC_STAGE_PLUGIN_VIDEO_SINK_BT601_EXPORT_GRID_H
