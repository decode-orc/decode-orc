/*
 * File:        naplps_raster.h
 * Module:      nabts_sink stage plugin
 * Purpose:     Deposits NAPLPS drawing primitives into a receiver's pixel grid
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_SINK_NAPLPS_RASTER_H
#define ORC_NABTS_SINK_NAPLPS_RASTER_H

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "naplps_font.h"
#include "naplps_render_grid.h"
#include "vbi-services/nabts_page.h"

namespace orc {

/**
 * @brief One physical pixel of the receiver's display
 *
 * X3.110 §4.2.3 builds a picture in layers — "if a subsequent drawing command
 * or alphabetic character affects a given pixel of the display, it supersedes
 * the effect of any previous command on that pixel" — so a cell holds only what
 * was deposited last, not a history.
 *
 * The colour map address is carried alongside the resolved colour because a
 * receiver stores the address (§5.3.2.5), so a later change to the entry — a
 * map write, or a blink process (§5.3.2.7.2) — reaches every pixel that entry
 * coloured, wherever it was drawn from.
 */
struct NaplpsCell {
  /// Whether anything has been deposited here. An untouched cell is not black;
  /// it is nothing, and a renderer leaves the ground showing through it.
  bool painted = false;

  NabtsColour colour;
  int16_t colour_map_address = -1;
};

/// What one drawing operation deposits, which is everything a cell records
/// apart from the fact of having been painted at all.
struct NaplpsInk {
  NabtsColour colour;
  int16_t colour_map_address = -1;
};

/// Whether two cells would be drawn identically, which is what lets one run
/// stand for both. The colour map address is part of it: two cells of the
/// same colour drawn from different entries can change apart.
bool naplps_cells_match(const NaplpsCell& a, const NaplpsCell& b);

/// The ink an operation draws in, taken from the attributes the interpreter
/// resolved onto it.
NaplpsInk naplps_ink_of(const NabtsPrimitive& primitive);

/// The ink an operation's *background* fills in, for the colour mode that has
/// one (§5.3.2.6 mode 2).
NaplpsInk naplps_background_ink_of(const NabtsPrimitive& primitive);

/**
 * @brief The receiver's frame buffer, one cell per physical pixel
 *
 * Row 0 is the bottom, matching the unit screen's y and the element order of
 * the DRCS and texture-mask buffers, so nothing in the decoder has to think
 * about which way up a buffer is.
 *
 * The surface covers the display area alone — the lower @ref
 * kNabtsDisplayAreaHeight of the unit screen, which Table D1 item 10 is the
 * part a receiver guarantees is visible. A coordinate outside it is not an
 * error; it is simply not displayed, and is dropped.
 */
class NaplpsCellSurface {
 public:
  explicit NaplpsCellSurface(NaplpsRenderGrid grid);

  const NaplpsRenderGrid& grid() const { return grid_; }
  int width() const { return grid_.width; }
  int height() const { return grid_.height; }

  bool contains(int column, int row) const {
    return column >= 0 && column < grid_.width && row >= 0 &&
           row < grid_.height;
  }

  /// The cell at |column|, |row|, or an unpainted one for a position off the
  /// surface, so a reader never has to bounds-check before looking.
  const NaplpsCell& at(int column, int row) const;

  /// Lay |ink| into one cell, superseding whatever was there (§4.2.3). A
  /// position off the surface is dropped.
  void deposit(int column, int row, const NaplpsInk& ink);

  /// Set one cell to exactly |cell|, painted or not — which is what putting
  /// back a saved stretch of the surface needs. A position off the surface is
  /// dropped.
  void put(int column, int row, const NaplpsCell& cell);

  /// Cells that have been painted, which is what tells an empty page from a
  /// black one.
  size_t painted_count() const;

  /// Unpaint every cell: the display cleared (§6.1.2.6 CS, §5.3.2.9.2 RESET).
  void clear();

  /// Whether anything has been deposited in |row| since the last
  /// forget_dirty(), which is how a page drawn in stages finds what one stage
  /// changed without comparing the whole surface.
  bool row_dirty(int row) const {
    return row >= 0 && row < grid_.height &&
           dirty_rows_[static_cast<size_t>(row)] != 0;
  }
  void forget_dirty();

