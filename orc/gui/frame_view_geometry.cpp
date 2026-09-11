/*
 * File:        frame_view_geometry.cpp
 * Module:      orc-gui
 * Purpose:     Shared display geometry for frame preview/editor viewports
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "frame_view_geometry.h"

#include <QtGlobal>
#include <algorithm>
#include <cmath>

namespace orc::gui {

void FrameViewGeometry::setImageSize(const QSize& size) {
  image_size_ = size.isValid() ? size : QSize();
  clampVisibleOrigin();
}

void FrameViewGeometry::setAspectCorrection(double correction) {
  aspect_correction_ = (correction > 0.0) ? correction : 1.0;
  clampVisibleOrigin();
}

void FrameViewGeometry::setZoom(double zoom) {
  zoom_ = (zoom > 0.0) ? zoom : 1.0;
  clampVisibleOrigin();
}

void FrameViewGeometry::setViewportSize(const QSize& size) {
  viewport_size_ = size.isValid() ? size : QSize();
  clampVisibleOrigin();
}

void FrameViewGeometry::setVisibleOrigin(const QPoint& origin) {
  visible_origin_ = origin;
  clampVisibleOrigin();
}

QSize FrameViewGeometry::maxVisibleOrigin() const {
  if (!hasImage()) {
    return QSize(0, 0);
  }
  const QSize display = displaySize();
  return QSize(std::max(0, display.width() - viewport_size_.width()),
               std::max(0, display.height() - viewport_size_.height()));
}

void FrameViewGeometry::clampVisibleOrigin() {
  const QSize limit = maxVisibleOrigin();
  visible_origin_ = QPoint(qBound(0, visible_origin_.x(), limit.width()),
                           qBound(0, visible_origin_.y(), limit.height()));
}

bool FrameViewGeometry::hasImage() const {
  return image_size_.width() > 0 && image_size_.height() > 0;
}

QSize FrameViewGeometry::displaySize() const {
  if (!hasImage()) {
    return QSize();
  }
  int width =
      std::max(1, qRound(image_size_.width() * aspect_correction_ * zoom_));
  int height = std::max(1, qRound(image_size_.height() * zoom_));
  return QSize(width, height);
}

QRect FrameViewGeometry::targetRect() const {
  if (!hasImage()) {
    return QRect();
  }
  // Centred while the content fits, panned once it does not: the origin is
  // clamped to zero on an axis with room to spare, so the two rules never
  // fight.
  const QSize display = displaySize();
  const int left = std::max(0, (viewport_size_.width() - display.width()) / 2) -
                   visible_origin_.x();
  const int top =
      std::max(0, (viewport_size_.height() - display.height()) / 2) -
      visible_origin_.y();
  return QRect(left, top, display.width(), display.height());
}

double FrameViewGeometry::fitZoom() const {
  if (!hasImage() || viewport_size_.width() <= 0 ||
      viewport_size_.height() <= 0) {
    return 1.0;
  }
  const double corrected_width = image_size_.width() * aspect_correction_;
  const double zoom_x = viewport_size_.width() / corrected_width;
  const double zoom_y =
      static_cast<double>(viewport_size_.height()) / image_size_.height();
  return std::min(zoom_x, zoom_y);
}

QPointF FrameViewGeometry::imageFromWidget(const QPointF& widget_pos) const {
  const QRect rect = targetRect();
  if (!hasImage() || rect.width() <= 0 || rect.height() <= 0) {
    return QPointF();
  }
  const double x = (widget_pos.x() - rect.left()) * image_size_.width() /
                   static_cast<double>(rect.width());
  const double y = (widget_pos.y() - rect.top()) * image_size_.height() /
                   static_cast<double>(rect.height());
  return QPointF(x, y);
}

QPointF FrameViewGeometry::widgetFromImage(const QPointF& image_pos) const {
  const QRect rect = targetRect();
  if (!hasImage() || rect.width() <= 0 || rect.height() <= 0) {
    return QPointF();
  }
  const double x = rect.left() + image_pos.x() * rect.width() /
                                     static_cast<double>(image_size_.width());
  const double y = rect.top() + image_pos.y() * rect.height() /
                                    static_cast<double>(image_size_.height());
  return QPointF(x, y);
}

QPoint FrameViewGeometry::imagePixelFromWidget(const QPoint& widget_pos) const {
  if (!hasImage()) {
    return QPoint();
  }
  const QPointF image_pos = imageFromWidget(QPointF(widget_pos));
  const int x = qBound(0, static_cast<int>(std::floor(image_pos.x())),
                       image_size_.width() - 1);
  const int y = qBound(0, static_cast<int>(std::floor(image_pos.y())),
                       image_size_.height() - 1);
  return QPoint(x, y);
}

bool FrameViewGeometry::widgetPointOnImage(const QPoint& widget_pos) const {
  return hasImage() && targetRect().contains(widget_pos);
}

QPoint FrameViewGeometry::scrollAfterZoom(const QPoint& old_scroll,
                                          const QPoint& viewport_pos,
                                          double zoom_ratio) {
  const double content_x = old_scroll.x() + viewport_pos.x();
  const double content_y = old_scroll.y() + viewport_pos.y();
  return QPoint(qRound(content_x * zoom_ratio - viewport_pos.x()),
                qRound(content_y * zoom_ratio - viewport_pos.y()));
}

}  // namespace orc::gui
