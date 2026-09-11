/*
 * File:        scope_frame.cpp
 * Module:      orc-gui
 * Purpose:     What a scope canvas draws for one acquired frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "scope_frame.h"

namespace orc::gui::gpu {

std::array<float, kScopeMapUniformFloats> packScopeMapUniforms(
    const ScopeMapUniforms& uniforms, QSize canvas_size,
    bool flip_accumulation_v) {
  std::array<float, kScopeMapUniformFloats> packed{};

  // vec4 trace_color
  packed[0] = static_cast<float>(uniforms.trace_color.redF());
  packed[1] = static_cast<float>(uniforms.trace_color.greenF());
  packed[2] = static_cast<float>(uniforms.trace_color.blueF());
  packed[3] = static_cast<float>(uniforms.trace_color.alphaF());

  // vec4 background_color
  packed[4] = static_cast<float>(uniforms.background_color.redF());
  packed[5] = static_cast<float>(uniforms.background_color.greenF());
  packed[6] = static_cast<float>(uniforms.background_color.blueF());
  packed[7] = static_cast<float>(uniforms.background_color.alphaF());

  // vec4 scales: primary, secondary, bias, colorize
  packed[8] = uniforms.primary_scale;
  packed[9] = uniforms.secondary_scale;
  packed[10] = uniforms.brightness_bias;
  packed[11] = uniforms.colorize ? 1.0F : 0.0F;

  // vec4 params: ramp, pixels per U/V unit, U/V full scale, flip
  packed[12] = uniforms.ramp_from_background ? 1.0F : 0.0F;
  packed[13] = static_cast<float>(uniforms.pixels_per_uv_unit);
  packed[14] = static_cast<float>(uniforms.uv_full_scale);
  packed[15] = flip_accumulation_v ? 1.0F : 0.0F;

  // vec4 centre: canvas position of U=V=0, then the canvas size
  packed[16] = static_cast<float>(uniforms.canvas_centre.x());
  packed[17] = static_cast<float>(uniforms.canvas_centre.y());
  packed[18] = static_cast<float>(canvas_size.width());
  packed[19] = static_cast<float>(canvas_size.height());

  // vec4 dwell: mode, gain, the per-line anchor cap, the transit weight
  packed[20] = uniforms.trace_mode == ScopeTraceMode::kDwell ? 1.0F : 0.0F;
  packed[21] = uniforms.gain;
  packed[22] = uniforms.per_line_anchor;
  packed[23] = uniforms.transit_scale_dwell;

  // vec4 composition
  packed[24] = uniforms.additive_composite ? 1.0F : 0.0F;
  packed[25] = 0.0F;
  packed[26] = 0.0F;
  packed[27] = 0.0F;

  return packed;
}

}  // namespace orc::gui::gpu
