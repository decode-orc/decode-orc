/*
 * File:        catalogue_flash_clock.cpp
 * Module:      orc-gui
 * Purpose:     Shared flash phase for the catalogue character-grid view
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "catalogue_flash_clock.h"

#include <algorithm>

namespace {

// World System Teletext flashes at 0.75 Hz on a 3:1 on/off ratio: the
// character is displayed for three quarters of the cycle and blanked for the
// remaining quarter.
//
// The rate is a property of the receiver, not of the signal. BBC Broadcast
// Teletext (1976) §3.1.4 puts flashing characters "under the control of a
// timing device in the receiver", and ETSI EN 300 706 §12.2 code 0/8 defines
// only what flashing does — the foreground pixels alternate between the
// foreground and background colours — without fixing a period at Level 1.
// (§12.3.4's additional flash functions do name rates, but those are the
// Level 2.5 packet X/26 modes, which a single flash attribute cannot express.)
constexpr int kFlashCycleMs = 1333;  // 0.75 Hz
constexpr int kFlashLitMs = 1000;    // 3:1 lit to blanked within the cycle

}  // namespace

CatalogueFlashClock::CatalogueFlashClock(QObject* parent) : QObject(parent) {
  timer_.setSingleShot(true);
  connect(&timer_, &QTimer::timeout, this, &CatalogueFlashClock::onTimeout);
}

void CatalogueFlashClock::acquire() {
  ++subscribers_;
  if (subscribers_ > 1) {
    return;  // already running, and the phase carries across the new view
  }
  // The first subscriber starts the cycle lit, so a page that has just been
  // selected shows its flashing text rather than opening on the blank phase.
  cycle_.start();
  setLit(true);
  arm();
}

void CatalogueFlashClock::release() {
  if (subscribers_ == 0) {
    return;
  }
  --subscribers_;
  if (subscribers_ > 0) {
    return;
  }
  timer_.stop();
  // Nothing is animating, and the still form of a flashing character is the
  // one that is shown.
  setLit(true);
}

void CatalogueFlashClock::arm() {
  const qint64 position = cycle_.elapsed() % kFlashCycleMs;
  const qint64 boundary = position < kFlashLitMs ? kFlashLitMs : kFlashCycleMs;
  timer_.start(static_cast<int>(std::max<qint64>(1, boundary - position)));
}

void CatalogueFlashClock::onTimeout() {
  const qint64 position = cycle_.elapsed() % kFlashCycleMs;
  setLit(position < kFlashLitMs);
  arm();
}

void CatalogueFlashClock::setLit(bool lit) {
  if (lit_ == lit) {
    return;
  }
  lit_ = lit;
  emit litChanged(lit_);
}
