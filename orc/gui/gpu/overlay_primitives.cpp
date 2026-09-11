/*
 * File:        overlay_primitives.cpp
 * Module:      orc-gui
 * Purpose:     Drawing primitives shared by the raster and GPU frame surfaces
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "overlay_primitives.h"

#include <QPainter>
#include <QPen>
#include <algorithm>

namespace orc::gui::gpu {

OverlayPrimitives scaled(const OverlayPrimitives& primitives, double factor) {
  OverlayPrimitives result;
  result.quads.reserve(primitives.quads.size());
  result.lines.reserve(primitives.lines.size());

  for (const OverlayQuad& quad : primitives.quads) {
    result.quads.push_back(OverlayQuad{
        QRectF(quad.rect.x() * factor, quad.rect.y() * factor,
               quad.rect.width() * factor, quad.rect.height() * factor),
        quad.color});
  }
  for (const OverlayLine& line : primitives.lines) {
    result.lines.push_back(OverlayLine{
        QPointF(line.from.x() * factor, line.from.y() * factor),
        QPointF(line.to.x() * factor, line.to.y() * factor), line.color});
  }
  return result;
}

void appendRectOutline(OverlayPrimitives& out, const QRectF& rect, double width,
                       const QColor& color) {
  if (rect.isEmpty() || width <= 0.0) {
    return;
  }

  // Drawn inside the bounds so an outline never enlarges the shape it marks;
  // a rectangle thinner than two edges collapses to a single filled quad
  // rather than overlapping itself.
  const double horizontal = std::min(width, rect.height() / 2.0);
  const double vertical = std::min(width, rect.width() / 2.0);

  if (horizontal * 2.0 >= rect.height() || vertical * 2.0 >= rect.width()) {
    out.quads.push_back(OverlayQuad{rect, color});
    return;
  }

  out.quads.push_back(OverlayQuad{
      QRectF(rect.left(), rect.top(), rect.width(), horizontal), color});
  out.quads.push_back(OverlayQuad{
      QRectF(rect.left(), rect.bottom() - horizontal, rect.width(), horizontal),
      color});
  out.quads.push_back(
      OverlayQuad{QRectF(rect.left(), rect.top() + horizontal, vertical,
                         rect.height() - 2.0 * horizontal),
                  color});
  out.quads.push_back(
      OverlayQuad{QRectF(rect.right() - vertical, rect.top() + horizontal,
                         vertical, rect.height() - 2.0 * horizontal),
                  color});
}

void paintOverlayPrimitives(QPainter& painter,
                            const OverlayPrimitives& primitives) {
  if (primitives.isEmpty()) {
    return;
  }

  const bool had_antialiasing = painter.testRenderHint(QPainter::Antialiasing);
  painter.setRenderHint(QPainter::Antialiasing, false);
  painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

  for (const OverlayQuad& quad : primitives.quads) {
    painter.fillRect(quad.rect, quad.color);
  }

  for (const OverlayLine& line : primitives.lines) {
    QPen pen(line.color);
    pen.setWidth(1);
    painter.setPen(pen);
    painter.drawLine(line.from, line.to);
  }

  painter.setRenderHint(QPainter::Antialiasing, had_antialiasing);
}

}  // namespace orc::gui::gpu
