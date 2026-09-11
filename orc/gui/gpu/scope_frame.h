/*
 * File:        scope_frame.h
 * Module:      orc-gui
 * Purpose:     What a scope canvas draws for one acquired frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_SCOPE_FRAME_H
#define ORC_GUI_SCOPE_FRAME_H

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QSize>
#include <array>
#include <cstdint>
#include <vector>

namespace orc::gui::gpu {

/**
 * @brief One accumulated sample: where it lands and how much it deposits.
 *
 * Positions are canvas pixels with y down, the space the scope's own plot
 * geometry works in. Which trace channel the weight reaches is a property of
 * the draw rather than of the vertex, so the same buffer can be accumulated
 * twice - once as landings and once as the beam transits between them.
 */
struct ScopeVertex {
  float x = 0.0F;
  float y = 0.0F;
  float weight = 1.0F;
};

/**
 * @brief A stretch of @ref ScopeFrame::points the beam traced without a break.
 *
 * Drawn as a line strip, so the rasteriser fills in the path between
 * consecutive samples. Runs stop where the beam is blanked over the retrace to
 * the next line and where it leaves the screen, because joining across either
 * would strike a chord the signal never traced.
 */
struct ScopeStrip {
  std::uint32_t first = 0;
  std::uint32_t count = 0;
};

/// Largest separable spread radius the map pipeline's uniform block holds.
constexpr int kMaxScopeSpreadRadius = 15;

/// How samples landing on the same canvas pixel combine.
///
/// kAdd counts them, which is what a scope that plots one point per sample
/// wants. kMax keeps the largest, which is what a scope whose vertices are
/// already per-cell counts wants when several cells fall under one pixel.
enum class ScopeBlend { kAdd, kMax };

/**
 * @brief The beam spot a plot is spread by before it is mapped.
 *
 * @ref weights holds the half-kernel, index 0 being the centre, normalised so
 * that a symmetric application sums to one. An empty kernel means no spread.
 */
struct ScopeSpotKernel {
  std::vector<float> weights;
  int radius = 0;
  double sigma = 0.0;

  bool isEmpty() const { return radius <= 0 || weights.empty(); }

  /// What the spread divides an isolated landing's peak by, which is the
  /// factor that puts a raw per-pixel count into spread units.
  float peakFraction() const {
    return weights.empty() ? 1.0F : (weights.front() * weights.front());
  }
};

/**
 * @brief How accumulated counts become brightness.
 *
 * kCount is `min(1, count * scale + bias)`, the reading both the decoded
 * vectorscope and the waveform monitor use.
 *
 * kDwell is the composite vectorscope's: intensity linear in the spread dwell,
 * normalised by an anchor that the canvas itself reduces out of the plot -
 * whichever is lower of the dwell a vector reaches landing on the same pixel
 * on every line, and the level above which half the plot's charge sits.
 */
enum class ScopeTraceMode { kCount, kDwell };

/**
 * @brief The mapping from accumulated counts to pixels.
 *
 * Brightness is `min(1, count * scale + bias)` wherever anything accumulated
 * and zero everywhere else, which is the form both CPU renderers use. The
 * colour it drives is one of three: a hue recovered from the canvas position
 * (the vectorscope's blend-colour mode), a ramp from the background to the
 * trace colour (the waveform monitor), or the trace colour scaled by
 * brightness.
 */
struct ScopeMapUniforms {
  float primary_scale = 0.0F;
  float secondary_scale = 0.0F;
  float brightness_bias = 0.0F;
  bool colorize = false;
  bool ramp_from_background = false;
  QColor trace_color = QColor(0, 255, 0);
  QColor background_color = QColor(0, 0, 0);

  /// Canvas position of U=V=0, and the scale that recovers U/V from a canvas
  /// offset. Only read when @ref colorize is set.
  QPointF canvas_centre;
  double pixels_per_uv_unit = 1.0;
  double uv_full_scale = 1.0;

  ScopeTraceMode trace_mode = ScopeTraceMode::kCount;

  // ---- kDwell only -------------------------------------------------------
  /// Intensity knob: how few samples of a line are enough to saturate.
  float gain = 1.0F;
  /// The dwell a vector reaches landing on the same pixel on every line, in
  /// spread units. Caps the reduced anchor so that the origin, where the beam
  /// rests through blanking, cannot starve the rest of the plot.
  float per_line_anchor = 0.0F;
  /// Weight of the beam-transit channel, which is held near a faint constant
  /// rather than scaled with the vectors.
  float transit_scale_dwell = 0.0F;
  /// Add the trace to what is painted behind it rather than replacing it: a
  /// graticule under the trace must not be punched out wherever the beam
  /// passed dimly.
  bool additive_composite = false;
};

/// Floats after the projection matrix in the map shader's uniform block.
constexpr int kScopeMapUniformFloats = 28;

/**
 * @brief Lay the map parameters out as the shader's uniform block expects.
 *
 * std140 packing of five vec4s, which follow the 64-byte projection matrix.
 * A free function so the packing can be checked without a graphics device.
 *
 * @param canvas_size Accumulation canvas size in pixels, which the colorize
 *                    branch needs to turn a texture coordinate back into a
 *                    canvas position.
 * @param flip_accumulation_v True when the backend's framebuffer origin is at
 *                    the bottom, so the accumulation texture has to be
 *                    sampled upside down to read back what was rendered.
 */
std::array<float, kScopeMapUniformFloats> packScopeMapUniforms(
    const ScopeMapUniforms& uniforms, QSize canvas_size,
    bool flip_accumulation_v);

/**
 * @brief Everything a scope canvas needs to draw one acquisition.
 *
 * Handed over whole rather than through a setter each: every field is decided
 * together by the renderer that produced the vertices, and a canvas holding
 * half of one frame and half of the next would draw a plot that never existed.
 */
struct ScopeFrame {
  /// Accumulation resolution. The trace is plotted here and scaled to the
  /// widget by the map pass, so it does not move when the window is resized.
  QSize canvas_size;

  std::vector<ScopeVertex> points;
  /// Runs within @ref points to trace as well as land on. Empty for a plot
  /// whose samples are not joined.
  std::vector<ScopeStrip> strips;
  ScopeBlend blend = ScopeBlend::kAdd;

  /// Beam spot the accumulation is spread by before it is mapped. Empty for a
  /// plot that reads its counts directly.
  ScopeSpotKernel spot;

  /// Painted behind the trace, and seen wherever nothing accumulated.
  QImage underlay;
  /// Painted over the trace, composited by its own alpha.
  QImage overlay;

  ScopeMapUniforms map;

  /// Fit the canvas into the widget keeping its aspect ratio (a round plot),
  /// rather than stretching it to fill (a rectangular one).
  bool preserve_aspect = true;
  /// Filter the trace when it is scaled to the widget. Off gives the blocky
  /// replication a per-cell plot wants; on gives the smooth scale a plot
  /// drawn at a fixed canvas size wants.
  bool smooth = true;
};

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_SCOPE_FRAME_H
