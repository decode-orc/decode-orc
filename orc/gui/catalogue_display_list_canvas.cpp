/*
 * File:        catalogue_display_list_canvas.cpp
 * Module:      orc-gui
 * Purpose:     Where a display list's fills go: a painter, or a document
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "catalogue_display_list_canvas.h"

#include <QImage>
#include <QPainter>

namespace orc::gui {

QBrush video_brush(qreal square, const QPointF& origin) {
  // Two squares across and down, the brush's own pixels, scaled to |square|.
  QImage tile(2, 2, QImage::Format_RGB32);
  tile.setPixel(0, 0, kVideoDark);
  tile.setPixel(1, 1, kVideoDark);
  tile.setPixel(1, 0, kVideoLight);
  tile.setPixel(0, 1, kVideoLight);
  QBrush brush(tile);
  QTransform transform;
  transform.translate(origin.x(), origin.y());
  transform.scale(square, square);
  brush.setTransform(transform);
  return brush;
}

QBrush PainterCanvas::brush_of(const CanvasPaint& paint) const {
  if (paint.brush.style() == Qt::SolidPattern &&
      shows_video(paint.brush.color())) {
    return video_brush(video_square_, video_origin_);
  }
  return paint.brush;
}

void PainterCanvas::fillPath(const QPainterPath& path, const CanvasPaint& paint,
                             bool hard_edged) {
  if (paint.brush.style() == Qt::NoBrush) {
    return;
  }
  if (!hard_edged || !painter_.testRenderHint(QPainter::Antialiasing)) {
    painter_.fillPath(path, brush_of(paint));
    return;
  }
  painter_.save();
  painter_.setRenderHint(QPainter::Antialiasing, false);
  painter_.fillPath(path, brush_of(paint));
  painter_.restore();
}

void PainterCanvas::fillRect(const QRectF& rect, const CanvasPaint& paint,
                             bool hard_edged) {
  if (paint.brush.style() == Qt::NoBrush) {
    return;
  }
  if (!hard_edged || !painter_.testRenderHint(QPainter::Antialiasing)) {
    painter_.fillRect(rect, brush_of(paint));
    return;
  }
  painter_.save();
  painter_.setRenderHint(QPainter::Antialiasing, false);
  painter_.fillRect(rect, brush_of(paint));
  painter_.restore();
}

}  // namespace orc::gui
