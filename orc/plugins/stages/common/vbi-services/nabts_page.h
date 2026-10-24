/*
 * File:        nabts_page.h
 * Module:      orc-vbi-services (shared plugin library)
 * Purpose:     A decoded NAPLPS presentation record as a resolved display list
 *              in unit space
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_PAGE_H
#define ORC_NABTS_PAGE_H

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace orc {

/**
 * @file
 * @brief The decode/render boundary for a NABTS presentation record
 *
 * A NABTS presentation record is NAPLPS — CEA-516 §6.1, which is ANSI
 * X3.110-1983 / ITU-T T.101 Data Syntax III — and NAPLPS is a drawing language
 * rather than a character grid. There is no equivalent of the World System
 * Teletext 40x25 cell array to decode into: a page is a program of geometric
 * primitives, and what it looks like depends on where it is drawn.
 *
 * So the decoder resolves rather than renders. It runs the program, tracks the
 * presentation state the standard defines, and emits each drawing operation
 * with the attributes that were in force when it executed — in the unit
 * Cartesian space of X3.110 §5.3.1, before any mapping to pixels. Rasterising
 * that is the host's business, which keeps the MVP boundary exactly where the
 * teletext viewer's is: decode below, render above.
 *
 * Coordinates are unit space throughout: 0 <= x < 1 and 0 <= y < 1, y upwards
 * from the bottom left. The display area is the lower 0,78125 of it (Table D1
 * item 10 and T.101 Table II-3), and the strip above is border that Table D1
 * does not guarantee is visible.
 */

/// The unit screen's y extent that Table D1 item 10 guarantees is visible.
constexpr double kNabtsDisplayAreaHeight = 0.78125;

/// X3.110 §5.3.2.6: colour modes 0, 1 and 2, which decide whether a drawing
/// colour is a value or a colour-map address, and whether a background colour
/// applies.
enum class NabtsColourMode : uint8_t {
  /// Colour value used directly; no background colour (§5.3.2.5).
  kDirect = 0,
  /// Drawing colour is a map address; foreground pixels only.
  kMapped = 1,
  /// Drawing and background colours are both map addresses.
  kMappedWithBackground = 2,
};

/**
 * @brief Bits a gun, and full intensity
 *
 * Table D1 item 5(4) asks a teletext receiver for "sixteen simultaneous
 * colours out of a set of 512 obtained by allocating three bits each to G R &
 * B", and that is a floor. CEA-516 §8.6.3 has services choose from 4096 — four
 * bits a gun — picked to look right on the 512-colour receivers too; the
 * reference colour map of the standard's authors holds twelve-bit colours (US
 * 4,439,759); and both receivers of the day whose firmware the recordings were
 * checked against have four-bit converters. Kept at three, greys a page sends
 * that differ only in their fourth bit merge into one.
 */
constexpr int kNabtsGunBits = 4;
constexpr int kNabtsGunMax = (1 << kNabtsGunBits) - 1;

/**
 * @brief One colour, as the GRB system of X3.110 §5.3.1 expresses it
 *
 * Each gun 0 to kNabtsGunMax. Held as components rather than a packed value
 * because the transmission order (green, red, blue, by decreasing luminance)
 * is not the order anything renders in.
 */
struct NabtsColour {
  uint8_t green = 0;
  uint8_t red = 0;
  uint8_t blue = 0;
  /// X3.110 §5.3.2.5: a SET COLOR with no operand sets the transparent colour,
  /// which shows lower planes through — the analogue video signal, in a
  /// captioning application. Rendered as black where there is nothing behind.
  bool transparent = false;

  bool operator==(const NabtsColour& other) const {
    return green == other.green && red == other.red && blue == other.blue &&
           transparent == other.transparent;
  }
};

/// Colour-map entries Table D1 item 5(4) requires, and T.101 Table II-3
/// tabulates.
constexpr size_t kNabtsColourMapEntries = 16;

/// X3.110 §5.3.2.4.2 Table 12.
enum class NabtsLineTexture : uint8_t {
  kSolid = 0,
  kDotted = 1,
  kDashed = 2,
  kDottedDashed = 3,
};

