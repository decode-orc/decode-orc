/*
 * File:        naplps_interpreter.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Run a NAPLPS presentation record into a resolved display list
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NAPLPS_INTERPRETER_H
#define ORC_NAPLPS_INTERPRETER_H

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "naplps_code_env.h"
#include "naplps_pdi.h"
#include "naplps_receiver_pace.h"
#include "naplps_state.h"
#include "vbi-services/nabts_page.h"

namespace orc {

/**
 * @brief How a receiver read one byte of a record
 *
 * What NaplpsInterpreter::set_trace() records, byte for byte of the record as
 * sent: whether a byte was text, PDI data or a control is decided by
 * everything before it (the code environment of §4.3, the operand rules of
 * §5.3.1), so it takes the whole walk to say — which is what reading the
 * stretch a recording lost needs, and what learning how a service writes
 * needs.
 */
enum class NaplpsByteRole : uint8_t {
  /// Never reached: the record ended inside the command it belongs to.
  kUnread,
  /// A C0 control, an escape sequence or C1 control, or bytes one of them took
  /// after itself (the row and column of APS or NSR, REPEAT's count, the code a
  /// definition names).
  kControl,
  /// A transmission or device control or NUL, which "have no effect on the
  /// presentation layer" (§6.1.4-6.1.6.1), or a C0 control that arrived
  /// failing its parity (CEA-516 §3.3): dropped as the record is read.
  kTransparent,
  /// A character of the primary, supplementary, mosaic or DRCS set.
  kText,
  /// A PDI opcode or its numeric data (§5.3.1).
  kPdi,
  /// Stored into a macro definition rather than executed (§6.2.2.1).
  kDefinition,
  /// A macro invoked from the macro set (§5.5), or a byte of a null set.
  kOther,
};

/// The keys a receiver links to macros: the ten digits of its keypad, digit n
/// to macro 2/n. X3.110 §5.5 lets "any macro ... be linked to a user input
/// mechanism, such as a function key", and Table D1 item 7 has the linkages
/// start at 2/0 — a videotex receiver needs "a minimum of eight such linkages
/// ... to activate macros 2/0 to 2/7", and "it is recommended that the service
/// provider define nonlinked macros starting at 7/15". Game pages define
/// macros 2/0 to 2/9 and tell the viewer to "Press Option 1-9"; the
/// Panasonic TU-2000x runs macro 2/n for the keys "n #".
constexpr uint8_t kNaplpsLinkedKeyCount = 10;
constexpr uint8_t kNaplpsFirstLinkedMacro = 0x20;

/**
 * @brief Runs a NAPLPS presentation record and emits what it drew
 *
 * One instance per *service*, not per record: macros, DRCS characters, the
 * programmable texture masks and the colour map persist across records unless
 * a RESET clears them. CEA-516 §8.7.2.3.1 spells out what the recommended
 * per-page NSR leaves alone — "without affecting macros, DRCS, texture
 * definitions, color map or the currently displayed image" — and §8.7.1.4's
 * Support Record exists precisely to define macros other pages then invoke.
 * A caller therefore chooses where the resets fall: reset_decoder() is the
 * receiver's general reset (power-up or channel change, CEA-516 §8.5), and
 * run() executes one record from wherever the state currently stands.
 *
 * The record is a program (CEA-516 §6.1, ANSI
 * X3.110-1983), and running it means walking the byte stream while three things
 * change underneath: which G-set a byte belongs to (NaplpsCodeEnvironment),
 * what the presentation attributes are (NaplpsState), and where the drawing
 * point is. Each drawing operation is emitted with the attributes that were in
 * force when it ran, so the display list needs none of that state to be
 * re-drawn.
 *
 * What is deliberately not done here is rasterisation. The primitives carry
 * their control points, their logical pel, their texture and their colours in
 * unit space; turning an arc into pixels, choosing a font, or stepping a
 * hatching pattern is the renderer's business. That keeps the MVP boundary
 * where the teletext viewer's is and keeps this testable against the standard
 * rather than against a screenshot. The one exception is what a DRCS or
 * texture definition draws, which the receiver keeps as pels of the character
 * or mask (draw_into_definition()).
 *
 * Two limits are enforced rather than assumed, because a recovered record has
 * lost packets in it and a decoder that trusts its input will hang on one:
 * macro nesting (kNaplpsMaxMacroDepth) and the shared storage budget
 * (kNaplpsSharedStorageBytes). Both are counted in the diagnostics.
 */
