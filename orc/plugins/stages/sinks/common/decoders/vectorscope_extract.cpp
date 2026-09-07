/*
 * File:        vectorscope_extract.cpp
 * Module:      chroma-decoder
 * Purpose:     Extract vectorscope U/V samples from decoded ComponentFrames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "vectorscope_extract.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <cstddef>

#include "componentframe.h"

namespace {

double clamp_normalized(double value) {
  if (value > 1.0) {
    return 1.0;
  }
  if (value < -1.0) {
    return -1.0;
  }
  return value;
}

}  // namespace

namespace orc {

VectorscopeData extract_vectorscope_from_component_frame(
    const ::ComponentFrame& frame, const SourceParameters& video_parameters,
    uint64_t field_number, uint32_t subsample) {
  VectorscopeData data;
  const int32_t width = frame.getWidth();
  const int32_t height = frame.getHeight();
  data.field_number = field_number;

  if (width == 0 || height == 0 || subsample == 0) {
    return data;
  }

  int32_t x_start = 0;
  int32_t x_end = width;
  int32_t y_start = 0;
  int32_t y_end = height;

  if (video_parameters.active_area_cropping_applied) {
    // The comb decoder wrote U/V with relative 0-based indexing: row 0 maps to
    // first_active_frame_line, col 0 maps to active_video_start.  Read the
    // active picture as a contiguous block starting at the frame origin.
    // orc_source_parameters.h §active_area_cropping_applied.
    const int32_t active_w =
        video_parameters.active_video_end - video_parameters.active_video_start;
    const int32_t active_h = video_parameters.last_active_frame_line -
                             video_parameters.first_active_frame_line;
    if (active_w > 0 && active_w <= width && active_h > 0 &&
        active_h <= height) {
      x_start = 0;
      x_end = active_w;
      y_start = 0;
      y_end = active_h;
    }
  } else {
    if (video_parameters.active_video_start >= 0 &&
        video_parameters.active_video_end >
            video_parameters.active_video_start &&
        video_parameters.active_video_end <= width) {
      x_start = video_parameters.active_video_start;
      x_end = video_parameters.active_video_end;
    }

    if (video_parameters.first_active_frame_line >= 0 &&
        video_parameters.last_active_frame_line >
            video_parameters.first_active_frame_line &&
        video_parameters.last_active_frame_line <= height) {
      y_start = video_parameters.first_active_frame_line;
      y_end = video_parameters.last_active_frame_line;
    }
  }

  data.width = static_cast<uint32_t>(x_end - x_start);
  data.height = static_cast<uint32_t>(y_end - y_start);

  // Reserve space for samples from the active picture area only.
  const size_t active_width = static_cast<size_t>(x_end - x_start);
  const size_t active_height = static_cast<size_t>(y_end - y_start);
  size_t estimated_samples =
      (active_width / subsample) * (active_height / subsample);
  data.samples.reserve(estimated_samples);

  // Normalise CVBS-domain U/V to the ±32767 scale expected by UVSample.
  // ComponentFrame U/V planes carry values in the CVBS decoder domain;
  // dividing by the picture excursion (black→white) maps them to
  // approximately ±1, consistent with render_preview_from_colour_carrier.
  //
  // Black, not blanking: the setup pedestal shortens the excursion that the
  // encoding equation scales chroma by, so a 7.5 IRE setup system carries one
  // unit of U/V in 92.5 IRE.  Dividing by that recovers the same normalised
  // colour difference from every system — the 0.925 of SMPTE 170M-2004 §10
  // included, and correctly absent on the systems without a pedestal (PAL,
  // and NTSC-J, whose picture black tbc_source reports at blanking).  The
  // decoded-component graticule targets assume this normalisation; a
  // composite acquisition measures against blanking instead and carries the
  // factor in its targets (see measurementTargetUv()).
  const double level_range =
      std::max(1.0, static_cast<double>(video_parameters.white_level -
                                        video_parameters.black_level));

  // Row 0 of a cropped frame is first_active_frame_line of the uncropped one.
  // UVSample::line_number is an interlaced frame-line index whichever way the
  // decoder wrote the plane, so a cropped row is reported at the line it came
  // from — and that line's parity, not the row's, says which field it is on.
  const int32_t line_offset = video_parameters.active_area_cropping_applied
                                  ? video_parameters.first_active_frame_line
                                  : 0;

  // Process both fields separately
  // Field 0 (first/odd field): even lines (0, 2, 4, ...)
  // Field 1 (second/even field): odd lines (1, 3, 5, ...)
  for (uint8_t field_id = 0; field_id < 2; field_id++) {
    int32_t first_y = y_start;
    if (((first_y + line_offset) & 1) != field_id) {
      ++first_y;
    }

    // Process every (2 * subsample)th line starting from field_id
    for (int32_t y = first_y; y < y_end;
         y += (2 * static_cast<int32_t>(subsample))) {
      const double* uLine = frame.u(y);
      const double* vLine = frame.v(y);

      for (int32_t x = x_start; x < x_end;
           x += static_cast<int32_t>(subsample)) {
        UVSample uv;
        uv.u = clamp_normalized(uLine[x] / level_range) * 32767.0;
        uv.v = clamp_normalized(vLine[x] / level_range) * 32767.0;
        uv.field_id = field_id;  // Tag which field this sample came from
        // The line a sample came from: the renderer joins consecutive samples
        // into a trace and must break it at each line rather than striking a
        // chord back across the plot.
        uv.line_number = static_cast<uint16_t>(y + line_offset);
        data.samples.push_back(uv);
      }
    }
  }

  ORC_LOG_DEBUG(
      "Extracted {} native U/V samples from ComponentFrame field {} (active "
      "{}x{} within {}x{}, subsample={}, both fields)",
      data.samples.size(), field_number, data.width, data.height, width, height,
      subsample);

  return data;
}

}  // namespace orc
