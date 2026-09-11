/*
 * File:        overlay_primitive_builder.cpp
 * Module:      orc-gui
 * Purpose:     Builds frame-surface overlay primitives from image-space data
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "overlay_primitive_builder.h"

#include <algorithm>

namespace orc::gui::gpu {

namespace {

/// Side of a square grab handle, in widget pixels.
constexpr double kHandleSize = 8.0;

/// Strike-through geometry. Qt's Qt::DashLine is a 4-on, 2-off pattern in
/// units of pen width, and the strike has always been drawn with a 1.5-wide
/// pen, so these are that pattern measured in pixels.
constexpr double kStrikeThickness = 1.5;
constexpr double kStrikeDash = 4.0 * kStrikeThickness;
constexpr double kStrikeGap = 2.0 * kStrikeThickness;

}  // namespace

int OverlayPrimitiveBuilder::dropoutBandThickness(int target_height) {
  return std::max(1, std::min(4, target_height / 200));
}

bool OverlayPrimitiveBuilder::spanFitsImage(const FrameViewGeometry& geometry,
                                            const OverlaySpan& span) {
  const QSize image = geometry.imageSize();
  return span.line >= 0 && span.line < image.height() &&
         span.start_sample >= 0 && span.end_sample <= image.width() &&
         span.start_sample < span.end_sample;
}

void OverlayPrimitiveBuilder::appendDropoutBand(
    OverlayPrimitives& out, const FrameViewGeometry& geometry,
    const OverlaySpan& span, const QColor& color, int thickness) {
  if (!spanFitsImage(geometry, span)) {
    return;
  }

  const QPointF left =
      geometry.widgetFromImage(QPointF(span.start_sample, span.line));
  const QPointF right =
      geometry.widgetFromImage(QPointF(span.end_sample, span.line));

  out.quads.push_back(OverlayQuad{QRectF(left.x(), left.y() - thickness / 2.0,
                                         right.x() - left.x(), thickness),
                                  color});
}

void OverlayPrimitiveBuilder::appendCrosshairs(
    OverlayPrimitives& out, const FrameViewGeometry& geometry,
    const QPoint& image_pixel, const QRect& bounds, const QColor& color) {
  if (!geometry.hasImage() || bounds.isEmpty()) {
    return;
  }

  // The lines run through the centre of the named image pixel, found by
  // mapping the pixel's own corners: at any zoom that is where the pixel
  // actually is on screen.
  const QPointF top_left =
      geometry.widgetFromImage(QPointF(image_pixel.x(), image_pixel.y()));
  const QPointF bottom_right = geometry.widgetFromImage(
      QPointF(image_pixel.x() + 1, image_pixel.y() + 1));

  const qreal center_x = (top_left.x() + bottom_right.x()) / 2.0;
  const qreal center_y = (top_left.y() + bottom_right.y()) / 2.0;

  out.lines.push_back(OverlayLine{QPointF(center_x, bounds.top()),
                                  QPointF(center_x, bounds.bottom()), color});
  out.lines.push_back(OverlayLine{QPointF(bounds.left(), center_y),
                                  QPointF(bounds.right(), center_y), color});
}

QRectF OverlayPrimitiveBuilder::regionBandRect(
    const FrameViewGeometry& geometry, const OverlaySpan& span,
    bool emphasized) {
  if (!spanFitsImage(geometry, span)) {
    return QRectF();
  }

  // Centred on the scanline, with a constant on-screen thickness regardless
  // of zoom so the bands stay crisp, visible and clickable.
  const QPointF left =
      geometry.widgetFromImage(QPointF(span.start_sample, span.line + 0.5));
  const QPointF right =
      geometry.widgetFromImage(QPointF(span.end_sample, span.line + 0.5));
  const double thickness =
      std::max(4.0, geometry.zoom()) + (emphasized ? 3.0 : 0.0);
  return QRectF(left.x(), left.y() - thickness / 2.0, right.x() - left.x(),
                thickness);
}

QRectF OverlayPrimitiveBuilder::leftHandleRect(const QRectF& band) {
  return QRectF(band.left() - kHandleSize / 2.0,
                band.center().y() - kHandleSize / 2.0, kHandleSize,
                kHandleSize);
}

QRectF OverlayPrimitiveBuilder::rightHandleRect(const QRectF& band) {
  return QRectF(band.right() - kHandleSize / 2.0,
                band.center().y() - kHandleSize / 2.0, kHandleSize,
                kHandleSize);
}

void OverlayPrimitiveBuilder::appendRegionBand(
    OverlayPrimitives& out, const FrameViewGeometry& geometry,
    const OverlaySpan& span, const QColor& color, bool emphasized,
    bool struck) {
  const QRectF band = regionBandRect(geometry, span, emphasized);
  if (band.isEmpty()) {
    return;
  }

  QColor fill = color;
  fill.setAlpha(emphasized ? 220 : 150);
  out.quads.push_back(OverlayQuad{band, fill});

  if (struck) {
    // Dashes are expanded here rather than left to a pen style: a GPU
    // pipeline has no dash pattern, and expanding once means both paths draw
    // the same segments.
    const double y = band.center().y() - kStrikeThickness / 2.0;
    for (double x = band.left(); x < band.right();
         x += kStrikeDash + kStrikeGap) {
      const double width = std::min(kStrikeDash, band.right() - x);
      out.quads.push_back(
          OverlayQuad{QRectF(x, y, width, kStrikeThickness), Qt::white});
    }
  }

  if (emphasized) {
    appendRectOutline(out, band, 1.0, color.darker(150));
  }
}

void OverlayPrimitiveBuilder::appendResizeHandles(OverlayPrimitives& out,
                                                  const QRectF& band) {
  if (band.isEmpty()) {
    return;
  }

  for (const QRectF& handle : {leftHandleRect(band), rightHandleRect(band)}) {
    out.quads.push_back(OverlayQuad{handle, Qt::white});
    appendRectOutline(out, handle, 1.0, Qt::black);
  }
}

}  // namespace orc::gui::gpu
