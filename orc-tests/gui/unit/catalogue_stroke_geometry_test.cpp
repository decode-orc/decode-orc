/*
 * File:        catalogue_stroke_geometry_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests for logical-pel stroking of display lists
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "catalogue_stroke_geometry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace gui_unit_test {
namespace {

using orc::gui::PelBrush;
using orc::gui::StrokePiece;

constexpr double kTolerance = 1e-9;

/// Twice the signed area of |polygon|, by the shoelace formula.
double doubled_area(const QPolygonF& polygon) {
  double sum = 0.0;
  for (int i = 0; i < polygon.size(); ++i) {
    const QPointF& a = polygon[i];
    const QPointF& b = polygon[(i + 1) % polygon.size()];
    sum += a.x() * b.y() - b.x() * a.y();
  }
  return sum;
}

void expect_brush(const PelBrush& brush, double left, double right, double top,
                  double bottom) {
  EXPECT_NEAR(brush.left, left, kTolerance);
  EXPECT_NEAR(brush.right, right, kTolerance);
  EXPECT_NEAR(brush.top, top, kTolerance);
  EXPECT_NEAR(brush.bottom, bottom, kTolerance);
}

}  // namespace

// X3.110 §5.3.2.2.6: the drawing point sits at the pel's lower left corner
// when both dimensions are positive, the lower right when dx is negative, the
// upper left when dy is negative, and the upper right when both are. Device y
// runs down, so "up" is a negative offset. A negative dimension is counted
// from the drawing point's own pixel (here one device pixel, the floor), so it
// ends a pixel past the drawing point: both receivers of the day, the Norpak
// TTX 650 and the Panasonic TU-2000x, lay it so when driven through their
// firmware (lines with pels of each sign, laid on their grid), as the pixel
// modes do, where the corner alone would leave the point's own pixel out.
TEST(CatalogueStrokeGeometry, ThePelHangsFromTheCornerItsSignsName) {
  const double unit = 100.0;
  expect_brush(
      orc::gui::pel_brush(orc::CatalogueSize{0.1, 0.2}, unit, unit, 1.0, 1.0),
      0.0, 10.0, -20.0, 0.0);
  expect_brush(
      orc::gui::pel_brush(orc::CatalogueSize{-0.1, 0.2}, unit, unit, 1.0, 1.0),
      -9.0, 1.0, -20.0, 0.0);
  expect_brush(
      orc::gui::pel_brush(orc::CatalogueSize{0.1, -0.2}, unit, unit, 1.0, 1.0),
      0.0, 10.0, -1.0, 19.0);
  expect_brush(
      orc::gui::pel_brush(orc::CatalogueSize{-0.1, -0.2}, unit, unit, 1.0, 1.0),
      -9.0, 1.0, -1.0, 19.0);
}

// The pel "will always map to at least one and possibly many display pixels",
// and a dimensionless one draws "the physical picture elements (pixels) through
// which the infinitely small locus point passes" (§5.3.2.2.6): the pixel the
// point falls in, which Appendix B maps it to by truncation and US 4,454,593
// FIG. 10 by truncating the pel's near corner. An axis finer than the floor is
// therefore widened from the drawing point's corner, not about its centre —
// centred, a dimensionless line straddled two pixels and a receiver's grid
// line came out half a pixel off and blended into both.
TEST(CatalogueStrokeGeometry, AFinePelIsWidenedFromTheDrawingPointsCorner) {
  expect_brush(
      orc::gui::pel_brush(orc::CatalogueSize{0.0, 0.0}, 100.0, 100.0, 1.0, 1.0),
      0.0, 1.0, -1.0, 0.0);
  // 0.4 device pixels across, widened to 1 from x = 0.
  expect_brush(orc::gui::pel_brush(orc::CatalogueSize{0.004, 0.0}, 100.0, 100.0,
                                   1.0, 1.0),
               0.0, 1.0, -1.0, 0.0);
  // Exactly one floor wide is already in its corner.
  expect_brush(orc::gui::pel_brush(orc::CatalogueSize{0.01, 0.01}, 100.0, 100.0,
                                   1.0, 1.0),
               0.0, 1.0, -1.0, 0.0);
  // A fine negative pel is the drawing point's own pixel too.
  expect_brush(orc::gui::pel_brush(orc::CatalogueSize{-0.004, -0.004}, 100.0,
                                   100.0, 1.0, 1.0),
               0.0, 1.0, -1.0, 0.0);
  // The floors are per axis: one pixel of a grid that is not square.
  const PelBrush wide =
      orc::gui::pel_brush(orc::CatalogueSize{0.0, 0.0}, 100.0, 100.0, 4.0, 2.0);
  expect_brush(wide, 0.0, 4.0, -2.0, 0.0);
  EXPECT_EQ(wide.cell_x, 4.0) << "the brush carries the grid's pixel";
  EXPECT_EQ(wide.cell_y, 2.0);
}

// A brush that names no cell sweeps the pel whole: the hull of the pel at both
// ends, along an axis a rectangle one pel longer than the line, and on a
// diagonal the pel's area plus its breadth across the line times the line's
// length.
TEST(CatalogueStrokeGeometry, TheSweptPelIsTheHullOfBothEnds) {
  const PelBrush brush{0.0, 2.0, -2.0, 0.0};

  const QPolygonF level =
      orc::gui::swept_pel(QPointF(0.0, 0.0), QPointF(10.0, 0.0), brush);
  EXPECT_EQ(level.boundingRect(), QRectF(0.0, -2.0, 12.0, 2.0));
  EXPECT_NEAR(std::fabs(doubled_area(level)) / 2.0, 24.0, 1e-9);

  const QPolygonF diagonal =
      orc::gui::swept_pel(QPointF(0.0, 0.0), QPointF(10.0, 10.0), brush);
  // 4 for the pel, and 10√2 along by 2√2 across.
  EXPECT_NEAR(std::fabs(doubled_area(diagonal)) / 2.0, 44.0, 1e-9);

  const QPolygonF stamp =
      orc::gui::swept_pel(QPointF(5.0, 5.0), QPointF(5.0, 5.0), brush);
  EXPECT_EQ(stamp.boundingRect(), QRectF(5.0, 3.0, 2.0, 2.0));
}

// A receiver walks a line by "a straight line algorithm" (§5.3.2.2.6), a cell
// at a time along the axis it moves faster on, putting the pel down at each
// step, so a stroke is as heavy across its slope as the pel is across that
// axis — one cell a column on a shallow line as on a level one. At the cells'
// centres that is the pel with half a cell off each end along that axis,
// swept; swept whole, a one-cell pel lay over half as many cells again on a
// diagonal (621 lit pixels against both receivers' 303 on three diagonals,
// driven through their firmware). Where the ends coincide it is the whole pel.
TEST(CatalogueStrokeGeometry, AMovingPelLosesHalfACellAtEachEndAlongItsAxis) {
  const PelBrush cell =
      orc::gui::pel_brush(orc::CatalogueSize{0.0, 0.0}, 1.0, 1.0, 1.0, 1.0);

  const QPolygonF level =
      orc::gui::swept_pel(QPointF(0.0, 0.0), QPointF(10.0, 0.0), cell);
  EXPECT_EQ(level.boundingRect(), QRectF(0.5, -1.0, 10.0, 1.0));

  // At every column a diagonal covers one cell's height, as a level line does:
  // its area is its run times one cell.
  const QPolygonF diagonal =
      orc::gui::swept_pel(QPointF(0.0, 0.0), QPointF(10.0, -10.0), cell);
  EXPECT_NEAR(std::fabs(doubled_area(diagonal)) / 2.0, 10.0, 1e-9);
  const QPolygonF steep =
      orc::gui::swept_pel(QPointF(0.0, 0.0), QPointF(3.0, -12.0), cell);
  EXPECT_NEAR(std::fabs(doubled_area(steep)) / 2.0, 12.0, 1e-9)
      << "a line nearer upright steps along y and is one cell wide";

  const QPolygonF stamp =
      orc::gui::swept_pel(QPointF(5.0, 5.0), QPointF(5.0, 5.0), cell);
  EXPECT_EQ(stamp.boundingRect(), QRectF(5.0, 4.0, 1.0, 1.0));

  // A pel wider than a cell keeps all but the cell's half at each end.
  PelBrush wide{0.0, 3.0, -3.0, 0.0};
  wide.cell_x = 1.0;
  wide.cell_y = 1.0;
  EXPECT_EQ(orc::gui::swept_pel(QPointF(0.0, 0.0), QPointF(10.0, 0.0), wide)
                .boundingRect(),
            QRectF(0.5, -3.0, 12.0, 3.0));
}

// Every sweep winds the same way, which is what lets any number of them be
// filled together as their union under Qt::WindingFill.
TEST(CatalogueStrokeGeometry, EverySweepWindsTheSameWay) {
  PelBrush stepped{-1.0, 3.0, -2.0, 1.0};
  stepped.cell_x = 1.0;
  stepped.cell_y = 1.0;
  const std::vector<QPointF> ends = {QPointF(10.0, 0.0), QPointF(-10.0, 0.0),
                                     QPointF(0.0, 10.0), QPointF(0.0, -10.0),
                                     QPointF(7.0, 3.0),  QPointF(-4.0, -9.0),
                                     QPointF(0.0, 0.0)};
  for (const PelBrush& brush : {PelBrush{-1.0, 3.0, -2.0, 1.0}, stepped}) {
    const double first =
        doubled_area(orc::gui::swept_pel(QPointF(0.0, 0.0), ends[0], brush));
    ASSERT_NE(first, 0.0);
    for (const QPointF& end : ends) {
      const double area =
          doubled_area(orc::gui::swept_pel(QPointF(0.0, 0.0), end, brush));
      EXPECT_GT(area * first, 0.0) << "towards " << end.x() << "," << end.y();
    }
  }
}

// §5.3.2.4.2: pel widths along a horizontal line, pel heights along a
// vertical one, and solid wherever the pel has no extent along the line.
TEST(CatalogueStrokeGeometry, TheTextureStepIsThePelAlongTheLine) {
  EXPECT_NEAR(orc::gui::texture_step(QPointF(1.0, 0.0), 4.0, 6.0), 4.0, 1e-9);
  EXPECT_NEAR(orc::gui::texture_step(QPointF(0.0, -1.0), 4.0, 6.0), 6.0, 1e-9);
  const double diagonal = std::sqrt(0.5);
  // A square pel on a diagonal steps across its own corner.
  EXPECT_NEAR(orc::gui::texture_step(QPointF(diagonal, diagonal), 4.0, 4.0),
              4.0 * std::sqrt(2.0), 1e-9);
  // "If logical pel size dx = 0, all nonvertical lines are solid."
  EXPECT_EQ(orc::gui::texture_step(QPointF(diagonal, diagonal), 0.0, 4.0), 0.0);
  EXPECT_NEAR(orc::gui::texture_step(QPointF(0.0, 1.0), 0.0, 4.0), 4.0, 1e-9);
}

// A dot is one pel stamped once and the gap beside it one pel; a dash is the
// pel swept across two steps, three pels in all, with a gap of three.
TEST(CatalogueStrokeGeometry, TexturesRunInWholePels) {
  const QVector<QPointF> line = {QPointF(0.0, 0.0), QPointF(100.0, 0.0)};

  const auto dotted = orc::gui::textured_pieces(
      line, false, orc::CatalogueLineStyle::kDotted, 10.0, 10.0, 1.0);
  ASSERT_EQ(dotted.size(), 6u);
  for (size_t i = 0; i < dotted.size(); ++i) {
    EXPECT_NEAR(dotted[i].from.x(), 20.0 * i, 1e-9);
    EXPECT_NEAR(dotted[i].to.x(), 20.0 * i, 1e-9) << "a dot is one stamp";
  }

  const auto dashed = orc::gui::textured_pieces(
      line, false, orc::CatalogueLineStyle::kDashed, 10.0, 10.0, 1.0);
  ASSERT_EQ(dashed.size(), 2u);
  EXPECT_NEAR(dashed[0].from.x(), 0.0, 1e-9);
  EXPECT_NEAR(dashed[0].to.x(), 20.0, 1e-9);
  EXPECT_NEAR(dashed[1].from.x(), 60.0, 1e-9);
  EXPECT_NEAR(dashed[1].to.x(), 80.0, 1e-9);

  // A dot, a gap of one, a dash of three, a gap of one: the order the
  // receiver lays a straight line in (receiver_texture()).
  const auto dash_dotted = orc::gui::textured_pieces(
      line, false, orc::CatalogueLineStyle::kDashDotted, 10.0, 10.0, 1.0);
  ASSERT_EQ(dash_dotted.size(), 4u);
  EXPECT_NEAR(dash_dotted[0].from.x(), 0.0, 1e-9);
  EXPECT_NEAR(dash_dotted[0].to.x(), 0.0, 1e-9);
  EXPECT_NEAR(dash_dotted[1].from.x(), 20.0, 1e-9);
  EXPECT_NEAR(dash_dotted[1].to.x(), 40.0, 1e-9);
  EXPECT_NEAR(dash_dotted[2].from.x(), 60.0, 1e-9);
  EXPECT_NEAR(dash_dotted[2].to.x(), 60.0, 1e-9);

  // Each dot and dash is marked as one, so the pel is put down whole at its
  // ends (a moving pel loses half a cell there, see swept_pel()).
  for (const auto& piece : dashed) {
    EXPECT_TRUE(piece.dash);
  }
}

// The texture runs on round a corner rather than restarting, and each segment
// is stepped in its own direction's pel: the second leg of this path is
// vertical, so it steps in pel heights.
TEST(CatalogueStrokeGeometry, TheTextureRunsOnRoundACorner) {
  const QVector<QPointF> path = {QPointF(0.0, 0.0), QPointF(40.0, 0.0),
                                 QPointF(40.0, 60.0)};
  const auto dashed = orc::gui::textured_pieces(
      path, false, orc::CatalogueLineStyle::kDashed, 10.0, 20.0, 1.0);
  // Four pels along the first leg (40 / 10) and three along the second
  // (60 / 20). The dash covers pels 0 to 2, the gap pel 3 and the first two
  // pels of the second leg, and the next dash starts at its third, 40 down.
  ASSERT_EQ(dashed.size(), 2u);
  EXPECT_NEAR(dashed[0].to.x(), 20.0, 1e-9);
  EXPECT_NEAR(dashed[1].from.x(), 40.0, 1e-9);
  EXPECT_NEAR(dashed[1].from.y(), 40.0, 1e-9);
  EXPECT_NEAR(dashed[1].to.y(), 60.0, 1e-9);
}

// Solid wherever the step is too fine to show, and a solid texture covers each
// segment whole — including the one a closed path adds back to its start.
TEST(CatalogueStrokeGeometry, AFineOrSolidTextureCoversEachSegmentWhole) {
  const QVector<QPointF> path = {QPointF(0.0, 0.0), QPointF(50.0, 0.0),
                                 QPointF(50.0, 50.0)};
  const auto fine = orc::gui::textured_pieces(
      path, false, orc::CatalogueLineStyle::kDotted, 0.5, 0.5, 1.0);
  ASSERT_EQ(fine.size(), 2u);
  EXPECT_EQ(fine[0].from, path[0]);
  EXPECT_EQ(fine[0].to, path[1]);

  const auto closed = orc::gui::textured_pieces(
      path, true, orc::CatalogueLineStyle::kSolid, 10.0, 10.0, 1.0);
  ASSERT_EQ(closed.size(), 3u);
  EXPECT_EQ(closed[2].from, path[2]);
  EXPECT_EQ(closed[2].to, path[0]);
  // A whole segment is not a dash: its ends are the caller's to stamp, which
  // it does at the record's own vertices only.
  EXPECT_FALSE(closed[2].dash);
  EXPECT_FALSE(fine[0].dash);
}

namespace {

// The receiver's grid for the registered textures below: cells 4 device pixels
// square, column 0 and row 0 with their lower left corner at (8, 400), rows
// counting up the screen.
const orc::gui::ReceiverGrid kGrid{QPointF(8.0, 400.0), 4.0, 4.0};

/// The device point at the lower left corner of cell (|column|, |row|).
QPointF cell_corner(double column, double row) {
  return QPointF(8.0 + 4.0 * column, 400.0 - 4.0 * row);
}

/// The columns (or, with |by_row|, the rows) at which each piece starts and
/// ends, in cells, to a millionth of one.
std::vector<std::pair<double, double>> cells_of(
    const std::vector<StrokePiece>& pieces, bool by_row = false) {
  const auto cell = [by_row](const QPointF& point) {
    const double at =
        by_row ? (400.0 - point.y()) / 4.0 : (point.x() - 8.0) / 4.0;
    return std::round(at * 1e6) / 1e6;
  };
  std::vector<std::pair<double, double>> out;
  for (const StrokePiece& piece : pieces) {
    EXPECT_TRUE(piece.dash);
    out.emplace_back(cell(piece.from), cell(piece.to));
  }
  return out;
}

}  // namespace

// A straight line's pattern belongs to the screen, not to the line: the Norpak
// TTX 650 lights a cell of a dotted line where its column falls on the
// pattern, one cell every two pels ($ADC8-$AE17, $B0B7-$B219), as the pixel
// modes do. Two dotted lines started a cell apart therefore light the same
// columns, where a pattern counted from each line's start would put their dots
// out of step. The ends are not among the pieces: the caller stamps them.
TEST(CatalogueStrokeGeometry, AGridTextureIsRegisteredToTheScreen) {
  const std::vector<std::pair<double, double>> even = {
      {0, 0}, {2, 2}, {4, 4}, {6, 6}, {8, 8}, {10, 10}, {12, 12}};
  const auto from_zero = orc::gui::textured_pieces(
      {cell_corner(0, 10), cell_corner(12, 10)}, false,
      orc::CatalogueLineStyle::kDotted, 4.0, 4.0, 1.0, &kGrid);
  EXPECT_EQ(cells_of(from_zero), even);

  const auto from_one = orc::gui::textured_pieces(
      {cell_corner(1, 10), cell_corner(12, 10)}, false,
      orc::CatalogueLineStyle::kDotted, 4.0, 4.0, 1.0, &kGrid);
  const std::vector<std::pair<double, double>> even_from_two(even.begin() + 1,
                                                             even.end());
  EXPECT_EQ(cells_of(from_one), even_from_two);

  // Drawn the other way, and off the screen's left edge, the same columns.
  const auto backwards = orc::gui::textured_pieces(
      {cell_corner(4, 10), cell_corner(-3, 10)}, false,
      orc::CatalogueLineStyle::kDotted, 4.0, 4.0, 1.0, &kGrid);
  const std::vector<std::pair<double, double>> around_zero = {
      {-2, -2}, {0, 0}, {2, 2}, {4, 4}};
  auto backwards_cells = cells_of(backwards);
  std::sort(backwards_cells.begin(), backwards_cells.end());
  EXPECT_EQ(backwards_cells, around_zero);
}

// The receiver's periods in whole pels, here two cells: dashed lights two pels
// and a cell in every six, which the two-cell pel laid at each makes a dash of
// six cells and a gap of six; dot-dashed lights a cell, then two pels and a
// cell from the second pel — a dot, a gap, a dash and a gap.
TEST(CatalogueStrokeGeometry, AGridTextureIsTheReceiversPatternInWholePels) {
  const QVector<QPointF> line = {cell_corner(0, 10), cell_corner(24, 10)};
  const auto dashed = orc::gui::textured_pieces(
      line, false, orc::CatalogueLineStyle::kDashed, 8.0, 4.0, 1.0, &kGrid);
  EXPECT_EQ(cells_of(dashed), (std::vector<std::pair<double, double>>{
                                  {0, 4}, {12, 16}, {24, 24}}));

  const auto dash_dotted = orc::gui::textured_pieces(
      line, false, orc::CatalogueLineStyle::kDashDotted, 8.0, 4.0, 1.0, &kGrid);
  EXPECT_EQ(cells_of(dash_dotted),
            (std::vector<std::pair<double, double>>{
                {0, 0}, {4, 8}, {12, 12}, {16, 20}, {24, 24}}));
}

// On a line steeper than 45 degrees the pattern runs by row, in pels of the
// pel's height, and each dot is the line's own point where its row begins.
TEST(CatalogueStrokeGeometry, ASteepGridTextureRunsByRow) {
  const QPointF start = cell_corner(5, 3);
  const QPointF end = cell_corner(8, 15);
  const auto dotted = orc::gui::textured_pieces(
      {start, end}, false, orc::CatalogueLineStyle::kDotted, 4.0, 4.0, 1.0,
      &kGrid);
  EXPECT_EQ(cells_of(dotted, /*by_row=*/true),
            (std::vector<std::pair<double, double>>{
                {4, 4}, {6, 6}, {8, 8}, {10, 10}, {12, 12}, {14, 14}}));
  for (const StrokePiece& piece : dotted) {
    // On the line from (5, 3) to (8, 15): a quarter of a cell across a row.
    const double row = (400.0 - piece.from.y()) / 4.0;
    EXPECT_NEAR((piece.from.x() - 8.0) / 4.0, 5.0 + (row - 3.0) / 4.0,
                kTolerance);
  }
}

