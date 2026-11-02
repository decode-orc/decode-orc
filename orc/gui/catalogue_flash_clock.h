/*
 * File:        catalogue_flash_clock.h
 * Module:      orc-gui
 * Purpose:     Shared flash phase for the catalogue character-grid view
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef CATALOGUE_FLASH_CLOCK_H
#define CATALOGUE_FLASH_CLOCK_H

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

/**
 * @brief The flash phase the catalogue's character-grid view runs on
 *
 * Teletext flashing text alternates the foreground between being drawn and
 * not, on a cycle the receiver owns rather than the transmission. The clock
 * belongs to the dialogue rather than the view, so a viewer stepping between
 * pages does not restart the cycle under the reader. A display list does not
 * use it: it carries its own timing (CatalogueDisplayList::palette_tracks) and
 * is played against that instead.
 *
 * Subscribers are counted: the timer runs only while at least one view is
 * visible and actually holds something that flashes. A catalogue browser left
 * open on a still page costs nothing.
 *
 * The phase is derived from a monotonic clock rather than counted off the
 * timer. Timer resolution and coalescing differ on every platform — Windows'
 * default tick, macOS timer coalescing, compositor throttling on Wayland — and
 * a phase toggled per timeout drifts under all of them.
 *
 * Not thread-safe: a Qt widget clock, used from the GUI thread only.
 */
class CatalogueFlashClock : public QObject {
  Q_OBJECT

 public:
  explicit CatalogueFlashClock(QObject* parent = nullptr);

  /// The phase to paint: true is the lit one, where the character is shown.
  bool lit() const { return lit_; }

  /// Take a reference on the clock, starting it if it was idle.
  void acquire();
  /// Drop a reference, stopping the clock when the last one goes.
  void release();

  /// Whether the cycle is running (test seam)
  bool running() const { return timer_.isActive(); }
  /// How many views are subscribed (test seam)
  int subscribers() const { return subscribers_; }

 signals:
  void litChanged(bool lit);

 private:
  /// Wake at the next phase boundary rather than on a fixed tick
  void arm();
  void onTimeout();
  void setLit(bool lit);

  QTimer timer_;
  QElapsedTimer cycle_;
  int subscribers_ = 0;
  bool lit_ = true;
};

#endif  // CATALOGUE_FLASH_CLOCK_H
