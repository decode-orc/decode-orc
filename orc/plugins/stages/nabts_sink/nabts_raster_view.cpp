/*
 * File:        nabts_raster_view.cpp
 * Module:      nabts_sink stage plugin
 * Purpose:     A NAPLPS page deposited into a receiver's pixel grid and
 *              emitted as a scalable display list
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_raster_view.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "naplps_raster.h"

namespace orc {

namespace {

// Full scale of a gun in the stored colour (nabts_page.h).
constexpr int kGunMax = kNabtsGunMax;

// The tolerance an arc's rows are found at: the curve as a polyline within
// half a pixel of the reference receiver, as its drawing time is measured.
constexpr double kArcRowTolerance = 1.0 / 512.0;

uint8_t to_channel(uint8_t gun) {
  return static_cast<uint8_t>((std::min<int>(gun, kGunMax) * 255) / kGunMax);
}

/**
 * @brief The colour a highlighted figure's outline is drawn in
 *
 * §5.3.2.4.3: the outline is drawn "in nominal black in color modes 0 and 1,
 * and in the background color in color mode 2".
 */
NaplpsInk highlight_ink(const NabtsPrimitive& primitive) {
  if (primitive.colour_mode == NabtsColourMode::kMappedWithBackground) {
    return naplps_background_ink_of(primitive);
  }
  NaplpsInk ink;
  ink.colour = kNabtsNominalBlack;
  return ink;
}

/// The texture mask a primitive's fill pattern selects, or nullptr where the
/// pattern is not one of the programmable four.
const NabtsTextureMask* mask_for(const NabtsPrimitive& primitive,
                                 const NabtsTextureMask* masks) {
  const auto pattern = static_cast<int>(primitive.texture_pattern);
  const auto first_mask = static_cast<int>(NabtsTexturePattern::kMaskA);
  if (pattern < first_mask || masks == nullptr) {
    return nullptr;
  }
  const size_t slot = static_cast<size_t>(pattern - first_mask);
  if (slot >= kNabtsTextureMaskCount) {
    return nullptr;
  }
  return &masks[slot];
}

}  // namespace

