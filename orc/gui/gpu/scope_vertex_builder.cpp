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
#include <random>

namespace orc::gui::gpu {

namespace {

/// Both renderers count a sample into the pixel its mapped position truncates
/// to, so a vertex has to sit at that pixel's centre to light the same one.
ScopeVertex pixelCentreVertex(const QPointF& point) {
  return ScopeVertex{static_cast<float>(std::floor(point.x())) + 0.5F,
                     static_cast<float>(std::floor(point.y())) + 0.5F, 1.0F};
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

  // Same engine, same seed and same deviation as the CPU renderer, so a
  // defocused plot is the same plot on both paths.
  std::minstd_rand random_engine(12345);
  std::normal_distribution<double> normal_dist(0.0, 100.0);

  std::uint8_t counted_field_id = 255;
  std::uint16_t counted_line_number = 0;

  bool tracing = false;
  std::uint8_t previous_field_id = 255;
  std::uint16_t previous_line_number = 0;

  // The run being traced, closed off whenever the beam is blanked or leaves
  // the screen.
  const auto close_run = [&out, &tracing]() {
    if (!tracing) {
      return;
    }
    tracing = false;
    // A run of one sample is a landing with nothing to join it to.
    if (!out.strips.empty() && out.strips.back().count < 2) {
      out.strips.pop_back();
    }
  };

  for (const orc::UVSample& sample : data.samples) {
    if (options.field_select == 1 && sample.field_id != 0) continue;
    if (options.field_select == 2 && sample.field_id != 1) continue;

    double u = sample.u;
    double v = sample.v;
    if (options.defocus) {
      u += normal_dist(random_engine);
      v += normal_dist(random_engine);
    }

    // Samples arrive line by line, so a change of line is a new line. Counted
    // before the canvas test: a line the beam left the screen on is still a
    // line of signal that was plotted.
    if (sample.field_id != counted_field_id ||
        sample.line_number != counted_line_number || out.plotted_lines == 0) {
      counted_field_id = sample.field_id;
      counted_line_number = sample.line_number;
      ++out.plotted_lines;
    }

    const QPointF plot_point = geometry.mapUV(u, v);
    if (!isWithinVectorscopeCanvas(plot_point, geometry.canvas_size)) {
      // The beam is off the screen; whatever it draws next is not joined to
      // where it was last seen.
      close_run();
      continue;
    }

    const auto index = static_cast<std::uint32_t>(out.points.size());
    out.points.push_back(pixelCentreVertex(plot_point));

    if (options.draw_trace_lines) {
      // The beam is continuous along a line and blanked over the retrace to
      // the next one, so a change of line starts a new run.
      const bool joined = tracing && sample.field_id == previous_field_id &&
                          sample.line_number == previous_line_number;
      if (joined) {
        ++out.strips.back().count;
      } else {
        close_run();
        out.strips.push_back(ScopeStrip{index, 1});
        tracing = true;
      }
    }

    previous_field_id = sample.field_id;
    previous_line_number = sample.line_number;
  }
  close_run();

  return out;
}

ScopeSpotKernel vectorscopeSpotKernel(int canvas_size) {
  ScopeSpotKernel kernel;
  // A quarter of a per cent of the plot diameter across. Without a spot a
  // vector that never moves lands on a single pixel, which at any sensible
  // display scale is smaller than the graticule mark it is read against.
  kernel.sigma = static_cast<double>(canvas_size) / 410.0;
  kernel.radius = std::clamp(static_cast<int>(std::ceil(3.0 * kernel.sigma)), 1,
                             kMaxScopeSpreadRadius);

  kernel.weights.assign(static_cast<std::size_t>(kernel.radius) + 1, 0.0F);
  double sum = 0.0;
  for (int t = 0; t <= kernel.radius; ++t) {
    const double weight =
        std::exp(-0.5 * (t * t) / (kernel.sigma * kernel.sigma));
    kernel.weights[static_cast<std::size_t>(t)] = static_cast<float>(weight);
    sum += (t == 0) ? weight : (2.0 * weight);
  }
  for (float& weight : kernel.weights) {
    weight = static_cast<float>(weight / sum);
  }
  return kernel;
}

ScopeMapUniforms vectorscopeCompositeMapUniforms(
    const VectorscopePlotGeometry& geometry, const ScopeSpotKernel& spot,
    double gain, bool colorize, std::uint32_t plotted_lines,
    std::uint32_t sample_stride) {
  ScopeMapUniforms uniforms;
  uniforms.trace_mode = ScopeTraceMode::kDwell;
  uniforms.gain = static_cast<float>(gain);
  uniforms.additive_composite = true;

  const auto stride = static_cast<float>(std::max(sample_stride, 1U));
  const auto lines = static_cast<float>(plotted_lines);
  const float spot_peak = spot.peakFraction();

  // A colour-bar vector is tens of samples a line; the transit between two of
  // them is one. Dividing by the lines plotted and the stride turns a hit
  // count into samples of a line, a figure that does not move when the line
  // range or the field selection change.
  constexpr float kSaturationSamplesPerLine = 8.0F;
  uniforms.per_line_anchor =
      (plotted_lines > 0)
          ? ((kSaturationSamplesPerLine * lines / stride) * spot_peak)
          : 0.0F;

  // The beam moving at full speed reads as a faint constant however far it
  // has to travel; scaling it with the vectors would let a frame of colour
  // bars wash the plot out with the ink it lays down joining them.
  constexpr float kTransitBrightnessAtUnitGain = 0.04F;
  uniforms.transit_scale_dwell =
      (plotted_lines > 0) ? ((static_cast<float>(gain) *
                              kTransitBrightnessAtUnitGain * stride) /
                             (lines * spot_peak))
                          : 0.0F;

  uniforms.colorize = colorize;
  uniforms.trace_color = QColor(0, 255, 0);
  uniforms.background_color = QColor(0, 0, 0);
  uniforms.canvas_centre = geometry.centre_point;
  uniforms.pixels_per_uv_unit = geometry.pixels_per_uv_unit;
  uniforms.uv_full_scale = kVectorscopeSignedFullScale;
  return uniforms;
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
          static_cast<float>(count)});
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
