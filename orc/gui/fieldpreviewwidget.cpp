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

#include "frame_profiler.h"
#include "gpu/frame_surface_factory.h"
#include "gpu/overlay_primitive_builder.h"
#include "logging.h"
#include "preview_image_qt.h"

namespace {

/// Dropout bands are solid red, as they have always been.
const QColor kDropoutColor(255, 0, 0);

}  // namespace

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

  surface_ = orc::gui::gpu::createFrameSurface(this);
  applyBackgroundColor();
  applySurfaceGeometry();

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
  frame_size_ = current_image_.size();

  if (current_image_.isNull()) {
    dropout_regions_.clear();
  } else {
    // Store dropout regions for visualization
    dropout_regions_ = image.dropout_regions;
    ORC_LOG_DEBUG("FieldPreviewWidget::setImage - dropout regions count: {}",
                  dropout_regions_.size());
  }

  surface_->setFrameImage(current_image_);
  updateViewGeometry();
  refreshSurface();
}

void FieldPreviewWidget::setImage(
    const QImage& image,
    const std::vector<orc::DropoutRegion>& dropout_regions) {
  current_image_ = image;
  frame_size_ = current_image_.size();
  dropout_regions_ = current_image_.isNull() ? std::vector<orc::DropoutRegion>{}
                                             : dropout_regions;

  surface_->setFrameImage(current_image_);
  updateViewGeometry();
  refreshSurface();
}

bool FieldPreviewWidget::setPlanes(
    std::shared_ptr<const orc::PreviewPlanes> planes) {
  if (planes == nullptr) {
    return false;
  }
  const QSize size(static_cast<int>(planes->width),
                   static_cast<int>(planes->height));
  std::vector<orc::DropoutRegion> dropouts = planes->dropout_regions;

  // Asked before anything is discarded: a path that cannot take planes leaves
  // the frame on screen rather than blanking it while a converted one is
  // fetched.
  if (!surface_->setFramePlanes(std::move(planes))) {
    Q_EMIT frameNeedsConvertedImage();
    return false;
  }

  // Nothing on this side holds the frame any more - the surface converts it
  // straight into its texture - so the QImage is dropped and only the size it
  // would have had is kept.
  current_image_ = QImage();
  frame_size_ = size;
  dropout_regions_ = std::move(dropouts);

  updateViewGeometry();
  refreshSurface();
  return true;
}

void FieldPreviewWidget::clearImage() {
  current_image_ = QImage();
  frame_size_ = QSize();
  dropout_regions_.clear();
  surface_->setFrameImage(current_image_);
  updateViewGeometry();
  refreshSurface();
}

void FieldPreviewWidget::setAspectCorrection(double correction) {
  geometry_.setAspectCorrection(correction);
  updateViewGeometry();
  refreshSurface();
}

void FieldPreviewWidget::updateViewGeometry() {
  // Fit-to-widget presentation: the zoom always letterboxes the
  // aspect-corrected image inside the widget rect.
  geometry_.setImageSize(frame_size_);
  geometry_.setViewportSize(size());
  geometry_.setZoom(geometry_.fitZoom());
  image_rect_ = geometry_.targetRect();
  surface_->setTargetRect(image_rect_);
  rebuildOverlay();
}

std::optional<QPoint> FieldPreviewWidget::crosshairPixel() const {
  if (frame_size_.isEmpty()) {
    return std::nullopt;
  }
  const QSize image_size = frame_size_;
  if (locked_crosshairs_image_.has_value()) {
    // Re-clamp in case the frame dimensions changed since locking.
    return QPoint(
        qBound(0, locked_crosshairs_image_->x(), image_size.width() - 1),
        qBound(0, locked_crosshairs_image_->y(), image_size.height() - 1));
  }
  if (mouse_over_ && image_rect_.contains(mouse_pos_)) {
    return geometry_.imagePixelFromWidget(mouse_pos_);
  }
  return std::nullopt;
}

void FieldPreviewWidget::rebuildOverlay() {
  using orc::gui::gpu::OverlayPrimitiveBuilder;
  using orc::gui::gpu::OverlaySpan;

  orc::gui::gpu::OverlayPrimitives primitives;

  if (!frame_size_.isEmpty()) {
    if (show_dropouts_ && !dropout_regions_.empty()) {
      // Thickness scales with the displayed image, one to four pixels.
      const int thickness =
          OverlayPrimitiveBuilder::dropoutBandThickness(image_rect_.height());
      for (const auto& region : dropout_regions_) {
        const OverlaySpan span{static_cast<int>(region.line),
                               static_cast<int>(region.start_sample),
                               static_cast<int>(region.end_sample)};
        OverlayPrimitiveBuilder::appendDropoutBand(primitives, geometry_, span,
                                                   kDropoutColor, thickness);
      }
    }

    // The locked position is stored in image pixels and mapped here, so the
    // cross-hairs follow the preview through resizes and aspect changes.
    if (crosshairs_enabled_) {
      if (const std::optional<QPoint> pixel = crosshairPixel()) {
        OverlayPrimitiveBuilder::appendCrosshairs(primitives, geometry_, *pixel,
                                                  image_rect_, Qt::green);
      }
    }
  }

  surface_->setOverlay(std::move(primitives));
}

