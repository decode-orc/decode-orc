/*
 * File:        catalogue_display_list_canvas.cpp
 * Module:      orc-gui
 * Purpose:     Where a display list's fills go: a painter, or a document
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "catalogue_display_list_canvas.h"

#include <QPainter>

namespace orc::gui {

void PainterCanvas::fillPath(const QPainterPath& path,
                             const CanvasPaint& paint) {
  if (paint.brush.style() == Qt::NoBrush) {
    return;
  }
  painter_.fillPath(path, paint.brush);
}

void PainterCanvas::fillRect(const QRectF& rect, const CanvasPaint& paint,
                             bool hard_edged) {
  if (paint.brush.style() == Qt::NoBrush) {
    return;
  }
  if (!hard_edged || !painter_.testRenderHint(QPainter::Antialiasing)) {
    painter_.fillRect(rect, paint.brush);
    return;
  }
  painter_.save();
  painter_.setRenderHint(QPainter::Antialiasing, false);
  painter_.fillRect(rect, paint.brush);
  painter_.restore();
}

}  // namespace orc::gui
