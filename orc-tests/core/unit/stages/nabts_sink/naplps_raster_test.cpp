/*
 * File:        naplps_raster_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the pel-accurate NAPLPS raster core
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_raster.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "nabts_raster_view.h"
#include "naplps_font.h"

namespace orc {
namespace {

// A grid small enough to reason about cell by cell. 32 * 0,78125 = 25, so a
// cell is square in unit space exactly as the shipped grids' are, and one cell
// is 1/32 of the unit screen along both axes — which makes a unit coordinate
// and a cell coordinate the same number over 32.
constexpr NaplpsRenderGrid kTestGrid{32, 25};
constexpr double kCell = 1.0 / 32.0;

/// A unit-screen point at cell-space |column|, |row|.
NabtsPoint at(double column, double row) {
  return NabtsPoint{column * kCell, row * kCell};
}

/// A pel |columns| by |rows| cells.
NabtsSize pel(double columns, double rows) {
  return NabtsSize{columns * kCell, rows * kCell};
}

NaplpsInk white() {
  NaplpsInk ink;
  ink.colour = kNabtsNominalWhite;
  return ink;
}

/// The cells painted in a row, as a string of '#' and '.', for golden patterns
/// that read as pictures rather than as coordinates.
std::string row_pattern(const NaplpsCellSurface& surface, int row,
                        int first_column, int last_column) {
  std::string out;
  for (int column = first_column; column <= last_column; ++column) {
    out.push_back(surface.at(column, row).painted ? '#' : '.');
  }
  return out;
}

/// The cells painted in a column, bottom row first.
std::string column_pattern(const NaplpsCellSurface& surface, int column,
                           int first_row, int last_row) {
  std::string out;
  for (int row = first_row; row <= last_row; ++row) {
    out.push_back(surface.at(column, row).painted ? '#' : '.');
  }
  return out;
}

// ---------------------------------------------------------------------------
// The pel brush
// ---------------------------------------------------------------------------

// §5.3.2.2.6: "the logical pel, therefore, will always map to at least one and
// possibly many display pixels". The default pel is dimensionless, and a point
// drawn with it still lights the pixel the drawing point falls in.
TEST(NaplpsRasterTest, ADimensionlessPelStillPaintsOnePixel) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stamp_pel(at(3.5, 7.5), NabtsSize{0.0, 0.0}, white());

  EXPECT_EQ(surface.painted_count(), 1u);
  EXPECT_TRUE(surface.at(3, 7).painted);
}

// "All of those pixels that lie under any portion of the logical pel": a pel
// three cells wide by two high, laid on a cell boundary, is exactly six pixels.
TEST(NaplpsRasterTest, APelPaintsEveryPixelItCovers) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stamp_pel(at(4.0, 6.0), pel(3.0, 2.0), white());

  EXPECT_EQ(surface.painted_count(), 6u);
  EXPECT_EQ(row_pattern(surface, 6, 3, 8), ".###..");
  EXPECT_EQ(row_pattern(surface, 7, 3, 8), ".###..");
  EXPECT_FALSE(surface.at(4, 8).painted);
}

// The pel is a brush, and §5.3.2.2.6 sizes it without reference to where it
// happens to be: one of a given size is the same number of pixels wherever it
// is put down. A brush that grew a pixel whenever it straddled a boundary would
// draw a line whose weight varied along its own length.
TEST(NaplpsRasterTest, APelIsTheSameSizeWhereverItFallsOnTheGrid) {
  for (const double offset : {0.0, 0.25, 0.5, 0.75}) {
    NaplpsCellSurface surface(kTestGrid);
    NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

    raster.stamp_pel(at(4.0 + offset, 6.0 + offset), pel(1.0, 1.0), white());

    EXPECT_EQ(surface.painted_count(), 1u) << "at an offset of " << offset;
    EXPECT_EQ(row_pattern(surface, 6, 3, 7), ".#...")
        << "at an offset of " << offset;
  }
}

// And the size it is given is the size it comes out: a two-cell pel is two
// cells, not the three a boundary-straddling brush would cover.
TEST(NaplpsRasterTest, APelIsAsManyPixelsAsItIsWide) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stamp_pel(at(4.5, 6.5), pel(2.0, 2.0), white());

  EXPECT_EQ(surface.painted_count(), 4u);
  EXPECT_EQ(row_pattern(surface, 6, 3, 8), ".##...");
  EXPECT_EQ(row_pattern(surface, 7, 3, 8), ".##...");
}

// §5.3.2.2.6 puts the drawing point on the pel's lower left when both
// dimensions are positive and on its other three corners for the other three
// sign combinations, so a negative width reaches back from the drawing point
// rather than forward. Both receivers of the day count it from the drawing
// point's own pixel: a pel four wide ending at column 10 covers 7 to 10.
TEST(NaplpsRasterTest, ThePelSignDecidesWhichCornerTheDrawingPointIs) {
  NaplpsCellSurface positive(kTestGrid);
  NaplpsRasteriser(positive, NaplpsGridMapping{kTestGrid})
      .stamp_pel(at(10.0, 4.0), pel(4.0, 2.0), white());
  EXPECT_EQ(row_pattern(positive, 4, 5, 14), ".....####.");

  NaplpsCellSurface negative_width(kTestGrid);
  NaplpsRasteriser(negative_width, NaplpsGridMapping{kTestGrid})
      .stamp_pel(at(10.0, 4.0), pel(-4.0, 2.0), white());
  EXPECT_EQ(row_pattern(negative_width, 4, 5, 14), "..####....");

  NaplpsCellSurface negative_height(kTestGrid);
  NaplpsRasteriser(negative_height, NaplpsGridMapping{kTestGrid})
      .stamp_pel(at(10.0, 4.0), pel(4.0, -2.0), white());
  EXPECT_EQ(column_pattern(negative_height, 10, 1, 5), "..##.");
}

// "A LINE is a locus of points following a straight line algorithm ... The
// physical picture elements through which the infinitely small locus point
// passes would be drawn." With no pel to widen it, a diagonal is one cell wide.
TEST(NaplpsRasterTest, ADimensionlessPelDrawsAOneCellWideLocus) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path({at(0.5, 0.5), at(10.5, 10.5)}, NabtsSize{0.0, 0.0},
                     NabtsLineTexture::kSolid, white());

  // Eleven cells, one per step of the diagonal, and nothing beside them.
  EXPECT_EQ(surface.painted_count(), 11u);
  for (int i = 0; i <= 10; ++i) {
    EXPECT_TRUE(surface.at(i, i).painted) << "cell " << i << " of the locus";
  }
}

// The pel is a brush that "turns on additional pixels as it traverses its
// geometric path and generates the effect of line width". A horizontal line
// swept with a pel four wide and two high is a band two cells tall, running a
// pel's width past the end point because the pel is anchored at its left edge.
TEST(NaplpsRasterTest, ThePelGivesALineItsWidth) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path({at(2.0, 4.0), at(12.0, 4.0)}, pel(4.0, 2.0),
                     NabtsLineTexture::kSolid, white());

  EXPECT_EQ(row_pattern(surface, 4, 0, 17), "..##############..");
  EXPECT_EQ(row_pattern(surface, 5, 0, 17), "..##############..");
  EXPECT_EQ(row_pattern(surface, 3, 0, 17), "..................");
  EXPECT_EQ(row_pattern(surface, 6, 0, 17), "..................");
}

// A coordinate outside the display area is not drawn. Table D1 item 10 makes
// the area the part a receiver guarantees is visible, and the surface is that
// area alone.
TEST(NaplpsRasterTest, DrawingOffTheSurfaceIsDropped) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stamp_pel(at(-5.0, 4.0), pel(2.0, 2.0), white());
  raster.stamp_pel(at(4.0, 200.0), pel(2.0, 2.0), white());

  EXPECT_EQ(surface.painted_count(), 0u);
}

// ---------------------------------------------------------------------------
// Line textures (§5.3.2.4.2)
// ---------------------------------------------------------------------------

// "The size of the dot is set equal to the size of the logical pel. For
// horizontal lines, the inter-dot spacing is the width of the logical pel." A
// pel two cells wide therefore gives two cells of dot and two of gap.
TEST(NaplpsRasterTest, ADottedLineAlternatesOnePelOnAndOnePelOff) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path({at(0.0, 4.0), at(16.0, 4.0)}, pel(2.0, 1.0),
                     NabtsLineTexture::kDotted, white());

  EXPECT_EQ(row_pattern(surface, 4, 0, 15), "##..##..##..##..");
}

// "The width (length) of the dash and the inter-dash spacing are equal to three
// times the width of the logical pel."
TEST(NaplpsRasterTest, ADashedLineIsThreePelsOnAndThreeOff) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path({at(0.0, 4.0), at(24.0, 4.0)}, pel(2.0, 1.0),
                     NabtsLineTexture::kDashed, white());

  EXPECT_EQ(row_pattern(surface, 4, 0, 23), "######......######......");
}

// Dot of one pel, gap of one, dash of three, gap of one — "the inter-dot-dash
// spacing is equivalent to the inter-dot spacing". The pattern is registered
// to the screen as the Norpak TTX 650 registers it, each period of six pels
// starting with its dot at a whole multiple of six pels across.
TEST(NaplpsRasterTest, ADottedDashedLineRunsDotGapDashGap) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path({at(0.0, 4.0), at(24.0, 4.0)}, pel(2.0, 1.0),
                     NabtsLineTexture::kDottedDashed, white());

  EXPECT_EQ(row_pattern(surface, 4, 0, 23), "##..######..##..######..");
}

// A curve flattened into a polyline carries its texture along rather than
// registering it to the screen, and starts it as a straight line does: dot
// first, as the TTX 650 lays a line and an arc.
TEST(NaplpsRasterTest, ADottedDashedCurveStartsWithItsDot) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path({at(0.0, 4.0), at(10.0, 4.0), at(24.0, 4.0)},
                     pel(2.0, 1.0), NabtsLineTexture::kDottedDashed, white(),
                     /*closed=*/false, /*gaps=*/nullptr,
                     /*points_are_vertices=*/false);

  EXPECT_EQ(row_pattern(surface, 4, 0, 23), "##..######..##..######..");
}

// The pattern belongs to the screen, not to the line: two dotted lines started
// a cell apart light the same columns, as the TTX 650 lays them, where a
// pattern counted from each line's start would put their dots out of step.
// Both ends are drawn whatever the pattern says there.
TEST(NaplpsRasterTest, ALineTextureIsRegisteredToTheScreen) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path({at(0.0, 4.0), at(16.0, 4.0)}, pel(1.0, 1.0),
                     NabtsLineTexture::kDotted, white());
  raster.stroke_path({at(1.0, 6.0), at(16.0, 6.0)}, pel(1.0, 1.0),
                     NabtsLineTexture::kDotted, white());

  EXPECT_EQ(row_pattern(surface, 4, 0, 16), "#.#.#.#.#.#.#.#.#");
  EXPECT_EQ(row_pattern(surface, 6, 0, 16), ".##.#.#.#.#.#.#.#")
      << "its start drawn, then the screen's dots";
}

// Where a line passes exactly midway between two cells, the cell nearer its
// upper end is lit, as both receivers of the day light it on a line drawn
// upward — whichever end the record starts the line from.
TEST(NaplpsRasterTest, ALineTieGoesToTheUpperCell) {
  for (const bool upward : {true, false}) {
    NaplpsCellSurface surface(kTestGrid);
    NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
    // Four across and two up: the line passes midway between two cells at
    // columns 3 and 5, and takes the upper one at both.
    const NabtsPoint low = at(2.0, 2.0);
    const NabtsPoint high = at(6.0, 4.0);
    raster.stroke_path(upward ? std::vector<NabtsPoint>{low, high}
                              : std::vector<NabtsPoint>{high, low},
                       pel(1.0, 1.0), NabtsLineTexture::kSolid, white());
    EXPECT_EQ(row_pattern(surface, 4, 2, 6), "...##") << upward;
    EXPECT_EQ(row_pattern(surface, 3, 2, 6), ".##..") << upward;
    EXPECT_EQ(row_pattern(surface, 2, 2, 6), "#....") << upward;
  }
}

// For a vertical line the spacings are the pel's height rather than its width,
// so the same texture reads down the line at the other dimension.
TEST(NaplpsRasterTest, AVerticalLineIsTexturedByThePelHeight) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path({at(6.0, 0.0), at(6.0, 16.0)}, pel(1.0, 2.0),
                     NabtsLineTexture::kDotted, white());

  EXPECT_EQ(column_pattern(surface, 6, 0, 15), "##..##..##..##..");
}