/// X3.110 §5.3.2.4.4 Table 13. The four programmable masks are defined by DEF
/// TEXTURE (§6.2.4) and carried in NabtsPageSnapshot::texture_masks.
enum class NabtsTexturePattern : uint8_t {
  kSolid = 0,
  kVerticalHatch = 1,
  kHorizontalHatch = 2,
  kCrossHatch = 3,
  kMaskA = 4,
  kMaskB = 5,
  kMaskC = 6,
  kMaskD = 7,
};

/// X3.110 §5.3.2.3.2 Table 6: rotation counterclockwise about the character
/// field origin.
enum class NabtsCharRotation : uint8_t {
  kNone = 0,
  k90 = 1,
  k180 = 2,
  k270 = 3,
};

/// X3.110 §5.3.2.3.3 Table 7: the direction the cursor advances after a
/// character.
enum class NabtsCharPath : uint8_t {
  kRight = 0,
  kLeft = 1,
  kUp = 2,
  kDown = 3,
};

/// X3.110 §5.3.2.3.8 Table 11.
enum class NabtsCursorStyle : uint8_t {
  kUnderscore = 0,
  kBlock = 1,
  kCrossHair = 2,
  kCustom = 3,
};

/// A point in unit space.
struct NabtsPoint {
  double x = 0.0;
  double y = 0.0;
};

/// A size in unit space. May be negative, which reflects the thing it sizes —
/// X3.110 §5.3.2.2.6 for the logical pel, §5.3.2.3.9 for a character field.
struct NabtsSize {
  double dx = 0.0;
  double dy = 0.0;
};

/// What a display-list entry draws.
enum class NabtsPrimitiveKind : uint8_t {
  /// A visible point, sized by the logical pel (§5.3.3.1).
  kPoint,
  /// A straight line between two points (§5.3.3.2).
  kLine,
  /// A circular arc through three points, or a spline through more
  /// (§5.3.3.3). |points| holds them in order: start, intermediate, end.
  kArc,
  /// An axis-aligned rectangle from an origin and a size (§5.3.3.4).
  kRectangle,
  /// A closed polygon through |points| (§5.3.3.5).
  kPolygon,
  /// One row of colours INCREMENTAL POINT deposited (§5.3.3.6.3), one logical
  /// pel apiece, from the drawing point |origin| in the direction of the pel's
  /// dx. |incremental_colours| holds them in deposit order, |logical_pel| is
  /// the pel the command ran with, and |points|[0] and |size| are the active
  /// field, which a receiver rescales the pel against.
  kIncrementalPoints,
  /// One character of a G-set, at |origin| in the character field of |size|.
  kCharacter,
  /// The display within the area at |origin| of |size| moves by |points|[0]
  /// (a displacement), and what moves out of the area is lost: INCREMENTAL
  /// POINT scrolling its active field (§5.3.3.6.3). |logical_pel| is the pel
  /// the displacement was measured in, rescaled by a receiver like the pel.
  kScroll,
};

/**
 * @brief One drawing operation, with the state that was in force when it ran
 *
 * Self-contained by design: a renderer walks the list front to back and needs
 * no state of its own. That costs a few dozen bytes per entry and buys a
 * display list that can be drawn, re-drawn, clipped or scaled without
 * re-running the interpreter.
 */
struct NabtsPrimitive {
  NabtsPrimitiveKind kind = NabtsPrimitiveKind::kPoint;

  /// Points the primitive needs, in unit space, already resolved from whatever
  /// mix of absolute and relative coordinates the record used. They may lie
  /// outside the unit screen: §5.3.1 lets a receiver execute such a drawing
  /// "and clip it", which is what a renderer does with them.
  std::vector<NabtsPoint> points;

  /// Origin for a rectangle or a character; the first of |points| otherwise.
  NabtsPoint origin;
  /// Extent for a rectangle, or the character field for a character.
  NabtsSize size;

  /// Filled rather than outlined (the "filled" forms of ARC, RECTANGLE and
  /// POLYGON, and INCREMENTAL POLYGON).
  bool filled = false;
  /// X3.110 §5.3.2.4.3: a filled figure whose outline is drawn solid in
  /// nominal black, or in the background colour in colour mode 2.
  bool highlighted = false;

