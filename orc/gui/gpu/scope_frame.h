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
#include <vector>

namespace orc::gui::gpu {

/**
 * @brief One accumulated sample: where it lands and what it deposits.
 *
 * Positions are canvas pixels with y down, the space the scope's own plot
 * geometry works in. The two weights are separate trace channels, so a scope
 * that plots two things at once - the vectorscope's landings and the beam
 * transits between them - accumulates both in a single pass.
 */
struct ScopeVertex {
  float x = 0.0F;
  float y = 0.0F;
  float primary = 0.0F;
  float secondary = 0.0F;
};

/// How samples landing on the same canvas pixel combine.
///
/// kAdd counts them, which is what a scope that plots one point per sample
/// wants. kMax keeps the largest, which is what a scope whose vertices are
/// already per-cell counts wants when several cells fall under one pixel.
enum class ScopeBlend { kAdd, kMax };

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
};

/// Floats after the projection matrix in the map shader's uniform block.
constexpr int kScopeMapUniformFloats = 20;

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
  /// Consecutive pairs, each a segment the rasteriser fills in.
  std::vector<ScopeVertex> lines;
  ScopeBlend blend = ScopeBlend::kAdd;

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
