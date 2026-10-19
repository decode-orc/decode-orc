/*
 * File:        naplps_state.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     The NAPLPS presentation state: attributes, colour map, macros,
 *              DRCS and the storage budget they share
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NAPLPS_STATE_H
#define ORC_NAPLPS_STATE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "naplps_pdi.h"
#include "naplps_render_grid.h"
#include "vbi-services/nabts_page.h"

namespace orc {

/**
 * @file
 * @brief Everything a NAPLPS primitive inherits from what came before it
 *
 * X3.110 defines its defaults per command rather than in one place, which makes
 * "what does a freshly reset interpreter look like?" a question the standard
 * does not answer directly. ITU-T T.101 Table II-3 does: it tabulates the
 * Data Syntax III default state as a single list, and every default here is
 * cross-checked against it.
 */

/// §5.5 and §6.2.2: 96 macro names, the code positions 2/0 to 7/15.
constexpr size_t kNaplpsMacroCount = 96;
/// §5.6 and §6.2.3: 96 DRCS characters, at the same code positions.
constexpr size_t kNaplpsDrcsCount = 96;
/// First code position of a 96-position set.
constexpr uint8_t kNaplpsFirstCode = 0x20;
constexpr uint8_t kNaplpsLastCode = 0x7F;

/**
 * @brief Shared storage for macro definitions and DRCS characters
 *
 * CEA-516 §8.6.1 and T.101 Table II-3 both give 3072 bytes, shared. Table D1
 * item 11 states the accounting for the teletext service: "The storage of macro
 * definitions requires one byte for each byte of defining code excluding the
 * DEF and terminating codes", and the DRCS assumption is that 96 normal-size
 * characters fit and still leave 2048 bytes for macros — so a DRCS character
 * costs (3072 - 2048) / 96 bytes, rounded up to 11.
 *
 * The budget is enforced rather than assumed. A record that overruns it is a
 * record a conforming receiver would also have refused, and silently accepting
 * it would show a page no viewer ever saw.
 */
constexpr size_t kNaplpsSharedStorageBytes = 3072;

/// Table D1 item 5(5): "Sixteen blink processes shall be available."
constexpr size_t kNaplpsMaxBlinkProcesses = 16;

/// §5.5: the depth to which a macro may invoke another macro. The standard
/// bounds this only by memory; a decoder needs a hard stop, because a macro
/// that invokes itself is expressible and §6.2.2.2 only rules it out for DEFP
/// MACRO.
constexpr size_t kNaplpsMaxMacroDepth = 8;

/// The DOMAIN state of §5.3.2.2.
struct NaplpsDomainState {
  NaplpsOperandFormat format;
  /// §5.3.2.2.6: the "brush" a drawing primitive is traced with, which is what
  /// gives a line its width. Default 0,0 — a dimensionless drawing point.
  NabtsSize logical_pel{0.0, 0.0};

  void reset() {
    format = NaplpsOperandFormat{};
    logical_pel = NabtsSize{0.0, 0.0};
  }
};

/// The TEXT state of §5.3.2.3, plus the C1 text controls of §6.2.7 that write
/// to the same parameters.
struct NaplpsTextState {
  NabtsCharRotation rotation = NabtsCharRotation::kNone;
  NabtsCharPath path = NabtsCharPath::kRight;
  /// §5.3.2.3.4 Table 8: 1, 5/4, 3/2, or proportional. Held as the code, since
  /// the proportional case has no numeric value.
  uint8_t intercharacter_spacing = 0;
  /// §5.3.2.3.5 Table 9: 1, 5/4, 3/2, 2.
  uint8_t interrow_spacing = 0;
  /// §5.3.2.3.7 Table 10.
  uint8_t move_attribute = 0;
  NabtsCursorStyle cursor_style = NabtsCursorStyle::kUnderscore;
  /// §5.3.2.3.9: default dx = 1/40, dy = 5/128, which T.101 Table II-3 gives as
  /// the basic-char-size-state.
  NabtsSize character_field{1.0 / 40.0, 5.0 / 128.0};

  /// §6.2.7.4: characters drawn as the field around their shape.
  bool reverse_video = false;
  /// §6.2.7.15.
  bool underlined = false;
  /// §6.2.7.13.
  bool scroll = false;

  void reset() { *this = NaplpsTextState{}; }

  /// The character field sizes the C1 text controls select (§6.2.7.6-10).
  static NabtsSize small_field() { return {1.0 / 80.0, 5.0 / 128.0}; }
  static NabtsSize medium_field() { return {1.0 / 32.0, 3.0 / 64.0}; }
  static NabtsSize normal_field() { return {1.0 / 40.0, 5.0 / 128.0}; }
  static NabtsSize double_height_field() { return {1.0 / 40.0, 5.0 / 64.0}; }
  static NabtsSize double_size_field() { return {1.0 / 20.0, 5.0 / 64.0}; }
};