  /// Logical pel in force (§5.3.2.2.6), which is what gives a line its width.
  NabtsSize logical_pel;
  NabtsLineTexture line_texture = NabtsLineTexture::kSolid;
  NabtsTexturePattern texture_pattern = NabtsTexturePattern::kSolid;
  /// Step-and-repeat size for the programmable masks (§5.3.2.4.5).
  NabtsSize texture_mask_size;

  NabtsColourMode colour_mode = NabtsColourMode::kDirect;
  /// Drawing colour, resolved through the colour map where it has a map
  /// address, against the map as it stood at the *end* of the record: X3.110
  /// §5.3.2.5 makes a map write retroactive — "A change in the color map will
  /// immediately be reflected in the color of all pixels whose associated
  /// color map address points to the color map entry that has been changed" —
  /// so the final map is the one that was on screen.
  NabtsColour colour;
  /// Background colour; meaningful only in colour mode 2. Resolved like
  /// |colour|.
  NabtsColour background;
  /// Colour map address |colour| was resolved from — in colour mode 0 too,
  /// which §5.3.2.5.1 has use the map as well — or -1 where the colour is held
  /// as a value. Carried so a consumer can tell a mapped colour from a held
  /// one; the resolution itself has already been done.
  int16_t colour_map_address = -1;
  /// Colour map address |background| was resolved from, or -1 outside mode 2.
  int16_t background_map_address = -1;

  /// When the primitive was drawn, in milliseconds from the start of the
  /// record's presentation: the WAIT intervals (X3.110 §5.3.2.8) executed
  /// before it. Blinking is not a property of the primitive — a blink process
  /// runs on a colour map entry (§5.3.2.7.2), and every pixel stored with that
  /// address follows it — so it is in NabtsPageSnapshot::timeline instead.
  uint32_t appear_ms = 0;
  /// How long the receiver took to draw the primitive, in milliseconds ending
  /// at |appear_ms|, where the page is played at a receiver's pace
  /// (NaplpsInterpreter::set_receiver_pace()); 0 where it appeared at once.
  /// What lets a filled figure be shown the way the sets drew one, a row at a
  /// time (NabtsPresentationTimeline::fill_sweep).
  uint32_t draw_ms = 0;

  /// kIncrementalPoints only: the colour of each pel of the row, in deposit
  /// order — a value in colour mode 0, and in modes 1 and 2 the entry of
  /// |incremental_addresses| resolved through the final map like |colour|.
  std::vector<NabtsColour> incremental_colours;
  /// The colour map address of each of |incremental_colours|, or -1 where the
  /// colour is a value (colour mode 0).
  std::vector<int16_t> incremental_addresses;

  // ---- kCharacter only ----------------------------------------------------

  /// The character's code position within its G-set, 0x20 to 0x7F.
  uint8_t character = 0;
  /// Which G-set the character came from, so a renderer knows which repertoire
  /// to look it up in.
  enum class Repertoire : uint8_t {
    kPrimary,        ///< X3.110 §7.1, the 94 ASCII-positioned graphics
    kSupplementary,  ///< §7.2, accents and symbols
    kMosaic,         ///< §5.4, the six-element block graphics
    kDrcs,           ///< §5.6, defined by DEF DRCS into |drcs_index|
  } repertoire = Repertoire::kPrimary;
  /// Index into NabtsPageSnapshot::drcs when |repertoire| is kDrcs.
  uint8_t drcs_index = 0;

  NabtsCharRotation rotation = NabtsCharRotation::kNone;
  /// The character path in force (§5.3.2.3.3), i.e. the direction the cursor
  /// moved to the next character field. Carried because a display list of one
  /// primitive per character loses it otherwise, and a consumer laying the
  /// characters back out as a run needs it.
  NabtsCharPath path = NabtsCharPath::kRight;
  /// Reverse video (§6.2.7.4): the character shape is left undrawn and the
  /// field around it is filled instead.
  bool reverse_video = false;
  /// Underline mode (§6.2.7.15).
  bool underlined = false;
};

