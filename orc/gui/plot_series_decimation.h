/*
 * File:        plot_series_decimation.h
 * Module:      orc-gui
 * Purpose:     Column decimation for plot series wider than the plot
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_PLOT_SERIES_DECIMATION_H
#define ORC_GUI_PLOT_SERIES_DECIMATION_H

#include <QPointF>
#include <QVector>

namespace orc::gui {

/**
 * @brief Reduce a series to at most two points per pixel column.
 *
 * A plot narrower than its data puts several points in the same column: the
 * path carries every one of them, the rasteriser resolves them all to the same
 * pixels, and the cost is paid again on every replot. Keeping only each
 * column's extremes draws the same picture - the ink in a column spans exactly
 * its minimum to its maximum either way - from a path no longer than twice the
 * plot width.
 *
 * Columns are assigned by x across [@p x_min, @p x_max] and emitted in x
 * order. Within a column the minimum and maximum come out in the order they
 * occurred, so a trace that rises through a column still rises through it.
 * Points outside the range fall in columns of their own rather than being
 * folded into the edge ones, which would let an off-plot value decide what the
 * first or last visible column looks like; the two-per-column bound is
 * therefore over the columns the data actually occupies.
 *
 * Returns @p data unchanged when there is nothing to gain - no more points
 * than columns, a non-positive column count, or an empty x range - because a
 * caller must never lose points it had the resolution to draw.
 */
QVector<QPointF> decimateSeriesToColumns(const QVector<QPointF>& data,
                                         double x_min, double x_max,
                                         int column_count);

}  // namespace orc::gui

#endif  // ORC_GUI_PLOT_SERIES_DECIMATION_H
