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

TEST(ScopeVertexBuilder, GivesEverySampleTheSameWeight) {
  VectorscopePlotGeometry geometry;
  orc::VectorscopeData data;
  data.samples = {makeSample(1000.0, 1000.0), makeSample(-2000.0, 500.0)};

  const VectorscopeVertices vertices =
      buildVectorscopeVertices(data, geometry, VectorscopeVertexOptions{});

  ASSERT_EQ(vertices.points.size(), 2u);
  EXPECT_FLOAT_EQ(vertices.points[0].weight, 1.0F);
  EXPECT_FLOAT_EQ(vertices.points[1].weight, 1.0F);
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

TEST(ScopeVertexBuilder, TracesConsecutiveSamplesOfOneLineOnly) {
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

  // Two runs of two, indexing the samples rather than copying them.
  ASSERT_EQ(vertices.strips.size(), 2u);
  EXPECT_EQ(vertices.strips[0].first, 0u);
  EXPECT_EQ(vertices.strips[0].count, 2u);
  EXPECT_EQ(vertices.strips[1].first, 2u);
  EXPECT_EQ(vertices.strips[1].count, 2u);
  EXPECT_EQ(vertices.points.size(), 4u);
}

// A field boundary is a blanking interval like any other, even when the line
// number happens not to change across it.
TEST(ScopeVertexBuilder, BreaksTheTraceBetweenFields) {
  VectorscopePlotGeometry geometry;
  orc::VectorscopeData data;
  data.samples = {
      makeSample(1000.0, 0.0, 0, 10), makeSample(2000.0, 0.0, 0, 10),
      makeSample(3000.0, 0.0, 1, 10), makeSample(4000.0, 0.0, 1, 10)};

  VectorscopeVertexOptions options;
  options.draw_trace_lines = true;
  const VectorscopeVertices vertices =
      buildVectorscopeVertices(data, geometry, options);

  ASSERT_EQ(vertices.strips.size(), 2u);
  EXPECT_EQ(vertices.strips[0].first, 0u);
  EXPECT_EQ(vertices.strips[1].first, 2u);
}

// A line with one sample on the plot has nothing to be joined to, and a run
// of one draws no segment at all.
TEST(ScopeVertexBuilder, EmitsNoRunForALineWithASingleSample) {
  VectorscopePlotGeometry geometry;
  orc::VectorscopeData data;
  data.samples = {makeSample(1000.0, 0.0, 0, 10),
                  makeSample(2000.0, 0.0, 0, 11),
                  makeSample(3000.0, 0.0, 0, 12)};

  VectorscopeVertexOptions options;
  options.draw_trace_lines = true;
  const VectorscopeVertices vertices =
      buildVectorscopeVertices(data, geometry, options);

  EXPECT_TRUE(vertices.strips.empty());
  EXPECT_EQ(vertices.points.size(), 3u);
}

TEST(ScopeVertexBuilder, LeavesSamplesUnjoinedWhenTraceLinesAreOff) {
  VectorscopePlotGeometry geometry;
  orc::VectorscopeData data;
  data.samples = {makeSample(1000.0, 0.0, 0, 10),
                  makeSample(2000.0, 0.0, 0, 10)};

  EXPECT_TRUE(
      buildVectorscopeVertices(data, geometry, VectorscopeVertexOptions{})
          .strips.empty());
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
  EXPECT_TRUE(buildVectorscopeVertices(data, geometry, options).strips.empty());
}

// Every run has to stay inside the samples it indexes, whatever broke it.
TEST(ScopeVertexBuilder, KeepsEveryRunInsideThePointsItIndexes) {
  VectorscopePlotGeometry geometry;
  orc::VectorscopeData data;
  for (int line = 0; line < 8; ++line) {
    for (int i = 0; i < 5; ++i) {
      const double u = (i == 3) ? 500000.0 : (1000.0 * i);
      data.samples.push_back(makeSample(u, 0.0, static_cast<std::uint8_t>(0),
                                        static_cast<std::uint16_t>(line)));
    }
  }

  VectorscopeVertexOptions options;
  options.draw_trace_lines = true;
  const VectorscopeVertices vertices =
      buildVectorscopeVertices(data, geometry, options);

  ASSERT_FALSE(vertices.strips.empty());
  for (const ScopeStrip& strip : vertices.strips) {
    EXPECT_GE(strip.count, 2u);
    EXPECT_LE(static_cast<std::size_t>(strip.first) + strip.count,
              vertices.points.size());
  }
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
  EXPECT_FLOAT_EQ(vertices[0].weight, 2.0F);
  EXPECT_FLOAT_EQ(vertices[1].weight, 1.0F);
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
// Beam spot
// ---------------------------------------------------------------------------

// The kernel is applied symmetrically, so it is the doubled tail plus the
// centre that has to sum to one - otherwise spreading a plot would change how
// much charge it holds.
TEST(ScopeVertexBuilder, NormalisesTheSpotOverItsSymmetricApplication) {
  for (int size : {256, 1024, 2048}) {
    const ScopeSpotKernel spot = vectorscopeSpotKernel(size);
    ASSERT_FALSE(spot.isEmpty()) << size;

    double total = static_cast<double>(spot.weights[0]);
    for (std::size_t t = 1; t < spot.weights.size(); ++t) {
      total += 2.0 * spot.weights[t];
    }
    EXPECT_NEAR(total, 1.0, 1e-5) << size;
    EXPECT_EQ(spot.weights.size(), static_cast<std::size_t>(spot.radius) + 1);
  }
}

TEST(ScopeVertexBuilder, SizesTheSpotToTheCanvasAndClampsItsReach) {
  const ScopeSpotKernel spot = vectorscopeSpotKernel(1024);
  EXPECT_NEAR(spot.sigma, 1024.0 / 410.0, 1e-9);
  EXPECT_EQ(spot.radius, static_cast<int>(std::ceil(3.0 * spot.sigma)));

  // The map pipeline's uniform block holds a bounded half-kernel, so a canvas
  // large enough to ask for more has to be cut back rather than overrun it.
  EXPECT_LE(vectorscopeSpotKernel(100000).radius, kMaxScopeSpreadRadius);
  EXPECT_GE(vectorscopeSpotKernel(1).radius, 1);
}

TEST(ScopeVertexBuilder, ReportsWhatTheSpotDividesAnIsolatedLandingBy) {
  const ScopeSpotKernel spot = vectorscopeSpotKernel(1024);
  EXPECT_FLOAT_EQ(spot.peakFraction(), spot.weights[0] * spot.weights[0]);
}

// ---------------------------------------------------------------------------
// Composite (dwell) map uniforms
// ---------------------------------------------------------------------------

TEST(ScopeVertexBuilder, AnchorsTheCompositePlotAsTheCpuRendererDoes) {
  const VectorscopePlotGeometry geometry;
  const ScopeSpotKernel spot = vectorscopeSpotKernel(geometry.canvas_size);
  constexpr std::uint32_t kLines = 576;
  constexpr std::uint32_t kStride = 3;
  constexpr double kGain = 4.0;

  const ScopeMapUniforms uniforms = vectorscopeCompositeMapUniforms(
      geometry, spot, kGain, false, kLines, kStride);

  EXPECT_EQ(uniforms.trace_mode, ScopeTraceMode::kDwell);
  EXPECT_TRUE(uniforms.additive_composite);
  EXPECT_FLOAT_EQ(uniforms.gain, static_cast<float>(kGain));

  // Eight samples of a line landing on one pixel is full brightness, measured
  // in the spread units the plot is read in.
  const float expected_anchor = (8.0F * kLines / kStride) * spot.peakFraction();
  EXPECT_FLOAT_EQ(uniforms.per_line_anchor, expected_anchor);

  // A pixel the beam crosses once on every line sits near this whatever the
  // vectors either end of it are doing.
  const float expected_transit =
      (static_cast<float>(kGain) * 0.04F * kStride) /
      (static_cast<float>(kLines) * spot.peakFraction());
  EXPECT_FLOAT_EQ(uniforms.transit_scale_dwell, expected_transit);
}

TEST(ScopeVertexBuilder, GivesTheCompositePlotNoAnchorWithoutLines) {
  const VectorscopePlotGeometry geometry;
  const ScopeSpotKernel spot = vectorscopeSpotKernel(geometry.canvas_size);

  const ScopeMapUniforms uniforms =
      vectorscopeCompositeMapUniforms(geometry, spot, 5.0, true, 0, 1);

  EXPECT_FLOAT_EQ(uniforms.per_line_anchor, 0.0F);
  EXPECT_FLOAT_EQ(uniforms.transit_scale_dwell, 0.0F);
  EXPECT_TRUE(uniforms.colorize);
}

// The per-line anchor is expressed in dwell, so it has to count the lines the
// beam was on even where it left the screen - otherwise moving the trace off
// the plot would brighten what is left of it.
TEST(ScopeVertexBuilder, CountsPlottedLinesIncludingThoseOffTheCanvas) {
  VectorscopePlotGeometry geometry;
  orc::VectorscopeData data;
  data.samples = {makeSample(0.0, 0.0, 0, 10), makeSample(0.0, 0.0, 0, 10),
                  makeSample(500000.0, 0.0, 0, 11), makeSample(0.0, 0.0, 0, 12),
                  makeSample(0.0, 0.0, 1, 12)};

  const VectorscopeVertices vertices =
      buildVectorscopeVertices(data, geometry, VectorscopeVertexOptions{});

  EXPECT_EQ(vertices.plotted_lines, 4u);
  EXPECT_EQ(vertices.points.size(), 4u);
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
  EXPECT_FLOAT_EQ(packed[20], 0.0F);  // count mode
  EXPECT_FLOAT_EQ(packed[24], 0.0F);  // replace, not add
}

TEST(ScopeVertexBuilder, PacksTheDwellMappingWhereTheShaderReadsIt) {
  ScopeMapUniforms uniforms;
  uniforms.trace_mode = ScopeTraceMode::kDwell;
  uniforms.gain = 6.0F;
  uniforms.per_line_anchor = 123.5F;
  uniforms.transit_scale_dwell = 0.0078F;
  uniforms.additive_composite = true;

  const std::array<float, kScopeMapUniformFloats> packed =
      packScopeMapUniforms(uniforms, QSize(1024, 1024), false);

  EXPECT_FLOAT_EQ(packed[20], 1.0F);
  EXPECT_FLOAT_EQ(packed[21], 6.0F);
  EXPECT_FLOAT_EQ(packed[22], 123.5F);
  EXPECT_FLOAT_EQ(packed[23], 0.0078F);
  EXPECT_FLOAT_EQ(packed[24], 1.0F);
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