void naplps_deposit_primitive(const NabtsPrimitive& primitive,
                              const NaplpsDepositContext& context,
                              NaplpsRasteriser& raster) {
  const auto shown = [&context](NaplpsInk ink) {
    if (context.animated != nullptr && ink.colour_map_address >= 0 &&
        !(*context.animated)[static_cast<size_t>(ink.colour_map_address) %
                             kNabtsColourMapEntries]) {
      ink.colour_map_address = -1;
    }
    return ink;
  };
  static const std::vector<NabtsDrcsCharacter> kNoDrcs;
  const std::vector<NabtsDrcsCharacter>& drcs =
      context.drcs != nullptr ? *context.drcs : kNoDrcs;

  const NaplpsInk ink = shown(naplps_ink_of(primitive));
  const bool has_background =
      primitive.colour_mode == NabtsColourMode::kMappedWithBackground;
  const NaplpsInk background_ink = shown(naplps_background_ink_of(primitive));
  const NaplpsInk* background = has_background ? &background_ink : nullptr;
  // §5.3.2.4.2 and §5.3.2.4.4: in colour mode 2 the gaps of a line texture
  // and of a fill pattern are drawn in the background colour.
  const NaplpsInk* gaps = background;
  // A figure's outline, or its fill and, highlighted, the outline the fill is
  // edged with (§5.3.2.4.3). |vertices| says whether the points are the
  // figure's vertices rather than a curve flattened into them.
  const auto draw_figure = [&](const std::vector<NabtsPoint>& points,
                               bool closed, bool vertices) {
    if (!primitive.filled) {
      raster.stroke_path(points, primitive.logical_pel, primitive.line_texture,
                         ink, closed, gaps, vertices);
      return;
    }
    raster.fill_path(points, primitive.logical_pel, primitive.texture_pattern,
                     primitive.texture_mask_size,
                     mask_for(primitive, context.masks), ink, gaps);
    if (primitive.highlighted) {
      raster.highlight_path(points, primitive.logical_pel,
                            shown(highlight_ink(primitive)), closed);
    }
  };

  switch (primitive.kind) {
    case NabtsPrimitiveKind::kPoint:
      if (!primitive.points.empty()) {
        raster.stamp_pel(primitive.points.front(), primitive.logical_pel, ink);
      }
      return;

    case NabtsPrimitiveKind::kLine:
      raster.stroke_path(primitive.points, primitive.logical_pel,
                         primitive.line_texture, ink, /*closed=*/false, gaps);
      return;

    case NabtsPrimitiveKind::kArc: {
      // An arc through three points is traced cell by cell as the receivers
      // of the day traced it (NaplpsRasteriser::arc_cells()); a spline,
      // through more, is drawn along the polyline that stands in for it.
      std::vector<std::pair<int, int>> path;
      bool circle = false;
      bool collinear = false;
      if (raster.arc_cells(primitive.points, path, circle, collinear)) {
        if (!primitive.filled) {
          raster.stroke_cells(path, primitive.logical_pel,
                              primitive.line_texture, ink, gaps);
          return;
        }
        std::vector<std::pair<int, int>> outline = path;
        if (!circle) {
          raster.append_chord(outline);
        }
        raster.fill_cells(outline, primitive.logical_pel,
                          primitive.texture_pattern,
                          primitive.texture_mask_size,
                          mask_for(primitive, context.masks), ink, gaps);
        if (primitive.highlighted) {
          // §5.3.3.3: "the chord is not considered a part of the arc and, as
          // such, is not highlighted".
          raster.stroke_cells(path, primitive.logical_pel,
                              NabtsLineTexture::kSolid,
                              shown(highlight_ink(primitive)), nullptr);
        }
        return;
      }
      if (collinear) {
        // No circle passes through three points on a line: the receivers draw
        // a line from the start to the intermediate point and on to the end.
        raster.stroke_path(primitive.points, primitive.logical_pel,
                           primitive.line_texture, ink, /*closed=*/false, gaps);
        return;
      }
      // §5.3.3.3 keeps the chord out of the highlight — "the chord is not
      // considered a part of the arc and, as such, is not highlighted" — so
      // the open arc is stroked, not the closure.
      draw_figure(raster.arc_polyline(primitive.points), /*closed=*/false,
                  /*vertices=*/false);
      return;
    }

    case NabtsPrimitiveKind::kRectangle: {
      // A negative extent draws back from the origin, which taking the corners
      // in order resolves.
      const NabtsPoint origin = primitive.origin;
      const NabtsPoint far{origin.x + primitive.size.dx,
                           origin.y + primitive.size.dy};
      draw_figure({origin, NabtsPoint{far.x, origin.y}, far,
                   NabtsPoint{origin.x, far.y}},
                  /*closed=*/true, /*vertices=*/true);
      return;
    }

    case NabtsPrimitiveKind::kPolygon:
      // X3.110 §5.3.3.5.1: "There is implicit closure between the start point
      // and the last vertex specified".
      draw_figure(primitive.points, /*closed=*/true, /*vertices=*/true);
      return;

    case NabtsPrimitiveKind::kIncrementalPoints: {
      // §5.3.3.6.3: each colour is a value in colour mode 0 and an entry in
      // modes 1 and 2, already resolved by the interpreter.
      std::vector<NaplpsInk> colours;
      colours.reserve(primitive.incremental_colours.size());
      for (size_t i = 0; i < primitive.incremental_colours.size(); ++i) {
        NaplpsInk pel_ink;
        pel_ink.colour = primitive.incremental_colours[i];
        pel_ink.colour_map_address = i < primitive.incremental_addresses.size()
                                         ? primitive.incremental_addresses[i]
                                         : int16_t{-1};
        colours.push_back(shown(pel_ink));
      }
      const NabtsPoint field_origin = primitive.points.empty()
                                          ? primitive.origin
                                          : primitive.points.front();
      raster.deposit_incremental_row(primitive.origin, primitive.logical_pel,
                                     field_origin, colours);
      return;
    }

    case NabtsPrimitiveKind::kScroll:
      // §6.2.7.13: what is scrolled in is "nominal black in color modes 0 and
      // 1 and the background color in color mode 2".
      if (!primitive.points.empty()) {
        raster.scroll_area(primitive.origin, primitive.size,
                           primitive.points.front(), primitive.logical_pel,
                           background);
      }
      return;

    case NabtsPrimitiveKind::kCharacter:
      raster.deposit_character(primitive, drcs, ink, background);
      return;
  }
}

