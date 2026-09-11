/*
 * File:        frame_view_geometry.h
 * Module:      orc-gui
 * Purpose:     Shared display geometry for frame preview/editor viewports
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef FRAME_VIEW_GEOMETRY_H
#define FRAME_VIEW_GEOMETRY_H

#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>

namespace orc::gui {

/**
 * @brief Pure display-geometry model shared by frame-viewing widgets
 *
 * Models the mapping between a source image (frame pixels: samples x lines)
 * and its on-screen presentation, combining:
 * - Aspect-ratio correction (width scale factor; 1.0 = square samples)
 * - Zoom level (1.0 = one image pixel per widget pixel, pre-aspect)
 * - Letterbox centering within a viewport
 *
 * Used by FieldPreviewWidget (fit-to-widget mode) and FrameViewportWidget
 * (scroll-area zoom mode) so both present frames identically and share one
 * implementation of widget<->image coordinate mapping.
 *
 * Pure logic (QtCore types only, no QWidget) so it is unit-testable without
 * a QApplication.
 *
 * Thread safety: not thread-safe; use from a single thread.
 */
class FrameViewGeometry {
 public:
  FrameViewGeometry() = default;

  /// Set the source image size in image pixels (samples x lines).
  void setImageSize(const QSize& size);
  QSize imageSize() const { return image_size_; }

  /// Set the width scale factor (1.0 = SAR 1:1; e.g. ~0.7 for DAR 4:3).
  void setAspectCorrection(double correction);
  double aspectCorrection() const { return aspect_correction_; }

  /// Set the zoom level (1.0 = 100%; display width also scales by aspect).
  void setZoom(double zoom);
  double zoom() const { return zoom_; }

  /// Set the viewport (widget) size the display rect is centered within.
  void setViewportSize(const QSize& size);

  /**
   * @brief Set the top-left of the visible window into the displayed image.
   *
   * In widget pixels, clamped to maxVisibleOrigin(). This is what panning
   * moves: a viewport widget stays its own size and shifts what it shows,
   * rather than growing to the zoomed image and letting a scroll area move
   * it, which a GPU surface cannot do - there is no 8000-pixel-wide texture
   * to scroll across.
   */
  void setVisibleOrigin(const QPoint& origin);
  QPoint visibleOrigin() const { return visible_origin_; }

  /// Largest origin that keeps the content in view: the displayed size less
  /// the viewport, and zero on whichever axes already fit.
  QSize maxVisibleOrigin() const;

  /// True when a non-empty image size has been set.
  bool hasImage() const;

  /// Size of the displayed image after aspect correction and zoom.
  QSize displaySize() const;

  /// Display rect centered (letterboxed) within the viewport.
  QRect targetRect() const;

  /// Zoom level that fits the aspect-corrected image inside the viewport.
  double fitZoom() const;

  /// Map a widget-space position to image-space (unclamped, fractional).
  QPointF imageFromWidget(const QPointF& widget_pos) const;

  /// Map an image-space position to widget-space (fractional).
  QPointF widgetFromImage(const QPointF& image_pos) const;

  /// Map a widget-space position to the containing image pixel, clamped to
  /// the image bounds.
  QPoint imagePixelFromWidget(const QPoint& widget_pos) const;

  /// True when the widget-space point lies on the displayed image.
  bool widgetPointOnImage(const QPoint& widget_pos) const;

  /**
   * @brief Scroll offsets that keep the content point under the cursor
   * stationary across a zoom change.
   *
   * @param old_scroll Pan origin (or scrollbar values) before the zoom change
   * @param viewport_pos Cursor position within the scroll viewport
   * @param zoom_ratio new_zoom / old_zoom
   * @return New scrollbar values
   */
  static QPoint scrollAfterZoom(const QPoint& old_scroll,
                                const QPoint& viewport_pos, double zoom_ratio);

 private:
  /// Re-apply the origin's clamp after a change to the pannable range.
  void clampVisibleOrigin();

  QSize image_size_;
  QSize viewport_size_;
  QPoint visible_origin_;
  double aspect_correction_ = 1.0;
  double zoom_ = 1.0;
};

}  // namespace orc::gui

#endif  // FRAME_VIEW_GEOMETRY_H
