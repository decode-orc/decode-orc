/*
 * File:        ab_flicker_state.h
 * Module:      orc-gui
 * Purpose:     Which side of an A/B comparison is shown, and whether the
 *              view is alternating between them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef AB_FLICKER_STATE_H
#define AB_FLICKER_STATE_H

#include <cstdint>

namespace orc::gui {

/// One side of an A/B comparison.
enum class ABSide { A, B };

/**
 * @brief The flicker of an A/B comparison, without the timer that drives it.
 *
 * The comparison window owns a QTimer and calls tick() on each timeout; this
 * class decides what that tick does. The flicker runs while the interval is
 * not zero (zero is the window's "Off") and the user has not held it.
 *
 * - hold() stops on the side shown; resume() starts again from it.
 * - show() picks a side and holds there, so the A and B keys stop the
 *   flicker on the side asked for.
 * - swap() is called when the window swaps which node is A and which is B.
 *   It moves the shown side too, so the picture on screen does not change,
 *   only its label.
 *
 * Thread safety: none; the window uses it on the GUI thread only.
 */
class ABFlickerState {
 public:
  /// The interval the window starts with, in milliseconds.
  static constexpr uint32_t kDefaultIntervalMs = 800;

  explicit ABFlickerState(uint32_t interval_ms = kDefaultIntervalMs);

  /// How long each side is shown, in milliseconds; 0 means the flicker is
  /// off.
  uint32_t intervalMs() const { return interval_ms_; }

  /// Change the interval. A running flicker keeps running at the new rate
  /// (the window restarts its timer); 0 stops it, and a later non-zero
  /// interval starts it again unless it is held.
  void setIntervalMs(uint32_t interval_ms);

  /// True while ticks alternate the sides.
  bool isRunning() const { return interval_ms_ != 0 && !held_; }

  /// True when the user has stopped the flicker on one side.
  bool isHeld() const { return held_; }

  /// The side on screen.
  ABSide shownSide() const { return shown_; }

  /// One timer period has passed. Shows the other side and returns true when
  /// running; does nothing and returns false otherwise.
  bool tick();

  /// Stop on the side shown.
  void hold();

  /// Start alternating again from the side shown.
  void resume();

  /// hold() when running, resume() when held. What Space does.
  void toggleHold();

  /// Show @p side and hold there.
  void show(ABSide side);

  /// A and B have been swapped; follow the picture to its new side.
  void swap();

 private:
  uint32_t interval_ms_;
  bool held_ = false;
  ABSide shown_ = ABSide::A;
};

/// The other side.
inline ABSide otherSide(ABSide side) {
  return side == ABSide::A ? ABSide::B : ABSide::A;
}

}  // namespace orc::gui

#endif  // AB_FLICKER_STATE_H
