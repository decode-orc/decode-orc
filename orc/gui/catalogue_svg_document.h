/*
 * File:        catalogue_svg_document.h
 * Module:      orc-gui
 * Purpose:     A display list as an SVG document, its presentation as SMIL
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef CATALOGUE_SVG_DOCUMENT_H
#define CATALOGUE_SVG_DOCUMENT_H

#include <orc/stage/tooling/catalogue_results.h>

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <cstdint>
#include <vector>

#include "catalogue_display_list_canvas.h"

namespace orc::gui {

/**
 * @brief An SVG document written from the fills a display list is drawn with
 *
 * Each fill becomes a path or a rectangle of the document, in the device space
 * the list is drawn into, so the document is the drawing a view makes of the
 * list, as vectors. Drawn still, every colour is the one the canvas is handed.
 * Drawn animated, the document carries the presentation as SMIL animation
 * (SVG 1.1 §19), which a web browser plays with no script:
 *   - the operations of each stage (CatalogueDrawOp::appear_ms) are a group
 *     kept out of the rendering until the moment the stage appears;
 *   - a stage swept onto the screen before then (CatalogueDrawOp::
 *     reveal_duration_ms) appears when its sweep starts, clipped to a
 *     rectangle that takes each position of the sweep in turn, and is
 *     unclipped once it is whole;
 *   - a colour naming a palette entry that changes is painted from a
 *     one-stop gradient standing for the entry — "if one stop is defined,
 *     then paint with the solid color fill using the color defined for that
 *     gradient stop" (SVG 1.1 §13.2.4) — whose stop colour is set at each
 *     step of the entry's track and repeats its loop, so that every figure
 *     drawn in a blinking entry blinks together, as the colour map makes it.
 *
 * Complexity: O(n) in what is written. Not thread-safe.
 */
class SvgDocumentCanvas final : public DisplayListCanvas {
 public:
  /**
   * @param size      The document's width and height in its own units: the
   *                  pixels an image of it would have.
   * @param viewport  The part of device space the document shows — the
   *                  drawable area. Whatever falls outside it is clipped.
   * @param list      The list being drawn, for its palette tracks.
   * @param animated  Whether the document plays the presentation or shows the
   *                  still render.
   */
  SvgDocumentCanvas(const QSizeF& size, const QRectF& viewport,
                    const orc::CatalogueDisplayList& list, bool animated);

  void fillPath(const QPainterPath& path, const CanvasPaint& paint) override;
  void fillRect(const QRectF& rect, const CanvasPaint& paint,
                bool hard_edged) override;

  /// One position of a sweep, in device space: from |time_ms| until the next,
  /// the stage is shown within |clip| alone.
  struct RevealFrame {
    uint32_t time_ms = 0;
    QRectF clip;

    bool operator==(const RevealFrame& other) const {
      return time_ms == other.time_ms && clip == other.clip;
    }
  };

  /// The fills from here on belong to the stage that is whole from |time_ms|
  /// of the presentation. Where |reveal| has frames, the stage is swept onto
  /// the screen before then, shown from the first frame's time within each
  /// frame's clip in turn. Does nothing in a still document.
  void beginStage(uint32_t time_ms,
                  const std::vector<RevealFrame>& reveal = {});

  /// Outline |rect| with a dashed line in |colour|, over everything and from
  /// the start: a view's marking of the display area, not part of the page.
  void outline(const QRectF& rect, const QColor& colour);

  /// The finished document, as UTF-8.
  QByteArray document() const;

 private:
  /// The fill for |colour| as the list sent it, which the canvas was handed
  /// already resolved to |resolved|.
  QString colourValue(const orc::CatalogueColour& colour,
                      const QColor& resolved);
  /// The fill for |paint|, or an empty string where it paints nothing.
  QString paintValue(const CanvasPaint& paint);
  /// A pattern standing for a texture brush, written once per distinct tile.
  QString patternFor(const CanvasPaint& paint);

  QSizeF size_;
  QRectF viewport_;
  bool animated_ = false;
  bool pixel_aligned_ = false;
  std::vector<orc::CataloguePaletteTrack> tracks_;
  /// The palette entries a fill has followed, which is which gradients the
  /// document needs.
  std::vector<bool> entries_used_;
  QString patterns_defs_;
  QHash<QString, QString> pattern_ids_;
  QString body_;
  QString overlay_;
  bool stage_open_ = false;
  uint32_t stage_time_ = 0;
  std::vector<RevealFrame> stage_reveal_;
  /// The clip paths written so far, which names the next.
  int reveal_clips_ = 0;
};

}  // namespace orc::gui

#endif  // CATALOGUE_SVG_DOCUMENT_H