/**
 * @brief A DRCS character's storage buffer (X3.110 §6.2.3)
 *
 * DEF DRCS runs presentation code into a buffer rather than onto the screen,
 * and every element the code writes comes on unless it was written in nominal
 * black. The buffer's aspect ratio is the character field's at the moment DEF
 * DRCS arrived, so it is carried with it.
 *
 * The elements are the resolved bitmap rather than the code that made it,
 * because a renderer scaling a DRCS character into a character field needs the
 * former and could not use the latter.
 */
struct NabtsDrcsCharacter {
  /// Code position this was defined at, 0x20 to 0x7F.
  uint8_t code = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  /// Row-major, |width| * |height| entries, true for an element that is on.
  /// The first row is the bottom of the buffer, matching unit space.
  std::vector<bool> elements;

  bool defined() const {
    return width > 0 && height > 0 &&
           elements.size() == static_cast<size_t>(width) * height;
  }
};

/// One programmable texture mask (X3.110 §6.2.4). Table D1 item 5(3)(b)
/// requires 16x16 stored elements, and item 11 keeps that storage outside the
/// shared budget.
struct NabtsTextureMask {
  uint16_t width = 0;
  uint16_t height = 0;
  std::vector<bool> elements;

  bool defined() const {
    return width > 0 && height > 0 &&
           elements.size() == static_cast<size_t>(width) * height;
  }
};

/// Programmable texture masks A to D.
constexpr size_t kNabtsTextureMaskCount = 4;

/// What the interpreter could not do with the record it was given.
struct NabtsDecodeDiagnostics {
  /// PDI sequences whose operands ran out before the opcode had what it needed.
  uint64_t truncated_pdis = 0;
  /// Polygons, arcs and splines not drawn because a vertex lies outside the
  /// unit screen — the other of §5.3.1's two handlings, "rejected (ie,
  /// executed as a null operation)", which is what the Panasonic TU-2000x
  /// does with them.
  uint64_t rejected_figures = 0;
  /// Controls and PDI opcodes not executed because they arrived failing the
  /// odd parity CEA-516 §3.3 gives every data byte: known to be wrong, so the
  /// clear, shift or drawing command they read as is not run. The Panasonic
  /// TU-2000x skips them as it draws, and the Norpak TTX 650 never passes a
  /// byte failing parity on at all.
  uint64_t parity_skipped = 0;
  /// Macro invocations that could not run: undefined, or nested past the depth
  /// bound.
  uint64_t unresolved_macros = 0;
  /// Definitions refused because the shared macro and DRCS budget of CEA-516
  /// §8.6.1 and T.101 Table II-3 — 3072 bytes — was full.
  uint64_t storage_refusals = 0;

  // What a reconstruction of the bytes the recording lost did before the
  // record was run (see naplps_reconstruct.h), all zero for a page presented
  // as recovered. The interpreter never sets these — it is not told the record
  // was rebuilt — so they are stamped on by whoever ran both (see
  // naplps_stamp_reconstruction_diagnostics).

  /// Bytes no copy of the record delivered, before anything was done.
  uint64_t lost_bytes = 0;
  /// Of those, bytes copied in from another record whose bytes around them
  /// agree, and bytes put back because the reading of what follows calls for
  /// them (a shift).
  uint64_t filled_bytes = 0;
  uint64_t inferred_bytes = 0;
  /// Bytes that arrived failing their parity, known to be wrong, and of those
  /// the ones replaced by what another record agreeing around them has there.
  uint64_t corrupt_bytes = 0;
  uint64_t corrected_bytes = 0;
  /// The recordings the filled and corrected bytes were copied from.
  std::vector<std::string> fill_sources;
};

// ---- Presentation time ----------------------------------------------------
//
// A record is a program that runs in time as well as in space: WAIT
// (§5.3.2.8) pauses it part way through, and a blink process (§5.3.2.7)
// rewrites a colour map entry on a cycle of its own for as long as the page is
// shown. The display list above is where things are; this is when.

/// A time that never comes, for a blink process still running at the end.
constexpr int64_t kNabtsNeverMs = std::numeric_limits<int64_t>::max();

/**
 * @brief One blink process (X3.110 §5.3.2.7)
 *
 * "This process periodically overwrites the contents of the current in-use
 * drawing color (the "blink-from" color) and substitutes the current contents
 * of another entry in the color map ... The ON and OFF intervals alternate
 * with each other, starting with the ON interval."
 *
 * Times are milliseconds from the start of the record's presentation. A
 * process carried in from an earlier record — a Support Record's, or a More
 * chain predecessor's — was defined before this one began, so its times are
 * negative.
 */
