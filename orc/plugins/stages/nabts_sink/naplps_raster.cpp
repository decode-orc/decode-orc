/*
 * File:        naplps_raster.cpp
 * Module:      nabts_sink stage plugin
 * Purpose:     Deposits NAPLPS drawing primitives into a receiver's pixel grid
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_raster.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <string>
#include <utility>

#include "naplps_font.h"

namespace orc {

namespace {

// Two coordinates closer than this are the same point. Unit space runs 0 to 1
// over at most 512 cells, so this is several orders of magnitude below anything
// the grid can distinguish and exists only to absorb the arithmetic of
// resolving a relative coordinate.
constexpr double kCoincident = 1e-9;

// Below this a determinant means three colinear control points, which §5.3.3.3
// draws as a line rather than as an arc of unbounded radius.
constexpr double kColinear = 1e-12;

constexpr double kPi = 3.14159265358979323846;

// How far the polyline standing in for an arc may stray from it, as a fraction
// of a cell. Well inside one pixel, so the approximation cannot show up in the
// cells that get painted.
constexpr double kArcToleranceCells = 0.2;

// An arc is never split into more segments than this. A circle of the whole
// screen at the finest grid needs a few thousand, and the cap only bites on a
// radius no display could show.
constexpr int kMaxArcSegments = 8192;

// How far past a filled figure its working grid reaches: two cells, so that the
// outline the pel traces lies inside that grid however the path rounds onto
// it.
constexpr double kFillMarginCells = 2.0;

double distance_between(const NabtsPoint& a, const NabtsPoint& b) {
  return std::hypot(a.x - b.x, a.y - b.y);
}

/**
 * @brief The cells one axis of the logical pel occupies
 *
 * How a *brush* is measured onto the grid, which is not the same thing as an
 * area. §5.3.2.2.6 sizes the pel in the drawing space and then maps it to "at
 * least one and possibly many display pixels"; nothing in that sizing refers to
 * where the pel happens to be or which way it is being swept, so nor does this.
 * The count of cells comes from the size alone, rounded to whole cells and
 * never below one.
 *
 * Measuring the brush by what it covers instead would give it an extra cell
 * whenever it straddled a cell boundary, and a line would come out a pixel
 * heavier here than there along its own length.
 */
int pel_span(double extent) {
  return std::max(1, static_cast<int>(std::lround(std::fabs(extent))));
}

/**
 * @brief The first cell of one axis of the pel at |position|
 *
 * The pel is laid from the cell the drawing point falls in, forward along an
 * axis where its size there is positive and back where it is negative.
 * §5.3.2.2.6 puts the drawing point on a corner of the pel, which for a
 * negative size would leave the drawing point's own cell out; both receivers of
 * the day count a negative pel from that cell instead — stripes drawn with a
 * pel one pixel wide and minus one high sit on the row their coordinate names
 * on both — as they count a FIELD of negative size (see the
 * interpreter's active_field_area()).
 */
int pel_anchor(double position, double extent) {
  const int own = static_cast<int>(std::floor(position));
  return extent < 0.0 ? own - pel_span(extent) + 1 : own;
}

double normalised_radians(double angle) {
  double out = std::fmod(angle, 2.0 * kPi);
  if (out < 0.0) {
    out += 2.0 * kPi;
  }
  return out;
}

/**
 * @brief The polyline standing in for an arc of a circle
 *
 * |sweep| radians of the circle of |radius| about |centre| from |start_angle|,
 * counter-clockwise where positive: vertices close enough together to keep
 * within |tolerance_unit| of it, and one at every quarter-turn point the sweep
 * passes, where the circle reaches furthest along an axis. Both receivers of
 * the day light those extreme pixels; chords between evenly spaced vertices
 * fall just short of them, and the figure comes out a pixel narrow.
 */
std::vector<NabtsPoint> arc_points(const NabtsPoint& centre, double radius,
                                   double start_angle, double sweep,
                                   double tolerance_unit, int min_segments) {
  // Chord sagitta of a segment spanning t radians is r(1 - cos(t/2)), which for
  // a small t is about r t^2 / 8; inverting that gives the step that keeps the
  // polyline within tolerance.
  const double step = std::sqrt(8.0 * tolerance_unit / radius);
  int segments =
      step > 0.0 ? static_cast<int>(std::ceil(std::fabs(sweep) / step)) : 1;
  segments = std::clamp(segments, min_segments, kMaxArcSegments);

  std::vector<double> angles;
  angles.reserve(static_cast<size_t>(segments) + 5);
  for (int i = 0; i <= segments; ++i) {
    angles.push_back(start_angle + sweep * (static_cast<double>(i) /
                                            static_cast<double>(segments)));
  }
  const double quarter = kPi / 2.0;
  const double low = std::min(start_angle, start_angle + sweep);
  const double high = std::max(start_angle, start_angle + sweep);
  for (double at = std::floor(low / quarter + 1.0) * quarter; at < high;
       at += quarter) {
    angles.push_back(at);
  }
  // In the order the sweep runs, a quarter-turn point that falls on a vertex
  // already there taken once.
  if (sweep >= 0.0) {
    std::sort(angles.begin(), angles.end());
  } else {
    std::sort(angles.begin(), angles.end(), std::greater<>());
  }
  std::vector<NabtsPoint> out;
  out.reserve(angles.size());
  for (size_t i = 0; i < angles.size(); ++i) {
    if (i > 0 && std::fabs(angles[i] - angles[i - 1]) < kCoincident) {
      continue;
    }
    out.push_back(NabtsPoint{centre.x + radius * std::cos(angles[i]),
                             centre.y + radius * std::sin(angles[i])});
  }
  return out;
}

/**
 * @brief The arc of a circle running start to end the way round that passes
 *        through |through|
 *
 * Three points on a circle fix its centre as their circumcentre. Which of the
 * two arcs between the ends is meant is then decided by the intermediate point:
 * the sweep is taken counter-clockwise unless that would leave the intermediate
 * point on the other arc.
 */
std::vector<NabtsPoint> arc_segment(const NabtsPoint& start,
                                    const NabtsPoint& through,
                                    const NabtsPoint& end,
                                    double tolerance_unit) {
  // Circumcentre by the perpendicular-bisector determinant.
  const double ax = start.x;
  const double ay = start.y;
  const double bx = through.x;
  const double by = through.y;
  const double cx = end.x;
  const double cy = end.y;

  const double d = 2.0 * (ax * (by - cy) + bx * (cy - ay) + cx * (ay - by));
  if (std::fabs(d) < kColinear) {
    // §5.3.3.3: "If the three drawing points are colinear, a line is drawn from
    // the start point to the end point".
    return {start, end};
  }

  const double a_sq = ax * ax + ay * ay;
  const double b_sq = bx * bx + by * by;
  const double c_sq = cx * cx + cy * cy;
  const NabtsPoint centre{
      (a_sq * (by - cy) + b_sq * (cy - ay) + c_sq * (ay - by)) / d,
      (a_sq * (cx - bx) + b_sq * (ax - cx) + c_sq * (bx - ax)) / d};

  const double radius = distance_between(centre, start);
  if (radius < kCoincident) {
    return {start, end};
  }

  const auto angle_of = [&centre](const NabtsPoint& point) {
    return std::atan2(point.y - centre.y, point.x - centre.x);
  };
  const double start_angle = angle_of(start);
  const double sweep_to_through =
      normalised_radians(angle_of(through) - start_angle);
  double sweep = normalised_radians(angle_of(end) - start_angle);
  if (sweep_to_through > sweep) {
    // Counter-clockwise would pass the end before reaching the intermediate
    // point, so the arc meant is the other way round.
    sweep -= 2.0 * kPi;
  }
  return arc_points(centre, radius, start_angle, sweep, tolerance_unit, 1);
}

/**
 * @brief The distance along a path that one period of a line texture occupies
 *
 * §5.3.2.4.2 sizes line textures in the logical pel: a dot is one pel, and for
 * a horizontal line the spacings are measured in pel widths while for a
 * vertical line they are measured in pel heights. For a line at any other angle
 * the standard leaves the algorithm to the implementation, asking only for "a
 * visually consistent effect with that specified for horizontal and vertical
 * lines".
 *
 * The step taken here is the longest one that advances no further than a pel
 * width horizontally and no further than a pel height vertically. It is exactly
 * the pel width on a horizontal line and the pel height on a vertical one, and
 * it collapses to zero — a solid line — exactly where §5.3.2.4.2's closing note
 * says it should: "if logical pel size dx = 0, all nonvertical lines are solid.
 * If logical pel size dy = 0, all nonhorizontal lines are solid."
 */
double texture_step(double unit_dx, double unit_dy, const NaplpsCellSize& pel) {
  double step = std::numeric_limits<double>::infinity();
  if (std::fabs(unit_dx) > kCoincident) {
    step = std::min(step, pel.columns / std::fabs(unit_dx));
  }
  if (std::fabs(unit_dy) > kCoincident) {
    step = std::min(step, pel.rows / std::fabs(unit_dy));
  }
  return std::isfinite(step) ? step : 0.0;
}

/**
 * @brief One period of a line texture, in pels (§5.3.2.4.2, Table 12)
 *
 * A run is named by its first and last *pel*, because what a run of N pels
 * means on the grid is the pel swept from the run's first pel to its last —
 * N - 1 steps of travel, which the pel's own extent completes to N pels
 * exactly. Sweeping the whole run instead would draw it a pel too long, and a
 * single dot is then one stamp rather than a sweep.
 */
struct TextureRun {
  double first;
  double last;
};
struct TexturePeriod {
  std::vector<TextureRun> on;
  double length = 1.0;
};

TexturePeriod texture_period(NabtsLineTexture texture) {
  switch (texture) {
    case NabtsLineTexture::kSolid:
      return {{{0.0, 1.0}}, 1.0};
    case NabtsLineTexture::kDotted:
      // A dot one pel long, then a gap of the same.
      return {{{0.0, 0.0}}, 2.0};
    case NabtsLineTexture::kDashed:
      // A dash three pels long, then a gap of three.
      return {{{0.0, 2.0}}, 6.0};
    case NabtsLineTexture::kDottedDashed:
      // Dot of one, gap of one, dash of three, gap of one — "the
      // inter-dot-dash spacing is equivalent to the inter-dot spacing" — in
      // the order the TTX 650 lays a straight line and an arc
      // (line_pattern_lit()), so a curve through more points starts as they do.
      return {{{0.0, 0.0}, {2.0, 4.0}}, 6.0};
  }
  return {{{0.0, 1.0}}, 1.0};
}

}  // namespace

// ---------------------------------------------------------------------------
// Ink
// ---------------------------------------------------------------------------

NaplpsInk naplps_ink_of(const NabtsPrimitive& primitive) {
  NaplpsInk ink;
  ink.colour = primitive.colour;
  ink.colour_map_address = primitive.colour_map_address;
  return ink;
}

NaplpsInk naplps_background_ink_of(const NabtsPrimitive& primitive) {
  NaplpsInk ink;
  ink.colour = primitive.background;
  ink.colour_map_address = primitive.background_map_address;
  return ink;
}

// ---------------------------------------------------------------------------
// Surface
// ---------------------------------------------------------------------------

NaplpsCellSurface::NaplpsCellSurface(NaplpsRenderGrid grid) : grid_(grid) {
  const size_t count = static_cast<size_t>(std::max(0, grid_.width)) *
                       static_cast<size_t>(std::max(0, grid_.height));
  cells_.assign(count, NaplpsCell{});
  dirty_rows_.assign(static_cast<size_t>(std::max(0, grid_.height)), 0);
}

const NaplpsCell& NaplpsCellSurface::at(int column, int row) const {
  if (!contains(column, row)) {
    return blank_;
  }
  return cells_[static_cast<size_t>(row) * static_cast<size_t>(grid_.width) +
                static_cast<size_t>(column)];
}