// §5.3.2.4.2's closing note: "If logical pel size dx = 0, all nonvertical lines
// are solid. If logical pel size dy = 0, all nonhorizontal lines are solid."
// With no extent along the line there is no dot to space, so the texture has
// nothing to work with and the line comes out solid.
TEST(NaplpsRasterTest, ATextureWithNoPelAlongTheLineDrawsSolid) {
  NaplpsCellSurface horizontal(kTestGrid);
  NaplpsRasteriser(horizontal, NaplpsGridMapping{kTestGrid})
      .stroke_path({at(0.0, 4.0), at(12.0, 4.0)}, pel(0.0, 2.0),
                   NabtsLineTexture::kDashed, white());
  EXPECT_EQ(row_pattern(horizontal, 4, 0, 11), "############");

  // A diagonal is nonvertical too, so a pel of no width leaves it solid.
  NaplpsCellSurface diagonal(kTestGrid);
  NaplpsRasteriser(diagonal, NaplpsGridMapping{kTestGrid})
      .stroke_path({at(0.5, 0.5), at(10.5, 10.5)}, pel(0.0, 2.0),
                   NabtsLineTexture::kDotted, white());
  for (int i = 0; i <= 10; ++i) {
    EXPECT_TRUE(diagonal.at(i, i).painted) << "cell " << i << " of a solid run";
  }
}

// "All end points of lines and arcs and all vertices ... must be plotted
// regardless of the line texture used." The end point here falls in a gap of
// the pattern and is drawn anyway.
TEST(NaplpsRasterTest, EndPointsArePlottedThroughAGapInTheTexture) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  // The line stops two cells into the gap that follows the first dot.
  raster.stroke_path({at(0.0, 4.0), at(3.0, 4.0)}, pel(1.0, 1.0),
                     NabtsLineTexture::kDotted, white());

  EXPECT_TRUE(surface.at(0, 4).painted) << "the start point";
  EXPECT_TRUE(surface.at(3, 4).painted) << "the end point, mid-gap";
}

// ---------------------------------------------------------------------------
// Fills, hatching and highlight
// ---------------------------------------------------------------------------

/// The four corners of a rectangle as a closed path.
std::vector<NabtsPoint> rectangle(double column, double row, double columns,
                                  double rows) {
  return {at(column, row), at(column + columns, row),
          at(column + columns, row + rows), at(column, row + rows)};
}

// A line is drawn by sweeping the pel along it, and §5.3.2.2.6 sizes the pel
// without reference to the direction of the sweep, so a line comes out the
// pel's weight whichever way it runs. Measured across the grid, that is the
// count of cells in a cross-section taken at right angles to the axis the
// sweep is stepping along: one cell for a one-cell pel, running horizontally,
// vertically or at any slope between.
TEST(NaplpsRasterTest, ALineIsThePelsWeightWhicheverWayItRuns) {
  struct Direction {
    const char* name;
    int run;
    int rise;
  };
  const Direction directions[] = {{"horizontal", 16, 0}, {"vertical", 0, 16},
                                  {"shallow", 16, 5},    {"steep", 5, 16},
                                  {"diagonal", 16, 16},  {"backwards", -16, 7}};

  for (const Direction& direction : directions) {
    // Started wherever the line fits on the grid, which for one running
    // leftwards is the far side.
    const int start_column = direction.run < 0 ? 24 : 8;
    NaplpsCellSurface surface(kTestGrid);
    NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
    raster.stroke_path({at(start_column, 3.0),
                        at(start_column + direction.run, 3.0 + direction.rise)},
                       pel(1.0, 1.0), NabtsLineTexture::kSolid, white());

    // Across whichever axis the sweep steps along, so that the section is taken
    // across the line rather than along it.
    const bool steps_along_columns =
        std::abs(direction.run) >= std::abs(direction.rise);
    const int sections = steps_along_columns ? std::abs(direction.run)
                                             : std::abs(direction.rise);
    for (int section = 1; section < sections; ++section) {
      int painted = 0;
      for (int across = 0;
           across < (steps_along_columns ? kTestGrid.height : kTestGrid.width);
           ++across) {
        const int column =
            steps_along_columns
                ? start_column + (direction.run < 0 ? -section : section)
                : across;
        const int row = steps_along_columns ? across : 3 + section;
        painted += surface.at(column, row).painted ? 1 : 0;
      }
      EXPECT_EQ(painted, 1) << "a one-cell pel drawn " << direction.name
                            << ", across section " << section;
    }
  }
}

// And the weight follows the pel: state it two cells and the line is two cells
// across, not the three a brush measured by what it covered would give.
TEST(NaplpsRasterTest, ALineIsAsManyCellsAcrossAsThePelIsWide) {
  NaplpsCellSurface horizontal(kTestGrid);
  NaplpsRasteriser(horizontal, NaplpsGridMapping{kTestGrid})
      .stroke_path({at(4.5, 6.5), at(16.5, 6.5)}, pel(2.0, 2.0),
                   NabtsLineTexture::kSolid, white());
  EXPECT_EQ(column_pattern(horizontal, 10, 4, 9), "..##..");

  NaplpsCellSurface vertical(kTestGrid);
  NaplpsRasteriser(vertical, NaplpsGridMapping{kTestGrid})
      .stroke_path({at(6.5, 4.5), at(6.5, 16.5)}, pel(2.0, 2.0),
                   NabtsLineTexture::kSolid, white());
  EXPECT_EQ(row_pattern(vertical, 10, 4, 9), "..##..");
}

// An outlined figure is its outline alone: the pel sweeps the edges and the
// inside is left showing whatever was under it.
TEST(NaplpsRasterTest, AnOutlinedRectangleIsHollow) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.stroke_path(rectangle(4.0, 4.0, 6.0, 5.0), NabtsSize{0.0, 0.0},
                     NabtsLineTexture::kSolid, white(), /*closed=*/true);

  EXPECT_EQ(row_pattern(surface, 4, 3, 11), ".#######.");
  EXPECT_EQ(row_pattern(surface, 6, 3, 11), ".#.....#.");
  EXPECT_EQ(row_pattern(surface, 9, 3, 11), ".#######.");
}

// §5.3.3.4.1 fills "the area enclosed by the outline (including the region of
// the outline traced by the logical pel)", so a filled rectangle is the
// outlined one solid: the same cells @ref AnOutlinedRectangleIsHollow leaves
// round the edge, with the middle filled in too.
TEST(NaplpsRasterTest, AFilledRectangleIsTheOutlinedOneSolid) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.fill_path(rectangle(4.0, 4.0, 6.0, 5.0), NabtsSize{0.0, 0.0},
                   NabtsTexturePattern::kSolid, NabtsSize{0.0, 0.0}, nullptr,
                   white());

  EXPECT_EQ(row_pattern(surface, 4, 3, 11), ".#######.");
  EXPECT_EQ(row_pattern(surface, 6, 3, 11), ".#######.");
  EXPECT_EQ(row_pattern(surface, 9, 3, 11), ".#######.");
  EXPECT_EQ(surface.painted_count(), 42u);
}

// The traced outline is part of the filled region, not a line over it: a path
// enclosing no area at all still draws, because the pel gives it its weight.
// This is what a service relies on when it draws a letterform as a stroke.
TEST(NaplpsRasterTest, AFilledFigureIncludesTheOutlineThePelTraces) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  // Out and back along one line: zero enclosed area.
  raster.fill_path({at(6.0, 4.0), at(6.0, 12.0), at(6.0, 4.0)}, pel(2.0, 1.0),
                   NabtsTexturePattern::kSolid, NabtsSize{0.0, 0.0}, nullptr,
                   white());

  EXPECT_GT(surface.painted_count(), 0u)
      << "the pel traced no outline, so the figure drew nothing at all";
  EXPECT_EQ(row_pattern(surface, 8, 4, 9), "..##..");
}

// And a dimensionless pel traces a one-pixel outline rather than none, because
// §5.3.2.2.6 maps the pel to "at least one" pixel however small it is stated.
// The same path stroked with the same pel draws the same line, which is what a
// fill and the outline a service draws over it depend on.
TEST(NaplpsRasterTest, ADimensionlessPelTracesAFillsOutlineOnePixelWide) {
  const std::vector<NabtsPoint> out_and_back = {at(6.0, 4.0), at(6.0, 12.0),
                                                at(6.0, 4.0)};

  NaplpsCellSurface filled(kTestGrid);
  NaplpsRasteriser(filled, NaplpsGridMapping{kTestGrid})
      .fill_path(out_and_back, NabtsSize{0.0, 0.0}, NabtsTexturePattern::kSolid,
                 NabtsSize{0.0, 0.0}, nullptr, white());

  NaplpsCellSurface stroked(kTestGrid);
  NaplpsRasteriser(stroked, NaplpsGridMapping{kTestGrid})
      .stroke_path(out_and_back, NabtsSize{0.0, 0.0}, NabtsLineTexture::kSolid,
                   white(), /*closed=*/true);

  EXPECT_EQ(filled.painted_count(), 9u);
  EXPECT_EQ(column_pattern(filled, 6, 4, 12), "#########");
  EXPECT_EQ(filled.painted_count(), stroked.painted_count());
}

// §5.3.2.4.4: "The width and spacing of hatching lines in the vertical hatching
// pattern are equal to the width of the logical pel." A pel two cells wide
// gives two cells of line and two of gap.
TEST(NaplpsRasterTest, VerticalHatchingIsSpacedByThePelWidth) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.fill_path(rectangle(0.0, 4.0, 16.0, 4.0), pel(2.0, 2.0),
                   NabtsTexturePattern::kVerticalHatch, NabtsSize{0.0, 0.0},
                   nullptr, white());

  EXPECT_EQ(row_pattern(surface, 5, 0, 15), "##..##..##..##..");
  EXPECT_EQ(row_pattern(surface, 6, 0, 15), "##..##..##..##..");
}

// The hatch is registered against the unit screen's origin rather than the
// figure's, which is what §5.3.2.4.4 means by "registration of the patterns
// shall be maintained across figures if the logical pel size is the same". Two
// figures at different offsets therefore hatch in step.
TEST(NaplpsRasterTest, HatchingKeepsRegistrationAcrossFigures) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.fill_path(rectangle(0.0, 4.0, 8.0, 2.0), pel(2.0, 2.0),
                   NabtsTexturePattern::kVerticalHatch, NabtsSize{0.0, 0.0},
                   nullptr, white());
  // A second figure starting at an odd column: were the pattern anchored to the
  // figure it would start a line here and fall out of step with the first.
  raster.fill_path(rectangle(9.0, 8.0, 7.0, 2.0), pel(2.0, 2.0),
                   NabtsTexturePattern::kVerticalHatch, NabtsSize{0.0, 0.0},
                   nullptr, white());

  // Every painted cell of both figures falls in an even-numbered band of two
  // columns, counted from the unit screen's origin — which is what being in
  // register means, and is not something either figure's own extent could give.
  int painted = 0;
  for (int row = 0; row < kTestGrid.height; ++row) {
    for (int column = 0; column < kTestGrid.width; ++column) {
      if (!surface.at(column, row).painted) {
        continue;
      }
      ++painted;
      EXPECT_EQ((column / 2) % 2, 0)
          << "cell " << column << "," << row << " is in a gap of the hatch";
    }
  }
  EXPECT_GT(painted, 0);
  // Both figures drew: the second one's rows are above the first one's.
  EXPECT_TRUE(surface.at(0, 4).painted);
  EXPECT_TRUE(surface.at(12, 8).painted);
}

// §5.3.2.4.3: a highlighted figure is filled as usual and its outline drawn
// solid in nominal black (colour modes 0 and 1), so the outline reads as a
// different colour from the fill rather than as more of it.
TEST(NaplpsRasterTest, AHighlightOutlinesTheFillInItsOwnColour) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  const std::vector<NabtsPoint> path = rectangle(4.0, 4.0, 6.0, 5.0);
  raster.fill_path(path, NabtsSize{0.0, 0.0}, NabtsTexturePattern::kSolid,
                   NabtsSize{0.0, 0.0}, nullptr, white());

  NaplpsInk black;
  black.colour = kNabtsNominalBlack;
  raster.highlight_path(path, NabtsSize{0.0, 0.0}, black);

  EXPECT_EQ(surface.at(4, 4).colour.red, kNabtsNominalBlack.red)
      << "the outline should be the highlight colour";
  EXPECT_EQ(surface.at(6, 6).colour.red, kNabtsNominalWhite.red)
      << "the inside should still be the fill colour";
}

// ---------------------------------------------------------------------------
// Arcs, circles and splines (§5.3.3.3)
// ---------------------------------------------------------------------------

// "If the end point is omitted, it is taken to be coincident with the start
// point and a circle is drawn", the intermediate point being diametrically
// opposite. Drawn with no pel it is a closed ring one cell thick.
TEST(NaplpsRasterTest, CoincidentEndPointsDrawACircle) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  // A circle of radius 5 cells centred on cell (11, 11).
  const std::vector<NabtsPoint> outline =
      raster.arc_polyline({at(6.0, 11.0), at(16.0, 11.0), at(6.0, 11.0)});
  raster.stroke_path(outline, NabtsSize{0.0, 0.0}, NabtsLineTexture::kSolid,
                     white());

  // The ring passes through the four extremes of the circle and leaves the
  // middle untouched.
  EXPECT_TRUE(surface.at(6, 11).painted);
  EXPECT_TRUE(surface.at(11, 6).painted);
  EXPECT_TRUE(surface.at(11, 16).painted);
  EXPECT_FALSE(surface.at(11, 11).painted) << "a stroked circle is hollow";
}

