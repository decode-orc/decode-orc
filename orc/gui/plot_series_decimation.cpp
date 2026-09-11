/*
 * File:        plot_series_decimation.cpp
 * Module:      orc-gui
 * Purpose:     Column decimation for plot series wider than the plot
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "plot_series_decimation.h"

#include <cmath>
#include <cstdint>

namespace orc::gui {

QVector<QPointF> decimateSeriesToColumns(const QVector<QPointF>& data,
                                         double x_min, double x_max,
                                         int column_count) {
  if (column_count <= 0 || data.size() <= column_count || !(x_max > x_min)) {
    return data;
  }

  const double span = x_max - x_min;
  QVector<QPointF> reduced;
  reduced.reserve(static_cast<qsizetype>(column_count) * 2);

  // The extremes of the column being accumulated, as indices into data so the
  // pair can be emitted in the order they occurred.
  std::int64_t current_column = 0;
  int lowest = -1;
  int highest = -1;

  const auto flush = [&]() {
    if (lowest < 0) {
      return;
    }
    if (lowest == highest) {
      reduced.append(data[lowest]);
      return;
    }
    const int first = lowest < highest ? lowest : highest;
    const int second = lowest < highest ? highest : lowest;
    reduced.append(data[first]);
    reduced.append(data[second]);
  };

  for (int i = 0; i < data.size(); ++i) {
    const double normalised = (data[i].x() - x_min) / span;
    std::int64_t column =
        static_cast<std::int64_t>(std::floor(normalised * column_count));
    // The axis maps half-open onto the columns, so a point sitting exactly on
    // x_max lands one past the last one. It belongs to the last column, not to
    // a bucket of its own - unlike a point genuinely off the plot.
    if (column == column_count && normalised <= 1.0) {
      column = column_count - 1;
    }

    if (lowest < 0 || column != current_column) {
      flush();
      current_column = column;
      lowest = i;
      highest = i;
      continue;
    }
    if (data[i].y() < data[lowest].y()) {
      lowest = i;
    }
    if (data[i].y() > data[highest].y()) {
      highest = i;
    }
  }
  flush();

  return reduced;
}

}  // namespace orc::gui