class NaplpsInterpreter {
 public:
  NaplpsInterpreter();

  /**
   * @brief Decode for a receiver of the given resolution
   *
   * Almost nothing the interpreter does depends on the receiver — it resolves
   * the presentation into unit-space geometry and leaves rendering to a
   * consumer. DRCS is the exception: §6.2.3 sizes a downloadable character's
   * storage buffer from "the minimum physical resolution ... covered by the
   * character field", so the buffer a definition is captured into is as coarse
   * or as fine as the receiver is.
   */
  explicit NaplpsInterpreter(NaplpsRenderGrid grid);

  /**
   * @brief The general reset of CEA-516 §8.5
   *
   * "On power-up or TV-channel change ... A general reset is equivalent to a
   * NSR, followed by a RESET command with two operands consisting of all 1s" —
   * everything to its default, macros, DRCS, texture masks and colour map
   * included. Call before the first record of an independent presentation;
   * do not call between records that share state (a Support Record and the
   * pages that need it).
   */
  void reset_decoder();

  /**
   * @brief The Caption Record preset of CEA-516 §5.2.7.3
   *
   * A receiver executing a caption record "first executes a Non-Selective
   * Reset (NSR)" and is then left with the cursor and drawing point at (0,0),
   * map entries 0, 1 and 7 loaded with transparent, black and white, colour
   * mode 2 drawing white over black, and the code environment at its defaults.
   * Call after reset_decoder() (and after any Support Record), before running
   * a record whose Caption Flag is set.
   */
  void apply_caption_state();

  /**
   * @brief Run |record| and return what it drew
   *
   * @param record Record data as CEA-516 §5.3 delivers it — the record header
   *               already removed, byte parity still in place
   *
   * Byte parity is stripped here rather than by the caller: §3.3 puts odd
   * parity in b8 of every data byte of a type-zero group, and a NAPLPS byte is
   * its low seven bits. A caller passing already-stripped bytes gets the same
   * result, since stripping twice is idempotent. Where the record carries its
   * parity (some byte has b8 set), a C0 control or a PDI opcode that fails it
   * — known to be wrong, the vote having had no copy with a clean value for
   * it — is not executed (see NabtsDecodeDiagnostics::parity_skipped).
   *
   * Never throws. A record that runs out mid-sequence yields the primitives it
   * managed, with the shortfall in the diagnostics.
   *
   * Persistent state — macros, DRCS, texture masks, the colour map and the
   * presentation attributes — is carried in from previous runs and left in
   * place afterwards; only the per-record transients (the display list, open
   * definitions, execution frames) start fresh. The snapshot's colour map,
   * DRCS set and texture masks therefore include whatever earlier records
   * defined, which is what a page drawn against a Support Record needs.
   *
   * |keep_display| carries the previous run's display list in as the starting
   * canvas, which is how a More Record is presented: CEA-516 §5.2.7.8 has it
   * "presented after the completion of the presentation of the current
   * Record", drawing over what is already on screen rather than over a
   * cleared one — the record's own CS or RESET is what clears, when it wants
   * to. The carried primitives keep their colour map addresses, so a map
   * write in the continuation retro-colours them exactly as it would the
   * pixels of a real display. Diagnostics always describe this record alone.
   */
  NabtsPageSnapshot run(const std::vector<uint8_t>& record,
                        bool keep_display = false);

  /// The state after the last run(), for a test that wants to inspect it.
  const NaplpsState& state() const { return state_; }

  /**
   * @brief The keys the last run() linked to macros: bit n for digit n
   *
   * A digit is linked where the record run last defined macro 2/n — by DEF
   * MACRO or DEFP MACRO, still defined at its end — and never invoked it
   * itself. A page draws its own macros wherever it means them to be drawn
   * (§5.5); one it defines and leaves alone is there for something else to
   * activate, and Table D1 item 7 names that something: "Macros may be
   * accessed by user input mechanisms, for example, keys". A transmit macro
   * (§6.2.2.3) is "transmitted to a local process" rather than executed, so it
   * draws nothing and is not counted.
   */
  uint16_t linked_keys() const { return linked_keys_; }