void NaplpsCellSurface::deposit(int column, int row, const NaplpsInk& ink) {
  if (!contains(column, row)) {
    return;
  }
  NaplpsCell& cell =
      cells_[static_cast<size_t>(row) * static_cast<size_t>(grid_.width) +
             static_cast<size_t>(column)];
  cell.painted = true;
  cell.colour = ink.colour;
  cell.colour_map_address = ink.colour_map_address;
  dirty_rows_[static_cast<size_t>(row)] = 1;
}

void NaplpsCellSurface::put(int column, int row, const NaplpsCell& cell) {
  if (!contains(column, row)) {
    return;
  }
  cells_[static_cast<size_t>(row) * static_cast<size_t>(grid_.width) +
         static_cast<size_t>(column)] = cell;
  dirty_rows_[static_cast<size_t>(row)] = 1;
}

size_t NaplpsCellSurface::painted_count() const {
  size_t count = 0;
  for (const NaplpsCell& cell : cells_) {
    if (cell.painted) {
      ++count;
    }
  }
  return count;
}

void NaplpsCellSurface::clear() {
  std::fill(cells_.begin(), cells_.end(), NaplpsCell{});
  std::fill(dirty_rows_.begin(), dirty_rows_.end(), uint8_t{1});
}

void NaplpsCellSurface::forget_dirty() {
  std::fill(dirty_rows_.begin(), dirty_rows_.end(), uint8_t{0});
}

void NaplpsCellSurface::shift_area(int first_column, int last_column,
                                   int first_row, int last_row, int columns,
                                   int rows, const NaplpsInk* fill) {
  first_column = std::max(first_column, 0);
  last_column = std::min(last_column, grid_.width);
  first_row = std::max(first_row, 0);
  last_row = std::min(last_row, grid_.height);
  if ((columns == 0 && rows == 0) || first_column >= last_column ||
      first_row >= last_row) {
    return;
  }
  const auto width = static_cast<size_t>(grid_.width);
  const auto cell = [&](int column, int row) -> NaplpsCell& {
    return cells_[static_cast<size_t>(row) * width +
                  static_cast<size_t>(column)];
  };
  NaplpsCell vacated;
  if (fill != nullptr) {
    vacated.painted = true;
    vacated.colour = fill->colour;
    vacated.colour_map_address = fill->colour_map_address;
  }
  // Walk against the direction of travel, so no cell is read after it has
  // been written.
  const int row_step = rows > 0 ? -1 : 1;
  const int row_from = rows > 0 ? last_row - 1 : first_row;
  const int row_to = rows > 0 ? first_row - 1 : last_row;
  const int column_step = columns > 0 ? -1 : 1;
  const int column_from = columns > 0 ? last_column - 1 : first_column;
  const int column_to = columns > 0 ? first_column - 1 : last_column;
  for (int row = row_from; row != row_to; row += row_step) {
    const int source_row = row - rows;
    for (int column = column_from; column != column_to; column += column_step) {
      const int source_column = column - columns;
      const bool inside = source_row >= first_row && source_row < last_row &&
                          source_column >= first_column &&
                          source_column < last_column;
      cell(column, row) = inside ? cell(source_column, source_row) : vacated;
    }
    dirty_rows_[static_cast<size_t>(row)] = 1;
  }
}

void NaplpsCellSurface::copy_dirty_rows_from(const NaplpsCellSurface& other) {
  const auto width = static_cast<size_t>(std::max(0, grid_.width));
  for (int row = 0; row < grid_.height; ++row) {
    if (!other.row_dirty(row)) {
      continue;
    }
    const size_t first = static_cast<size_t>(row) * width;
    std::copy(other.cells_.begin() + static_cast<std::ptrdiff_t>(first),
              other.cells_.begin() + static_cast<std::ptrdiff_t>(first + width),
              cells_.begin() + static_cast<std::ptrdiff_t>(first));
  }
}

// ---------------------------------------------------------------------------
// Run merging
// ---------------------------------------------------------------------------

bool naplps_cells_match(const NaplpsCell& a, const NaplpsCell& b) {
  return a.painted == b.painted && a.colour == b.colour &&
         a.colour_map_address == b.colour_map_address;
}

namespace {

/// Appends to |runs| the runs along row |row| of |surface| of the cells
/// |listed| takes, each run as many such cells in a row as match its first.
template <typename Listed>
void append_row_runs(const NaplpsCellSurface& surface, int row, Listed listed,
                     std::vector<NaplpsCellRun>& runs) {
  int column = 0;
  while (column < surface.width()) {
    if (!listed(column)) {
      ++column;
      continue;
    }
    const NaplpsCell& cell = surface.at(column, row);
    int end = column + 1;
    while (end < surface.width() && listed(end) &&
           naplps_cells_match(surface.at(end, row), cell)) {
      ++end;
    }
    NaplpsCellRun run;
    run.column = column;
    run.row = row;
    run.columns = end - column;
    run.cell = cell;
    runs.push_back(run);
    column = end;
  }
}

}  // namespace

std::vector<NaplpsCellRun> naplps_merge_runs(const NaplpsCellSurface& surface) {
  std::vector<NaplpsCellRun> runs;
  for (int row = 0; row < surface.height(); ++row) {
    append_row_runs(
        surface, row,
        [&](int column) { return surface.at(column, row).painted; }, runs);
  }
  return runs;
}

std::vector<NaplpsCellRun> naplps_changed_runs(
    const NaplpsCellSurface& now, const NaplpsCellSurface& before) {
  std::vector<NaplpsCellRun> runs;
  for (int row = 0; row < now.height(); ++row) {
    if (!now.row_dirty(row)) {
      continue;
    }
    // A cell a scroll emptied changed too: it goes back to black.
    append_row_runs(
        now, row,
        [&](int column) {
          const NaplpsCell& cell = now.at(column, row);
          const NaplpsCell& was = before.at(column, row);
          return (cell.painted || was.painted) &&
                 !naplps_cells_match(cell, was);
        },
        runs);
  }
  return runs;
}

// ---------------------------------------------------------------------------
// Arc geometry
// ---------------------------------------------------------------------------

bool naplps_character_too_small(const NabtsPrimitive& character,
                                const std::vector<NabtsDrcsCharacter>& drcs) {
  if (character.kind != NabtsPrimitiveKind::kCharacter) {
    return false;
  }
  const int columns = naplps_reference_pixels(character.size.dx);
  const int rows = naplps_reference_pixels(character.size.dy);
  switch (character.repertoire) {
    case NabtsPrimitive::Repertoire::kPrimary:
    case NabtsPrimitive::Repertoire::kSupplementary:
      if (character.character == 0x20 ||
          (character.repertoire == NabtsPrimitive::Repertoire::kSupplementary &&
           nabts_supplementary_is_nonspacing(character.character))) {
        return false;
      }
      return columns < kNaplpsSmallestFieldColumns ||
             rows < kNaplpsSmallestFieldRows;
    case NabtsPrimitive::Repertoire::kDrcs:
      for (const NabtsDrcsCharacter& glyph : drcs) {
        if (glyph.code == character.character && glyph.defined()) {
          return columns < glyph.width || rows < glyph.height;
        }
      }
      return false;
    case NabtsPrimitive::Repertoire::kMosaic:
      break;
  }
  return false;
}

std::vector<NabtsPoint> naplps_arc_polyline(
    const std::vector<NabtsPoint>& control, double tolerance_unit) {
  if (control.size() != 3) {
    // §5.3.3.3: more than three points are a spline, whose minimum
    // implementation is the series of lines joining its points — the standard
    // reserves the smooth form for future standardization — and fewer are only
    // the line through them.
    return control;
  }

  const NabtsPoint& start = control[0];
  const NabtsPoint& through = control[1];
  const NabtsPoint& end = control[2];

  if (distance_between(start, end) < kCoincident) {
    // §5.3.3.2.2: "If the end point is omitted, it is taken to be coincident
    // with the start point and a circle is drawn", with the intermediate point
    // diametrically opposite.
    const NabtsPoint centre{(start.x + through.x) / 2.0,
                            (start.y + through.y) / 2.0};
    const double radius = distance_between(start, through) / 2.0;
    if (radius < kCoincident) {
      return {start};
    }
    return arc_points(centre, radius,
                      std::atan2(start.y - centre.y, start.x - centre.x),
                      2.0 * kPi, tolerance_unit, 3);
  }

  return arc_segment(start, through, end, tolerance_unit);
}

// ---------------------------------------------------------------------------
// Pel
// ---------------------------------------------------------------------------

void NaplpsRasteriser::deposit_block(int first_column, int first_row,
                                     int last_column, int last_row,
                                     const NaplpsInk& ink) {
  // Only the cells that can take anything are visited: the traced region when
  // an outline is being traced, the surface otherwise. Off the surface is not
  // an error — it is simply not displayed — but a figure a damaged record
  // throws hundreds of screens wide would otherwise cost a visit to every cell
  // of its pel at every step of the way.
  int min_column = 0;
  int min_row = 0;
  int max_column = surface_.width() - 1;
  int max_row = surface_.height() - 1;
  if (traced_ != nullptr) {
    min_column = traced_->first_column;
    min_row = traced_->first_row;
    max_column = traced_->first_column + traced_->columns - 1;
    max_row = traced_->first_row + traced_->rows - 1;
  }
  first_column = std::max(first_column, min_column);
  first_row = std::max(first_row, min_row);
  last_column = std::min(last_column, max_column);
  last_row = std::min(last_row, max_row);

  for (int row = first_row; row <= last_row; ++row) {
    for (int column = first_column; column <= last_column; ++column) {
      if (traced_ != nullptr) {
        traced_->cells[traced_->index_of(column, row)] = 1u;
      } else {
        surface_.deposit(column, row, ink);
      }
    }
  }
}

void NaplpsRasteriser::stamp_pel_block(int anchor_column, int anchor_row,
                                       const NaplpsCellSize& pel,
                                       const NaplpsInk& ink) {
  deposit_block(anchor_column, anchor_row,
                anchor_column + pel_span(pel.columns) - 1,
                anchor_row + pel_span(pel.rows) - 1, ink);
}

void NaplpsRasteriser::stamp_pel_cells(const NaplpsCellPoint& where,
                                       const NaplpsCellSize& pel,
                                       const NaplpsInk& ink) {
  stamp_pel_block(pel_anchor(where.column, pel.columns),
                  pel_anchor(where.row, pel.rows), pel, ink);
}

void NaplpsRasteriser::stamp_pel(const NabtsPoint& point, const NabtsSize& pel,
                                 const NaplpsInk& ink) {
  stamp_pel_cells(mapping_.cell_point(point), mapping_.cell_size(pel), ink);
}

// ---------------------------------------------------------------------------
// Stroking
// ---------------------------------------------------------------------------

