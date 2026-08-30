/*
 * File:        cvbs_signal_state_notice.h
 * Module:      orc-gui
 * Purpose:     Pure helper that explains a CVBS source's signal state to the
 *              user when it is not burst-locked (Tier 1 / gui-logic testable)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <string>

namespace orc::gui {

// Note shown when a CVBS source is opened whose .meta declares a signal state
// that is not burst-locked. Empty for a locked source, and for a state the
// source stage would refuse outright — those get an error from the stage, not
// an advisory here.
//
// Nothing is wrong with the file: decode-orc measures colour-sequence phase
// from the burst of each frame rather than trusting the sidecar, so an
// unlocked source decodes normally. What the marker says is that the phase
// sequence is not continuous end to end, which on a LaserDisc means the player
// skipped or jumped during the decode. The Disc Mapper puts the frames back
// into their recorded order, which is what restores the sequence, so it is
// worth running before exporting.
inline std::string cvbsSignalStateNotice(const std::string& signal_state) {
  if (signal_state != "STANDARD_TBC_UNLOCKED") {
    return {};
  }
  return "This CVBS source is marked STANDARD_TBC_UNLOCKED, meaning its "
         "colour phase sequence is not continuous from start to end. That "
         "normally means the player skipped or jumped while the disc was "
         "being decoded.\n\n"
         "The source will load and decode normally: decode-orc measures the "
         "colour phase of every frame from its burst rather than relying on "
         "this marker.\n\n"
         "Before exporting, it is worth running the Disc Mapper to put the "
         "frames back into their recorded order: add a Frame Map stage, then "
         "right-click it and choose Stage Tools > Disc Mapper. A CVBS file "
         "exported from decode-orc is marked locked or unlocked according to "
         "the phase sequence actually measured in the frames it writes.";
}

}  // namespace orc::gui
