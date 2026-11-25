/*
 * File:        edge_ring_track.h
 * Module:      orc-stage-plugin-edge-ring-correct
 * Purpose:     The certified windows of an edge-ring correction along a
 *              source, and their text form (the stage's ring_track)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_EDGE_RING_CORRECT_EDGE_RING_TRACK_H
#define ORC_STAGE_EDGE_RING_CORRECT_EDGE_RING_TRACK_H

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "edge_ring.h"

namespace orc::edge_ring {

// Windows in frame order, not overlapping. A frame in no window is not
// corrected.
class RingTrack {
 public:
  RingTrack() = default;

  // std::nullopt when windows are empty, overlap or are out of order, or a
  // gain is outside [0, 1], a kappa outside [0.5, 2] or a template value
  // outside [-1, 1].
  static std::optional<RingTrack> from_windows(
      std::vector<WindowCorrection> windows);

  // Text, items separated by ";": first "levels:" and the rising templates
  // (kRiseClasses x kLevelBins tables, "/" between), then the windows, each
  //   first-end:f|w:rise,fall,line,fill gains:kappa_rise,kappa_fall:fall:line
  // f = gains scaled per frame, w = fixed; each template a comma list of its
  // bins in units of 1e-5 of the step. Empty text, empty track.
  static std::optional<RingTrack> parse(const std::string& text);
  std::string to_string() const;

  bool empty() const { return windows_.empty(); }
  const std::vector<WindowCorrection>& windows() const { return windows_; }

  // The window holding |frame|, or nullptr.
  const WindowCorrection* at(int64_t frame) const;

 private:
  std::vector<WindowCorrection> windows_;
  std::shared_ptr<const LevelTables> levels_;
};

}  // namespace orc::edge_ring

#endif  // ORC_STAGE_EDGE_RING_CORRECT_EDGE_RING_TRACK_H