// "If the three drawing points are colinear, a line is drawn from the start
// point to the end point" — an arc of unbounded radius is not attempted.
TEST(NaplpsRasterTest, ColinearControlPointsDrawALine) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  const std::vector<NabtsPoint> outline =
      raster.arc_polyline({at(2.5, 4.5), at(6.5, 4.5), at(10.5, 4.5)});
  raster.stroke_path(outline, NabtsSize{0.0, 0.0}, NabtsLineTexture::kSolid,
                     white());

  EXPECT_EQ(row_pattern(surface, 4, 0, 13), "..#########...");
}

// The arc runs the way round that passes through the intermediate point, which
// is the only thing that distinguishes the two arcs joining its ends.
TEST(NaplpsRasterTest, AnArcRunsThroughItsIntermediatePoint) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  // Ends level at cell row 11, intermediate point below them: the arc dips.
  const std::vector<NabtsPoint> outline =
      raster.arc_polyline({at(6.0, 11.0), at(11.0, 6.0), at(16.0, 11.0)});
  raster.stroke_path(outline, NabtsSize{0.0, 0.0}, NabtsLineTexture::kSolid,
                     white());

  EXPECT_TRUE(surface.at(11, 6).painted) << "the arc should pass below";
  EXPECT_FALSE(surface.at(11, 16).painted)
      << "the arc took the long way round instead";
}

// A circle reaches as far as its radius along each axis, and both receivers of
// the day light those extreme pixels, so the polyline standing in for it runs
// through all four whichever way round it starts.
TEST(NaplpsRasterTest, ACircleReachesItsExtremes) {
  const NabtsPoint centre{0.5, 0.5};
  const double radius = 0.25;
  const NabtsPoint start{centre.x + radius * std::cos(0.3),
                         centre.y + radius * std::sin(0.3)};
  const NabtsPoint through{2.0 * centre.x - start.x, 2.0 * centre.y - start.y};
  const std::vector<NabtsPoint> circle =
      naplps_arc_polyline({start, through, start}, 0.2 * kCell);
  double left = 1.0;
  double right = 0.0;
  double bottom = 1.0;
  double top = 0.0;
  for (const NabtsPoint& point : circle) {
    left = std::min(left, point.x);
    right = std::max(right, point.x);
    bottom = std::min(bottom, point.y);
    top = std::max(top, point.y);
  }
  EXPECT_NEAR(left, 0.25, 1e-12);
  EXPECT_NEAR(right, 0.75, 1e-12);
  EXPECT_NEAR(bottom, 0.25, 1e-12);
  EXPECT_NEAR(top, 0.75, 1e-12);
}

// §5.3.3.3: "The minimum implementation of the spline shall be a series of
// lines connecting the start, intermediate, and end points of the spline", and
// the smooth form is reserved for future standardization.
TEST(NaplpsRasterTest, MoreThanThreePointsDrawAsAPolyline) {
  const std::vector<NabtsPoint> control = {at(2.0, 2.0), at(6.0, 10.0),
                                           at(12.0, 4.0), at(18.0, 12.0)};
  const std::vector<NabtsPoint> line =
      naplps_arc_polyline(control, 0.2 * kCell);
  EXPECT_EQ(line.size(), control.size());
  for (size_t i = 0; i < control.size(); ++i) {
    EXPECT_DOUBLE_EQ(line[i].x, control[i].x);
    EXPECT_DOUBLE_EQ(line[i].y, control[i].y);
  }
}

// Both receivers of the day draw a circle through the pixel corners its
// coordinates name, the Norpak TTX 650 a step at a time to whichever
// neighbouring cell stays nearest the radius. A circle of radius 14 about
// (128, 100) at 256 x 200, taken through its firmware, is 80 pixels, its top
// row x 125 to 131 and symmetric about the centre.
TEST(NaplpsRasterTest, ACircleIsTracedAsTheReceiversTracedIt) {
  NaplpsCellSurface surface(kNaplpsGridReference);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kNaplpsGridReference});
  const auto reference = [](double column, double row) {
    return NabtsPoint{column / 256.0, row / 256.0};
  };
  std::vector<std::pair<int, int>> path;
  bool circle = false;
  bool collinear = false;
  ASSERT_TRUE(raster.arc_cells(
      {reference(114, 100), reference(142, 100), reference(114, 100)}, path,
      circle, collinear));
  EXPECT_TRUE(circle);
  raster.stroke_cells(path, NabtsSize{0.0, 0.0}, NabtsLineTexture::kSolid,
                      white(), nullptr);
  EXPECT_EQ(surface.painted_count(), 80u);
  int top = -1;
  for (int row = 0; row < kNaplpsGridReference.height; ++row) {
    for (int column = 0; column < kNaplpsGridReference.width; ++column) {
      if (surface.at(column, row).painted) {
        top = row;
      }
    }
  }
  EXPECT_EQ(row_pattern(surface, top, 123, 133), "..#######..");
}

// An arc through three points is walked from its start to its end, one cell to
// the next with no gap, the way round that passes its intermediate point.
TEST(NaplpsRasterTest, AnArcIsWalkedThroughItsIntermediatePoint) {
  for (const bool reversed : {false, true}) {
    NaplpsCellSurface surface(kTestGrid);
    NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
    std::vector<NabtsPoint> control = {at(4.0, 10.0), at(14.0, 20.0),
                                       at(24.0, 10.0)};
    if (reversed) {
      std::swap(control.front(), control.back());
    }
    std::vector<std::pair<int, int>> path;
    bool circle = false;
    bool collinear = false;
    ASSERT_TRUE(raster.arc_cells(control, path, circle, collinear));
    EXPECT_FALSE(circle);
    ASSERT_GE(path.size(), 2u);
    EXPECT_EQ(path.front(), (std::pair<int, int>{reversed ? 24 : 4, 10}));
    EXPECT_EQ(path.back(), (std::pair<int, int>{reversed ? 4 : 24, 10}));
    int highest = path.front().second;
    for (size_t i = 1; i < path.size(); ++i) {
      EXPECT_LE(std::abs(path[i].first - path[i - 1].first), 1) << i;
      EXPECT_LE(std::abs(path[i].second - path[i - 1].second), 1) << i;
      highest = std::max(highest, path[i].second);
    }
    EXPECT_EQ(highest, 20) << "over the top, through (14, 20)";
  }
}

// Three points on a line make no circle: the receivers draw a line from the
// start to the intermediate point and on to the end.
TEST(NaplpsRasterTest, ThreePointsOnALineAreNoArc) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  std::vector<std::pair<int, int>> path;
  bool circle = false;
  bool collinear = false;
  EXPECT_FALSE(raster.arc_cells({at(2.0, 4.0), at(8.0, 4.0), at(14.0, 4.0)},
                                path, circle, collinear));
  EXPECT_TRUE(collinear);
}

// §5.3.3.5.1's enclosed area is the odd-even one, and the receivers fill a
// figure by scanning each row between the path's crossings, with its traced
// outline round it: a hole a single row high inside a frame drawn as one path
// stays open, where a fill flooded in from the outside could not reach it and
// closed it.
TEST(NaplpsRasterTest, AOneRowHoleInAFilledFrameStaysOpen) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  // Round the outside, in along row 6, round a hole from row 5 to row 7, and
  // back out.
  const std::vector<NabtsPoint> frame = {
      at(2.0, 2.0),  at(14.0, 2.0), at(14.0, 10.0), at(2.0, 10.0),
      at(2.0, 6.0),  at(4.0, 6.0),  at(4.0, 5.0),   at(10.0, 5.0),
      at(10.0, 7.0), at(4.0, 7.0),  at(4.0, 6.0),   at(2.0, 6.0)};
  raster.fill_path(frame, NabtsSize{0.0, 0.0}, NabtsTexturePattern::kSolid,
                   NabtsSize{0.0, 0.0}, nullptr, white());
  EXPECT_EQ(row_pattern(surface, 6, 5, 9), ".....") << "the hole";
  EXPECT_EQ(row_pattern(surface, 5, 4, 10), "#######") << "its edges";
  EXPECT_EQ(row_pattern(surface, 7, 4, 10), "#######");
  EXPECT_EQ(row_pattern(surface, 4, 3, 13), "###########") << "the frame";
}

// A filled arc encloses the area between the curve and the chord joining its
// ends (§5.3.3.3), so the middle of that region is painted.
TEST(NaplpsRasterTest, AFilledArcEnclosesTheAreaUpToItsChord) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  const std::vector<NabtsPoint> outline =
      raster.arc_polyline({at(6.0, 12.0), at(11.0, 6.0), at(16.0, 12.0)});
  raster.fill_path(outline, NabtsSize{0.0, 0.0}, NabtsTexturePattern::kSolid,
                   NabtsSize{0.0, 0.0}, nullptr, white());

  EXPECT_TRUE(surface.at(11, 9).painted)
      << "between the curve and its chord should be filled";
  EXPECT_FALSE(surface.at(11, 16).painted)
      << "outside the arc should be left alone";
}

// ---------------------------------------------------------------------------
// Incremental colour runs (§5.3.3.6.3)
// ---------------------------------------------------------------------------

/// |count| inks, alternating so a raster's layout is readable from the result.
std::vector<NaplpsInk> alternating(size_t count) {
  std::vector<NaplpsInk> out;
  for (size_t i = 0; i < count; ++i) {
    NaplpsInk ink;
    ink.colour = (i % 2 == 0) ? kNabtsNominalWhite : kNabtsNominalBlack;
    out.push_back(ink);
  }
  return out;
}

// §5.3.3.6.3: INCREMENTAL POINT lays one colour under the pel, then moves the
// drawing point a pel along x. A row is laid from its drawing point, so the
// colours follow one another pel by pel.
TEST(NaplpsRasterTest, AnIncrementalRowLaysOnePelPerColour) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.deposit_incremental_row(at(4.0, 10.0), pel(2.0, 2.0), at(4.0, 8.0),
                                 alternating(4));

  EXPECT_EQ(row_pattern(surface, 10, 3, 12), ".########.");
  EXPECT_EQ(row_pattern(surface, 11, 3, 12), ".########.");
  EXPECT_EQ(row_pattern(surface, 9, 3, 12), "..........");
  EXPECT_EQ(surface.at(4, 10).colour, kNabtsNominalWhite);
  EXPECT_EQ(surface.at(6, 10).colour, kNabtsNominalBlack);
  EXPECT_EQ(surface.at(8, 10).colour, kNabtsNominalWhite);
}

// "Note that if dx is positive, the drawing point moves to the right and if dx
// is negative, the drawing point moves to the left." A pel of negative width is
// counted from the drawing point's own pixel, as both receivers of the day
// count it, so a row laid leftwards from the field's last pixel fills it.
TEST(NaplpsRasterTest, ANegativePelLaysTheRowRightToLeft) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.deposit_incremental_row(at(11.0, 8.0), pel(-2.0, 2.0), at(4.0, 8.0),
                                 alternating(4));

  EXPECT_EQ(row_pattern(surface, 8, 3, 12), ".########.");
  EXPECT_EQ(surface.at(11, 8).colour, kNabtsNominalWhite) << "first colour";
  EXPECT_EQ(surface.at(9, 8).colour, kNabtsNominalBlack) << "second colour";
}

// "It is required ... to perform a preexecution rescaling ... to make the
// logical pel dimensions become either integer multiples or integer fractions
// of the corresponding physical pixel dimensions" — a pel of one and a half
// pixels becomes one, so every pel of the row is the same width rather than
// alternating one and two.
TEST(NaplpsRasterTest, AnIncrementalPelIsRescaledToWholePixels) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.deposit_incremental_row(at(4.0, 8.0), pel(1.5, 1.5), at(4.0, 8.0),
                                 alternating(4));

  EXPECT_EQ(row_pattern(surface, 8, 3, 9), ".####..");
  EXPECT_EQ(surface.at(5, 8).colour, kNabtsNominalBlack);
  EXPECT_EQ(surface.at(6, 8).colour, kNabtsNominalWhite);
}

// A pel smaller than a pixel is rescaled to a whole fraction of one, and "if a
// pixel lies under the logical pel ... for more than one deposit operation, it
// retains the color deposited there last".
TEST(NaplpsRasterTest, PelsSmallerThanAPixelShareIt) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.deposit_incremental_row(at(4.0, 8.0), pel(0.5, 0.5), at(4.0, 8.0),
                                 alternating(4));

  EXPECT_EQ(row_pattern(surface, 8, 3, 7), ".##..");
  EXPECT_EQ(surface.at(4, 8).colour, kNabtsNominalBlack);
  EXPECT_EQ(surface.at(5, 8).colour, kNabtsNominalBlack);
}

// "the entire display image lying within the area of the screen defined by the
// active field is scrolled": only that area moves, what leaves it is lost, and
// what it leaves behind is empty.
TEST(NaplpsRasterTest, AScrollMovesOnlyTheFieldAndDropsWhatLeavesIt) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  NaplpsInk red;
  red.colour = NabtsColour{0, 15, 0, false};
  for (int row = 8; row < 12; ++row) {
    surface.deposit(5, row, row == 8 ? red : white());
  }
  surface.deposit(2, 10, white());  // outside the field

  raster.scroll_area(at(4.0, 8.0), NabtsSize{4.0 * kCell, 4.0 * kCell},
                     NabtsPoint{0.0, 2.0 * kCell}, pel(2.0, 2.0));

  EXPECT_EQ(column_pattern(surface, 5, 8, 11), "..##");
  EXPECT_EQ(surface.at(5, 10).colour, red.colour) << "row 8 moved up two";
  EXPECT_TRUE(surface.at(2, 10).painted) << "outside the field";
}

