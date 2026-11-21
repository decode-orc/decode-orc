/*
 * File:        frame_marker_slider.cpp
 * Module:      orc-view_core
 * Purpose:     Slider with tick markers for values of interest
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "frame_marker_slider.h"

#include <QPainter>
#include <QStyle>
#include <QStyleOptionSlider>

FrameMarkerSlider::FrameMarkerSlider(Qt::Orientation orientation,
                                     QWidget* parent)
    : QSlider(orientation, parent) {}

void FrameMarkerSlider::setMarkedValues(std::vector<int> values) {
  if (values == marked_values_) {
    return;
  }
  marked_values_ = std::move(values);
  update();
}

void FrameMarkerSlider::paintEvent(QPaintEvent* event) {
  QSlider::paintEvent(event);

  if (marked_values_.empty() || maximum() <= minimum() ||
      orientation() != Qt::Horizontal) {
    return;
  }

  QStyleOptionSlider option;
  initStyleOption(&option);
  const QRect groove = style()->subControlRect(QStyle::CC_Slider, &option,
                                               QStyle::SC_SliderGroove, this);
  const QRect handle = style()->subControlRect(QStyle::CC_Slider, &option,
                                               QStyle::SC_SliderHandle, this);

  // Marker x positions use the same value-to-pixel mapping as the handle.
  const int span = groove.width() - handle.width();
  if (span <= 0) {
    return;
  }

  QPainter painter(this);
  QPen pen(QColor(230, 126, 34));  // Orange accent, visible on light and dark
  pen.setWidth(2);
  painter.setPen(pen);

  for (int value : marked_values_) {
    if (value < minimum() || value > maximum()) {
      continue;
    }
    const int pos = QStyle::sliderPositionFromValue(minimum(), maximum(), value,
                                                    span, option.upsideDown);
    const int x = groove.x() + handle.width() / 2 + pos;
    painter.drawLine(x, groove.top(), x, groove.bottom());
  }
}
