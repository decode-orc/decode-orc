/*
 * File:        cataloguedisplaylistwidget.cpp
 * Module:      orc-gui
 * Purpose:     Widget rasterising a CataloguePayload 2D display list
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "cataloguedisplaylistwidget.h"

#include <QBrush>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QHash>
#include <QHideEvent>
#include <QImage>
#include <QKeyEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QShowEvent>
#include <QString>
#include <QStringList>
#include <QTransform>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "catalogue_display_list_canvas.h"
#include "catalogue_stroke_geometry.h"
#include "catalogue_svg_document.h"

namespace {

using orc::CatalogueColour;
using orc::CatalogueDisplayList;
using orc::CatalogueDrawKind;
using orc::CatalogueDrawOp;
using orc::CatalogueFillPattern;
using orc::CatalogueLineStyle;
using orc::CataloguePoint;
using orc::gui::CanvasPaint;
using orc::gui::DisplayListCanvas;
using orc::gui::PelBrush;
using orc::gui::StrokePiece;

// Nominal size of the drawable area, which is what the widget's size hint is
// built from. The height follows the payload's own aspect at paint time; this
// is only a starting shape.
constexpr int kNominalPixelsAcross = 256;
constexpr int kNominalPixelsDown = 200;

// How wide a saved drawable area is. A display list is resolution-independent —
// its unit space carries no pixels of its own — so the width is a choice, and
// this one puts the fine strokes a service draws letterforms with well clear of
// a single pixel.
constexpr int kSavedPixelsAcross = 720;

// How wide a saved drawable area is when the list names the grid it was
// resolved against.
//
// One size for every grid, rather than one proportional to each. What the grid
// changes is how much of the page's detail survives being drawn, and that is
// exactly what someone saving the same page at two of them is looking at; two
// images of different sizes make the comparison about the sizes instead. It is
// also the size a reader gets to keep: a page is saved to put somewhere, and
// what it is worth on the far side does not depend on which receiver was
// picked.
//
// 1536 is the smallest width that is a whole multiple of every grid a page is
// drawn against — six times the reference model's 256, three times twice it and
// twice three times it — so a column of any of them lands on a whole number of
// image columns. The rows cannot: the grid fills a 4:3 area, which makes the
// source pixel a twenty-fifth wider than it is tall, and a scale whole both
// ways would have to be 25 across for every 24 down.
constexpr int kSavedGridPixelsAcross = 1536;

// Without a grid to measure against, the logical pel is never drawn narrower
// than one device pixel: "the logical pel ... will always map to at least one
// and possibly many display pixels" (X3.110 §5.3.2.2.6). Where the list names
// the grid it was resolved against, one pixel of *that* grid is the floor
// instead — see pelFloor().
constexpr qreal kMinimumPenWidth = 1.0;

// The longest a QTimer can be armed for. A change further off than this is
// reached over more than one wake, each of which reads the clock afresh and
// only re-arms if the change is still to come.
constexpr uint32_t kLongestWakeMs =
    static_cast<uint32_t>(std::numeric_limits<int>::max());

// How far a flattened arc may stray from its circle, in device pixels. A
// fraction of a pixel, so the polyline cannot be told from the curve.
constexpr qreal kArcTolerancePixels = 0.2;

// The reference size character outlines are laid out at, in pixels. Outlines
// scale exactly, so this only has to be large enough that the font's own
// metrics are not rounded coarsely.
constexpr int kGlyphReferencePixels = 128;

// The colour the drawable area's edge is marked in, while data errors are
// shown.
const QColor kDisplayAreaOutline(255, 96, 96);

// The box drawn for a character no face has: inset from the field so that a
// run of them reads as separate characters, and outlined rather than solid so
// that it cannot be taken for a block mosaic or a character of the repertoire.
constexpr qreal kReplacementInsetAcross = 0.15;
constexpr qreal kReplacementInsetDown = 0.1;
constexpr qreal kReplacementStroke = 1.0 / 8.0;

/**
 * @brief The narrowest the pel is drawn along one axis, in device pixels
 *
 * A source that names the receiver it was authored for sizes a stroke of no
 * stated width in that receiver's pixels, not in whatever pixels the list is
 * being drawn into: it is one pixel wide on a 256-across receiver whether that
 * receiver's picture is shown small or large. Deriving the floor from the grid
 * is what stops stroke width tracking the size of the window. It is also the
 * cell the pel is laid from and stepped in (orc::gui::pel_brush(),
 * orc::gui::swept_pel()).
 *
 * @param extent         The drawable area's size along the axis.
 * @param nominal_cells  The grid's pixels along the axis.
 */
qreal pelFloor(qreal extent, int nominal_cells) {
  return std::max(extent / nominal_cells, kMinimumPenWidth);
}

/// Whether |list| names the grid it was resolved against.
bool names_grid(const CatalogueDisplayList& list) {
  return list.nominal_width > 0 && list.nominal_height > 0;
}

/// The extent of unit y |list| covers, which is 1 where it states none.
double unit_aspect(const CatalogueDisplayList& list) {
  return list.aspect_height > 0.0 ? list.aspect_height : 1.0;
}

/// The shape |list| is displayed at, height over width: its display aspect
/// where it states one, otherwise the extent of unit y it covers.
double shown_aspect(const CatalogueDisplayList& list) {
  return list.display_aspect_height > 0.0 ? list.display_aspect_height
                                          : unit_aspect(list);
}

// The finest a pattern's step is drawn before it is filled solid instead, in
// device pixels. A step below one pixel has no band or element left to show,
// and a tile sampled at that scale is noise rather than a pattern; solid is
// both the honest answer and the one §5.3.2.4.4 gives the dimensionless pel it
// is on its way to.
constexpr qreal kMinimumPatternStep = 1.0;

/**
 * @brief The fill for an operation's pattern, at the size the standard gives it
 *
 * Both kinds of pattern are sized by the drawing itself rather than by the
 * renderer: §5.3.2.4.4 makes a hatch line and the gap beside it each one
 * logical pel across, and §5.3.2.4.5 steps and repeats a programmable mask over
 * the mask size the source stated. Both are registered against the *unit
 * screen's* origin and not the figure's, so that "registration of the patterns
 * shall be maintained across figures" — two figures filled with the same
 * pattern line up with each other.
 *
 * That is what the brush's own transform carries here: it puts brush space's
 * origin on unit (0, 0) and runs its y upwards, so the tile repeats across the
 * whole page at the stated size instead of at whatever pitch a stock Qt brush
 * happens to have in device pixels.
 *
 * What the pattern leaves undrawn depends on the colour mode. In modes 0 and 1
 * it is left alone, so whatever is beneath shows through; in mode 2 — where the
 * operation has a background — "the fill areas not drawn in the drawing color
 * are drawn in the background color" (§5.3.2.4.4). A programmable mask that was
 * never defined is "a null texture pattern, resulting in no fill in color modes
 * 0 and 1, and a fill with the background color in color mode 2" (§5.3.2.4.4),
 * which is Qt::NoBrush in the first case.
 *
 * @param ground  The background colour in mode 2, or nullptr in modes 0 and 1.
 */
