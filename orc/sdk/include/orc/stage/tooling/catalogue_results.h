/*
 * File:        catalogue_results.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Host-agnostic browsable result set: a stage hands over a list of
 *              catalogued items and something drawable for each, and the host
 *              presents them without knowing what service produced them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_STAGE_TOOLING_CATALOGUE_RESULTS_H
#define ORC_STAGE_TOOLING_CATALOGUE_RESULTS_H

// SDK TIER: stage/tooling — stage contract type crossing the plugin boundary.
// A layout change here bumps the host ABI version.

#include <cstdint>
#include <string>
#include <vector>

namespace orc {

/**
 * @file
 * @brief What a stage that catalogues *things* hands the host to show
 *
 * A stage that decodes a data service over a frame range ends up with a set of
 * items — pages, records, cues — each of which a reader wants listed and then
 * looked at one at a time. Before this contract existed, each such stage needed
 * the host to grow a viewer that knew its data model, which is why the host
 * carried a teletext page viewer and a NABTS record viewer and could not have
 * accepted a third from outside the tree at all.
 *
 * Nothing here names a service. An item is an id, some column values and a
 * payload; a payload is a character-cell grid, a 2D display list, a text
 * document or a table. Those four cover a WST page, a NAPLPS record, a function
 * listing and a cue track, and they are the shapes a broadcast data service
 * presents in generally — the host draws them and stays ignorant of the rest.
 *
 * Everything is resolved on the plugin's side of the boundary: colours are RGB
 * or palette indices with the palette attached, characters are Unicode, and
 * geometry is in a unit space. A renderer walks a payload and needs no state
 * and no decoder.
 */

// ---------------------------------------------------------------------------
// Shared primitives
// ---------------------------------------------------------------------------

/// One resolved colour. @ref transparent is the videotex convention of a colour
/// that shows the programme video through rather than painting; a renderer with
/// nothing behind draws it as black, but it is carried rather than resolved
/// away because a caption is exactly the case where something *is* behind.
struct CatalogueColour {
  uint8_t red = 0;
  uint8_t green = 0;
  uint8_t blue = 0;
  bool transparent = false;
  /// Where the colour is an entry of CatalogueDisplayList::palette rather than
  /// a value — a pixel that stores a colour map address, as a videotex one
  /// does — the entry's index, so a renderer playing the list over time can
  /// follow the entry as it changes (CatalogueDisplayList::palette_tracks).
  /// -1 for a plain value. The RGB above is always the still colour: what the
  /// entry holds once the payload has finished drawing, blinks at rest.
  int16_t palette_index = -1;

  bool operator==(const CatalogueColour& other) const {
    return red == other.red && green == other.green && blue == other.blue &&
           transparent == other.transparent &&
           palette_index == other.palette_index;
  }
  bool operator!=(const CatalogueColour& other) const {
    return !(*this == other);
  }
};

/// A time that never comes, for "no further change".
inline constexpr uint32_t kCatalogueNever = 0xFFFFFFFFu;

/// One colour a palette entry takes from a moment on.
struct CatalogueColourStep {
  /// Milliseconds from the start of the payload's presentation.
  uint32_t time_ms = 0;
  CatalogueColour colour;
};

/**
 * @brief How one palette entry changes over a presentation
 *
 * The entry holds each step's colour from its time until the next step's. A
 * track that loops repeats its steps from @ref loop_start_ms every
 * @ref loop_period_ms for as long as the payload is shown — a blinking entry
 * settles into such a cycle once the payload has finished drawing. The source
 * works the cycle out; a renderer only reads it, so it needs to know nothing
 * about why the entry changes.
 */
struct CataloguePaletteTrack {
  /// Ascending by time, the first at 0 when there are any. Empty for an entry
  /// that never changes, which then shows its still colour throughout.
  std::vector<CatalogueColourStep> steps;
  /// Start of the repeating part. A looping track has a step exactly here.
  uint32_t loop_start_ms = 0;
  /// Length of the repeating part; 0 holds the last step for good.
  uint32_t loop_period_ms = 0;