// A pattern finer than the device can show is drawn solid, and so is a line the
// pel has no extent along — §5.3.2.4.2's "if logical pel size dx = 0, all
// nonvertical lines are solid" — on a grid as off one.
TEST(CatalogueStrokeGeometry, AGridTextureTooFineOrWithoutAPelIsSolid) {
  const orc::gui::ReceiverGrid fine{QPointF(0.0, 100.0), 0.25, 0.25};
  const QVector<QPointF> line = {QPointF(0.0, 50.0), QPointF(40.0, 50.0)};
  const auto tiny = orc::gui::textured_pieces(
      line, false, orc::CatalogueLineStyle::kDotted, 0.25, 0.25, 1.0, &fine);
  ASSERT_EQ(tiny.size(), 1u);
  EXPECT_EQ(tiny[0].from, line[0]);
  EXPECT_EQ(tiny[0].to, line[1]);
  EXPECT_FALSE(tiny[0].dash);

  const auto no_width = orc::gui::textured_pieces(
      {cell_corner(0, 10), cell_corner(12, 10)}, false,
      orc::CatalogueLineStyle::kDashed, 0.0, 4.0, 1.0, &kGrid);
  ASSERT_EQ(no_width.size(), 1u);
  EXPECT_FALSE(no_width[0].dash);
}