// §5.3.2.4.4: "The default pattern for the four programmable texture masks is
// a null texture pattern, resulting in no fill in color modes 0 and 1, and a
// fill with the background color in color mode 2."
TEST(NaplpsRasterTest, AnUndefinedMaskFillsNothingOrTheBackground) {
  const std::vector<NabtsPoint> square = {at(4, 4), at(12, 4), at(12, 12),
                                          at(4, 12)};
  const NabtsTextureMask undefined;
  NaplpsInk ground;
  ground.colour = NabtsColour{0, 0, 15, false};

  NaplpsCellSurface plain(kTestGrid);
  NaplpsRasteriser raster(plain, NaplpsGridMapping{kTestGrid});
  raster.fill_path(square, pel(0, 0), NabtsTexturePattern::kMaskA, pel(4, 4),
                   &undefined, white());
  EXPECT_EQ(plain.painted_count(), 0u) << "modes 0 and 1: no fill";

  NaplpsCellSurface with_ground(kTestGrid);
  NaplpsRasteriser mode_two(with_ground, NaplpsGridMapping{kTestGrid});
  mode_two.fill_path(square, pel(0, 0), NabtsTexturePattern::kMaskA, pel(4, 4),
                     &undefined, white(), &ground);
  EXPECT_EQ(with_ground.at(8, 8).colour, ground.colour) << "mode 2: ground";
}

// §5.3.2.4.4: "In color mode 2, the fill areas not drawn in the drawing color
// are drawn in the background color" — the gaps of a hatch.
TEST(NaplpsRasterTest, AHatchInModeTwoFillsItsGapsWithTheBackground) {
  const std::vector<NabtsPoint> square = {at(4, 4), at(12, 4), at(12, 12),
                                          at(4, 12)};
  NaplpsInk ground;
  ground.colour = NabtsColour{0, 0, 15, false};
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  raster.fill_path(square, pel(1, 1), NabtsTexturePattern::kVerticalHatch,
                   pel(4, 4), nullptr, white(), &ground);
  // Pel-wide columns alternate between the drawing colour and the ground.
  EXPECT_FALSE(surface.at(8, 8).colour == surface.at(9, 8).colour);
  EXPECT_TRUE(surface.at(8, 8).painted && surface.at(9, 8).painted);
}

// §5.3.2.4.2: "In color mode 2, the inter-dot and inter-dash spacing is drawn
// in the background color."
TEST(NaplpsRasterTest, ADashedLineInModeTwoDrawsItsGapsInTheBackground) {
  NaplpsInk ground;
  ground.colour = NabtsColour{0, 0, 15, false};
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  raster.stroke_path({at(2.5, 6.5), at(28.5, 6.5)}, pel(1, 1),
                     NabtsLineTexture::kDashed, white(), false, &ground);
  EXPECT_EQ(row_pattern(surface, 6, 2, 28), std::string(27, '#'))
      << "no gap is left undrawn";
  bool saw_ground = false;
  for (int column = 2; column <= 28; ++column) {
    saw_ground = saw_ground || surface.at(column, 6).colour == ground.colour;
  }
  EXPECT_TRUE(saw_ground);
}

// ---------------------------------------------------------------------------
// Characters, mosaics and downloadable glyphs
// ---------------------------------------------------------------------------

/// A character primitive of |code| in |repertoire|, its field |columns| by
/// |rows| cells with its origin at the given cell position.
NabtsPrimitive character_at(uint8_t code, NabtsPrimitive::Repertoire repertoire,
                            double column, double row, double columns,
                            double rows) {
  NabtsPrimitive primitive;
  primitive.kind = NabtsPrimitiveKind::kCharacter;
  primitive.character = code;
  primitive.repertoire = repertoire;
  primitive.origin = at(column, row);
  primitive.size = NabtsSize{columns * kCell, rows * kCell};
  return primitive;
}

// A character is deposited as the pattern a receiver stored for it, so what
// lands on the grid is a glyph rather than a box: some cells of the field come
// on and some do not.
TEST(NaplpsRasterTest, ACharacterDepositsItsStoredPattern) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  raster.deposit_character(
      character_at('H', NabtsPrimitive::Repertoire::kPrimary, 4, 4, 6, 10), {},
      white(), nullptr);

  const size_t painted = surface.painted_count();
  EXPECT_GT(painted, 0u) << "the character drew nothing at all";
  EXPECT_LT(painted, 60u) << "the character filled its whole field";
}

// Table D1 item 2 asks for the primary characters to be presented only in
// fields of 6/256 by 8/256 or more. In a smaller one both receivers of the day
// present no pattern; the Norpak TTX 650 lights the one pixel at the field's
// origin, which is what is drawn here, at every grid.
TEST(NaplpsRasterTest, ACharacterTooSmallToPresentIsADot) {
  const NabtsPrimitive tiny = [] {
    NabtsPrimitive primitive;
    primitive.kind = NabtsPrimitiveKind::kCharacter;
    primitive.character = 'A';
    primitive.repertoire = NabtsPrimitive::Repertoire::kPrimary;
    primitive.origin = NabtsPoint{100.0 / 256.0, 100.0 / 256.0};
    primitive.size = NabtsSize{4.0 / 256.0, 6.0 / 256.0};
    return primitive;
  }();
  EXPECT_TRUE(naplps_character_too_small(tiny, {}));
  for (const NaplpsRenderGrid grid :
       {kNaplpsGridReference, NaplpsRenderGrid{512, 400}}) {
    NaplpsCellSurface surface(grid);
    NaplpsRasteriser(surface, NaplpsGridMapping{grid})
        .deposit_character(tiny, {}, white(), nullptr);
    const int factor = grid.width / 256;
    EXPECT_EQ(surface.painted_count(),
              static_cast<size_t>(factor) * static_cast<size_t>(factor))
        << grid.width;
    EXPECT_TRUE(surface.at(100 * factor, 100 * factor).painted) << grid.width;
  }

  // A field of 6 by 8 or more is presented; mosaics and SPACE never count as
  // too small, and neither does a mark a letter will compose.
  NabtsPrimitive presented = tiny;
  presented.size = NabtsSize{6.0 / 256.0, 8.0 / 256.0};
  EXPECT_FALSE(naplps_character_too_small(presented, {}));
  NabtsPrimitive mosaic = tiny;
  mosaic.repertoire = NabtsPrimitive::Repertoire::kMosaic;
  mosaic.character = 0x7F;
  EXPECT_FALSE(naplps_character_too_small(mosaic, {}));
  NabtsPrimitive space = tiny;
  space.character = 0x20;
  EXPECT_FALSE(naplps_character_too_small(space, {}));
}

// The default character field of §5.3.2.3.9, deposited at a place that is a
// whole number of cells from the origin on all three grids: unit x 16/256 and
// y 100/256 are columns 16, 32 and 48 and rows 100, 200 and 300.
NabtsPrimitive default_field_character(uint8_t code) {
  NabtsPrimitive primitive;
  primitive.kind = NabtsPrimitiveKind::kCharacter;
  primitive.character = code;
  primitive.repertoire = NabtsPrimitive::Repertoire::kPrimary;
  primitive.origin = NabtsPoint{16.0 / 256.0, 100.0 / 256.0};
  primitive.size = NabtsSize{1.0 / 40.0, 5.0 / 128.0};
  return primitive;
}

/// Deposit |code| in the default character field on |grid|.
void deposit_default_field(NaplpsCellSurface& surface, uint8_t code) {
  NaplpsRasteriser(surface, NaplpsGridMapping{surface.grid()})
      .deposit_character(default_field_character(code), {}, white(), nullptr);
}

/// Whether the reference receiver's 6 by 10 face would light the cell
/// |column|, |row| of a |columns| by |rows| field measured from the field's
/// lower left — the pattern a receiver holding only that one face deposits,
/// by the same nearest-neighbour rule the deposit uses.
bool reference_face_lights(char32_t code, int columns, int rows, int column,
                           int row) {
  const NaplpsFontFace& face = naplps_font_face(0);
  const uint16_t* pattern = naplps_face_pattern(face, code);
  if (pattern == nullptr) {
    return false;
  }
  const int from_top = rows - 1 - row;
  const int source_row =
      std::min(face.height - 1, face.height * from_top / rows);
  const int source_column =
      std::min(face.width - 1, face.width * column / columns);
  return (pattern[source_row] & (1u << (face.width - 1 - source_column))) != 0u;
}

/// How many cells of the field the deposit and the reference face disagree on.
int cells_differing_from_the_reference_face(const NaplpsRenderGrid& grid,
                                            uint8_t code) {
  NaplpsCellSurface surface(grid);
  deposit_default_field(surface, code);

  const int scale = grid.width / kNaplpsGridReference.width;
  const NaplpsGridMapping mapping{grid};
  const int columns =
      static_cast<int>(std::lround(mapping.columns_across(1.0 / 40.0)));
  const int rows = static_cast<int>(std::lround(mapping.rows_up(5.0 / 128.0)));

  int differing = 0;
  for (int row = 0; row < rows; ++row) {
    for (int column = 0; column < columns; ++column) {
      const bool drawn =
          surface.at(16 * scale + column, 100 * scale + row).painted;
      if (drawn != reference_face_lights(code, columns, rows, column, row)) {
        ++differing;
      }
    }
  }
  return differing;
}

// §5.1 constrains the character patterns only "for a given display
// resolution", and a receiver with more pixels held a generator with finer
// ones. So a finer grid has to deposit a different pattern rather than the
// reference receiver's one sampled up: with one face for every receiver the
// three grids draw the same shape over more cells, which is what this
// disagreement count would find at zero.
TEST(NaplpsRasterTest, AFinerReceiverDrawsAFinerLetterform) {
  EXPECT_EQ(cells_differing_from_the_reference_face(kNaplpsGridReference, 'R'),
            0)
      << "the reference receiver drew something other than its own face";
  EXPECT_GT(cells_differing_from_the_reference_face(kNaplpsGridTwice, 'R'), 0)
      << "the 512 by 400 receiver drew the reference face sampled up";
  EXPECT_GT(cells_differing_from_the_reference_face(kNaplpsGridThrice, 'R'), 0)
      << "the 768 by 600 receiver drew the reference face sampled up";
}

// Whichever face a receiver draws from, the character stays inside the field
// the page gave it: a finer pattern is not licence to spill into the character
// beside it.
TEST(NaplpsRasterTest, ACharacterStaysInsideItsFieldAtEveryResolution) {
  for (const NaplpsRenderGrid& grid :
       {kNaplpsGridReference, kNaplpsGridTwice, kNaplpsGridThrice}) {
    const int scale = grid.width / kNaplpsGridReference.width;
    NaplpsCellSurface surface(grid);
    deposit_default_field(surface, 'R');

    const NaplpsGridMapping mapping{grid};
    const int first_column = 16 * scale;
    const int first_row = 100 * scale;
    const int columns =
        static_cast<int>(std::lround(mapping.columns_across(1.0 / 40.0)));
    const int rows =
        static_cast<int>(std::lround(mapping.rows_up(5.0 / 128.0)));

    for (int row = 0; row < grid.height; ++row) {
      for (int column = 0; column < grid.width; ++column) {
        if (!surface.at(column, row).painted) {
          continue;
        }
        EXPECT_GE(column, first_column) << grid.width;
        EXPECT_LT(column, first_column + columns) << grid.width;
        EXPECT_GE(row, first_row) << grid.width;
        EXPECT_LT(row, first_row + rows) << grid.width;
      }
    }
  }
}

// The reference receiver is the era-accurate one, and its generator is the
// 6 by 10 alone: at its own default field the cells it deposits are that face's
// pattern, one element to one pixel, with nothing scaled or resampled.
TEST(NaplpsRasterTest, TheReferenceReceiverDepositsItsFacePatternExactly) {
  NaplpsCellSurface surface(kNaplpsGridReference);
  deposit_default_field(surface, 'R');

  const NaplpsFontFace& face =
      naplps_font_face_for_field(kNaplpsGridReference, 6, 10);
  ASSERT_EQ(std::string(face.name), "misc-fixed 6x10");
  const uint16_t* pattern = naplps_face_pattern(face, U'R');
  ASSERT_NE(pattern, nullptr);

  for (int row = 0; row < face.height; ++row) {
    for (int column = 0; column < face.width; ++column) {
      // The pattern's rows run top down while the field's run up.
      const bool stored =
          (pattern[row] & (1u << (face.width - 1 - column))) != 0u;
      EXPECT_EQ(surface.at(16 + column, 100 + face.height - 1 - row).painted,
                stored)
          << "row " << row << " column " << column;
    }
  }
}