void NaplpsRasteriser::sweep_pel(const NaplpsCellPoint& from,
                                 const NaplpsCellPoint& to,
                                 const NaplpsCellSize& pel,
                                 const NaplpsInk& ink) {
  // The locus is walked in whole cells, from the cell the pel starts in to the
  // cell it ends in, by the straight line algorithm §5.3.2.2.6 names: one cell
  // per step along whichever axis is moving faster. That draws a line of the
  // pel's own weight whichever way it runs — stepping along both axes instead
  // would put the pel down at every cell the locus so much as grazes, and a
  // rectangular brush grazes more cells running diagonally than running square,
  // so a diagonal would come out half as thick again as a horizontal from a pel
  // the standard sizes without reference to direction at all.
  //
  // Walking it in cells rather than by sampling a continuous position also
  // makes the swept region gap-free by construction, every step touching the
  // last. Sampling could drop a cell wherever the arithmetic of the endpoints
  // rounded the wrong way, and a filled figure works out its own inside from
  // what its outline shuts off from the outside: one dropped cell let the whole
  // fill out.
  //
  // Where the line passes exactly midway between two cells, the walk takes the
  // one nearer the end it starts from, so it starts from the upper end: both
  // receivers of the day light the upper cell of such a tie on a line drawn
  // upward — the Norpak TTX 650 always steps a line from its lower end
  // ($AE17 swaps them) with the error starting at half the major axis, which
  // comes to the same — and a line then draws the same cells whichever end
  // the record started it from.
  int from_column = pel_anchor(from.column, pel.columns);
  int from_row = pel_anchor(from.row, pel.rows);
  int to_column = pel_anchor(to.column, pel.columns);
  int to_row = pel_anchor(to.row, pel.rows);
  if (from_row < to_row) {
    std::swap(from_column, to_column);
    std::swap(from_row, to_row);
  }

  const int columns = std::abs(to_column - from_column);
  const int rows = std::abs(to_row - from_row);
  const int column_step = to_column >= from_column ? 1 : -1;
  const int row_step = to_row >= from_row ? 1 : -1;

  // Each step moves the pel at most one cell along each axis, and puts down
  // the same ink it did one step back, so all that is new is the row or column
  // it moved into: the cells it covered already hold this ink. That keeps a
  // wide pel's line to the cost of its edges rather than its area at every
  // step, and leaves exactly the cells stamping the whole block would.
  const int span_columns = pel_span(pel.columns);
  const int span_rows = pel_span(pel.rows);
  int column = from_column;
  int row = from_row;
  int error = columns - rows;
  stamp_pel_block(column, row, pel, ink);
  while (column != to_column || row != to_row) {
    const int doubled = 2 * error;
    int moved_column = 0;
    int moved_row = 0;
    if (doubled > -rows) {
      error -= rows;
      column += column_step;
      moved_column = column_step;
    }
    if (doubled < columns) {
      error += columns;
      row += row_step;
      moved_row = row_step;
    }
    if (moved_column != 0) {
      const int edge = moved_column > 0 ? column + span_columns - 1 : column;
      deposit_block(edge, row, edge, row + span_rows - 1, ink);
    }
    if (moved_row != 0) {
      const int edge = moved_row > 0 ? row + span_rows - 1 : row;
      deposit_block(column, edge, column + span_columns - 1, edge, ink);
    }
  }
}

namespace {

/**
 * @brief Whether the |index|th cell along a line in |texture| is lit, the pel
 *        |pel| cells along the line's major axis
 *
 * §5.3.2.4.2 names the line textures and leaves their proportions to the
 * receiver. These are the Norpak TTX 650's ($B0B7-$B219): dotted, one cell lit
 * every 2 pels; dashed, 2 pels and a cell lit every 6; dot-dashed, every 6
 * pels, one cell, then 2 pels and a cell from the second pel on.
 */
bool line_pattern_lit(NabtsLineTexture texture, int64_t pel, int64_t index) {
  const int64_t period =
      texture == NabtsLineTexture::kDotted ? 2 * pel : 6 * pel;
  int64_t phase = index % period;
  if (phase < 0) {
    phase += period;
  }
  switch (texture) {
    case NabtsLineTexture::kDotted:
      return phase == 0;
    case NabtsLineTexture::kDashed:
      return phase <= 2 * pel;
    case NabtsLineTexture::kDottedDashed:
      return phase == 0 || (phase >= 2 * pel && phase <= 4 * pel);
    case NabtsLineTexture::kSolid:
      break;
  }
  return true;
}

/// The cells of a straight line from (|x0|, |y0|) to (|x1|, |y1|) as the TTX
/// 650 steps one ($C494/$C4E3): from its lower end, one cell a step along the
/// major axis, the error starting at half of it.
std::vector<std::pair<int, int>> receiver_line_cells(int x0, int y0, int x1,
                                                     int y1) {
  if (y1 < y0) {
    std::swap(x0, x1);
    std::swap(y0, y1);
  }
  const int dx = x1 - x0;
  const int dy = y1 - y0;
  const int column_step = dx >= 0 ? 1 : -1;
  const int across = std::abs(dx);
  const bool steep = dy > across;
  const int major = steep ? dy : across;
  const int minor = steep ? across : dy;
  int error = major >> 1;
  std::vector<std::pair<int, int>> cells;
  cells.reserve(static_cast<size_t>(major) + 1);
  int x = x0;
  int y = y0;
  cells.emplace_back(x, y);
  for (int step = 0; step < major; ++step) {
    error += minor;
    if (error >= major) {
      x += column_step;
      y += 1;
      error -= major;
    } else if (steep) {
      y += 1;
    } else {
      x += column_step;
    }
    cells.emplace_back(x, y);
  }
  return cells;
}

}  // namespace

void NaplpsRasteriser::stroke_path(const std::vector<NabtsPoint>& points,
                                   const NabtsSize& pel,
                                   NabtsLineTexture texture,
                                   const NaplpsInk& ink, bool closed,
                                   const NaplpsInk* gaps,
                                   bool points_are_vertices) {
  if (points.empty()) {
    return;
  }
  if (points.size() == 1) {
    stamp_pel(points.front(), pel, ink);
    return;
  }

  const NaplpsCellSize pel_size = mapping_.cell_size(pel);
  const TexturePeriod period = texture_period(texture);

  // Along a curve flattened into a polyline the texture runs on rather than
  // restarting at each point, so the curve dashes as one line does.
  double phase = 0.0;

  const size_t segment_count = closed ? points.size() : points.size() - 1;
  for (size_t i = 0; i < segment_count; ++i) {
    const NaplpsCellPoint a = mapping_.cell_point(points[i]);
    const NaplpsCellPoint b =
        mapping_.cell_point(points[(i + 1) % points.size()]);
    const double run = b.column - a.column;
    const double rise = b.row - a.row;
    const double length = std::hypot(run, rise);

    const double step_length =
        length > 0.0 ? texture_step(run / length, rise / length, pel_size)
                     : 0.0;

    if (texture == NabtsLineTexture::kSolid || !(step_length > 0.0)) {
      // Solid, either because the texture is or because the pel has no extent
      // along this line — §5.3.2.4.2's "if logical pel size dx = 0, all
      // nonvertical lines are solid" and its converse.
      sweep_pel(a, b, pel_size, ink);
      continue;
    }

    // A position along the segment at |steps_along| pels from its start.
    const auto at_phase = [&](double steps_along) {
      const double t =
          std::clamp((steps_along * step_length) / length, 0.0, 1.0);
      return NaplpsCellPoint{a.column + run * t, a.row + rise * t};
    };

    // In colour mode 2 the gaps are drawn too, in the background: the whole
    // segment goes down in it first and the dots and dashes over it.
    if (gaps != nullptr) {
      sweep_pel(a, b, pel_size, *gaps);
    }
    if (points_are_vertices) {
      // A straight line's pattern is registered to the screen, not to the
      // line's start, as the Norpak TTX 650 lays it ($ADC8-$AE17,
      // $C288-$C3D4): each cell of the line, stepped from its lower end, is
      // lit by the pattern at its column — its row, on a line steeper than 45
      // degrees — and a lit cell takes the pel. Both ends are drawn whatever
      // the pattern says there ($AE8E, $AEA4).
      const int x0 = static_cast<int>(std::floor(a.column));
      const int y0 = static_cast<int>(std::floor(a.row));
      const int x1 = static_cast<int>(std::floor(b.column));
      const int y1 = static_cast<int>(std::floor(b.row));
      const bool steep = std::abs(y1 - y0) > std::abs(x1 - x0);
      const int pel_cells =
          steep ? pel_span(pel_size.rows) : pel_span(pel_size.columns);
      for (const auto& [x, y] : receiver_line_cells(x0, y0, x1, y1)) {
        if (line_pattern_lit(texture, pel_cells, steep ? y : x)) {
          stamp_pel_cells(
              NaplpsCellPoint{static_cast<double>(x), static_cast<double>(y)},
              pel_size, ink);
        }
      }
      stamp_pel_cells(a, pel_size, ink);
      stamp_pel_cells(b, pel_size, ink);
      continue;
    }

    const double entry_phase = phase;
    const double exit_phase = entry_phase + length / step_length;

    const double first_period = std::floor(entry_phase / period.length);
    const double last_period = std::floor(exit_phase / period.length);
    for (double p = first_period; p <= last_period; p += 1.0) {
      for (const TextureRun& on : period.on) {
        const double run_first =
            std::max(p * period.length + on.first, entry_phase);
        const double run_last =
            std::min(p * period.length + on.last, exit_phase);
        if (run_last < run_first) {
          continue;
        }
        sweep_pel(at_phase(run_first - entry_phase),
                  at_phase(run_last - entry_phase), pel_size, ink);
      }
    }

    // §5.3.2.4.2: "All end points of lines and arcs and all vertices ... must
    // be plotted regardless of the line texture used." A flattened arc's
    // interior points are not among them.
    if (i == 0) {
      stamp_pel_cells(a, pel_size, ink);
    }
    if (i + 1 == segment_count) {
      stamp_pel_cells(b, pel_size, ink);
    }

    phase = exit_phase;
  }
}

void NaplpsRasteriser::highlight_path(const std::vector<NabtsPoint>& points,
                                      const NabtsSize& pel,
                                      const NaplpsInk& ink, bool closed) {
  // §5.3.2.4.3: solid, "independent of the current line texture", using the
  // current logical pel size.
  stroke_path(points, pel, NabtsLineTexture::kSolid, ink, closed);
}

// ---------------------------------------------------------------------------
// Filling
// ---------------------------------------------------------------------------

namespace {

/**
 * @brief Whether a texture pattern covers a cell (§5.3.2.4.4)
 *
 * The predefined hatches are lines whose width and spacing are the logical
 * pel's, and the programmable masks are bitmaps stepped and repeated at the
 * mask size. Both are registered against the unit screen's origin rather than
 * the figure's, which is what §5.3.2.4.4 and §5.3.2.4.5 require so that
 * "registration of the patterns shall be maintained across figures".
 */
bool pattern_covers(NabtsTexturePattern pattern, const NabtsSize& pel,
                    const NabtsSize& mask_size, const NabtsTextureMask* mask,
                    double unit_x, double unit_y) {
  // A hatch line and the gap beside it are each one pel across, so a position
  // is on a line when it falls in an even-numbered pel-pair band.
  const auto in_hatch_band = [](double coordinate, double pitch) {
    if (!(std::fabs(pitch) > kCoincident)) {
      // §5.3.2.4.4: with no pel there is no pattern to draw, and the fill is
      // solid.
      return true;
    }
    const double band = std::floor(coordinate / std::fabs(pitch));
    return std::fmod(std::fabs(band), 2.0) < 1.0;
  };

  switch (pattern) {
    case NabtsTexturePattern::kSolid:
      return true;
    case NabtsTexturePattern::kVerticalHatch:
      return in_hatch_band(unit_x, pel.dx);
    case NabtsTexturePattern::kHorizontalHatch:
      return in_hatch_band(unit_y, pel.dy);
    case NabtsTexturePattern::kCrossHatch:
      // Both sets of lines are drawn, so a cell on either is covered.
      return in_hatch_band(unit_x, pel.dx) || in_hatch_band(unit_y, pel.dy);
    default:
      break;
  }

  if (mask == nullptr || !mask->defined() || mask->width <= 0 ||
      mask->height <= 0) {
    // "The default pattern for the four programmable texture masks is a null
    // texture pattern, resulting in no fill in color modes 0 and 1, and a fill
    // with the background color in color mode 2" (§5.3.2.4.4).
    return false;
  }

  const double step_x = std::fabs(mask_size.dx);
  const double step_y = std::fabs(mask_size.dy);
  if (!(step_x > kCoincident) || !(step_y > kCoincident)) {
    return true;
  }

  // Step and repeat from the unit screen's origin (§5.3.2.4.5).
  double fraction_x = std::fmod(unit_x, step_x) / step_x;
  double fraction_y = std::fmod(unit_y, step_y) / step_y;
  if (fraction_x < 0.0) {
    fraction_x += 1.0;
  }
  if (fraction_y < 0.0) {
    fraction_y += 1.0;
  }
  // "The sign bits of dx and dy are used to reflect the mask pattern within the
  // mask field" (§5.3.2.4.5).
  if (mask_size.dx < 0.0) {
    fraction_x = 1.0 - fraction_x;
  }
  if (mask_size.dy < 0.0) {
    fraction_y = 1.0 - fraction_y;
  }

  int element_x = static_cast<int>(fraction_x * mask->width);
  int element_y = static_cast<int>(fraction_y * mask->height);
  element_x = std::clamp(element_x, 0, mask->width - 1);
  element_y = std::clamp(element_y, 0, mask->height - 1);

  // Row 0 of a mask buffer is its bottom, matching unit space.
  return mask->elements[static_cast<size_t>(element_y) *
                            static_cast<size_t>(mask->width) +
                        static_cast<size_t>(element_x)] != 0;
}

}  // namespace