  /// The entry's colour at @p time_ms, or @p still for a track with no steps.
  CatalogueColour colour_at(uint32_t time_ms,
                            const CatalogueColour& still) const {
    if (steps.empty()) {
      return still;
    }
    const uint32_t t = in_first_cycle(time_ms);
    const CatalogueColourStep* at = &steps.front();
    for (const CatalogueColourStep& step : steps) {
      if (step.time_ms > t) {
        break;
      }
      at = &step;
    }
    return at->colour;
  }

  /// The first time after @p time_ms at which the entry changes, or
  /// kCatalogueNever where it has settled for good.
  uint32_t next_change_after(uint32_t time_ms) const {
    if (steps.empty()) {
      return kCatalogueNever;
    }
    const uint32_t t = in_first_cycle(time_ms);
    for (const CatalogueColourStep& step : steps) {
      if (step.time_ms > t) {
        return time_ms + (step.time_ms - t);
      }
    }
    if (loop_period_ms == 0) {
      return kCatalogueNever;
    }
    // Round the loop: the next step is the first one inside it, one period on.
    for (const CatalogueColourStep& step : steps) {
      if (step.time_ms >= loop_start_ms) {
        return time_ms + (step.time_ms + loop_period_ms - t);
      }
    }
    return kCatalogueNever;
  }

 private:
  /// @p time_ms folded back into the first pass of the loop, if there is one.
  uint32_t in_first_cycle(uint32_t time_ms) const {
    if (loop_period_ms == 0 || time_ms < loop_start_ms + loop_period_ms) {
      return time_ms;
    }
    return loop_start_ms + (time_ms - loop_start_ms) % loop_period_ms;
  }
};

/// A point in the payload's unit space.
struct CataloguePoint {
  double x = 0.0;
  double y = 0.0;
};

/// A size in unit space, which may be negative — a character field or a
/// rectangle drawn back from its origin is, and the sign is the direction.
struct CatalogueSize {
  double dx = 0.0;
  double dy = 0.0;
};

/**
 * @brief A monochrome element bitmap: a downloadable glyph or a fill mask
 *
 * Row 0 is the *bottom* row, matching the unit space the display list uses.
 */
struct CatalogueBitmap {
  int width = 0;
  int height = 0;
  /// Row-major, @ref width * @ref height entries, true for an element that is
  /// on.
  std::vector<bool> elements;

  bool defined() const {
    return width > 0 && height > 0 &&
           elements.size() ==
               static_cast<size_t>(width) * static_cast<size_t>(height);
  }
};

// ---------------------------------------------------------------------------
// Payload: character-cell grid
// ---------------------------------------------------------------------------

/**
 * @brief One cell of a character-grid page
 *
 * @ref foreground and @ref background index CatalogueCellGrid::palette rather
 * than carrying RGB, because a cell grid is a service with a fixed display
 * palette and showing the palette is part of showing the page.
 */
struct CatalogueCell {
  /// Render as a block mosaic (@ref mosaic_pattern) instead of a glyph.
  bool mosaic = false;
  /// Unicode code point for an alphanumeric cell, already mapped out of
  /// whatever code table the service transmitted.
  char32_t character = U' ';
  /// Six sub-element bits for a mosaic cell in a 2x3 grid: bit 0 top-left,
  /// 1 top-right, 2 middle-left, 3 middle-right, 4 bottom-left,
  /// 5 bottom-right.
  uint8_t mosaic_pattern = 0;
  /// Separated (bordered) rather than contiguous mosaic blocks.
  bool mosaic_separated = false;

  uint8_t foreground = 0;  ///< Index into CatalogueCellGrid::palette
  uint8_t background = 0;  ///< Index into CatalogueCellGrid::palette

  /// Origin (upper) cell of a double-height pair, and the lower cell of one.
  /// The lower cell carries background only: the origin paints the character's
  /// lower half over it.
  bool double_height = false;
  bool double_height_lower = false;

  /// The cell's character alternates with being blanked. A host that animates
  /// picks its own rate; a still one draws the lit phase.
  bool flash = false;
  bool concealed = false;  ///< Rendered as SPACE until revealed
  /// Inside a boxed region. Only meaningful with
  /// CatalogueCellGrid::boxed_only.
  bool boxed = false;
  /// The transmitted byte was known damaged and substituted. A damaged byte
  /// renders exactly like a transmitted SPACE, so this is the only thing that
  /// makes the difference visible.
  bool damaged = false;
};

