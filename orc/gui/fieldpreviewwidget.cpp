/*
 * File:        fieldpreviewwidget.cpp
 * Module:      orc-gui
 * Purpose:     Field preview widget
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "fieldpreviewwidget.h"

#include <orc/stage/preview/orc_rendering.h>  // For public API PreviewImage and DropoutRegion

#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>

#include "frame_profiler.h"
#include "logging.h"
#include "preview_image_qt.h"

FieldPreviewWidget::FieldPreviewWidget(QWidget* parent) : QWidget(parent) {
  setMinimumSize(320, 240);
  setBackgroundRole(QPalette::Base);
  // paintEvent() covers every pixel of the damaged region itself, so Qt need
  // not erase to the background first. Without this the whole widget was
  // filled and then painted over on every repaint - and a mouse move over the
  // preview is a repaint.
  setAttribute(Qt::WA_OpaquePaintEvent, true);
  setCursor(Qt::CrossCursor);
  setMouseTracking(true);  // Enable mouse tracking for cross-hairs

  // Setup line scope update throttling timer
  line_scope_update_timer_ = new QTimer(this);
  line_scope_update_timer_->setSingleShot(true);
  line_scope_update_timer_->setInterval(100);  // 100ms throttle
  connect(line_scope_update_timer_, &QTimer::timeout, this,
          &FieldPreviewWidget::onLineScopeUpdateTimer);
}

FieldPreviewWidget::~FieldPreviewWidget() {}

void FieldPreviewWidget::setImage(const orc::PreviewImage& image) {
  current_image_ =
      orc::gui::previewImageToQImage(image, std::move(current_image_));

  if (current_image_.isNull()) {
    dropout_regions_.clear();
  } else {
    // Store dropout regions for visualization
    dropout_regions_ = image.dropout_regions;
    ORC_LOG_DEBUG("FieldPreviewWidget::setImage - dropout regions count: {}",
                  dropout_regions_.size());
  }

  updateViewGeometry();
  update();
}

void FieldPreviewWidget::clearImage() {
  current_image_ = QImage();
  dropout_regions_.clear();
  updateViewGeometry();
  update();
}

void FieldPreviewWidget::setAspectCorrection(double correction) {
  geometry_.setAspectCorrection(correction);
  updateViewGeometry();
  update();
}

void FieldPreviewWidget::updateViewGeometry() {
  // Fit-to-widget presentation: the zoom always letterboxes the
  // aspect-corrected image inside the widget rect.
  geometry_.setImageSize(current_image_.size());
  geometry_.setViewportSize(size());
  geometry_.setZoom(geometry_.fitZoom());
  image_rect_ = geometry_.targetRect();
}

void FieldPreviewWidget::setShowDropouts(bool show) {
  show_dropouts_ = show;
  ORC_LOG_DEBUG("FieldPreviewWidget::setShowDropouts: {} (regions count: {})",
                show, dropout_regions_.size());
  update();
}

void FieldPreviewWidget::setCrosshairsEnabled(bool enabled) {
  crosshairs_enabled_ = enabled;
  if (!enabled) {
    // Clear locked state when disabling
    locked_crosshairs_image_.reset();
  }
  update();
}

void FieldPreviewWidget::updateCrosshairsPosition(int image_x, int image_y) {
  if (current_image_.isNull()) {
    return;
  }

  // Clamp coordinates to valid image bounds and store in image space; the
  // paint event maps to widget coordinates at the current scale.
  QSize image_size = current_image_.size();
  locked_crosshairs_image_ =
      QPoint(qBound(0, image_x, image_size.width() - 1),
             qBound(0, image_y, image_size.height() - 1));
  update();
}

QSize FieldPreviewWidget::sizeHint() const {
  return QSize(768, 576);  // PAL-ish aspect
}

void FieldPreviewWidget::paintEvent(QPaintEvent* event) {
  ORC_FRAME_STAGE(orc::gui::FrameStage::kPreviewPaint);
  QPainter painter(this);

  // Only the damaged region needs repainting, and this widget is repainted on
  // every mouse move for the crosshairs. Clipping keeps the rescale of a
  // full-size frame down to the part of it that actually changed.
  painter.setClipRect(event->rect());
  painter.fillRect(event->rect(), palette().color(backgroundRole()));

  // Core always provides a renderable image (real content or placeholder)
  // so we don't need local "No preview available" handling
  if (current_image_.isNull()) {
    return;
  }

  // Scale image to fit widget with aspect-ratio correction (shared geometry)
  const QRect dest_rect = image_rect_;
  QSize image_size = current_image_.size();

  // Use smooth transformation for better quality when scaling
  // This is especially important for chroma information to avoid aliasing
  // artifacts Modern systems should handle this fine, even during scrubbing
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  painter.drawImage(dest_rect, current_image_);

  // Draw dropout regions if enabled
  if (show_dropouts_ && !dropout_regions_.empty()) {
    painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

    // Draw each dropout region as a semi-transparent red rectangle
    for (const auto& region : dropout_regions_) {
      int line = static_cast<int>(region.line);
      int start_sample = static_cast<int>(region.start_sample);
      int end_sample = static_cast<int>(region.end_sample);

      // Validate region bounds
      if (line < 0 || line >= image_size.height() || start_sample < 0 ||
          end_sample > image_size.width() || start_sample >= end_sample) {
        continue;
      }

      // Map image coordinates to widget coordinates
      const qreal widget_x1 =
          geometry_.widgetFromImage(QPointF(start_sample, line)).x();
      const qreal widget_x2 =
          geometry_.widgetFromImage(QPointF(end_sample, line)).x();
      const qreal widget_y =
          geometry_.widgetFromImage(QPointF(start_sample, line)).y();

      // Calculate thickness - scale with image height, 1-4 pixels
      int thickness = std::max(1, std::min(4, image_rect_.height() / 200));

      // Draw solid red region centered on the scanline
      QColor color(255, 0, 0);  // Solid red
      QRectF dropout_rect(widget_x1, widget_y - thickness / 2.0,
                          widget_x2 - widget_x1, thickness);
      painter.fillRect(dropout_rect, color);
    }
  }

  // Draw cross-hairs if enabled. The locked position is stored in image
  // pixels and mapped to widget coordinates here, so the cross-hairs follow
  // the preview through resizes and aspect-ratio changes.
  std::optional<QPoint> crosshair_pixel;
  if (locked_crosshairs_image_.has_value()) {
    // Re-clamp in case the frame dimensions changed since locking.
    crosshair_pixel = QPoint(
        qBound(0, locked_crosshairs_image_->x(), image_size.width() - 1),
        qBound(0, locked_crosshairs_image_->y(), image_size.height() - 1));
  } else if (mouse_over_ && image_rect_.contains(mouse_pos_)) {
    crosshair_pixel = geometry_.imagePixelFromWidget(mouse_pos_);
  }

  if (crosshairs_enabled_ && crosshair_pixel.has_value()) {
    painter.setRenderHint(QPainter::Antialiasing, false);
    QPen pen(Qt::green);
    pen.setWidth(1);
    painter.setPen(pen);

    // Map the image pixel to widget coordinates to get pixel-aligned
    // positions - this ensures the cross-hairs align with the actual pixel
    // boundaries
    const QPointF pixel_top_left = geometry_.widgetFromImage(
        QPointF(crosshair_pixel->x(), crosshair_pixel->y()));
    const QPointF pixel_bottom_right = geometry_.widgetFromImage(
        QPointF(crosshair_pixel->x() + 1, crosshair_pixel->y() + 1));

    // Calculate center of the pixel
    const qreal center_x = (pixel_top_left.x() + pixel_bottom_right.x()) / 2.0;
    const qreal center_y = (pixel_top_left.y() + pixel_bottom_right.y()) / 2.0;

    // Draw vertical line through pixel center
    painter.drawLine(QPointF(center_x, image_rect_.top()),
                     QPointF(center_x, image_rect_.bottom()));

    // Draw horizontal line through pixel center
    painter.drawLine(QPointF(image_rect_.left(), center_y),
                     QPointF(image_rect_.right(), center_y));
  }
}

void FieldPreviewWidget::mouseMoveEvent(QMouseEvent* event) {
  mouse_pos_ = event->pos();
  mouse_over_ = true;

  // If button is pressed and we're dragging, unlock cross-hairs
  if (mouse_button_pressed_) {
    locked_crosshairs_image_.reset();
  }

  update();  // Trigger repaint to show cross-hairs at new position

  // If mouse button is pressed and we're over the image, request line scope
  // update
  if (mouse_button_pressed_ && image_rect_.contains(event->pos()) &&
      !current_image_.isNull()) {
    // Throttle updates using timer
    pending_line_scope_pos_ = event->pos();
    line_scope_update_pending_ = true;

    if (!line_scope_update_timer_->isActive()) {
      // Fire immediately for first update, then throttle
      onLineScopeUpdateTimer();
      line_scope_update_timer_->start();
    }
  }
}

void FieldPreviewWidget::mousePressEvent(QMouseEvent* event) {
  // Track mouse button state for drag detection
  if (event->button() == Qt::LeftButton) {
    mouse_button_pressed_ = true;

    // Lock cross-hairs at the clicked image pixel and emit the click when
    // over the image area
    if (image_rect_.contains(event->pos()) && !current_image_.isNull()) {
      const QPoint image_pixel = geometry_.imagePixelFromWidget(event->pos());
      locked_crosshairs_image_ = image_pixel;
      emit lineClicked(image_pixel.x(), image_pixel.y());
    }
    update();  // Redraw with locked cross-hairs
  }

  QWidget::mousePressEvent(event);
}

void FieldPreviewWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    mouse_button_pressed_ = false;
    line_scope_update_timer_->stop();
    line_scope_update_pending_ = false;

    // Lock cross-hairs at final position after drag
    if (image_rect_.contains(event->pos()) && !current_image_.isNull()) {
      locked_crosshairs_image_ = geometry_.imagePixelFromWidget(event->pos());
      update();
    }
  }

  QWidget::mouseReleaseEvent(event);
}

void FieldPreviewWidget::onLineScopeUpdateTimer() {
  if (!line_scope_update_pending_ || current_image_.isNull()) {
    return;
  }

  line_scope_update_pending_ = false;

  const QPoint image_pixel =
      geometry_.imagePixelFromWidget(pending_line_scope_pos_);
  emit lineClicked(image_pixel.x(), image_pixel.y());
}

void FieldPreviewWidget::leaveEvent(QEvent* event) {
  mouse_over_ = false;
  // Keep cross-hairs locked when mouse leaves - don't unlock them
  update();  // Trigger repaint
  QWidget::leaveEvent(event);
}

void FieldPreviewWidget::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  updateViewGeometry();
  update();
}