void NaplpsRasteriser::fill_path(const std::vector<NabtsPoint>& points,
                                 const NabtsSize& pel,
                                 NabtsTexturePattern pattern,
                                 const NabtsSize& mask_size,
                                 const NabtsTextureMask* mask,
                                 const NaplpsInk& ink, const NaplpsInk* gaps) {
  if (points.size() < 2) {
    if (points.size() == 1) {
      stamp_pel(points.front(), pel, ink);
    }
    return;
  }

  // The figure is worked out as a coverage mask first and deposited afterwards.
  // The enclosed area and the outline the pel traces are one filled region
  // (§5.3.3.4.1, §5.3.3.5.1), and the texture pattern applies to the whole of
  // it, so the two cannot be deposited independently without the pattern
  // falling out of registration between them.
  if (surface_.width() <= 0 || surface_.height() <= 0) {
    return;
  }

  // The figure's neighbourhood: its own extent, the pel it is outlined with,
  // and a margin round the lot, measured from the path rather than clipped to
  // the display area so that the outline is traced whole however far off the
  // screen the figure runs.
  const NaplpsCellSize pel_size = mapping_.cell_size(pel);
  std::vector<NaplpsCellPoint> path;
  path.reserve(points.size());
  for (const NabtsPoint& point : points) {
    path.push_back(mapping_.cell_point(point));
  }
  double left = std::numeric_limits<double>::infinity();
  double right = -std::numeric_limits<double>::infinity();
  double bottom = std::numeric_limits<double>::infinity();
  double top = -std::numeric_limits<double>::infinity();
  for (const NaplpsCellPoint& at : path) {
    left = std::min(left, at.column);
    right = std::max(right, at.column);
    bottom = std::min(bottom, at.row);
    top = std::max(top, at.row);
  }
  left += std::min(0.0, pel_size.columns) - kFillMarginCells;
  right += std::max(0.0, pel_size.columns) + kFillMarginCells;
  bottom += std::min(0.0, pel_size.rows) - kFillMarginCells;
  top += std::max(0.0, pel_size.rows) + kFillMarginCells;

  // A coordinate far outside the display area draws nothing a receiver shows,
  // so there is no point carrying the grid out to meet it. Beyond a screen's
  // width either way the figure covers everything on show in any case.
  const auto clamp_column = [this](double column) {
    return std::clamp(static_cast<int>(std::floor(column)), -surface_.width(),
                      2 * surface_.width());
  };
  const auto clamp_row = [this](double row) {
    return std::clamp(static_cast<int>(std::floor(row)), -surface_.height(),
                      2 * surface_.height());
  };

  NaplpsTracedRegion region;
  region.first_column = clamp_column(left);
  region.first_row = clamp_row(bottom);
  region.columns = clamp_column(std::ceil(right)) - region.first_column + 1;
  region.rows = clamp_row(std::ceil(top)) - region.first_row + 1;
  if (region.columns <= 0 || region.rows <= 0) {
    return;
  }
  region.cells.assign(
      static_cast<size_t>(region.columns) * static_cast<size_t>(region.rows),
      0u);

  // The outline the pel traces, which §5.3.3.4.1 and §5.3.3.5.1 put inside the
  // filled area rather than over it — so it is swept solid, whatever the line
  // texture. It is worked out by running the routine that *strokes* an outline
  // and noting the cells it touched: that region and a stroked outline are the
  // same thing, and a service that fills a figure and then outlines it depends
  // on their agreeing cell for cell.
  traced_ = &region;
  stroke_path(points, pel, NabtsLineTexture::kSolid, ink, /*closed=*/true);
  traced_ = nullptr;

  // The figure is that outline and every cell whose own corner — its lower
  // left, the point its coordinate names, which is how both receivers of the
  // day address a pixel — lies inside the path by the odd-even rule
  // §5.3.3.5.1 gives the enclosed area, so a frame drawn as one path, round
  // the outside, in through a slit and round the inside, is a frame and not a
  // slab. That is the Norpak TTX 650's fill ($B844-$BCB6): a scan of each row
  // between pairs of the path's crossings, with the outline round it. A gap a
  // single cell wide inside the figure stays open, as it does on both
  // receivers (the face mask of a drawn football helmet), where a fill
  // flooded in from the outside could not reach it and closed it.
  const int last_column = region.first_column + region.columns - 1;
  const int last_row = region.first_row + region.rows - 1;
  std::vector<uint8_t> in_figure = region.cells;
  std::vector<double> crossings;
  for (int row = region.first_row; row <= last_row; ++row) {
    const double corner = static_cast<double>(row);
    crossings.clear();
    for (size_t i = 0; i < path.size(); ++i) {
      const NaplpsCellPoint& from = path[i];
      const NaplpsCellPoint& to = path[(i + 1) % path.size()];
      // A half-open test in y counts a vertex once rather than twice, which is
      // what keeps a shared vertex from toggling the span twice and punching a
      // hole in the fill.
      if ((from.row <= corner && to.row > corner) ||
          (to.row <= corner && from.row > corner)) {
        crossings.push_back(from.column + (corner - from.row) /
                                              (to.row - from.row) *
                                              (to.column - from.column));
      }
    }
    std::sort(crossings.begin(), crossings.end());
    for (size_t i = 0; i + 1 < crossings.size(); i += 2) {
      const int from_column = std::max(
          region.first_column, static_cast<int>(std::ceil(crossings[i])));
      const int to_column =
          std::min(last_column, static_cast<int>(std::floor(crossings[i + 1])));
      for (int column = from_column; column <= to_column; ++column) {
        in_figure[region.index_of(column, row)] = 1u;
      }
    }
  }

  // Only what the receiver shows is deposited.
  const int first_shown_column = std::max(region.first_column, 0);
  const int last_shown_column = std::min(last_column, surface_.width() - 1);
  const int first_shown_row = std::max(region.first_row, 0);
  const int last_shown_row = std::min(last_row, surface_.height() - 1);
  if (first_shown_column > last_shown_column) {
    return;
  }
  for (int row = first_shown_row; row <= last_shown_row; ++row) {
    const uint8_t* figure_row =
        in_figure.data() + region.index_of(first_shown_column, row);
    for (int column = first_shown_column; column <= last_shown_column;
         ++column) {
      if (figure_row[column - first_shown_column] == 0u) {
        continue;
      }
      if (!pattern_covers(pattern, pel, mask_size, mask,
                          mapping_.centre_x(column), mapping_.centre_y(row))) {
        if (gaps != nullptr) {
          surface_.deposit(column, row, *gaps);
        }
        continue;
      }
      surface_.deposit(column, row, ink);
    }
  }
}

// ---------------------------------------------------------------------------
// Arcs as the receivers traced them
// ---------------------------------------------------------------------------

namespace {

/// The arc walk's unit: 1/64 of a cell, as the TTX 650 works at 256 x 200 in
/// 16-bit coordinates with 14 bits of fraction.
constexpr int64_t kArcUnit = 64;

/// The largest control-point coordinate the walk takes, in cells: four
/// screens' width of the finest grid. Within it every product the walk forms
/// fits 64 bits; the interpreter never sends an arc with a vertex off the unit
/// screen in any case.
constexpr int64_t kArcLargestCell = 4096;

/// Steps before a walk that never comes back to its end point is given up:
/// far more than any circle on the screen takes.
constexpr int kArcMostSteps = 200000;

/// Steps the walk may take past the point where it first came within a cell of
/// its end, while each still brings it closer.
constexpr int kArcClosingSteps = 8;

int64_t floor_div(int64_t numerator, int64_t denominator) {
  int64_t quotient = numerator / denominator;
  if (numerator % denominator != 0 && (numerator < 0) != (denominator < 0)) {
    --quotient;
  }
  return quotient;
}

}  // namespace

