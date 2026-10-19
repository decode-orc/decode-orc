/*
 * File:        catalogue_stroke_geometry.h
 * Module:      orc-gui
 * Purpose:     Logical-pel stroking for the display-list renderer: the pel as a
 *              corner-anchored rectangle, the region it sweeps, line textures
 *              in whole pels or registered to the receiver's grid, and arcs as
 *              polylines (ANSI X3.110)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef CATALOGUE_STROKE_GEOMETRY_H
#define CATALOGUE_STROKE_GEOMETRY_H

#include <orc/stage/tooling/catalogue_results.h>

#include <QPainterPath>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QVector>
#include <vector>

namespace orc::gui {

/**
 * @brief The logical pel as a brush, in device pixels about the drawing point
 *
 * Device y runs down. The four edges are offsets from the drawing point, so the
 * rectangle the pel covers with the drawing point at p is
 * [p.x + left, p.x + right] x [p.y + top, p.y + bottom].
 */
struct PelBrush {
  qreal left = 0.0;
  qreal right = 0.0;
  qreal top = 0.0;
  qreal bottom = 0.0;
  /// One pixel of the receiver's grid across and down, in device pixels: the
  /// cell a receiver puts the pel down in at each step of a stroke (see
  /// swept_pel()). Zero for a brush that names no grid, which is then swept
  /// whole.
  qreal cell_x = 0.0;
  qreal cell_y = 0.0;

  /// The rectangle the pel covers with the drawing point at @p point.
  QRectF at(const QPointF& point) const {
    return QRectF(QPointF(point.x() + left, point.y() + top),
                  QPointF(point.x() + right, point.y() + bottom));
  }
};

/**
 * @brief The logical pel of X3.110 §5.3.2.2.6 as a device-space brush
 *
 * The pel "is a rectangle whose orientation is fixed with respect to the
 * Cartesian coordinate system", and the drawing point sits at one of its
 * corners: the lower left when both dimensions are positive, the lower right
 * when only dx is negative, the upper left when only dy is negative, and the
 * upper right when both are. That is the rectangle between the drawing point
 * and the drawing point plus the pel.
 *
 * "The logical pel ... will always map to at least one and possibly many
 * display pixels", and a dimensionless one draws "the physical picture
 * elements (pixels) through which the infinitely small locus point passes".
 * The floors are one pixel of the receiver's grid, and an axis narrower than
 * its floor is widened to it from the drawing point's corner, in the pel's own
 * direction — not about the drawing point: the pixel a point is in lies up and
 * to the right of it when the point is on the grid, which is where Appendix B's
 * truncation ("those physical pixels are illuminated to which any portion of
 * the internal representation of the drawing maps ... simply a truncation of
 * the low order 4 bits") and US 4,454,593 FIG. 10 put a dimensionless pel, and
 * where both receivers of the day light it.
 *
 * A negative dimension is laid back from the far side of the drawing point's
 * own pixel, so that pixel is the pel's last on that axis. §5.3.2.2.6 puts the
 * drawing point on the pel's corner, which for a negative size would leave that
 * pixel out; the Norpak TTX 650 and the Panasonic TU-2000x both count the pel
 * from it, as the pixel modes do (see the sink's naplps_raster.cpp), and
 * stripes drawn with a pel one pixel wide and minus one high sit on the rows
 * their coordinates name only so.
 *
 * @param pel      The pel in unit space, y up.
 * @param unit_x   Device pixels per unit of x.
 * @param unit_y   Device pixels per unit of y.
 * @param floor_x  One pixel of the receiver's grid across, in device pixels:
 *                 the narrowest the brush is.
 * @param floor_y  One pixel of the receiver's grid down, in device pixels.
 */
PelBrush pel_brush(const orc::CatalogueSize& pel, qreal unit_x, qreal unit_y,
                   qreal floor_x, qreal floor_y);

/**
 * @brief The region the pel covers moving in a straight line
 *
 * The locus of the drawing point from @p from to @p to, with the pel as the
 * "larger brush" of §5.3.2.2.6. A receiver walks the locus by "a straight line
 * algorithm", one cell at a time along whichever axis it moves faster on, and
 * puts the pel down at each step, so a stroke is as heavy across its slope as
 * the pel is across that axis: a one-pixel pel draws one pixel a column on a
 * line nearer horizontal, as on a level one. Swept continuously, the pel would
 * lie over half as many cells again on a diagonal. At the cells' centres the
 * stepped pel is the pel with half a cell (@ref PelBrush::cell_x or
 * @ref PelBrush::cell_y) taken off each end along that axis, swept: the convex
 * hull of that at both ends. Where the two ends coincide it is the pel itself,
 * whole, and a brush that names no cell is swept whole as well. Every polygon
 * this returns winds the same way, so any number of them filled together under
 * Qt::WindingFill cover exactly their union.
 *
 * Stepping leaves a stroke's ends half a cell short of the pel there: the
 * caller stamps the pel whole at the record's own end points and vertices,
 * which §5.3.2.4.2 has "plotted regardless of the line texture used".
 */
QPolygonF swept_pel(const QPointF& from, const QPointF& to,
                    const PelBrush& brush);

/// Add swept_pel() to @p path as a closed subpath.
void add_swept_pel(QPainterPath& path, const QPointF& from, const QPointF& to,
                   const PelBrush& brush);

