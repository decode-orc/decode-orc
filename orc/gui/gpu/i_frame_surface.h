/*
 * File:        i_frame_surface.h
 * Module:      orc-gui
 * Purpose:     Seam between frame-viewing widgets and their drawing path
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_I_FRAME_SURFACE_H
#define ORC_GUI_I_FRAME_SURFACE_H

#include <QColor>
#include <QImage>
#include <QRect>
#include <memory>

#include "overlay_primitives.h"

class QPainter;
class QWidget;

namespace orc {
struct PreviewPlanes;
}  // namespace orc

namespace orc::gui::gpu {

/**
 * @brief What a frame-viewing widget hands its drawing path.
 *
 * FieldPreviewWidget and FrameViewportWidget both do the same three things:
 * put a frame image somewhere in their rect, fill the rest with the
 * background, and draw overlays over the top. This interface is that, and
 * nothing else - the widgets keep every mouse, zoom and selection decision
 * they already own.
 *
 * Two implementations: RasterFrameSurface paints with QPainter into the
 * owner's own paintEvent, and FramePreviewSurface is a QRhiWidget child that
 * draws the same thing on the GPU. The choice is GpuSurfacePolicy's, made
 * once when the owner is constructed.
 *
 * Coordinates are the owner widget's own, in logical pixels; the GPU path
 * converts to device pixels itself.
 *
 * Thread safety: GUI thread only.
 */
class IFrameSurface {
 public:
  virtual ~IFrameSurface() = default;

  /// The frame to display. A null image clears it.
  virtual void setFrameImage(const QImage& image) = 0;

  /**
   * @brief The frame as the component planes it was decoded into.
   *
   * The conversion to display RGB is a pass over every sample, so a render
   * whose only consumer is a graphics device hands the planes over instead
   * and the device's fragment shader finishes the job. Replaces whatever
   * frame was set before, image or planes.
   *
   * Shared rather than copied: the payload is three floats per sample of the
   * frame, and it is produced on the render worker for this one consumer.
   *
   * @return False when this path cannot convert planes - which the raster
   *         path never can - so the caller knows it still owes the surface a
   *         converted image.
   */
  virtual bool setFramePlanes(
      std::shared_ptr<const orc::PreviewPlanes> planes) = 0;

  /// Where the frame is drawn, from FrameViewGeometry::targetRect().
  virtual void setTargetRect(const QRect& rect) = 0;

  /// Fill for everything the frame does not cover.
  virtual void setBackgroundColor(const QColor& color) = 0;

  /// This frame's overlays. Replaces whatever was set before.
  virtual void setOverlay(OverlayPrimitives primitives) = 0;

  /// Linear filtering when rescaling (the default, matching the smooth
  /// transform the preview has always used), or nearest for 1:1 inspection.
  virtual void setSmoothScaling(bool smooth) = 0;

  /// Ask for a repaint.
  virtual void refresh() = 0;

  /**
   * @brief The widget that draws, when the path has one of its own.
   *
   * The GPU path returns its QRhiWidget, which the owner must keep sized to
   * its own rect. The raster path returns nullptr, meaning "you paint me":
   * the owner calls paint() from its paintEvent.
   */
  virtual QWidget* widget() = 0;

  /// Draw into the owner's painter, clipped to @p damaged. Does nothing on
  /// the GPU path, which draws in its own child widget.
  virtual void paint(QPainter& painter, const QRect& damaged) = 0;
};

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_I_FRAME_SURFACE_H