// §5.3.3.3: more than three points are a spline, whose minimum implementation
// is "a series of lines connecting the start, intermediate, and end points";
// two are the line a truncated record leaves.
TEST(CatalogueStrokeGeometry, ASplineIsItsSeriesOfLines) {
  const std::vector<orc::CataloguePoint> spline = {
      {0.1, 0.2}, {0.3, 0.8}, {0.7, 0.8}, {0.9, 0.2}};
  const auto out = orc::gui::arc_polyline(spline, 1e-4);
  ASSERT_EQ(out.size(), spline.size());
  for (size_t i = 0; i < out.size(); ++i) {
    EXPECT_EQ(out[i].x, spline[i].x);
    EXPECT_EQ(out[i].y, spline[i].y);
  }
  EXPECT_EQ(orc::gui::arc_polyline({{0.1, 0.1}, {0.2, 0.2}}, 1e-4).size(), 2u);
}

// "If the three drawing points are colinear, a line is drawn from the start
// point to the end point".
TEST(CatalogueStrokeGeometry, ThreeColinearPointsAreALine) {
  const auto out =
      orc::gui::arc_polyline({{0.1, 0.1}, {0.2, 0.2}, {0.4, 0.4}}, 1e-4);
  ASSERT_EQ(out.size(), 2u);
  EXPECT_EQ(out[0].x, 0.1);
  EXPECT_EQ(out[1].x, 0.4);
}