bool NaplpsRasteriser::arc_cells(const std::vector<NabtsPoint>& control,
                                 std::vector<std::pair<int, int>>& path,
                                 bool& circle, bool& collinear) const {
  path.clear();
  circle = false;
  collinear = false;
  if (control.size() != 3) {
    return false;
  }
  const auto in_units = [this](const NabtsPoint& point) {
    return std::pair<int64_t, int64_t>{
        std::llround(mapping_.column_at(point.x) * kArcUnit),
        std::llround(mapping_.row_at(point.y) * kArcUnit)};
  };
  const auto [start_x, start_y] = in_units(control[0]);
  const auto [middle_x, middle_y] = in_units(control[1]);
  const auto [end_x, end_y] = in_units(control[2]);
  for (const int64_t coordinate :
       {start_x, start_y, middle_x, middle_y, end_x, end_y}) {
    if (std::llabs(coordinate) > kArcLargestCell * kArcUnit) {
      return false;
    }
  }
  const int64_t ax = middle_x - start_x;
  const int64_t ay = middle_y - start_y;
  const int64_t bx = end_x - middle_x;
  const int64_t by = end_y - middle_y;
  const auto cell_of = [](int64_t units) {
    return static_cast<int>(floor_div(units, kArcUnit));
  };

  // The offset of the walk from the centre, and which way round it goes.
  int64_t offset_x = 0;
  int64_t offset_y = 0;
  bool counter_clockwise = false;
  circle = ax + bx == 0 && ay + by == 0;
  if (circle) {
    // §5.3.3.3.1: the end point the start point is a full circle, the
    // intermediate point diametrically opposite. The TTX 650 puts the centre
    // half the diameter, truncated, from the start ($82B6) and goes round
    // clockwise.
    offset_x = -(ax >> 1);
    offset_y = -(ay >> 1);
  } else {
    const int64_t cx = ax + bx;
    const int64_t cy = ay + by;
    const int64_t cross = ax * cy - ay * cx;
    if (cross == 0) {
      // Three points on a line are no circle: the receivers draw a line from
      // the start to the intermediate point and on to the end ($8AB7).
      collinear = true;
      return false;
    }
    // The circumcentre, from the start, the divisions truncated toward zero
    // as the firmware's are ($E64A, $E6AD); counter-clockwise where the
    // intermediate point lies to the left of the way to the end.
    counter_clockwise = cross > 0;
    const int64_t a2 = ax * ax + ay * ay;
    const int64_t c2 = cx * cx + cy * cy;
    offset_x = -((a2 * cy - c2 * ay) / (2 * cross));
    offset_y = -((c2 * ax - a2 * cx) / (2 * cross));
  }

  const std::pair<int, int> start{cell_of(start_x), cell_of(start_y)};
  const std::pair<int, int> end{cell_of(end_x), cell_of(end_y)};
  // A figure no more than a cell across is its start alone ($8325).
  bool tiny = true;
  for (const int64_t step : {ax, ay, bx, by, ax + bx, ay + by}) {
    tiny = tiny && std::llabs(step) <= kArcUnit;
  }
  if (tiny) {
    path.push_back(start);
    return true;
  }

  // Each step goes one cell along x, along y or both, the way round the
  // circle the walk is going, whichever leaves it nearest the radius: the
  // error is how far the squared distance from the centre is from the squared
  // radius, in units of 1/64 cell squared over 64.
  int64_t error = 0;
  const auto step_round = [&](int& step_x, int& step_y) {
    if (counter_clockwise) {
      step_y = offset_x < 0 ? -1 : 1;
      step_x = offset_y < 0 ? 1 : -1;
    } else {
      step_y = offset_x < 0 ? 1 : -1;
      step_x = offset_y < 0 ? -1 : 1;
    }
    const int64_t along_x = error + 2 * int64_t{step_x} * offset_x + kArcUnit;
    const int64_t along_y = error + 2 * int64_t{step_y} * offset_y + kArcUnit;
    const int64_t diagonal = along_x + along_y - error;
    if (std::llabs(diagonal) < std::llabs(along_x) &&
        std::llabs(diagonal) < std::llabs(along_y)) {
      offset_x += step_x * kArcUnit;
      offset_y += step_y * kArcUnit;
      error = diagonal;
    } else if (std::llabs(along_x) <= std::llabs(along_y)) {
      offset_x += step_x * kArcUnit;
      error = along_x;
      step_y = 0;
    } else {
      offset_y += step_y * kArcUnit;
      error = along_y;
      step_x = 0;
    }
  };

  // A circle takes three steps whatever, so that it leaves the start before it
  // is judged to have come back to it; then the walk goes on until it is
  // within a cell of the end on both axes, on while each step still brings it
  // closer, and puts down the end.
  std::pair<int, int> at = start;
  path.push_back(at);
  for (int steps = 0; steps < kArcMostSteps; ++steps) {
    int step_x = 0;
    int step_y = 0;
    step_round(step_x, step_y);
    at = {at.first + step_x, at.second + step_y};
    path.push_back(at);
    if (circle && steps < 2) {
      continue;
    }
    if (std::abs(end.first - at.first) <= 1 &&
        std::abs(end.second - at.second) <= 1) {
      break;
    }
  }
  for (int steps = 0; steps < kArcClosingSteps; ++steps) {
    int step_x = 0;
    int step_y = 0;
    step_round(step_x, step_y);
    const std::pair<int, int> next{at.first + step_x, at.second + step_y};
    if (std::abs(end.first - next.first) > std::abs(end.first - at.first) ||
        std::abs(end.second - next.second) > std::abs(end.second - at.second)) {
      break;
    }
    at = next;
    path.push_back(at);
  }
  path.push_back(end);
  return true;
}

void NaplpsRasteriser::append_chord(std::vector<std::pair<int, int>>& path) {
  if (path.size() < 2) {
    return;
  }
  const auto [start_x, start_y] = path.front();
  const auto [end_x, end_y] = path.back();
  const std::vector<std::pair<int, int>> chord =
      receiver_line_cells(end_x, end_y, start_x, start_y);
  path.insert(path.end(), chord.begin(), chord.end());
}

void NaplpsRasteriser::stroke_cells(
    const std::vector<std::pair<int, int>>& path, const NabtsSize& pel,
    NabtsLineTexture texture, const NaplpsInk& ink, const NaplpsInk* gaps) {
  if (path.empty()) {
    return;
  }
  const NaplpsCellSize pel_size = mapping_.cell_size(pel);
  const int pel_cells = pel_span(pel_size.columns);
  const auto at = [](const std::pair<int, int>& cell) {
    return NaplpsCellPoint{static_cast<double>(cell.first),
                           static_cast<double>(cell.second)};
  };
  for (size_t index = 0; index < path.size(); ++index) {
    const bool lit =
        texture == NabtsLineTexture::kSolid || index == 0 ||
        index + 1 == path.size() ||
        line_pattern_lit(texture, pel_cells, static_cast<int64_t>(index));
    if (lit) {
      stamp_pel_cells(at(path[index]), pel_size, ink);
    } else if (gaps != nullptr) {
      stamp_pel_cells(at(path[index]), pel_size, *gaps);
    }
  }
}

void NaplpsRasteriser::fill_cells(
    const std::vector<std::pair<int, int>>& outline, const NabtsSize& pel,
    NabtsTexturePattern pattern, const NabtsSize& mask_size,
    const NabtsTextureMask* mask, const NaplpsInk& ink, const NaplpsInk* gaps) {
  if (outline.empty()) {
    return;
  }
  const NaplpsCellSize pel_size = mapping_.cell_size(pel);
  const int pel_columns = pel_span(pel_size.columns);
  const int pel_rows = pel_span(pel_size.rows);
  int lowest = outline.front().second;
  int highest = lowest;
  for (const auto& cell : outline) {
    lowest = std::min(lowest, cell.second);
    highest = std::max(highest, cell.second);
  }
  // Rows far off the surface draw nothing; the spans are kept for what can
  // show.
  const int first_row = std::max(lowest, -surface_.height());
  const int last_row = std::min(highest, 2 * surface_.height());
  if (first_row > last_row) {
    return;
  }
  std::vector<int> left_of(static_cast<size_t>(last_row - first_row + 1),
                           std::numeric_limits<int>::max());
  std::vector<int> right_of(left_of.size(), std::numeric_limits<int>::min());
  for (const auto& [column, row] : outline) {
    if (row < first_row || row > last_row) {
      continue;
    }
    const size_t index = static_cast<size_t>(row - first_row);
    left_of[index] = std::min(left_of[index], column);
    right_of[index] = std::max(right_of[index], column);
  }

  // A row takes the spans of the rows the pel reaches it from, and the span is
  // widened by the pel's width the way the pel lies ($87FC).
  const bool pel_down = pel_size.rows < 0.0;
  const int first_filled = pel_down ? first_row - (pel_rows - 1) : first_row;
  const int last_filled = pel_down ? last_row : last_row + (pel_rows - 1);
  for (int row = std::max(first_filled, 0);
       row <= std::min(last_filled, surface_.height() - 1); ++row) {
    int left = std::numeric_limits<int>::max();
    int right = std::numeric_limits<int>::min();
    for (int reach = 0; reach < pel_rows; ++reach) {
      const int from = pel_down ? row + reach : row - reach;
      if (from < first_row || from > last_row) {
        continue;
      }
      const size_t index = static_cast<size_t>(from - first_row);
      if (left_of[index] > right_of[index]) {
        continue;
      }
      left = std::min(left, left_of[index]);
      right = std::max(right, right_of[index]);
    }
    if (left > right) {
      continue;
    }
    if (pel_size.columns < 0.0) {
      left -= pel_columns - 1;
    } else {
      right += pel_columns - 1;
    }
    for (int column = std::max(left, 0);
         column <= std::min(right, surface_.width() - 1); ++column) {
      if (!pattern_covers(pattern, pel, mask_size, mask,
                          mapping_.centre_x(column), mapping_.centre_y(row))) {
        if (gaps != nullptr) {
          surface_.deposit(column, row, *gaps);
        }
        continue;
      }
      surface_.deposit(column, row, ink);
    }
  }
}

std::vector<NabtsPoint> NaplpsRasteriser::arc_polyline(
    const std::vector<NabtsPoint>& control) const {
  // A fraction of a cell, so the polyline standing in for the curve cannot
  // change which cells get painted.
  return naplps_arc_polyline(control,
                             kArcToleranceCells / mapping_.columns_per_unit);
}

// ---------------------------------------------------------------------------
// Characters, mosaics and downloadable glyphs
// ---------------------------------------------------------------------------

namespace {

// Cell-space slack when a field edge is truncated to a whole cell. A relative
// coordinate is resolved with a little arithmetic error, and an edge that falls
// on a cell boundary has to land on the same side of it for the field ending
// there as for the field starting there, or the two would part by a cell.
constexpr double kFieldEdgeSlack = 1e-6;

// Table D1 item 2: "The offset of the visual font baseline, ie, the apparent
// bottom of upper case alphabetic characters ... shall be 20% of the height
// (dy) of the current character field size".
constexpr double kBaselineFraction = 0.2;

int truncated_cell(double position) {
  return static_cast<int>(std::floor(position + kFieldEdgeSlack));
}

/**
 * @brief One axis of a character field on the surface
 *
 * The field runs |extent| cells from |origin| along a surface axis, towards
 * higher cell indices or, where rotation has turned it round, lower ones. Its
 * cells are the ones its edges truncate to — [floor(origin),
 * floor(origin + extent)) — which is how Appendix B maps a position onto the
 * physical pixels: its 12-bit example reaches 256 pixels by "simply a
 * truncation of the low order 4 bits". A field that ends where the next one
 * begins shares that edge with it, so a line of fields tiles the row with no
 * cell left between two of them and none drawn by both, however the field
 * width falls on the grid. Rounding each field to whole cells instead, while
 * the cursor advances exactly, would leave a column of gaps through text in
 * the 6.4-pixel default field.
 */
struct FieldAxis {
  double origin = 0.0;
  double extent = 0.0;
  int direction = 1;

  /// The number of the field's cells nearer its origin edge than |distance|,
  /// which is also the index of the cell |distance| falls in.
  int cells_within(double distance) const {
    return direction > 0
               ? truncated_cell(origin + distance) - truncated_cell(origin)
               : truncated_cell(origin) - truncated_cell(origin - distance);
  }
  int cells() const { return cells_within(extent); }

  /// The surface index of the field's |index|th cell from its origin edge.
  ///
  /// A field turned round to run back along an axis starts from the cell its
  /// origin falls in, not the one before it: both receivers of the day — the
  /// Norpak TTX 650 and the Panasonic TU-2000x, driven through their firmware
  /// at 90, 180 and 270 degrees — lay it so, as they count a logical pel or a
  /// FIELD of negative size from its origin's own cell (pel_anchor()). Only
  /// which cells the field covers moves: it is as if the origin edge stood a
  /// cell further on, so cells_within() and centre() measure the same.
  int surface(int index) const {
    return direction > 0 ? truncated_cell(origin) + index
                         : truncated_cell(origin) - index;
  }

  /// How far the middle of the field's |index|th cell is from its origin edge.
  double centre(int index) const {
    const double middle = surface(index) + 0.5;
    return direction > 0 ? middle - origin : origin - middle;
  }
};

/**
 * @brief A character field on the surface, in the field's own frame
 *
 * |across| runs along the character's width from its left edge and |up| along
 * its height from its foot, onto whichever surface axes the rotation turns
 * them (§5.3.2.3.2, Figure 17). "The character field origin is the lower left
 * corner of the character field at the default 0 degree rotation regardless
 * of the sign of the character field dimensions dx and dy" (§5.3.2.3.2), so
 * the field always runs across and up from its origin, and a negative
 * dimension only reflects what is drawn in it (§5.3.2.3.9).
 */
struct CharacterFieldGeometry {
  FieldAxis across;
  FieldAxis up;
  /// Whether |across| lies along the surface's columns (0 and 180 degrees)
  /// rather than its rows (90 and 270).
  bool across_is_columns = true;
  /// Cells per unit along the surface axes |across| and |up| lie on.
  double across_cells_per_unit = 0.0;
  double up_cells_per_unit = 0.0;
  /// The cells the field covers where it is.
  int columns = 0;
  int rows = 0;
  /// The whole cells the field's width and height hold wherever it falls. A
  /// field 6.4 cells wide covers six cells in one place and seven in the next;
  /// what is drawn in it is sized by the six, so a letter is the same shape in
  /// both and the seventh column is space.
  int nominal_columns = 0;
  int nominal_rows = 0;