QBrush texture_brush(const CatalogueDrawOp& op,
                     const CatalogueDisplayList& list, const QColor& colour,
                     const QColor* ground, const QRectF& area, qreal unit_x,
                     qreal unit_y) {
  const QBrush solid(colour, Qt::SolidPattern);
  if (op.fill_pattern == CatalogueFillPattern::kSolid) {
    return solid;
  }
  const QColor unlit = ground != nullptr ? *ground : QColor(Qt::transparent);

  // Brush space is the tile's own pixels; this puts its origin on the unit
  // screen's and points its y the way unit y points. |tile_x| and |tile_y| are
  // how far the whole tile reaches in device pixels before it repeats, which is
  // one step of a mask but *two* pels of a hatch — a lit band and the gap
  // beside it.
  const auto anchored = [&area](const QImage& tile, qreal tile_x,
                                qreal tile_y) {
    QBrush brush(tile);
    QTransform transform;
    transform.translate(area.left(), area.bottom());
    transform.scale(tile_x / tile.width(), -tile_y / tile.height());
    brush.setTransform(transform);
    return brush;
  };

  const bool is_hatch =
      op.fill_pattern == CatalogueFillPattern::kVerticalHatch ||
      op.fill_pattern == CatalogueFillPattern::kHorizontalHatch ||
      op.fill_pattern == CatalogueFillPattern::kCrossHatch;
  if (is_hatch) {
    // One pel across and one down, in device pixels. Either may be too fine to
    // draw as bands, in which case that axis is solid — which is also what the
    // standard's own degenerate case says of a pel with no extent.
    const qreal step_x = std::fabs(op.pen_size.dx) * unit_x;
    const qreal step_y = std::fabs(op.pen_size.dy) * unit_y;
    const bool across_solid = !(step_x > kMinimumPatternStep);
    const bool down_solid = !(step_y > kMinimumPatternStep);

    // A hatch line and the gap beside it are each one pel, so a band is lit
    // when its index is even — which over two bands means the first of them.
    const auto lit = [&](int band_x, int band_y) {
      switch (op.fill_pattern) {
        case CatalogueFillPattern::kVerticalHatch:
          return across_solid || band_x == 0;
        case CatalogueFillPattern::kHorizontalHatch:
          return down_solid || band_y == 0;
        default:
          // Cross hatch draws both sets, so a band on either is lit.
          return across_solid || band_x == 0 || down_solid || band_y == 0;
      }
    };
    if (lit(0, 0) && lit(0, 1) && lit(1, 0) && lit(1, 1)) {
      return solid;
    }

    QImage tile(2, 2, QImage::Format_ARGB32_Premultiplied);
    for (int band_x = 0; band_x < 2; ++band_x) {
      for (int band_y = 0; band_y < 2; ++band_y) {
        tile.setPixelColor(band_x, band_y,
                           lit(band_x, band_y) ? colour : unlit);
      }
    }
    // The tile is a lit band and an unlit one on each axis, so it repeats every
    // two pels. An axis drawn solid tiles at any pitch, so it borrows the
    // other's rather than scaling by a zero that would collapse the transform.
    return anchored(tile, 2.0 * (across_solid ? step_y : step_x),
                    2.0 * (down_solid ? step_x : step_y));
  }

  const size_t slot = static_cast<size_t>(op.fill_pattern) -
                      static_cast<size_t>(CatalogueFillPattern::kMask0);
  if (slot >= list.fill_masks.size() || !list.fill_masks[slot].defined()) {
    // The null pattern of a mask that was never defined.
    return ground != nullptr ? QBrush(*ground, Qt::SolidPattern)
                             : QBrush(Qt::NoBrush);
  }
  const auto& mask = list.fill_masks[slot];

  const qreal step_x = std::fabs(op.fill_mask_size.dx) * unit_x;
  const qreal step_y = std::fabs(op.fill_mask_size.dy) * unit_y;
  if (!(step_x > kMinimumPatternStep) || !(step_y > kMinimumPatternStep)) {
    // No stated size, or one too fine to resolve into elements.
    return solid;
  }

  QImage tile(mask.width, mask.height, QImage::Format_ARGB32_Premultiplied);
  for (int row = 0; row < mask.height; ++row) {
    for (int column = 0; column < mask.width; ++column) {
      const bool on =
          mask.elements[static_cast<size_t>(row) * mask.width + column];
      // Row 0 of the buffer is its bottom, which is where the brush transform's
      // upward y already puts row 0 of the tile. "The sign bits of dx and dy
      // are used to reflect the mask pattern within the mask field"
      // (§5.3.2.4.5), and reflecting the tile is reflecting the field.
      const int x =
          op.fill_mask_size.dx < 0.0 ? mask.width - 1 - column : column;
      const int y = op.fill_mask_size.dy < 0.0 ? mask.height - 1 - row : row;
      tile.setPixelColor(x, y, on ? colour : unlit);
    }
  }
  return anchored(tile, step_x, step_y);
}

/// The pieces that cover every segment of |path| whole, which is how a solid
/// line, a traced outline and a highlight are swept.
std::vector<StrokePiece> solid_pieces(const QVector<QPointF>& path,
                                      bool closed) {
  return orc::gui::textured_pieces(path, closed, CatalogueLineStyle::kSolid,
                                   0.0, 0.0, kMinimumPatternStep);
}

/// What the pel covers along |pieces|, and stamped at |plotted|, as one path:
/// every sweep winds the same way, so the path fills as their union. A dash is
/// stamped whole at both its ends too, where a receiver put the pel down
/// first and last (orc::gui::swept_pel() takes half a cell off a moving pel).
QPainterPath swept_path(const std::vector<StrokePiece>& pieces,
                        const QVector<QPointF>& plotted, const PelBrush& pel) {
  QPainterPath swept;
  swept.setFillRule(Qt::WindingFill);
  for (const StrokePiece& piece : pieces) {
    orc::gui::add_swept_pel(swept, piece.from, piece.to, pel);
    if (piece.dash && piece.from != piece.to) {
      orc::gui::add_swept_pel(swept, piece.from, piece.from, pel);
      orc::gui::add_swept_pel(swept, piece.to, piece.to, pel);
    }
  }
  for (const QPointF& point : plotted) {
    orc::gui::add_swept_pel(swept, point, point, pel);
  }
  return swept;
}

/**
 * @brief Fill what the pel covers along |pieces|, and stamped at |plotted|
 *
 * One path for the lot, so an antialiased edge is worked out once for the
 * union rather than once per sweep: where two sweeps overlap or abut, there is
 * no seam.
 */
void fill_swept(DisplayListCanvas& canvas,
                const std::vector<StrokePiece>& pieces,
                const QVector<QPointF>& plotted, const PelBrush& pel,
                const CanvasPaint& paint) {
  if (paint.brush.style() == Qt::NoBrush) {
    return;
  }
  canvas.fillPath(swept_path(pieces, plotted, pel), paint);
}

/// How one operation's lines are drawn: the pel, the texture, and the colours
/// of the dots and dashes and of the spaces between them.
struct LineInk {
  PelBrush pel;
  /// The pel's own extent in device pixels, before any floor: what the
  /// texture steps by.
  qreal pel_width = 0.0;
  qreal pel_height = 0.0;
  CatalogueLineStyle texture = CatalogueLineStyle::kSolid;
  CanvasPaint colour;
  /// The background colour in mode 2, where the spaces of a texture are drawn
  /// in it; nullopt in modes 0 and 1, where they are left alone.
  std::optional<CanvasPaint> gaps;
  /// The receiver's grid the record's points are plotted on, which a straight
  /// line's texture is registered to (orc::gui::textured_pieces()); nullopt
  /// for a curve, and in a list that names no grid.
  std::optional<orc::gui::ReceiverGrid> grid;
};

/**
 * @brief Draw a line, or the outline of a figure, in its line texture
 *
 * The pel is swept along the path's dots and dashes — in whole pels along a
 * curve, and registered to the receiver's grid as the receiver laid them along
 * the straight lines of a list that names one (orc::gui::textured_pieces()) —
 * and stamped at |plotted|: "all end points of lines and arcs and all vertices
 * of ... outlined or highlighted rectangles and polygons must be plotted
 * regardless of the line texture used" (§5.3.2.4.2). The caller names those,
 * because only it knows which vertices are the record's and which only flatten
 * a curve.
 *
 * "In color mode 2, the inter-dot and inter-dash spacing is drawn in the
 * background color" (§5.3.2.4.2): the whole path is swept in the background
 * first, and the texture drawn over it.
 */