  /**
   * @brief Press digit |key|, as a viewer of the page did, and return the page
   *        as it then stands
   *
   * Runs macro 2/|key| (kNaplpsFirstLinkedMacro) from the presentation state
   * the last run left — the drawing point, the colours selected, the code
   * environment — over the display it left, which is run(keep_display)'s
   * presentation of a More Record. Table D1 item 7: "A macro activated in this
   * manner shall be executed in a contiguous sequence to completion", and its
   * execution "may result in an implementation-dependent state of the receive
   * presentation process ... It is the responsibility of the service provider
   * to restore the receiving device to a known state, if desired" — so what it
   * leaves is where the next key starts, which is how games keep a cursor's
   * place between moves and how a game's one "somewhat unpredictable" key acts
   * on whichever colour the key before it selected.
   *
   * The macro need not be available only "when the cursor is on", as Table D1
   * item 7 first had it: the 1993 Addendum to T.101 Annex D (clause 5) makes
   * "Macros that are linked to user input ... always available for user
   * activation", the TU-2000x runs them with the cursor off, and the games
   * never turn it on.
   *
   * The snapshot's clock starts at the key press, the standing display already
   * on screen, so at a receiver's pace (set_receiver_pace()) what the key draws
   * is seen being drawn. A key with no macro, or with a transmit macro, leaves
   * the page as it stands.
   */
  NabtsPageSnapshot press_key(uint8_t key);

  /**
   * @brief Record how each byte of the records run next is read
   *
   * While set, run() resizes |roles| to its record and fills it, one role per
   * byte of the record as passed in. A byte a macro expansion executes is the
   * macro's, not the record's, and is not traced. Pass nullptr to stop.
   */
  void set_trace(std::vector<NaplpsByteRole>* roles) { trace_ = roles; }

  /**
   * @brief Take the time a receiver of the day took to draw
   *
   * Off, drawing between two WAITs is instant: the presentation clock moves
   * only for a WAIT (§5.3.2.8), and each stage appears whole. At a receiver's
   * pace every command also takes what that receiver took over it
   * (naplps_receiver_pace.h) — a few milliseconds a character or a command
   * that draws nothing, a clear 169 ms on the Norpak TTX 650 and 70 on the
   * Panasonic TU-2000x, and a figure what its drawing loops cost — so the page
   * plays at the length a viewer of the day watched it, and each stage is seen
   * being drawn. Each figure carries how long it took (NabtsPrimitive::
   * draw_ms), and the timeline which way that receiver swept a fill across
   * its screen. What is drawn is the same either way.
   */
  void set_receiver_pace(NaplpsReceiverPace pace) { pace_ = pace; }

 private:
  /// Where bytes are being executed from: the record, or an expanding macro.
  struct Frame {
    const uint8_t* bytes = nullptr;
    size_t length = 0;
    size_t position = 0;
    /// A macro expansion's own copy of the definition, which |bytes| points
    /// into: the macro may clear or redefine the macros while it runs (see
    /// invoke_macro()), and the frame must not read the storage it freed.
    std::shared_ptr<const std::vector<uint8_t>> body;
  };

  /// Where a definition in progress is collecting its bytes.
  enum class Collecting : uint8_t {
    kNothing,
    /// DEF MACRO / DEFT MACRO: stored, not executed (§6.2.2.1, §6.2.2.3).
    kMacro,
    /// DEFP MACRO: stored *and* executed (§6.2.2.2).
    kMacroExecuting,
    /// DEF DRCS: executed into a character buffer (§6.2.3).
    kDrcs,
    /// DEF TEXTURE: executed into a mask buffer (§6.2.4).
    kTextureMask,
  };

  // ---- The main loop -------------------------------------------------------

  /// Execute one byte from the top frame. Returns false when the frame is done.
  bool step();
  /// Consume and execute the next byte of the innermost frame.
  void step_byte();