// §5.3.2.3.9's character size is continuously variable, and a receiver meets it
// by depositing the same stored pattern over more pixels — so a field twice the
// size shows the same shape, larger, rather than a different one.
TEST(NaplpsRasterTest, ADoubleSizeFieldScalesTheSamePattern) {
  NaplpsCellSurface normal(kTestGrid);
  NaplpsRasteriser(normal, NaplpsGridMapping{kTestGrid})
      .deposit_character(
          character_at('H', NabtsPrimitive::Repertoire::kPrimary, 2, 2, 6, 10),
          {}, white(), nullptr);

  NaplpsCellSurface doubled(kTestGrid);
  NaplpsRasteriser(doubled, NaplpsGridMapping{kTestGrid})
      .deposit_character(
          character_at('H', NabtsPrimitive::Repertoire::kPrimary, 2, 2, 12, 20),
          {}, white(), nullptr);

  // Four times the area for twice the field in each direction, near enough:
  // nearest-neighbour puts each stored element under a 2 by 2 block.
  EXPECT_NEAR(static_cast<double>(doubled.painted_count()),
              4.0 * static_cast<double>(normal.painted_count()),
              0.35 * 4.0 * static_cast<double>(normal.painted_count()));
}

// §6.2.7.4: reverse video fills the character field and leaves the shape
// undrawn, so the cells that were on and off swap over.
TEST(NaplpsRasterTest, ReverseVideoFillsTheFieldAndLeavesTheShape) {
  NabtsPrimitive normal =
      character_at('H', NabtsPrimitive::Repertoire::kPrimary, 4, 4, 6, 10);
  NabtsPrimitive reversed = normal;
  reversed.reverse_video = true;

  NaplpsInk ground;
  ground.colour = kNabtsNominalBlack;

  NaplpsCellSurface plain(kTestGrid);
  NaplpsRasteriser(plain, NaplpsGridMapping{kTestGrid})
      .deposit_character(normal, {}, white(), &ground);

  NaplpsCellSurface inverted(kTestGrid);
  NaplpsRasteriser(inverted, NaplpsGridMapping{kTestGrid})
      .deposit_character(reversed, {}, white(), &ground);

  // Both cover the same field; a cell white in one is black in the other.
  EXPECT_EQ(plain.painted_count(), inverted.painted_count());
  int swapped = 0;
  for (int row = 4; row < 14; ++row) {
    for (int column = 4; column < 10; ++column) {
      const bool lit_plain = plain.at(column, row).colour == kNabtsNominalWhite;
      const bool lit_inverted =
          inverted.at(column, row).colour == kNabtsNominalWhite;
      if (lit_plain != lit_inverted) {
        ++swapped;
      }
    }
  }
  EXPECT_GT(swapped, 0) << "reverse video drew the same thing as normal video";
}

// §5.4's mosaics are 2 by 3 blocks of the character field, bit 0 top-left
// through bit 5 bottom-right.
TEST(NaplpsRasterTest, AMosaicFillsTheSubElementsItsCodeNames) {
  // Figure 62 puts sixel bits 0 to 4 straight through from the code's low
  // bits, so code 2/1 lights bit 0 alone: the top-left block of the six.
  NabtsPrimitive mosaic =
      character_at(0x21, NabtsPrimitive::Repertoire::kMosaic, 4, 4, 4, 6);

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .deposit_character(mosaic, {}, white(), nullptr);

  // The field is 4 by 6 cells, so each block is 2 by 2 and the top-left one
  // sits at the field's top-left corner.
  EXPECT_EQ(row_pattern(surface, 8, 3, 9), ".##....");
  EXPECT_EQ(row_pattern(surface, 9, 3, 9), ".##....");
  EXPECT_EQ(row_pattern(surface, 7, 3, 9), ".......");
}

// §5.3.2.2.6 lists separated mosaics among the things the logical pel sizes: a
// separated element is shrunk by the pel, which is what opens the gap that
// makes the blocks read as separate.
TEST(NaplpsRasterTest, SeparatedMosaicsAreInsetByThePel) {
  NabtsPrimitive mosaic =
      character_at(0x21, NabtsPrimitive::Repertoire::kMosaic, 4, 4, 4, 6);
  mosaic.logical_pel = pel(1.0, 1.0);
  // §6.2.7.15: underline mode is what puts mosaics into separated mode.
  mosaic.underlined = true;

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .deposit_character(mosaic, {}, white(), nullptr);

  // One cell shrunk off the right and the top of the 2 by 2 block.
  EXPECT_EQ(row_pattern(surface, 8, 3, 9), ".#.....");
  EXPECT_EQ(row_pattern(surface, 9, 3, 9), ".......");
}

// §5.6: a downloadable character is displayed from the buffer its definition
// filled, and one that was never defined is displayed as SPACE.
TEST(NaplpsRasterTest, ADrcsCharacterDepositsItsDefinedBuffer) {
  NabtsDrcsCharacter glyph;
  glyph.code = 0x21;
  glyph.width = 2;
  glyph.height = 2;
  // Row 0 is the buffer's bottom: light the bottom-left and top-right.
  glyph.elements = {true, false, false, true};

  NabtsPrimitive primitive =
      character_at(0x21, NabtsPrimitive::Repertoire::kDrcs, 4, 4, 2, 2);

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .deposit_character(primitive, {glyph}, white(), nullptr);

  EXPECT_TRUE(surface.at(4, 4).painted) << "bottom-left of the buffer";
  EXPECT_TRUE(surface.at(5, 5).painted) << "top-right of the buffer";
  EXPECT_FALSE(surface.at(5, 4).painted);
  EXPECT_FALSE(surface.at(4, 5).painted);

  NaplpsCellSurface undefined(kTestGrid);
  NaplpsRasteriser(undefined, NaplpsGridMapping{kTestGrid})
      .deposit_character(primitive, {}, white(), nullptr);
  EXPECT_EQ(undefined.painted_count(), 0u)
      << "a character never defined is displayed as SPACE";
}

/// The cells of columns |first_column| to |last_column| and rows |first_row| to
/// |last_row|, top row first, as a picture of '#' and '.'.
std::vector<std::string> picture(const NaplpsCellSurface& surface,
                                 int first_column, int last_column,
                                 int first_row, int last_row) {
  std::vector<std::string> out;
  for (int row = last_row; row >= first_row; --row) {
    out.push_back(row_pattern(surface, row, first_column, last_column));
  }
  return out;
}

/// A picture of the inks laid in a row: '.' unpainted, and |letters| naming
/// each ink by its red gun.
std::string ink_pattern(const NaplpsCellSurface& surface, int row,
                        int first_column, int last_column) {
  std::string out;
  for (int column = first_column; column <= last_column; ++column) {
    const NaplpsCell& cell = surface.at(column, row);
    out.push_back(!cell.painted                       ? '.'
                  : cell.colour == kNabtsNominalWhite ? 'W'
                                                      : 'R');
  }
  return out;
}

NaplpsInk red() {
  NaplpsInk ink;
  ink.colour = NabtsColour{0, 15, 0, false};
  return ink;
}

/// The 6 by 10 face's pattern for |code|, top row first.
std::vector<std::string> face_picture(char32_t code) {
  const NaplpsFontFace& face = naplps_font_face(0);
  const uint16_t* pattern = naplps_face_pattern(face, code);
  std::vector<std::string> out;
  for (int row = 0; pattern != nullptr && row < face.height; ++row) {
    std::string line;
    for (int column = 0; column < face.width; ++column) {
      line.push_back(
          (pattern[row] & (1u << (face.width - 1 - column))) != 0u ? '#' : '.');
    }
    out.push_back(line);
  }
  return out;
}

/// |a| with |b| laid over it, picture by picture.
std::vector<std::string> overlaid(std::vector<std::string> a,
                                  const std::vector<std::string>& b) {
  for (size_t row = 0; row < a.size() && row < b.size(); ++row) {
    for (size_t column = 0; column < a[row].size() && column < b[row].size();
         ++column) {
      if (b[row][column] == '#') {
        a[row][column] = '#';
      }
    }
  }
  return a;
}

// A character field is 6.4 cells wide in the default field on the reference
// grid (1/40 of 256), and the cursor advances exactly. Each field covers the
// cells [floor(x0), floor(x0 + dx)) — Appendix B's truncation — so the fields
// of a line share their edges: six cells, six, seven, six, with none left
// between two fields and none claimed by both.
TEST(NaplpsRasterTest, CharacterFieldsTileTheirRowWithNoGapOrOverlap) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  for (int field = 0; field < 4; ++field) {
    // A reverse-video SPACE paints its whole field.
    NabtsPrimitive space =
        character_at(' ', NabtsPrimitive::Repertoire::kPrimary,
                     1.0 + 6.4 * field, 2, 6.4, 10);
    space.reverse_video = true;
    raster.deposit_character(space, {}, field % 2 == 0 ? white() : red(),
                             nullptr);
  }
  EXPECT_EQ(ink_pattern(surface, 5, 0, 27), ".WWWWWWRRRRRRWWWWWWWRRRRRR..");
}

// The pattern is laid at a whole multiple of its size from the field's own
// cells, so a letter is the same shape in the seven-cell fields of a line as
// in the six-cell ones, and a field wider than the pattern leaves its spare
// columns as space rather than stretching the letter into them.
TEST(NaplpsRasterTest, ALetterKeepsItsShapeInAWiderField) {
  const std::vector<std::string> h = {"......", "#...#.", "#...#.", "#...#.",
                                      "#####.", "#...#.", "#...#.", "#...#.",
                                      "......", "......"};
  std::vector<std::string> h_seven;
  for (const std::string& row : h) {
    h_seven.push_back(row + ".");
  }

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  // [6.4, 12.8) is six cells; [12.8, 19.2) is seven.
  raster.deposit_character(
      character_at('H', NabtsPrimitive::Repertoire::kPrimary, 6.4, 2, 6.4, 10),
      {}, white(), nullptr);
  raster.deposit_character(
      character_at('H', NabtsPrimitive::Repertoire::kPrimary, 12.8, 2, 6.4, 10),
      {}, white(), nullptr);
  EXPECT_EQ(picture(surface, 6, 11, 2, 11), h);
  EXPECT_EQ(picture(surface, 12, 18, 2, 11), h_seven);

  // A field 7.5 cells wide holds the six-cell pattern once, not stretched.
  NaplpsCellSurface wide(kTestGrid);
  NaplpsRasteriser(wide, NaplpsGridMapping{kTestGrid})
      .deposit_character(character_at('H', NabtsPrimitive::Repertoire::kPrimary,
                                      4, 2, 7.5, 10),
                         {}, white(), nullptr);
  EXPECT_EQ(picture(wide, 4, 10, 2, 11), h_seven);
}

// §5.3.2.3.2: "Rotation causes the character field and the cursor to rotate
// counterclockwise about the character field origin", which is the field's
// lower left corner at 0 degrees. Figure 17 has the rotated fields around the
// origin: at 90 degrees above it and to the left, the letter's top pointing
// left; at 180 below and to the left, upside down; at 270 below and to the
// right, its top pointing right.
TEST(NaplpsRasterTest, RotationTurnsTheFieldAboutItsOrigin) {
  const auto rotated = [](NabtsCharRotation rotation) {
    NabtsPrimitive primitive =
        character_at('F', NabtsPrimitive::Repertoire::kPrimary, 16, 12, 6, 10);
    primitive.rotation = rotation;
    NaplpsCellSurface surface(kTestGrid);
    NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
        .deposit_character(primitive, {}, white(), nullptr);
    return surface;
  };

  const NaplpsCellSurface upright = rotated(NabtsCharRotation::kNone);
  EXPECT_EQ(picture(upright, 16, 21, 12, 21), face_picture(U'F'));
  EXPECT_EQ(upright.painted_count(), 14u);

  // A field turned to run back along an axis starts from the cell its origin
  // falls in (column 16 or row 12 here), as both receivers of the day lay it.
  const NaplpsCellSurface quarter = rotated(NabtsCharRotation::k90);
  EXPECT_EQ(
      picture(quarter, 7, 16, 12, 17),
      (std::vector<std::string>{"..........", ".#........", ".#..#.....",
                                ".#..#.....", ".#..#.....", ".#######.."}));
  EXPECT_EQ(quarter.painted_count(), 14u) << "all of it in that field";

  const NaplpsCellSurface half = rotated(NabtsCharRotation::k180);
  EXPECT_EQ(picture(half, 11, 16, 3, 12),
            (std::vector<std::string>{"......", "......", ".....#", ".....#",
                                      ".....#", "..####", ".....#", ".....#",
                                      ".#####", "......"}));
  EXPECT_EQ(half.painted_count(), 14u);

  const NaplpsCellSurface three_quarters = rotated(NabtsCharRotation::k270);
  EXPECT_EQ(
      picture(three_quarters, 16, 25, 7, 12),
      (std::vector<std::string>{"..#######.", ".....#..#.", ".....#..#.",
                                ".....#..#.", "........#.", ".........."}));
  EXPECT_EQ(three_quarters.painted_count(), 14u);
}

