/*
 * File:        catalogue_stroke_geometry.cpp
 * Module:      orc-gui
 * Purpose:     Logical-pel stroking for the display-list renderer: the pel as a
 *              corner-anchored rectangle, the region it sweeps, line textures
 *              in whole pels or registered to the receiver's grid, and arcs as
 *              polylines (ANSI X3.110)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "catalogue_stroke_geometry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <utility>

namespace orc::gui {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Below this a direction component counts as none, and two points as one.
constexpr double kCoincident = 1e-9;
// Twice the signed area of three points in unit space below which they are
// colinear.
constexpr double kColinear = 1e-12;

// How far a point plotted on a receiver's grid may lie from a cell's edge, in
// cells, and still be read as on it: mapping the point to the device leaves it
// a rounding error off.
constexpr double kOnGridCells = 1e-6;

// An arc is never split into more segments than this. A circle of the whole
// screen at a fraction of a device pixel's tolerance needs a few thousand, and
// the cap only bites on a radius no display could show.
constexpr int kMaxArcSegments = 8192;

/// One axis of the pel from the drawing point, as [low, high] along the axis's
/// own (unit-space) direction, in device pixels: at least one @p cell, laid
/// from the drawing point where @p extent is positive or nothing, and back from
/// the far side of the drawing point's own cell where it is negative.
void lay_pel_axis(qreal extent, qreal cell, qreal& low, qreal& high) {
  const qreal size = std::max(std::fabs(extent), cell);
  if (extent < 0.0) {
    high = cell;
    low = cell - size;
  } else {
    low = 0.0;
    high = size;
  }
}

/// Twice the signed area of the triangle o, a, b.
qreal cross(const QPointF& o, const QPointF& a, const QPointF& b) {
  return (a.x() - o.x()) * (b.y() - o.y()) - (a.y() - o.y()) * (b.x() - o.x());
}

/// The convex hull of @p points by the monotone chain, which winds the same way
/// for every input.
QPolygonF convex_hull(QVector<QPointF> points) {
  std::sort(points.begin(), points.end(),
            [](const QPointF& a, const QPointF& b) {
              return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
            });
  QVector<QPointF> hull;
  hull.reserve(points.size() * 2);
  for (const QPointF& point : points) {
    while (hull.size() >= 2 &&
           cross(hull[hull.size() - 2], hull.back(), point) <= 0.0) {
      hull.pop_back();
    }
    hull.push_back(point);
  }
  const qsizetype lower = hull.size() + 1;
  for (qsizetype i = points.size() - 2; i >= 0; --i) {
    const QPointF& point = points[i];
    while (hull.size() >= lower &&
           cross(hull[hull.size() - 2], hull.back(), point) <= 0.0) {
      hull.pop_back();
    }
    hull.push_back(point);
  }
  if (!hull.isEmpty()) {
    hull.pop_back();  // the first point, reached again
  }
  return QPolygonF(hull);
}

/// One period of a line texture (§5.3.2.4.2, Table 12). A run is named by its
/// first and last pel, because what a run of N pels means is the pel swept from
/// its first position to its last — N - 1 steps of travel, which the pel's own
/// extent completes to N pels.
struct TextureRun {
  double first;
  double last;
};
struct TexturePeriod {
  std::vector<TextureRun> on;
  double length = 1.0;
};

TexturePeriod texture_period(orc::CatalogueLineStyle style) {
  switch (style) {
    case orc::CatalogueLineStyle::kSolid:
      break;
    case orc::CatalogueLineStyle::kDotted:
      // "The size of the dot is set equal to the size of the logical pel", and
      // for a horizontal line "the inter-dot spacing is the width of the
      // logical pel".
      return {{{0.0, 0.0}}, 2.0};
    case orc::CatalogueLineStyle::kDashed:
      // "The width (length) of the dash and the inter-dash spacing are equal to
      // three times the width of the logical pel."
      return {{{0.0, 2.0}}, 6.0};
    case orc::CatalogueLineStyle::kDashDotted:
      // A dot, a gap of one, a dash of three, a gap of one — "the
      // inter-dot-dash spacing is equivalent to the inter-dot spacing" — in
      // the order the receiver lays a straight line (receiver_texture()), so
      // a curve starts its pattern as a line and the pixel modes' arcs do.
      return {{{0.0, 0.0}, {2.0, 4.0}}, 6.0};
  }
  return {{{0.0, 0.0}}, 1.0};
}

/// One period of a line texture as the Norpak TTX 650 lays it on its screen
/// ($B0B7-$B219), in cells along a line's major axis for a pel |pel| cells long
/// on that axis: the runs of cells it lights, by their first and last cell.
struct ReceiverTexture {
  std::vector<std::pair<int64_t, int64_t>> lit;
  int64_t period = 1;
};

ReceiverTexture receiver_texture(orc::CatalogueLineStyle style, int64_t pel) {
  switch (style) {
    case orc::CatalogueLineStyle::kSolid:
      break;
    case orc::CatalogueLineStyle::kDotted:
      // One cell every two pels: with the pel laid there, a dot one pel long
      // and a gap of one.
      return {{{0, 0}}, 2 * pel};
    case orc::CatalogueLineStyle::kDashed:
      // Two pels and a cell every six: a dash three pels long and a gap of
      // three.
      return {{{0, 2 * pel}}, 6 * pel};
    case orc::CatalogueLineStyle::kDashDotted:
      // Every six pels one cell, and from the second pel two pels and a cell: a
      // dot, a gap of a pel, a dash of three and a gap of one.
      return {{{0, 0}, {2 * pel, 4 * pel}}, 6 * pel};
  }
  return {{{0, 0}}, 1};
}

/// The largest whole number of |period|s at or below |cell|.
int64_t periods_below(int64_t cell, int64_t period) {
  const int64_t quotient = cell / period;
  return (cell % period != 0 && cell < 0) ? quotient - 1 : quotient;
}

/**
 * @brief The dots and dashes of the straight line from @p a to @p b,
 *        registered to @p grid as the receiver lays them
 *
 * The receiver steps a line one cell at a time along its major axis — the
 * cells' columns, or their rows on a line steeper than 45 degrees, counted
 * between the cells the two ends are in — and lights each cell whose column or
 * row falls on a lit part of the pattern (see textured_pieces()). Each run of
 * lit cells becomes one piece of the line itself, between its points at the
 * near edges of the run's first and last cell along that axis, so the pel laid
 * from them covers the run's cells.
 *
 * @return False, adding nothing, where the pattern's pel would come out finer
 *         than @p finest_step device pixels along the axis, or the ends lie
 *         nowhere on the grid: the caller draws the line solid.
 */
