/*
 * File:        raster_frame_surface.cpp
 * Module:      orc-gui
 * Purpose:     QPainter implementation of the frame surface seam
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "raster_frame_surface.h"

#include <QPainter>
#include <QWidget>

namespace orc::gui::gpu {

RasterFrameSurface::RasterFrameSurface(QWidget* owner) : owner_(owner) {}

void RasterFrameSurface::setFrameImage(const QImage& image) { frame_ = image; }

bool RasterFrameSurface::setFramePlanes(
    std::shared_ptr<const orc::PreviewPlanes> planes) {
  // QPainter has no shader to finish the conversion with, and doing it here
  // would put the pass this path exists to avoid back on the GUI thread. The
  // caller is told so it can ask for a converted image instead; a render is
  // only ever asked for planes while a GPU surface is live, so this is the
  // frame that arrives in flight as one gives up.
  Q_UNUSED(planes);
  return false;
}

void RasterFrameSurface::setTargetRect(const QRect& rect) {
  target_rect_ = rect;
}

void RasterFrameSurface::setBackgroundColor(const QColor& color) {
  background_ = color;
}

void RasterFrameSurface::setOverlay(OverlayPrimitives primitives) {
  overlay_ = std::move(primitives);
}

void RasterFrameSurface::setSmoothScaling(bool smooth) { smooth_ = smooth; }

void RasterFrameSurface::refresh() {
  if (owner_ != nullptr) {
    owner_->update();
  }
}

void RasterFrameSurface::paint(QPainter& painter, const QRect& damaged) {
  // Clipping keeps the rescale of a full-size frame down to the part that
  // actually changed: these widgets repaint on every mouse move.
  painter.setClipRect(damaged);
  painter.fillRect(damaged, background_);

  if (frame_.isNull() || target_rect_.isEmpty()) {
    return;
  }

  painter.setRenderHint(QPainter::SmoothPixmapTransform, smooth_);
  painter.drawImage(target_rect_, frame_);

  paintOverlayPrimitives(painter, overlay_);
}

}  // namespace orc::gui::gpu
