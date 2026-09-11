/*
 * File:        overlay_primitives.h
 * Module:      orc-gui
 * Purpose:     Drawing primitives shared by the raster and GPU frame surfaces
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_OVERLAY_PRIMITIVES_H
#define ORC_GUI_OVERLAY_PRIMITIVES_H

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <vector>

class QPainter;

namespace orc::gui::gpu {

/**
 * @brief A filled, axis-aligned rectangle in surface coordinates.
 *
 * Everything with area is one of these - dropout bands, editor bands, resize
 * handles and the four edges an outline is made of. A GPU pipeline can draw a
 * stroked rectangle only by emitting its edges as quads anyway, so the
 * builders emit the edges and both paths draw the same shapes.
 */
struct OverlayQuad {
  QRectF rect;
  QColor color;
};

/**
 * @brief A hairline segment in surface coordinates.
 *
 * One device pixel wide and never antialiased, matching how the cross-hairs
 * are drawn today. Anything thicker is a quad: RHI backends do not agree on
 * line widths above one.
 */
struct OverlayLine {
  QPointF from;
  QPointF to;
  QColor color;
};

/**
 * @brief One frame's worth of overlay drawing, ready for either path.
 *
 * Built in widget coordinates by OverlayPrimitiveBuilder from the same
 * FrameViewGeometry mapping the raster code has always used, then either
 * painted directly (raster) or scaled into device pixels and uploaded as
 * vertices (GPU). Draw order is quads first, then lines, which is the order
 * the existing paint code uses.
 */
struct OverlayPrimitives {
  std::vector<OverlayQuad> quads;
  std::vector<OverlayLine> lines;

  bool isEmpty() const { return quads.empty() && lines.empty(); }
  void clear() {
    quads.clear();
    lines.clear();
  }
};

/// Multiply every coordinate by @p factor, for the device-pixel conversion a
/// GPU render target needs. Colours are untouched.
OverlayPrimitives scaled(const OverlayPrimitives& primitives, double factor);

/// Append an outline of @p rect as its four edge quads, @p width pixels
/// thick, drawn inside the rectangle's bounds.
void appendRectOutline(OverlayPrimitives& out, const QRectF& rect, double width,
                       const QColor& color);

/// Draw the primitives with QPainter. This is the raster path, and is also
/// what the GPU path is checked against.
void paintOverlayPrimitives(QPainter& painter,
                            const OverlayPrimitives& primitives);

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_OVERLAY_PRIMITIVES_H