bool append_registered_pieces(const QPointF& a, const QPointF& b,
                              orc::CatalogueLineStyle style, qreal pel_width,
                              qreal pel_height, const ReceiverGrid& grid,
                              qreal finest_step,
                              std::vector<StrokePiece>& pieces) {
  if (!(grid.cell_x > 0.0) || !(grid.cell_y > 0.0)) {
    return false;
  }
  // Where each end lies on the grid, in cells; rows count up the screen.
  const double from_x = (a.x() - grid.origin.x()) / grid.cell_x;
  const double from_y = (grid.origin.y() - a.y()) / grid.cell_y;
  const double to_x = (b.x() - grid.origin.x()) / grid.cell_x;
  const double to_y = (grid.origin.y() - b.y()) / grid.cell_y;
  // A grid of any size the page could be drawn at is far smaller than this;
  // past it a cell number would not fit the arithmetic below.
  constexpr double kFarthestCell = 1e9;
  for (const double at : {from_x, from_y, to_x, to_y}) {
    if (!(std::fabs(at) < kFarthestCell)) {
      return false;
    }
  }
  const auto cell_of = [](double at) {
    return static_cast<int64_t>(std::floor(at + kOnGridCells));
  };
  const int64_t column_from = cell_of(from_x);
  const int64_t row_from = cell_of(from_y);
  const int64_t column_to = cell_of(to_x);
  const int64_t row_to = cell_of(to_y);
  const bool steep =
      std::llabs(row_to - row_from) > std::llabs(column_to - column_from);

  // The pel along that axis in whole cells, never fewer than one, which is
  // what the receiver counts its pattern in (the pixel modes' pel_span()).
  const qreal cell = steep ? grid.cell_y : grid.cell_x;
  const int64_t pel = std::max<int64_t>(
      1, std::llround(std::fabs(steep ? pel_height : pel_width) / cell));
  if (!(static_cast<qreal>(pel) * cell >= finest_step)) {
    return false;
  }
  const ReceiverTexture texture = receiver_texture(style, pel);

  const double start = steep ? from_y : from_x;
  const double end = steep ? to_y : to_x;
  const int64_t first_cell =
      std::min(steep ? row_from : column_from, steep ? row_to : column_to);
  const int64_t last_cell =
      std::max(steep ? row_from : column_from, steep ? row_to : column_to);
  // The line's own point where cell |at| begins along the major axis.
  const auto point_at = [&](int64_t at) {
    const double t =
        std::fabs(end - start) > kCoincident
            ? std::clamp((static_cast<double>(at) - start) / (end - start), 0.0,
                         1.0)
            : 0.0;
    return QPointF(a.x() + (b.x() - a.x()) * t, a.y() + (b.y() - a.y()) * t);
  };
  for (int64_t base =
           periods_below(first_cell, texture.period) * texture.period;
       base <= last_cell; base += texture.period) {
    for (const auto& [on, off] : texture.lit) {
      const int64_t first = std::max(first_cell, base + on);
      const int64_t last = std::min(last_cell, base + off);
      if (last >= first) {
        pieces.push_back(
            StrokePiece{point_at(first), point_at(last), /*dash=*/true});
      }
    }
  }
  return true;
}