  /// Copy the rows of |other| that are dirty there into this surface, which
  /// covers the same grid.
  void copy_dirty_rows_from(const NaplpsCellSurface& other);

  /// Move the cells of columns [first_column, last_column) and rows
  /// [first_row, last_row) right by |columns| and up by |rows| (the other way
  /// where negative). Cells moved out of the area are lost, and the cells
  /// left behind take |fill|, or are unpainted where it is nullptr.
  void shift_area(int first_column, int last_column, int first_row,
                  int last_row, int columns, int rows,
                  const NaplpsInk* fill = nullptr);

 private:
  NaplpsRenderGrid grid_;
  std::vector<NaplpsCell> cells_;
  std::vector<uint8_t> dirty_rows_;
  NaplpsCell blank_;
};

/// A position in continuous cell space.
struct NaplpsCellPoint {
  double column = 0.0;
  double row = 0.0;
};

/// An extent in continuous cell space, which may be signed: the sign of a
/// NAPLPS size says which way it runs from its origin.
struct NaplpsCellSize {
  double columns = 0.0;
  double rows = 0.0;
};

/**
 * @brief Maps the unit screen onto a surface's cells
 *
 * Unit x runs 0 to 1 across the grid's width and unit y runs 0 to
 * @ref kNabtsDisplayAreaHeight up its height, so a cell is square in unit space
 * (see naplps_render_grid.h). Positions are continuous: a coordinate lands
 * partway through a cell, and which cells that covers is the drawing rule's
 * business rather than the mapping's.
 */
struct NaplpsGridMapping {
  NaplpsRenderGrid grid;
  /// Cells per unit of x, and per unit of y.
  double columns_per_unit = 0.0;
  double rows_per_unit = 0.0;

  /// A display: unit x over the grid's width, unit y over the display area.
  explicit NaplpsGridMapping(NaplpsRenderGrid target)
      : grid(target),
        columns_per_unit(target.width),
        rows_per_unit(1.0 / target.pitch_y()) {}

  /// Continuous cell-space x of a unit-screen x.
  double column_at(double x) const { return x * columns_per_unit; }
  /// Continuous cell-space y of a unit-screen y, counting up from the bottom.
  double row_at(double y) const { return y * rows_per_unit; }

  /// A unit-space width as a count of cells.
  double columns_across(double dx) const { return dx * columns_per_unit; }
  /// A unit-space height as a count of cells.
  double rows_up(double dy) const { return dy * rows_per_unit; }

  /// A unit-screen point and a unit-screen size in cell space, which is the
  /// form every drawing rule works in.
  NaplpsCellPoint cell_point(const NabtsPoint& point) const {
    return NaplpsCellPoint{column_at(point.x), row_at(point.y)};
  }
  NaplpsCellSize cell_size(const NabtsSize& size) const {
    return NaplpsCellSize{columns_across(size.dx), rows_up(size.dy)};
  }

  /// The unit-screen x and y of the centre of a cell, which is what a pattern
  /// anchored to the unit screen is sampled at.
  double centre_x(int column) const {
    return (column + 0.5) / columns_per_unit;
  }
  double centre_y(int row) const { return (row + 0.5) / rows_per_unit; }
};

/**
 * @brief An ARC's control points as a polyline that approximates it
 *
 * §5.3.3.3 reads a control-point run three ways. Three points are a circle when
 * the start and end coincide — the intermediate point being diametrically
 * opposite — a straight line when all three are colinear, and otherwise the
 * segment of the circle through all three. More than three points are a
 * curvilinear spline whose "minimum implementation shall be a series of lines
 * connecting the start, intermediate, and end points", which is what this
 * returns for them.
 *
 * @p tolerance_unit is the greatest distance the polyline may stray from the
 * true curve, in unit-screen terms; a caller passes a fraction of a cell so the
 * approximation cannot be seen on the grid it is drawn into.
 */
std::vector<NabtsPoint> naplps_arc_polyline(
    const std::vector<NabtsPoint>& control, double tolerance_unit);