/**
 * @brief How far a line runs for its texture to advance one pel, in device
 *        pixels
 *
 * §5.3.2.4.2 sizes a texture in pels along the line — pel widths for a
 * horizontal line, pel heights for a vertical one — and leaves other angles to
 * the implementation. This is the longest step that moves no more than a pel
 * width across and no more than a pel height down, which is exactly the pel
 * width on a horizontal line and the pel height on a vertical one. It is zero,
 * a solid line, exactly where the clause's closing note says: "if logical pel
 * size dx = 0, all nonvertical lines are solid. If logical pel size dy = 0, all
 * nonhorizontal lines are solid."
 *
 * @param direction  A unit vector along the line.
 */
qreal texture_step(const QPointF& direction, qreal pel_width, qreal pel_height);

/**
 * @brief A receiver's pixel grid, in device pixels
 *
 * Column 0 of the grid lies to the right of @ref origin and row 0 above it,
 * rows counting up the screen as unit y does (device y runs down), so the cell
 * a device point is in is how many whole cells it lies right of and above the
 * origin.
 */
struct ReceiverGrid {
  /// Where unit (0, 0) lands: the lower left corner of column 0, row 0.
  QPointF origin;
  /// One pixel of the grid across and down, in device pixels.
  qreal cell_x = 0.0;
  qreal cell_y = 0.0;
};

/// One stretch of a textured stroke the pel is swept along in the line colour.
/// A dot is a stretch whose two ends coincide.
struct StrokePiece {
  QPointF from;
  QPointF to;
  /// A dot or dash of a texture, as opposed to a whole segment drawn solid. A
  /// receiver puts the pel down whole at a dash's first and last pel, so the
  /// pel is stamped there as well as swept between (see swept_pel()).
  bool dash = false;
};

/**
 * @brief The stretches of a path a line texture draws, stepped in whole pels
 *
 * §5.3.2.4.2 (Table 12) counts a texture in pels: a dot is one pel with a gap
 * of one, a dash three with a gap of three, and the dotted-dashed texture puts
 * a dot between dashes with "the inter-dot-dash spacing ... equivalent to the
 * inter-dot spacing". A run of N pels is the pel swept across N - 1 steps of
 * texture_step(), which its own extent completes to N pels: a dash is exactly
 * three pels long and a dot one pel, whichever way the line runs.
 *
 * Without a grid, the texture runs on along the whole path rather than
 * restarting at each vertex, so a polyline or a flattened arc dashes as one
 * line does, and the step is taken afresh for each segment's own direction —
 * along a curve too. A segment whose step is below @p finest_step device
 * pixels is drawn solid: either the pel has no extent along it, which the
 * standard makes solid, or what is left of the texture is too fine to break
 * the line up with. A solid texture gives each segment whole.
 *
 * Given the receiver's @p grid, the path is straight lines between the
 * record's own points, plotted on that grid, and each line's texture is
 * registered to the screen rather than to the line's start, as the Norpak TTX
 * 650 lays it ($ADC8-$AE17, $C288-$C3D4) and the pixel modes draw it: the cell
 * a line passes through at each column — at each row, on a line steeper than
 * 45 degrees — is lit by where that column or row falls in the pattern, which
 * the receiver counts in pels of whole cells along that axis ($B0B7-$B219).
 * Dotted lights one cell every two pels; dashed, two pels and a cell every six;
 * dot-dashed, every six pels one cell and then, from the second pel, two pels
 * and a cell. With the pel laid at each lit cell, that is a dot one pel long
 * and a gap of one, a dash of three pels and a gap of three, and a dot, a gap,
 * a dash and a gap. Each run of lit cells is one piece, from the line's own
 * point where the run's first cell begins along that axis to where its last
 * does, so the dots and dashes of every line of a page fall in step with each
 * other and with the pixel modes'. Each line starts afresh, as the receiver
 * draws each line of a polyline. A line the pel has no extent along is solid,
 * as without a grid, and so is one whose pel would come out finer than
 * @p finest_step device pixels along its axis.
 *
 * The end points and vertices §5.3.2.4.2 requires plotted "regardless of the
 * line texture" are not included; the caller knows which vertices are real
 * and which only flatten a curve.
 *
 * @param path        Device-space vertices.
 * @param closed      Join the last vertex back to the first.
 * @param pel_width   The pel's width in device pixels, without any floor.
 * @param pel_height  The pel's height in device pixels, without any floor.
 * @param grid        The receiver's grid the path's points are plotted on, or
 *                    nullptr for a path that only flattens a curve or a list
 *                    that names no grid.
 */
std::vector<StrokePiece> textured_pieces(const QVector<QPointF>& path,
                                         bool closed,
                                         orc::CatalogueLineStyle style,
                                         qreal pel_width, qreal pel_height,
                                         qreal finest_step,
                                         const ReceiverGrid* grid = nullptr);

/**
 * @brief An ARC's control points as a polyline, in unit space
 *
 * §5.3.3.3 reads a control-point run three ways. Three points are a circle when
 * the start and end coincide, "the intermediate point [defining] the
 * diameter"; a straight line from start to end when the three are colinear;
 * and otherwise the segment of the circle through all three, running from the
 * start to the end the way round that passes through the intermediate point.
 * More than three points are a curvilinear spline, whose "minimum
 * implementation shall be a series of lines connecting the start,
 * intermediate, and end points" — which is what they are returned as, since the
 * smooth form is "reserved for future standardization". Two points, which is
 * what a truncated record leaves, are the line between them.
 *
 * The circle is the unit screen's, so it keeps the shape the page gives it
 * whatever shape the unit screen is displayed at.
 *
 * @param tolerance_unit  The greatest distance the polyline may stray from the
 *                        circle, in unit space.
 */
std::vector<orc::CataloguePoint> arc_polyline(
    const std::vector<orc::CataloguePoint>& control, double tolerance_unit);

}  // namespace orc::gui

#endif  // CATALOGUE_STROKE_GEOMETRY_H
