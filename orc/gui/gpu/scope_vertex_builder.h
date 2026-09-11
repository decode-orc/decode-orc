/*
 * File:        scope_vertex_builder.h
 * Module:      orc-gui
 * Purpose:     Turns scope acquisitions into canvas-space vertices
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_SCOPE_VERTEX_BUILDER_H
#define ORC_GUI_SCOPE_VERTEX_BUILDER_H

#include <orc/stage/preview/orc_vectorscope.h>

#include <QColor>
#include <QSize>
#include <cstdint>
#include <vector>

#include "../preview/vectorscope_geometry.h"
#include "../waveform_count_grid.h"
#include "scope_frame.h"

namespace orc::gui::gpu {

/// The vectorscope display controls that decide which samples are plotted and
/// where. Everything else about the plot is a map uniform.
struct VectorscopeVertexOptions {
  /// 0 both fields, 1 first only, 2 second only - the dialogue's button ids.
  int field_select = 0;
  /// Scatter each sample, the way an unfocused beam would.
  bool defocus = false;
  /// Join consecutive samples of a line, the way a continuous beam does.
  bool draw_trace_lines = false;
};

struct VectorscopeVertices {
  std::vector<ScopeVertex> points;
  /// Consecutive pairs, each the transit between two samples of one line.
  std::vector<ScopeVertex> lines;
};

/**
 * @brief Place every plotted sample on the canvas.
 *
 * Positions come from @p geometry.mapUV, the same mapping the CPU renderer
 * uses, and land on the centre of the pixel that renderer would have counted
 * in - so a sample lights the same canvas pixel on both paths.
 *
 * Samples that fall off the canvas are dropped, and they break the trace:
 * the beam has left the screen, so the next sample it comes back on is not
 * joined to the last one it was on.
 *
 * Defocus is a fixed Gaussian scatter with a fixed seed, so the same
 * acquisition always produces the same plot.
 */
VectorscopeVertices buildVectorscopeVertices(
    const orc::VectorscopeData& data, const VectorscopePlotGeometry& geometry,
    const VectorscopeVertexOptions& options);

/**
 * @brief Brightness and colour mapping for the decoded-component plot.
 *
 * `min(1, (count * 5 * gain + 128) / 255)`, which is the CPU renderer's
 * formula, with transits counted alongside landings as that renderer counts
 * them.
 *
 * @param gain          Point-size spin box value, 1-10.
 * @param colorize      Blend-colour checkbox: hue from the canvas position
 *                      rather than a flat green trace.
 */
ScopeMapUniforms vectorscopeDecodedMapUniforms(
    const VectorscopePlotGeometry& geometry, double gain, bool colorize);

/**
 * @brief Accumulation canvas for a waveform plot area.
 *
 * Never larger than the count grid in either axis: one vertex per occupied
 * cell can only light the pixel it lands on, so a canvas finer than the grid
 * would leave gaps between cells. Scaling the canvas up to the plot area
 * without filtering then reproduces the cell replication the CPU renderer
 * does.
 */
QSize waveformCanvasSize(const WaveformCountGrid& grid, QSize plot_area_size);

/**
 * @brief Place every occupied grid cell on the canvas.
 *
 * One vertex per cell carrying that cell's count, to be accumulated with
 * ScopeBlend::kMax: where several cells fall under one canvas pixel the
 * largest wins, which is the area-max reduction the CPU renderer performs.
 */
std::vector<ScopeVertex> buildWaveformVertices(const WaveformCountGrid& grid,
                                               QSize canvas_size);

/**
 * @brief Brightness and colour mapping for the waveform plot.
 *
 * `min(1, (count * 5 * gain + bias) / 255)` ramped from the background colour
 * to the trace colour, which is the CPU renderer's formula.
 */
ScopeMapUniforms waveformMapUniforms(double gain, double brightness_bias,
                                     const QColor& background,
                                     const QColor& trace);

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_SCOPE_VERTEX_BUILDER_H