namespace {

/// One run of cells as a display-list operation, drawn from |appear_ms| on.
CatalogueDrawOp run_op(const NaplpsCellRun& run, const NaplpsRenderGrid& grid,
                       const NaplpsPaletteTimeline& palette,
                       uint32_t appear_ms) {
  const double pitch_x = grid.pitch_x();
  const double pitch_y = grid.pitch_y();
  CatalogueDrawOp op;
  op.kind = CatalogueDrawKind::kRectangle;
  op.filled = true;
  // Unit space, so a renderer scales the run with the rest of the page. The
  // run's own extent is its cells, which is what keeps a receiver's pixel a
  // rectangle rather than a point.
  op.origin = CataloguePoint{run.column * pitch_x, run.row * pitch_y};
  op.size = CatalogueSize{run.columns * pitch_x, pitch_y};
  op.points.push_back(op.origin);
  op.colour = naplps_catalogue_colour(run.cell.colour,
                                      run.cell.colour_map_address, palette);
  op.appear_ms = appear_ms;
  return op;
}

/// The whole display area in nominal black, which is what a clear leaves.
CatalogueDrawOp clear_op(const NaplpsRenderGrid& grid, uint32_t appear_ms) {
  CatalogueDrawOp op;
  op.kind = CatalogueDrawKind::kRectangle;
  op.filled = true;
  op.origin = CataloguePoint{0.0, 0.0};
  op.size =
      CatalogueSize{grid.width * grid.pitch_x(), grid.height * grid.pitch_y()};
  op.points.push_back(op.origin);
  op.colour = naplps_catalogue_colour(kNabtsNominalBlack);
  op.appear_ms = appear_ms;
  return op;
}

}  // namespace

CatalogueColour naplps_catalogue_colour(const NabtsColour& colour) {
  CatalogueColour out;
  out.red = to_channel(colour.red);
  out.green = to_channel(colour.green);
  out.blue = to_channel(colour.blue);
  out.transparent = colour.transparent;
  return out;
}

CatalogueColour naplps_catalogue_colour(const NabtsColour& colour,
                                        int16_t map_address,
                                        const NaplpsPaletteTimeline& palette) {
  CatalogueColour out = naplps_catalogue_colour(colour);
  if (map_address >= 0) {
    const size_t entry =
        static_cast<size_t>(map_address) % kNabtsColourMapEntries;
    if (palette.tracks[entry].changes()) {
      out.palette_index = static_cast<int16_t>(entry);
    }
  }
  return out;
}

void naplps_emit_palette(const NabtsPageSnapshot& snapshot,
                         const NaplpsPaletteTimeline& palette,
                         CatalogueDisplayList& list) {
  list.palette.clear();
  list.palette.reserve(kNabtsColourMapEntries);
  for (size_t i = 0; i < kNabtsColourMapEntries; ++i) {
    list.palette.push_back(naplps_catalogue_colour(snapshot.colour_map[i]));
  }
  list.palette_tracks.clear();
  if (!palette.changes()) {
    return;
  }
  list.palette_tracks.resize(kNabtsColourMapEntries);
  for (size_t i = 0; i < kNabtsColourMapEntries; ++i) {
    const NaplpsPaletteTrack& track = palette.tracks[i];
    CataloguePaletteTrack& out = list.palette_tracks[i];
    out.steps.reserve(track.steps.size());
    for (const NaplpsColourStep& step : track.steps) {
      out.steps.push_back(CatalogueColourStep{
          step.time_ms, naplps_catalogue_colour(step.colour)});
    }
    out.loop_start_ms = track.loop_start_ms;
    out.loop_period_ms = track.loop_period_ms;
  }
}

