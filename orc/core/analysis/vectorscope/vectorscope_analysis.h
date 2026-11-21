/*
 * File:        vectorscope_analysis.h
 * Module:      orc-core
 * Purpose:     Vectorscope samples from a colour preview carrier
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef ORC_CORE_ANALYSIS_VECTORSCOPE_ANALYSIS_H
#define ORC_CORE_ANALYSIS_VECTORSCOPE_ANALYSIS_H

#include <orc/stage/frame_id.h>
#include <orc/stage/orc_source_parameters.h>
#include <orc/stage/preview/orc_preview_carriers.h>

#include <cstdint>

#include "vectorscope_data.h"

namespace orc {

/**
 * @brief Extract vectorscope data from a colour preview carrier.
 *
 * Uses the decoded U/V planes already present in the carrier.
 *
 * |active_area_only| limits sampling to the carrier's active picture window
 * in both axes; cleared, the whole decoded frame is plotted.
 *
 * |first_line| / |last_line| are an inclusive interlaced frame-line range
 * (0-based), the same numbering the composite acquisition selects and
 * reports lines in, so a range means the same thing in either scope.
 * |last_line| == 0 means "to the last line of the frame"; the range
 * intersects with the active-picture restriction rather than overriding it.
 */
VectorscopeData extract_vectorscope_from_colour_frame(
    const ColourFrameCarrier& carrier, uint64_t field_number,
    uint32_t subsample = 1, bool active_area_only = true,
    uint32_t first_line = 0, uint32_t last_line = 0);

}  // namespace orc

#endif  // ORC_CORE_ANALYSIS_VECTORSCOPE_ANALYSIS_H