  void execute_byte(uint8_t byte);
  void execute_c0(uint8_t byte);
  void execute_escape();
  void execute_c1(NaplpsC1 control);
  void execute_graphic(uint8_t byte);

  /// The reset half of NSR (§6.1.6.5 items 1-5): code environment, DOMAIN,
  /// text parameters and the active field, TEXTURE attributes, and colour mode
  /// 0 with a white drawing colour. The colour map, programmable masks, macros
  /// and DRCS are left alone — which is what makes them persist between pages.
  void apply_nsr_reset();

  /// Clear the display: what was drawn so far moves to the timeline's
  /// superseded list, and where |colour| is what the display was cleared to,
  /// the clear is recorded as a full-display-area filled rectangle so a
  /// renderer shows the cleared colour rather than its own canvas.
  /// |map_address| is the colour's map address, or -1 for a direct colour;
  /// nominal black with no address draws nothing, black being what a renderer
  /// shows for an empty list anyway.
  void clear_display(const NabtsColour& colour, int map_address);

  // ---- Time ----------------------------------------------------------------

  /// Put the colour map writes made since the last call on the presentation
  /// clock, as events of the record's timeline.
  void flush_map_writes();

  /**
   * @brief Start a blink process on entry |from| (§5.3.2.7, §6.2.8.1)
   *
   * Terminates any process already running on the same pair of entries
   * (§5.3.2.7.4), and places the first ON interval |delay_ms| after the start
   * of the current or next ON interval of the most recently defined process
   * still running — or now, where none is (§5.3.2.7.2). |to_address| is -1
   * for a process that blinks to |to_colour| rather than to an entry.
   */
  void define_blink(uint32_t from, int16_t to_address,
                    const NabtsColour& to_colour, uint32_t on_ms,
                    uint32_t off_ms, uint32_t delay_ms);

  /// Which running blink processes end_blinks() terminates.
  enum class BlinkSelection : uint8_t {
    kAll,   ///< Every one (RESET, §5.3.2.9.3).
    kFrom,  ///< Those blinking entry |from| (BLINK alone, BLINK STOP).
    kPair,  ///< The one on |from| and |to_address| (§5.3.2.7.3-4).
  };

  /// Terminate the running blink processes |selection| picks out.
  void end_blinks(BlinkSelection selection, uint32_t from = 0,
                  int16_t to_address = -1);

  // ---- PDI ----------------------------------------------------------------

  /**
   * @brief Execute the PDI opcode |opcode| with the numeric data that follows
   *
   * The operand bytes are gathered first, because §5.3.2.2.5 makes the count
   * decide the meaning — short is zero-extended, long repeats the opcode.
   * §5.3.1 defines the run's end: "A PDI sequence is terminated by an opcode
   * introducing the next PDI sequence or by any other presentation layer code
   * not from the numeric data section of the same PDI set", with the
   * transparent controls of §6.1.4-6.1.6.1 explicitly not terminating it.
   */
  void execute_pdi(uint8_t opcode);

  /// Collect the numeric data bytes following the current position.
  std::vector<uint8_t> gather_operands();

  void pdi_reset(NaplpsOperandReader& reader);
  void pdi_domain(NaplpsOperandReader& reader);
  void pdi_text(NaplpsOperandReader& reader);
  void pdi_texture(NaplpsOperandReader& reader);
  void pdi_set_colour(NaplpsOperandReader& reader);
  void pdi_select_colour(NaplpsOperandReader& reader, size_t operand_bytes);
  void pdi_blink(NaplpsOperandReader& reader, size_t operand_bytes);
  void pdi_wait(NaplpsOperandReader& reader);
  void pdi_point(NaplpsPdi opcode, NaplpsOperandReader& reader);
  void pdi_line(NaplpsPdi opcode, NaplpsOperandReader& reader);
  void pdi_arc(NaplpsPdi opcode, NaplpsOperandReader& reader);
  void pdi_rect(NaplpsPdi opcode, NaplpsOperandReader& reader);
  void pdi_poly(NaplpsPdi opcode, NaplpsOperandReader& reader);
  void pdi_field(NaplpsOperandReader& reader);
  void pdi_incremental(NaplpsPdi opcode, NaplpsOperandReader& reader,
                       const std::vector<uint8_t>& operands);