double distance_between(const orc::CataloguePoint& a,
                        const orc::CataloguePoint& b) {
  return std::hypot(a.x - b.x, a.y - b.y);
}

double normalised_radians(double angle) {
  double out = std::fmod(angle, 2.0 * kPi);
  if (out < 0.0) {
    out += 2.0 * kPi;
  }
  return out;
}

/// How many straight segments keep a polyline on a circle of @p radius within
/// @p tolerance over @p sweep radians. A chord spanning t radians strays from
/// the circle by r(1 - cos(t/2)), about r t^2 / 8 for a small t.
int arc_segments(double radius, double sweep, double tolerance, int fewest) {
  const double step = std::sqrt(8.0 * std::max(tolerance, kCoincident) /
                                std::max(radius, kCoincident));
  // Clamped before it is made an integer: a radius no display could show asks
  // for more segments than an int holds.
  const double wanted = std::ceil(std::fabs(sweep) / step);
  return static_cast<int>(std::clamp(wanted, static_cast<double>(fewest),
                                     static_cast<double>(kMaxArcSegments)));
}

std::vector<orc::CataloguePoint> circle_points(
    const orc::CataloguePoint& centre, double radius, double start_angle,
    double sweep, int segments) {
  std::vector<orc::CataloguePoint> out;
  out.reserve(static_cast<size_t>(segments) + 1);
  for (int i = 0; i <= segments; ++i) {
    const double angle =
        start_angle + sweep * (static_cast<double>(i) / segments);
    out.push_back(orc::CataloguePoint{centre.x + radius * std::cos(angle),
                                      centre.y + radius * std::sin(angle)});
  }
  return out;
}

}  // namespace