/**
 * @brief Whether @p character's field is too small for it to be presented
 *
 * Table D1 item 2 has the primary characters "uniquely presented for character
 * field sizes greater than or equal to dx = 6/256, dy = 8/256", and asks
 * nothing of a smaller field. Both receivers of the day, driven through their
 * firmware, present no pattern there: the Norpak TTX 650 lights a single pixel
 * at the field's origin, the Panasonic TU-2000x nothing at all. A downloaded
 * character in a field smaller than its buffer (§6.2.3, @p drcs) they treat
 * the same way. Judged at the 256 x 200 grid, where they drew, so a character
 * is presented or not alike at every resolution. Mosaics are drawn at any
 * size; SPACE and the non-spacing marks, which a letter composes, are never
 * too small.
 */
bool naplps_character_too_small(const NabtsPrimitive& character,
                                const std::vector<NabtsDrcsCharacter>& drcs);

/**
 * @brief One run of identical cells along a row of the surface
 *
 * A rasterised page is emitted as runs rather than as a bitmap so that a
 * renderer can put a receiver's rectangular pixels on screen at any size and
 * keep their edges sharp. @ref columns is how many cells the run covers,
 * starting at @ref column on @ref row.
 */
struct NaplpsCellRun {
  int column = 0;
  int row = 0;
  int columns = 0;
  NaplpsCell cell;
};

/**
 * @brief The painted cells of @p surface as run-length merged runs
 *
 * Merged along a row only. Merging rectangles as well would cut the count
 * further on a page of flat colour, but a run per row keeps the emitted
 * geometry a plain function of the surface, which is what makes it testable
 * against the cells rather than against itself.
 *
 * Unpainted cells produce no run at all: a page does not cover the screen, and
 * emitting its ground would paint over whatever a renderer puts behind it.
 */
std::vector<NaplpsCellRun> naplps_merge_runs(const NaplpsCellSurface& surface);

/**
 * @brief The cells of @p now that differ from @p before, as runs
 *
 * Only the rows @p now marks dirty are looked at: what a stage of a page
 * drawn over time changed on top of what was already shown. A cell the stage
 * emptied — a scroll moving what was there away — comes out as an unpainted
 * cell, which a renderer draws black. Runs are merged along a row as
 * naplps_merge_runs() merges them, and a run never spans a cell the stage left
 * as it was.
 */
std::vector<NaplpsCellRun> naplps_changed_runs(const NaplpsCellSurface& now,
                                               const NaplpsCellSurface& before);

/**
 * @brief The cells a filled figure is worked out in, and its traced outline
 *
 * A rectangle of cell space with a bit per cell. It covers the figure's own
 * path and a margin round it, wherever that falls — including cells the
 * receiver does not show, so the outline is traced whole however far off the
 * screen the figure runs (NaplpsRasteriser::fill_path()). Only the depositing
 * at the end is clipped to what the receiver shows.
 */
struct NaplpsTracedRegion {
  int first_column = 0;
  int first_row = 0;
  int columns = 0;
  int rows = 0;
  std::vector<uint8_t> cells;

  size_t index_of(int column, int row) const {
    return static_cast<size_t>(row - first_row) * static_cast<size_t>(columns) +
           static_cast<size_t>(column - first_column);
  }
};

/**
 * @brief A stored character pattern of any size, read element by element
 *
 * The one form every pattern is deposited through: a face's pattern (rows of
 * bits, top row first, bit width - 1 the leftmost element) and a DRCS buffer
 * (one element per bool, row 0 at its foot). A DRCS buffer holds an element
 * for every pixel of the field it was defined in (§6.2.3, Table D1 item 8), so
 * at 512 by 400 and above even a normal field's buffer is wider than a packed
 * row could carry; read by element, its width is no limit.
 */
struct NaplpsGlyphView {
  int width = 0;
  int height = 0;
  const uint16_t* rows = nullptr;
  const std::vector<bool>* elements = nullptr;

  static NaplpsGlyphView of_rows(const uint16_t* rows, int width, int height) {
    NaplpsGlyphView view;
    view.width = width;
    view.height = height;
    view.rows = rows;
    return view;
  }

  static NaplpsGlyphView of_drcs(const NabtsDrcsCharacter& glyph) {
    NaplpsGlyphView view;
    if (glyph.defined()) {
      view.width = glyph.width;
      view.height = glyph.height;
      view.elements = &glyph.elements;
    }
    return view;
  }