void stroke_line(DisplayListCanvas& canvas, const QVector<QPointF>& path,
                 bool closed, const QVector<QPointF>& plotted,
                 const LineInk& ink) {
  if (path.isEmpty()) {
    return;
  }
  if (ink.gaps && ink.texture != CatalogueLineStyle::kSolid) {
    fill_swept(canvas, solid_pieces(path, closed), {}, ink.pel, *ink.gaps);
  }
  fill_swept(canvas,
             orc::gui::textured_pieces(path, closed, ink.texture, ink.pel_width,
                                       ink.pel_height, kMinimumPatternStep,
                                       ink.grid ? &*ink.grid : nullptr),
             plotted, ink.pel, ink.colour);
}

/**
 * @brief Fill a figure, with the outline the pel traces round it
 *
 * X3.110 §5.3.3.3, §5.3.3.4.1, §5.3.3.5.1 and §5.3.3.6.5 all put "the region
 * of the outline traced by the logical pel" *inside* the filled area, and
 * §5.3.2.4.3 defines that region as what the pel sweeps along the outline. A
 * filled figure is therefore its enclosed area plus a sweep of the pel, in the
 * same fill — not the enclosed area alone. A service draws a letterform or a
 * thin mark as a path that encloses almost nothing and lets the pel give it
 * its weight: a logo can draw each letter this way.
 *
 * The enclosed area is taken by the odd-even rule, as the pixel modes take it,
 * so a frame drawn as one path — round the outside, in through a slit and round
 * the inside — keeps its hole.
 *
 * @param outline   The figure's vertices, closed back to the first.
 * @param vertices  The record's own points among them, where the pel is
 *                  stamped whole (a flattened curve's other vertices are not).
 */
void fill_figure(DisplayListCanvas& canvas, const QVector<QPointF>& outline,
                 const QVector<QPointF>& vertices, const CanvasPaint& fill,
                 const PelBrush& pel) {
  if (outline.isEmpty() || fill.brush.style() == Qt::NoBrush) {
    return;
  }
  QPainterPath enclosed;
  enclosed.setFillRule(Qt::OddEvenFill);
  enclosed.addPolygon(QPolygonF(outline));
  enclosed.closeSubpath();
  // One fill of the union. Filled apart, the enclosed area and the traced
  // outline would meet along the outline with each only partly covering the
  // pixels there, and the ground would show through between them as a seam.
  canvas.fillPath(enclosed.united(swept_path(
                      solid_pieces(outline, /*closed=*/true), vertices, pel)),
                  fill);
}

/**
 * @brief A hollow box in |outer|, |thickness| wide
 *
 * The inner edge winds the opposite way to the outer one, so the box stays
 * hollow under either fill rule and whatever it is added to.
 */
QPainterPath ring(const QRectF& outer, qreal thickness) {
  QPainterPath box;
  box.setFillRule(Qt::WindingFill);
  box.addRect(outer);
  const QRectF inner =
      outer.adjusted(thickness, thickness, -thickness, -thickness);
  if (inner.width() > 0.0 && inner.height() > 0.0) {
    box.moveTo(inner.topLeft());
    box.lineTo(inner.bottomLeft());
    box.lineTo(inner.bottomRight());
    box.lineTo(inner.topRight());
    box.closeSubpath();
  }
  return box;
}

/**
 * @brief Device space from a character field's own frame
 *
 * The field's frame has x running right from the character field origin and y
 * running down, with the field occupying QRectF(0, -height, width, height): the
 * origin is its lower left, "regardless of the sign of the character field
 * dimensions dx and dy" (§5.3.2.3.2).
 *
 * A negative dimension reflects the pattern within the field — "about the
 * vertical center axis of the character field" for dx, and about the
 * horizontal one for dy (§5.3.2.3.9) — and rotation then turns the field
 * counter-clockwise about its origin (§5.3.2.3.2, Figure 17), taking the
 * pattern with it, "so that the relative position of the images within the
 * character field is unchanged". Each character turns about its own origin:
 * the path the cursor takes is "independent of the character rotation"
 * (§5.3.2.3.3).
 */
QTransform field_transform(const QPointF& origin, qreal width, qreal height,
                           bool reflect_x, bool reflect_y,
                           int rotation_degrees) {
  QTransform transform;
  transform.translate(origin.x(), origin.y());
  // Counter-clockwise on a y-up screen is a negative angle in device space.
  transform.rotate(-rotation_degrees);
  if (reflect_x) {
    transform.translate(width, 0.0);
    transform.scale(-1.0, 1.0);
  }
  if (reflect_y) {
    transform.translate(0.0, -height);
    transform.scale(1.0, -1.0);
  }
  return transform;
}

/**
 * @brief Paint character fields and the shapes in them, in one colour mode
 *
 * §6.2.7.4: in reverse video "the pixels surrounding the character shape in the
 * character field are drawn in the drawing color. The pixels of the character
 * shape are not drawn, except in color mode 2, when they are drawn in the
 * background color." So in modes 0 and 1 the shape is cut out of the field and
 * whatever lies beneath shows through it.
 *
 * Every field of a run is filled in one go, so fields that abut meet without
 * an antialiased seam between them.
 *
 * @param fields      The character fields, in device space.
 * @param shapes      The character shapes, in device space.
 * @param background  The background colour in mode 2, or nullptr.
 */
void paint_characters(DisplayListCanvas& canvas, const QPainterPath& fields,
                      const QPainterPath& shapes, const CanvasPaint& colour,
                      const CanvasPaint* background, bool reverse) {
  if (reverse) {
    if (background != nullptr) {
      canvas.fillPath(fields, colour);
      canvas.fillPath(shapes, *background);
    } else {
      canvas.fillPath(fields.subtracted(shapes), colour);
    }
    return;
  }
  if (background != nullptr) {
    canvas.fillPath(fields, *background);
  }
  canvas.fillPath(shapes, colour);
}

/**
 * @brief The six sub-elements of a 2x3 mosaic, in a field's own frame
 *
 * Bit 0 is top-left through bit 5 bottom-right. In separated mode (§5.4) each
 * lit element is "reduced in the horizontal dimension by the absolute value of
 * the width (dx) of the logical pel size" and in the vertical by its height,
 * and "left and bottom justified within the normal element area", so the gap
 * opens on the right and at the top; a pel as large as the element reduces it
 * to nothing. The caller floors the inset at one pixel of the receiver's grid:
 * with a dimensionless pel "the separation of separated mosaics is the minimal
 * physical separation which may be presented; that is, a separation of one
 * physical pixel" (T.101 Annex D Addendum §6), so separated mosaics always
 * look different from contiguous ones (Table D1 item 6).
 */
QPainterPath mosaic_shape(qreal width, qreal height, uint8_t pattern,
                          bool separated, qreal inset_x, qreal inset_y) {
  QPainterPath shape;
  shape.setFillRule(Qt::WindingFill);
  const qreal element_w = width / 2.0;
  const qreal element_h = height / 3.0;
  for (int element = 0; element < 6; ++element) {
    if ((pattern & (1 << element)) == 0) {
      continue;
    }
    const int column = element % 2;
    const int row = element / 2;
    QRectF rect(column * element_w, -height + row * element_h, element_w,
                element_h);
    if (separated) {
      rect.setWidth(std::max<qreal>(0.0, rect.width() - inset_x));
      rect.setTop(rect.top() + std::min(inset_y, rect.height()));
    }
    if (rect.width() > 0.0 && rect.height() > 0.0) {
      shape.addRect(rect);
    }
  }
  return shape;
}

/**
 * @brief A downloadable glyph's lit elements, in a field's own frame
 *
 * The pattern is stretched to the field, whatever shape it is. Row 0 of the
 * buffer is its bottom. Each row's lit elements are merged into runs, so a
 * glyph is a handful of rectangles rather than one per element.
 */
