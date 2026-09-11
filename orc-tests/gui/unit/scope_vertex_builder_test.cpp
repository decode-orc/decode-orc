/*
 * File:        scope_vertex_builder_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests for the scope canvas vertex and uniform builders
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "gpu/scope_vertex_builder.h"

#include <gtest/gtest.h>

#include <cmath>

#include "gpu/scope_frame.h"

namespace orc::gui::gpu {
namespace {

orc::UVSample makeSample(double u, double v, std::uint8_t field_id = 0,
                         std::uint16_t line_number = 0) {
  orc::UVSample sample;
  sample.u = u;
  sample.v = v;
  sample.field_id = field_id;
  sample.line_number = line_number;
  return sample;
}

// ---------------------------------------------------------------------------
// Vectorscope vertices
// ---------------------------------------------------------------------------

// The canvas has to light the pixel the CPU renderer counts in, so the vertex
// is checked against that renderer's own mapping rather than against a
// transcription of it.
TEST(ScopeVertexBuilder, PlacesSamplesOnThePixelTheCpuRendererWouldCount) {
  VectorscopePlotGeometry geometry;
  orc::VectorscopeData data;
  data.samples = {makeSample(0.0, 0.0), makeSample(12000.0, -8000.0),
                  makeSample(-20000.0, 25000.0)};

  const VectorscopeVertices vertices =
      buildVectorscopeVertices(data, geometry, VectorscopeVertexOptions{});

  ASSERT_EQ(vertices.points.size(), data.samples.size());
  for (std::size_t i = 0; i < data.samples.size(); ++i) {
    const QPointF expected =
        geometry.mapUV(data.samples[i].u, data.samples[i].v);
    EXPECT_EQ(static_cast<int>(vertices.points[i].x),
              static_cast<int>(expected.x()));
    EXPECT_EQ(static_cast<int>(vertices.points[i].y),
              static_cast<int>(expected.y()));
    // Rasterised as a point, the vertex must sit inside the pixel rather than
    // on the boundary between two of them.
    EXPECT_FLOAT_EQ(vertices.points[i].x - std::floor(vertices.points[i].x),
                    0.5F);
    EXPECT_FLOAT_EQ(vertices.points[i].y - std::floor(vertices.points[i].y),
                    0.5F);
  }
}

TEST(ScopeVertexBuilder, CountsALandingOnTheFirstChannelOnly) {
  VectorscopePlotGeometry geometry;
  orc::VectorscopeData data;
  data.samples = {makeSample(1000.0, 1000.0)};

  const VectorscopeVertices vertices =
      buildVectorscopeVertices(data, geometry, VectorscopeVertexOptions{});

  ASSERT_EQ(vertices.points.size(), 1u);
  EXPECT_FLOAT_EQ(vertices.points[0].primary, 1.0F);
  EXPECT_FLOAT_EQ(vertices.points[0].secondary, 0.0F);
}

TEST(ScopeVertexBuilder, DropsSamplesThatLandOffTheCanvas) {
  VectorscopePlotGeometry geometry;
  orc::VectorscopeData data;
  // Far beyond signed full scale in both axes.
  data.samples = {makeSample(500000.0, 0.0), makeSample(0.0, 0.0),
                  makeSample(0.0, -500000.0)};

  const VectorscopeVertices vertices =
      buildVectorscopeVertices(data, geometry, VectorscopeVertexOptions{});

  EXPECT_EQ(vertices.points.size(), 1u);
}

TEST(ScopeVertexBuilder, PlotsOnlyTheSelectedField) {
  VectorscopePlotGeometry geometry;
  orc::VectorscopeData data;
  data.samples = {makeSample(1000.0, 0.0, 0), makeSample(2000.0, 0.0, 1),
                  makeSample(3000.0, 0.0, 0)};

  VectorscopeVertexOptions first_only;
  first_only.field_select = 1;
  EXPECT_EQ(buildVectorscopeVertices(data, geometry, first_only).points.size(),
            2u);

  VectorscopeVertexOptions second_only;
  second_only.field_select = 2;
  EXPECT_EQ(buildVectorscopeVertices(data, geometry, second_only).points.size(),
            1u);

  EXPECT_EQ(buildVectorscopeVertices(data, geometry, VectorscopeVertexOptions{})
                .points.size(),
            3u);
}

TEST(ScopeVertexBuilder, JoinsConsecutiveSamplesOfOneLineOnly) {
  VectorscopePlotGeometry geometry;
  orc::VectorscopeData data;
  data.samples = {
      makeSample(1000.0, 0.0, 0, 10),
      makeSample(2000.0, 0.0, 0, 10),   // joined to the one before
      makeSample(3000.0, 0.0, 0, 11),   // new line: the beam was blanked
      makeSample(4000.0, 0.0, 0, 11)};  // joined to the one before

  VectorscopeVertexOptions options;
  options.draw_trace_lines = true;
  const VectorscopeVertices vertices =
      buildVectorscopeVertices(data, geometry, options);

  // Two segments, two vertices each.
  ASSERT_EQ(vertices.lines.size(), 4u);
  for (const ScopeVertex& vertex : vertices.lines) {
    EXPECT_FLOAT_EQ(vertex.primary, 0.0F);
    EXPECT_FLOAT_EQ(vertex.secondary, 1.0F);
  }
  EXPECT_FLOAT_EQ(vertices.lines[0].x, vertices.points[0].x);
  EXPECT_FLOAT_EQ(vertices.lines[1].x, vertices.points[1].x);
}

TEST(ScopeVertexBuilder, LeavesSamplesUnjoinedWhenTraceLinesAreOff) {
  VectorscopePlotGeometry geometry;
  orc::VectorscopeData data;
  data.samples = {makeSample(1000.0, 0.0, 0, 10),
                  makeSample(2000.0, 0.0, 0, 10)};

  EXPECT_TRUE(
      buildVectorscopeVertices(data, geometry, VectorscopeVertexOptions{})
          .lines.empty());
}

// A sample the beam lost the screen for breaks the trace: the next one it is
// seen on is somewhere else entirely, and joining the two would draw a chord
// the signal never traced.
TEST(ScopeVertexBuilder, DoesNotJoinAcrossASampleThatLeftTheCanvas) {
  VectorscopePlotGeometry geometry;
  orc::VectorscopeData data;
  data.samples = {makeSample(1000.0, 0.0, 0, 10),
                  makeSample(500000.0, 0.0, 0, 10),
                  makeSample(2000.0, 0.0, 0, 10)};

  VectorscopeVertexOptions options;
  options.draw_trace_lines = true;
  EXPECT_TRUE(buildVectorscopeVertices(data, geometry, options).lines.empty());
}

TEST(ScopeVertexBuilder, ScattersDefocusedSamplesReproducibly) {
  VectorscopePlotGeometry geometry;
  orc::VectorscopeData data;
  for (int i = 0; i < 32; ++i) {
    data.samples.push_back(makeSample(0.0, 0.0));
  }

  VectorscopeVertexOptions options;
  options.defocus = true;
  const VectorscopeVertices first =
      buildVectorscopeVertices(data, geometry, options);
  const VectorscopeVertices again =
      buildVectorscopeVertices(data, geometry, options);

  ASSERT_EQ(first.points.size(), again.points.size());
  bool moved_off_centre = false;
  for (std::size_t i = 0; i < first.points.size(); ++i) {
    EXPECT_FLOAT_EQ(first.points[i].x, again.points[i].x);
    EXPECT_FLOAT_EQ(first.points[i].y, again.points[i].y);
    if (first.points[i].x != first.points[0].x) {
      moved_off_centre = true;
    }
  }
  EXPECT_TRUE(moved_off_centre);
}

// ---------------------------------------------------------------------------
// Waveform vertices
// ---------------------------------------------------------------------------

TEST(ScopeVertexBuilder, KeepsTheWaveformCanvasNoFinerThanTheCountGrid) {
  WaveformCountGrid grid;
  grid.reset(100, 500);

  EXPECT_EQ(waveformCanvasSize(grid, QSize(800, 400)), QSize(100, 400));
  EXPECT_EQ(waveformCanvasSize(grid, QSize(50, 900)), QSize(50, 500));
  EXPECT_EQ(waveformCanvasSize(grid, QSize(0, 400)), QSize());
}

TEST(ScopeVertexBuilder, EmitsOneVertexPerOccupiedCellCarryingItsCount) {
  WaveformCountGrid grid;
  grid.reset(4, 4);
  grid.increment(1, 2);
  grid.increment(1, 2);
  grid.increment(3, 0);

  const std::vector<ScopeVertex> vertices =
      buildWaveformVertices(grid, QSize(4, 4));

  ASSERT_EQ(vertices.size(), 2u);
  EXPECT_FLOAT_EQ(vertices[0].primary, 2.0F);
  EXPECT_FLOAT_EQ(vertices[1].primary, 1.0F);
  EXPECT_FLOAT_EQ(vertices[0].secondary, 0.0F);
}

// Millivolts rise up the plot while canvas rows count down it, so the lowest
// bin has to land on the bottom row and the highest on the top one.
TEST(ScopeVertexBuilder, PutsTheLowestBinAtTheBottomOfTheCanvas) {
  WaveformCountGrid grid;
  grid.reset(2, 4);
  grid.increment(0, 0);
  grid.increment(1, 3);

  const std::vector<ScopeVertex> vertices =
      buildWaveformVertices(grid, QSize(2, 4));

  ASSERT_EQ(vertices.size(), 2u);
  EXPECT_FLOAT_EQ(vertices[0].y, 3.5F);
  EXPECT_FLOAT_EQ(vertices[1].y, 0.5F);
  EXPECT_FLOAT_EQ(vertices[0].x, 0.5F);
  EXPECT_FLOAT_EQ(vertices[1].x, 1.5F);
}

// With more bins than canvas rows several cells share a row; each still has to
// land on the row the CPU renderer would have reduced it into.
TEST(ScopeVertexBuilder, LandsEveryCellOnTheRowTheCpuRendererReducesItInto) {
  constexpr int kBins = 12;
  constexpr int kRows = 5;
  WaveformCountGrid grid;
  grid.reset(1, kBins);
  for (int bin = 0; bin < kBins; ++bin) {
    grid.increment(0, bin);
  }

  const std::vector<ScopeVertex> vertices =
      buildWaveformVertices(grid, QSize(1, kRows));
  ASSERT_EQ(vertices.size(), static_cast<std::size_t>(kBins));

  for (int bin = 0; bin < kBins; ++bin) {
    const int row = static_cast<int>(vertices[static_cast<std::size_t>(bin)].y);
    // The renderer reduces the cells [lo, hi] of each row; the cell must fall
    // in the row whose span contains it.
    const int lo =
        static_cast<int>(static_cast<double>(kRows - row - 1) / kRows * kBins);
    const int hi = std::min(
        static_cast<int>(static_cast<double>(kRows - row) / kRows * kBins),
        kBins - 1);
    EXPECT_GE(bin, lo) << "bin " << bin << " row " << row;
    EXPECT_LE(bin, hi) << "bin " << bin << " row " << row;
  }
}

TEST(ScopeVertexBuilder, EmitsNothingForAnEmptyGrid) {
  WaveformCountGrid grid;
  EXPECT_TRUE(buildWaveformVertices(grid, QSize(10, 10)).empty());

  grid.reset(4, 4);
  EXPECT_TRUE(buildWaveformVertices(grid, QSize(4, 4)).empty());
}

// ---------------------------------------------------------------------------
// Map uniforms
// ---------------------------------------------------------------------------

// Both CPU renderers reach full brightness after (255 - bias) / (5 * gain)
// counts; the canvas must saturate at the same count.
TEST(ScopeVertexBuilder, SaturatesAtTheSameCountAsTheDecodedCpuRenderer) {
  const VectorscopePlotGeometry geometry;
  for (double gain : {1.0, 3.0, 10.0}) {
    const ScopeMapUniforms uniforms =
        vectorscopeDecodedMapUniforms(geometry, gain, false);
    for (std::uint32_t count = 0; count < 80; ++count) {
      const float shader =
          std::min(1.0F, static_cast<float>(count) * uniforms.primary_scale +
                             uniforms.brightness_bias);
      const float renderer = std::min(
          1.0F, (static_cast<float>(count) * 5.0F * static_cast<float>(gain) +
                 128.0F) /
                    255.0F);
      EXPECT_NEAR(shader, renderer, 1e-6F)
          << "gain " << gain << " count " << count;
    }
  }
}

TEST(ScopeVertexBuilder, WeighsVectorscopeTransitsAsLandings) {
  const VectorscopePlotGeometry geometry;
  const ScopeMapUniforms uniforms =
      vectorscopeDecodedMapUniforms(geometry, 4.0, true);

  EXPECT_FLOAT_EQ(uniforms.secondary_scale, uniforms.primary_scale);
  EXPECT_TRUE(uniforms.colorize);
  EXPECT_FALSE(uniforms.ramp_from_background);
  EXPECT_EQ(uniforms.canvas_centre, geometry.centre_point);
  EXPECT_DOUBLE_EQ(uniforms.pixels_per_uv_unit, geometry.pixels_per_uv_unit);
  EXPECT_DOUBLE_EQ(uniforms.uv_full_scale, kVectorscopeSignedFullScale);
}

TEST(ScopeVertexBuilder, SaturatesAtTheSameCountAsTheWaveformCpuRenderer) {
  for (double gain : {0.5, 1.0, 10.0}) {
    const ScopeMapUniforms uniforms =
        waveformMapUniforms(gain, 64.0, QColor(10, 20, 30), QColor(0, 230, 0));
    for (std::uint32_t count = 0; count < 120; ++count) {
      const float shader =
          std::min(1.0F, static_cast<float>(count) * uniforms.primary_scale +
                             uniforms.brightness_bias);
      const float renderer = std::min(
          1.0F, (static_cast<float>(count) * 5.0F * static_cast<float>(gain) +
                 64.0F) /
                    255.0F);
      EXPECT_NEAR(shader, renderer, 1e-6F)
          << "gain " << gain << " count " << count;
    }
    EXPECT_FLOAT_EQ(uniforms.secondary_scale, 0.0F);
    EXPECT_TRUE(uniforms.ramp_from_background);
    EXPECT_FALSE(uniforms.colorize);
  }
}

// ---------------------------------------------------------------------------
// Uniform packing
// ---------------------------------------------------------------------------

TEST(ScopeVertexBuilder, PacksMapUniformsWhereTheShaderReadsThem) {
  ScopeMapUniforms uniforms;
  uniforms.primary_scale = 0.25F;
  uniforms.secondary_scale = 0.125F;
  uniforms.brightness_bias = 0.5F;
  uniforms.colorize = true;
  uniforms.ramp_from_background = true;
  uniforms.trace_color = QColor(255, 0, 0);
  uniforms.background_color = QColor(0, 0, 255);
  uniforms.canvas_centre = QPointF(512.0, 511.0);
  uniforms.pixels_per_uv_unit = 0.0155;
  uniforms.uv_full_scale = 32768.0;

  const std::array<float, kScopeMapUniformFloats> packed =
      packScopeMapUniforms(uniforms, QSize(1024, 512), true);

  EXPECT_FLOAT_EQ(packed[0], 1.0F);  // trace red
  EXPECT_FLOAT_EQ(packed[1], 0.0F);
  EXPECT_FLOAT_EQ(packed[3], 1.0F);  // trace alpha
  EXPECT_FLOAT_EQ(packed[6], 1.0F);  // background blue
  EXPECT_FLOAT_EQ(packed[8], 0.25F);
  EXPECT_FLOAT_EQ(packed[9], 0.125F);
  EXPECT_FLOAT_EQ(packed[10], 0.5F);
  EXPECT_FLOAT_EQ(packed[11], 1.0F);  // colorize
  EXPECT_FLOAT_EQ(packed[12], 1.0F);  // ramp
  EXPECT_FLOAT_EQ(packed[13], 0.0155F);
  EXPECT_FLOAT_EQ(packed[14], 32768.0F);
  EXPECT_FLOAT_EQ(packed[15], 1.0F);  // flip
  EXPECT_FLOAT_EQ(packed[16], 512.0F);
  EXPECT_FLOAT_EQ(packed[17], 511.0F);
  EXPECT_FLOAT_EQ(packed[18], 1024.0F);
  EXPECT_FLOAT_EQ(packed[19], 512.0F);
}

TEST(ScopeVertexBuilder, PacksFlagsOffWhenTheyAreNotSet) {
  ScopeMapUniforms uniforms;
  const std::array<float, kScopeMapUniformFloats> packed =
      packScopeMapUniforms(uniforms, QSize(8, 8), false);

  EXPECT_FLOAT_EQ(packed[11], 0.0F);  // colorize
  EXPECT_FLOAT_EQ(packed[12], 0.0F);  // ramp
  EXPECT_FLOAT_EQ(packed[15], 0.0F);  // flip
}

}  // namespace
}  // namespace orc::gui::gpu