/// How a grid row fared in recovery. Both default to the benign reading, so a
/// service with nothing to say about its rows can leave the vector empty.
struct CatalogueRowStatus {
  /// A packet was recovered for this row. False is *not* by itself a fault:
  /// services routinely omit the blank rows that space a page out rather than
  /// transmitting them, so a gap only means data went astray when
  /// CatalogueCellGrid::data_lost says something did.
  bool received = true;
  /// The row rests on a single unchecked copy while other rows of the same
  /// page have been confirmed by a repeat — nothing is known to be wrong with
  /// it, it has simply never been checked.
  bool unconfirmed = false;
};

/**
 * @brief A page of character cells, ready to draw
 */
struct CatalogueCellGrid {
  int rows = 0;
  int columns = 0;
  /// Row-major, @ref rows * @ref columns entries.
  std::vector<CatalogueCell> cells;
  /// One entry per row, or empty when the service has nothing to say.
  std::vector<CatalogueRowStatus> row_status;
  /// Display palette the cell colour indices address. An index past the end
  /// resolves to the last entry.
  std::vector<CatalogueColour> palette;

  /// Nominal shape of one character rectangle. The grid is drawn into a rect of
  /// this aspect so glyphs and mosaic blocks keep their proportions whatever
  /// shape the host gives the view.
  int cell_aspect_width = 1;
  int cell_aspect_height = 1;

  /// Only cells with CatalogueCell::boxed set are displayed; the rest of the
  /// screen is transparent to the video behind. What a newsflash or subtitle
  /// page is.
  bool boxed_only = false;

  /// Something is known to have gone astray in this page, so a row that carries
  /// no packet is a candidate for what was lost and worth marking. Without it,
  /// an un-received row is assumed to be one the service chose not to send.
  bool data_lost = false;

  const CatalogueCell& at(int row, int column) const {
    return cells[static_cast<size_t>(row) * static_cast<size_t>(columns) +
                 static_cast<size_t>(column)];
  }
  bool valid() const {
    return rows > 0 && columns > 0 &&
           cells.size() ==
               static_cast<size_t>(rows) * static_cast<size_t>(columns);
  }
};

// ---------------------------------------------------------------------------
// Payload: 2D display list
// ---------------------------------------------------------------------------

/// What a display-list operation draws.
enum class CatalogueDrawKind : uint8_t {
  kPoint,      ///< A single pen-sized mark
  kLine,       ///< A polyline through the control points
  kArc,        ///< Three points an arc, more a spline through them
  kRectangle,  ///< From @ref origin by @ref size
  kPolygon,
  /// A raster run of colours deposited across a field, one pen apiece.
  kColourRun,
  /// A run of characters, @ref text as UTF-8, laid out from @ref origin along
  /// @ref advance with @ref size per character field.
  kText,
  /// One 2x3 block mosaic; @ref mosaic_pattern holds the six elements.
  kMosaic,
  /// One downloadable glyph; @ref glyph_index indexes
  /// CatalogueDisplayList::glyphs.
  kGlyph,
};

enum class CatalogueLineStyle : uint8_t {
  kSolid = 0,
  kDotted = 1,
  kDashed = 2,
  kDashDotted = 3,
};

/// kMask0 to kMask3 are the programmable masks carried in
/// CatalogueDisplayList::fill_masks.
enum class CatalogueFillPattern : uint8_t {
  kSolid = 0,
  kVerticalHatch = 1,
  kHorizontalHatch = 2,
  kCrossHatch = 3,
  kMask0 = 4,
  kMask1 = 5,
  kMask2 = 6,
  kMask3 = 7,
};

/**
 * @brief One drawing operation with the state that was in force when it ran
 *
 * Self-contained, so the list can be drawn, re-drawn, clipped or scaled without
 * re-running anything. The fields a kind does not use keep their defaults.
 */
struct CatalogueDrawOp {
  CatalogueDrawKind kind = CatalogueDrawKind::kPoint;

