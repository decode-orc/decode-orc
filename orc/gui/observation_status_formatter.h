/*
 * File:        observation_status_formatter.h
 * Module:      orc-gui
 * Purpose:     Pure helper that turns a background-observation workload into a
 *              status-bar message (Tier 1 / gui-logic testable)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>
#include <string>

namespace orc::gui {

// Round a completion fraction (observed / total) to an integer percentage in
// [0, 100]. A zero total is treated as complete (100%) so an empty batch never
// reports partial progress; the caller decides whether to display it via the
// active flag. Half-values round up.
inline int roundObservationPercent(std::uint64_t frames_observed,
                                   std::uint64_t frames_total) {
  if (frames_total == 0) {
    return 100;
  }
  if (frames_observed >= frames_total) {
    return 100;
  }
  const std::uint64_t pct =
      (frames_observed * 100 + frames_total / 2) / frames_total;
  return pct > 100 ? 100 : static_cast<int>(pct);
}

// Status-bar text for a background-observation workload. Returns an empty
// string when the workload is idle (@p active == false), so the caller clears
// the message. While active the text is honest about what the batch is doing:
// "Computing observations… N%" once at least one frame has actually been
// computed (@p computing == true), and "Checking observations… N%" while the
// batch is only verifying that already-stored frames are covered. @p
// percent_complete is clamped to [0, 100].
//
// @p sweep_paused takes precedence over both: a percentage that has stopped
// moving needs a reason, and while a preview is playing the whole-node sweep is
// deliberately held back rather than stalled.
inline std::string formatObservationStatus(bool active, int percent_complete,
                                           bool computing = true,
                                           bool sweep_paused = false) {
  if (!active) {
    return {};
  }
  int pct = percent_complete;
  if (pct < 0) {
    pct = 0;
  } else if (pct > 100) {
    pct = 100;
  }
  if (sweep_paused) {
    return "Observations paused during playback\xE2\x80\xA6 " +
           std::to_string(pct) + "%";
  }
  const char* verb = computing ? "Computing" : "Checking";
  return std::string(verb) + " observations\xE2\x80\xA6 " +
         std::to_string(pct) + "%";
}

}  // namespace orc::gui