  bool empty() const { return columns <= 0 || rows <= 0; }

  void surface_cell(int column, int row, int& surface_column,
                    int& surface_row) const {
    const int along = across.surface(column);
    const int rising = up.surface(row);
    surface_column = across_is_columns ? along : rising;
    surface_row = across_is_columns ? rising : along;
  }
};

CharacterFieldGeometry character_field(const NabtsPrimitive& primitive,
                                       const NaplpsGridMapping& mapping) {
  const NaplpsCellPoint origin = mapping.cell_point(primitive.origin);
  const double width = std::fabs(primitive.size.dx);
  const double height = std::fabs(primitive.size.dy);
  const double per_column = mapping.columns_per_unit;
  const double per_row = mapping.rows_per_unit;

  // §5.3.2.3.2: "Rotation causes the character field and the cursor to rotate
  // counterclockwise about the character field origin." A quarter turn about
  // the origin carries the field's width onto the upward axis and its height
  // onto the leftward one, so at 90 degrees the field lies above and to the
  // left of its origin, at 180 below and to the left, and at 270 below and to
  // the right (Figure 17).
  CharacterFieldGeometry field;
  switch (primitive.rotation) {
    case NabtsCharRotation::kNone:
      field.across = FieldAxis{origin.column, width * per_column, 1};
      field.up = FieldAxis{origin.row, height * per_row, 1};
      field.across_is_columns = true;
      break;
    case NabtsCharRotation::k90:
      field.across = FieldAxis{origin.row, width * per_row, 1};
      field.up = FieldAxis{origin.column, height * per_column, -1};
      field.across_is_columns = false;
      break;
    case NabtsCharRotation::k180:
      field.across = FieldAxis{origin.column, width * per_column, -1};
      field.up = FieldAxis{origin.row, height * per_row, -1};
      field.across_is_columns = true;
      break;
    case NabtsCharRotation::k270:
      field.across = FieldAxis{origin.row, width * per_row, -1};
      field.up = FieldAxis{origin.column, height * per_column, 1};
      field.across_is_columns = false;
      break;
  }
  field.across_cells_per_unit = field.across_is_columns ? per_column : per_row;
  field.up_cells_per_unit = field.across_is_columns ? per_row : per_column;
  field.columns = field.across.cells();
  field.rows = field.up.cells();
  field.nominal_columns = std::max(1, truncated_cell(field.across.extent));
  field.nominal_rows = std::max(1, truncated_cell(field.up.extent));
  return field;
}

/// What a character puts in its field, before the attributes decide the ink
/// each cell takes: lit is the character's shape, unlit the field around it.
/// Row 0 is the field's foot, as the surface's is.
struct FieldImage {
  int columns = 0;
  int rows = 0;
  std::vector<uint8_t> lit;

  FieldImage(int field_columns, int field_rows)
      : columns(std::max(0, field_columns)),
        rows(std::max(0, field_rows)),
        lit(static_cast<size_t>(columns) * static_cast<size_t>(rows), 0u) {}

  bool at(int column, int row) const {
    return column >= 0 && column < columns && row >= 0 && row < rows &&
           lit[static_cast<size_t>(row) * static_cast<size_t>(columns) +
               static_cast<size_t>(column)] != 0u;
  }

  void set(int column, int row) {
    if (column >= 0 && column < columns && row >= 0 && row < rows) {
      lit[static_cast<size_t>(row) * static_cast<size_t>(columns) +
          static_cast<size_t>(column)] = 1u;
    }
  }