  /// Control points in unit space, already resolved from whatever mix of
  /// absolute and relative coordinates the source used.
  std::vector<CataloguePoint> points;
  /// Origin of a rectangle, a character run, a mosaic or a glyph; the first of
  /// @ref points otherwise.
  CataloguePoint origin;
  /// Extent of a rectangle, or the character field of a character run.
  CatalogueSize size;

  bool filled = false;
  /// A filled figure additionally outlined in black, or in the background
  /// colour where there is one.
  bool outlined = false;

  /// The drawing pen in unit space, which is what gives a line its width, a
  /// point its size and a separated mosaic its gap. It is a rectangle hung
  /// from the drawing point by one corner — the lower left where both
  /// dimensions are positive, and the corner the signs name otherwise — and
  /// swept along a stroke, so a line is as wide as the pen is across it.
  CatalogueSize pen_size;
  CatalogueLineStyle line_style = CatalogueLineStyle::kSolid;
  CatalogueFillPattern fill_pattern = CatalogueFillPattern::kSolid;
  /// Step-and-repeat size for a programmable fill mask.
  CatalogueSize fill_mask_size;

  CatalogueColour colour;
  /// Background colour. @ref has_background is how a renderer tells "black
  /// background" from "no background".
  CatalogueColour background;
  bool has_background = false;

  /// kColourRun only: one colour per pen position, in raster order within the
  /// field @ref size describes.
  std::vector<CatalogueColour> colour_run;

  /// kText only: the run as UTF-8, one character per character field along the
  /// path. Combining marks are already in Unicode order, so this is directly
  /// displayable.
  std::string text;
  /// kText only: the step from one character field origin to the next. The
  /// character path and the inter-character spacing decide it, and a renderer
  /// laying the run out can recover neither from the origin alone.
  CatalogueSize advance;
  /// kText only: how many character fields the run occupies, which is the count
  /// of *base* characters — a composed accent occupies one field, not two, so
  /// this cannot be recovered from @ref text alone.
  int character_count = 0;

  /// kMosaic only: the six sub-elements, bit 0 top-left through bit 5
  /// bottom-right, and whether they are separated by the pen rather than
  /// contiguous.
  uint8_t mosaic_pattern = 0;
  bool mosaic_separated = false;

  /// kGlyph only: index into CatalogueDisplayList::glyphs, or -1 for a
  /// character the source used but never defined (drawn as SPACE).
  int glyph_index = -1;

  /// Counterclockwise rotation of the character field about its origin:
  /// 0, 90, 180 or 270 degrees.
  int rotation_degrees = 0;
  /// The field is filled and the character shape left undrawn.
  bool reverse_video = false;
  bool underlined = false;

  /// When the operation is drawn, in milliseconds from the start of the
  /// payload's presentation. A payload that draws in stages — a videotex page
  /// that pauses part way through — gives each operation the moment its stage
  /// is reached; operations are listed in that order, so a renderer showing
  /// time T draws the ones up to T and a still render draws them all.
  uint32_t appear_ms = 0;

  /// How long the operation takes to appear, in milliseconds ending at
  /// @ref appear_ms: a figure its source drew a row at a time, as the
  /// receivers of the day filled a shape or cleared the screen, so that a
  /// large one was seen to sweep across it. From appear_ms less this until
  /// appear_ms the operation is drawn only on the side of the sweep the sweep
  /// has passed (@ref reveal_from_y), and from appear_ms whole. 0, the
  /// default, for an operation that appears whole at appear_ms, as every
  /// operation of a source that names no sweep does; never more than
  /// appear_ms.
  uint32_t reveal_duration_ms = 0;
  /// The sweep that reveals the operation: a line across the drawable area at
  /// unit y @ref reveal_from_y as the reveal starts, moving at an even pace to
  /// @ref reveal_to_y at appear_ms. The operation is drawn on the side of the
  /// line it has passed — above it for a sweep down the screen (from_y above
  /// to_y), below it for a sweep up. Operations sharing a sweep, such as the
  /// pixel runs of one figure, are revealed together. Meaningful only where
  /// @ref reveal_duration_ms is not 0.
  double reveal_from_y = 0.0;
  double reveal_to_y = 0.0;
};

