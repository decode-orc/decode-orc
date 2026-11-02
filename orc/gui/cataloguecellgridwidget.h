/*
 * File:        cataloguecellgridwidget.h
 * Module:      orc-gui
 * Purpose:     Widget painting a CataloguePayload character-cell grid
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef CATALOGUECELLGRIDWIDGET_H
#define CATALOGUECELLGRIDWIDGET_H

#include <orc/stage/tooling/catalogue_results.h>

#include <QImage>
#include <QPainterPath>
#include <QPointer>
#include <QRectF>
#include <QSize>
#include <QWidget>
#include <optional>
#include <unordered_map>

class CatalogueFlashClock;
class QPainter;

/**
 * @brief Paints a character-cell grid payload
 *
 * The grid is drawn into an aspect-locked rect built from the nominal character
 * rectangle the payload carries, so glyphs and mosaic blocks keep their
 * proportions whatever shape the widget is given.
 *
 * Alphanumeric cells are drawn from the teletext character generator's own
 * face (teletext_glyph_painter.h), scaled to the character rectangle, and fall
 * back to the platform's monospaced font for a character that face has no
 * glyph for. Mosaic cells are painted as 2x3 sub-element blocks, contiguous or
 * separated. Cell attributes are honoured: foreground and background out of
 * the payload's own palette, double height (the character spans both rows at
 * unchanged width), boxing on a boxed-only page, conceal (rendered as SPACE —
 * no reveal control) and flash (animated against a CatalogueFlashClock, or
 * held in its lit phase when there is no clock or animation is switched off).
 * The palette travels with the payload rather than being theme-derived,
 * because a service with a fixed display palette means it.
 *
 * Painting is two-pass — every background first, then every foreground —
 * because a double-height character occupies the row below its origin and a
 * row-sequential paint would erase its lower half.
 */
class CatalogueCellGridWidget : public QWidget {
  Q_OBJECT

 public:
  explicit CatalogueCellGridWidget(QWidget* parent = nullptr);
  ~CatalogueCellGridWidget() override;

  void setGrid(const orc::CatalogueCellGrid& grid);
  void clearGrid();
  bool hasGrid() const { return grid_.has_value(); }

  /**
   * @brief Overlay the grid with markers for data that never arrived
   *
   * A row no packet was recovered for renders exactly like a transmitted blank
   * row, and a damaged byte exactly like a SPACE, so without this overlay a
   * recovery gap is indistinguishable from content. Off by default, because
   * the markers are not part of the page.
   */
  void setShowDataErrors(bool show);
  bool showDataErrors() const { return show_data_errors_; }

  /**
   * @brief Drive the flash phase from |clock|
   *
   * nullptr detaches, which leaves the grid in its lit phase. The dialogue
   * owns the clock, so stepping between pages does not restart the cycle.
   */
  void setFlashClock(CatalogueFlashClock* clock);

  /// Animate flashing cells. Off holds every cell in its lit phase, which is
  /// the page as transmitted and what a reader wants to sit and read.
  void setAnimationsEnabled(bool enabled);
  bool animationsEnabled() const { return animations_enabled_; }

  /// Currently displayed grid (test seam; nullptr when cleared)
  const orc::CatalogueCellGrid* grid() const {
    return grid_ ? &*grid_ : nullptr;
  }

  /// Whether the grid holds a cell whose flash attribute changes anything
  /// (test seam; also what gates the clock subscription)
  bool hasFlashingCells() const { return has_flashing_cells_; }

  /// The flash phase being painted; true is the lit one. Settable so tests can
  /// exercise both phases without a wall clock (test seam).
  bool flashLit() const { return flash_lit_; }
  void setFlashLit(bool lit);

  /// Placeholder shown when no grid is set
  void setPlaceholderText(const QString& text);

  QSize sizeHint() const override;

  /**
   * @brief The page alone as an image |size| pixels across and down
   *
   * The page is fitted to |size| at the character-rectangle aspect exactly as
   * it is fitted to the widget, so a size of the page's own aspect — which is
   * what pageImageSize() gives — leaves no border around it.
   *
   * Always the lit phase, whatever phase the view is holding: flashing text is
   * on the page because the service put it there, and a still of the blank
   * phase would drop it. The damage overlay follows the view's own setting,
   * because a reader who turned it on is looking at the damage.
   *
   * Null when there is no grid to draw or |size| is empty.
   */
  QImage renderPageImage(const QSize& size) const;

  /// The size renderPageImage() is worth being asked for: whole pixels per
  /// character rectangle, at the page's own aspect. Null when there is no grid.
  QSize pageImageSize() const;

 protected:
  void paintEvent(QPaintEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

 private:
  /// Where the page sits in the widget, at the character-rectangle aspect
  struct PageGeometry {
    QRectF page;
    qreal cell_w = 0.0;
    qreal cell_h = 0.0;
    /// False when there is no grid, or when it has been squeezed below one
    /// device pixel per cell and nothing can usefully be drawn
    bool valid = false;
  };
  /// Where the page sits within a paint surface |bounds| pixels across and
  /// down, whose origin is (0, 0) — the widget itself, or the image a save
  /// renders into
  PageGeometry pageGeometryIn(const QSizeF& bounds) const;
  PageGeometry pageGeometry() const { return pageGeometryIn(QSizeF(size())); }

  /// Draw the page into |bounds| of |painter|, in the given flash phase. What
  /// both the widget's own paint and an exported image are.
  void paintContent(QPainter& painter, const QRectF& bounds, bool lit) const;

  /// The character generator's outline for |character|, in the unit character
  /// rectangle teletext_glyph_path() gives, or nullptr where the face has no
  /// glyph for it and the platform font has to draw it instead. Cached: a page
  /// is a thousand cells drawn from a few dozen distinct characters, and every
  /// flash phase redraws them.
  const QPainterPath* glyphPath(char32_t character) const;

  /// Hatch rows that carried no packet and outline damaged cells
  static void paintDataErrorOverlay(QPainter& painter,
                                    const orc::CatalogueCellGrid& grid,
                                    const QRectF& page_rect, qreal cell_w,
                                    qreal cell_h);

  /// Subscribe to the clock only while this view is visible and holds
  /// something that flashes; release it otherwise
  void updateFlashSubscription();
  /// Repaint the flashing cells alone — a phase change touches nothing else
  void repaintFlashingCells();

  std::optional<orc::CatalogueCellGrid> grid_;
  mutable std::unordered_map<char32_t, QPainterPath> glyph_paths_;
  QString placeholder_;
  bool show_data_errors_ = false;

  QPointer<CatalogueFlashClock> flash_clock_;
  bool animations_enabled_ = true;
  bool flash_lit_ = true;
  bool has_flashing_cells_ = false;
  bool flash_subscribed_ = false;
};

#endif  // CATALOGUECELLGRIDWIDGET_H
