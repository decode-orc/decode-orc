/*
 * File:        naplps_palette_timeline.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     How a NAPLPS page's colour map entries change over its
 *              presentation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NAPLPS_PALETTE_TIMELINE_H
#define ORC_NAPLPS_PALETTE_TIMELINE_H

#include <array>
#include <cstdint>
#include <vector>

#include "vbi-services/nabts_page.h"

namespace orc {

/// One colour a colour map entry takes from a moment on.
struct NaplpsColourStep {
  /// Milliseconds from the start of the record's presentation.
  uint32_t time_ms = 0;
  NabtsColour colour;
};

/**
 * @brief How one colour map entry changes over a presentation
 *
 * The entry holds each step's colour from its time until the next step's.
 * Where |loop_period_ms| is not zero the steps from |loop_start_ms| on repeat
 * every |loop_period_ms| for as long as the page is shown, which is what the
 * blink processes still running at the end of a record settle into; a looping
 * track has a step exactly at |loop_start_ms|.
 *
 * Empty for an entry whose colour never changes once the page is on screen,
 * which is most of them: the entry then shows its colour in the final map
 * throughout.
 */
struct NaplpsPaletteTrack {
  std::vector<NaplpsColourStep> steps;
  uint32_t loop_start_ms = 0;
  uint32_t loop_period_ms = 0;

  bool changes() const { return !steps.empty(); }
};

/// Every entry's track.
struct NaplpsPaletteTimeline {
  std::array<NaplpsPaletteTrack, kNabtsColourMapEntries> tracks;
  /// Whether a loop had to be shortened: blink processes whose periods share
  /// no small common multiple repeat together only after longer than a loop is
  /// allowed to be (kNaplpsMaxLoopMs), so the fastest of them set the loop and
  /// the rest are cut where it wraps.
  bool approximated = false;

  /// Whether any entry changes at all.
  bool changes() const {
    for (const NaplpsPaletteTrack& track : tracks) {
      if (track.changes()) {
        return true;
      }
    }
    return false;
  }
};

/// The longest loop a track is given.
constexpr uint32_t kNaplpsMaxLoopMs = 10 * 60 * 1000;

/**
 * @brief Work out how the colour map changes as a page is shown
 *
 * A discrete-event simulation of X3.110 §5.3.2.5 and §5.3.2.7 over the
 * record's timeline, starting from its initial map. At each instant the
 * record's own events come first, in the order it made them: a map write
 * loads the entry, and "the original blink-from color shall be restored ...
 * when all blink processes using that blink-from color are terminated". Then
 * the blink processes due at that instant run, "starting with the most
 * recently defined blink process and ending with the least recently defined
 * blink process", each taking "as its input the color map that resulted from
 * the previously executed blink process": the start of an ON interval copies
 * the blink-to entry's current contents into the blink-from entry, and the
 * start of an OFF interval puts the blink-from entry's own colour back.
 *
 * Once the record has finished and every process still running has started,
 * the processes blinking into one another settle into a cycle as long as the
 * least common multiple of their periods. Each such group of entries is given
 * a loop of that length — checked, by comparing the entries' colours a whole
 * cycle apart, rather than assumed — so a renderer can go on showing the page
 * for as long as it likes.
 *
 * Pure: no clock, no I/O. The same timeline always gives the same tracks.
 */
NaplpsPaletteTimeline naplps_palette_timeline(
    const NabtsPresentationTimeline& timeline);

}  // namespace orc

#endif  // ORC_NAPLPS_PALETTE_TIMELINE_H
