/*
 * File:        catalogue_playback_bar.cpp
 * Module:      orc-gui
 * Purpose:     Transport controls for a timed display list: play and pause,
 *              step, seek, speed and loop
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "catalogue_playback_bar.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QShowEvent>
#include <QSlider>
#include <QToolButton>
#include <algorithm>

#include "cataloguedisplaylistwidget.h"

namespace {

// How far one press of an arrow key moves the slider when it has focus: a
// tenth of a second, the unit the services time their pauses and blinks in.
constexpr int kSliderStepMs = 100;

// The glyphs the preview dialog's transport uses, so the two read alike.
const QString kPlayGlyph = QStringLiteral("▶");   // ▶
const QString kPauseGlyph = QStringLiteral("⏸");  // ⏸

}  // namespace

CataloguePlaybackBar::CataloguePlaybackBar(CatalogueDisplayListWidget* display,
                                           QWidget* parent)
    : QWidget(parent), display_(display) {
  auto* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);

  const auto make_button = [this, layout](const char* name, const QString& text,
                                          const QString& tip) {
    auto* button = new QToolButton(this);
    button->setObjectName(QString::fromLatin1(name));
    button->setText(text);
    button->setToolButtonStyle(Qt::ToolButtonTextOnly);
    button->setToolTip(tip);
    button->setAccessibleName(tip);
    layout->addWidget(button);
    return button;
  };

  play_button_ =
      make_button("cataloguePlayButton", kPlayGlyph, tr("Play (Space)"));
  connect(play_button_, &QToolButton::clicked, display_,
          &CatalogueDisplayListWidget::togglePlayback);

  // A display at a time, as a sign steps a frame at a time: to the start of
  // the one before or the next, held there to be looked at.
  back_button_ = make_button("catalogueStepBackButton", QStringLiteral("<"),
                             tr("The display before (Left)"));
  connect(back_button_, &QToolButton::clicked, this,
          [this] { display_->stepDisplay(-1); });
  forward_button_ =
      make_button("catalogueStepForwardButton", QStringLiteral(">"),
                  tr("The next display (Right)"));
  connect(forward_button_, &QToolButton::clicked, this,
          [this] { display_->stepDisplay(1); });

  replay_button_ = make_button(
      "catalogueReplayButton", tr("Replay"),
      tr("Present the display again from the start: each stage appears when "
         "the service drew it, and each blinking colour starts its timing "
         "afresh."));
  connect(replay_button_, &QToolButton::clicked, display_,
          &CatalogueDisplayListWidget::replay);

  slider_ = new QSlider(Qt::Horizontal, this);
  slider_->setObjectName("catalogueSeekSlider");
  slider_->setSingleStep(kSliderStepMs);
  slider_->setPageStep(
      static_cast<int>(CatalogueDisplayListWidget::kSeekStepMs));
  slider_->setToolTip(
      tr("Where the presentation is. Drag to seek; Shift+Left and Shift+Right "
         "seek five seconds."));
  slider_->setAccessibleName(tr("Where the presentation is"));
  connect(slider_, &QSlider::valueChanged, this,
          &CataloguePlaybackBar::onSliderMoved);
  layout->addWidget(slider_, 1);

  clock_label_ = new QLabel(this);
  clock_label_->setObjectName("catalogueClockLabel");
  clock_label_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  // As wide as the widest time it shows, so the controls beside it do not
  // shift as the digits change.
  clock_label_->setMinimumWidth(clock_label_->fontMetrics().horizontalAdvance(
      QStringLiteral("00:00.00 / 00:00.00")));
  layout->addWidget(clock_label_);

  auto* speed_label = new QLabel(tr("Speed"), this);
  layout->addWidget(speed_label);
  speed_combo_ = new QComboBox(this);
  speed_combo_->setObjectName("catalogueSpeedCombo");
  for (const double speed : kSpeeds) {
    speed_combo_->addItem(QString::number(speed) + QStringLiteral("×"), speed);
  }
  speed_combo_->setCurrentIndex(speed_combo_->findData(1.0));
  speed_combo_->setToolTip(
      tr("Play the presentation faster or slower than the service timed it."));
  speed_label->setBuddy(speed_combo_);
  connect(
      speed_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
      [this](int index) {
        if (!updating_ && index >= 0) {
          display_->setPlaybackSpeed(speed_combo_->itemData(index).toDouble());
        }
      });
  layout->addWidget(speed_combo_);

  // Off to begin with: a receiver drew a page once and held it, blinking
  // where it blinks, and that is what plays until the reader asks for more.
  loop_check_ = new QCheckBox(tr("Loop"), this);
  loop_check_->setObjectName("catalogueLoopCheck");
  loop_check_->setToolTip(
      tr("Start the presentation again from the beginning each time it ends. "
         "Clear this to play it once and hold the finished display, still "
         "blinking where it blinks."));
  connect(loop_check_, &QCheckBox::toggled, this, [this](bool checked) {
    if (!updating_) {
      display_->setLooping(checked);
    }
  });
  layout->addWidget(loop_check_);

  follow_timer_.setInterval(kFollowIntervalMs);
  connect(&follow_timer_, &QTimer::timeout, this,
          &CataloguePlaybackBar::refresh);
  connect(display_, &CatalogueDisplayListWidget::playbackChanged, this,
          &CataloguePlaybackBar::refresh);

  refresh();
}

QString CataloguePlaybackBar::clockText(uint32_t time_ms) {
  const uint32_t hundredths = time_ms / 10;
  const uint32_t minutes = hundredths / 6000;
  const uint32_t seconds = (hundredths / 100) % 60;
  return QStringLiteral("%1:%2.%3")
      .arg(minutes)
      .arg(seconds, 2, 10, QLatin1Char('0'))
      .arg(hundredths % 100, 2, 10, QLatin1Char('0'));
}

void CataloguePlaybackBar::refresh() {
  updating_ = true;
  const bool playing = display_->isPlaying();
  play_button_->setText(playing ? kPauseGlyph : kPlayGlyph);
  const QString play_tip = playing ? tr("Pause (Space)") : tr("Play (Space)");
  play_button_->setToolTip(play_tip);
  play_button_->setAccessibleName(play_tip);

  const uint32_t length = display_->presentationLength();
  const uint32_t now = std::min(display_->presentationTime(), length);
  slider_->setRange(0, static_cast<int>(length));
  // A reader dragging the slider is where the presentation is; the bar does
  // not pull it back from under the pointer.
  if (!slider_->isSliderDown()) {
    slider_->setValue(static_cast<int>(now));
  }
  slider_->setAccessibleDescription(clockText(now));
  clock_label_->setText(clockText(now) + QStringLiteral(" / ") +
                        clockText(length));

  const int speed_index = speed_combo_->findData(display_->playbackSpeed());
  if (speed_index >= 0) {
    speed_combo_->setCurrentIndex(speed_index);
  }
  loop_check_->setChecked(display_->looping());
  updating_ = false;
  follow();
}

void CataloguePlaybackBar::follow() {
  if (shown_ && display_->isPlaying()) {
    if (!follow_timer_.isActive()) {
      follow_timer_.start();
    }
    return;
  }
  follow_timer_.stop();
}

void CataloguePlaybackBar::onSliderMoved(int value) {
  if (updating_) {
    return;
  }
  display_->seek(static_cast<uint32_t>(std::max(value, 0)));
}

void CataloguePlaybackBar::keyPressEvent(QKeyEvent* event) {
  if (display_->handlePlaybackKey(*event)) {
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

void CataloguePlaybackBar::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  shown_ = true;
  refresh();
}

void CataloguePlaybackBar::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  // Nobody is watching the slider, so it is not moved on; shown again, the
  // bar reads where the presentation has got to.
  shown_ = false;
  follow_timer_.stop();
}
