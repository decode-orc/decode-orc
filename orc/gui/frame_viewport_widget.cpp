/*
 * File:        frame_viewport_widget.cpp
 * Module:      orc-gui
 * Purpose:     Reusable zoomable frame viewport widget
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "frame_viewport_widget.h"

#include <QPainter>
#include <QWheelEvent>
#include <algorithm>

#include "frame_profiler.h"
#include "gpu/frame_surface_factory.h"

FrameViewportWidget::FrameViewportWidget(QWidget* parent) : QWidget(parent) {
  setMouseTracking(true);
  // paintEvent() covers every pixel of the damaged region itself, so Qt need
  // not erase to the background first. The editor repaints on every hover and
  // every drag step, at up to 8x zoom.
  setAttribute(Qt::WA_OpaquePaintEvent, true);
  setCursor(Qt::CrossCursor);

  surface_ = orc::gui::gpu::createFrameSurface(this);
  surface_->setBackgroundColor(palette().color(QPalette::Base));
  applySurfaceGeometry();
}

FrameViewportWidget::~FrameViewportWidget() = default;

void FrameViewportWidget::setImage(const QImage& image) {
  image_ = image;
  geometry_.setImageSize(image_.size());
  surface_->setFrameImage(image_);
  applyGeometry();
  refreshOverlay();
}

void FrameViewportWidget::clearImage() { setImage(QImage()); }

void FrameViewportWidget::setAspectCorrection(double correction) {
  geometry_.setAspectCorrection(correction);
  applyGeometry();
  refreshOverlay();
}

void FrameViewportWidget::setZoomLevel(double zoom) {
  const double clamped = std::clamp(zoom, min_zoom_, max_zoom_);
  if (clamped == geometry_.zoom()) {
    return;
  }
  geometry_.setZoom(clamped);
  applyGeometry();
  refreshOverlay();
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
  orc::gui::FrameViewGeometry fit_geometry = geometry_;
  fit_geometry.setViewportSize(size());
  setZoomLevel(fit_geometry.fitZoom());
}

void FrameViewportWidget::setVisibleOrigin(const QPoint& origin) {
  const QPoint before = geometry_.visibleOrigin();
  geometry_.setVisibleOrigin(origin);
  if (geometry_.visibleOrigin() == before) {
    return;
  }
  surface_->setTargetRect(geometry_.targetRect());
  refreshOverlay();
  Q_EMIT visibleOriginChanged(geometry_.visibleOrigin());
}

QSize FrameViewportWidget::sizeHint() const { return QSize(800, 600); }

void FrameViewportWidget::paintEvent(QPaintEvent* event) {
  if (surface_->widget() != nullptr) {
    // A GPU surface child draws the frame and its overlays itself.
    return;
  }

  ORC_FRAME_STAGE(orc::gui::FrameStage::kPreviewPaint);
  QPainter painter(this);
  surface_->paint(painter, event->rect());
}

void FrameViewportWidget::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  applySurfaceGeometry();
  applyGeometry();
  refreshOverlay();
}

void FrameViewportWidget::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  // Reachable before the surface exists, from widget construction.
  if (surface_ && event->type() == QEvent::PaletteChange) {
    surface_->setBackgroundColor(palette().color(QPalette::Base));
    surface_->refresh();
  }
}

void FrameViewportWidget::wheelEvent(QWheelEvent* event) {
  if (!hasImage()) {
    event->ignore();
    return;
  }

  // Plain wheel pans; Ctrl+wheel zooms at the cursor. Panning is the widget's
  // own job now that it no longer grows to the zoomed image for a scroll area
  // to move around.
  if (!event->modifiers().testFlag(Qt::ControlModifier)) {
    const QPoint delta = event->pixelDelta().isNull() ? event->angleDelta() / 8
                                                      : event->pixelDelta();
    if (delta.isNull()) {
      event->ignore();
      return;
    }
    setVisibleOrigin(geometry_.visibleOrigin() - delta);
    event->accept();
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

  // Same maths as when a scroll area held the offsets: the content point
  // under the cursor is the one that must not move.
  const QPoint cursor = event->position().toPoint();
  const QPoint old_origin = geometry_.visibleOrigin();
  setZoomLevel(new_zoom);
  setVisibleOrigin(orc::gui::FrameViewGeometry::scrollAfterZoom(
      old_origin, cursor, new_zoom / old_zoom));

  event->accept();
}

void FrameViewportWidget::buildOverlay(
    orc::gui::gpu::OverlayPrimitives& out) const {
  Q_UNUSED(out);
}

void FrameViewportWidget::refreshOverlay() {
  downgradeSurfaceIfNeeded();

  orc::gui::gpu::OverlayPrimitives primitives;
  buildOverlay(primitives);
  surface_->setOverlay(std::move(primitives));
  surface_->refresh();
}

void FrameViewportWidget::applyGeometry() {
  geometry_.setViewportSize(size());
  surface_->setTargetRect(geometry_.targetRect());
  Q_EMIT panRangeChanged(geometry_.maxVisibleOrigin());
  Q_EMIT visibleOriginChanged(geometry_.visibleOrigin());
}

void FrameViewportWidget::downgradeSurfaceIfNeeded() {
  if (!orc::gui::gpu::downgradeSurfaceIfFailed(surface_, this)) {
    return;
  }
  // The replacement starts empty; everything the old surface held has to be
  // handed over before it can draw.
  surface_->setBackgroundColor(palette().color(QPalette::Base));
  surface_->setFrameImage(image_);
  surface_->setTargetRect(geometry_.targetRect());
}

void FrameViewportWidget::applySurfaceGeometry() {
  if (QWidget* surface_widget = surface_->widget()) {
    surface_widget->setGeometry(rect());
  }
}
