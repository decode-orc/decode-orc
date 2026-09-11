/*
 * File:        overlay_primitive_builder.h
 * Module:      orc-gui
 * Purpose:     Builds frame-surface overlay primitives from image-space data
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_OVERLAY_PRIMITIVE_BUILDER_H
#define ORC_GUI_OVERLAY_PRIMITIVE_BUILDER_H

#include <QColor>
#include <QPoint>
#include <QRect>
#include <QRectF>

#include "../frame_view_geometry.h"
#include "overlay_primitives.h"

namespace orc::gui::gpu {

/**
 * @brief A dropout extent in image space, independent of which layer named it.
 *
 * The preview widget gets these from `orc::DropoutRegion` and the dropout
 * editor from `orc::presenters::DropoutRegion`; the geometry is the same in
 * both cases, so the builder takes neither type.
 */
struct OverlaySpan {
  int line = 0;
  int start_sample = 0;
  int end_sample = 0;
};

/**
 * @brief Overlay geometry shared by the raster and GPU frame surfaces.
 *
 * Every function here is a pure mapping from image-space data plus a
 * FrameViewGeometry to widget-space primitives, with no widget, painter or
 * graphics API involved, so the coordinates both paths draw can be pinned by
 * unit tests without a GPU or a QApplication.
 *
 * Two of them - regionBandRect() and the handle rects - are also what the
 * dropout editor hit-tests against, so a band that is drawn and a band that
 * can be clicked cannot drift apart.
 */
class OverlayPrimitiveBuilder {
 public:
  /// On-screen thickness of a preview dropout band, from the height of the
  /// displayed image. One to four pixels, as the preview has always used.
  static int dropoutBandThickness(int target_height);

  /// Preview dropout band: a solid bar centred on the scanline, spanning the
  /// dropout's samples. Spans outside the image are skipped.
  static void appendDropoutBand(OverlayPrimitives& out,
                                const FrameViewGeometry& geometry,
                                const OverlaySpan& span, const QColor& color,
                                int thickness);

  /// Cross-hairs through the centre of an image pixel, clipped to @p bounds.
  static void appendCrosshairs(OverlayPrimitives& out,
                               const FrameViewGeometry& geometry,
                               const QPoint& image_pixel, const QRect& bounds,
                               const QColor& color);

  /**
   * @brief Widget-space band for a dropout-editor region.
   *
   * Constant on-screen thickness regardless of zoom so the bands stay crisp,
   * visible and clickable; an emphasised band is thicker so that hovering or
   * selecting one is legible at a glance. Returns an empty rect for a span
   * that does not lie inside the image.
   */
  static QRectF regionBandRect(const FrameViewGeometry& geometry,
                               const OverlaySpan& span, bool emphasized);

  /// Grab handle at the left edge of a band.
  static QRectF leftHandleRect(const QRectF& band);
  /// Grab handle at the right edge of a band.
  static QRectF rightHandleRect(const QRectF& band);

  /**
   * @brief Dropout-editor band, with its strike-through and outline.
   *
   * @param struck     Draws the dashed strike marking a source dropout the
   *                   user has removed
   * @param emphasized Selected or hovered: more opaque, thicker, outlined
   */
  static void appendRegionBand(OverlayPrimitives& out,
                               const FrameViewGeometry& geometry,
                               const OverlaySpan& span, const QColor& color,
                               bool emphasized, bool struck);

  /// The two white, black-bordered grab handles on a selected band.
  static void appendResizeHandles(OverlayPrimitives& out, const QRectF& band);

 private:
  /// True when the span lies wholly inside the geometry's image.
  static bool spanFitsImage(const FrameViewGeometry& geometry,
                            const OverlaySpan& span);
};

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_OVERLAY_PRIMITIVE_BUILDER_H
