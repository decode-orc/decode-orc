/*
 * File:        frame_viewport_widget.h
 * Module:      decode-orc Plugin SDK (view-support tier)
 * Purpose:     Zoomable, pannable, aspect-corrected frame viewport for
 *              tool windows
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * STABILITY: PUBLIC — This header is part of the stable plugin SDK.
 */

#pragma once

// SDK TIER: view-support — see <orc/view-support/view_support_export.h>.

#include <orc/view-support/frame_surface.h>
#include <orc/view-support/frame_view_geometry.h>
#include <orc/view-support/overlay_primitives.h>
#include <orc/view-support/view_support_export.h>

#include <QImage>
#include <QPoint>
#include <QWidget>
#include <memory>

namespace orc::view {

/**
 * @brief Zoomable, aspect-corrected frame viewport
 *
 * Displays a rendered frame image with:
 * - Zoom (buttons/API plus Ctrl+wheel zoom-at-cursor)
 * - Aspect-ratio correction (width scale, matching the preview dialog)
 * - Fit-to-viewport
 * - Panning, via a visible origin the owner drives from scroll bars
 * - Widget<->image coordinate mapping via FrameViewGeometry
 *
 * The widget stays its own size and moves what it shows, rather than growing
 * to the zoomed image inside a scroll area: at 8x zoom that widget is several
 * thousand pixels across, which a GPU surface cannot be. The owner supplies
 * scroll bars and keeps them in step with panRangeChanged() and
 * visibleOriginChanged().
 *
 * Subclasses draw interactive overlays by overriding buildOverlay(), which
 * contributes primitives in widget coordinates - so they stay crisp at any
 * zoom, and the raster and GPU paths draw exactly the same shapes. In orc-gui
 * the frame is drawn on the GPU when the host's surface provider allows it
 * (frame_surface.h), and with QPainter otherwise.
 *
 * Thread safety: GUI thread only.
 */
class ORC_VIEW_SUPPORT_EXPORT FrameViewportWidget : public QWidget {
  Q_OBJECT

 public:
  explicit FrameViewportWidget(QWidget* parent = nullptr);
  ~FrameViewportWidget() override;

  /// Set the frame image to display (null image clears the display).
  void setImage(const QImage& image);
  void clearImage();
  bool hasImage() const { return !image_.isNull(); }
  QSize imageSize() const { return image_.size(); }

  /// Width scale factor for display (1.0 = SAR 1:1; ~0.7 for DAR 4:3).
  void setAspectCorrection(double correction);
  double aspectCorrection() const { return geometry_.aspectCorrection(); }

  /// Zoom control. Levels are clamped to the configured range.
  void setZoomLevel(double zoom);
  double zoomLevel() const { return geometry_.zoom(); }
  void setZoomRange(double min_zoom, double max_zoom);
  void zoomIn();
  void zoomOut();

  /// Fit the image inside the widget.
  void fitToViewport();

  /// @name Panning
  /// @{
  /// Top-left of the visible window into the zoomed image, in widget pixels.
  void setVisibleOrigin(const QPoint& origin);
  QPoint visibleOrigin() const { return geometry_.visibleOrigin(); }
  /// Largest origin that keeps content in view; the scroll bar ranges.
  QSize maxVisibleOrigin() const { return geometry_.maxVisibleOrigin(); }
  /// @}

  /// @name Coordinate mapping (widget <-> image space)
  /// @{
  QPointF imageFromWidget(const QPointF& widget_pos) const {
    return geometry_.imageFromWidget(widget_pos);
  }
  QPointF widgetFromImage(const QPointF& image_pos) const {
    return geometry_.widgetFromImage(image_pos);
  }
  QPoint imagePixelFromWidget(const QPoint& widget_pos) const {
    return geometry_.imagePixelFromWidget(widget_pos);
  }
  /// @}

  QSize sizeHint() const override;

 signals:
  /// Emitted whenever the zoom level changes (API, buttons, or Ctrl+wheel).
  void zoomChanged(double zoom_level);

  /// Emitted when the pannable range changes (image, zoom or widget size).
  void panRangeChanged(QSize max_origin);

  /// Emitted when the visible origin moves, including from zoom-at-cursor.
  void visibleOriginChanged(QPoint origin);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void wheelEvent(QWheelEvent* event) override;
  void changeEvent(QEvent* event) override;

  /**
   * @brief Overlay hook for subclasses.
   *
   * Append primitives in widget coordinates; use widgetFromImage() (or the
   * builders in OverlayPrimitiveBuilder) to position them so they stay crisp
   * at any zoom level. Called whenever the overlay needs rebuilding, which
   * subclasses request with refreshOverlay().
   */
  virtual void buildOverlay(OverlayPrimitives& out) const;

  /// Rebuild the overlay and repaint. Subclasses call this where they would
  /// otherwise have called update().
  void refreshOverlay();

  /// Display geometry for subclasses needing direct access.
  const FrameViewGeometry& viewGeometry() const { return geometry_; }

 private:
  /// Refresh geometry, the surface's target and the pannable range.
  void applyGeometry();

  /// Keep a GPU surface child filling the widget.
  void applySurfaceGeometry();

  /// Swap a failed GPU surface for the raster path, hand the frame over and
  /// give the window back. Deferred by a turn of the event loop because the
  /// paint path calls it; a no-op unless a surface has actually failed.
  void downgradeSurfaceIfNeeded();

  FrameViewGeometry geometry_;
  std::unique_ptr<IFrameSurface> surface_;
  /// A deferred surface swap is already queued.
  bool downgrade_pending_ = false;
  QImage image_;
  double min_zoom_ = 0.25;
  double max_zoom_ = 8.0;
};

}  // namespace orc::view