struct NabtsBlinkProcess {
  /// The blink-from colour map entry.
  uint8_t from = 0;
  /// The blink-to colour map entry, or -1 where the process names a colour
  /// instead: §6.2.8.1's BLINK START blinks to nominal black in colour modes 0
  /// and 1, which is a colour rather than an address.
  int16_t to_address = -1;
  /// The blink-to colour where |to_address| is -1.
  NabtsColour to_colour;
  uint32_t on_ms = 0;
  uint32_t off_ms = 0;
  /// When the process was defined.
  int64_t defined_ms = 0;
  /// The start of an ON interval, which fixes the process's phase: its ON
  /// intervals start here and every on_ms + off_ms either side of it. Later
  /// than |defined_ms| when a start delay put the first ON interval off. A
  /// delay measured from an ON interval another process is already part way
  /// through (§5.3.2.7.2) is counted from that process's next ON interval
  /// where it would otherwise have gone by, as both receivers of the day
  /// count it.
  int64_t first_on_ms = 0;
  /// When the process was terminated, or kNabtsNeverMs.
  int64_t ended_ms = kNabtsNeverMs;

  uint32_t period_ms() const { return on_ms + off_ms; }
};

/**
 * @brief Something the record did to the colour map or its blink processes
 *
 * In execution order. Order matters within one instant as well as across
 * them: a SET COLOR on an entry a blink process has just overwritten puts the
 * entry's colour back, and the other way round leaves it blinking.
 */
struct NabtsPaletteEvent {
  enum class Kind : uint8_t {
    /// |address| was loaded with |colour| (SET COLOR, and the map reloads of
    /// RESET and the caption preset).
    kMapWrite,
    /// NabtsPresentationTimeline::blinks[|process|] started running.
    kBlinkStart,
    /// NabtsPresentationTimeline::blinks[|process|] was terminated.
    kBlinkEnd,
  };

  Kind kind = Kind::kMapWrite;
  int64_t time_ms = 0;
  uint8_t address = 0;
  NabtsColour colour;
  uint32_t process = 0;
};

/**
 * @brief A clear of the display area (CS, and RESET's screen actions)
 *
 * Everything drawn before it goes. Its position is in the paint stream —
 * NabtsPresentationTimeline::superseded followed by NabtsPageSnapshot::
 * primitives — so a renderer replaying the page knows which primitives it
 * removed: the first |stream_index| of them.
 */
struct NabtsDisplayClear {
  /// When the display was clear, in milliseconds from the start of the
  /// record's presentation.
  uint32_t time_ms = 0;
  size_t stream_index = 0;
  /// How long the clear took the receiver, ending at |time_ms|, where the page
  /// is played at a receiver's pace; 0 where it was instant.
  uint32_t draw_ms = 0;
};

/**
 * @brief Which way a receiver swept a fill or a clear across its screen
 *
 * The sets of the day filled a figure and cleared the display a row at a time,
 * so a large one was seen to sweep across the screen: down it from the top on
 * the Panasonic TU-2000x, up it from the bottom on the Norpak TTX 650.
 */
enum class NabtsFillSweep : uint8_t {
  kTopDown,
  kBottomUp,
};

/**
 * @brief When a record's drawing happened and how its colours change
 *
 * Enough to replay the page as a receiver showed it, without running the
 * program again: the primitives each carry their appear time, the clears say
 * what went when, and the colour map's history is its starting contents plus
 * the events. Working out the colours a blink process produces at a given
 * moment is a renderer's business, and a pure function of this.
 */
