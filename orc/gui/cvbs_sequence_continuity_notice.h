/*
 * File:        cvbs_sequence_continuity_notice.h
 * Module:      orc-gui
 * Purpose:     Pure helper that explains a CVBS source's sequence_continuous
 *              marker to the user when the content is not continuous
 *              (Tier 1 / gui-logic testable)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <optional>
#include <string>

namespace orc::gui {

// Note shown when a CVBS source is opened whose .meta declares
// sequence_continuous = FALSE (CVBS file format spec v1.6.0). Empty when the
// content is continuous or continuity is unknown (NULL), and for a signal
// state the source stage would refuse outright — those get an error from the
// stage, not an advisory here.
//
// Nothing is wrong with the file: decode-orc measures colour-sequence phase
// from the burst of each frame rather than trusting the sidecar, so a
// discontinuous source decodes normally. What the marker says is that the
// stored content is not one unbroken sequence, which on a LaserDisc means the
// player skipped or jumped during the decode. The Disc Mapper puts the frames
// back into their recorded order, which is what restores the sequence, so it
// is worth running before exporting.
inline std::string cvbsSequenceContinuityNotice(
    const std::optional<bool>& sequence_continuous) {
  if (!sequence_continuous.has_value() || *sequence_continuous) {
    return {};
  }
  return "This CVBS source is marked sequence_continuous = FALSE, meaning "
         "the stored content contains at least one discontinuity. That "
         "normally means the player skipped or jumped while the disc was "
         "being decoded.\n\n"
         "The source will load and decode normally: decode-orc measures the "
         "colour phase of every frame from its burst rather than relying on "
         "this marker.\n\n"
         "Before exporting, it is worth running the Disc Mapper to put the "
         "frames back into their recorded order: add a Frame Map stage, then "
         "right-click it and choose Stage Tools > Disc Mapper. A CVBS file "
         "exported from decode-orc records the continuity actually measured "
         "in the frames it writes.";
}

}  // namespace orc::gui