std::vector<NaplpsPaintStep> naplps_paint_stream(
    const NabtsPageSnapshot& snapshot) {
  const NabtsPresentationTimeline& timeline = snapshot.timeline;
  std::vector<NaplpsPaintStep> out;
  out.reserve(timeline.superseded.size() + snapshot.primitives.size() +
              timeline.clears.size());
  size_t clear = 0;
  size_t index = 0;
  const auto clears_before = [&](size_t position) {
    while (clear < timeline.clears.size() &&
           timeline.clears[clear].stream_index <= position) {
      out.push_back(NaplpsPaintStep{timeline.clears[clear].time_ms, nullptr,
                                    timeline.clears[clear].draw_ms});
      ++clear;
    }
  };
  for (const NabtsPrimitive& primitive : timeline.superseded) {
    clears_before(index++);
    out.push_back(
        NaplpsPaintStep{primitive.appear_ms, &primitive, primitive.draw_ms});
  }
  for (const NabtsPrimitive& primitive : snapshot.primitives) {
    clears_before(index++);
    out.push_back(
        NaplpsPaintStep{primitive.appear_ms, &primitive, primitive.draw_ms});
  }
  clears_before(index);
  return out;
}

bool naplps_fill_rows(const NabtsPrimitive& primitive, double& low,
                      double& high) {
  if (!primitive.filled) {
    return false;
  }
  std::vector<NabtsPoint> outline;
  switch (primitive.kind) {
    case NabtsPrimitiveKind::kRectangle:
      outline = {primitive.origin,
                 NabtsPoint{primitive.origin.x + primitive.size.dx,
                            primitive.origin.y + primitive.size.dy}};
      break;
    case NabtsPrimitiveKind::kPolygon:
      outline = primitive.points;
      break;
    case NabtsPrimitiveKind::kArc:
      // An arc's rows reach past its control points wherever it bulges past
      // them, so they are the curve's own.
      outline = naplps_arc_polyline(primitive.points, kArcRowTolerance);
      break;
    default:
      return false;
  }
  if (outline.empty()) {
    return false;
  }
  low = outline.front().y;
  high = outline.front().y;
  for (const NabtsPoint& point : outline) {
    low = std::min(low, point.y);
    high = std::max(high, point.y);
  }
  // The receiver sweeps the rows it has: those on the display.
  low = std::max(low, 0.0);
  high = std::min(high, kNabtsDisplayAreaHeight);
  return high > low;
}

void naplps_reveal_op(CatalogueDrawOp& op, uint32_t draw_ms, double low,
                      double high, NabtsFillSweep sweep) {
  if (draw_ms == 0 || !(high > low)) {
    return;
  }
  op.reveal_duration_ms = std::min(draw_ms, op.appear_ms);
  const bool down = sweep == NabtsFillSweep::kTopDown;
  op.reveal_from_y = down ? high : low;
  op.reveal_to_y = down ? low : high;
}