QPainterPath glyph_shape(const orc::CatalogueBitmap& glyph, qreal width,
                         qreal height) {
  QPainterPath shape;
  shape.setFillRule(Qt::WindingFill);
  if (!glyph.defined()) {
    return shape;
  }
  const qreal cell_w = width / glyph.width;
  const qreal cell_h = height / glyph.height;
  for (int row = 0; row < glyph.height; ++row) {
    const qreal top = -(row + 1) * cell_h;
    int column = 0;
    while (column < glyph.width) {
      const auto lit = [&](int at) {
        return glyph.elements[static_cast<size_t>(row) * glyph.width + at];
      };
      if (!lit(column)) {
        ++column;
        continue;
      }
      const int first = column;
      while (column < glyph.width && lit(column)) {
        ++column;
      }
      shape.addRect(
          QRectF(first * cell_w, top, (column - first) * cell_w, cell_h));
    }
  }
  return shape;
}

/**
 * @brief Where a receiver plots a point of the record, in unit space
 *
 * "Those physical pixels are illuminated to which any portion of the internal
 * representation of the drawing maps; for example, an internal representation
 * of 12 bits accuracy might map to a physical resolution of 256 pixels (8
 * bits). The mapping in this case would simply be a truncation of the low order
 * 4 bits" (X3.110 Appendix B; US 4,454,593 FIG. 10 truncates the pel's near
 * corner the same way). A list that names the grid it was resolved against
 * therefore has each point it plots at the corner of the grid pixel the point
 * falls in, and the pel laid from there covers that pixel, as both receivers of
 * the day cover it. Points on the grid stay where they are; the slack only
 * absorbs the arithmetic that put them there.
 */
CataloguePoint onto_grid(const CataloguePoint& point,
                         const CatalogueDisplayList& list) {
  constexpr double kSlack = 1e-9;
  const double pitch_x = 1.0 / list.nominal_width;
  const double pitch_y = list.aspect_height / list.nominal_height;
  return CataloguePoint{std::floor(point.x / pitch_x + kSlack) * pitch_x,
                        std::floor(point.y / pitch_y + kSlack) * pitch_y};
}

/// Whether the points of |op| are plotted on |list|'s grid (onto_grid()): the
/// points, lines, rectangles and polygons of a list that names a grid and is
/// not that grid's pixels already. Arcs and splines keep their exact control
/// points, so a curve stays the curve; characters are laid in their fields.
bool plots_on_grid(const CatalogueDrawOp& op,
                   const CatalogueDisplayList& list) {
  if (list.pixel_aligned || !names_grid(list) || !(list.aspect_height > 0.0)) {
    return false;
  }
  switch (op.kind) {
    case CatalogueDrawKind::kPoint:
    case CatalogueDrawKind::kLine:
    case CatalogueDrawKind::kRectangle:
    case CatalogueDrawKind::kPolygon:
      return true;
    default:
      return false;
  }
}

/**
 * @brief The part of |area| a sweep standing at unit y |line| has revealed of
 *        |op|, in device space
 *
 * Above the line for a sweep down the screen and below it for one up
 * (CatalogueDrawOp::reveal_from_y). A list of the receiver's own pixels lays
 * the line on a whole device row, rounded as its runs' edges are, so a row of
 * them is either shown or not.
 */
QRectF revealed_part(const CatalogueDrawOp& op, double line, const QRectF& area,
                     qreal unit_y, bool pixel_aligned) {
  qreal y = area.bottom() - line * unit_y;
  if (pixel_aligned) {
    y = std::round(y);
  }
  y = std::clamp(y, area.top(), area.bottom());
  if (op.reveal_from_y >= op.reveal_to_y) {
    return QRectF(QPointF(area.left(), area.top()), QPointF(area.right(), y));
  }
  return QRectF(QPointF(area.left(), y), QPointF(area.right(), area.bottom()));
}

/// Whether |code_point| rides on the character before it rather than taking a
/// character field of its own.
bool is_combining(char32_t code_point) {
  const QChar::Category category = QChar::category(code_point);
  return category == QChar::Mark_NonSpacing ||
         category == QChar::Mark_Enclosing;
}

/// A text run's characters, one per character field: a base character with
/// whatever combining marks follow it. A nonspacing character does not advance
/// the cursor (§5.2), so it shares its base character's field.
QStringList character_cells(const std::string& text) {
  QStringList cells;
  QString cell;
  for (const char32_t code_point : QString::fromStdString(text).toUcs4()) {
    if (!cell.isEmpty() && !is_combining(code_point)) {
      cells << cell;
      cell.clear();
    }
    cell += QString::fromUcs4(&code_point, 1);
  }
  if (!cell.isEmpty()) {
    cells << cell;
  }
  return cells;
}

}  // namespace

/**
 * @brief Character outlines in the display font, fitted to character fields
 *
 * "The font and position of alphanumeric text characters within the character
 * field are implementation-dependent. Each such character shall be completely
 * contained within the area defined by the current character field"
 * (§5.3.2.3.9). A receiver deposits a stored pattern over the whole field, so a
 * field of any shape is filled by it; here the font's own character cell — its
 * pitch across, and its ascent and descent either side of the baseline — is
 * stretched onto the field in the same way, rather than the glyph keeping the
 * font's proportions and its advance.
 *
 * Outlines are laid out once, at a reference size, and kept.
 */
struct CatalogueDisplayListWidget::GlyphOutlines {
  struct Outline {
    /// The character at the reference size, its cell's left edge at x 0 and
    /// its baseline at y 0.
    QPainterPath path;
    /// How wide a cell the outline is laid out in.
    qreal advance = 0.0;
    /// No face can draw the base character.
    bool missing = false;
  };

  GlyphOutlines()
      : font(reference_font()),
        metrics(font),
        pitch(std::max(metrics.horizontalAdvance(QLatin1Char('M')), 1.0)),
        ascent(metrics.ascent()),
        descent(metrics.descent()) {}

  /// The reference face: the platform's fixed-pitch font, as outlines, which
  /// scale to any field where a bitmap face would have to be resampled.
  static QFont reference_font() {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    font.setPixelSize(kGlyphReferencePixels);
    font.setStyleStrategy(QFont::ForceOutline);
    return font;
  }

  const Outline& outline(const QString& cell) {
    const auto found = outlines.constFind(cell);
    if (found != outlines.constEnd()) {
      return *found;
    }
    return *outlines.insert(cell, lay_out(cell));
  }

  Outline lay_out(const QString& cell) const {
    Outline out;
    out.advance = pitch;
    const QList<uint> code_points = cell.toUcs4();
    if (code_points.isEmpty()) {
      return out;
    }
    if (!metrics.inFontUcs4(code_points.front())) {
      out.missing = true;
      return out;
    }
    // A combining mark no face has is left off: the base character alone is
    // "an approximate form" of the composition (§8.3.3(3)), where the platform
    // would put a box over it.
    QString drawable;
    for (const uint code_point : code_points) {
      if (drawable.isEmpty() || metrics.inFontUcs4(code_point)) {
        const char32_t character = code_point;
        drawable += QString::fromUcs4(&character, 1);
      }
    }
    out.path.addText(QPointF(0.0, 0.0), font, drawable);
    // A character wider than the pitch — one a fallback face supplied — is
    // measured by its own width, so it too stays inside its field.
    out.advance = std::max(pitch, metrics.horizontalAdvance(drawable));
    return out;
  }

