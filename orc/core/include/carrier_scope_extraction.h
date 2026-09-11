/*
 * File:        carrier_scope_extraction.h
 * Module:      orc-core
 * Purpose:     Vectorscope and histogram payloads from a colour frame carrier
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_CORE_CARRIER_SCOPE_EXTRACTION_H
#define ORC_CORE_CARRIER_SCOPE_EXTRACTION_H

#include <orc/stage/preview/orc_vectorscope.h>

#include <cstdint>
#include <optional>

#include "orc_histogram.h"

namespace orc {

struct ColourFrameCarrier;

/**
 * @brief Which part of a carrier a vectorscope acquisition samples.
 *
 * Mirrors the vectorscope fields of PreviewCoordinate, separated from it so
 * the extraction can be reached without a coordinate — the render worker holds
 * a carrier and a selection, not a request.
 */
struct PreviewScopeSelection {
  /// Restrict the acquisition to the active picture lines.
  bool active_area_only = true;
  /// Inclusive interlaced frame-line range, 0-based. A last_line of 0 means
  /// "to the last line of the frame".
  std::uint32_t first_line = 0;
  std::uint32_t last_line = 0;

  /// True when the selection is the carrier's whole active picture, which the
  /// producing stage has already extracted on the way past.
  bool isWholeActivePicture() const {
    return active_area_only && first_line == 0 && last_line == 0;
  }

  bool operator==(const PreviewScopeSelection& other) const {
    return active_area_only == other.active_area_only &&
           first_line == other.first_line && last_line == other.last_line;
  }
  bool operator!=(const PreviewScopeSelection& other) const {
    return !(*this == other);
  }
};

/**
 * @brief Vectorscope payload for @p carrier under @p selection.
 *
 * The default selection is answered from the payload the stage attached to the
 * carrier rather than walking the planes again; any narrower selection is
 * taken from the planes. System and level metadata is filled in from the
 * carrier either way, so the two paths produce interchangeable results.
 *
 * @param carrier               Decoded colour frame to sample.
 * @param selection             Portion of the carrier to plot.
 * @param fallback_field_number Field number to stamp when the carrier's own
 *                              attached payload does not supply one.
 * @return The payload, or nullopt when the carrier yields no samples.
 */
std::optional<VectorscopeData> extract_vectorscope_from_carrier(
    const ColourFrameCarrier& carrier, const PreviewScopeSelection& selection,
    std::uint64_t fallback_field_number);

/**
 * @brief Per-channel histogram of @p carrier's active picture.
 *
 * Channel normalisation follows the same convention as the vectorscope:
 *   Y   relative to the picture-black floor (cvbs_black); 0 % = black.
 *   U/V relative to full active-video swing, centred at zero.
 *   I/Q U/V rotated by 33 degrees (SMPTE 170M-2004 section 7.3), NTSC only.
 *
 * An invalid carrier yields a zeroed histogram rather than an error: the
 * caller has already decided the carrier was worth fetching.
 */
VideoHistogramData extract_histogram_from_carrier(
    const ColourFrameCarrier& carrier, std::uint64_t field_number);

}  // namespace orc

#endif  // ORC_CORE_CARRIER_SCOPE_EXTRACTION_H
