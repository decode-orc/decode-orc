/*
 * File:        plot_series_decimation_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests for plot-series column decimation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "plot_series_decimation.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace orc::gui {
namespace {

QVector<QPointF> ramp(int count) {
  QVector<QPointF> points;
  points.reserve(count);
  for (int i = 0; i < count; ++i) {
    points.append(QPointF(static_cast<double>(i), static_cast<double>(i)));
  }
  return points;
}

TEST(PlotSeriesDecimation, LeavesASeriesThePlotCanAlreadyResolveAlone) {
  const QVector<QPointF> points = ramp(40);
  EXPECT_EQ(decimateSeriesToColumns(points, 0.0, 39.0, 40), points);
  EXPECT_EQ(decimateSeriesToColumns(points, 0.0, 39.0, 400), points);
}

TEST(PlotSeriesDecimation, KeepsAtMostTwoPointsPerColumn) {
  const QVector<QPointF> points = ramp(4000);
  const QVector<QPointF> reduced =
      decimateSeriesToColumns(points, 0.0, 3999.0, 300);
  EXPECT_LE(reduced.size(), 2 * 300);
  EXPECT_LT(reduced.size(), points.size());
}

// The reason a column may be cut to two points at all: whatever else it held,
// the ink it draws runs from its lowest value to its highest.
TEST(PlotSeriesDecimation, PreservesEachColumnsExtremes) {
  QVector<QPointF> points;
  // Four points per column across two columns, with the peak and trough in the
  // middle of each so a naive "keep every Nth" would lose them.
  const double values[] = {1.0, 9.0, -4.0, 2.0, 3.0, -7.0, 8.0, 0.0};
  for (int i = 0; i < 8; ++i) {
    points.append(QPointF(static_cast<double>(i), values[i]));
  }

  const QVector<QPointF> reduced = decimateSeriesToColumns(points, 0.0, 8.0, 2);
  ASSERT_EQ(reduced.size(), 4);

  // First column: trough -4 at x=2 occurs after peak 9 at x=1.
  EXPECT_DOUBLE_EQ(reduced[0].y(), 9.0);
  EXPECT_DOUBLE_EQ(reduced[1].y(), -4.0);
  // Second column: trough -7 at x=5 occurs before peak 8 at x=6.
  EXPECT_DOUBLE_EQ(reduced[2].y(), -7.0);
  EXPECT_DOUBLE_EQ(reduced[3].y(), 8.0);
}

// The right-hand edge of the axis is part of the plot, so the point sitting
// exactly on it shares the last column rather than claiming one past the end.
TEST(PlotSeriesDecimation, PutsThePointOnTheRightEdgeInTheLastColumn) {
  const QVector<QPointF> reduced =
      decimateSeriesToColumns(ramp(4000), 0.0, 3999.0, 300);
  EXPECT_LE(reduced.size(), 2 * 300);
  EXPECT_DOUBLE_EQ(reduced.back().x(), 3999.0);
}

TEST(PlotSeriesDecimation, EmitsColumnsInIncreasingX) {
  const QVector<QPointF> reduced =
      decimateSeriesToColumns(ramp(1000), 0.0, 999.0, 50);
  ASSERT_FALSE(reduced.isEmpty());
  EXPECT_TRUE(std::is_sorted(
      reduced.begin(), reduced.end(),
      [](const QPointF& a, const QPointF& b) { return a.x() < b.x(); }));
}

// An off-plot value must not decide what the first visible column looks like,
// so out-of-range points are grouped apart rather than folded into the edges.
TEST(PlotSeriesDecimation, DoesNotLetOutOfRangePointsColourTheEdgeColumns) {
  QVector<QPointF> points;
  points.append(QPointF(-50.0, 1000.0));  // far left of the axis
  for (int i = 0; i < 20; ++i) {
    points.append(QPointF(static_cast<double>(i), 1.0));
  }

  const QVector<QPointF> reduced =
      decimateSeriesToColumns(points, 0.0, 20.0, 2);
  ASSERT_FALSE(reduced.isEmpty());
  EXPECT_DOUBLE_EQ(reduced.front().y(), 1000.0);
  EXPECT_DOUBLE_EQ(reduced.front().x(), -50.0);
  for (int i = 1; i < reduced.size(); ++i) {
    EXPECT_DOUBLE_EQ(reduced[i].y(), 1.0);
  }
}

TEST(PlotSeriesDecimation, ReturnsTheSeriesUnchangedForADegenerateAxis) {
  const QVector<QPointF> points = ramp(1000);
  EXPECT_EQ(decimateSeriesToColumns(points, 5.0, 5.0, 100), points);
  EXPECT_EQ(decimateSeriesToColumns(points, 0.0, 999.0, 0), points);
  EXPECT_EQ(decimateSeriesToColumns(points, 0.0, 999.0, -3), points);
}

}  // namespace
}  // namespace orc::gui