  /// Whether the element |column| across and |row| down from the top is on.
  bool lit(int column, int row) const {
    if (column < 0 || column >= width || row < 0 || row >= height) {
      return false;
    }
    if (rows != nullptr) {
      return (rows[row] & (1u << (width - 1 - column))) != 0u;
    }
    return elements != nullptr &&
           (*elements)[static_cast<size_t>(height - 1 - row) *
                           static_cast<size_t>(width) +
                       static_cast<size_t>(column)];
  }
};

/**
 * @brief What a receiver draws for a character its faces hold no pattern for
 *
 * §8.3.3(3) has every remaining character of the repertoire presented "exactly
 * or in an approximate form or as a symbol(s) that cannot be confused with any
 * character in the graphic character repertoire". A hollow box the height of
 * a capital is none of the characters of Tables 18 to 27, and unlike a blank it
 * shows that something was sent. It is taken from @p face's own capital H, so
 * it stands on the face's baseline at the face's weight. The rows are in the
 * face's form: @p face's height of them, top first.
 */
std::vector<uint16_t> naplps_replacement_pattern(const NaplpsFontFace& face);

/**
 * @brief Draws NAPLPS primitives into a cell surface
 *
 * Every geometric primitive of §5.3.3 comes down to two operations on the grid:
 * sweeping the logical pel along a path, and filling an enclosed area. This
 * carries out both against the rules §5.3.2.2.6, §5.3.2.4.2 and §5.3.2.4.4
 * state in terms of the receiver's physical pixels — which is the whole reason
 * the grid has to be known before anything can be drawn.
 */
class NaplpsRasteriser {
 public:
  NaplpsRasteriser(NaplpsCellSurface& surface, NaplpsGridMapping mapping)
      : surface_(surface), mapping_(mapping) {}

  /**
   * @brief Lay the logical pel down once, with the drawing point at |point|
   *
   * §5.3.2.2.6: the deposit covers "all of those pixels that lie under any
   * portion of the logical pel as it is mapped to the display screen", and
   * "the logical pel, therefore, will always map to at least one and possibly
   * many display pixels" — so a dimensionless pel still paints the cell the
   * drawing point falls in.
   *
   * The pel's sign decides which of its corners the drawing point is: lower
   * left when both dimensions are positive, and the other three corners for the
   * other three sign combinations, which is what taking the rectangle between
   * the point and the point plus the pel gives.
   *
   * How many pixels that is comes from the pel's size alone, so a pel of a
   * given size is the same brush wherever it is put down — see @ref
   * NaplpsRasteriser::stamp_pel_cells.
   */
  void stamp_pel(const NabtsPoint& point, const NabtsSize& pel,
                 const NaplpsInk& ink);

  /**
   * @brief Sweep the pel along a path, in the given line texture
   *
   * "A LINE is a locus of points following a straight line algorithm between
   * two specified coordinates. The physical picture elements (pixels) through
   * which the infinitely small locus point passes would be drawn. The logical
   * pel specification allows the locus point to take on specific dimensions,
   * thereby acting as a larger brush" (§5.3.2.2.6). The locus is walked at one
   * cell per step along whichever axis is moving faster, which is the straight
   * line algorithm that leaves no gap and draws a line of the pel's weight
   * whichever way it runs.
   *
   * |closed| joins the last point back to the first, which is what an outlined
   * rectangle or polygon needs.
   *
   * |gaps| is what the gaps of a dotted or dashed texture are drawn in, or
   * nullptr to leave them: §5.3.2.4.2 has "the inter-dot and inter-dash
   * spacing ... drawn in the background color" in colour mode 2.
   *
   * §5.3.2.4.2 plots "all end points of lines and arcs and all vertices"
   * whatever the texture. |points_are_vertices| says whether the interior
   * points are vertices of the figure; the points an arc is flattened into
   * are not, and only its two ends are plotted.
   */
  void stroke_path(const std::vector<NabtsPoint>& points, const NabtsSize& pel,
                   NabtsLineTexture texture, const NaplpsInk& ink,
                   bool closed = false, const NaplpsInk* gaps = nullptr,
                   bool points_are_vertices = true);