/// The TEXTURE state of §5.3.2.4.
struct NaplpsTextureState {
  NabtsLineTexture line_texture = NabtsLineTexture::kSolid;
  NabtsTexturePattern pattern = NabtsTexturePattern::kSolid;
  bool highlight = false;
  /// §5.3.2.4.5: default dx = 1/40, dy = 5/128 — the default character field,
  /// which Table II-3 gives as "texture mask = 1/40, 5/128".
  NabtsSize mask_size{1.0 / 40.0, 5.0 / 128.0};

  void reset() { *this = NaplpsTextureState{}; }
};

/**
 * @brief The colour state of §5.3.2.5 and §5.3.2.6
 *
 * Three modes with three different meanings for the same two opcodes, which is
 * most of what makes NAPLPS colour awkward. In mode 0 SET COLOR carries a value
 * and SELECT COLOR is unused; in modes 1 and 2 SELECT COLOR carries a map
 * address and SET COLOR writes the value at it.
 */
class NaplpsColourState {
 public:
  NaplpsColourState() { reset(); }

  /// Back to Table II-3's current-foreground-colour: white, mode direct.
  void reset();

  /// Reload the default colour map of §5.3.2.5.2 without touching the mode.
  void reset_map();

  /// §6.1.6.5(5), the NSR case: "The color mode is set to color mode 0 and the
  /// drawing color is set to nominal white. The color map is not changed." A
  /// plain set_colour(white) would touch the map's used-entry accounting, which
  /// NSR must not. The drawing address follows the colour to the entry holding
  /// white, since mode 0 stores what it draws by address too.
  void reset_drawing_to_white();

  NabtsColourMode mode() const { return mode_; }

  /// §5.3.2.6: no operand selects mode 0, one selects mode 1, two select
  /// mode 2.
  void select_direct_mode();
  void select_mapped_mode(uint32_t drawing_address);
  void select_mapped_background_mode(uint32_t drawing_address,
                                     uint32_t background_address);

  /**
   * @brief RESET's colour-mode actions into mode 1 (§5.3.2.9.2, Table 14)
   *
   * Reloads the default map and selects mode 1. With @p drawing_white the
   * in-use drawing colour is also set to white, which in a mapped mode means
   * pointing it at the lowest entry holding white, as a mode-0 SET COLOR of a
   * colour already in the map does (§5.3.2.5.1: "the color map is not
   * changed"). Writing white at the drawing address instead would overwrite
   * the nominal black at entry 0 that the reset has just restored, and a page
   * drawing its outlines in entry 0 would have them all come out white. The
   * drawing address is otherwise kept: Table 14 selects the mode, not an
   * entry.
   */
  void reset_to_mapped_mode(bool drawing_white);

  /// §5.3.2.5: in mode 0 this sets the drawing colour and finds or claims a map
  /// entry for it; in modes 1 and 2 it writes the value at the drawing address.
  void set_colour(const NabtsColour& colour);

  /// A SET COLOR with no operand, which §5.3.2.5.1 makes the transparent
  /// colour.
  void set_transparent();

  /// The colour a primitive drawn now is drawn in.
  NabtsColour drawing_colour() const;
  /// The background, which is meaningful only in mode 2.
  NabtsColour background_colour() const;

  /// X3.110 §2 definitions: "Nominal black means the color black (all zeros)
  /// in color mode zero, or the color that is at color map address zero in
  /// color modes 1 and 2" — so a display cleared or blinked to nominal black in
  /// those modes follows a later write to entry 0, as both receivers of the
  /// day show it. The address is -1 in mode 0, where there is none.
  NabtsColour nominal_black() const;
  int16_t nominal_black_address() const {
    return mode_ == NabtsColourMode::kDirect ? int16_t{-1} : int16_t{0};
  }

  /// The map address the drawing colour currently points at. Meaningful in
  /// modes 1 and 2, where §5.3.2.5 makes a later map write at this address
  /// retroactive over what was drawn with it.
  uint32_t drawing_address() const {
    return drawing_address_ % kNabtsColourMapEntries;
  }
  /// The map address of the mode-2 background colour.
  uint32_t map_background_address() const {
    return background_address_ % kNabtsColourMapEntries;
  }

  /**
   * @brief The map address a pixel drawn now stores, or -1 for none
   *
   * §5.3.2.5.1: "Color mode 0 also uses the color map" — mode 0 finds the
   * entry holding its colour, or claims one, so what a receiver stores for a
   * mode-0 pixel is an address like any other, and a later write to that
   * entry or a blink process on it reaches the pixel. Where mode 0 found no
   * entry (the map was full, or the colour has since been overwritten there)
   * the colour is held as a value, and -1 says so.
   */
  int16_t drawing_map_address() const;

