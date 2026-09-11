/*
 * File:        frame_viewport_widget.cpp
 * Module:      orc-gui
 * Purpose:     Reusable zoomable frame viewport widget for scroll areas
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "frame_viewport_widget.h"

#include <QPainter>
#include <QScrollArea>
#include <QScrollBar>
#include <QWheelEvent>
#include <algorithm>

#include "frame_profiler.h"

FrameViewportWidget::FrameViewportWidget(QWidget* parent) : QWidget(parent) {
  setMouseTracking(true);
  // paintEvent() covers every pixel of the damaged region itself, so Qt need
  // not erase to the background first. The editor repaints on every hover and
  // every drag step, at up to 8x zoom.
  setAttribute(Qt::WA_OpaquePaintEvent, true);
  setCursor(Qt::CrossCursor);
}

void FrameViewportWidget::setImage(const QImage& image) {
  image_ = image;
  geometry_.setImageSize(image_.size());
  applyGeometry();
  update();
}

void FrameViewportWidget::clearImage() { setImage(QImage()); }

void FrameViewportWidget::setAspectCorrection(double correction) {
  geometry_.setAspectCorrection(correction);
  applyGeometry();
  update();
}

void FrameViewportWidget::setZoomLevel(double zoom) {
  const double clamped = std::clamp(zoom, min_zoom_, max_zoom_);
  if (clamped == geometry_.zoom()) {
    return;
  }
  geometry_.setZoom(clamped);
  applyGeometry();
  update();
  Q_EMIT zoomChanged(clamped);
}

void FrameViewportWidget::setZoomRange(double min_zoom, double max_zoom) {
  if (min_zoom > 0.0 && max_zoom >= min_zoom) {
    min_zoom_ = min_zoom;
    max_zoom_ = max_zoom;
    setZoomLevel(geometry_.zoom());
  }
}

void FrameViewportWidget::zoomIn() { setZoomLevel(geometry_.zoom() * 1.25); }

void FrameViewportWidget::zoomOut() { setZoomLevel(geometry_.zoom() / 1.25); }

void FrameViewportWidget::fitToViewport() {
  if (!hasImage()) {
    return;
  }
  QSize available = size();
  if (QScrollArea* scroll_area = enclosingScrollArea()) {
    // Small margin so fit does not immediately produce scroll bars.
    available = scroll_area->viewport()->size() - QSize(2, 2);
  }
  orc::gui::FrameViewGeometry fit_geometry = geometry_;
  fit_geometry.setViewportSize(available);
  setZoomLevel(fit_geometry.fitZoom());
}

QSize FrameViewportWidget::sizeHint() const {
  if (hasImage()) {
    return geometry_.displaySize();
  }
  return QSize(800, 600);
}

void FrameViewportWidget::paintEvent(QPaintEvent* event) {
  ORC_FRAME_STAGE(orc::gui::FrameStage::kPreviewPaint);
  QPainter painter(this);
  // Clipped to the damaged region: at 8x zoom the widget is several thousand
  // pixels across, and a hover damages a few dozen of them.
  painter.setClipRect(event->rect());
  painter.fillRect(event->rect(), palette().color(QPalette::Base));

  if (!hasImage()) {
    return;
  }

  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  painter.drawImage(geometry_.targetRect(), image_);

  paintOverlay(painter);
}

void FrameViewportWidget::wheelEvent(QWheelEvent* event) {
  // Ctrl+wheel zooms at the cursor; plain wheel propagates to the scroll
  // area for panning.
  if (!event->modifiers().testFlag(Qt::ControlModifier) || !hasImage()) {
    event->ignore();
    return;
  }

  const double steps = event->angleDelta().y() / 120.0;
  const double old_zoom = geometry_.zoom();
  const double new_zoom =
      std::clamp(old_zoom * (1.0 + steps * 0.1), min_zoom_, max_zoom_);
  if (new_zoom == old_zoom) {
    event->accept();
    return;
  }

  if (QScrollArea* scroll_area = enclosingScrollArea()) {
    const QPoint viewport_pos = scroll_area->viewport()->mapFromGlobal(
        event->globalPosition().toPoint());
    const QPoint old_scroll(scroll_area->horizontalScrollBar()->value(),
                            scroll_area->verticalScrollBar()->value());

    setZoomLevel(new_zoom);

    const QPoint new_scroll = orc::gui::FrameViewGeometry::scrollAfterZoom(
        old_scroll, viewport_pos, new_zoom / old_zoom);
    scroll_area->horizontalScrollBar()->setValue(new_scroll.x());
    scroll_area->verticalScrollBar()->setValue(new_scroll.y());
  } else {
    setZoomLevel(new_zoom);
  }

  event->accept();
}

void FrameViewportWidget::paintOverlay(QPainter& painter) { Q_UNUSED(painter); }

void FrameViewportWidget::applyGeometry() {
  if (hasImage()) {
    const QSize display = geometry_.displaySize();
    setFixedSize(display);
    geometry_.setViewportSize(display);
  } else {
    setMinimumSize(QSize(0, 0));
    setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
    geometry_.setViewportSize(size());
  }
  updateGeometry();
}

QScrollArea* FrameViewportWidget::enclosingScrollArea() const {
  for (QWidget* ancestor = parentWidget(); ancestor;
       ancestor = ancestor->parentWidget()) {
    if (auto* scroll_area = qobject_cast<QScrollArea*>(ancestor)) {
      return scroll_area;
    }
  }
  return nullptr;
}