  /**
   * @brief |cell| stretched onto a field |width| by |height|, in the field's
   *        own frame (see field_transform())
   *
   * A character no face can draw is a box: §8.3.3(3) has a conforming receiver
   * present every character of the repertoire "exactly or in an approximate
   * form or as a symbol(s) that cannot be confused with any character in the
   * graphic character repertoire", and a blank field is none of those.
   */
  QPainterPath fitted(const QString& cell, qreal width, qreal height) {
    const Outline& found = outline(cell);
    if (found.missing) {
      const QRectF box(width * kReplacementInsetAcross,
                       -height + height * kReplacementInsetDown,
                       width * (1.0 - 2.0 * kReplacementInsetAcross),
                       height * (1.0 - 2.0 * kReplacementInsetDown));
      return ring(
          box, std::max(kMinimumPenWidth, std::min(box.width(), box.height()) *
                                              kReplacementStroke));
    }
    if (found.path.isEmpty()) {
      return {};  // SPACE, and anything else that draws nothing
    }
    const qreal scale_x = width / found.advance;
    const qreal scale_y = height / std::max(ascent + descent, 1.0);
    QTransform fit;
    // The cell's top, the ascent above the baseline, lands on the field's top
    // and its bottom, the descent below it, on the field's bottom.
    fit.translate(0.0, -height + ascent * scale_y);
    fit.scale(scale_x, scale_y);
    return fit.map(found.path);
  }

  QFont font;
  QFontMetricsF metrics;
  qreal pitch = 0.0;
  qreal ascent = 0.0;
  qreal descent = 0.0;
  QHash<QString, Outline> outlines;
};

CatalogueDisplayListWidget::CatalogueDisplayListWidget(QWidget* parent)
    : CatalogueDisplayListWidget(nullptr, parent) {}

CatalogueDisplayListWidget::CatalogueDisplayListWidget(
    std::shared_ptr<const ICatalogueAnimationClock> clock, QWidget* parent)
    : QWidget(parent),
      placeholder_(tr("Nothing to show")),
      playback_(std::move(clock)) {
  setAutoFillBackground(false);
  setMinimumSize(sizeHint() / 2);
  // Clicked or tabbed to, the view takes the transport keys; until then the
  // keys belong to whatever the reader was using, the item list included.
  setFocusPolicy(Qt::StrongFocus);

  // One shot per change, armed afresh at each wake for exactly as long as the
  // next change is away. Precise rather than Qt's default coarse timer, which
  // may fire as much as 5% of the interval away from when it was asked to: the
  // list names its times to the millisecond.
  wake_timer_.setSingleShot(true);
  wake_timer_.setTimerType(Qt::PreciseTimer);
  connect(&wake_timer_, &QTimer::timeout, this,
          &CatalogueDisplayListWidget::onWake);
}

CatalogueDisplayListWidget::~CatalogueDisplayListWidget() = default;

void CatalogueDisplayListWidget::setDisplayList(
    const orc::CatalogueDisplayList& list) {
  list_ = list;
  timeline_ = CataloguePresentationTimeline(*list_);
  playback_.load(timeline_);
  transportChanged();
}

void CatalogueDisplayListWidget::clearDisplayList() {
  list_.reset();
  timeline_ = CataloguePresentationTimeline();
  ops_painted_ = 0;
  playback_.load(timeline_);
  transportChanged();
}

void CatalogueDisplayListWidget::setAnimationsEnabled(bool enabled) {
  if (animations_enabled_ == enabled) {
    return;
  }
  animations_enabled_ = enabled;
  if (enabled) {
    // Picking the presentation up wherever the clock had got to while it was
    // held still would drop the reader into the middle of it.
    playback_.restart();
  }
  transportChanged();
}

void CatalogueDisplayListWidget::replay() {
  playback_.restart();
  transportChanged();
}

void CatalogueDisplayListWidget::play() {
  playback_.play();
  transportChanged();
}

void CatalogueDisplayListWidget::pause() {
  playback_.pause();
  transportChanged();
}

void CatalogueDisplayListWidget::togglePlayback() {
  if (isPlaying()) {
    pause();
  } else {
    play();
  }
}

void CatalogueDisplayListWidget::seek(uint32_t time_ms) {
  playback_.seek(time_ms);
  transportChanged();
}

void CatalogueDisplayListWidget::stepDisplay(int direction) {
  if (!isTimed()) {
    return;
  }
  // As a sign steps a frame at a time: to the start of the display beside the
  // one on screen, held there. Past the last display there is nowhere to go,
  // so the step goes back to the start of the one on screen.
  const uint32_t shown = timeline_.changeAtOrBefore(presentationTime());
  uint32_t to = shown;
  if (direction > 0) {
    const std::optional<uint32_t> next = timeline_.nextChangeAfter(shown);
    if (next && *next < playback_.length()) {
      to = *next;
    }
  } else if (direction < 0 && shown > 0) {
    to = timeline_.changeAtOrBefore(shown - 1);
  }
  playback_.pause();
  playback_.seek(to);
  transportChanged();
}

void CatalogueDisplayListWidget::setPlaybackSpeed(double speed) {
  playback_.setSpeed(speed);
  transportChanged();
}

void CatalogueDisplayListWidget::setLooping(bool looping) {
  playback_.setLooping(looping);
  transportChanged();
}

uint32_t CatalogueDisplayListWidget::presentationLength() const {
  return isTimed() ? playback_.length() : 0;
}

void CatalogueDisplayListWidget::setPresentationTime(uint32_t time_ms) {
  playback_.pause();
  playback_.seek(time_ms);
  transportChanged();
}

uint32_t CatalogueDisplayListWidget::presentationTime() const {
  return playback_.time();
}

bool CatalogueDisplayListWidget::isPlaying() const {
  return animations_enabled_ && isTimed() && playback_.playing();
}

bool CatalogueDisplayListWidget::handlePlaybackKey(const QKeyEvent& event) {
  if (!animations_enabled_ || !isTimed()) {
    return false;
  }
  const bool shift = event.modifiers().testFlag(Qt::ShiftModifier);
  switch (event.key()) {
    case Qt::Key_Space:
      togglePlayback();
      return true;
    case Qt::Key_Left:
      if (shift) {
        const uint32_t now = presentationTime();
        seek(now > kSeekStepMs ? now - kSeekStepMs : 0);
      } else {
        stepDisplay(-1);
      }
      return true;
    case Qt::Key_Right:
      if (shift) {
        seek(static_cast<uint32_t>(std::min<uint64_t>(
            uint64_t{presentationTime()} + kSeekStepMs, presentationLength())));
      } else {
        stepDisplay(1);
      }
      return true;
    default:
      return false;
  }
}