  /**
   * @brief Give the mode-0 drawing colour an entry of its own (§6.2.8.1)
   *
   * "In color mode 0 the C1 BLINK START command establishes an automatic
   * process that implicitly defines a blinking color. Note that objects drawn
   * with the previous drawing color will remain not blinking, but objects
   * subsequently drawn with the new drawing color will blink." Blinking the
   * entry the colour already had would blink everything drawn in it before,
   * so the colour is copied into the lowest entry mode 0 could claim and the
   * drawing address moved there. Where none is free the address is left
   * alone, and the process then blinks what is already there.
   */
  void claim_entry_for_drawing_colour();

  /// Write |colour| at |address| directly, marking the entry used. This is the
  /// implicit-repeat write of §5.3.2.5.1, which walks addresses of its own
  /// without moving the drawing colour's, and the caption preset of CEA-516
  /// §5.2.7.3, which loads three stated entries.
  void write_map_entry(uint32_t address, const NabtsColour& colour);

  const NabtsColour* map() const { return map_; }
  void copy_map_to(NabtsColour (&out)[kNabtsColourMapEntries]) const;

  /// One write to the map, as take_writes() reports it.
  struct MapWrite {
    uint8_t address = 0;
    NabtsColour colour;
  };

  /**
   * @brief The map writes made since the last call, oldest first
   *
   * Every write is logged, whatever made it — SET COLOR in any mode, mode 0
   * claiming an entry, RESET's map reload, the caption preset — so that the
   * interpreter can put each on its presentation clock: §5.3.2.5's retroactive
   * map is a map that changes on screen.
   */
  std::vector<MapWrite> take_writes();

  /// §5.3.2.5.1's implicit-repeat address increment: "change the most
  /// significant zero to a one and to change all ones to the left of it to
  /// zero", which walks the map in an order that degrades gracefully on a
  /// receiver with fewer entries. Returns false at the physical limit.
  static bool increment_map_address(uint32_t& address);

  /// The map address in the |bit_count| bits of |bits|: §5.3.2.6.1
  /// left-justifies an address in its operand, so only the high N bits are
  /// significant, and fewer bits than the address needs have trailing zeroes
  /// supplied.
  static uint32_t map_address(uint64_t bits, size_t bit_count);

 private:
  /// Lowest map entry holding |colour|, or kNabtsColourMapEntries.
  size_t find_in_map(const NabtsColour& colour) const;

  /// Lowest entry mode 0 may claim for a new colour (§5.3.2.5.1), or
  /// kNabtsColourMapEntries when there is none.
  size_t claimable_entry() const;

  /// Load |colour| into entry |index|, logging the write.
  void store(size_t index, const NabtsColour& colour);

  NabtsColourMode mode_ = NabtsColourMode::kDirect;
  NabtsColour map_[kNabtsColourMapEntries]{};
  /// Mode 0's directly specified colour.
  NabtsColour direct_colour_ = kNabtsNominalWhite;
  /// Modes 1 and 2.
  uint32_t drawing_address_ = 0;
  uint32_t background_address_ = 0;
  /// §5.3.2.5.1: entries used since the last map-restoring RESET, which is what
  /// mode 0 consults to find "the lowest address that has not been used".
  bool used_[kNabtsColourMapEntries]{};
  /// Writes not yet collected by take_writes().
  std::vector<MapWrite> writes_;
};

/// One stored macro definition (§6.2.2).
struct NaplpsMacro {
  /// The defining code, verbatim. §6.2.2.1: "a reference to a macro results in
  /// the storage of that reference only, not the expansion", so this is stored
  /// unexpanded and expanded at invocation.
  std::vector<uint8_t> code;
  /// §6.2.2.3: a transmit macro is transmitted rather than executed, so an
  /// invocation of one draws nothing.
  bool transmit = false;

  /// Whether a definition is stored: §6.2.2.1 has a null definition delete
  /// the macro, so a defined macro is never empty.
  bool defined() const { return !code.empty(); }
};

/**
 * @brief The whole presentation state, and the storage the record may define
 *
 * Reset values are Table II-3's throughout. RESET itself is selective
 * (§5.3.2.9), so its bits are applied through the individual reset methods
 * rather than by rebuilding the object.
 */
class NaplpsState {
 public:
  NaplpsState() { reset_all(); }

  /**
   * @brief The receiver resolution DRCS storage is sized against
   *
   * §6.2.3 sizes a DRCS buffer from the physical resolution its character field
   * covers, so the grid is a property of the receiver being emulated rather
   * than of the presentation. RESET and NSR leave it alone for that reason.
   */
  void set_render_grid(NaplpsRenderGrid grid) { render_grid_ = grid; }
  const NaplpsRenderGrid& render_grid() const { return render_grid_; }