// §5.3.2.3.2: the origin is the lower left corner "regardless of the sign of
// the character field dimensions dx and dy", and §5.3.2.3.9: "If dx is
// negative, the character patterns are reflected about the vertical center
// axis of the character field. If dy is negative, ... about the horizontal
// center axis". So a negative dimension mirrors the letter where it stands; it
// does not move the field to the other side of its origin.
TEST(NaplpsRasterTest, ANegativeFieldMirrorsTheLetterInPlace) {
  NaplpsCellSurface across(kTestGrid);
  NaplpsRasteriser(across, NaplpsGridMapping{kTestGrid})
      .deposit_character(
          character_at('F', NabtsPrimitive::Repertoire::kPrimary, 4, 4, -6, 10),
          {}, white(), nullptr);
  EXPECT_EQ(picture(across, 4, 9, 4, 13),
            (std::vector<std::string>{"......", ".#####", ".....#", ".....#",
                                      "..####", ".....#", ".....#", ".....#",
                                      "......", "......"}));
  EXPECT_EQ(across.painted_count(), 14u) << "nothing left of the origin";

  NaplpsCellSurface up(kTestGrid);
  NaplpsRasteriser(up, NaplpsGridMapping{kTestGrid})
      .deposit_character(
          character_at('F', NabtsPrimitive::Repertoire::kPrimary, 4, 4, 6, -10),
          {}, white(), nullptr);
  EXPECT_EQ(picture(up, 4, 9, 4, 13),
            (std::vector<std::string>{"......", "......", "#.....", "#.....",
                                      "#.....", "####..", "#.....", "#.....",
                                      "#####.", "......"}));
  EXPECT_EQ(up.painted_count(), 14u) << "nothing below the origin";
}

// A mosaic is a character pattern like any other, so the same reflection and
// the same place apply to it.
TEST(NaplpsRasterTest, ANegativeFieldMirrorsAMosaicInPlace) {
  // 2/1 lights the top-left element alone.
  NaplpsCellSurface across(kTestGrid);
  NaplpsRasteriser(across, NaplpsGridMapping{kTestGrid})
      .deposit_character(
          character_at(0x21, NabtsPrimitive::Repertoire::kMosaic, 4, 4, -4, 6),
          {}, white(), nullptr);
  EXPECT_EQ(picture(across, 4, 7, 4, 9),
            (std::vector<std::string>{"..##", "..##", "....", "....", "....",
                                      "...."}));

  NaplpsCellSurface up(kTestGrid);
  NaplpsRasteriser(up, NaplpsGridMapping{kTestGrid})
      .deposit_character(
          character_at(0x21, NabtsPrimitive::Repertoire::kMosaic, 4, 4, 4, -6),
          {}, white(), nullptr);
  EXPECT_EQ(picture(up, 4, 7, 4, 9),
            (std::vector<std::string>{"....", "....", "....", "....", "##..",
                                      "##.."}));
}

// §6.2.7.15: in underline mode the primary, supplementary and DRCS characters
// and SPACE "have a line added to their patterns ... starting at the character
// field origin and extending across the entire width (dx dimension) of the
// character field". "Its thickness is determined by the vertical dimension
// (dy) of the logical pel size", and the pel is never less than a pixel
// (§5.3.2.2.6).
TEST(NaplpsRasterTest, UnderlineModeLinesTheFootOfTheField) {
  NabtsPrimitive letter =
      character_at('H', NabtsPrimitive::Repertoire::kPrimary, 4, 4, 6, 10);
  letter.underlined = true;
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .deposit_character(letter, {}, white(), nullptr);
  EXPECT_EQ(picture(surface, 4, 9, 4, 13),
            (std::vector<std::string>{"......", "#...#.", "#...#.", "#...#.",
                                      "#####.", "#...#.", "#...#.", "#...#.",
                                      "......", "######"}));

  // Two cells of pel, and a seven-cell field the line spans all of.
  NabtsPrimitive space =
      character_at(' ', NabtsPrimitive::Repertoire::kPrimary, 12.8, 4, 6.4, 10);
  space.underlined = true;
  space.logical_pel = pel(1, 2);
  NaplpsCellSurface thick(kTestGrid);
  NaplpsRasteriser(thick, NaplpsGridMapping{kTestGrid})
      .deposit_character(space, {}, white(), nullptr);
  EXPECT_EQ(picture(thick, 11, 20, 3, 6),
            (std::vector<std::string>{"..........", ".#######..", ".#######..",
                                      ".........."}));
}

// §6.2.7.15: "Mosaic characters are not underlined" — underline mode puts them
// into separated mode instead (§5.4) — and the space of the mosaic set is not
// subject to it either.
TEST(NaplpsRasterTest, UnderlineModeSeparatesMosaicsRatherThanLiningThem) {
  NabtsPrimitive space =
      character_at(0x20, NabtsPrimitive::Repertoire::kMosaic, 4, 4, 4, 6);
  space.underlined = true;
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .deposit_character(space, {}, white(), nullptr);
  EXPECT_EQ(surface.painted_count(), 0u);
}

// §6.2.7.4: reverse video draws "the pixels surrounding the character shape in
// the character field ... in the drawing color", and mosaics are among the
// characters it applies to. "The pixels of the character shape are not drawn,
// except in color mode 2, when they are drawn in the background color."
TEST(NaplpsRasterTest, AReverseVideoMosaicDrawsTheFieldAroundItsElements) {
  NabtsPrimitive mosaic =
      character_at(0x21, NabtsPrimitive::Repertoire::kMosaic, 4, 4, 4, 6);
  mosaic.reverse_video = true;

  NaplpsCellSurface plain(kTestGrid);
  NaplpsRasteriser(plain, NaplpsGridMapping{kTestGrid})
      .deposit_character(mosaic, {}, white(), nullptr);
  EXPECT_EQ(picture(plain, 4, 7, 4, 9),
            (std::vector<std::string>{"..##", "..##", "####", "####", "####",
                                      "####"}));

  const NaplpsInk ground = red();
  NaplpsCellSurface with_ground(kTestGrid);
  NaplpsRasteriser(with_ground, NaplpsGridMapping{kTestGrid})
      .deposit_character(mosaic, {}, white(), &ground);
  EXPECT_EQ(ink_pattern(with_ground, 9, 4, 7), "RRWW");
  EXPECT_EQ(ink_pattern(with_ground, 7, 4, 7), "WWWW");
}

// §5.4: "the six mosaic elements that compose each character shall completely
// span the given character field", so they divide the field's own cells at
// its halves and thirds. In a 6.4-cell field at x = 1 the left elements are
// cells 1 to 3 and the right ones 4 to 6, and a solid mosaic beside it starts
// at cell 7: no element reaches into its neighbour's cells.
TEST(NaplpsRasterTest, MosaicElementsTileTheirFieldExactly) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  // Top-left alone, then top-right alone, in the same field.
  raster.deposit_character(
      character_at(0x21, NabtsPrimitive::Repertoire::kMosaic, 1, 2, 6.4, 10),
      {}, white(), nullptr);
  raster.deposit_character(
      character_at(0x22, NabtsPrimitive::Repertoire::kMosaic, 1, 2, 6.4, 10),
      {}, red(), nullptr);
  // The top third of ten rows at y = 2 is rows 8 to 11.
  EXPECT_EQ(ink_pattern(surface, 11, 0, 7), ".WWWRRR.");
  EXPECT_EQ(ink_pattern(surface, 8, 0, 7), ".WWWRRR.");
  EXPECT_EQ(ink_pattern(surface, 7, 0, 7), "........");

  // Solid mosaics side by side meet without overlapping.
  NaplpsCellSurface row(kTestGrid);
  NaplpsRasteriser tiles(row, NaplpsGridMapping{kTestGrid});
  tiles.deposit_character(
      character_at(0x7F, NabtsPrimitive::Repertoire::kMosaic, 1, 2, 6.4, 10),
      {}, white(), nullptr);
  tiles.deposit_character(
      character_at(0x7F, NabtsPrimitive::Repertoire::kMosaic, 7.4, 2, 6.4, 10),
      {}, red(), nullptr);
  for (int line = 2; line < 12; ++line) {
    EXPECT_EQ(ink_pattern(row, line, 0, 14), ".WWWWWWRRRRRR..") << line;
  }
}

// §5.6 and §6.2.3: a DRCS buffer holds an element for every pixel of the
// field it was defined in, so at the finer receivers even a normal field's
// buffer is wider than sixteen elements. It is deposited element for element,
// whatever its width.
TEST(NaplpsRasterTest, AWideDrcsCharacterDepositsEveryElement) {
  NabtsDrcsCharacter glyph;
  glyph.code = 0x41;
  glyph.width = 20;
  glyph.height = 3;
  glyph.elements.assign(60, false);
  // Bottom row: the first and last elements. Middle row: every third one.
  // Top row: all of them.
  glyph.elements[0] = true;
  glyph.elements[19] = true;
  for (int column = 0; column < 20; column += 3) {
    glyph.elements[static_cast<size_t>(20 + column)] = true;
  }
  for (int column = 0; column < 20; ++column) {
    glyph.elements[static_cast<size_t>(40 + column)] = true;
  }

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .deposit_character(
          character_at(0x41, NabtsPrimitive::Repertoire::kDrcs, 4, 4, 20, 3),
          {glyph}, white(), nullptr);
  EXPECT_EQ(picture(surface, 3, 24, 4, 6),
            (std::vector<std::string>{".####################.",
                                      ".#..#..#..#..#..#..#..",
                                      ".#..................#."}));
}

// §7.2: an accented letter is sent as the non-spacing mark and then the
// letter, and §5.2 has the mark leave the cursor where it was, so both arrive
// in one field. The receiver draws the one character: here Table 18's "S 4/15
// 4/12", L with caron, from the face's own pattern — whose caron sits beside
// the L, not where the caron drawn on its own sits.
TEST(NaplpsRasterTest, AnAccentComposesOntoTheLetterThatFollowsIt) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  raster.deposit_character(
      character_at(0x4F, NabtsPrimitive::Repertoire::kSupplementary, 4, 4, 6,
                   10),
      {}, white(), nullptr);
  raster.deposit_character(
      character_at('L', NabtsPrimitive::Repertoire::kPrimary, 4, 4, 6, 10), {},
      white(), nullptr);
  EXPECT_EQ(picture(surface, 4, 9, 4, 13), face_picture(U'\u013D'));
}

// Where the face holds no precomposed letter — "x" with a caron is not one of
// Table 18's — the mark is laid over the letter, which is how §7.1 leaves any
// other combination to the implementation.
TEST(NaplpsRasterTest, AMarkWithNoPrecomposedLetterIsLaidOverIt) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  raster.deposit_character(
      character_at(0x4F, NabtsPrimitive::Repertoire::kSupplementary, 4, 4, 6,
                   10),
      {}, white(), nullptr);
  raster.deposit_character(
      character_at('x', NabtsPrimitive::Repertoire::kPrimary, 4, 4, 6, 10), {},
      white(), nullptr);
  EXPECT_EQ(picture(surface, 4, 9, 4, 13),
            overlaid(face_picture(U'x'), face_picture(U'\u030C')));
}

// Table 26 codes a diacritical mark standing alone as the mark followed by
// SPACE ("S 4/2 2/0"), and a mark no letter follows is the same: the mark in
// its spacing form, in the field it was sent to.
TEST(NaplpsRasterTest, AMarkWithNoLetterAfterItTakesItsSpacingForm) {
  NaplpsCellSurface with_space(kTestGrid);
  NaplpsRasteriser spaced(with_space, NaplpsGridMapping{kTestGrid});
  spaced.deposit_character(
      character_at(0x42, NabtsPrimitive::Repertoire::kSupplementary, 4, 4, 6,
                   10),
      {}, white(), nullptr);
  spaced.deposit_character(
      character_at(' ', NabtsPrimitive::Repertoire::kPrimary, 4, 4, 6, 10), {},
      white(), nullptr);
  EXPECT_EQ(picture(with_space, 4, 9, 4, 13), face_picture(U'\u00B4'));

  // No letter at all, and a letter that goes to another field instead.
  NaplpsCellSurface alone(kTestGrid);
  NaplpsRasteriser lone(alone, NaplpsGridMapping{kTestGrid});
  lone.deposit_character(
      character_at(0x42, NabtsPrimitive::Repertoire::kSupplementary, 4, 4, 6,
                   10),
      {}, white(), nullptr);
  EXPECT_EQ(picture(alone, 4, 9, 4, 13), face_picture(U'\u00B4'));
  lone.deposit_character(
      character_at('e', NabtsPrimitive::Repertoire::kPrimary, 10, 4, 6, 10), {},
      white(), nullptr);
  EXPECT_EQ(picture(alone, 4, 9, 4, 13), face_picture(U'\u00B4'));
  EXPECT_EQ(picture(alone, 10, 15, 4, 13), face_picture(U'e'));
}

// Table 27's marks may be combined with any character, an accented one
// included (§7.1): a nonspacing underline before an acute before "e" is the
// accented letter with the line laid under it.
TEST(NaplpsRasterTest, MarksStackOntoOneLetter) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  for (const uint8_t code : {uint8_t{0x4C}, uint8_t{0x42}}) {
    raster.deposit_character(
        character_at(code, NabtsPrimitive::Repertoire::kSupplementary, 4, 4, 6,
                     10),
        {}, white(), nullptr);
  }
  raster.deposit_character(
      character_at('e', NabtsPrimitive::Repertoire::kPrimary, 4, 4, 6, 10), {},
      white(), nullptr);
  EXPECT_EQ(picture(surface, 4, 9, 4, 13),
            overlaid(face_picture(U'\u00E9'), face_picture(U'_')));
}