PelBrush pel_brush(const orc::CatalogueSize& pel, qreal unit_x, qreal unit_y,
                   qreal floor_x, qreal floor_y) {
  // Across, and up in unit space, from the drawing point by the pel.
  qreal left = 0.0;
  qreal right = 0.0;
  lay_pel_axis(pel.dx * unit_x, floor_x, left, right);
  qreal below = 0.0;
  qreal above = 0.0;
  lay_pel_axis(pel.dy * unit_y, floor_y, below, above);
  // Device y runs down, so what is above the drawing point is a negative
  // offset.
  PelBrush brush{left, right, -above, -below};
  brush.cell_x = floor_x;
  brush.cell_y = floor_y;
  return brush;
}

QPolygonF swept_pel(const QPointF& from, const QPointF& to,
                    const PelBrush& brush) {
  // Half a cell off each end of the pel along the axis the locus moves faster
  // on, which is what a pel put down once a step covers at the cells' centres.
  PelBrush stepped = brush;
  const qreal run = to.x() - from.x();
  const qreal rise = to.y() - from.y();
  if (std::fabs(run) > kCoincident || std::fabs(rise) > kCoincident) {
    if (std::fabs(run) >= std::fabs(rise)) {
      const qreal half = std::min(brush.cell_x, brush.right - brush.left) / 2.0;
      stepped.left += half;
      stepped.right -= half;
    } else {
      const qreal half = std::min(brush.cell_y, brush.bottom - brush.top) / 2.0;
      stepped.top += half;
      stepped.bottom -= half;
    }
  }
  QVector<QPointF> corners;
  corners.reserve(8);
  for (const QPointF& at : {from, to}) {
    corners.push_back(QPointF(at.x() + stepped.left, at.y() + stepped.top));
    corners.push_back(QPointF(at.x() + stepped.right, at.y() + stepped.top));
    corners.push_back(QPointF(at.x() + stepped.right, at.y() + stepped.bottom));
    corners.push_back(QPointF(at.x() + stepped.left, at.y() + stepped.bottom));
  }
  return convex_hull(std::move(corners));
}

void add_swept_pel(QPainterPath& path, const QPointF& from, const QPointF& to,
                   const PelBrush& brush) {
  path.addPolygon(swept_pel(from, to, brush));
  path.closeSubpath();
}

qreal texture_step(const QPointF& direction, qreal pel_width,
                   qreal pel_height) {
  qreal step = std::numeric_limits<qreal>::infinity();
  if (std::fabs(direction.x()) > kCoincident) {
    step = std::min(step, pel_width / std::fabs(direction.x()));
  }
  if (std::fabs(direction.y()) > kCoincident) {
    step = std::min(step, pel_height / std::fabs(direction.y()));
  }
  return std::isfinite(step) ? step : 0.0;
}

std::vector<StrokePiece> textured_pieces(const QVector<QPointF>& path,
                                         bool closed,
                                         orc::CatalogueLineStyle style,
                                         qreal pel_width, qreal pel_height,
                                         qreal finest_step,
                                         const ReceiverGrid* grid) {
  std::vector<StrokePiece> pieces;
  const int count = static_cast<int>(path.size());
  if (count == 0) {
    return pieces;
  }
  if (count == 1) {
    pieces.push_back(StrokePiece{path.front(), path.front()});
    return pieces;
  }

  const bool solid = style == orc::CatalogueLineStyle::kSolid;
  const TexturePeriod period = texture_period(style);

  // How far along the texture the path has got, in pels. Carried from one
  // segment into the next, so the texture does not restart at a vertex.
  double phase = 0.0;

  const int segments = closed ? count : count - 1;
  for (int i = 0; i < segments; ++i) {
    const QPointF a = path[i];
    const QPointF b = path[(i + 1) % count];
    const double run = b.x() - a.x();
    const double rise = b.y() - a.y();
    const double length = std::hypot(run, rise);
    if (!(length > 0.0)) {
      continue;
    }
    const double step = texture_step(QPointF(run / length, rise / length),
                                     pel_width, pel_height);
    if (solid || !(step > 0.0)) {
      pieces.push_back(StrokePiece{a, b});
      continue;
    }
    if (grid != nullptr) {
      if (!append_registered_pieces(a, b, style, pel_width, pel_height, *grid,
                                    finest_step, pieces)) {
        pieces.push_back(StrokePiece{a, b});
      }
      continue;
    }
    if (!(step >= finest_step)) {
      pieces.push_back(StrokePiece{a, b});
      continue;
    }

    // A position |pels| along the segment from its start.
    const auto at = [&](double pels) {
      const double t = std::clamp(pels * step / length, 0.0, 1.0);
      return QPointF(a.x() + run * t, a.y() + rise * t);
    };

    const double entry = phase;
    const double exit = entry + length / step;
    const double first_period = std::floor(entry / period.length);
    const double last_period = std::floor(exit / period.length);
    for (double p = first_period; p <= last_period; p += 1.0) {
      for (const TextureRun& on : period.on) {
        const double first = std::max(p * period.length + on.first, entry);
        const double last = std::min(p * period.length + on.last, exit);
        if (last < first) {
          continue;
        }
        pieces.push_back(
            StrokePiece{at(first - entry), at(last - entry), /*dash=*/true});
      }
    }
    phase = exit;
  }
  return pieces;
}