  /// Everything to its default, the colour map, macros, DRCS and programmable
  /// masks included: the receiver's general reset (CEA-516 §8.5).
  void reset_all();

  /// The home position of X3.110 §6.1.2.6: the upper left character position of
  /// the display area. Depends on the current character field, so it moves with
  /// the text size.
  NabtsPoint home_position() const;

  NaplpsDomainState domain;
  NaplpsTextState text;
  NaplpsTextureState texture;
  NaplpsColourState colour;

  /// §5.3.1: the point a geometric primitive starts from.
  NabtsPoint drawing_point{0.0, 0.0};
  /// §6.1.2: the character field origin, which Table II-3 puts at the lower
  /// left corner.
  NabtsPoint cursor{0.0, 0.0};

  /// §5.3.3.6.2: the active field, which defaults to the whole unit screen.
  NabtsPoint field_origin{0.0, 0.0};
  NabtsSize field_size{1.0, 1.0};
  /// The default active field: the whole unit screen, from (0,0)
  /// (§5.3.3.6.2).
  void reset_active_field() {
    field_origin = NabtsPoint{0.0, 0.0};
    field_size = NabtsSize{1.0, 1.0};
  }

  /**
   * @brief The blink processes running (X3.110 §5.3.2.7), oldest first
   *
   * A blink process is attached to a map *entry*, so what blinks is everything
   * drawn in that entry, whenever it was drawn; it is not a mode that
   * everything after the command inherits. §6.2.8.1 says so of the C1 form
   * outright: "If the drawing color is changed, the old color remains blinking
   * and the new drawing color does not blink."
   *
   * Kept in the order defined, because the standard orders by it twice: a
   * start delay is measured from "the most recently defined active blink
   * process", and processes that fall due together run "starting with the
   * most recently defined". Times are on the clock of the record running now;
   * NaplpsInterpreter::run() moves a process carried in from the record before
   * back by that record's length.
   */
  struct Blink {
    NabtsBlinkProcess process;
    /// The process's place in the running record's timeline.
    uint32_t timeline_index = 0;
  };
  std::vector<Blink> blinks;

  // ---- Storage -------------------------------------------------------------

  /// Macro at |code|, or nullptr for a code outside 2/0 to 7/15.
  const NaplpsMacro* macro(uint8_t code) const;

  /**
   * @brief Store |body| as the macro at |code|
   *
   * §6.2.2.1: "A null macro definition ... causes that macro to be deleted",
   * and a definition replaces whatever was there. Returns false when the shared
   * budget could not take it, in which case nothing is stored.
   */
  bool define_macro(uint8_t code, std::vector<uint8_t> body, bool transmit);

  /// §5.3.2.9.3 b5: clear every macro, transmit macros included.
  void clear_macros();

  /// Reserve buffer space for a DRCS character at |code|, sized from the
  /// character field per §6.2.3. Returns nullptr when the budget is full.
  NabtsDrcsCharacter* begin_drcs(uint8_t code, uint16_t width, uint16_t height);

  /// §5.3.2.9.3 b6: clear every DRCS character.
  void clear_drcs();

  /// §6.2.3: "If a DRCS definition is immediately terminated with no
  /// intervening presentation layer code, the buffer space allocated to that
  /// character is freed." Undefines the character and refunds its storage.
  void free_drcs(uint8_t code);

  /// The DRCS characters actually defined, ascending by code.
  std::vector<NabtsDrcsCharacter> defined_drcs() const;

  NabtsTextureMask texture_masks[kNabtsTextureMaskCount];

  /// Bytes of the shared budget in use.
  size_t storage_used() const { return storage_used_; }
  /// Whether |bytes| more would fit.
  bool storage_available(size_t bytes) const {
    return storage_used_ + bytes <= kNaplpsSharedStorageBytes;
  }

  /// §6.2.3: the DRCS storage buffer's aspect ratio is the character field's,
  /// at the physical resolution the field covers on the receiver being
  /// emulated — see set_render_grid().
  void drcs_buffer_size(uint16_t& width, uint16_t& height) const;

 private:
  /// Index of |code| within a 96-position set, or kNaplpsMacroCount if outside.
  static size_t index_of_code(uint8_t code);

  NaplpsRenderGrid render_grid_ = kNaplpsGridReference;
  std::array<NaplpsMacro, kNaplpsMacroCount> macros_{};
  std::array<NabtsDrcsCharacter, kNaplpsDrcsCount> drcs_{};
  size_t storage_used_ = 0;
};

}  // namespace orc

#endif  // ORC_NAPLPS_STATE_H
