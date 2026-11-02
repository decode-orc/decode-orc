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
#include <cstddef>
#include <cstdint>
#include <vector>

#include "naplps_raster.h"

namespace orc {

namespace {

// Bits per gun in the transmitted colour (X3.110 §5.3.1, Table D1 item 5(4)).
constexpr int kGunBits = 3;
constexpr int kGunMax = (1 << kGunBits) - 1;  // 7

uint8_t to_channel(uint8_t gun) {
  return static_cast<uint8_t>((std::min<int>(gun, kGunMax) * 255) / kGunMax);
}

CatalogueColour to_colour(const NabtsColour& colour) {
  CatalogueColour out;
  out.red = to_channel(colour.red);
  out.green = to_channel(colour.green);
  out.blue = to_channel(colour.blue);
  out.transparent = colour.transparent;
  return out;
}

/**
 * @brief One six-bit numeric colour specification as a colour (X3.110 Fig. 12)
 *
 * The six payload bits are two three-tuples in the order green, red, blue, so
 * each gun gets two bits taken one per tuple, most significant first. §5.3.2.5
 * zero-extends a value with fewer bits than the map holds, which for two bits
 * into three is a single shift.
 */
NabtsColour colour_from_numeric(uint8_t payload) {
  const auto gun = [payload](int offset) {
    const uint32_t high = (payload >> (3 + offset)) & 0x1u;
    const uint32_t low = (payload >> offset) & 0x1u;
    return static_cast<uint8_t>(((high << 1) | low) << (kGunBits - 2));
  };
  NabtsColour colour;
  colour.green = gun(2);
  colour.red = gun(1);
  colour.blue = gun(0);
  return colour;
}

/// One entry of an incremental colour run, resolved (§5.3.3.6.3): a value in
/// colour mode 0, a colour-map address in modes 1 and 2.
NaplpsInk incremental_ink(uint8_t entry, const NabtsPrimitive& primitive,
                          const NabtsPageSnapshot& snapshot) {
  NaplpsInk ink = naplps_ink_of(primitive);
  if (primitive.colour_mode == NabtsColourMode::kDirect) {
    ink.colour = colour_from_numeric(entry);
    ink.colour_map_address = -1;
    return ink;
  }
  const size_t address = static_cast<size_t>(entry) % kNabtsColourMapEntries;
  ink.colour = snapshot.colour_map[address];
  ink.colour_map_address = static_cast<int16_t>(address);
  return ink;
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
                                 const NabtsPageSnapshot& snapshot) {
  const auto pattern = static_cast<int>(primitive.texture_pattern);
  const auto first_mask = static_cast<int>(NabtsTexturePattern::kMaskA);
  if (pattern < first_mask) {
    return nullptr;
  }
  const size_t slot = static_cast<size_t>(pattern - first_mask);
  if (slot >= kNabtsTextureMaskCount) {
    return nullptr;
  }
  return &snapshot.texture_masks[slot];
}

/// Draw one primitive into the surface, the way a receiver would execute it.
void deposit_primitive(const NabtsPrimitive& primitive,
                       const NabtsPageSnapshot& snapshot,
                       NaplpsRasteriser& raster) {
  const NaplpsInk ink = naplps_ink_of(primitive);
  const bool has_background =
      primitive.colour_mode == NabtsColourMode::kMappedWithBackground;
  const NaplpsInk background_ink = naplps_background_ink_of(primitive);
  const NaplpsInk* background = has_background ? &background_ink : nullptr;

  switch (primitive.kind) {
    case NabtsPrimitiveKind::kPoint:
      if (!primitive.points.empty()) {
        raster.stamp_pel(primitive.points.front(), primitive.logical_pel, ink);
      }
      return;

    case NabtsPrimitiveKind::kLine:
      raster.stroke_path(primitive.points, primitive.logical_pel,
                         primitive.line_texture, ink);
      return;

    case NabtsPrimitiveKind::kArc: {
      const std::vector<NabtsPoint> outline =
          raster.arc_polyline(primitive.points);
      if (!primitive.filled) {
        raster.stroke_path(outline, primitive.logical_pel,
                           primitive.line_texture, ink);
        return;
      }
      raster.fill_path(outline, primitive.logical_pel,
                       primitive.texture_pattern, primitive.texture_mask_size,
                       mask_for(primitive, snapshot), ink);
      if (primitive.highlighted) {
        // §5.3.3.3 keeps the chord out of the highlight — "the chord is not
        // considered a part of the arc and, as such, is not highlighted" — so
        // the open arc is stroked, not the closure.
        raster.highlight_path(outline, primitive.logical_pel,
                              highlight_ink(primitive), /*closed=*/false);
      }
      return;
    }

    case NabtsPrimitiveKind::kRectangle: {
      // A negative extent draws back from the origin, which taking the corners
      // in order resolves.
      const NabtsPoint origin = primitive.origin;
      const NabtsPoint far{origin.x + primitive.size.dx,
                           origin.y + primitive.size.dy};
      const std::vector<NabtsPoint> corners = {origin,
                                               NabtsPoint{far.x, origin.y}, far,
                                               NabtsPoint{origin.x, far.y}};
      if (!primitive.filled) {
        raster.stroke_path(corners, primitive.logical_pel,
                           primitive.line_texture, ink, /*closed=*/true);
        return;
      }
      raster.fill_path(corners, primitive.logical_pel,
                       primitive.texture_pattern, primitive.texture_mask_size,
                       mask_for(primitive, snapshot), ink);
      if (primitive.highlighted) {
        raster.highlight_path(corners, primitive.logical_pel,
                              highlight_ink(primitive));
      }
      return;
    }

    case NabtsPrimitiveKind::kPolygon: {
      if (!primitive.filled) {
        raster.stroke_path(primitive.points, primitive.logical_pel,
                           primitive.line_texture, ink);
        return;
      }
      raster.fill_path(primitive.points, primitive.logical_pel,
                       primitive.texture_pattern, primitive.texture_mask_size,
                       mask_for(primitive, snapshot), ink);
      if (primitive.highlighted) {
        raster.highlight_path(primitive.points, primitive.logical_pel,
                              highlight_ink(primitive));
      }
      return;
    }

    case NabtsPrimitiveKind::kIncrementalPoints: {
      std::vector<NaplpsInk> colours;
      colours.reserve(primitive.incremental_colours.size());
      for (const uint8_t entry : primitive.incremental_colours) {
        colours.push_back(incremental_ink(entry, primitive, snapshot));
      }
      raster.deposit_colour_run(primitive.origin, primitive.size,
                                primitive.logical_pel, colours);
      return;
    }

    case NabtsPrimitiveKind::kCharacter:
      raster.deposit_character(primitive, snapshot.drcs, ink, background);
      return;
  }
}

}  // namespace

void naplps_emit_raster_page(const NabtsPageSnapshot& snapshot,
                             NaplpsRenderGrid grid,
                             CatalogueDisplayList& list) {
  NaplpsCellSurface surface(grid);
  NaplpsGridMapping mapping{grid};
  NaplpsRasteriser raster(surface, mapping);

  // §4.2.3 builds the picture in layers, "with the effects of each superimposed
  // over those of previous ones", so the primitives are deposited in the order
  // they were executed and a later one simply overwrites the pixels of an
  // earlier one.
  for (const NabtsPrimitive& primitive : snapshot.primitives) {
    deposit_primitive(primitive, snapshot, raster);
  }

  const std::vector<NaplpsCellRun> runs = naplps_merge_runs(surface);
  list.ops.reserve(list.ops.size() + runs.size());
  // The operations below are the receiver's pixels, so a renderer draws them
  // hard-edged: their boundaries are the content, not an artefact of drawing.
  list.pixel_aligned = true;

  const double pitch_x = grid.pitch_x();
  const double pitch_y = grid.pitch_y();
  for (const NaplpsCellRun& run : runs) {
    CatalogueDrawOp op;
    op.kind = CatalogueDrawKind::kRectangle;
    op.filled = true;
    // Unit space, so a renderer scales the run with the rest of the page. The
    // run's own extent is its cells, which is what keeps a receiver's pixel a
    // rectangle rather than a point.
    op.origin = CataloguePoint{run.column * pitch_x, run.row * pitch_y};
    op.size = CatalogueSize{run.columns * pitch_x, pitch_y};
    op.points.push_back(op.origin);
    op.colour = to_colour(run.cell.colour);
    list.ops.push_back(std::move(op));
  }
}

}  // namespace orc