void naplps_emit_raster_page(const NabtsPageSnapshot& snapshot,
                             NaplpsRenderGrid grid,
                             CatalogueDisplayList& list) {
  const NaplpsPaletteTimeline palette =
      naplps_palette_timeline(snapshot.timeline);
  naplps_emit_palette(snapshot, palette, list);
  // A cell keeps its entry only where the entry changes: one that holds still
  // is just its colour, and two cells of that colour then merge into one run
  // however they were addressed.
  std::array<bool, kNabtsColourMapEntries> animated{};
  for (size_t i = 0; i < kNabtsColourMapEntries; ++i) {
    animated[i] = palette.tracks[i].changes();
  }
  NaplpsDepositContext context;
  context.masks = snapshot.texture_masks;
  context.drcs = &snapshot.drcs;
  context.animated = &animated;

  NaplpsCellSurface surface(grid);
  NaplpsGridMapping mapping{grid};
  NaplpsRasteriser raster(surface, mapping);
  // What the operations emitted so far draw, which a stage is measured
  // against.
  NaplpsCellSurface on_screen(grid);
  bool anything_shown = false;

  // The operations below are the receiver's pixels, so a renderer draws them
  // hard-edged: their boundaries are the content, not an artefact of drawing.
  list.pixel_aligned = true;

  // §4.2.3 builds the picture in layers, "with the effects of each superimposed
  // over those of previous ones", so the primitives are deposited in the order
  // they were executed and a later one simply overwrites the pixels of an
  // earlier one. The stream is taken a stage — one instant of the
  // presentation clock — at a time, and each stage emits what it changed.
  const std::vector<NaplpsPaintStep> stream = naplps_paint_stream(snapshot);
  size_t next = 0;
  while (next < stream.size()) {
    const uint32_t time = stream[next].time_ms;
    size_t end = next;
    while (end < stream.size() && stream[end].time_ms == time) {
      ++end;
    }
    // A stage shows the display as it stands at the end of its instant, so
    // whatever a clear in the same instant wipes is never seen, and is not
    // drawn: a damaged record's stray CS bytes can otherwise have hundreds of
    // figures drawn only to be cleared again before anything is shown.
    bool cleared = false;
    for (size_t i = next; i < end; ++i) {
      if (stream[i].primitive == nullptr) {
        next = i + 1;
        cleared = true;
      }
    }
    // At a receiver's pace a stage is one figure, and one the receiver filled
    // or cleared a row at a time is swept onto the screen over the time it
    // took (NabtsPresentationTimeline::fill_sweep).
    uint32_t sweep_ms = cleared ? stream[next - 1].draw_ms : 0;
    for (size_t i = next; i < end; ++i) {
      double low = 0.0;
      double high = 0.0;
      if (stream[i].draw_ms > sweep_ms &&
          naplps_fill_rows(*stream[i].primitive, low, high)) {
        sweep_ms = stream[i].draw_ms;
      }
    }
    if (cleared) {
      surface.clear();
      raster.forget_pending_marks();
    }
    for (; next < end; ++next) {
      naplps_deposit_primitive(*stream[next].primitive, context, raster);
    }

    const size_t first_op = list.ops.size();
    std::vector<NaplpsCellRun> runs;
    if (cleared || !anything_shown) {
      // A cleared stage starts again from black, so it is everything now on
      // screen after a clear of what was.
      if (cleared && anything_shown) {
        list.ops.push_back(clear_op(grid, time));
      }
      runs = naplps_merge_runs(surface);
      on_screen = surface;
    } else {
      runs = naplps_changed_runs(surface, on_screen);
      on_screen.copy_dirty_rows_from(surface);
    }
    surface.forget_dirty();

    list.ops.reserve(list.ops.size() + runs.size());
    for (const NaplpsCellRun& run : runs) {
      list.ops.push_back(run_op(run, grid, palette, time));
    }
    anything_shown = anything_shown || !runs.empty();

    if (sweep_ms > 0 && list.ops.size() > first_op) {
      // The rows the stage changed, or the whole display a clear swept; each
      // run is one row, so each appears as the sweep reaches it.
      double low = 0.0;
      double high = grid.height * grid.pitch_y();
      if (!cleared) {
        low = high;
        high = 0.0;
        for (const NaplpsCellRun& run : runs) {
          low = std::min(low, run.row * grid.pitch_y());
          high = std::max(high, (run.row + 1) * grid.pitch_y());
        }
      }
      for (size_t i = first_op; i < list.ops.size(); ++i) {
        naplps_reveal_op(list.ops[i], sweep_ms, low, high,
                         snapshot.timeline.fill_sweep);
      }
    }
  }
}

}  // namespace orc
