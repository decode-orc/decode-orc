/*
 * File:        catalogue_playback.h
 * Module:      orc-gui
 * Purpose:     Where the presentation of a timed display list stands as a
 *              reader plays, pauses, seeks, speeds up and loops it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef CATALOGUE_PLAYBACK_H
#define CATALOGUE_PLAYBACK_H

#include <cstdint>
#include <memory>
#include <optional>

#include "catalogue_animation_clock.h"
#include "catalogue_presentation_timeline.h"

/**
 * @brief The transport a timed display list is played with
 *
 * A presentation has a length, which is what a reader seeks over:
 *   - a list that settles runs to kFinalHoldMs past its last change, so the
 *     finished display is on screen a while before anything starts again;
 *   - a list that never settles — one that blinks — runs until its cycle
 *     (CataloguePresentationTimeline::cycle()) has gone round once.
 *
 * Played past its length, a presentation that loops starts again from 0. One
 * that does not loop stops at its length if it settles, and otherwise goes
 * round its cycle again: the page held on screen with its blinks running, as
 * a receiver showed it. Either way the time stays within [0, length()].
 *
 * The time is worked out from the clock whenever it is asked for — where it
 * last stood, plus the clock's run since, times the speed — so nothing drifts
 * however late the view that reads it wakes. Every change of transport moves
 * that anchor to the moment it is made.
 *
 * Complexity: load() is O(k) in the timeline's palette steps (it reads
 * CataloguePresentationTimeline::cycle() or settledAt()); every other call is
 * O(1). Not thread-safe: read and moved from the GUI thread only, as the clock
 * is.
 */
class CataloguePlayback {
 public:
  /// How long a presentation that settles stays on screen, finished, before
  /// its end. A presentation that loops would otherwise start again the
  /// moment its last stage went up, too soon to see it there.
  static constexpr uint32_t kFinalHoldMs = 2000;

  /// Plays against @p clock; a null clock means the platform's monotonic one.
  explicit CataloguePlayback(
      std::shared_ptr<const ICatalogueAnimationClock> clock);

  /// Take up the presentation @p timeline reads, playing from its start at
  /// the speed and looping already chosen. An untimed list has a length of 0.
  void load(const CataloguePresentationTimeline& timeline);

  /// The far end of the presentation, in milliseconds from its start.
  uint32_t length() const { return length_; }
  /// Where a presentation that never settles goes round from; nullopt for
  /// one that settles.
  std::optional<uint32_t> cycleStart() const { return cycle_start_; }

  /// Where the presentation stands, in milliseconds from its start.
  uint32_t time() const;
  /// Whether it is moving: played, and not stopped at the end.
  bool playing() const;
  double speed() const { return speed_; }
  bool looping() const { return looping_; }

  /// Play on from where the presentation stands, or from the start once it
  /// has stopped at its end.
  void play();
  /// Hold the presentation where it stands.
  void pause();
  /// Move to @p time_ms, playing on from there if it was playing and held
  /// there if it was not. A time past the length is where playing on from the
  /// start would stand then: round the loop or the cycle, or at the end of a
  /// presentation that settles.
  void seek(uint32_t time_ms);
  /// Play from the start.
  void restart();
  /// Play @p speed times as fast as the service timed it: 1 is as timed.
  /// Anything not above zero is ignored.
  void setSpeed(double speed);
  /// Whether playing past the length starts again from 0.
  void setLooping(bool looping);

  /// When playing on next jumps back — to the start of the presentation or of
  /// its cycle — which is at its length. nullopt while it is not playing or
  /// where playing on stops at the end instead.
  std::optional<uint32_t> nextJumpBack() const;

  /// How long by the clock the presentation takes to play @p span_ms of
  /// itself at the speed chosen, rounded up so that a view woken that much
  /// later has reached it.
  int64_t clockMsFor(uint32_t span_ms) const;

 private:
  /// Where the presentation would stand at clock reading @p now_ms, before
  /// the length is applied.
  int64_t runAt(int64_t now_ms) const;
  uint32_t timeAt(int64_t now_ms) const;
  /// Where a presentation played on to @p run, at or past its length, stands:
  /// round the loop, round the cycle, or stopped at the end.
  uint32_t pastTheEnd(int64_t run) const;
  bool playingAt(int64_t now_ms) const;
  /// Hold the presentation at @p time_ms from clock reading @p now_ms,
  /// following the clock from there if @p playing.
  void anchor(uint32_t time_ms, int64_t now_ms, bool playing);

  std::shared_ptr<const ICatalogueAnimationClock> clock_;
  uint32_t length_ = 0;
  std::optional<uint32_t> cycle_start_;
  double speed_ = 1.0;
  bool looping_ = false;
  bool playing_ = false;
  uint32_t anchor_time_ms_ = 0;
  int64_t anchor_clock_ms_ = 0;
};

#endif  // CATALOGUE_PLAYBACK_H