struct NabtsPresentationTimeline {
  /// The colour map when the record began, before any of its own writes.
  NabtsColour initial_map[kNabtsColourMapEntries];
  /// Map writes and blink process starts and ends, in execution order.
  std::vector<NabtsPaletteEvent> events;
  /// Every blink process that ran during the record, in the order defined.
  std::vector<NabtsBlinkProcess> blinks;
  /// Primitives a clear removed, in the order they were drawn. With
  /// NabtsPageSnapshot::primitives after them they are the whole paint stream.
  std::vector<NabtsPrimitive> superseded;
  /// The clears, in execution order.
  std::vector<NabtsDisplayClear> clears;
  /// When the record finished: the total of its WAIT intervals.
  uint32_t end_ms = 0;
  /// Which way the receiver whose pace the record was played at swept a fill
  /// or a clear across its screen, over the time NabtsPrimitive::draw_ms and
  /// NabtsDisplayClear::draw_ms give it.
  NabtsFillSweep fill_sweep = NabtsFillSweep::kTopDown;
};

/**
 * @brief A decoded NAPLPS presentation record
 *
 * The display list is in execution order, which is also back-to-front paint
 * order: NAPLPS has no z-ordering, so a later primitive covers an earlier one.
 *
 * A value type with no decoder types in its interface, so it crosses the
 * plugin boundary and reaches a presenter unchanged.
 */
struct NabtsPageSnapshot {
  /// What is on screen once the record has finished drawing.
  std::vector<NabtsPrimitive> primitives;

  /// The colour map as it stood at the end of the record. A renderer needs it
  /// for colour modes 1 and 2, where a map change is retroactive: §5.3.2.5 has
  /// a write "immediately reflected in the colour of all pixels whose
  /// associated colour map address points to the entry that has been changed",
  /// so the final map is the one that was on screen.
  NabtsColour colour_map[kNabtsColourMapEntries];

  /// DRCS characters the record defined, ascending by code position.
  std::vector<NabtsDrcsCharacter> drcs;
  /// Programmable texture masks A to D.
  NabtsTextureMask texture_masks[kNabtsTextureMaskCount];

  /// How the page was drawn over time, and how its colours change.
  NabtsPresentationTimeline timeline;

  /// Whether the record drew anything at all. A record that only defined
  /// macros or DRCS is legitimately empty.
  bool empty() const { return primitives.empty(); }

  NabtsDecodeDiagnostics diagnostics;
};

/// The default colour map of X3.110 §5.3.2.5.2, as T.101 Table II-3 tabulates
/// it for Data Syntax III: a uniform grey ramp in the low half and eight hues
/// equally spaced around the hue circle in the high half.
void nabts_default_colour_map(NabtsColour (&map)[kNabtsColourMapEntries]);

/// Nominal black and nominal white, which several rules of the standard name
/// specifically (§5.3.2.4.3 highlight, §6.2.3 DRCS elements, §6.2.8.1 blink).
constexpr NabtsColour kNabtsNominalBlack{0, 0, 0, false};
constexpr NabtsColour kNabtsNominalWhite{kNabtsGunMax, kNabtsGunMax,
                                         kNabtsGunMax, false};
/// The transparent colour (§5.3.2.5), SET COLOR's with no operand.
constexpr NabtsColour kNabtsTransparent{0, 0, 0, true};

// ---- Character repertoires ------------------------------------------------
//
// A kCharacter primitive names a code position and the G-set it belongs to;
// what that position *is* comes from X3.110 §7. The mapping lives here rather
// than in a presenter because two consumers need the same answer: the host's
// viewer, which draws it, and the sink stage, which writes caption text.

/**
 * @brief The primary set (X3.110 §5.1, §7) as a Unicode code point
 *
 * §7.2 puts the primary set's graphics at 2/1 to 7/14 and bases the repertoire
 * on ANSI X3.4-1977, so the set is ASCII: Table 25 places the number sign at
 * 2/3, the dollar sign at 2/4, the grave accent at 6/0, the circumflex at 5/14
 * and the tilde at 7/14, each where ASCII has it. 2/0 is SPACE; 7/15 is not a
 * graphic and reads as SPACE.
 */
char32_t nabts_primary_to_unicode(uint8_t code);

/**
 * @brief The supplementary set (X3.110 §5.2, §7) as a Unicode code point
 *
 * The layout is the ISO 6937-1982 supplementary set §7.2 names as a source,
 * with the additions X3.110 makes at 5/6 to 5/11 and 6/5 (Table 25: the full
 * horizontal and vertical lines, the four diagonals and the cross, which ISO
 * 6937 leaves vacant or uses differently).
 *
 * Columns 4 is the non-spacing diacriticals of Tables 26 and 27, returned as
 * the corresponding Unicode combining mark: a composite character is
 * transmitted as the mark followed by the letter (§7.2), which is the opposite
 * of Unicode's order, so a consumer assembling text has to swap them.
 * Unassigned positions read as SPACE.
 */
