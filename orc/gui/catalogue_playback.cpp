/*
 * File:        catalogue_playback.cpp
 * Module:      orc-gui
 * Purpose:     Where the presentation of a timed display list stands as a
 *              reader plays, pauses, seeks, speeds up and loops it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "catalogue_playback.h"

#include <algorithm>
#include <cmath>
#include <utility>

CataloguePlayback::CataloguePlayback(
    std::shared_ptr<const ICatalogueAnimationClock> clock)
    : clock_(clock ? std::move(clock)
                   : std::make_shared<CatalogueMonotonicClock>()) {
  anchor_clock_ms_ = clock_->nowMs();
}

void CataloguePlayback::load(const CataloguePresentationTimeline& timeline) {
  length_ = 0;
  cycle_start_.reset();
  if (timeline.timed()) {
    if (const auto cycle = timeline.cycle()) {
      cycle_start_ = cycle->start_ms;
      length_ = static_cast<uint32_t>(std::min<uint64_t>(
          uint64_t{cycle->start_ms} + cycle->period_ms, orc::kCatalogueNever));
    } else {
      length_ = static_cast<uint32_t>(std::min<uint64_t>(
          uint64_t{timeline.settledAt().value_or(0)} + kFinalHoldMs,
          orc::kCatalogueNever));
    }
  }
  restart();
}

int64_t CataloguePlayback::runAt(int64_t now_ms) const {
  if (!playing_) {
    return anchor_time_ms_;
  }
  // A clock only runs forwards, but one handed over by a test may be set
  // back; the presentation never is.
  const int64_t elapsed = std::max<int64_t>(0, now_ms - anchor_clock_ms_);
  return anchor_time_ms_ + static_cast<int64_t>(std::floor(
                               static_cast<double>(elapsed) * speed_));
}

uint32_t CataloguePlayback::timeAt(int64_t now_ms) const {
  if (length_ == 0) {
    return 0;
  }
  // Held, the presentation is wherever it was put, its end included: only
  // playing on past the end goes round again.
  if (!playing_) {
    return anchor_time_ms_;
  }
  const int64_t run = runAt(now_ms);
  return run < length_ ? static_cast<uint32_t>(run) : pastTheEnd(run);
}

uint32_t CataloguePlayback::pastTheEnd(int64_t run) const {
  if (looping_) {
    return static_cast<uint32_t>(run % length_);
  }
  if (cycle_start_) {
    const int64_t period = int64_t{length_} - *cycle_start_;
    return static_cast<uint32_t>(*cycle_start_ +
                                 (run - *cycle_start_) % period);
  }
  return length_;
}

bool CataloguePlayback::playingAt(int64_t now_ms) const {
  if (!playing_ || length_ == 0) {
    return false;
  }
  return looping_ || cycle_start_.has_value() || runAt(now_ms) < length_;
}

uint32_t CataloguePlayback::time() const { return timeAt(clock_->nowMs()); }

bool CataloguePlayback::playing() const { return playingAt(clock_->nowMs()); }

void CataloguePlayback::anchor(uint32_t time_ms, int64_t now_ms, bool playing) {
  anchor_time_ms_ = std::min(time_ms, length_);
  anchor_clock_ms_ = now_ms;
  playing_ = playing;
}

void CataloguePlayback::play() {
  const int64_t now = clock_->nowMs();
  const uint32_t at = timeAt(now);
  // Played again once over, a presentation starts from the beginning rather
  // than sitting at its end.
  anchor(at >= length_ ? 0 : at, now, true);
}

void CataloguePlayback::pause() {
  const int64_t now = clock_->nowMs();
  anchor(timeAt(now), now, false);
}

void CataloguePlayback::seek(uint32_t time_ms) {
  const int64_t now = clock_->nowMs();
  // A time past the end is the moment playing on from the start reaches then:
  // round the loop or the cycle, or the end of one that settles.
  anchor(time_ms <= length_ || length_ == 0 ? time_ms : pastTheEnd(time_ms),
         now, playingAt(now));
}

void CataloguePlayback::restart() { anchor(0, clock_->nowMs(), true); }

void CataloguePlayback::setSpeed(double speed) {
  if (!(speed > 0.0) || speed == speed_) {
    return;
  }
  const int64_t now = clock_->nowMs();
  anchor(timeAt(now), now, playingAt(now));
  speed_ = speed;
}

void CataloguePlayback::setLooping(bool looping) {
  if (looping == looping_) {
    return;
  }
  const int64_t now = clock_->nowMs();
  anchor(timeAt(now), now, playingAt(now));
  looping_ = looping;
}

std::optional<uint32_t> CataloguePlayback::nextJumpBack() const {
  if (!playing() || !(looping_ || cycle_start_)) {
    return std::nullopt;
  }
  return length_;
}

int64_t CataloguePlayback::clockMsFor(uint32_t span_ms) const {
  return static_cast<int64_t>(std::ceil(static_cast<double>(span_ms) / speed_));
}