/**
 * @brief A drawing program run out into something a renderer can walk
 *
 * The list is in execution order, which is also back-to-front paint order: a
 * later operation covers an earlier one.
 */
struct CatalogueDisplayList {
  std::vector<CatalogueDrawOp> ops;

  /// Extent of unit y the list is drawn over, as a fraction of unit x. Unit x
  /// runs 0 to 1 left to right and unit y runs 0 to this value *upwards* from
  /// the bottom left.
  double aspect_height = 1.0;

  /**
   * Height of the drawable area as a fraction of its width, i.e. the shape the
   * unit space above is drawn at. Zero means the nominal pixels are square, so
   * the area takes the shape @ref aspect_height gives it.
   *
   * The two differ wherever the source's pixels are not square. A display the
   * unit space is authored against is a physical screen with an aspect of its
   * own, and stating that aspect separately is what lets a renderer put a
   * rectangular pixel on screen as a rectangle.
   */
  double display_aspect_height = 0.0;

  /**
   * The physical pixel grid the list was resolved against, across and down the
   * drawable area, or zero for a list that names none.
   *
   * A display list is geometry, not pixels, but the source it came from may
   * have been authored for a receiver of a particular resolution, and some of
   * its geometry only means anything against that resolution — a stroke of no
   * stated width is one *pixel* wide, not one unit or one device dot. A
   * renderer uses this to size such things; without it, a device pixel is all
   * it can fall back on, which makes them depend on how large the list is
   * drawn.
   */
  int nominal_width = 0;
  int nominal_height = 0;

  /**
   * The operations tile the grid above rather than describing shapes drawn on
   * it: each is one pixel of it, or a run of pixels along a row.
   *
   * A renderer draws such a list with its edges hard and aligned to whole
   * device pixels. Smoothing them would be smoothing the source's pixels, which
   * are the content here rather than an artefact of drawing it — and where two
   * runs abut, blending both sides of the boundary leaves a seam along a join
   * that should be invisible.
   */
  bool pixel_aligned = false;

  /// The colour palette as it stood at the end, carried for display even though
  /// the operations already have their colours resolved.
  std::vector<CatalogueColour> palette;
  /// How each palette entry changes over the presentation, parallel to
  /// @ref palette, or empty for a payload whose colours hold still. A colour
  /// whose palette_index names an entry takes the track's colour at the time
  /// shown.
  std::vector<CataloguePaletteTrack> palette_tracks;

  /// Downloadable glyphs CatalogueDrawOp::glyph_index addresses.
  std::vector<CatalogueBitmap> glyphs;
  /// Programmable fill masks CatalogueFillPattern::kMask0 onwards address.
  std::vector<CatalogueBitmap> fill_masks;

  bool empty() const { return ops.empty(); }

  /// Whether the payload changes as it is shown: operations that appear after
  /// the start, or palette entries that change.
  bool timed() const {
    for (const CataloguePaletteTrack& track : palette_tracks) {
      if (!track.steps.empty()) {
        return true;
      }
    }
    for (const CatalogueDrawOp& op : ops) {
      if (op.appear_ms != 0) {
        return true;
      }
    }
    return false;
  }
};

// ---------------------------------------------------------------------------
// Payload: text and table
// ---------------------------------------------------------------------------

struct CatalogueTextDocument {
  std::string text;
  /// Render in a fixed-pitch font, which a listing or a dump wants and prose
  /// does not.
  bool monospace = true;
};

/// A column of the item list, or of a table payload.
struct CatalogueColumn {
  std::string id;
  std::string title;
  /// Sort and align as a number rather than as text.
  bool numeric = false;
};

struct CatalogueTable {
  std::vector<CatalogueColumn> columns;
  /// One entry per row, each with one value per column.
  std::vector<std::vector<std::string>> rows;
};

// ---------------------------------------------------------------------------
// Payload
// ---------------------------------------------------------------------------

/**
 * @brief What the host shows for one selected item
 *
 * One of the four forms, plus the readouts that go around it. The unused forms
 * are left empty rather than being a variant: the cost is a few empty vectors
 * per item, and every consumer stays a plain switch.
 */