  /**
   * @brief Fill the area a path encloses, plus the outline the pel traces
   *
   * §5.3.3.4.1, §5.3.3.5.1 and §5.3.3.3 all put "the region of the outline
   * traced by the logical pel" inside the filled area, so a filled figure is
   * its enclosed area *plus* a solid sweep of the pel around its outline. A
   * service that draws a letterform as a path enclosing almost nothing and lets
   * the pel give it weight depends on this.
   *
   * The outline is therefore traced first, by @ref stroke_path itself, and
   * the figure is that outline and every cell whose corner lies inside the
   * path by the odd-even rule §5.3.3.5.1 gives the enclosed area, row by row
   * between the path's crossings as the Norpak TTX 650 fills. Taking the edge
   * from the stroked outline is what settles a fill against the line a service
   * draws over it: the fill covers the cells the outline does, no more and no
   * fewer. A service that draws a map by filling each region and then
   * outlining it depends on both — a fill that overshot showed a coloured
   * fringe outside the coastline, and one that fell short left a seam down
   * every diagonal.
   *
   * |pattern| is applied through the texture rules of §5.3.2.4.4: a cell is
   * filled only where the pattern covers it, with the pattern registered
   * against the unit screen's origin so it lines up across figures. A
   * programmable mask that was never defined covers nothing — "a null texture
   * pattern, resulting in no fill in color modes 0 and 1".
   *
   * |gaps| is what the cells the pattern leaves are filled in, or nullptr to
   * leave them: "In color mode 2, the fill areas not drawn in the drawing color
   * are drawn in the background color" (§5.3.2.4.4), and an undefined mask is
   * "a fill with the background color in color mode 2".
   */
  void fill_path(const std::vector<NabtsPoint>& points, const NabtsSize& pel,
                 NabtsTexturePattern pattern, const NabtsSize& mask_size,
                 const NabtsTextureMask* mask, const NaplpsInk& ink,
                 const NaplpsInk* gaps = nullptr);

  /**
   * @brief Outline a filled figure in the highlight colour
   *
   * §5.3.2.4.3: the outline of a highlighted figure is drawn "with solid line
   * texture (independent of the current line texture) using the current logical
   * pel size", in nominal black in colour modes 0 and 1 and in the background
   * colour in mode 2.
   */
  void highlight_path(const std::vector<NabtsPoint>& points,
                      const NabtsSize& pel, const NaplpsInk& ink,
                      bool closed = true);

  /**
   * @brief Deposit one row of INCREMENTAL POINT colours (§5.3.3.6.3)
   *
   * Colour k goes under the pel with its drawing point at |start| plus k
   * pels along x, the pel's signs giving the direction. Before that the pel
   * is rescaled about the active field's origin |field_origin|, as §5.3.3.6.3
   * requires "to make the logical pel dimensions become either integer
   * multiples or integer fractions of the corresponding physical pixel
   * dimensions", keeping "the relative position of the drawing point inside
   * the scaled field" — so every pel of a row is the same number of pixels
   * and the picture cannot skew. Rescaling only ever shrinks, which is what
   * keeps the image "within the original active field".
   */
  void deposit_incremental_row(const NabtsPoint& start, const NabtsSize& pel,
                               const NabtsPoint& field_origin,
                               const std::vector<NaplpsInk>& colours);

  /**
   * @brief Scroll the display within an area (§5.3.3.6.3, §6.2.7.13)
   *
   * The cells of the area at |origin| of |size| move by |displacement|; what
   * moves out of the area is lost, and what is scrolled in is |fill| — the
   * background in colour mode 2 — or left empty, which shows nominal black.
   *
   * With a pel, the move is INCREMENTAL POINT's, a pel's height rescaled as
   * the pel is. Without one it is text scrolling "pixel-by-pixel ... far
   * enough to bring the next intended character location just into the area",
   * so it rounds up to whole pixels.
   */
  void scroll_area(const NabtsPoint& origin, const NabtsSize& size,
                   const NabtsPoint& displacement, const NabtsSize& pel,
                   const NaplpsInk* fill = nullptr);

  /// @ref naplps_arc_polyline at a tolerance fine enough for this grid, which
  /// is what a caller drawing into it wants and saves it working out.
  std::vector<NabtsPoint> arc_polyline(
      const std::vector<NabtsPoint>& control) const;