  /**
   * @brief INCREMENTAL POINT (§5.3.3.6.3)
   *
   * Colour specifications of the packing counter's width, deposited one
   * logical pel apiece from the drawing point across the active field, a row
   * at a time: one kIncrementalPoints primitive per row, and a kScroll where a
   * row would leave the field and the field scrolls instead.
   */
  void incremental_point(const std::vector<uint8_t>& operands);

  // ---- Emission ------------------------------------------------------------

  /// A primitive with the current attributes already filled in.
  NabtsPrimitive make_primitive(NabtsPrimitiveKind kind) const;

  /// Emit |primitive|, into the display list or into whatever definition buffer
  /// is collecting — §6.2.3 and §6.2.4 both redirect drawing rather than
  /// suppressing it.
  void emit(NabtsPrimitive primitive);
  /// Move the clock on by |us| microseconds of the receiver's work
  /// (set_receiver_pace()); nothing without the receiver's pace.
  void pace(int64_t us);

  /// Move the drawing point to |point|, taking the cursor with it if the move
  /// attribute says to (§5.3.2.3.7).
  void move_drawing_point(const NabtsPoint& point);
  /// Move the cursor, taking the drawing point with it if the move attribute
  /// says to.
  void move_cursor(const NabtsPoint& point);

  /// Whether |figure| — a polygon, arc or spline — has a vertex off the unit
  /// screen and so is not drawn (§5.3.1), counting it if so.
  bool reject_off_screen(const NabtsPrimitive& figure);

  /// A cursor movement expressed relative to the character path, which is what
  /// §6.1.2 defines the four format effectors in terms of.
  enum class CursorMove : uint8_t {
    kAdvance,   ///< The advance after a character (§5.3.2.3.6)
    kForward,   ///< APF (§6.1.2.2): along the path
    kBackward,  ///< APB (§6.1.2.1): 180 degrees from the path
    kDown,      ///< APD (§6.1.2.3): -90 degrees, by the interrow spacing
    kUp,        ///< APU (§6.1.2.5): +90 degrees, by the interrow spacing
  };

  /**
   * @brief Move the cursor one step of |move|, with the edge rules
   *
   * A step that would put any part of the character field outside the area
   * the cursor is confined to does what the standard says for that step: a
   * character's advance is followed by an automatic APR APD (§5.3.2.3.6), APF
   * and APB go to the opposite end of the line and on to the next line or back
   * to the last (§6.1.2.1-2), and APD and APU wrap to the opposite edge under
   * SCROLL OFF or scroll the area under SCROLL ON (§6.2.7.13-14). Returns
   * whether the step wrapped or scrolled.
   *
   * |along|, where not zero, is how far a step along the path goes instead of
   * the field and the inter-character spacing: a proportionally spaced
   * character's own displacement.
   */
  bool move_cursor_by(CursorMove move, double along = 0.0);

  /// Where the drawing point sits relative to the cursor (§5.3.2.3.7): at the
  /// character field origin for the underscore and block cursors, at its
  /// centre for the cross-hair and custom ones.
  NabtsPoint cursor_alignment() const;

  /**
   * @brief |cursor| moved as little as it takes to keep its character field
   *        on the unit screen
   *
   * §5.3.2.3.7: "Movement of the drawing point should never cause the cursor to
   * be located such that any part of the character field indicated by the
   * cursor would fall outside the unit screen. Should such a situation arise,
   * the cursor shall be adjusted as close as possible to the drawing point
   * without violating the above condition."
   */
  NabtsPoint fit_cursor(NabtsPoint cursor) const;

  /// Draw the graphic |byte| of |set| at the cursor and advance, which is
  /// what displaying a character is, whether it was received or repeated.
  /// Sets |line_wrapped_| to whether the advance wrapped.
  void draw_character(uint8_t byte, NaplpsGSet set);

  /// The first character position of the cursor's line along the character
  /// path, in the display area or the active field the cursor is in (§6.1.2.7
  /// APR).
  NabtsPoint line_start() const;