struct CataloguePayload {
  enum class Kind : uint8_t {
    kNone,  ///< Nothing to show; a parent item whose children carry the content
    kCellGrid,
    kDisplayList,
    kText,
    kTable,
  };

  Kind kind = Kind::kNone;

  CatalogueCellGrid grid;
  CatalogueDisplayList display_list;
  CatalogueTextDocument document;
  CatalogueTable table;

  /// Text form shown beside a visual payload, for content that is usually read
  /// rather than looked at. Empty when there is none.
  std::string companion_text;

  /// One line identifying what is on show and how often it was seen —
  /// "Page 100 seen 12 times (frames 5-4210)".
  std::string headline;
  /// One line on the condition of what is on show — "rows 23/24, 4 damaged
  /// byte(s)". Empty when there is nothing to say.
  std::string condition;
};

// ---------------------------------------------------------------------------
// The catalogue
// ---------------------------------------------------------------------------

/**
 * @brief A way from one item to another that the service gave the reader
 *
 * A key of its receivers' keypad (NEXT, MORE, INDEX) or a page number the
 * item's own content offers. @ref label is what the reader pressed or read
 * ("NEXT", "751"); @ref target_id the CatalogueItem::id it leads to.
 */
struct CatalogueLink {
  enum class Kind : uint8_t { kNext, kMore, kIndex, kReference };
  Kind kind = Kind::kReference;
  std::string label;
  std::string target_id;
};

/**
 * @brief One row of the item list
 *
 * Items nest one level: an item with a @ref parent_id is a variant of that
 * parent — a sub-page of a page — which the host steps through beside the
 * payload rather than listing separately. One level is what a carousel of
 * variants needs, and a deeper tree would buy a shape no service has asked for.
 */
struct CatalogueItem {
  /// Opaque, unique within one dataset, and stable across a re-read of the same
  /// trigger run. The host round-trips it and never parses it.
  std::string id;
  /// Empty for a top-level item; otherwise the id of the item this is a variant
  /// of. A parent must appear before its children.
  std::string parent_id;

  /// One value per CatalogueSchema::columns entry. Children need not fill them:
  /// they are not listed.
  std::vector<std::string> values;

  /// Short marks shown against the row — "subs", "damaged". Free text; the
  /// host appends them to the first column and attaches no meaning.
  std::vector<std::string> badges;

  /// A row the reader can select. False lists the item greyed, for something
  /// the catalogue knows of but cannot usefully be shown.
  bool selectable = true;

  /// Explanation shown on hover, for anything about the item a column cannot
  /// carry — why it is unselectable, what a badge means.
  std::string tooltip;

  /// What the reader types into the find box to reach this item, when the
  /// schema offers one. Matched case-insensitively after trimming, so a service
  /// should give the canonical form. Empty means the item cannot be reached by
  /// typing.
  std::string find_key;

  /// Further keys the find box takes for this item, matched like
  /// @ref find_key: the number a reader of the service typed on its keypad to
  /// reach it ("150", "1985"). Empty when @ref find_key is the only one.
  std::vector<std::string> find_aliases;

  /// Label for this item in the variant stepper — "0002" for a sub-page.
  /// Ignored on a top-level item.
  std::string variant_label;

  /// Where the reader can go from this item, in the order the service offers
  /// it. Empty for an item that names no other.
  std::vector<CatalogueLink> links;

  /// The keys the reader can press on this item, each of which changes what it
  /// shows: a page played on a keypad, as a broadcast game was. Each id is
  /// also the key's label — "0" to "9" for a keypad's digits — and the host
  /// offers exactly these, under CatalogueSchema::keypad_label. Empty for an
  /// item that takes no keys, which is every item of a service that has none.
  /// What a sequence of presses shows is asked of the stage through
  /// ICatalogueResults::played().
  std::vector<std::string> keys;
};

/**
 * @brief One way of presenting the same catalogue, offered to the reader
 *
 * Not a filter and not a setting: every option shows the same items, drawn
 * differently. What differs is a property of the display rather than of the
 * decode — which receiver a page is resolved against, say — so the reader picks
 * between them while looking, and the stage builds the catalogue again under
 * whichever is asked for.
 */