  /// Light the cells [first_column, end_column) by [first_row, end_row).
  void fill(int first_column, int end_column, int first_row, int end_row) {
    for (int row = first_row; row < end_row; ++row) {
      for (int column = first_column; column < end_column; ++column) {
        set(column, row);
      }
    }
  }
};

/**
 * @brief Where a stored pattern sits in a character field
 *
 * At a whole multiple of its size, which is how Appendix B has a receiver
 * scale what it stored — "limited to integer exact divisions or
 * multiplications" — so a letterform keeps its shape in every field of a line
 * rather than gaining a column in some fields and not in others. A field a
 * little short of the pattern takes it squeezed by the nearest-neighbour rule
 * — Table D1 item 2's smallest presented field, 6 by 8 at 256 x 200, is two
 * rows short of the 6 by 10 face — and one smaller than that presents no
 * pattern at all (naplps_character_too_small()).
 */
struct PatternPlacement {
  /// The cells the pattern covers, and where its left edge and foot are in
  /// the field.
  int columns = 0;
  int rows = 0;
  int first_column = 0;
  int first_row = 0;
  /// The multiple, which is the width in cells of one of the pattern's
  /// strokes; 1 where the pattern is squeezed.
  int scale_across = 1;
  int scale_up = 1;
};

/**
 * @brief Place a |pattern_width| by |pattern_height| pattern in |field|
 *
 * A face's pattern, |descent| of its rows below the baseline, is centred across
 * the field and raised to put its baseline where Table D1 item 2 puts the
 * field's: "It is recommended that, if possible, the font be centered within
 * the character field ... but in any case, there shall be some such space to
 * the right of the font", and the baseline "shall be 20% of the height (dy)".
 * A DRCS buffer (|descent| negative) stands for the whole field (§6.2.3), so it
 * is laid from the field's origin.
 */
PatternPlacement place_pattern(int pattern_width, int pattern_height,
                               int descent,
                               const CharacterFieldGeometry& field) {
  PatternPlacement place;
  const auto fit = [](int field_cells, int pattern_cells, int& cells,
                      int& scale) {
    if (field_cells >= pattern_cells) {
      scale = field_cells / pattern_cells;
      cells = scale * pattern_cells;
    } else {
      scale = 1;
      cells = field_cells;
    }
  };
  fit(field.nominal_columns, pattern_width, place.columns, place.scale_across);
  fit(field.nominal_rows, pattern_height, place.rows, place.scale_up);
  if (descent < 0) {
    return place;
  }
  // A spare column goes to the right.
  place.first_column = (field.nominal_columns - place.columns) / 2;
  if (place.rows == place.scale_up * pattern_height) {
    const int baseline =
        static_cast<int>(std::lround(kBaselineFraction * field.up.extent));
    place.first_row = std::clamp(baseline - place.scale_up * descent, 0,
                                 field.nominal_rows - place.rows);
  }
  return place;
}

/// Lay |glyph| into |image| at |place|.
void lay_pattern(FieldImage& image, const PatternPlacement& place,
                 const NaplpsGlyphView& glyph) {
  if (place.columns <= 0 || place.rows <= 0 || glyph.width <= 0 ||
      glyph.height <= 0) {
    return;
  }
  for (int row = 0; row < place.rows; ++row) {
    // Nearest-neighbour from the field back into the pattern: a whole
    // multiple repeats every element, and a squeeze drops some.
    const int from_top = place.rows - 1 - row;
    const int source_row =
        std::min(glyph.height - 1, from_top * glyph.height / place.rows);
    for (int column = 0; column < place.columns; ++column) {
      const int source_column =
          std::min(glyph.width - 1, column * glyph.width / place.columns);
      if (glyph.lit(source_column, source_row)) {
        image.set(place.first_column + column, place.first_row + row);
      }
    }
  }
}

/// Lay |face|'s pattern for |code| into |image|; false where it has none.
bool lay_face_pattern(FieldImage& image, const NaplpsFontFace& face,
                      const PatternPlacement& place, char32_t code) {
  const uint16_t* pattern = naplps_face_pattern(face, code);
  if (pattern == nullptr) {
    return false;
  }
  lay_pattern(image, place,
              NaplpsGlyphView::of_rows(pattern, face.width, face.height));
  return true;
}

/// Lay naplps_replacement_pattern() for |face| into |image|.
void lay_replacement_box(FieldImage& image, const NaplpsFontFace& face,
                         const PatternPlacement& place) {
  const std::vector<uint16_t> box = naplps_replacement_pattern(face);
  lay_pattern(image, place,
              NaplpsGlyphView::of_rows(box.data(), face.width, face.height));
}

/// Table 25's line and triangle graphics, which notes 1 to 7 define by the
/// character field itself rather than by a pattern.
enum class FieldGraphic : uint8_t {
  kNone,
  kHorizontalLine,         // S 5/6, note 6
  kVerticalLine,           // S 5/7, note 7
  kDiagonal,               // S 5/8, note 1
  kReverseDiagonal,        // S 5/9, note 2
  kFilledDiagonal,         // S 5/10, note 3
  kFilledReverseDiagonal,  // S 5/11, note 4
  kCross,                  // S 6/5, note 5
};

FieldGraphic field_graphic(const NabtsPrimitive& primitive) {
  if (primitive.repertoire != NabtsPrimitive::Repertoire::kSupplementary) {
    return FieldGraphic::kNone;
  }
  switch (primitive.character) {
    case 0x56:
      return FieldGraphic::kHorizontalLine;
    case 0x57:
      return FieldGraphic::kVerticalLine;
    case 0x58:
      return FieldGraphic::kDiagonal;
    case 0x59:
      return FieldGraphic::kReverseDiagonal;
    case 0x5A:
      return FieldGraphic::kFilledDiagonal;
    case 0x5B:
      return FieldGraphic::kFilledReverseDiagonal;
    case 0x65:
      return FieldGraphic::kCross;
    default:
      break;
  }
  return FieldGraphic::kNone;
}

/**
 * @brief Draw one of Table 25's field graphics into |image|
 *
 * The notes put them edge to edge and corner to corner of the character field
 * — "The full horizontal line extends from the middle of the left edge of the
 * character field to the middle of the right edge", "The diagonal extends from
 * the lower left corner of the character field to the upper right corner" —
 * which is what lets a run of them join into one line or one edge. So they are
 * drawn from the field's own cells rather than from a pattern placed in it: a
 * pattern at a whole multiple of its size leaves the field's spare column
 * empty, and a line would break there.
 *
 * A line is a stroke of the face at the field's size, |weight| cells across
 * and |weight_up| up, so it matches the text beside it. A filled triangle
 * takes every cell whose middle lies in it.
 */
void lay_field_graphic(FieldImage& image, FieldGraphic graphic,
                       const CharacterFieldGeometry& field, int weight,
                       int weight_up) {
  const double width = field.across.extent;
  const double height = field.up.extent;
  if (!(width > 0.0) || !(height > 0.0) || image.columns <= 0 ||
      image.rows <= 0) {
    return;
  }
  const auto horizontal = [&] {
    const double middle = height / 2.0;
    image.fill(0, image.columns,
               field.up.cells_within(middle - weight_up / 2.0),
               field.up.cells_within(middle + weight_up / 2.0));
  };
  const auto vertical = [&] {
    const double middle = width / 2.0;
    image.fill(field.across.cells_within(middle - weight / 2.0),
               field.across.cells_within(middle + weight / 2.0), 0, image.rows);
  };
  // Corner to corner, row by row: the cells the ideal line passes through in
  // each row, and never fewer than a stroke's width of them.
  const auto diagonal = [&](bool reverse) {
    // The row's last point is taken just short of where the line leaves it,
    // by more than the edge slack, so a line leaving a row exactly at a cell
    // edge does not also claim the cell beyond it.
    constexpr double kTouch = 10.0 * kFieldEdgeSlack;
    for (int row = 0; row < image.rows; ++row) {
      const double low = std::clamp(field.up.centre(row) - 0.5, 0.0, height);
      const double high = std::clamp(field.up.centre(row) + 0.5, 0.0, height);
      const double from =
          reverse ? width - high * width / height : low * width / height;
      const double to =
          reverse ? width - low * width / height : high * width / height;
      const int first =
          std::clamp(field.across.cells_within(from), 0, image.columns - 1);
      const int last = std::clamp(
          std::max(field.across.cells_within(std::max(from, to - kTouch)),
                   first + weight - 1),
          0, image.columns - 1);
      image.fill(first, last + 1, row, row + 1);
    }
  };
  const auto triangle = [&](bool reverse) {
    for (int row = 0; row < image.rows; ++row) {
      const double rise = field.up.centre(row);
      for (int column = 0; column < image.columns; ++column) {
        const double run = field.across.centre(column);
        // Below the diagonal from the lower left corner to the upper right,
        // or, reversed, below the one from the lower right to the upper left.
        const double edge = reverse ? width - run : run;
        if (rise * width <= edge * height) {
          image.set(column, row);
        }
      }
    }
  };

  switch (graphic) {
    case FieldGraphic::kNone:
      return;
    case FieldGraphic::kHorizontalLine:
      horizontal();
      return;
    case FieldGraphic::kVerticalLine:
      vertical();
      return;
    case FieldGraphic::kCross:
      // Note 5: "equivalent to overlaying the full horizontal line character
      // with the full vertical line character".
      horizontal();
      vertical();
      return;
    case FieldGraphic::kDiagonal:
      diagonal(false);
      return;
    case FieldGraphic::kReverseDiagonal:
      diagonal(true);
      return;
    case FieldGraphic::kFilledDiagonal:
      // Note 3: "a filled triangle occupying the lower right half".
      triangle(false);
      return;
    case FieldGraphic::kFilledReverseDiagonal:
      // Note 4: "a filled triangle occupying the lower left half".
      triangle(true);
      return;
  }
}

/**
 * @brief A mosaic's elements in its field (§5.4)
 *
 * "In contiguous mode, the six mosaic elements that compose each character
 * shall completely span the given character field at any size", so the
 * elements divide the field's own cells: their boundaries fall where the
 * field's halves and thirds do, truncated as the field's edges are, and the
 * mosaics of a row tile it exactly as a line of text does rather than each
 * taking every cell its elements touch.
 *
 * "In separated mode, each of the mosaic elements that are illuminated are
 * reduced in the horizontal dimension by the absolute value of the width (dx)
 * of the logical pel size, and are reduced in the vertical dimension by the
 * absolute value of the height (dy)", "left and bottom justified within the
 * normal element area, the remainder of the area being considered as
 * background". The pel always maps to at least one pixel (§5.3.2.2.6), which
 * is also what keeps a separated mosaic distinct from a contiguous one as
 * Table D1 item 6 requires.
 */
FieldImage mosaic_image(const NabtsPrimitive& primitive,
                        const CharacterFieldGeometry& field) {
  FieldImage image(field.columns, field.rows);
  // §5.4: a code position the standard does not assign "shall be displayed as
  // SPACE", which is a mosaic with nothing lit.
  const uint8_t sixels = nabts_is_mosaic_code(primitive.character)
                             ? nabts_mosaic_sixels(primitive.character)
                             : 0;
  const int split_across = field.across.cells_within(field.across.extent / 2.0);
  const int split_up[4] = {0, field.up.cells_within(field.up.extent / 3.0),
                           field.up.cells_within(2.0 * field.up.extent / 3.0),
                           field.rows};

  // §6.2.7.15: underline mode is what puts mosaics into separated mode.
  const bool separated = primitive.underlined;
  const int shrink_across =
      separated
          ? pel_span(primitive.logical_pel.dx * field.across_cells_per_unit)
          : 0;
  const int shrink_up =
      separated ? pel_span(primitive.logical_pel.dy * field.up_cells_per_unit)
                : 0;

  for (int element = 0; element < 6; ++element) {
    if ((sixels & (1u << element)) == 0) {
      continue;
    }
    // Bit 0 is top-left and bit 5 bottom-right, so the element's row counts
    // down from the top while the field's rows count up from its foot.
    const bool right = (element % 2) != 0;
    const int band = 2 - element / 2;
    const int first_column = right ? split_across : 0;
    const int end_column = right ? field.columns : split_across;
    const int first_row = split_up[band];
    const int end_row = split_up[band + 1];
    image.fill(first_column, std::max(first_column, end_column - shrink_across),
               first_row, std::max(first_row, end_row - shrink_up));
  }
  return image;
}

/// A DRCS character's buffer in its field, or nothing where it was never
/// defined: §5.6 "it shall be displayed as if it were SPACE".
FieldImage drcs_image(const NabtsPrimitive& primitive,
                      const std::vector<NabtsDrcsCharacter>& drcs,
                      const CharacterFieldGeometry& field) {
  FieldImage image(field.columns, field.rows);
  for (const NabtsDrcsCharacter& glyph : drcs) {
    if (glyph.code != primitive.character || !glyph.defined()) {
      continue;
    }
    // One element per pixel of the field it was defined in (§6.2.3), however
    // many that was.
    const NaplpsGlyphView view = NaplpsGlyphView::of_drcs(glyph);
    lay_pattern(image, place_pattern(view.width, view.height, -1, field), view);
    break;
  }
  return image;
}

/// Column 4 of the supplementary set, which §5.2 makes nonspacing: "when one
/// of these characters is received, the cursor is not automatically advanced".
bool is_nonspacing_mark(const NabtsPrimitive& primitive) {
  return primitive.repertoire == NabtsPrimitive::Repertoire::kSupplementary &&
         nabts_supplementary_is_nonspacing(primitive.character);
}

/// A character a mark is composed onto (§7.1): the primary set and the
/// spacing characters of the supplementary set. Mosaics and DRCS are not in
/// the graphic character repertoire.
bool takes_marks(const NabtsPrimitive& primitive) {
  return (primitive.repertoire == NabtsPrimitive::Repertoire::kPrimary ||
          primitive.repertoire == NabtsPrimitive::Repertoire::kSupplementary) &&
         !is_nonspacing_mark(primitive);
}

/// The marks as the Unicode combining characters they stand for, in the order
/// they arrived.
std::vector<char32_t> mark_codes(const std::vector<NabtsPrimitive>& marks) {
  std::vector<char32_t> out;
  out.reserve(marks.size());
  for (const NabtsPrimitive& mark : marks) {
    out.push_back(nabts_supplementary_to_unicode(mark.character));
  }
  return out;
}

/**
 * @brief Lay marks that modify no letter, in their spacing form
 *
 * Table 26 codes a diacritical mark standing alone as the mark followed by
 * SPACE ("S 4/2 2/0"), and §7.1 puts the marks of Table 27 in the repertoire
 * "when combined with SPACE": the first mark is drawn in its spacing form and
 * any other laid over it.
 */
void lay_spacing_marks(FieldImage& image, const NaplpsFontFace& face,
                       const PatternPlacement& place,
                       const std::vector<char32_t>& marks) {
  for (size_t i = 0; i < marks.size(); ++i) {
    const NaplpsMarkForms* forms = naplps_font_mark_forms(marks[i]);
    const char32_t code = forms == nullptr                ? marks[i]
                          : i == 0 && forms->spacing != 0 ? forms->spacing
                                                          : forms->overlay;
    if (!lay_face_pattern(image, face, place, code)) {
      lay_replacement_box(image, face, place);
    }
  }
}

/**
 * @brief Lay a letter with the marks that preceded it composed onto it (§7.2)
 *
 * Table 18's accented letters are drawn from the face's own pattern for the
 * accented letter; any other pairing — §7.1 makes those "not constitute
 * characters of the repertoire" and leaves their presentation to the
 * implementation — has the mark laid over the letter, and a mark with SPACE
 * is the mark on its own (Table 26).
 */
void lay_letter(FieldImage& image, const NaplpsFontFace& face,
                const PatternPlacement& place,
                const CharacterFieldGeometry& field,
                const NabtsPrimitive& primitive,
                const std::vector<char32_t>& marks) {
  const FieldGraphic graphic = field_graphic(primitive);
  char32_t base = primitive.repertoire == NabtsPrimitive::Repertoire::kPrimary
                      ? nabts_primary_to_unicode(primitive.character)
                      : nabts_supplementary_to_unicode(primitive.character);
  if (graphic == FieldGraphic::kNone && base == U' ' && !marks.empty()) {
    lay_spacing_marks(image, face, place, marks);
    return;
  }

  std::vector<char32_t> overlays;
  for (const char32_t mark : marks) {
    const NaplpsMarkForms* forms = naplps_font_mark_forms(mark);
    const char32_t composed =
        graphic == FieldGraphic::kNone ? naplps_font_composed(base, mark) : 0;
    if (composed != 0) {
      base = composed;
    } else {
      overlays.push_back(forms != nullptr ? forms->overlay : mark);
    }
  }

  if (graphic != FieldGraphic::kNone) {
    lay_field_graphic(image, graphic, field, place.scale_across,
                      place.scale_up);
  } else if (!lay_face_pattern(image, face, place, base)) {
    lay_replacement_box(image, face, place);
  }
  for (const char32_t overlay : overlays) {
    if (!lay_face_pattern(image, face, place, overlay)) {
      lay_replacement_box(image, face, place);
    }
  }
}

/**
 * @brief Deposit |image| into |field| in the character's attributes
 *
 * §5.3.2.3.9: "If dx is negative, the character patterns are reflected about
 * the vertical center axis of the character field. If dy is negative, the
 * character patterns are reflected about the horizontal center axis". §6.2.7.4:
 * reverse video draws "the pixels surrounding the character shape in the
 * character field ... in the drawing color. The pixels of the character shape
 * are not drawn, except in color mode 2, when they are drawn in the background
 * color."
 *
 * |underline| adds §6.2.7.15's line to the pattern: it "appears in the
 * character field starting at the character field origin and extending across
 * the entire width (dx dimension) of the character field ... Its thickness is
 * determined by the vertical dimension (dy) of the logical pel size" — at least
 * a pixel, as the pel always is (§5.3.2.2.6). Being part of the pattern it is
 * turned with the character (§5.3.2.3.2), and it stays at the origin's edge of
 * the field whichever way the pattern is reflected.
 */
void deposit_field_image(NaplpsCellSurface& surface,
                         const CharacterFieldGeometry& field,
                         const FieldImage& image,
                         const NabtsPrimitive& primitive, bool underline,
                         const NaplpsInk& ink, const NaplpsInk* background) {
  const bool mirror_across = primitive.size.dx < 0.0;
  const bool mirror_up = primitive.size.dy < 0.0;
  const int underline_rows =
      underline ? pel_span(primitive.logical_pel.dy * field.up_cells_per_unit)
                : 0;

  // Only the field's cells that land on the surface are visited. A field
  // steps one surface cell per index along each axis (FieldAxis::surface), so
  // the range that lands is found once per axis rather than tested per cell —
  // which matters for the screen-sized fields a damaged TEXT operand sets.
  const auto landing = [](const FieldAxis& axis, int cells, int limit,
                          int& first, int& last) {
    const int base = axis.surface(0);
    if (axis.direction > 0) {
      first = std::max(0, -base);
      last = std::min(cells - 1, limit - 1 - base);
    } else {
      first = std::max(0, base - (limit - 1));
      last = std::min(cells - 1, base);
    }
  };
  const int across_limit =
      field.across_is_columns ? surface.width() : surface.height();
  const int up_limit =
      field.across_is_columns ? surface.height() : surface.width();
  int first_column = 0;
  int last_column = -1;
  int first_row = 0;
  int last_row = -1;
  landing(field.across, field.columns, across_limit, first_column, last_column);
  landing(field.up, field.rows, up_limit, first_row, last_row);

  for (int row = first_row; row <= last_row; ++row) {
    for (int column = first_column; column <= last_column; ++column) {
      const bool lit =
          row < underline_rows ||
          image.at(mirror_across ? field.columns - 1 - column : column,
                   mirror_up ? field.rows - 1 - row : row);
      const NaplpsInk* pen = nullptr;
      if (primitive.reverse_video) {
        pen = lit ? background : &ink;
      } else {
        pen = lit ? &ink : background;
      }
      if (pen == nullptr) {
        continue;  // nothing to deposit: the ground shows through
      }
      int surface_column = 0;
      int surface_row = 0;
      field.surface_cell(column, row, surface_column, surface_row);
      surface.deposit(surface_column, surface_row, *pen);
    }
  }
}

}  // namespace