// The composed character is drawn once, in the letter's attributes, so reverse
// video shows the accented letter's shape cut out of the field — nothing of
// the mark's own reverse-video field is left in it.
TEST(NaplpsRasterTest, AComposedLetterKeepsNothingOfTheSpacingForm) {
  NabtsPrimitive mark = character_at(
      0x4F, NabtsPrimitive::Repertoire::kSupplementary, 4, 4, 6, 10);
  mark.reverse_video = true;
  NabtsPrimitive letter =
      character_at('L', NabtsPrimitive::Repertoire::kPrimary, 4, 4, 6, 10);
  letter.reverse_video = true;

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  raster.deposit_character(mark, {}, white(), nullptr);
  raster.deposit_character(letter, {}, white(), nullptr);

  std::vector<std::string> expected = face_picture(U'\u013D');
  for (std::string& row : expected) {
    for (char& cell : row) {
      cell = cell == '#' ? '.' : '#';
    }
  }
  EXPECT_EQ(picture(surface, 4, 9, 4, 13), expected);
}

// A stage already shown stays shown (§5.3.2.8): a letter drawn after a WAIT
// that fell between it and its mark joins the mark's spacing form rather than
// taking the field back.
TEST(NaplpsRasterTest, AMarkShownBeforeAPauseStaysUnderItsLetter) {
  NabtsPrimitive mark = character_at(
      0x4F, NabtsPrimitive::Repertoire::kSupplementary, 4, 4, 6, 10);
  NabtsPrimitive letter =
      character_at('L', NabtsPrimitive::Repertoire::kPrimary, 4, 4, 6, 10);
  letter.appear_ms = 700;

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  raster.deposit_character(mark, {}, white(), nullptr);
  raster.deposit_character(letter, {}, white(), nullptr);
  EXPECT_EQ(picture(surface, 4, 9, 4, 13),
            overlaid(face_picture(U'L'), face_picture(U'\u02C7')));
}

// Anything drawn over the mark's field before its letter arrives ends the
// wait: the letter is drawn plain, over what is there, rather than composed
// with a mark that has been drawn over.
TEST(NaplpsRasterTest, AFieldDrawnOverBeforeItsLetterIsNotTakenBack) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  raster.deposit_character(
      character_at(0x4F, NabtsPrimitive::Repertoire::kSupplementary, 4, 4, 6,
                   10),
      {}, white(), nullptr);
  surface.deposit(9, 4, white());  // the field's bottom right cell
  raster.deposit_character(
      character_at('L', NabtsPrimitive::Repertoire::kPrimary, 4, 4, 6, 10), {},
      white(), nullptr);
  std::vector<std::string> expected =
      overlaid(face_picture(U'L'), face_picture(U'\u02C7'));
  expected.back().back() = '#';
  EXPECT_EQ(picture(surface, 4, 9, 4, 13), expected);
}

// A supplementary character is drawn from the face like a primary one: S 5/5,
// the musical note, rather than nothing.
TEST(NaplpsRasterTest, ASupplementaryCharacterIsDrawnFromTheFace) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .deposit_character(
          character_at(0x55, NabtsPrimitive::Repertoire::kSupplementary, 4, 4,
                       6, 10),
          {}, white(), nullptr);
  EXPECT_EQ(picture(surface, 4, 9, 4, 13), face_picture(U'\u266A'));
}

/// Table 25's field graphic at |code| in a four by four field at 4, 4.
std::vector<std::string> field_graphic(uint8_t code) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .deposit_character(
          character_at(code, NabtsPrimitive::Repertoire::kSupplementary, 4, 4,
                       4, 4),
          {}, white(), nullptr);
  return picture(surface, 4, 7, 4, 7);
}

// Table 25's notes define the line and triangle graphics by the character
// field: "The full horizontal line extends from the middle of the left edge of
// the character field to the middle of the right edge", the diagonal "from
// the lower left corner ... to the upper right corner", the filled diagonal is
// "a filled triangle occupying the lower right half", and the cross overlays
// the two full lines.
TEST(NaplpsRasterTest, TheLineGraphicsAreDrawnFromTheField) {
  EXPECT_EQ(field_graphic(0x56),
            (std::vector<std::string>{"....", "....", "####", "...."}));
  EXPECT_EQ(field_graphic(0x57),
            (std::vector<std::string>{".#..", ".#..", ".#..", ".#.."}));
  EXPECT_EQ(field_graphic(0x58),
            (std::vector<std::string>{"...#", "..#.", ".#..", "#..."}));
  EXPECT_EQ(field_graphic(0x59),
            (std::vector<std::string>{"#...", ".#..", "..#.", "...#"}));
  EXPECT_EQ(field_graphic(0x5A),
            (std::vector<std::string>{"...#", "..##", ".###", "####"}));
  EXPECT_EQ(field_graphic(0x5B),
            (std::vector<std::string>{"#...", "##..", "###.", "####"}));
  EXPECT_EQ(field_graphic(0x65),
            (std::vector<std::string>{".#..", ".#..", "####", ".#.."}));
}

// Drawn from the field's own cells, a run of full lines joins into one line
// through fields of six and seven cells alike, where a pattern would leave the
// spare seventh column empty.
TEST(NaplpsRasterTest, FullLinesJoinAcrossFields) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  for (int field = 0; field < 3; ++field) {
    raster.deposit_character(
        character_at(0x56, NabtsPrimitive::Repertoire::kSupplementary,
                     1.0 + 6.4 * field, 2, 6.4, 10),
        {}, white(), nullptr);
  }
  // The middle of ten rows at y = 2 is row 6.
  EXPECT_EQ(row_pattern(surface, 6, 0, 20), ".###################.");
  EXPECT_EQ(surface.painted_count(), 19u);
}

// §8.3.3(3): a character a receiver holds no pattern for is presented "as a
// symbol(s) that cannot be confused with any character in the graphic
// character repertoire" — a hollow box a capital high, never a blank.
TEST(NaplpsRasterTest, TheReplacementForAMissingCharacterIsAHollowBox) {
  const NaplpsFontFace& face = naplps_font_face(0);
  const std::vector<uint16_t> box = naplps_replacement_pattern(face);
  ASSERT_EQ(box.size(), static_cast<size_t>(face.height));
  std::vector<std::string> drawn;
  for (const uint16_t row : box) {
    std::string line;
    for (int column = 0; column < face.width; ++column) {
      line.push_back((row & (1u << (face.width - 1 - column))) != 0u ? '#'
                                                                     : '.');
    }
    drawn.push_back(line);
  }
  EXPECT_EQ(drawn, (std::vector<std::string>{
                       "......", "#####.", "#...#.", "#...#.", "#...#.",
                       "#...#.", "#...#.", "#####.", "......", "......"}));
}

// ---------------------------------------------------------------------------
// Run merging
// ---------------------------------------------------------------------------

// Cells of one colour along a row become one run, so a page of flat colour
// costs a run per row rather than one per pixel.
// A filled figure is its enclosed area *and* the region its outline traces
// (§5.3.3.5.1), and the two are worked out differently: the area by asking
// whether a cell's centre is inside, the outline by sweeping the pel along the
// edge. On a diagonal edge those two rules can each miss the same cell — the
// centre falls outside, and the pel, anchored outside the edge by its sign
// (§5.3.2.2.6), never reaches it — which opens a one-cell gap running down the
// diagonal between a figure and its own outline.
//
// The letterforms of the reference ExtraVision service are drawn exactly this
// way: an octagon with a 45-degree corner, filled, with a one-cell pel.
TEST(NaplpsRasterTest, AFilledFigureIsSealedAlongADiagonalEdge) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});

  // Corners cut at 45 degrees, the way a letterform's are.
  const std::vector<NabtsPoint> letterform = {
      at(8.0, 4.0),   at(20.0, 4.0), at(24.0, 8.0), at(24.0, 16.0),
      at(20.0, 20.0), at(8.0, 20.0), at(4.0, 16.0), at(4.0, 8.0)};
  raster.fill_path(letterform, pel(1.0, 1.0), NabtsTexturePattern::kSolid,
                   NabtsSize{}, nullptr, white());

  // No unpainted cell anywhere inside the figure: a gap shows up as an
  // unpainted cell with painted cells on both sides of it along a row.
  for (int row = 0; row < kTestGrid.height; ++row) {
    for (int column = 1; column + 1 < kTestGrid.width; ++column) {
      if (surface.at(column, row).painted) {
        continue;
      }
      bool painted_left = false;
      for (int x = column - 1; x >= 0 && !painted_left; --x) {
        painted_left = surface.at(x, row).painted;
      }
      bool painted_right = false;
      for (int x = column + 1; x < kTestGrid.width && !painted_right; ++x) {
        painted_right = surface.at(x, row).painted;
      }
      EXPECT_FALSE(painted_left && painted_right)
          << "cell " << column << "," << row << " is a hole in the fill:\n  "
          << row_pattern(surface, row, 0, kTestGrid.width - 1);
    }
  }
}

// §5.3.3.5.1's enclosed area is the odd-even one, so a frame traced as a single
// path — round the outside, in through a slit, and round the inside — is a
// frame and not a slab. Its own outline cannot tell the two apart, the slit
// being closed to anything working inward from the edge, so what the path
// encloses has to have a say.
TEST(NaplpsRasterTest, AFilledFrameKeepsItsHole) {
  const std::vector<NabtsPoint> frame = {
      at(2.0, 2.0),  at(30.0, 2.0), at(30.0, 22.0), at(2.0, 22.0),
      at(2.0, 9.0),  at(8.0, 9.0),  at(8.0, 16.0),  at(24.0, 16.0),
      at(24.0, 8.0), at(8.0, 8.0),  at(8.0, 9.0),   at(2.0, 9.0)};

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .fill_path(frame, pel(1.0, 1.0), NabtsTexturePattern::kSolid, NabtsSize{},
                 nullptr, white());

  EXPECT_TRUE(surface.at(4, 12).painted) << "the frame's left bar";
  EXPECT_TRUE(surface.at(28, 12).painted) << "the frame's right bar";
  EXPECT_FALSE(surface.at(16, 12).painted) << "the hole in the middle";
}

// A figure drawn right up to the edge of the display area still has an outside,
// even though the receiver shows none of it. Deciding otherwise — taking the
// edge of the screen for the edge of the figure — let a page's leafy border
// claim the whole screen and paint out everything beneath it.
TEST(NaplpsRasterTest, AFigureDrawnToTheScreenEdgeDoesNotClaimTheScreen) {
  const double across = kTestGrid.width;
  const double up = kTestGrid.height;
  // A band round three sides of the screen, open at the top.
  const std::vector<NabtsPoint> border = {
      at(0.0, 0.0),          at(across, 0.0),
      at(across, up),        at(across - 4.0, up),
      at(across - 4.0, 4.0), at(4.0, 4.0),
      at(4.0, up),           at(0.0, up)};

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .fill_path(border, pel(1.0, 1.0), NabtsTexturePattern::kSolid,
                 NabtsSize{}, nullptr, white());

  EXPECT_TRUE(surface.at(1, 12).painted)
      << "the band up the left of the screen";
  EXPECT_TRUE(surface.at(16, 1).painted) << "the band along the bottom";
  EXPECT_FALSE(surface.at(16, 15).painted) << "the opening in the middle";
}

// A damaged operand can put a vertex many screens away. The figure is worked
// out over a region reaching no more than a screen past each edge, so the part
// of it beyond is cut off open-ended; what enters the screen must still fill
// as the wedge it is, and nothing beside it.
TEST(NaplpsRasterTest, AFigureReachingScreensAwayStillFillsWhatIsShown) {
  const std::vector<NabtsPoint> wedge = {at(-1000.0, 12.5), at(24.0, 4.5),
                                         at(24.0, 20.5)};

  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .fill_path(wedge, pel(1.0, 1.0), NabtsTexturePattern::kSolid, NabtsSize{},
                 nullptr, white());

  EXPECT_TRUE(surface.at(0, 12).painted) << "the wedge entering the screen";
  EXPECT_TRUE(surface.at(16, 12).painted) << "the wedge's middle";
  EXPECT_TRUE(surface.at(23, 6).painted) << "near the wedge's corner";
  EXPECT_FALSE(surface.at(0, 2).painted) << "below the wedge";
  EXPECT_FALSE(surface.at(0, 23).painted) << "above the wedge";
  EXPECT_FALSE(surface.at(28, 12).painted) << "beyond the wedge's end";
}