void FieldPreviewWidget::applyBackgroundColor() {
  surface_->setBackgroundColor(palette().color(backgroundRole()));
}

void FieldPreviewWidget::refreshSurface() {
  if (orc::gui::gpu::downgradeSurfaceIfFailed(surface_, this)) {
    // The replacement starts empty; everything the old surface held has to be
    // handed over before it can draw.
    applyBackgroundColor();
    applySurfaceGeometry();
    surface_->setFrameImage(current_image_);
    updateViewGeometry();
    // A frame that arrived as planes was converted inside the surface that
    // has just been thrown away, so there is no image to hand over and the
    // frame has to be fetched again. The next render answers with RGB,
    // because the policy that failed is the one that chooses.
    if (current_image_.isNull() && !frame_size_.isEmpty()) {
      Q_EMIT frameNeedsConvertedImage();
    }
  }
  surface_->refresh();
}

void FieldPreviewWidget::applySurfaceGeometry() {
  if (QWidget* surface_widget = surface_->widget()) {
    surface_widget->setGeometry(rect());
  }
}

void FieldPreviewWidget::setShowDropouts(bool show) {
  show_dropouts_ = show;
  ORC_LOG_DEBUG("FieldPreviewWidget::setShowDropouts: {} (regions count: {})",
                show, dropout_regions_.size());
  rebuildOverlay();
  refreshSurface();
}

void FieldPreviewWidget::setCrosshairsEnabled(bool enabled) {
  crosshairs_enabled_ = enabled;
  if (!enabled) {
    // Clear locked state when disabling
    locked_crosshairs_image_.reset();
  }
  rebuildOverlay();
  refreshSurface();
}

void FieldPreviewWidget::updateCrosshairsPosition(int image_x, int image_y) {
  if (frame_size_.isEmpty()) {
    return;
  }

  // Clamp coordinates to valid image bounds and store in image space; the
  // overlay build maps to widget coordinates at the current scale.
  QSize image_size = frame_size_;
  locked_crosshairs_image_ =
      QPoint(qBound(0, image_x, image_size.width() - 1),
             qBound(0, image_y, image_size.height() - 1));
  rebuildOverlay();
  refreshSurface();
}

void FieldPreviewWidget::changeEvent(QEvent* event) {
  QWidget::changeEvent(event);
  // Reachable before the surface exists: setBackgroundRole() in the
  // constructor propagates a palette change of its own.
  if (surface_ && event->type() == QEvent::PaletteChange) {
    applyBackgroundColor();
    refreshSurface();
  }
}

QSize FieldPreviewWidget::sizeHint() const {
  return QSize(768, 576);  // PAL-ish aspect
}

void FieldPreviewWidget::paintEvent(QPaintEvent* event) {
  if (surface_->widget() != nullptr) {
    // A GPU surface child draws the frame and its overlays itself; this
    // widget has nothing left to paint.
    return;
  }

  ORC_FRAME_STAGE(orc::gui::FrameStage::kPreviewPaint);
  QPainter painter(this);
  surface_->paint(painter, event->rect());
}

void FieldPreviewWidget::mouseMoveEvent(QMouseEvent* event) {
  mouse_pos_ = event->pos();
  mouse_over_ = true;

  // If button is pressed and we're dragging, unlock cross-hairs
  if (mouse_button_pressed_) {
    locked_crosshairs_image_.reset();
  }

  // Only the cross-hairs moved: the frame itself is untouched, which on the
  // GPU path means a redraw with no upload at all.
  rebuildOverlay();
  refreshSurface();

  // If mouse button is pressed and we're over the image, request line scope
  // update
  if (mouse_button_pressed_ && image_rect_.contains(event->pos()) &&
      !frame_size_.isEmpty()) {
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
    if (image_rect_.contains(event->pos()) && !frame_size_.isEmpty()) {
      const QPoint image_pixel = geometry_.imagePixelFromWidget(event->pos());
      locked_crosshairs_image_ = image_pixel;
      emit lineClicked(image_pixel.x(), image_pixel.y());
    }
    rebuildOverlay();
    refreshSurface();  // Redraw with locked cross-hairs
  }

  QWidget::mousePressEvent(event);
}

void FieldPreviewWidget::mouseReleaseEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    mouse_button_pressed_ = false;
    line_scope_update_timer_->stop();
    line_scope_update_pending_ = false;

    // Lock cross-hairs at final position after drag
    if (image_rect_.contains(event->pos()) && !frame_size_.isEmpty()) {
      locked_crosshairs_image_ = geometry_.imagePixelFromWidget(event->pos());
      rebuildOverlay();
      refreshSurface();
    }
  }

  QWidget::mouseReleaseEvent(event);
}

void FieldPreviewWidget::onLineScopeUpdateTimer() {
  if (!line_scope_update_pending_ || frame_size_.isEmpty()) {
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
  rebuildOverlay();
  refreshSurface();
  QWidget::leaveEvent(event);
}

void FieldPreviewWidget::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  applySurfaceGeometry();
  updateViewGeometry();
  refreshSurface();
}