  /// Forget the marks waiting for a letter: the display they were drawn on is
  /// gone, so nothing that follows can take their field back (§7.2).
  void forget_pending_marks() { pending_marks_ = PendingMarks{}; }

  /**
   * @brief The cells a receiver of the day visits drawing the ARC through
   *        @p control, in order
   *
   * At 256 x 200 every coordinate a record can send is a pixel's corner, and
   * both receivers of the day draw a curve through those lattice points rather
   * than through whichever pixels the true curve crosses — which would put it
   * half a pixel down and to the left of theirs. This is the Norpak TTX 650's
   * own walk (its firmware, bank 0 $81DD-$89EE), pixel-exact against it on
   * every crafted arc and circle tried: the centre half the diameter from the
   * start for a circle (the third point equal to the first, §5.3.3.3.1) and
   * the circumcentre otherwise, then a step at a time to the neighbouring
   * cell, straight or diagonal, whose distance from the centre stays nearest
   * the radius, until the end point is reached. A grid finer than the
   * reference one runs the same walk on its own cells.
   *
   * @p circle says whether it was a circle. False where the walk does not
   * apply: three collinear points, which @p collinear marks — the receivers
   * draw them as two lines, start to intermediate to end — or other than three
   * points (a spline).
   */
  bool arc_cells(const std::vector<NabtsPoint>& control,
                 std::vector<std::pair<int, int>>& path, bool& circle,
                 bool& collinear) const;

  /**
   * @brief Close a filled arc's @p path with its chord
   *
   * §5.3.3.3: a filled arc is bounded by the arc and the chord from its end
   * back to its start. The chord is stepped as the TTX 650 steps a line.
   */
  static void append_chord(std::vector<std::pair<int, int>>& path);

  /**
   * @brief Stamp the logical pel at each cell of @p path, in @p texture
   *
   * A textured arc counts its pattern along the cells it visits, from 0 at its
   * start (TTX 650 $857D), and always draws its start and end; in colour mode
   * 2 the gaps take @p gaps, the background (§5.3.2.4.1).
   */
  void stroke_cells(const std::vector<std::pair<int, int>>& path,
                    const NabtsSize& pel, NabtsLineTexture texture,
                    const NaplpsInk& ink, const NaplpsInk* gaps);

  /**
   * @brief Fill each row from the leftmost to the rightmost cell of
   *        @p outline in it
   *
   * How the TTX 650 fills an arc ($87FC): each row's span of the traced
   * outline, widened by the logical pel the way the pel lies, in the
   * texture's pattern (§5.3.2.4.2).
   */
  void fill_cells(const std::vector<std::pair<int, int>>& outline,
                  const NabtsSize& pel, NabtsTexturePattern pattern,
                  const NabtsSize& mask_size, const NabtsTextureMask* mask,
                  const NaplpsInk& ink, const NaplpsInk* gaps);

  /**
   * @brief Deposit the character @p primitive names into its character field
   *
   * The field (§5.3.2.3.9) covers the cells its edges truncate to — the
   * half-open [floor(x0), floor(x0 + dx)) across and likewise up, Appendix B's
   * truncation of a position to the physical pixels — so the fields of a line
   * tile it with no cell between two of them and none drawn by both, however
   * the field width falls on the grid. Its origin is its lower left corner
   * whatever the sign of its dimensions, a negative dimension reflecting the
   * pattern inside it (§5.3.2.3.9), and rotation turns it counterclockwise
   * about that origin (§5.3.2.3.2, Figure 17).
   *
   * What fills the field is a pattern: a face's for the primary and
   * supplementary sets (§5.1, §5.2), @p drcs's buffer for a downloadable
   * character (§5.6), the 2 by 3 elements of a mosaic (§5.4), or, for Table
   * 25's line and triangle graphics, the field's own edges and corners. A face
   * pattern or a DRCS buffer is laid at a whole multiple of its size, which is
   * how Appendix B has a receiver scale what it stored, so a letterform is the
   * same shape in every field of a line; a character the faces hold no
   * pattern for is shown as a replacement box (§8.3.3(3)).
   *
   * Underline mode adds a line along the field's foot (§6.2.7.15) and puts
   * mosaics into separated mode (§5.4); reverse video draws the field around
   * the shape (§6.2.7.4). @p background is what colour mode 2 fills the rest
   * of the field with, or nullptr where the ground shows through.
   *
   * A non-spacing mark (§5.2, column 4 of the supplementary set) is drawn at
   * once in its spacing form, which is what it is if no letter follows, and
   * held: the letter that follows in the same field takes the field back and
   * is drawn with the mark composed onto it (§7.2).
   */
  void deposit_character(const NabtsPrimitive& primitive,
                         const std::vector<NabtsDrcsCharacter>& drcs,
                         const NaplpsInk& ink, const NaplpsInk* background);