char32_t nabts_supplementary_to_unicode(uint8_t code);

/// True when |code| is one of the non-spacing marks of Tables 26 and 27, which
/// precede the character they apply to instead of following it.
bool nabts_supplementary_is_nonspacing(uint8_t code);

/**
 * @brief Whether two characters were deposited into the same character field
 *
 * A non-spacing mark leaves the cursor where it found it, so the letter it
 * applies to is the next character deposited into the same field (X3.110 §7.2,
 * Tables 26 and 27): the same origin, size and rotation. The pixel raster, the
 * vector view and the page's text all compose marks by this one test, so a
 * mark sits on the same letter whichever of them draws it. Positions are
 * compared to a millionth of the unit screen, a thousandth of the finest
 * grid's cell and far above the rounding of resolving a relative coordinate.
 */
bool nabts_same_character_field(const NabtsPrimitive& a,
                                const NabtsPrimitive& b);

/**
 * @brief The six sub-elements a mosaic code position lights (X3.110 §5.4)
 *
 * Figure 62 numbers the 2x3 cell b1 top-left, b2 top-right, b3 middle-left,
 * b4 middle-right, b5 bottom-left and b7 bottom-right, with b6 = 1 marking the
 * position as a mosaic — so the mosaics are columns 2, 3, 6 and 7, plus the
 * second copy of the solid mosaic §5.4 places at 5/15.
 *
 * Returned as six bits in the same order the World System Teletext sixels use
 * (bit 0 top-left through bit 5 bottom-right), because it is the same 2x3 cell:
 * a renderer draws either from one routine.
 */
uint8_t nabts_mosaic_sixels(uint8_t code);

/// Whether |code| is one of the 65 mosaic positions §5.4 assigns; the rest are
/// reserved and "shall be displayed as SPACE".
bool nabts_is_mosaic_code(uint8_t code);

/// |code| as UTF-8. Hand-rolled rather than pulled from <codecvt>, which is
/// deprecated; the repertoire reaches only three bytes.
std::string nabts_code_point_to_utf8(char32_t code);

/**
 * @brief One character of |repertoire| as UTF-8, for text rather than a glyph
 *
 * Used where the character is being read rather than drawn — a caption cue, a
 * record's text content. A mosaic or a DRCS character has no text form and
 * yields a SPACE, which is what §5.4 and §5.6 make an undrawable position
 * anyway.
 */
std::string nabts_character_to_utf8(uint8_t code,
                                    NabtsPrimitive::Repertoire repertoire);

/**
 * @brief A decoded record's text content, in reading order
 *
 * NAPLPS has no rows — a character goes wherever the cursor was — so the rows
 * are inferred from where the record put the characters: runs sharing a
 * baseline within half a character height are one line, lines run down the
 * screen (y decreasing, since unit space has y upwards), and a line reads left
 * to right. Lines are separated by a newline.
 *
 * For reading a record back rather than drawing it: a caption cue, a search, a
 * text pane beside the rendering. Block mosaics and DRCS characters are shapes
 * rather than text and are left out; the non-spacing marks of X3.110 Tables 26
 * and 27 are composed onto the letters that follow them, which is Unicode's
 * order and the reverse of the transmission's.
 *
 * It is read a screen at a time. A run of characters — a word, a line, sent
 * one after another along a baseline — that a filled rectangle or polygon
 * drawn after it mostly covers was taken off the screen by it: a timed page's
 * earlier messages, an index's opening headlines, a More Record's predecessor.
 * Each such figure's runs are read as a block of their own before the words
 * that replaced them, blocks in the order the figures were drawn and a blank
 * line between, then what the display holds once the record is done. A run
 * drawn again where the same characters already stand — a shadow a pixel off,
 * a redraw in another colour — is read once.
 */
std::string nabts_page_text(const NabtsPageSnapshot& page);

}  // namespace orc

#endif  // ORC_NABTS_PAGE_H
