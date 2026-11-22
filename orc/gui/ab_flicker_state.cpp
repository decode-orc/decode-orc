/*
 * File:        ab_flicker_state.cpp
 * Module:      orc-gui
 * Purpose:     Which side of an A/B comparison is shown, and whether the
 *              view is alternating between them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "ab_flicker_state.h"

namespace orc::gui {

ABFlickerState::ABFlickerState(uint32_t interval_ms)
    : interval_ms_(interval_ms) {}

void ABFlickerState::setIntervalMs(uint32_t interval_ms) {
  interval_ms_ = interval_ms;
}

bool ABFlickerState::tick() {
  if (!isRunning()) {
    return false;
  }
  shown_ = otherSide(shown_);
  return true;
}

void ABFlickerState::hold() { held_ = true; }

void ABFlickerState::resume() { held_ = false; }

void ABFlickerState::toggleHold() { held_ = !held_; }

void ABFlickerState::show(ABSide side) {
  shown_ = side;
  held_ = true;
}

void ABFlickerState::swap() { shown_ = otherSide(shown_); }

}  // namespace orc::gui
