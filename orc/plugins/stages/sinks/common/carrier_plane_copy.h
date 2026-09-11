/*
 * File:        carrier_plane_copy.h
 * Module:      video_sink stage (common)
 * Purpose:     Copy a decoded frame's component planes into a scope carrier
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VIDEO_SINK_CARRIER_PLANE_COPY_H
#define ORC_VIDEO_SINK_CARRIER_PLANE_COPY_H

#include <cstring>
#include <vector>

#include "decoders/componentframe.h"

namespace orc {

/**
 * @brief Flatten a decoded frame's Y, U and V planes into contiguous buffers.
 *
 * The vectorscope and histogram are fed from a carrier holding the decoder's
 * planes, and the decoder stores each plane a line at a time. Copying a line
 * at a time is the point: the element-by-element loop this replaced ran around
 * 2.1 million push_back() calls per PAL frame - one capacity test per sample -
 * to move roughly 17 MB, on a path a playing preview drives every frame.
 *
 * @p width samples are taken from each of the first @p height lines, which is
 * exactly the span the per-sample loop read.
 */
inline void copy_component_planes_to_carrier(const ComponentFrame& frame,
                                             int32_t width, int32_t height,
                                             std::vector<double>& y_plane,
                                             std::vector<double>& u_plane,
                                             std::vector<double>& v_plane) {
  if (width <= 0 || height <= 0) {
    y_plane.clear();
    u_plane.clear();
    v_plane.clear();
    return;
  }

  const std::size_t samples =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  const std::size_t row_bytes =
      static_cast<std::size_t>(width) * sizeof(double);

  y_plane.resize(samples);
  u_plane.resize(samples);
  v_plane.resize(samples);

  for (int32_t line = 0; line < height; ++line) {
    const std::size_t row =
        static_cast<std::size_t>(line) * static_cast<std::size_t>(width);
    std::memcpy(y_plane.data() + row, frame.y(line), row_bytes);
    std::memcpy(u_plane.data() + row, frame.u(line), row_bytes);
    std::memcpy(v_plane.data() + row, frame.v(line), row_bytes);
  }
}

}  // namespace orc

#endif  // ORC_VIDEO_SINK_CARRIER_PLANE_COPY_H
