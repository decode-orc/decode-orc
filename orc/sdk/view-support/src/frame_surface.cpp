/*
 * File:        frame_surface.cpp
 * Module:      orc-view-support
 * Purpose:     The raster frame surface and the host's surface provider
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/view-support/frame_surface.h>

#include <QPainter>
#include <QWidget>

namespace orc::view {

namespace {

// Set and read on the GUI thread only, like every widget that uses it.
IFrameSurfaceProvider* g_provider = nullptr;

}  // namespace

IFrameSurface::~IFrameSurface() = default;

IFrameSurfaceProvider::~IFrameSurfaceProvider() = default;

RasterFrameSurface::RasterFrameSurface(QWidget* owner) : owner_(owner) {}

RasterFrameSurface::~RasterFrameSurface() = default;

void RasterFrameSurface::setFrameImage(const QImage& image) { frame_ = image; }

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

void setFrameSurfaceProvider(IFrameSurfaceProvider* provider) {
  g_provider = provider;
}

std::unique_ptr<IFrameSurface> createFrameSurface(QWidget* owner) {
  if (g_provider != nullptr) {
    if (auto surface = g_provider->create(owner)) {
      return surface;
    }
  }
  return std::make_unique<RasterFrameSurface>(owner);
}

bool frameSurfaceHasFailed(IFrameSurface& surface) {
  return g_provider != nullptr && g_provider->hasFailed(surface);
}

bool replaceFailedFrameSurface(std::unique_ptr<IFrameSurface>& surface,
                               QWidget* owner) {
  return g_provider != nullptr && g_provider->replaceIfFailed(surface, owner);
}

}  // namespace orc::view