struct CatalogueViewOption {
  /// Opaque, unique within the schema, and round-tripped back to the stage
  /// untouched.
  std::string id;
  /// What the reader picks from the list — "240p (256 x 200)".
  std::string label;
  /// Explanation shown on hover, for an option whose label cannot carry why
  /// anyone would want it. Empty for one that needs none.
  std::string tooltip;
};

/**
 * @brief One presentation choice the reader turns on or off
 *
 * A CatalogueViewOption in every respect but its shape: it changes how the same
 * items are presented rather than what was found, and the stage is re-asked for
 * the catalogue whenever the reader changes it. What it is *not* is a second
 * way of spelling a view option — the two are separate because they are
 * independent axes, and folding a toggle into the dropdown would multiply out
 * the options rather than add to them.
 *
 * Use one where the choice is genuinely two-state and independent of the rest:
 * whether damaged data is presented as recovered or as transmitted, say. A
 * choice between three or more presentations belongs in @ref
 * CatalogueSchema::view_options.
 */
struct CatalogueViewToggle {
  /// Opaque, unique within the schema, and round-tripped back to the stage
  /// untouched.
  std::string id;
  /// What the reader sees beside the checkbox — "Error correction".
  std::string label;
  /// Explanation shown on hover. Empty for a toggle whose label needs none.
  std::string tooltip;
  /// Whether this catalogue was built with the toggle on, so the host can show
  /// the reader what they are looking at without having to remember what it
  /// asked for. The state the stage puts here on the first build is the default
  /// the reader is offered.
  bool active = false;
};

/**
 * @brief How the host should label and shape the browser
 *
 * All of it optional: a schema with columns and nothing else gets a plain list
 * and a payload pane.
 */
struct CatalogueSchema {
  std::vector<CatalogueColumn> columns;

  /// What one item is called, for the navigation controls — "Page", "Record".
  std::string item_noun;
  /// What one variant is called — "Sub-page". Empty when items have no
  /// children, which hides the variant stepper.
  std::string variant_noun;

  /// When non-empty the host offers a text entry that selects the item whose
  /// first column value matches what is typed, for a service whose items have
  /// a number a reader knows by heart.
  std::string find_label;
  std::string find_placeholder;

  /// When non-empty the host offers a toggle that overlays recovery damage on
  /// the payload. Empty for a service that reports no damage.
  std::string highlight_label;

  /// When @ref view_options is non-empty the host offers a dropdown labelled
  /// this way that re-asks the stage for its catalogue under the option the
  /// reader picks. Empty for a service that presents its payloads one way only.
  std::string view_label;
  std::vector<CatalogueViewOption> view_options;
  /// Which of @ref view_options this catalogue was built under, so the host can
  /// show what it is looking at without having to remember what it asked for.
  /// Empty selects the first option.
  std::string view_option;

  /// Presentation choices the reader turns on and off, shown beside the view
  /// dropdown. Empty for a service that offers none, which is what leaves
  /// today's browser unchanged. Each carries the state this catalogue was built
  /// under (CatalogueViewToggle::active).
  std::vector<CatalogueViewToggle> toggles;

  /// Shown in place of the payload when the run catalogued nothing at all —
  /// "No teletext pages were recovered". A recording that carried none of the
  /// service is the ordinary case, not an error, and saying so beats an empty
  /// pane.
  std::string empty_message;

  /// What the service calls the keys its interactive items take — "Option
  /// keys" — which the host labels its keypad with when an item that takes
  /// keys (CatalogueItem::keys) is on show. Empty for a service none of whose
  /// items take any, which offers no keypad.
  std::string keypad_label;
};

/// Run-wide readouts shown regardless of what is selected.
struct CatalogueSummary {
  /// One line on how the whole run went, for the status bar.
  std::string headline;
  /// Standing notices — "Subtitles on 190", "12 captions on A00/000". Shown
  /// above the list, and hidden when empty.
  std::vector<std::string> notices;
};

/**
 * @brief Everything a catalogue browser needs for one trigger run
 *
 * @ref payloads runs parallel to @ref items. A parent item may carry
 * CataloguePayload::Kind::kNone, in which case the host shows its first child.
 */
