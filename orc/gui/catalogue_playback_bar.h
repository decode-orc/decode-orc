/*
 * File:        catalogue_playback_bar.h
 * Module:      orc-gui
 * Purpose:     Transport controls for a timed display list: play and pause,
 *              step, seek, speed and loop
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef CATALOGUE_PLAYBACK_BAR_H
#define CATALOGUE_PLAYBACK_BAR_H

#include <QString>
#include <QTimer>
#include <QWidget>
#include <cstdint>

class CatalogueDisplayListWidget;
class QCheckBox;
class QComboBox;
class QKeyEvent;
class QLabel;
class QSlider;
class QToolButton;

/**
 * @brief The controls a reader plays a timed display list with, laid out
 *        as a bar to go under the view
 *
 * Play or pause; the display before and the next one; Replay from the start;
 * a slider over the whole presentation, dragged to seek; where the
 * presentation stands and how long it is, as minutes, seconds and hundredths;
 * the speed, from a quarter to four times as timed; and whether it loops.
 * Every control acts on the view it is given, and the bar follows the view:
 * it brings itself into line whenever the view's transport changes, and
 * while the presentation plays it moves the slider and the clock on at a
 * steady rate of its own, stopping as soon as the presentation does. The
 * transport keys the view takes (CatalogueDisplayListWidget::
 * handlePlaybackKey()) work on the bar as well.
 *
 * Not thread-safe: a widget, used from the GUI thread only.
 */
class CataloguePlaybackBar : public QWidget {
  Q_OBJECT

 public:
  /// Controls for @p display, which must outlive the bar.
  explicit CataloguePlaybackBar(CatalogueDisplayListWidget* display,
                                QWidget* parent = nullptr);

  /// Bring every control into line with the view: play or pause, the
  /// slider's range and position, the clock. What the bar does at each tick
  /// while the presentation plays (test seam).
  void refresh();

  /// Whether the bar is ticking to follow a presentation that plays (test
  /// seam).
  bool isFollowing() const { return follow_timer_.isActive(); }

  /// A presentation time as the clock shows it: minutes, then seconds to the
  /// hundredth, as "1:04.25".
  static QString clockText(uint32_t time_ms);

  /// The speeds offered, slowest first.
  static constexpr double kSpeeds[] = {0.25, 0.5, 1.0, 2.0, 4.0};
  /// How often the bar moves the slider and the clock on while the
  /// presentation plays: about thirty times a second, smooth to the eye and
  /// finer than the hundredths the clock shows are read at.
  static constexpr int kFollowIntervalMs = 33;

 protected:
  void keyPressEvent(QKeyEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

 private:
  /// Tick while the presentation plays and the bar is on screen; stop
  /// otherwise.
  void follow();
  void onSliderMoved(int value);

  CatalogueDisplayListWidget* display_ = nullptr;
  QToolButton* play_button_ = nullptr;
  QToolButton* back_button_ = nullptr;
  QToolButton* forward_button_ = nullptr;
  QToolButton* replay_button_ = nullptr;
  QSlider* slider_ = nullptr;
  QLabel* clock_label_ = nullptr;
  QComboBox* speed_combo_ = nullptr;
  QCheckBox* loop_check_ = nullptr;
  QTimer follow_timer_;
  /// Between a show event and a hide event. Kept from the events rather than
  /// read from isVisible(), which stays true through the spontaneous hide a
  /// minimised window sends.
  bool shown_ = false;
  /// Set while the bar moves its own controls, so that doing so does not read
  /// as the reader moving them.
  bool updating_ = false;
};

#endif  // CATALOGUE_PLAYBACK_BAR_H
