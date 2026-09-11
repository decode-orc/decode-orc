/*
 * File:        frame_plane_uniforms.cpp
 * Module:      orc-gui
 * Purpose:     Uniform packing for the frame surface's plane conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "frame_plane_uniforms.h"

#include <algorithm>

namespace orc::gui::gpu {

namespace {

/// Guard against a payload that would divide by zero. A zero excursion means
/// the levels never reached the carrier, and every sample then maps to the
/// same output whatever the reciprocal is.
float reciprocalOr(float value, float fallback) {
  return value != 0.0F ? 1.0F / value : fallback;
}

}  // namespace

int frameTransferLutRows(int entries) {
  if (entries <= 0) {
    return 0;
  }
  return (entries + kFrameTransferLutWidth - 1) / kFrameTransferLutWidth;
}

std::vector<float> packFrameTransferLut(const std::vector<float>& lut) {
  if (lut.empty()) {
    return {};
  }
  const int rows = frameTransferLutRows(static_cast<int>(lut.size()));
  std::vector<float> packed(
      static_cast<size_t>(rows) * static_cast<size_t>(kFrameTransferLutWidth),
      lut.back());
  std::copy(lut.begin(), lut.end(), packed.begin());
  return packed;
}

std::array<float, kFramePlaneUniformFloats> packSignalPlaneUniforms(
    const orc::PreviewPlanes& planes) {
  std::array<float, kFramePlaneUniformFloats> out{};

  if (planes.apply_level_scaling) {
    out[0] = planes.black_level;
    out[1] = planes.white_level - planes.black_level;
  } else {
    // Raw: the full analogue signal range (-300 mV to 1000 mV) spans the
    // display range, which is what makes sync and the back porch visible.
    out[0] = planes.sync_tip_level;
    out[1] = planes.peak_level - planes.sync_tip_level;
  }

  return out;
}

std::array<float, kFramePlaneUniformFloats> packFramePlaneUniforms(
    const orc::PreviewPlanes& planes) {
  std::array<float, kFramePlaneUniformFloats> out{};

  const float kg = 1.0F - planes.matrix_kr - planes.matrix_kb;

  out[0] = planes.cvbs_black;
  out[1] = reciprocalOr(planes.y_range, 1.0F);
  out[2] = reciprocalOr(planes.uv_range * planes.composite_u, 1.0F);
  out[3] = reciprocalOr(planes.uv_range * planes.composite_v, 1.0F);

  out[4] = planes.matrix_kr;
  out[5] = planes.matrix_kb;
  out[6] = reciprocalOr(kg, 1.0F);
  out[7] = 0.0F;

  const int entries =
      planes.transfer_lut ? static_cast<int>(planes.transfer_lut->size()) : 0;
  out[8] = static_cast<float>(kFrameTransferLutWidth);
  out[9] = static_cast<float>(frameTransferLutRows(entries));
  // The table is sampled by interpolating between neighbouring nodes, so the
  // domain [0, 1] is divided into one fewer interval than there are nodes -
  // the same count TransferLut::encode() scales by.
  out[10] = static_cast<float>(entries > 0 ? entries - 1 : 0);
  out[11] = 0.0F;

  return out;
}

}  // namespace orc::gui::gpu