void CatalogueDisplayListWidget::keyPressEvent(QKeyEvent* event) {
  if (handlePlaybackKey(*event)) {
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

void CatalogueDisplayListWidget::transportChanged() {
  schedulePlayback();
  update();
  Q_EMIT playbackChanged();
}

std::optional<int> CatalogueDisplayListWidget::scheduledWakeMs() const {
  if (!wake_timer_.isActive()) {
    return std::nullopt;
  }
  return wake_timer_.interval();
}

void CatalogueDisplayListWidget::fireScheduledWake() { onWake(); }

void CatalogueDisplayListWidget::schedulePlayback() {
  wake_timer_.stop();
  if (!shown_ || !isPlaying()) {
    return;
  }
  // The time is read from the clock at every wake rather than advanced by the
  // interval the timer was armed with. A wake that comes late delays only the
  // change it was armed for, and one that comes early re-arms for what is
  // left; neither error carries into the next change.
  const uint32_t now = presentationTime();
  std::optional<uint32_t> next = timeline_.nextChangeAfter(now);
  // Going round again — a loop back to the start, or a blink's cycle — is a
  // change too, and may come first.
  const std::optional<uint32_t> jump = playback_.nextJumpBack();
  if (jump && (!next || *jump < *next)) {
    next = jump;
  }
  if (!next) {
    return;  // settled: nothing on screen changes again
  }
  wake_timer_.start(static_cast<int>(
      std::min<int64_t>(playback_.clockMsFor(*next - now), kLongestWakeMs)));
}

void CatalogueDisplayListWidget::onWake() {
  // A display list places its operations anywhere in the drawable area and
  // they overlap freely, so there is no dirty rectangle worth deriving: the
  // area is repainted whole.
  update();
  schedulePlayback();
}

void CatalogueDisplayListWidget::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  shown_ = true;
  schedulePlayback();
}

void CatalogueDisplayListWidget::hideEvent(QHideEvent* event) {
  QWidget::hideEvent(event);
  // Nobody is watching, so nothing wakes. The presentation time is read from
  // the clock, so it runs on regardless and a view shown again picks up where
  // it has got to.
  shown_ = false;
  schedulePlayback();
}

void CatalogueDisplayListWidget::setShowDataErrors(bool show) {
  if (show_data_errors_ == show) {
    return;
  }
  show_data_errors_ = show;
  update();
}

void CatalogueDisplayListWidget::setPlaceholderText(const QString& text) {
  placeholder_ = text;
  update();
}

QSize CatalogueDisplayListWidget::sizeHint() const {
  return QSize(kNominalPixelsAcross, kNominalPixelsDown);
}

QRectF CatalogueDisplayListWidget::displayAreaRectIn(
    const QSizeF& bounds) const {
  // The area takes the shape the list is displayed at, which is only the extent
  // of unit y it covers when its nominal pixels are square.
  const double aspect_height = list_ ? shown_aspect(*list_) : 1.0;
  const qreal available_w = bounds.width();
  const qreal available_h = bounds.height();
  qreal draw_w = available_w;
  qreal draw_h = draw_w * aspect_height;
  if (draw_h > available_h) {
    draw_h = available_h;
    draw_w = draw_h / aspect_height;
  }
  return QRectF((available_w - draw_w) / 2.0, (available_h - draw_h) / 2.0,
                draw_w, draw_h);
}

std::optional<uint32_t> CatalogueDisplayListWidget::shownTime() const {
  if (!animations_enabled_ || !isTimed()) {
    return std::nullopt;
  }
  return presentationTime();
}

void CatalogueDisplayListWidget::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  // The time is read at paint rather than kept from the last wake, so a
  // repaint for any other reason — a resize, an expose — draws the moment it
  // happens at.
  paintContent(painter, QRectF(rect()), shownTime());
}

QSize CatalogueDisplayListWidget::pageImageSize() const {
  if (!list_) {
    return {};
  }
  const double aspect_height = shown_aspect(*list_);

  // A list that names the grid it was resolved against is saved at the size
  // every such list is saved at, whatever that grid was; one that names none
  // has nothing to line up with and keeps the plain width.
  const int across =
      names_grid(*list_) ? kSavedGridPixelsAcross : kSavedPixelsAcross;

  return QSize(
      across,
      std::max(1, static_cast<int>(std::lround(across * aspect_height))));
}

CatalogueDisplayListWidget::Mapping CatalogueDisplayListWidget::mappingFor(
    const QRectF& area) const {
  Mapping mapping;
  if (!list_) {
    return mapping;
  }
  mapping.unit_x = area.width();
  // Unit y runs 0 to its extent over the area's full height, so a unit of y is
  // that many device pixels — the same as unit_x only when the area's shape
  // and the list's extent agree, which is to say when its pixels are square.
  mapping.unit_y = area.height() / unit_aspect(*list_);
  // The pel is never narrower than one pixel of the list's own grid on each
  // axis: the grid pixel is not square, so each axis takes its own. Without a
  // grid, the floor is one device pixel.
  if (names_grid(*list_)) {
    mapping.pel_floor_x = pelFloor(area.width(), list_->nominal_width);
    mapping.pel_floor_y = pelFloor(area.height(), list_->nominal_height);
  }
  return mapping;
}

QImage CatalogueDisplayListWidget::renderPageImage(const QSize& size) const {
  return renderImage(size, std::nullopt);
}

QImage CatalogueDisplayListWidget::renderFrameImage(const QSize& size,
                                                    uint32_t time_ms) const {
  return renderImage(size, time_ms);
}

QByteArray CatalogueDisplayListWidget::renderSvgDocument(bool animated) const {
  if (!list_) {
    return {};
  }
  const QSizeF size(pageImageSize());
  const QRectF area = displayAreaRectIn(size);
  const Mapping mapping = mappingFor(area);
  if (!glyph_outlines_) {
    glyph_outlines_ = std::make_unique<GlyphOutlines>();
  }
  // Every operation, drawn in its colours as sent: the document itself follows
  // the palette entries through the presentation, and shows each stage from
  // the moment it appears — swept on, where its source drew it a row at a
  // time, through the same positions of the sweep a view shows.
  orc::gui::SvgDocumentCanvas canvas(size, area, *list_, animated);
  std::vector<orc::gui::SvgDocumentCanvas::RevealFrame> frames;
  const CatalogueDrawOp* framed = nullptr;
  for (const auto& op : list_->ops) {
    if (animated && op.reveal_duration_ms > 0) {
      // The runs of one swept figure share its sweep, so it is worked out once.
      if (framed == nullptr || framed->appear_ms != op.appear_ms ||
          framed->reveal_duration_ms != op.reveal_duration_ms ||
          framed->reveal_from_y != op.reveal_from_y ||
          framed->reveal_to_y != op.reveal_to_y) {
        frames.clear();
        for (const auto& step : timeline_.revealSteps(op)) {
          frames.push_back(orc::gui::SvgDocumentCanvas::RevealFrame{
              step.time_ms, revealed_part(op, step.line_y, area, mapping.unit_y,
                                          list_->pixel_aligned)});
        }
        framed = &op;
      }
      canvas.beginStage(op.appear_ms, frames);
    } else {
      canvas.beginStage(op.appear_ms);
    }
    paintOp(canvas, *list_, op, area, mapping, timeline_, std::nullopt,
            *glyph_outlines_);
  }
  if (show_data_errors_) {
    canvas.outline(area, kDisplayAreaOutline);
  }
  return canvas.document();
}

QImage CatalogueDisplayListWidget::renderImage(
    const QSize& size, std::optional<uint32_t> time_ms) const {
  if (!list_ || size.isEmpty()) {
    return {};
  }
  QImage image(size, QImage::Format_RGB32);
  image.fill(Qt::black);
  QPainter painter(&image);
  paintContent(painter, QRectF(QPointF(0.0, 0.0), QSizeF(size)), time_ms);
  return image;
}

