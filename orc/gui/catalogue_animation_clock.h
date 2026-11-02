/*
 * File:        catalogue_animation_clock.h
 * Module:      orc-gui
 * Purpose:     Time source a catalogue view plays a timed payload against
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef CATALOGUE_ANIMATION_CLOCK_H
#define CATALOGUE_ANIMATION_CLOCK_H

#include <QElapsedTimer>
#include <cstdint>

/**
 * @brief A monotonic millisecond reading to play a presentation against
 *
 * Only the difference between two readings means anything. A view reads it
 * whenever it needs to know where a presentation has got to rather than
 * counting timer ticks, so timer resolution and coalescing cannot make it
 * drift; a test hands the view a clock it moves by hand.
 *
 * Views read it from the GUI thread only, so an implementation need not be
 * thread-safe.
 */
class ICatalogueAnimationClock {
 public:
  virtual ~ICatalogueAnimationClock() = default;

  /// Milliseconds on a clock that never runs backwards.
  virtual int64_t nowMs() const = 0;
};

/**
 * @brief The clock a view uses unless it is handed another: milliseconds
 *        since construction, from the platform's monotonic clock
 *
 * Not thread-safe: read from the GUI thread only.
 */
class CatalogueMonotonicClock final : public ICatalogueAnimationClock {
 public:
  CatalogueMonotonicClock();

  int64_t nowMs() const override;

 private:
  QElapsedTimer elapsed_;
};

#endif  // CATALOGUE_ANIMATION_CLOCK_H