  /**
   * @brief §5.3.2.3.6's suppression of the explicit APR APD after an automatic
   *        one
   *
   * "If an explicit APR APD (or APD APR) sequence is received after an
   * automatic APR APD is executed but before the character field origin is
   * moved, aligned, or set by any other received command or sequence, the
   * explicit APR APD (or APD APR) sequence shall be executed as a null
   * operation." A service writes every line flush to its field and ends it
   * with CR LF; without this rule each exactly-full line gains a blank row and
   * everything below it lands on top of the page's positioned elements.
   */
  enum class WrapSuppress : uint8_t {
    kNone,    ///< No automatic APR APD outstanding.
    kArmed,   ///< An automatic APR APD ran; an explicit pair would be null.
    kSawApr,  ///< First half (APR) of the suppressed pair consumed.
    kSawApd,  ///< First half (APD) of the suppressed pair consumed.
  };

  /**
   * @brief Whether a move to |point| leaves the cursor on the row it is on
   *
   * Measured across the character path, since that is the direction a row
   * advance is in (§5.3.2.3.5) — a move along the path is a move within the
   * row however the path is turned.
   *
   * §5.3.2.3.6 closes its suppression window when the character field origin is
   * "moved, aligned, or set by any other received command". A drawing-point
   * command that sets the left margin of the row the cursor is already on has
   * not left that row, and the row advance the window stands for is still
   * outstanding — so the window survives it. Reading the clause to cover that
   * too double-spaces every line a service indents, and indenting is how the
   * services in hand write a paragraph: carriage return, set the indent, line
   * feed. Anything that moves the origin to another row closes the window, as
   * does displaying a character, which is what tells a service's own line
   * ending from a real one further down the line.
   */
  bool stays_on_row(const NabtsPoint& point) const;

  /**
   * @brief How far a character moves the cursor in proportional text
   *
   * Where the text is spaced proportionally, the displacement T.101 Annex D's
   * Addendum (clause 11) gives the character — its width class, the wider of
   * its own and its marks' for a composite — and |primitive| moved back so
   * its ink starts at the cursor, as both receivers of the day lay it. Zero,
   * and |primitive| as it was, where the text is not, which steps the field.
   * A non-spacing |mark| moves nothing: its class waits for its letter.
   */
  double proportional_advance(uint8_t byte, NaplpsGSet set, bool mark,
                              NabtsPrimitive& primitive);

  /// Advance the cursor one character along the character path (§5.3.2.3.3),
  /// or |along| where not zero (move_cursor_by()). Returns whether the advance
  /// wrapped to the next line.
  bool advance_cursor(double along = 0.0) {
    return move_cursor_by(CursorMove::kAdvance, along);
  }

  // ---- Definitions ---------------------------------------------------------

  /// Start collecting for |what| at code |code|.
  void begin_definition(Collecting what, uint8_t code);
  /// Finish whatever is being collected.
  void end_definition();

  /// Rasterise |primitive| into the definition buffer, which is what §6.2.3
  /// means by "all drawing operations affect the DRCS storage buffer rather
  /// than the display area".
  void draw_into_definition(const NabtsPrimitive& primitive);

  /// Invoke the macro at |code| by pushing a frame for it.
  void invoke_macro(uint8_t code);

  /// The macros the current run defined and invoked, by position in the macro
  /// set (2/0 first), from which linked_keys() is worked out when it ends.
  std::bitset<kNaplpsMacroCount> macros_defined_;
  std::bitset<kNaplpsMacroCount> macros_invoked_;
  /// See linked_keys().
  uint16_t linked_keys_ = 0;

  NaplpsCodeEnvironment env_;
  NaplpsState state_;
  NabtsPageSnapshot snapshot_;

  /// The presentation clock: milliseconds of WAIT (§5.3.2.8) the record has
  /// executed so far.
  int64_t clock_ms_ = 0;
  /// Where the previous record's clock stopped. A blink process that record
  /// left running goes on running into the next, which starts where it
  /// finished, so its times move back by this much.
  int64_t previous_end_ms_ = 0;