// The outline of a filled figure and the same path stroked are the same thing:
// §5.3.3.5.1 puts "the region of the outline traced by the logical pel" inside
// the fill, and §5.3.2.2.6 defines that region once, for every drawing
// operation alike. A service draws a map by filling each region and then
// outlining it, so where the two disagree the fill shows past the outline meant
// to cover it — which is what a coloured fringe outside a black coastline is.
TEST(NaplpsRasterTest, AFilledFigureNeverShowsPastItsOwnOutline) {
  const std::vector<NabtsPoint> coastline = {
      at(8.0, 6.0),   at(20.0, 6.0),  at(30.0, 12.0), at(31.0, 14.0),
      at(31.0, 20.0), at(22.0, 23.0), at(12.0, 22.0), at(5.0, 15.0)};

  NaplpsCellSurface filled(kTestGrid);
  NaplpsRasteriser fill_raster(filled, NaplpsGridMapping{kTestGrid});
  fill_raster.fill_path(coastline, pel(1.0, 1.0), NabtsTexturePattern::kSolid,
                        NabtsSize{}, nullptr, white());

  NaplpsCellSurface stroked(kTestGrid);
  NaplpsRasteriser stroke_raster(stroked, NaplpsGridMapping{kTestGrid});
  stroke_raster.stroke_path(coastline, pel(1.0, 1.0), NabtsLineTexture::kSolid,
                            white(), /*closed=*/true);

  // A filled cell the outline does not cover must be one the outline encloses:
  // it shares no edge with the ground, so it cannot be seen from outside the
  // figure. Edges rather than corners, because the outline is a locus stepped
  // one cell at a time and a diagonal step of it meets the ground corner to
  // corner at every tread of the staircase — which is what a line drawn on a
  // grid looks like, not a gap in it.
  constexpr int kSides[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
  for (int row = 0; row < kTestGrid.height; ++row) {
    for (int column = 0; column < kTestGrid.width; ++column) {
      if (!filled.at(column, row).painted || stroked.at(column, row).painted) {
        continue;
      }
      for (const auto& side : kSides) {
        const int x = column + side[0];
        const int y = row + side[1];
        if (!filled.contains(x, y)) {
          continue;
        }
        EXPECT_TRUE(filled.at(x, y).painted || stroked.at(x, y).painted)
            << "the fill shows past its outline at " << column << "," << row
            << ", which is open to " << x << "," << y;
      }
    }
  }
}

TEST(NaplpsRasterTest, IdenticalCellsAlongARowMergeIntoOneRun) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser(surface, NaplpsGridMapping{kTestGrid})
      .stroke_path({at(2.0, 4.0), at(12.0, 4.0)}, NabtsSize{0.0, 0.0},
                   NabtsLineTexture::kSolid, white());

  const std::vector<NaplpsCellRun> runs = naplps_merge_runs(surface);
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].row, 4);
  EXPECT_EQ(runs[0].column, 2);
  EXPECT_EQ(runs[0].columns, 11);
}

// An unpainted cell breaks a run and produces none of its own: a page does not
// cover the screen, and emitting its ground would paint over whatever a
// renderer puts behind it.
TEST(NaplpsRasterTest, UnpaintedCellsBreakRunsAndEmitNothing) {
  NaplpsCellSurface surface(kTestGrid);
  NaplpsRasteriser raster(surface, NaplpsGridMapping{kTestGrid});
  raster.stamp_pel(at(2.5, 4.5), NabtsSize{0.0, 0.0}, white());
  raster.stamp_pel(at(6.5, 4.5), NabtsSize{0.0, 0.0}, white());

  const std::vector<NaplpsCellRun> runs = naplps_merge_runs(surface);
  ASSERT_EQ(runs.size(), 2u);
  EXPECT_EQ(runs[0].column, 2);
  EXPECT_EQ(runs[0].columns, 1);
  EXPECT_EQ(runs[1].column, 6);
}

// A receiver stores the colour map address (§5.3.2.5), and a map write or a
// blink process changes an entry rather than a colour, so two cells of the same
// colour drawn from different entries can change apart and cannot share a run.
TEST(NaplpsRasterTest, CellsOfDifferentEntriesDoNotShareARun) {
  NaplpsCellSurface surface(kTestGrid);

  NaplpsInk steady = white();
  steady.colour_map_address = 3;
  NaplpsInk blinking = white();
  blinking.colour_map_address = 4;

  surface.deposit(4, 6, steady);
  surface.deposit(5, 6, blinking);

  const std::vector<NaplpsCellRun> runs = naplps_merge_runs(surface);
  ASSERT_EQ(runs.size(), 2u);
  EXPECT_EQ(runs[0].cell.colour_map_address, 3);
  EXPECT_EQ(runs[1].cell.colour_map_address, 4);
}

// A stage of a page drawn over time emits only what it changed: a cell drawn
// over with what it already held, and the cells it never touched, are not
// drawn again.
TEST(NaplpsRasterTest, ChangedRunsAreOnlyWhatAStageChanged) {
  NaplpsCellSurface before(kTestGrid);
  before.deposit(4, 6, white());
  before.deposit(5, 6, white());
  NaplpsCellSurface now = before;
  now.forget_dirty();
  NaplpsInk red;
  red.colour = NabtsColour{0, 15, 0, false};
  now.deposit(5, 6, red);      // changed
  now.deposit(6, 6, red);      // new
  now.deposit(4, 6, white());  // drawn over with what it held

  const std::vector<NaplpsCellRun> runs = naplps_changed_runs(now, before);
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].column, 5);
  EXPECT_EQ(runs[0].columns, 2);
  EXPECT_EQ(runs[0].row, 6);

  // A row the stage never touched is not looked at.
  now.forget_dirty();
  EXPECT_TRUE(naplps_changed_runs(now, NaplpsCellSurface(kTestGrid)).empty());
}

// ---------------------------------------------------------------------------
// Emission over time
// ---------------------------------------------------------------------------

/// A one-cell point at cell (|column|, |row|), drawn at |appear_ms|.
NabtsPrimitive point_at(int column, int row, uint32_t appear_ms,
                        const NabtsColour& colour = kNabtsNominalWhite) {
  NabtsPrimitive point;
  point.kind = NabtsPrimitiveKind::kPoint;
  point.colour = colour;
  point.points = {at(column + 0.5, row + 0.5)};
  point.origin = point.points.front();
  point.appear_ms = appear_ms;
  return point;
}

/// The ops of |list| that cover cell (|column|, |row|), in paint order.
std::vector<const CatalogueDrawOp*> ops_at(const CatalogueDisplayList& list,
                                           int column, int row) {
  std::vector<const CatalogueDrawOp*> out;
  const double x = (column + 0.5) * kCell;
  const double y = (row + 0.5) * kCell;
  for (const CatalogueDrawOp& op : list.ops) {
    if (x >= op.origin.x && x < op.origin.x + op.size.dx && y >= op.origin.y &&
        y < op.origin.y + op.size.dy) {
      out.push_back(&op);
    }
  }
  return out;
}

// A page drawn at once comes out as one stage, exactly the runs of its final
// surface, and says nothing about time.
TEST(NaplpsRasterViewTest, APageDrawnAtOnceIsOneStage) {
  NabtsPageSnapshot page;
  page.primitives = {point_at(2, 3, 0), point_at(3, 3, 0)};

  CatalogueDisplayList list;
  naplps_emit_raster_page(page, kTestGrid, list);
  ASSERT_EQ(list.ops.size(), 1u) << "two white cells side by side are one run";
  EXPECT_EQ(list.ops[0].appear_ms, 0u);
  EXPECT_FALSE(list.timed());
  EXPECT_EQ(list.palette.size(), kNabtsColourMapEntries);
  EXPECT_TRUE(list.palette_tracks.empty());
}

// §5.3.2.8 WAIT: what is drawn after a pause appears after it, and drawing
// every operation in order still gives the finished page.
TEST(NaplpsRasterViewTest, AStagedPageEmitsEachStageWhenItIsDrawn) {
  NabtsPageSnapshot page;
  page.primitives = {point_at(2, 3, 0), point_at(8, 3, 500),
                     point_at(2, 3, 900, NabtsColour{0, 15, 0, false})};

  CatalogueDisplayList list;
  naplps_emit_raster_page(page, kTestGrid, list);
  EXPECT_TRUE(list.timed());
  const auto first = ops_at(list, 2, 3);
  ASSERT_EQ(first.size(), 2u);
  EXPECT_EQ(first[0]->appear_ms, 0u);
  EXPECT_EQ(first[1]->appear_ms, 900u);
  EXPECT_EQ(first[1]->colour.red, 255) << "the later stage drew over it";
  const auto second = ops_at(list, 8, 3);
  ASSERT_EQ(second.size(), 1u);
  EXPECT_EQ(second[0]->appear_ms, 500u);
}

// A clear part way through is a black display area at the moment it happened,
// under whatever is drawn after it.
TEST(NaplpsRasterViewTest, AClearIsEmittedWhenItHappened) {
  NabtsPageSnapshot page;
  page.timeline.superseded = {point_at(2, 3, 0)};
  page.timeline.clears = {NabtsDisplayClear{1000, 1}};
  page.primitives = {point_at(8, 3, 1500)};

  CatalogueDisplayList list;
  naplps_emit_raster_page(page, kTestGrid, list);
  const auto cleared = ops_at(list, 2, 3);
  ASSERT_EQ(cleared.size(), 2u);
  EXPECT_EQ(cleared[0]->appear_ms, 0u);
  EXPECT_EQ(cleared[1]->appear_ms, 1000u);
  EXPECT_EQ(cleared[1]->colour.red, 0);
  EXPECT_EQ(cleared[1]->colour.green, 0);
  EXPECT_EQ(cleared[1]->colour.blue, 0);
  const auto later = ops_at(list, 8, 3);
  ASSERT_EQ(later.size(), 2u) << "the clear and the point drawn after it";
  EXPECT_EQ(later[1]->appear_ms, 1500u);
}

// Drawn and cleared within one instant, a figure was never on screen, and a
// clear with nothing shown before it has nothing to clear.
TEST(NaplpsRasterViewTest, WhatAClearRemovesAtOnceIsNeverEmitted) {
  NabtsPageSnapshot page;
  page.timeline.superseded = {point_at(2, 3, 0)};
  page.timeline.clears = {NabtsDisplayClear{0, 1}};
  page.primitives = {point_at(8, 3, 0)};

  CatalogueDisplayList list;
  naplps_emit_raster_page(page, kTestGrid, list);
  EXPECT_TRUE(ops_at(list, 2, 3).empty());
  ASSERT_EQ(list.ops.size(), 1u);
}

// A run names its colour map entry only where the entry changes as the page is
// shown, which is what a renderer follows; one that holds still is its colour.
TEST(NaplpsRasterViewTest, OnlyAChangingEntryIsNamed) {
  NabtsPageSnapshot page;
  nabts_default_colour_map(page.colour_map);
  nabts_default_colour_map(page.timeline.initial_map);
  NabtsPrimitive steady = point_at(2, 3, 0, page.colour_map[5]);
  steady.colour_map_address = 5;
  NabtsPrimitive blinking = point_at(8, 3, 0, page.colour_map[3]);
  blinking.colour_map_address = 3;
  page.primitives = {steady, blinking};
  NabtsBlinkProcess process;
  process.from = 3;
  process.to_address = 0;
  process.on_ms = 400;
  process.off_ms = 600;
  page.timeline.blinks.push_back(process);
  NabtsPaletteEvent start;
  start.kind = NabtsPaletteEvent::Kind::kBlinkStart;
  page.timeline.events.push_back(start);

  CatalogueDisplayList list;
  naplps_emit_raster_page(page, kTestGrid, list);
  ASSERT_EQ(list.palette_tracks.size(), kNabtsColourMapEntries);
  EXPECT_FALSE(list.palette_tracks[3].steps.empty());
  EXPECT_TRUE(list.palette_tracks[5].steps.empty());
  const auto steady_ops = ops_at(list, 2, 3);
  const auto blinking_ops = ops_at(list, 8, 3);
  ASSERT_EQ(steady_ops.size(), 1u);
  ASSERT_EQ(blinking_ops.size(), 1u);
  EXPECT_EQ(steady_ops[0]->colour.palette_index, -1);
  EXPECT_EQ(blinking_ops[0]->colour.palette_index, 3);
  EXPECT_TRUE(list.timed());
}

// X3.110 §5.3.3.5.1: "There is implicit closure between the start point and the
// last vertex specified", so an outlined polygon draws the edge back to its
// start as well as the edges between the vertices it was given.
TEST(NaplpsRasterViewTest, AnOutlinedPolygonDrawsItsClosingEdge) {
  NabtsPageSnapshot page;
  NabtsPrimitive poly;
  poly.kind = NabtsPrimitiveKind::kPolygon;
  poly.colour = kNabtsNominalWhite;
  poly.points = {at(4, 4), at(20, 4), at(20, 16)};
  page.primitives.push_back(poly);

  CatalogueDisplayList list;
  naplps_emit_raster_page(page, kTestGrid, list);
  const auto painted = [&](int column, int row) {
    for (const CatalogueDrawOp& op : list.ops) {
      const auto first = static_cast<int>(std::lround(op.origin.x / kCell));
      const auto at_row = static_cast<int>(std::lround(op.origin.y / kCell));
      const auto count = static_cast<int>(std::lround(op.size.dx / kCell));
      if (at_row == row && column >= first && column < first + count) {
        return op.colour.red != 0;
      }
    }
    return false;
  };
  // The closing edge runs from (20,16) back to (4,4), through (12,10).
  EXPECT_TRUE(painted(12, 10)) << "the polygon was left open";
  // The edges it was given are there too.
  EXPECT_TRUE(painted(12, 4));
  EXPECT_TRUE(painted(20, 10));
}

}  // namespace
}  // namespace orc
