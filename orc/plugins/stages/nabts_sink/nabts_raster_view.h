/*
 * File:        nabts_raster_view.h
 * Module:      nabts_sink stage plugin
 * Purpose:     A NAPLPS page deposited into a receiver's pixel grid and
 *              emitted as a scalable display list
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_SINK_NABTS_RASTER_VIEW_H
#define ORC_NABTS_SINK_NABTS_RASTER_VIEW_H

#include <orc/stage/tooling/catalogue_results.h>

#include <array>
#include <cstdint>
#include <vector>

#include "naplps_palette_timeline.h"
#include "naplps_render_grid.h"
#include "vbi-services/nabts_page.h"

namespace orc {

/// A stored colour as a display list carries a plain value: each gun scaled
/// from 0 to kNabtsGunMax onto 0 to 255, full scale to full scale.
CatalogueColour naplps_catalogue_colour(const NabtsColour& colour);

/**
 * @brief One colour as a display list carries it
 *
 * The value is the still colour — the entry's contents in the final map,
 * blinks at rest — and where the entry the pixel stores (@p map_address)
 * changes as the page is shown, the entry is named too, so a renderer
 * playing the page follows it. An entry that holds still is left unnamed:
 * the value says all there is to say, and runs of one colour from two entries
 * that never change can then be drawn as one.
 */
CatalogueColour naplps_catalogue_colour(const NabtsColour& colour,
                                        int16_t map_address,
                                        const NaplpsPaletteTimeline& palette);

/**
 * @brief Set @p list's palette: the final map, and how its entries change
 *
 * The tracks are left empty for a page whose colours hold still, which is
 * what tells a renderer there is nothing to play.
 */
void naplps_emit_palette(const NabtsPageSnapshot& snapshot,
                         const NaplpsPaletteTimeline& palette,
                         CatalogueDisplayList& list);

/**
 * @brief One step of the paint stream: a primitive, or a clear
 *
 * The primitives a clear removed (NabtsPresentationTimeline::superseded) and
 * those still on screen, in the order they were drawn, with the clears where
 * they fell — which is everything a renderer replaying the page needs, and
 * is the same walk for the pixel and the vector emissions.
 */
struct NaplpsPaintStep {
  uint32_t time_ms = 0;
  /// The primitive drawn, or nullptr for a clear of the display.
  const NabtsPrimitive* primitive = nullptr;
  /// How long the receiver took over the step, ending at |time_ms|
  /// (NabtsPrimitive::draw_ms, NabtsDisplayClear::draw_ms).
  uint32_t draw_ms = 0;
};

/// @p snapshot's paint stream, in execution order, which is also time order.
std::vector<NaplpsPaintStep> naplps_paint_stream(
    const NabtsPageSnapshot& snapshot);

/**
 * @brief The rows of the display @p primitive fills, if it is a fill
 *
 * A filled rectangle, polygon or arc is what a receiver swept across its
 * screen a row at a time. Sets @p low and @p high to the unit y its rows on
 * the display run between and returns true; false for anything else, or a
 * fill wholly off the display.
 */
bool naplps_fill_rows(const NabtsPrimitive& primitive, double& low,
                      double& high);

/**
 * @brief Have @p op appear the way a receiver drew a fill or a clear: a row at
 *        a time, over the @p draw_ms before its appear time
 *
 * The sweep runs between unit y @p low and @p high, down from the top or up
 * from the bottom as @p sweep says (CatalogueDrawOp::reveal_duration_ms).
 * Nothing is set where @p draw_ms is 0 or the rows are none, so a page played
 * without a receiver's pace keeps every operation appearing whole.
 */
void naplps_reveal_op(CatalogueDrawOp& op, uint32_t draw_ms, double low,
                      double high, NabtsFillSweep sweep);

class NaplpsRasteriser;

/// What drawing a primitive needs beyond the primitive itself.
struct NaplpsDepositContext {
  /// The four programmable texture masks (§5.3.2.4.4), or nullptr for none.
  const NabtsTextureMask* masks = nullptr;
  /// The DRCS characters a kCharacter of that repertoire is drawn from.
  const std::vector<NabtsDrcsCharacter>* drcs = nullptr;
  /// The colour map entries that change as the page is shown; an ink naming
  /// any other entry is deposited as its colour alone. nullptr keeps every
  /// entry.
  const std::array<bool, kNabtsColourMapEntries>* animated = nullptr;
};

/**
 * @brief Draw one primitive the way a receiver executes it
 *
 * Every drawing rule of X3.110 lives here once, for the display and for the
 * DRCS and texture-mask buffers alike (§6.2.3: definition code executes "in
 * the same manner as if it were being displayed"): the pel, line and fill
 * textures with colour mode 2's background gaps, highlights, the programmable
 * masks, characters, INCREMENTAL POINT rows and scrolls.
 */
void naplps_deposit_primitive(const NabtsPrimitive& primitive,
                              const NaplpsDepositContext& context,
                              NaplpsRasteriser& raster);

/**
 * @brief Draw @p snapshot into a @p grid receiver and append it to @p list
 *
 * This is the pixel-mode reading of a page: rather than handing a renderer the
 * geometry and letting it decide what a stroke of no stated width looks like,
 * the page is deposited into the receiver's frame buffer exactly as X3.110
 * describes and the resulting pixels are what get emitted.
 *
 * The pixels are emitted as run-length merged rectangles in unit space, not as
 * a bitmap, so a renderer can draw a receiver's rectangular pixels at any size
 * with their edges intact. Every run is a filled rectangle with no pen, which
 * is a shape every existing renderer of a display list already draws — the
 * pixel modes need no new operation kind.
 *
 * A page drawn over time (X3.110 §5.3.2.8 WAIT) is emitted stage by stage: the
 * first stage exactly as a page drawn at once would be, and each later one as
 * the runs of pixels it changed, stamped with the moment it was drawn — or,
 * where the stage cleared the display, a clear followed by everything then on
 * screen. Drawing every operation in order therefore still gives the final
 * page, and drawing those up to a time gives the page as it stood then. The
 * palette and its tracks come with it (naplps_emit_palette()).
 *
 * @p list keeps whatever aspect and nominal grid the caller has already set;
 * the palette is set and the operations are appended.
 */
void naplps_emit_raster_page(const NabtsPageSnapshot& snapshot,
                             NaplpsRenderGrid grid, CatalogueDisplayList& list);

}  // namespace orc

#endif  // ORC_NABTS_SINK_NABTS_RASTER_VIEW_H
