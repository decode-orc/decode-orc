/*
 * File:        scope_vertex_builder.cpp
 * Module:      orc-gui
 * Purpose:     Turns scope acquisitions into canvas-space vertices
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "scope_vertex_builder.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <random>

namespace orc::gui::gpu {

namespace {

/// Both renderers count a sample into the pixel its mapped position truncates
/// to, so a vertex has to sit at that pixel's centre to light the same one.
ScopeVertex pixelCentreVertex(const QPointF& point, float primary,
                              float secondary) {
  return ScopeVertex{static_cast<float>(std::floor(point.x())) + 0.5F,
                     static_cast<float>(std::floor(point.y())) + 0.5F, primary,
                     secondary};
}

/// Brightness rises 5 counts at a time at unit gain (ITU-R BT.601 norm, by
/// way of the Color Tools VirtualDub plugin the two CPU renderers took it
/// from), against an 8-bit range.
constexpr double kCountsPerStep = 5.0;
constexpr double kEightBitRange = 255.0;

}  // namespace

VectorscopeVertices buildVectorscopeVertices(
    const orc::VectorscopeData& data, const VectorscopePlotGeometry& geometry,
    const VectorscopeVertexOptions& options) {
  VectorscopeVertices out;
  out.points.reserve(data.samples.size());
  if (options.draw_trace_lines) {
    out.lines.reserve(data.samples.size() * 2);
  }

  // Same engine, same seed and same deviation as the CPU renderer, so a
  // defocused plot is the same plot on both paths.
  std::minstd_rand random_engine(12345);
  std::normal_distribution<double> normal_dist(0.0, 100.0);

  std::optional<ScopeVertex> previous;
  std::uint8_t previous_field_id = 255;
  std::uint16_t previous_line_number = 0;

  for (const orc::UVSample& sample : data.samples) {
    if (options.field_select == 1 && sample.field_id != 0) continue;
    if (options.field_select == 2 && sample.field_id != 1) continue;

    double u = sample.u;
    double v = sample.v;
    if (options.defocus) {
      u += normal_dist(random_engine);
      v += normal_dist(random_engine);
    }

    const QPointF plot_point = geometry.mapUV(u, v);
    if (!isWithinVectorscopeCanvas(plot_point, geometry.canvas_size)) {
      // The beam is off the screen; whatever it draws next is not joined to
      // where it was last seen.
      previous.reset();
      continue;
    }

    // A landing deposits on the first channel, a transit on the second, so
    // one accumulation pass carries both and the map can weigh them apart.
    const ScopeVertex vertex = pixelCentreVertex(plot_point, 1.0F, 0.0F);
    out.points.push_back(vertex);

    if (options.draw_trace_lines && previous.has_value() &&
        sample.field_id == previous_field_id &&
        sample.line_number == previous_line_number) {
      ScopeVertex from = *previous;
      ScopeVertex to = vertex;
      from.primary = 0.0F;
      from.secondary = 1.0F;
      to.primary = 0.0F;
      to.secondary = 1.0F;
      out.lines.push_back(from);
      out.lines.push_back(to);
    }

    previous = vertex;
    previous_field_id = sample.field_id;
    previous_line_number = sample.line_number;
  }

  return out;
}

ScopeMapUniforms vectorscopeDecodedMapUniforms(
    const VectorscopePlotGeometry& geometry, double gain, bool colorize) {
  ScopeMapUniforms uniforms;

  const auto scale =
      static_cast<float>((kCountsPerStep * gain) / kEightBitRange);
  uniforms.primary_scale = scale;
  // The decoded plot adds transits to landings and reads the total through
  // one formula, so the two channels carry the same weight.
  uniforms.secondary_scale = scale;
  uniforms.brightness_bias = static_cast<float>(128.0 / kEightBitRange);

  uniforms.colorize = colorize;
  uniforms.ramp_from_background = false;
  uniforms.trace_color = QColor(0, 255, 0);
  uniforms.background_color = QColor(0, 0, 0);

  uniforms.canvas_centre = geometry.centre_point;
  uniforms.pixels_per_uv_unit = geometry.pixels_per_uv_unit;
  uniforms.uv_full_scale = kVectorscopeSignedFullScale;
  return uniforms;
}

QSize waveformCanvasSize(const WaveformCountGrid& grid, QSize plot_area_size) {
  if (grid.empty() || plot_area_size.isEmpty()) {
    return QSize();
  }
  return QSize(std::min(plot_area_size.width(), grid.xSamples()),
               std::min(plot_area_size.height(), grid.yBins()));
}

std::vector<ScopeVertex> buildWaveformVertices(const WaveformCountGrid& grid,
                                               QSize canvas_size) {
  std::vector<ScopeVertex> out;
  if (grid.empty() || canvas_size.isEmpty()) {
    return out;
  }

  const double x_scale =
      static_cast<double>(canvas_size.width()) / grid.xSamples();
  const double y_scale =
      static_cast<double>(canvas_size.height()) / grid.yBins();

  for (int x = 0; x < grid.xSamples(); ++x) {
    for (int bin = 0; bin < grid.yBins(); ++bin) {
      const std::uint32_t count = grid.at(x, bin);
      if (count == 0) continue;

      // Millivolts rise up the plot, so the highest bin is the top row.
      out.push_back(ScopeVertex{
          static_cast<float>((x + 0.5) * x_scale),
          static_cast<float>(canvas_size.height() - ((bin + 0.5) * y_scale)),
          static_cast<float>(count), 0.0F});
    }
  }
  return out;
}

ScopeMapUniforms waveformMapUniforms(double gain, double brightness_bias,
                                     const QColor& background,
                                     const QColor& trace) {
  ScopeMapUniforms uniforms;
  uniforms.primary_scale =
      static_cast<float>((kCountsPerStep * gain) / kEightBitRange);
  uniforms.secondary_scale = 0.0F;
  uniforms.brightness_bias =
      static_cast<float>(brightness_bias / kEightBitRange);
  uniforms.colorize = false;
  uniforms.ramp_from_background = true;
  uniforms.trace_color = trace;
  uniforms.background_color = background;
  return uniforms;
}

}  // namespace orc::gui::gpu