 private:
  /// A rectangle of surface cells.
  struct CellRect {
    int first_column = 0;
    int first_row = 0;
    int columns = 0;
    int rows = 0;
  };

  /**
   * @brief Non-spacing marks drawn in their spacing form, waiting for a letter
   *
   * §7.2 has a mark's code "precede" the letter it modifies, and §5.2 has the
   * mark leave the cursor where it was, so the letter that follows lands in the
   * mark's field. The field's cells before the marks and after them are what
   * let that letter take the field back and draw the composed character once,
   * rather than over the spacing form.
   */
  struct PendingMarks {
    std::vector<NabtsPrimitive> marks;
    CellRect rect;
    std::vector<NaplpsCell> before;
    std::vector<NaplpsCell> after;
    uint32_t appear_ms = 0;
  };

  /// The cells of |rect|, row by row from its lower left.
  std::vector<NaplpsCell> cells_of(const CellRect& rect) const;

  /// Whether |rect| still holds |cells|.
  bool still_holds(const CellRect& rect,
                   const std::vector<NaplpsCell>& cells) const;

  /// Put |cells| back into |rect|, unpainted ones included.
  void put_back(const CellRect& rect, const std::vector<NaplpsCell>& cells);
  /**
   * @brief Lay the pel down with its drawing point at |where|
   *
   * Never fewer than one cell — §5.3.2.2.6's at-least-one-pixel — and never
   * more than the pel's own size, rounded to whole cells. Only *where* it is
   * laid comes from the position. The pel is a brush the standard sizes without
   * reference to where it is or which way it is being swept, so one that grew a
   * pixel whenever it happened to straddle a pixel boundary would draw a line
   * whose weight varied along its own length and with its own direction.
   */
  void stamp_pel_cells(const NaplpsCellPoint& where, const NaplpsCellSize& pel,
                       const NaplpsInk& ink);

  /// The same, laid from the cell the pel's low corner is in, which is the form
  /// a sweep works in: it walks whole cells, so it has no continuous position
  /// to hand back.
  void stamp_pel_block(int anchor_column, int anchor_row,
                       const NaplpsCellSize& pel, const NaplpsInk& ink);

  /// Deposit |ink| over the cells [first, last] in both axes that can take it
  /// — the traced region while an outline is traced, the surface otherwise —
  /// visiting no others.
  void deposit_block(int first_column, int first_row, int last_column,
                     int last_row, const NaplpsInk& ink);

  /// Sweep the pel from one cell-space position to another, stamping it along
  /// the way so the locus has no gap.
  void sweep_pel(const NaplpsCellPoint& from, const NaplpsCellPoint& to,
                 const NaplpsCellSize& pel, const NaplpsInk& ink);

  NaplpsCellSurface& surface_;
  NaplpsGridMapping mapping_;

  /// While set, a stamped pel records the cells it covers here instead of
  /// depositing into the surface.
  ///
  /// This is how a filled figure works out the outline the pel traces round it:
  /// by running the routine that *strokes* an outline and noting what it
  /// touched. §5.3.3.5.1 puts that traced region inside the fill and
  /// §5.3.2.2.6 defines it once for every drawing operation alike, so the two
  /// are the same region and computing them separately only invites them to
  /// disagree — as they did, leaving a fill showing past the outline a service
  /// drew over it.
  NaplpsTracedRegion* traced_ = nullptr;

  /// The marks deposit_character() is holding for the letter that follows.
  PendingMarks pending_marks_;
};

}  // namespace orc

#endif  // ORC_NABTS_SINK_NAPLPS_RASTER_H