void CatalogueDisplayListWidget::paintContent(
    QPainter& painter, const QRectF& bounds,
    std::optional<uint32_t> time_ms) const {
  painter.fillRect(bounds, Qt::black);
  ops_painted_ = 0;

  if (!list_) {
    painter.setPen(Qt::gray);
    painter.drawText(bounds, Qt::AlignCenter, placeholder_);
    return;
  }

  const QRectF area = displayAreaRectIn(bounds.size());
  const Mapping mapping = mappingFor(area);

  // A list that tiles its own pixel grid is drawn hard-edged: smoothing would
  // blur pixels a receiver had sharp, and blending both sides of the boundary
  // where two runs abut leaves a seam along a join that should be invisible.
  painter.setRenderHint(QPainter::Antialiasing, !list_->pixel_aligned);
  painter.save();
  // Nothing outside the drawable area is guaranteed visible on a receiver, so
  // nothing outside it is drawn here either.
  painter.setClipRect(area);

  if (!glyph_outlines_) {
    glyph_outlines_ = std::make_unique<GlyphOutlines>();
  }
  orc::gui::PainterCanvas canvas(painter);
  for (const auto& op : list_->ops) {
    // A moment of the presentation shows the operations that have appeared by
    // then; the still render shows them all.
    if (time_ms && !CataloguePresentationTimeline::drawnAt(op, *time_ms)) {
      continue;
    }
    // One being swept onto the screen shows the rows the sweep has passed.
    const std::optional<double> line =
        time_ms ? timeline_.revealLineAt(op, *time_ms) : std::nullopt;
    if (line) {
      painter.save();
      painter.setClipRect(
          revealed_part(op, *line, area, mapping.unit_y, list_->pixel_aligned),
          Qt::IntersectClip);
      paintOp(canvas, *list_, op, area, mapping, timeline_, time_ms,
              *glyph_outlines_);
      painter.restore();
    } else {
      paintOp(canvas, *list_, op, area, mapping, timeline_, time_ms,
              *glyph_outlines_);
    }
    ++ops_painted_;
  }
  painter.restore();

  if (show_data_errors_) {
    // The drawable area's edge, so a list drawn entirely in one corner is
    // visibly in a corner rather than looking mis-scaled.
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setPen(QPen(kDisplayAreaOutline, 1, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(area.adjusted(0, 0, -1, -1));
  }
}

void CatalogueDisplayListWidget::paintOp(
    DisplayListCanvas& canvas, const CatalogueDisplayList& list,
    const CatalogueDrawOp& op, const QRectF& area, const Mapping& mapping,
    const CataloguePresentationTimeline& timeline,
    std::optional<uint32_t> time_ms, GlyphOutlines& glyphs) {
  const qreal unit_x = mapping.unit_x;
  const qreal unit_y = mapping.unit_y;

  // Unit space has y upwards from the bottom left; the widget's y runs down.
  const auto map = [&area, unit_x, unit_y](const CataloguePoint& point) {
    return QPointF(area.left() + point.x * unit_x,
                   area.bottom() - point.y * unit_y);
  };
  const auto device = [&map](const std::vector<CataloguePoint>& points) {
    QVector<QPointF> out;
    out.reserve(static_cast<int>(points.size()));
    for (const CataloguePoint& point : points) {
      out.push_back(map(point));
    }
    return out;
  };

  // A colour that names a palette entry follows the entry through the
  // presentation, which is how a blink reaches every figure drawn in the
  // blinking entry and nothing else. The still render takes each colour's own
  // RGB, the entry at rest.
  const auto shown = [&timeline, time_ms](const CatalogueColour& value) {
    return orc::gui::shown_colour(time_ms ? timeline.colourAt(value, *time_ms)
                                          : value);
  };
  // A fill in one colour: the colour at the moment drawn, and the colour as
  // sent, which is what an exported presentation follows.
  const auto paint_of = [&shown](const CatalogueColour& value) {
    return CanvasPaint{QBrush(shown(value)), value, std::nullopt};
  };
  const QColor colour = shown(op.colour);
  const QColor background =
      op.has_background ? shown(op.background) : QColor(0, 0, 0);
  // Colour mode 2 is the one with a background, and it is what fills the
  // spaces a texture leaves (§5.3.2.4.2, §5.3.2.4.4).
  const QColor* ground = op.has_background ? &background : nullptr;
  const CanvasPaint colour_paint = paint_of(op.colour);
  const std::optional<CanvasPaint> background_paint =
      op.has_background ? std::optional<CanvasPaint>(paint_of(op.background))
                        : std::nullopt;
  // §5.3.2.4.3: a highlight is "in nominal black in color modes 0 and 1, and in
  // the background color in color mode 2".
  const CanvasPaint highlight =
      background_paint ? *background_paint : paint_of(CatalogueColour{});

  // The pel's own extent in device pixels, and the brush it is drawn as.
  const qreal pel_w = std::fabs(op.pen_size.dx) * unit_x;
  const qreal pel_h = std::fabs(op.pen_size.dy) * unit_y;
  const PelBrush pel = orc::gui::pel_brush(
      op.pen_size, unit_x, unit_y, mapping.pel_floor_x, mapping.pel_floor_y);
  // §6.2.7.15: the underline "appears in the character field starting at the
  // character field origin and extending across the entire width (dx
  // dimension) of the character field", and "its thickness is determined by
  // the vertical dimension (dy) of the logical pel size". It is part of the
  // character's pattern, so it turns and reflects with it.
  const auto underlined = [&op, &mapping, pel_h](QPainterPath shape,
                                                 qreal field_w) {
    if (op.underlined) {
      const qreal underline = std::max(pel_h, mapping.pel_floor_y);
      QPainterPath line;
      line.addRect(QRectF(0.0, -underline, field_w, underline));
      shape = shape.united(line);
    }
    return shape;
  };

  LineInk ink;
  ink.pel = pel;
  ink.pel_width = pel_w;
  ink.pel_height = pel_h;
  ink.texture = op.line_style;
  ink.colour = colour_paint;
  ink.gaps = background_paint;

  const auto fill_brush = [&]() {
    return CanvasPaint{
        texture_brush(op, list, colour, ground, area, unit_x, unit_y),
        op.colour,
        op.has_background ? std::optional<CatalogueColour>(op.background)
                          : std::nullopt};
  };
  // A filled figure: the fill, then §5.3.2.4.3's highlight swept over its
  // |outline| where the figure is outlined, round it where |closed|.
  const auto fill_highlighted = [&](const QVector<QPointF>& outline,
                                    const QVector<QPointF>& plotted,
                                    bool closed) {
    fill_figure(canvas, outline, plotted, fill_brush(), pel);
    if (op.outlined) {
      fill_swept(canvas, solid_pieces(outline, closed), plotted, pel,
                 highlight);
    }
  };

  // The record's points, plotted on the receiver's grid where the list names
  // one (onto_grid()).
  const bool on_grid = plots_on_grid(op, list);
  std::vector<CataloguePoint> plotted_points = op.points;
  if (on_grid) {
    for (CataloguePoint& point : plotted_points) {
      point = onto_grid(point, list);
    }
    // The grid they are plotted on, which a dotted or dashed line is
    // registered to as the receiver registers it.
    ink.grid = orc::gui::ReceiverGrid{area.bottomLeft(),
                                      area.width() / list.nominal_width,
                                      area.height() / list.nominal_height};
  }
  const QVector<QPointF> points = device(plotted_points);

  switch (op.kind) {
    case CatalogueDrawKind::kPoint: {
      // §5.3.3.1.4: a point "whose size is determined by the logical pel
      // size" — the pel itself, laid down once at the drawing point.
      if (!points.isEmpty()) {
        canvas.fillRect(pel.at(points.front()), colour_paint,
                        /*hard_edged=*/false);
      }
      return;
    }

    case CatalogueDrawKind::kLine: {
      stroke_line(canvas, points, /*closed=*/false, points, ink);
      return;
    }

    case CatalogueDrawKind::kArc: {
      if (op.points.empty()) {
        return;
      }
      // Flattened in unit space, where the record's circle is a circle, and
      // only then mapped to the device.
      const QVector<QPointF> outline = device(orc::gui::arc_polyline(
          op.points, kArcTolerancePixels / std::max({unit_x, unit_y, 1e-9})));
      // What §5.3.2.4.2 has plotted whatever the texture: an arc's start and
      // end point, and every point of a spline's series of lines — not the
      // vertices that only flatten a curve.
      const QVector<QPointF> plotted =
          op.points.size() == 3
              ? QVector<QPointF>{points.front(), points.back()}
              : points;
      if (!op.filled) {
        stroke_line(canvas, outline, /*closed=*/false, plotted, ink);
        return;
      }
      // §5.3.3.3: "the area enclosed by the outline and the chord (including
      // the region of the outline and the chord traced by the logical pel)".
      // It keeps the chord out of the highlight — "the chord is not considered
      // a part of the arc and, as such, is not highlighted" — so the open arc
      // is swept, not the closure.
      fill_highlighted(outline, plotted, /*closed=*/false);
      return;
    }

    case CatalogueDrawKind::kRectangle: {
      if (list.pixel_aligned && op.filled) {
        // A pixel list's rectangles are runs of the receiver's own pixels,
        // exactly as large as they say: nothing is traced round them, and
        // both edges are rounded the same way, so a run and the one beside
        // it land on the same device column and meet without a gap or an
        // overlap.
        const QPointF origin = map(op.origin);
        const QPointF far(origin.x() + op.size.dx * unit_x,
                          origin.y() - op.size.dy * unit_y);
        const QRectF box = QRectF(origin, far).normalized();
        const CanvasPaint fill = fill_brush();
        if (fill.brush.style() != Qt::NoBrush) {
          canvas.fillRect(
              QRectF(
                  QPointF(std::round(box.left()), std::round(box.top())),
                  QPointF(std::round(box.right()), std::round(box.bottom()))),
              fill, /*hard_edged=*/false);
        }
        return;
      }
      // §5.3.3.4.1: from the start point by dx and dy, either of which may be
      // negative, so the corners are taken in order from the origin — both
      // plotted on the grid where the list names one.
      CataloguePoint start = op.origin;
      CataloguePoint far{op.origin.x + op.size.dx, op.origin.y + op.size.dy};
      if (on_grid) {
        start = onto_grid(start, list);
        far = onto_grid(far, list);
      }
      const QVector<QPointF> corners =
          device({start, CataloguePoint{far.x, start.y}, far,
                  CataloguePoint{start.x, far.y}});
      if (!op.filled) {
        // "Outlined rectangles ... have a line width that is determined by the
        // logical pel size and a line texture that is specified by the TEXTURE
        // command" — dotted and dashed like any line.
        stroke_line(canvas, corners, /*closed=*/true, corners, ink);
        return;
      }
      fill_highlighted(corners, corners, /*closed=*/true);
      return;
    }

    case CatalogueDrawKind::kPolygon: {
      if (points.isEmpty()) {
        return;
      }
      if (!op.filled) {
        // §5.3.3.5.1: "There is implicit closure between the start point and
        // the last vertex specified", and the texture runs on round the
        // closing edge.
        stroke_line(canvas, points, /*closed=*/true, points, ink);
        return;
      }
      fill_highlighted(points, points, /*closed=*/true);
      return;
    }

    case CatalogueDrawKind::kColourRun: {
      if (op.colour_run.empty()) {
        return;
      }
      // §5.3.3.6.3: the colours are deposited raster-sequentially across the
      // field, one pel apiece, wrapping at its right edge. The field's origin
      // may be any of its corners, either dimension being signed.
      const int count = static_cast<int>(op.colour_run.size());
      const double width = std::fabs(op.size.dx);
      const double height = std::fabs(op.size.dy);
      const double pel_x = std::fabs(op.pen_size.dx);
      const double pel_y = std::fabs(op.pen_size.dy);
      // How many pels the field holds across, counted with the pel's own
      // width. A field one row high therefore carries exactly its row, and
      // the step across is the field's width over its colours; a floor on the
      // drawn size must not change that, or a row of fine pels would wrap.
      const int columns =
          pel_x > 0.0 && width > 0.0
              ? std::max(1, static_cast<int>(std::lround(width / pel_x)))
              : count;
      const int rows = (count + columns - 1) / columns;
      const qreal step_x = width > 0.0 ? width * unit_x / columns
                                       : std::max(pel_w, mapping.pel_floor_x);
      const qreal step_y = pel_y > 0.0    ? pel_h
                           : height > 0.0 ? height * unit_y / rows
                                          : mapping.pel_floor_y;
      // Every pel is drawn at least a floor across and down, so one finer
      // than the drawing is still seen, from where its own step puts it.
      const qreal drawn_w = std::max(step_x, mapping.pel_floor_x);
      const qreal drawn_h = std::max(step_y, mapping.pel_floor_y);
      const qreal left =
          area.left() +
          std::min(op.origin.x, op.origin.x + op.size.dx) * unit_x;
      const qreal top =
          area.bottom() -
          (std::min(op.origin.y, op.origin.y + op.size.dy) + height) * unit_y;

      // A raster is pixels: hard-edged, with both edges of every pel rounded
      // the same way, so neighbours meet without a gap or an overlap and a
      // stretch of one colour shows no seams.
      for (int i = 0; i < count; ++i) {
        const int row = i / columns;
        const qreal x = left + (i % columns) * step_x;
        const qreal y = top + row * step_y;
        QRectF rect(QPointF(std::round(x), std::round(y)),
                    QPointF(std::round(x + drawn_w), std::round(y + drawn_h)));
        rect.setWidth(std::max<qreal>(rect.width(), 1.0));
        rect.setHeight(std::max<qreal>(rect.height(), 1.0));
        // A run carries its own colour per pel, each of which may name an
        // entry of its own, so each is followed through the presentation
        // rather than |colour|.
        canvas.fillRect(rect, paint_of(op.colour_run[static_cast<size_t>(i)]),
                        /*hard_edged=*/true);
      }
      return;
    }

    case CatalogueDrawKind::kText: {
      const qreal field_w = std::fabs(op.size.dx) * unit_x;
      const qreal field_h = std::fabs(op.size.dy) * unit_y;
      const QStringList cells = character_cells(op.text);
      if (!(field_w > 0.0) || !(field_h > 0.0) || cells.isEmpty()) {
        return;
      }
      const QRectF field(0.0, -field_h, field_w, field_h);

      QPainterPath fields;
      fields.setFillRule(Qt::WindingFill);
      QPainterPath shapes;
      shapes.setFillRule(Qt::WindingFill);
      for (int index = 0; index < cells.size(); ++index) {
        // Each character at its own field origin along the path, measured by
        // the run's advance rather than by the font: the source placed them.
        const QPointF origin =
            map(CataloguePoint{op.origin.x + index * op.advance.dx,
                               op.origin.y + index * op.advance.dy});
        const QTransform to_device =
            field_transform(origin, field_w, field_h, op.size.dx < 0.0,
                            op.size.dy < 0.0, op.rotation_degrees);
        fields.addPolygon(to_device.map(QPolygonF(field)));
        fields.closeSubpath();
        shapes.addPath(to_device.map(underlined(
            glyphs.fitted(cells[index], field_w, field_h), field_w)));
      }
      paint_characters(canvas, fields, shapes, colour_paint,
                       background_paint ? &*background_paint : nullptr,
                       op.reverse_video);
      return;
    }

    case CatalogueDrawKind::kMosaic:
    case CatalogueDrawKind::kGlyph: {
      const qreal field_w = std::fabs(op.size.dx) * unit_x;
      const qreal field_h = std::fabs(op.size.dy) * unit_y;
      if (!(field_w > 0.0) || !(field_h > 0.0)) {
        return;
      }
      const QTransform to_device =
          field_transform(map(op.origin), field_w, field_h, op.size.dx < 0.0,
                          op.size.dy < 0.0, op.rotation_degrees);
      QPainterPath fields;
      fields.addPolygon(
          to_device.map(QPolygonF(QRectF(0.0, -field_h, field_w, field_h))));
      fields.closeSubpath();

      QPainterPath shape;
      if (op.kind == CatalogueDrawKind::kMosaic) {
        shape = mosaic_shape(field_w, field_h, op.mosaic_pattern,
                             op.mosaic_separated,
                             std::max(pel_w, mapping.pel_floor_x),
                             std::max(pel_h, mapping.pel_floor_y));
      } else {
        if (op.glyph_index >= 0 &&
            op.glyph_index < static_cast<int>(list.glyphs.size())) {
          shape = glyph_shape(list.glyphs[static_cast<size_t>(op.glyph_index)],
                              field_w, field_h);
        }
        // §6.2.7.15 underlines DRCS characters as it does text; mosaics it
        // puts into separated mode instead.
        shape = underlined(shape, field_w);
      }
      // A DRCS character never defined is displayed as SPACE (§5.6): an empty
      // shape, so only its field is drawn — in the background, or in reverse
      // video in the drawing colour.
      paint_characters(canvas, fields, to_device.map(shape), colour_paint,
                       background_paint ? &*background_paint : nullptr,
                       op.reverse_video);
      return;
    }
  }
}
