/*
 * File:        vectorscope_analysis.cpp
 * Module:      orc-core
 * Purpose:     Vectorscope samples from a colour preview carrier
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "vectorscope_analysis.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <cstddef>
#include <utility>

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

VectorscopeData extract_vectorscope_from_colour_frame(
    const ColourFrameCarrier& carrier, uint64_t field_number,
    uint32_t subsample, bool active_area_only, uint32_t first_line,
    uint32_t last_line) {
  VectorscopeData data;
  data.field_number = field_number;
  data.system = carrier.system;
  // CVBS_U10_4FSC normative levels from carrier (set by chroma_sink).
  data.cvbs_white = static_cast<int32_t>(carrier.cvbs_white);
  data.cvbs_blanking = static_cast<int32_t>(carrier.cvbs_blanking);

  if (!carrier.is_valid() || subsample == 0) {
    return data;
  }

  uint32_t x_start = 0;
  uint32_t x_end = carrier.width;
  uint32_t y_start = 0;
  uint32_t y_end = carrier.height;

  if (active_area_only) {
    if (carrier.active_x_end > carrier.active_x_start &&
        carrier.active_x_end <= carrier.width) {
      x_start = carrier.active_x_start;
      x_end = carrier.active_x_end;
    }

    if (carrier.active_y_end > carrier.active_y_start &&
        carrier.active_y_end <= carrier.height) {
      y_start = carrier.active_y_start;
      y_end = carrier.active_y_end;
    }
  }

  // Explicit line range, in the interlaced frame-line numbering both
  // acquisitions use.  It narrows whatever the active-picture restriction
  // left rather than replacing it, so the two controls compose.
  {
    uint32_t range_first = first_line;
    uint32_t range_last = (last_line == 0) ? (carrier.height - 1) : last_line;
    if (range_first > range_last) std::swap(range_first, range_last);
    y_start = std::max(y_start, range_first);
    y_end = std::min(y_end, range_last + 1);
  }

  if (y_end <= y_start || x_end <= x_start) {
    return data;
  }

  data.width = x_end - x_start;
  data.height = y_end - y_start;
  data.first_line = y_start;
  data.last_line = y_end - 1;

  const size_t sample_width = static_cast<size_t>(x_end - x_start);
  const size_t sample_height = static_cast<size_t>(y_end - y_start);
  const size_t estimated_samples =
      (sample_width / subsample) * (sample_height / subsample);
  data.samples.reserve(estimated_samples);

  // Normalise CVBS-domain U/V to the ±32767 scale expected by UVSample.
  const double uv_range =
      std::max(1.0, carrier.cvbs_white - carrier.cvbs_blanking);

  for (uint8_t field_id = 0; field_id < 2; ++field_id) {
    uint32_t first_y = y_start;
    if ((first_y & 1U) != field_id) {
      ++first_y;
    }

    for (uint32_t y = first_y; y < y_end; y += (2 * subsample)) {
      const size_t line_offset =
          static_cast<size_t>(y) * static_cast<size_t>(carrier.width);

      for (uint32_t x = x_start; x < x_end; x += subsample) {
        const size_t sample_index = line_offset + static_cast<size_t>(x);
        UVSample uv;
        uv.u = clamp_normalized(carrier.u_plane[sample_index] / uv_range) *
               32767.0;
        uv.v = clamp_normalized(carrier.v_plane[sample_index] / uv_range) *
               32767.0;
        uv.field_id = field_id;
        // The line a sample came from: the renderer joins consecutive
        // samples into a trace and must break it at each line rather than
        // striking a chord back across the plot.
        uv.line_number = static_cast<uint16_t>(y);
        data.samples.push_back(uv);
      }
    }
  }

  ORC_LOG_DEBUG(
      "Extracted {} U/V samples from colour preview carrier field {} ({} area "
      "{}x{} within {}x{}, lines {}..{}, subsample={}, both fields)",
      data.samples.size(), field_number, active_area_only ? "active" : "full",
      data.width, data.height, carrier.width, carrier.height, data.first_line,
      data.last_line, subsample);

  return data;
}

}  // namespace orc
