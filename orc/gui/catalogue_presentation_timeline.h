/*
 * File:        catalogue_presentation_timeline.h
 * Module:      orc-gui
 * Purpose:     Reads the timing a catalogue display list carries: which
 *              operations are drawn and what each palette entry holds at a
 *              moment of the presentation, and when that next changes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef CATALOGUE_PRESENTATION_TIMELINE_H
#define CATALOGUE_PRESENTATION_TIMELINE_H

#include <orc/stage/tooling/catalogue_results.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

/**
 * @brief The keyframes of one display list, ready to be played
 *
 * A display list carries its own timing, already worked out by the stage that
 * produced it: each operation's CatalogueDrawOp::appear_ms, the sweep that
 * reveals an operation drawn a row at a time before then
 * (CatalogueDrawOp::reveal_duration_ms), and a CataloguePaletteTrack per
 * palette entry that changes. This class only reads them. It answers what is
 * on screen at a presentation time and when that next changes, which is what
 * a view needs to play the list while waking no more often than the list
 * changes.
 *
 * A sweep moves on once a field of the receivers' NTSC display, every
 * kRevealFieldMs, which is as often as a viewer of the day saw it move, and a
 * whole row of the list's grid at a time, as a receiver drew it. So it changes
 * the screen a few dozen times rather than continuously, and a view and an
 * exported document that read it here show the same rows at every moment.
 *
 * Times are milliseconds from the start of the presentation, as in the SDK.
 *
 * Built once per list and immutable afterwards, so it can be read from any
 * thread.
 */
class CataloguePresentationTimeline {
 public:
  /// An empty timeline: nothing is timed, and every colour is its own.
  CataloguePresentationTimeline() = default;
  explicit CataloguePresentationTimeline(const orc::CatalogueDisplayList& list);

  /// Whether anything changes as the list is shown: operations that appear
  /// after the start, or palette entries that change
  /// (CatalogueDisplayList::timed()).
  bool timed() const { return timed_; }

  /// Whether @p track repeats for as long as the list is shown. A track with a
  /// period but no step inside the repeating part has nothing to repeat, and
  /// CataloguePaletteTrack::next_change_after() treats it as holding.
  static bool loops(const orc::CataloguePaletteTrack& track);

  /// How often a sweep moves on: a field of NTSC, 1001/60 ms, rounded down
  /// to the millisecond at each field.
  static constexpr uint32_t kRevealFieldNumerator = 1001;
  static constexpr uint32_t kRevealFieldDenominator = 60;

  /// When @p op starts to appear: its appear time less the reveal before it.
  static uint32_t revealStart(const orc::CatalogueDrawOp& op) {
    return op.appear_ms - std::min(op.reveal_duration_ms, op.appear_ms);
  }

  /// Whether @p op is drawn at @p time_ms, whole or in part.
  static bool drawnAt(const orc::CatalogueDrawOp& op, uint32_t time_ms) {
    return revealStart(op) <= time_ms;
  }

  /// One position of a sweep: from @ref time_ms on, until the next, the
  /// operation it reveals is drawn on the side of unit y @ref line_y the sweep
  /// has passed (CatalogueDrawOp::reveal_from_y).
  struct RevealStep {
    uint32_t time_ms = 0;
    double line_y = 0.0;
  };

  /// Every position of the sweep revealing @p op, in order: at the reveal's
  /// start, where it has passed nothing, and at each field of the reveal
  /// before @p op's appear time, the sweep a whole number of the list's rows
  /// along. Empty for an operation that appears whole.
  std::vector<RevealStep> revealSteps(const orc::CatalogueDrawOp& op) const;

  /// Where the sweep revealing @p op stands at @p time_ms: the line of the
  /// last of revealSteps() at or before it. nullopt where @p op is not being
  /// revealed then — not yet started, already whole, or never revealed.
  std::optional<double> revealLineAt(const orc::CatalogueDrawOp& op,
                                     uint32_t time_ms) const;

  /// @p colour as shown at @p time_ms. A colour naming a palette entry that
  /// has a track follows the track; a plain value, or an entry whose track has
  /// no steps, is its own still colour.
  orc::CatalogueColour colourAt(const orc::CatalogueColour& colour,
                                uint32_t time_ms) const;

  /// The first time after @p time_ms at which anything on screen changes: the
  /// next operation to appear, the next step of a sweep or the next step of a
  /// palette track, whichever comes first. nullopt once nothing changes
  /// again.
  std::optional<uint32_t> nextChangeAfter(uint32_t time_ms) const;

  /// The time from which nothing changes any more: the last operation's
  /// appearance or the last step of a track that holds, whichever is later.
  /// nullopt for a list with a looping track, which never settles.
  std::optional<uint32_t> settledAt() const;

  /// Where a list that never settles repeats: from start_ms on, what is on
  /// screen at any time is what was on screen period_ms earlier.
  struct Cycle {
    uint32_t start_ms = 0;
    uint32_t period_ms = 0;
  };

  /// The longest cycle cycle() works out exactly. Blinks whose periods share
  /// no multiple this short are given the longest of their own periods
  /// instead, which every one of them goes round within at least once; ten
  /// minutes of a page is more than anyone plays through to watch it repeat.
  static constexpr uint32_t kLongestCycleMs = 600000;

  /// The cycle a list that never settles goes round: from the moment every
  /// stage is on screen, every held step taken and every loop started, for a
  /// period every loop's own period divides. nullopt for a list that settles.
  /// O(k) in the palette steps.
  std::optional<Cycle> cycle() const;

  /// When what is on screen at @p time_ms went up: the latest operation to
  /// appear, sweep step or palette step to be taken at or before it, round a
  /// loop as many times as it has gone, or 0 where nothing has changed since
  /// the start. O(log n) in the stages and sweep steps, and O(log k) in each
  /// track's steps.
  uint32_t changeAtOrBefore(uint32_t time_ms) const;

 private:
  /// Where the sweep revealing @p op stands @p after_ms into it, which is
  /// short of its appear time: a whole number of rows from its start, where
  /// the list names a grid.
  double sweepLine(const orc::CatalogueDrawOp& op, uint64_t after_ms) const;

  std::vector<orc::CataloguePaletteTrack> tracks_;
  /// Every distinct CatalogueDrawOp::appear_ms in the list and every step of
  /// a sweep after its start, ascending.
  std::vector<uint32_t> appear_times_;
  /// The height of a row of the list's grid in unit y, which a sweep moves
  /// on by whole numbers of; 0 for a list that names no grid.
  double row_pitch_ = 0.0;
  bool timed_ = false;
};

#endif  // CATALOGUE_PRESENTATION_TIMELINE_H