// A segment of a circle runs from start to end the way round that passes
// through the intermediate point, and keeps within the tolerance of it.
TEST(CatalogueStrokeGeometry, AnArcTakesTheWayRoundThroughItsMiddlePoint) {
  const double tolerance = 1e-4;
  const auto upper = orc::gui::arc_polyline(
      {{0.75, 0.5}, {0.5, 0.75}, {0.25, 0.5}}, tolerance);
  const auto lower = orc::gui::arc_polyline(
      {{0.75, 0.5}, {0.5, 0.25}, {0.25, 0.5}}, tolerance);
  ASSERT_GT(upper.size(), 3u);
  ASSERT_GT(lower.size(), 3u);
  EXPECT_EQ(upper.front().x, 0.75);
  EXPECT_EQ(upper.back().x, 0.25);
  for (const auto& point : upper) {
    EXPECT_GE(point.y, 0.5 - 1e-9);
    EXPECT_NEAR(std::hypot(point.x - 0.5, point.y - 0.5), 0.25, 1e-9);
  }
  for (const auto& point : lower) {
    EXPECT_LE(point.y, 0.5 + 1e-9);
  }
  // Each chord strays from the circle by no more than the tolerance.
  for (size_t i = 0; i + 1 < upper.size(); ++i) {
    const double mid_x = (upper[i].x + upper[i + 1].x) / 2.0;
    const double mid_y = (upper[i].y + upper[i + 1].y) / 2.0;
    EXPECT_LE(0.25 - std::hypot(mid_x - 0.5, mid_y - 0.5), tolerance + 1e-12);
  }
}

// Coincident start and end points make a circle, "the intermediate point
// [defining] the diameter".
TEST(CatalogueStrokeGeometry, CoincidentEndsMakeACircle) {
  const auto out =
      orc::gui::arc_polyline({{0.8, 0.5}, {0.2, 0.5}, {0.8, 0.5}}, 1e-4);
  ASSERT_GT(out.size(), 8u);
  double lowest = 1.0;
  double highest = 0.0;
  for (const auto& point : out) {
    EXPECT_NEAR(std::hypot(point.x - 0.5, point.y - 0.5), 0.3, 1e-9);
    lowest = std::min(lowest, point.y);
    highest = std::max(highest, point.y);
  }
  EXPECT_NEAR(lowest, 0.2, 1e-3) << "the circle does not go all the way round";
  EXPECT_NEAR(highest, 0.8, 1e-3);
}

}  // namespace gui_unit_test