  /// Execution frames: the record at the bottom, expanding macros above it.
  std::vector<Frame> frames_;
  /// The record's own bytes, parity stripped and the transparent controls
  /// dropped, which frames_[0] points into.
  std::vector<uint8_t> record_;
  /// Where each byte of record_ stood in the record as passed in.
  std::vector<size_t> record_offsets_;
  /// Parallel to record_: non-zero where the byte arrived failing its parity.
  std::vector<uint8_t> record_parity_failed_;
  /// Whether the byte being executed from the record is one of those.
  bool executing_parity_failure_ = false;
  /// See set_trace().
  std::vector<NaplpsByteRole>* trace_ = nullptr;
  /// See set_receiver_pace(); the microseconds of drawing not yet a whole
  /// millisecond on the clock.
  NaplpsReceiverPace pace_ = NaplpsReceiverPace::kOff;
  int64_t pace_us_ = 0;
  /// How the byte being executed from the record is read, which the code that
  /// executes it settles.
  NaplpsByteRole role_ = NaplpsByteRole::kControl;

  Collecting collecting_ = Collecting::kNothing;
  /// Bytes gathered for a macro definition.
  std::vector<uint8_t> definition_body_;
  uint8_t definition_code_ = 0;
  bool definition_is_transmit_ = false;
  /// Frame the open definition is collecting from. Bytes executed from deeper
  /// frames — a macro expanding inside a DEFP MACRO body — are executed but not
  /// stored, which is §6.2.2.1's "storage of that reference only, not the
  /// expansion".
  size_t defining_frame_index_ = 0;
  /// Whether any presentation layer code has arrived inside the open DRCS
  /// definition. §6.2.3 frees the character's buffer when a definition is
  /// terminated with none.
  bool definition_had_code_ = false;
  /// The DRCS character or texture mask being written into.
  NabtsDrcsCharacter* drcs_target_ = nullptr;
  NabtsTextureMask* mask_target_ = nullptr;
  /// How much of each unit-screen axis the buffer being written covers: the
  /// larger dimension of its shape the whole axis, the other its share
  /// (§6.2.3).
  NabtsSize definition_extent_{1.0, 1.0};

  /// §6.2.3: the last DRCS code defined, so a DEF DRCS that terminates another
  /// takes "the next character of the DRCS G-set (ie, in the circular sequence
  /// 2/0, 2/1, ... 7/15, 2/0 ...)".
  uint8_t last_drcs_code_ = kNaplpsLastCode;
  bool have_last_drcs_code_ = false;

  /// The last spacing character displayed and the set it came from, which
  /// REPEAT (§6.2.7.2) repeats — from that set, so a character single-shifted
  /// in from G2 is repeated as itself rather than as whatever is in use.
  uint8_t last_graphic_ = 0;
  NaplpsGSet last_graphic_set_ = NaplpsGSet::kPrimary;
  /// Whether the byte just executed was that character. §6.2.7.2 repeats "the
  /// immediately preceding byte", so any other byte in between leaves nothing
  /// to repeat.
  bool last_byte_graphic_ = false;
  /// |last_byte_graphic_| as it stood at the ESC that opened a C1 control,
  /// which is the byte a 7-bit REPEAT immediately follows.
  bool escape_follows_graphic_ = false;
  /// Whether the last character's advance wrapped to the next line, which is
  /// where REPEAT TO EOL (§6.2.7.3) stops.
  bool line_wrapped_ = false;
  /// The width class of the non-spacing marks waiting in the cursor's field
  /// for their letter, in proportional text, and where the cursor stood; -1
  /// where none is waiting. A composite character takes the wider class of
  /// its parts (T.101 Annex D Addendum, clause 11.1).
  int pending_mark_class_ = -1;
  NabtsPoint pending_mark_cursor_;

  WrapSuppress wrap_suppress_ = WrapSuppress::kNone;
};

/// How a receiver freshly reset reads each byte of |record| (see
/// NaplpsByteRole).
std::vector<NaplpsByteRole> naplps_read_roles(
    const std::vector<uint8_t>& record);

}  // namespace orc

#endif  // ORC_NAPLPS_INTERPRETER_H
