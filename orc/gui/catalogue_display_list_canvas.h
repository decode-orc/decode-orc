/*
 * File:        catalogue_display_list_canvas.h
 * Module:      orc-gui
 * Purpose:     Where a display list's fills go: a painter, or a document
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef CATALOGUE_DISPLAY_LIST_CANVAS_H
#define CATALOGUE_DISPLAY_LIST_CANVAS_H

#include <orc/stage/tooling/catalogue_results.h>

#include <QBrush>
#include <QColor>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <optional>

class QPainter;

namespace orc::gui {

/// A colour as a view shows it. A transparent colour shows the lower planes
/// through — the programme a caption or an overlaid page was set over — which
/// a view has no picture of: it is the colour with nothing in it, which a
/// canvas fills with video_brush(). The view and the documents it exports
/// share this, so a texture's colours read back off its painted tile as the
/// document wrote them.
inline QColor shown_colour(const orc::CatalogueColour& colour) {
  if (colour.transparent) {
    return QColor(0, 0, 0, 0);
  }
  return QColor(colour.red, colour.green, colour.blue);
}

/// Whether |colour| is shown_colour()'s for a transparent colour.
inline bool shows_video(const QColor& colour) { return colour.alpha() == 0; }

/// The side of one square of the checkerboard standing for the video a
/// transparent colour shows, in units of the unit screen: four pixels of the
/// 256-pixel reference receiver, so it holds still across the receivers.
constexpr double kVideoSquareUnits = 4.0 / 256.0;
/// The checkerboard's two greys: a dark, plain ground no page draws with,
/// which reads as "the picture shows here" rather than as a colour.
constexpr QRgb kVideoDark = 0xff2a2a2a;
constexpr QRgb kVideoLight = 0xff3c3c3c;

/**
 * @brief The brush a canvas fills a transparent colour with: the checkerboard
 *        standing for the programme video
 *
 * @p square is the side of one of its squares in device units; @p origin
 * where a square's corner lies, so neighbouring fills meet seamlessly.
 */
QBrush video_brush(qreal square, const QPointF& origin);

/**
 * @brief What one fill is painted with
 *
 * The brush a painter fills with — a colour, or a texture tile of one — and
 * the colours as the display list sent them. A painter needs only the brush,
 * resolved to the moment being drawn. A document that carries the
 * presentation itself also needs to know which palette entry a colour
 * follows, so that the fill can change with the entry rather than be fixed at
 * one moment's colour.
 */
struct CanvasPaint {
  QBrush brush;
  /// The drawing colour as sent, palette entry and all.
  orc::CatalogueColour colour;
  /// The colour a texture's unlit elements take — colour mode 2's background
  /// — as sent, where they take one.
  std::optional<orc::CatalogueColour> ground;
};

/**
 * @brief Where the fills of a display list's operations go
 *
 * Every operation of a display list comes down to shapes filled in device
 * space. Drawing a list and exporting it share their geometry by sharing
 * this: the view paints through a QPainter, and an exported document writes
 * each fill as an element of its own.
 *
 * Not thread-safe; one canvas per drawing.
 */
class DisplayListCanvas {
 public:
  virtual ~DisplayListCanvas() = default;

  /// Fill |path| with |paint|, by the path's own fill rule. |hard_edged|
  /// draws it without smoothing whatever the rest of the drawing has: cells
  /// that tile, such as a mosaic's elements, whose shared edges smoothing
  /// would show as seams.
  virtual void fillPath(const QPainterPath& path, const CanvasPaint& paint,
                        bool hard_edged) = 0;

  /// Fill |rect| with |paint|. |hard_edged| draws it without smoothing
  /// whatever the rest of the drawing has: a receiver's pixel, whose edges are
  /// the content rather than an artefact of drawing.
  virtual void fillRect(const QRectF& rect, const CanvasPaint& paint,
                        bool hard_edged) = 0;
};

/**
 * @brief A canvas that paints into a QPainter, in whatever state the caller
 *        left it — clip, smoothing and all
 */
class PainterCanvas final : public DisplayListCanvas {
 public:
  /// |video_square|: the side of a square of the checkerboard a transparent
  /// colour is filled with (video_brush()), in device units, and
  /// |video_origin| a corner of one, the page's.
  PainterCanvas(QPainter& painter, qreal video_square,
                const QPointF& video_origin)
      : painter_(painter),
        video_square_(video_square),
        video_origin_(video_origin) {}

  void fillPath(const QPainterPath& path, const CanvasPaint& paint,
                bool hard_edged) override;
  void fillRect(const QRectF& rect, const CanvasPaint& paint,
                bool hard_edged) override;

 private:
  /// |paint|'s brush, the video's checkerboard for a transparent colour.
  QBrush brush_of(const CanvasPaint& paint) const;

  QPainter& painter_;
  qreal video_square_;
  QPointF video_origin_;
};

}  // namespace orc::gui

#endif  // CATALOGUE_DISPLAY_LIST_CANVAS_H