std::vector<uint16_t> naplps_replacement_pattern(const NaplpsFontFace& face) {
  // The capital H's extent: the face's cap height and letter width.
  const uint16_t* capital = naplps_face_pattern(face, U'H');
  int top = face.height;
  int bottom = -1;
  unsigned spread = 0u;
  for (int row = 0; capital != nullptr && row < face.height; ++row) {
    if (capital[row] != 0u) {
      top = std::min(top, row);
      bottom = std::max(bottom, row);
      spread |= capital[row];
    }
  }
  const unsigned full = (1u << face.width) - 1u;
  if (bottom < 0) {
    top = 0;
    bottom = face.height - 1;
    spread = full;
  }
  unsigned left = 0u;
  unsigned right = 0u;
  for (int column = 0; column < face.width; ++column) {
    const unsigned bit = 1u << (face.width - 1 - column);
    if ((spread & bit) != 0u) {
      left = left == 0u ? bit : left;
      right = bit;
    }
  }
  // Every column from the leftmost to the rightmost the capital uses.
  const unsigned span = right == 0u ? 0u : ((left << 1) - right);

  std::vector<uint16_t> rows(static_cast<size_t>(std::max(0, face.height)), 0u);
  for (int row = top; row <= bottom; ++row) {
    rows[static_cast<size_t>(row)] = static_cast<uint16_t>(
        row == top || row == bottom ? span : (left | right));
  }
  return rows;
}

std::vector<NaplpsCell> NaplpsRasteriser::cells_of(const CellRect& rect) const {
  std::vector<NaplpsCell> cells;
  cells.reserve(static_cast<size_t>(rect.columns) *
                static_cast<size_t>(rect.rows));
  for (int row = 0; row < rect.rows; ++row) {
    for (int column = 0; column < rect.columns; ++column) {
      cells.push_back(
          surface_.at(rect.first_column + column, rect.first_row + row));
    }
  }
  return cells;
}

bool NaplpsRasteriser::still_holds(const CellRect& rect,
                                   const std::vector<NaplpsCell>& cells) const {
  if (cells.size() !=
      static_cast<size_t>(rect.columns) * static_cast<size_t>(rect.rows)) {
    return false;
  }
  size_t index = 0;
  for (int row = 0; row < rect.rows; ++row) {
    for (int column = 0; column < rect.columns; ++column) {
      if (!naplps_cells_match(
              surface_.at(rect.first_column + column, rect.first_row + row),
              cells[index++])) {
        return false;
      }
    }
  }
  return true;
}

void NaplpsRasteriser::put_back(const CellRect& rect,
                                const std::vector<NaplpsCell>& cells) {
  size_t index = 0;
  for (int row = 0; row < rect.rows; ++row) {
    for (int column = 0; column < rect.columns; ++column) {
      surface_.put(rect.first_column + column, rect.first_row + row,
                   cells[index++]);
    }
  }
}

void NaplpsRasteriser::deposit_character(
    const NabtsPrimitive& primitive,
    const std::vector<NabtsDrcsCharacter>& drcs, const NaplpsInk& ink,
    const NaplpsInk* background) {
  const CharacterFieldGeometry field = character_field(primitive, mapping_);

  // The surface cells the field covers, which a mark waiting for its letter
  // keeps a record of.
  CellRect rect;
  if (!field.empty()) {
    int first_column = 0;
    int first_row = 0;
    int last_column = 0;
    int last_row = 0;
    field.surface_cell(0, 0, first_column, first_row);
    field.surface_cell(field.columns - 1, field.rows - 1, last_column,
                       last_row);
    rect.first_column = std::min(first_column, last_column);
    rect.first_row = std::min(first_row, last_row);
    rect.columns = std::abs(last_column - first_column) + 1;
    rect.rows = std::abs(last_row - first_row) + 1;
  }

  // §7.2: a mark's code precedes the letter it modifies, and §5.2 has the mark
  // leave the cursor where it was, so the two share one character field. Marks
  // waiting in this field are composed onto this character if it is a letter,
  // or joined by it if it is another mark — as long as nothing has been drawn
  // over them since. Anything else leaves them standing in the spacing form
  // they were drawn in.
  const bool mark = is_nonspacing_mark(primitive);
  std::vector<NabtsPrimitive> marks;
  std::vector<NaplpsCell> before;
  bool taken_back = false;
  if (!pending_marks_.marks.empty()) {
    PendingMarks pending = std::move(pending_marks_);
    pending_marks_ = PendingMarks{};
    if ((mark || takes_marks(primitive)) &&
        nabts_same_character_field(pending.marks.back(), primitive) &&
        still_holds(pending.rect, pending.after)) {
      marks = std::move(pending.marks);
      // The field goes back to what it held before the marks, so the
      // character is drawn once, composed, rather than over the spacing form —
      // but only within the instant the marks were drawn in: what a stage
      // showed stays shown (§5.3.2.8).
      if (primitive.appear_ms == pending.appear_ms) {
        put_back(pending.rect, pending.before);
        before = std::move(pending.before);
        taken_back = true;
      }
    }
  }
  if (field.empty()) {
    return;
  }
  if (mark && !taken_back) {
    before = cells_of(rect);
  }

  // §5.1 and §5.2 leave the patterns to the receiver, so the face is settled
  // by the receiver's grid and the field it has to fill.
  const NaplpsFontFace& face = naplps_font_face_for_field(
      mapping_.grid, field.nominal_columns, field.nominal_rows);
  const PatternPlacement place =
      place_pattern(face.width, face.height, face.descent, field);

  FieldImage image(field.columns, field.rows);
  bool underline = primitive.underlined;
  switch (primitive.repertoire) {
    case NabtsPrimitive::Repertoire::kMosaic:
      image = mosaic_image(primitive, field);
      // §6.2.7.15: "Mosaic characters are not underlined."
      underline = false;
      break;

    case NabtsPrimitive::Repertoire::kDrcs:
      image = drcs_image(primitive, drcs, field);
      break;

    case NabtsPrimitive::Repertoire::kPrimary:
    case NabtsPrimitive::Repertoire::kSupplementary:
      if (mark) {
        // Drawn now in its spacing form, and kept: a letter that follows in
        // this field takes the field back and composes the two.
        marks.push_back(primitive);
        lay_spacing_marks(image, face, place, mark_codes(marks));
      } else if (!marks.empty() && !taken_back) {
        // The marks' spacing form is on screen from a stage already shown, so
        // the letter joins it rather than replacing it.
        lay_letter(image, face, place, field, primitive, {});
        lay_spacing_marks(image, face, place, mark_codes(marks));
      } else {
        lay_letter(image, face, place, field, primitive, mark_codes(marks));
      }
      break;
  }

  // Too small to present: one pixel of the reference grid at the field's
  // origin, as the Norpak TTX 650 draws it (naplps_character_too_small()).
  if (!mark && naplps_character_too_small(primitive, drcs)) {
    image = FieldImage(field.columns, field.rows);
    const int reference_pixel = std::max(
        1, static_cast<int>(std::lround(mapping_.columns_per_unit / 256.0)));
    image.fill(0, reference_pixel, 0, reference_pixel);
  }

  deposit_field_image(surface_, field, image, primitive, underline, ink,
                      background);

  if (mark) {
    pending_marks_.marks = std::move(marks);
    pending_marks_.rect = rect;
    pending_marks_.before = std::move(before);
    pending_marks_.after = cells_of(rect);
    pending_marks_.appear_ms = primitive.appear_ms;
  }
}

// ---------------------------------------------------------------------------
// Incremental colour runs
// ---------------------------------------------------------------------------

namespace {

/// §5.3.3.6.3's rescaling of one pel dimension, in cells: a whole number of
/// cells where the pel covers at least one, and a whole fraction of a cell
/// where it covers less. Never larger than the pel, so a rescaled picture
/// stays inside its field.
double rescaled_cells(double cells) {
  const double extent = std::fabs(cells);
  if (!(extent > kCoincident)) {
    return 0.0;
  }
  if (extent >= 1.0) {
    return std::floor(extent);
  }
  return 1.0 / std::ceil(1.0 / extent);
}

}  // namespace

void NaplpsRasteriser::deposit_incremental_row(
    const NabtsPoint& start, const NabtsSize& pel,
    const NabtsPoint& field_origin, const std::vector<NaplpsInk>& colours) {
  const NaplpsCellSize pel_cells = mapping_.cell_size(pel);
  const double width = rescaled_cells(pel_cells.columns);
  const double height = rescaled_cells(pel_cells.rows);
  if (!(width > 0.0) || !(height > 0.0)) {
    return;
  }
  // The field is scaled with the pel, about its origin, so the drawing point
  // keeps its place in it.
  const double scale_x = width / std::fabs(pel_cells.columns);
  const double scale_y = height / std::fabs(pel_cells.rows);
  const NaplpsCellPoint origin = mapping_.cell_point(field_origin);
  const NaplpsCellPoint first = mapping_.cell_point(start);
  const double row = origin.row + (first.row - origin.row) * scale_y;
  const NaplpsCellSize scaled{std::copysign(width, pel_cells.columns),
                              std::copysign(height, pel_cells.rows)};
  for (size_t i = 0; i < colours.size(); ++i) {
    const double column = origin.column +
                          (first.column - origin.column) * scale_x +
                          static_cast<double>(i) * scaled.columns;
    stamp_pel_cells(NaplpsCellPoint{column, row}, scaled, colours[i]);
  }
}

void NaplpsRasteriser::scroll_area(const NabtsPoint& origin,
                                   const NabtsSize& size,
                                   const NabtsPoint& displacement,
                                   const NabtsSize& pel,
                                   const NaplpsInk* fill) {
  const NaplpsCellPoint corner = mapping_.cell_point(origin);
  const NaplpsCellSize extent = mapping_.cell_size(size);
  const int first_column = static_cast<int>(
      std::lround(std::min(corner.column, corner.column + extent.columns)));
  const int last_column = static_cast<int>(
      std::lround(std::max(corner.column, corner.column + extent.columns)));
  const int first_row = static_cast<int>(
      std::lround(std::min(corner.row, corner.row + extent.rows)));
  const int last_row = static_cast<int>(
      std::lround(std::max(corner.row, corner.row + extent.rows)));

  const double moved_columns = mapping_.columns_across(displacement.x);
  const double moved_rows = mapping_.rows_up(displacement.y);
  const NaplpsCellSize pel_cells = mapping_.cell_size(pel);
  const auto whole = [](double cells, double pel_extent) {
    const double magnitude = std::fabs(cells);
    double count = 0.0;
    if (std::fabs(pel_extent) > kCoincident) {
      // A pel's worth, rescaled as the pel is (§5.3.3.6.3).
      count = std::round(rescaled_cells(pel_extent) *
                         (magnitude / std::fabs(pel_extent)));
    } else {
      // Far enough to bring the next line wholly in (§6.2.7.13).
      count = std::ceil(magnitude - kCoincident);
    }
    return static_cast<int>(cells < 0.0 ? -count : count);
  };
  surface_.shift_area(first_column, last_column, first_row, last_row,
                      whole(moved_columns, pel_cells.columns),
                      whole(moved_rows, pel_cells.rows), fill);
}

}  // namespace orc