std::vector<orc::CataloguePoint> arc_polyline(
    const std::vector<orc::CataloguePoint>& control, double tolerance_unit) {
  if (control.size() != 3) {
    // Two points are the line between them, and more than three the spline's
    // series of lines.
    return control;
  }

  const orc::CataloguePoint& start = control[0];
  const orc::CataloguePoint& through = control[1];
  const orc::CataloguePoint& end = control[2];

  if (distance_between(start, end) < kCoincident) {
    // A circle, the intermediate point diametrically opposite the start.
    const orc::CataloguePoint centre{(start.x + through.x) / 2.0,
                                     (start.y + through.y) / 2.0};
    const double radius = distance_between(start, through) / 2.0;
    if (radius < kCoincident) {
      return {start};
    }
    const double start_angle =
        std::atan2(start.y - centre.y, start.x - centre.x);
    return circle_points(
        centre, radius, start_angle, 2.0 * kPi,
        arc_segments(radius, 2.0 * kPi, tolerance_unit, /*fewest=*/3));
  }

  // The circle through all three is their circumcircle, and a zero
  // determinant means there is none: "If the three drawing points are
  // colinear, a line is drawn from the start point to the end point".
  const double ax = start.x;
  const double ay = start.y;
  const double bx = through.x;
  const double by = through.y;
  const double cx = end.x;
  const double cy = end.y;
  const double determinant =
      2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
  if (std::fabs(determinant) < kColinear) {
    return {start, end};
  }
  const double a2 = ax * ax + ay * ay;
  const double b2 = bx * bx + by * by;
  const double c2 = cx * cx + cy * cy;
  const orc::CataloguePoint centre{
      (a2 * (by - cy) + b2 * (cy - ay) + c2 * (ay - by)) / determinant,
      (a2 * (cx - bx) + b2 * (ax - cx) + c2 * (bx - ax)) / determinant};
  const double radius = distance_between(centre, start);

  const auto angle_of = [&centre](const orc::CataloguePoint& point) {
    return std::atan2(point.y - centre.y, point.x - centre.x);
  };
  const double start_angle = angle_of(start);
  const double to_through = normalised_radians(angle_of(through) - start_angle);
  double sweep = normalised_radians(angle_of(end) - start_angle);
  if (to_through > sweep) {
    // Counter-clockwise would reach the end before the intermediate point, so
    // the arc meant is the other way round.
    sweep -= 2.0 * kPi;
  }
  std::vector<orc::CataloguePoint> out =
      circle_points(centre, radius, start_angle, sweep,
                    arc_segments(radius, sweep, tolerance_unit, /*fewest=*/1));
  // The ends are the points the record gave, exactly: the drawing point is
  // left at the end point (§5.3.3.3), and a figure closed by a chord meets it.
  out.front() = start;
  out.back() = end;
  return out;
}

}  // namespace orc::gui