struct CatalogueDataset {
  CatalogueSchema schema;
  std::vector<CatalogueItem> items;
  std::vector<CataloguePayload> payloads;
  CatalogueSummary summary;

  bool consistent() const { return items.size() == payloads.size(); }
};

/**
 * @brief A stage whose triggered results are a browsable catalogue
 *
 * Advertise the tool with StageToolKind::CatalogueBrowser and the contract id
 * kCatalogueBrowserContractId, and the host routes it to its generic browser
 * with no knowledge of the stage.
 *
 * Building the catalogue is the stage's own business, and it may be expensive:
 * the host calls @ref catalogue at most once per viewer opening, on a worker
 * thread, and copies what it gets. Build lazily and cache if the work is real.
 */
class ICatalogueResults {
 public:
  virtual bool has_results() const = 0;
  /// The catalogue as the stage's own settings have it, which is what the host
  /// asks for when it first opens the browser.
  virtual const CatalogueDataset& catalogue() const = 0;

  /**
   * @brief The catalogue built under one of CatalogueSchema::view_options
   *
   * The host calls this when the reader picks an option, passing the id
   * verbatim; an id the stage does not recognise means the settings' own view,
   * so a host need not police what it round-trips. The default ignores the
   * option, which is right for the stage that offers none.
   *
   * Same terms as @ref catalogue: called on a worker thread, at most once per
   * change of option, and what it returns is copied before the next call. A
   * stage is therefore free to build under one option at a time and replace
   * what it holds, which is the reference the previous call returned.
   */
  virtual const CatalogueDataset& catalogue(
      const std::string& view_option) const {
    (void)view_option;
    return catalogue();
  }

  /**
   * @brief The catalogue built under a view option and a set of toggles
   *
   * The host calls this when the reader changes either, passing the view
   * option's id and the ids of every toggle now on, all verbatim; ids the stage
   * does not recognise mean the settings' own view, so a host need not police
   * what it round-trips. A toggle the stage offers and the host does not name
   * is off.
   *
   * The default ignores the toggles and forwards to @ref catalogue(const
   * std::string&), which is right for the stage that offers none.
   *
   * Same terms as @ref catalogue: called on a worker thread, at most once per
   * change, and what it returns is copied before the next call.
   */
  virtual const CatalogueDataset& catalogue(
      const std::string& view_option,
      const std::vector<std::string>& active_toggles) const {
    (void)active_toggles;
    return catalogue(view_option);
  }

  /**
   * @brief An interactive item's payload after the reader has pressed keys
   *
   * @p item_id names an item whose CatalogueItem::keys is not empty, and
   * @p keys are ids from that list, every key pressed on the item since it was
   * shown, in order. The stage presents the item afresh and then each key in
   * turn, so a call is answered from scratch and the same sequence always gives
   * the same payload: the host keeps the sequence, and starting a game again is
   * asking with none. Where the payload plays over time, its time starts at the
   * last key, what was on screen before it standing from the start, so the host
   * plays what the key did as it plays any timed payload. @p view_option and
   * @p active_toggles are the presentation the reader has on, as for
   * catalogue(const std::string&, const std::vector<std::string>&), so a game
   * is drawn the way the catalogue on show is.
   *
   * The default, for a stage whose items take no keys, is a payload of kind
   * kNone: there is nothing to play. Same terms as @ref catalogue: called on a
   * worker thread, and what it returns is the host's.
   */
  virtual CataloguePayload played(
      const std::string& item_id, const std::vector<std::string>& keys,
      const std::string& view_option,
      const std::vector<std::string>& active_toggles) const {
    (void)item_id;
    (void)keys;
    (void)view_option;
    (void)active_toggles;
    return {};
  }

  virtual ~ICatalogueResults() = default;
};

/// Contract id a catalogue-browser StageToolDescriptor must carry.
inline constexpr const char* kCatalogueBrowserContractId =
    "decode-orc.stage-tools.catalogue.v1";

}  // namespace orc

#endif  // ORC_STAGE_TOOLING_CATALOGUE_RESULTS_H
