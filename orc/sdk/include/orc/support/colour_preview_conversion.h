/*
 * File:        colour_preview_conversion.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Render-boundary conversion from colour carriers to PreviewImage.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/stage/preview/orc_preview_carriers.h>
#include <orc/stage/preview/orc_rendering.h>

#include <memory>
#include <vector>

namespace orc {

/**
 * @brief Convert a colour-domain carrier to display-target RGB888 preview.
 *
 * The carrier's U/V are the composite-modulated colour-difference signals, so
 * the modulation weights are removed before the matrix given by
 * carrier.colorimetry is applied.  The result is then linearised through the
 * carrier's transfer characteristic and re-encoded to sRGB.  No gamut mapping
 * is performed: the source primaries (BT.470 System B/G or SMPTE 170M) are
 * taken as-is, which for SD primaries is a small error next to the transfer
 * difference.
 */
PreviewImage render_preview_from_colour_carrier(
    const ColourFrameCarrier& carrier);

/**
 * @brief Convert a colour-domain carrier into an unconverted plane payload.
 *
 * The same frame render_preview_from_colour_carrier() would have produced,
 * stopped one step earlier: the component planes are narrowed to float and
 * handed over with every constant the conversion would have applied, so a
 * consumer with a fragment shader can finish the job.  The conversion itself
 * is a pass over four million samples, which is why moving it is worth a
 * payload of its own.
 *
 * Returns a payload with domain None when the carrier is invalid.
 */
PreviewPlanes preview_planes_from_colour_carrier(
    const ColourFrameCarrier& carrier);

/**
 * @brief The transfer decode and sRGB encode, composed and tabulated.
 *
 * The table render_preview_from_colour_carrier() interpolates between, shared
 * so that a consumer converting the planes elsewhere applies the same curve
 * sampled at the same nodes rather than its own evaluation of it.  Entry i is
 * the curve at i / (size() - 1); consumers interpolate linearly between
 * neighbours, as the CPU conversion does.
 *
 * The returned table is immutable and cached per characteristic, so repeated
 * calls hand back the same object.
 */
std::shared_ptr<const std::vector<float>> preview_transfer_lut(
    ColorimetricTransferCharacteristics transfer);

/**
 * @brief Re-order an interlaced (weaved) plane payload into sequential fields.
 *
 * The row permutation reorder_preview_image_to_sequential_fields() applies,
 * applied to the three component planes instead.  Dropout regions are remapped
 * to their new display rows.
 */
void reorder_preview_planes_to_sequential_fields(PreviewPlanes& planes);

/**
 * @brief Re-order an interlaced (weaved) preview image into sequential fields.
 *
 * The weaved image has field 1 on even display rows (0-based) and field 2 on
 * odd rows.  This rearranges the rows in place so the field-1 lines form the
 * top block and the field-2 lines the bottom block, matching the
 * "sequential" layout offered by the signal-domain preview helpers.  Any
 * dropout regions are remapped to their new display rows.  Image dimensions
 * are unchanged; invalid images are left untouched.
 */
void reorder_preview_image_to_sequential_fields(PreviewImage& image);

}  // namespace orc
