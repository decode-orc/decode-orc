/*
 * File:        naplps_interpreter.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     NAPLPS interpreter implementation (X3.110 §5, §6)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_interpreter.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "naplps_raster.h"

namespace orc {

namespace {

/// §3.3 of CEA-516 puts odd parity in b8 of every data byte of a type-zero
/// group, so a NAPLPS byte is the low seven bits.
constexpr uint8_t kSevenBits = 0x7F;

/// A character field of no stated height still names one row, so a movement
/// across it is measured against at least this much.
constexpr double kRowHeightFloor = 1e-9;

/// Graphic bytes of a 7-bit in-use table: columns 2 to 7.
constexpr bool is_graphic(uint8_t byte) { return byte >= 0x20; }

/// C0 controls: columns 0 and 1.
constexpr bool is_c0(uint8_t byte) { return byte < 0x20; }

/// The repertoire a G-set maps to in the display list.
NabtsPrimitive::Repertoire repertoire_of(NaplpsGSet set) {
  switch (set) {
    case NaplpsGSet::kSupplementary:
      return NabtsPrimitive::Repertoire::kSupplementary;
    case NaplpsGSet::kMosaic:
      return NabtsPrimitive::Repertoire::kMosaic;
    case NaplpsGSet::kDrcs:
      return NabtsPrimitive::Repertoire::kDrcs;
    case NaplpsGSet::kPrimary:
    case NaplpsGSet::kPdi:
    case NaplpsGSet::kMacro:
    case NaplpsGSet::kNull:
      break;
  }
  return NabtsPrimitive::Repertoire::kPrimary;
}

}  // namespace

NaplpsInterpreter::NaplpsInterpreter()
    : NaplpsInterpreter(kNaplpsGridReference) {}

NaplpsInterpreter::NaplpsInterpreter(NaplpsRenderGrid grid) {
  state_.set_render_grid(grid);
}

// ---------------------------------------------------------------------------
// The main loop
// ---------------------------------------------------------------------------

void NaplpsInterpreter::reset_decoder() {
  // CEA-516 §8.5: "A general reset is equivalent to a NSR, followed by a RESET
  // command with two operands consisting of all 1s" — so everything, storage
  // included, goes back to Table II-3's defaults.
  state_.reset_all();
  env_.reset();
}

void NaplpsInterpreter::apply_caption_state() {
  // CEA-516 §5.2.7.3: the caption process "first executes a Non-Selective
  // Reset (NSR)" — which leaves macros, DRCS, masks and map alone — and then
  // establishes the stated state.
  apply_nsr_reset();
  state_.cursor = NabtsPoint{0.0, 0.0};
  state_.drawing_point = NabtsPoint{0.0, 0.0};
  // "color map entry 0/16 (0000) set to transparent, ... 1/16 (0001) set to
  // black, ... 7/16 (0111) set to white".
  NabtsColour transparent;
  transparent.transparent = true;
  state_.colour.write_map_entry(0, transparent);
  state_.colour.write_map_entry(1, kNabtsNominalBlack);
  state_.colour.write_map_entry(7, kNabtsNominalWhite);
  // "color mode set to 2, drawing color set to entry 7/16 (0111) (white),
  // background color set to entry 1/16 (0001) (black)".
  state_.colour.select_mapped_background_mode(7, 1);
}

NabtsPageSnapshot NaplpsInterpreter::run(const std::vector<uint8_t>& record,
                                         bool keep_display) {
  // Only the per-record transients start fresh. The presentation state — the
  // attributes, macros, DRCS, texture masks and colour map — carries over from
  // whatever ran before, because that is what a receiver does: CEA-516
  // §8.7.2.3.1's per-page NSR is recommended precisely because nothing resets
  // between records by itself.
  //
  // A continuation additionally starts from the display the previous record
  // left: a More Record repaints only what changes, and running it over a
  // blank canvas leaves holes where the page's standing artwork should be.
  std::vector<NabtsPrimitive> carried;
  if (keep_display) {
    carried = std::move(snapshot_.primitives);
  }
  snapshot_ = NabtsPageSnapshot{};
  snapshot_.primitives = std::move(carried);
  frames_.clear();
  collecting_ = Collecting::kNothing;
  definition_body_.clear();
  drcs_target_ = nullptr;
  mask_target_ = nullptr;
  have_last_drcs_code_ = false;
  have_last_graphic_ = false;
  defining_frame_index_ = 0;
  definition_had_code_ = false;
  wrap_suppress_ = WrapSuppress::kNone;

  record_.clear();
  record_.reserve(record.size());
  for (const uint8_t byte : record) {
    record_.push_back(static_cast<uint8_t>(byte & kSevenBits));
  }

  frames_.push_back(Frame{record_.data(), record_.size(), 0});
  while (!frames_.empty()) {
    if (!step()) {
      frames_.pop_back();
      // A definition opened inside a macro expansion keeps collecting from the
      // frame below once the expansion runs out.
      if (!frames_.empty() && defining_frame_index_ >= frames_.size()) {
        defining_frame_index_ = frames_.size() - 1;
      }
    }
  }

  // A definition left open by a truncated record is closed rather than dropped:
  // the bytes that did arrive defined something.
  end_definition();

  // §5.3.2.5: a colour map write is retroactive — "A change in the color map
  // will immediately be reflected in the color of all pixels whose associated
  // color map address points to the color map entry that has been changed". A
  // real display stores addresses per pixel and resolves them through the map
  // at display time, so what is on screen at the end is everything resolved
  // through the *final* map. The display list stores each mapped primitive's
  // address and resolves it here.
  const NabtsColour* final_map = state_.colour.map();
  for (NabtsPrimitive& primitive : snapshot_.primitives) {
    if (primitive.colour_map_address >= 0) {
      primitive.colour =
          final_map[primitive.colour_map_address % kNabtsColourMapEntries];
    }
    if (primitive.background_map_address >= 0) {
      primitive.background =
          final_map[primitive.background_map_address % kNabtsColourMapEntries];
    }
    // A blink-to entry can be rewritten after the process starts, and the
    // process reads the map rather than a copy of the colour it held.
    if (primitive.blink_to_map_address >= 0) {
      primitive.blink_to =
          final_map[primitive.blink_to_map_address % kNabtsColourMapEntries];
    }
  }

  snapshot_.diagnostics.bytes_read = record_.size();
  snapshot_.diagnostics.storage_used = state_.storage_used();
  state_.colour.copy_map_to(snapshot_.colour_map);
  snapshot_.drcs = state_.defined_drcs();
  for (size_t i = 0; i < kNabtsTextureMaskCount; ++i) {
    snapshot_.texture_masks[i] = state_.texture_masks[i];
  }
  return snapshot_;
}

bool NaplpsInterpreter::step() {
  Frame& frame = frames_.back();
  if (frame.position >= frame.length) {
    return false;
  }
  const uint8_t byte = frame.bytes[frame.position++];

  // Only the frame the definition was opened in is scanned for terminators and
  // stored from. Bytes from deeper frames are a macro expanding inside the
  // definition, and §6.2.2.1 stores "that reference only, not the expansion".
  const bool from_defining_frame = frames_.size() - 1 == defining_frame_index_;

  // A macro definition stores its bytes rather than executing them (§6.2.2.1),
  // so the only thing that matters while collecting is whether this byte ends
  // the definition. DEFP MACRO is the exception: §6.2.2.2 has it
  // "simultaneously executed and stored".
  if (collecting_ == Collecting::kMacro ||
      collecting_ == Collecting::kMacroExecuting) {
    if (from_defining_frame) {
      if (byte == kNaplpsEsc) {
        const Frame& current = frames_.back();
        const NaplpsEscape escape =
            naplps_parse_escape(current.bytes + current.position,
                                current.length - current.position);
        if (escape.kind == NaplpsEscapeKind::kControl &&
            naplps_terminates_definition(escape.c1)) {
          // §6.2.2.1: "Neither the terminating control character nor its
          // preceding ESC character in a 7-bit environment is stored as part
          // of the macro."
          frames_.back().position += escape.length - 1;
          end_definition();
          execute_c1(escape.c1);
          return true;
        }
      }
      if (collecting_ == Collecting::kMacro) {
        definition_body_.push_back(byte);
        return true;
      }
      // DEFP MACRO: execute the byte, then store it together with whatever
      // lookahead its execution consumed from this frame — escape sequences,
      // PDI operands, cursor addresses. Storing the lead byte alone left the
      // body a different program from the one that just ran.
      const size_t start = frame.position - 1;
      execute_byte(byte);
      const Frame& defining = frames_[defining_frame_index_];
      definition_body_.insert(definition_body_.end(), defining.bytes + start,
                              defining.bytes + defining.position);
      return true;
    }
    // A macro expanding inside a DEFP MACRO body: executed, not stored.
    execute_byte(byte);
    return true;
  }

  // §6.2.3: a DRCS definition terminated "with no intervening presentation
  // layer code" frees the character, so note whether this byte is code within
  // the definition or the sequence that terminates it. The transparent
  // controls have "no effect on the presentation layer" (§6.1.4-6.1.6.1) and
  // count for nothing either way.
  if ((collecting_ == Collecting::kDrcs ||
       collecting_ == Collecting::kTextureMask) &&
      from_defining_frame && !naplps_is_transparent_control(byte)) {
    bool terminator = false;
    if (byte == kNaplpsEsc) {
      const Frame& current = frames_.back();
      const NaplpsEscape escape = naplps_parse_escape(
          current.bytes + current.position, current.length - current.position);
      terminator = escape.kind == NaplpsEscapeKind::kControl &&
                   naplps_terminates_definition(escape.c1);
    }
    if (!terminator) {
      definition_had_code_ = true;
    }
  }

  execute_byte(byte);
  return true;
}

void NaplpsInterpreter::execute_byte(uint8_t byte) {
  if (is_c0(byte)) {
    execute_c0(byte);
    return;
  }
  execute_graphic(byte);
}

void NaplpsInterpreter::execute_c0(uint8_t byte) {
  // §6.1.4, §6.1.5, §6.1.6.1: no presentation effect, and explicitly permitted
  // inside any sequence.
  if (naplps_is_transparent_control(byte)) {
    ++snapshot_.diagnostics.ignored_controls;
    return;
  }

  switch (byte) {
    case kNaplpsEsc:
      execute_escape();
      return;

    // §6.1.3: the locking and non-locking invocations.
    case kNaplpsSi:
      env_.invoke_locking(NaplpsGSlot::kG0);
      return;
    case kNaplpsSo:
      env_.invoke_locking(NaplpsGSlot::kG1);
      return;
    case kNaplpsSs2:
      env_.invoke_single_shift(NaplpsGSlot::kG2);
      return;
    case kNaplpsSs3:
      env_.invoke_single_shift(NaplpsGSlot::kG3);
      return;

    // §6.1.2: the format effectors. The cursor moves; nothing is drawn. All
    // four are defined relative to the character path, and they are four
    // different movements — a line feed that stepped along the path instead of
    // across it would draw every row of a page on top of the last.
    case kNaplpsApb:
      move_cursor_by(CursorMove::kBackward);
      return;
    case kNaplpsApf:
      move_cursor_by(CursorMove::kForward);
      return;
    case kNaplpsApd:
      // §5.3.2.3.6: the explicit APR APD (or APD APR) after an automatic one
      // is a null operation — the wrap already did its work.
      if (wrap_suppress_ == WrapSuppress::kArmed) {
        wrap_suppress_ = WrapSuppress::kSawApd;
        return;
      }
      if (wrap_suppress_ == WrapSuppress::kSawApr) {
        wrap_suppress_ = WrapSuppress::kNone;
        return;
      }
      move_cursor_by(CursorMove::kDown);
      return;
    case kNaplpsApu:
      move_cursor_by(CursorMove::kUp);
      return;
    case kNaplpsApr:
      if (wrap_suppress_ == WrapSuppress::kArmed) {
        wrap_suppress_ = WrapSuppress::kSawApr;
        return;
      }
      if (wrap_suppress_ == WrapSuppress::kSawApd) {
        wrap_suppress_ = WrapSuppress::kNone;
        return;
      }
      // §6.1.2.7: to the first character position along the character path.
      move_cursor(NabtsPoint{state_.field_origin.x, state_.cursor.y});
      return;
    case kNaplpsCs:
      // §6.1.2.6: clears the display area and homes the cursor. "In color
      // modes 0 and 1, it clears the display area to nominal black. In color
      // mode 2, it clears the display area to the background color."
      if (state_.colour.mode() == NabtsColourMode::kMappedWithBackground) {
        clear_display(state_.colour.background_colour(),
                      static_cast<int>(state_.colour.map_background_address()));
      } else {
        clear_display(kNabtsNominalBlack, -1);
      }
      move_cursor(state_.home_position());
      return;
    case kNaplpsAph:
      move_cursor(state_.home_position());
      return;

    case kNaplpsNsr: {
      apply_nsr_reset();

      // §6.1.6.5(6): NSR "can be used as an alternative means to position the
      // cursor". If the two bytes that follow are both from columns 4 to 7 they
      // are a row and a column address and are consumed; anything else is left
      // to be executed, which is what the clause requires. Note the origin:
      // "row 0, column 0 in the upper leftmost character position in the
      // display area" — the opposite end from APS's own numbering (§6.1.2.4).
      Frame& frame = frames_.back();
      if (frame.position + 1 < frame.length) {
        const uint8_t row_byte =
            static_cast<uint8_t>(frame.bytes[frame.position] & 0x7F);
        const uint8_t column_byte =
            static_cast<uint8_t>(frame.bytes[frame.position + 1] & 0x7F);
        const auto is_address = [](uint8_t byte) {
          return byte >= 0x40 && byte <= 0x7F;  // columns 4 to 7
        };
        const auto is_ignored = [](uint8_t byte) {
          return byte >= 0x20 && byte < 0x40;  // columns 2 and 3
        };
        if (is_address(row_byte) && is_address(column_byte)) {
          frame.position += 2;
          const int row = row_byte & 0x3F;
          const int column = column_byte & 0x3F;
          const double dx = std::fabs(state_.text.character_field.dx);
          const double dy = std::fabs(state_.text.character_field.dy);
          // Row 0's character field has its top on the top of the display area,
          // and the cursor is that field's lower left corner (§5.3.2.3.2).
          move_cursor(NabtsPoint{
              static_cast<double>(column) * dx,
              kNabtsDisplayAreaHeight - static_cast<double>(row + 1) * dy});
          return;
        }
        if (is_ignored(row_byte) && is_ignored(column_byte)) {
          // "If the two bytes are from columns 2 and 3 (or columns 10 and 11),
          // they are ignored" — consumed and discarded rather than drawn.
          frame.position += 2;
        }
        // Anything else is left where it is: a C0 or C1 byte "terminates the
        // NSR sequence and is executed", and a mixed pair only costs the cursor
        // move.
      }
      // No address: the reset alone, and the cursor goes home as §5.3.2.9.3
      // has it for a reset of the text parameters.
      move_cursor(state_.home_position());
      return;
    }

    case kNaplpsCan:
      // §6.1.6.3: terminate every executing macro. Execution resumes after the
      // outermost one, which is frame 0.
      while (frames_.size() > 1) {
        frames_.pop_back();
      }
      return;

    case kNaplpsAps: {
      // §6.1.2.4: the two bytes following are a row and column address in the
      // nominal screen format the current character field establishes.
      Frame& frame = frames_.back();
      if (frame.position + 1 >= frame.length) {
        ++snapshot_.diagnostics.truncated_pdis;
        return;
      }
      const uint8_t row_byte = frame.bytes[frame.position];
      const uint8_t column_byte = frame.bytes[frame.position + 1];
      if (is_c0(row_byte) || is_c0(column_byte)) {
        // "If either of the characters following the APS character is a C0 or
        // C1 control, the APS is ignored and the C0 or C1 control is executed."
        return;
      }
      frame.position += 2;
      const int row = (row_byte & 0x7F) - 32;
      const int column = (column_byte & 0x7F) - 32;
      const double dx = std::fabs(state_.text.character_field.dx);
      const double dy = std::fabs(state_.text.character_field.dy);
      // "Rows and columns are numbered starting with row 0, column 0, in the
      // lower leftmost character position of the display area."
      move_cursor(NabtsPoint{static_cast<double>(column) * dx,
                             static_cast<double>(row) * dy});
      return;
    }

    // BEL (§6.1.6.2) is a transient indication and SDC (§6.1.6.4) a null
    // operation at this layer, and anything left is a C0 position this standard
    // does not define. None of them draws.
    case kNaplpsBel:
    case kNaplpsSdc:
    default:
      ++snapshot_.diagnostics.ignored_controls;
      return;
  }
}

void NaplpsInterpreter::execute_escape() {
  Frame& frame = frames_.back();
  const NaplpsEscape escape = naplps_parse_escape(
      frame.bytes + frame.position, frame.length - frame.position);

  // |length| counts the ESC, which has already been consumed.
  frame.position += escape.length - 1;

  switch (escape.kind) {
    case NaplpsEscapeKind::kDesignation:
      if (escape.set == NaplpsGSet::kNull) {
        ++snapshot_.diagnostics.unknown_designations;
      }
      env_.designate(escape.slot, escape.set);
      return;
    case NaplpsEscapeKind::kLockingShift:
      env_.invoke_locking(escape.slot);
      return;
    case NaplpsEscapeKind::kControl:
      execute_c1(escape.c1);
      return;
    // A sequence naming a set this does not implement, and one broken by a byte
    // outside the syntax, come to the same thing here: nothing is designated
    // and nothing is invoked. They differ only in what was consumed, which
    // naplps_parse_escape() has already decided — §4.3.2 leaves the offending
    // byte of a malformed sequence to be executed in its own right.
    case NaplpsEscapeKind::kUnsupported:
    case NaplpsEscapeKind::kMalformed:
      ++snapshot_.diagnostics.unknown_designations;
      return;
    case NaplpsEscapeKind::kTruncated:
      ++snapshot_.diagnostics.truncated_pdis;
      return;
  }
}

void NaplpsInterpreter::apply_nsr_reset() {
  // §6.1.6.5, items (1) to (5). What is *not* here is the point: the colour
  // map ("The color map is not changed"), the programmable masks ("The
  // programmable masks are not cleared"), macros and DRCS — only the selective
  // RESET of §5.3.2.9 clears those, which is what lets a Support Record's
  // definitions survive the NSR every page is recommended to open with
  // (CEA-516 §8.7.2.3.1). Blink processes are likewise not in the list, so
  // they are left running.
  env_.reset();
  state_.domain.reset();
  // (3) covers "the text parameters (from the TEXT opcode, from the C1 set
  // and the active field)" — §5.3.2.9.3's wording, which includes the active
  // field itself.
  state_.text.reset();
  state_.field_origin = NabtsPoint{0.0, 0.0};
  state_.field_size = NabtsSize{1.0, 1.0};
  state_.texture.reset();
  // (5): "The color mode is set to color mode 0 and the drawing color is set
  // to nominal white."
  state_.colour.reset_drawing_to_white();
}

void NaplpsInterpreter::clear_display(const NabtsColour& colour,
                                      int map_address) {
  snapshot_.primitives.clear();

  // Nominal black with no map address behind it is what a renderer shows for
  // an empty display list anyway.
  const bool black = colour.red == 0 && colour.green == 0 && colour.blue == 0 &&
                     !colour.transparent;
  if (black && map_address < 0) {
    return;
  }

  // The clear is recorded as a full-display-area filled rectangle, drawn
  // plainly whatever texture attributes happen to be in force — a clear is a
  // flood, not a patterned fill.
  NabtsPrimitive primitive;
  primitive.kind = NabtsPrimitiveKind::kRectangle;
  primitive.filled = true;
  primitive.origin = NabtsPoint{0.0, 0.0};
  primitive.size = NabtsSize{1.0, kNabtsDisplayAreaHeight};
  primitive.points.push_back(NabtsPoint{0.0, 0.0});
  primitive.points.push_back(NabtsPoint{1.0, kNabtsDisplayAreaHeight});
  primitive.colour_mode = state_.colour.mode();
  primitive.colour = colour;
  primitive.colour_map_address =
      map_address < 0 ? int16_t{-1} : static_cast<int16_t>(map_address);
  snapshot_.primitives.push_back(std::move(primitive));
}

void NaplpsInterpreter::execute_c1(NaplpsC1 control) {
  switch (control) {
    // §6.2.2, §6.2.3, §6.2.4: the definition openers. Each takes the code of
    // what is being defined from the next byte.
    case NaplpsC1::kDefMacro:
    case NaplpsC1::kDefpMacro:
    case NaplpsC1::kDeftMacro:
    case NaplpsC1::kDefDrcs:
    case NaplpsC1::kDefTexture: {
      const Collecting terminated = collecting_;
      end_definition();

      // §6.2.3's one exception: a DEF DRCS that terminated a previous DEF DRCS
      // is *not* followed by a code — "the next character of the DRCS G-set
      // (ie, in the circular sequence 2/0, 2/1, ... 7/15, 2/0 ...) is defined
      // by the presentation layer code immediately following this new DEF DRCS
      // command. (This is the only time DEF DRCS is not followed by the DRCS
      // character to be defined.)" Consuming a code byte here would both
      // define the wrong character and eat the first byte of its definition.
      if (control == NaplpsC1::kDefDrcs && terminated == Collecting::kDrcs &&
          have_last_drcs_code_) {
        const uint8_t next = last_drcs_code_ >= kNaplpsLastCode
                                 ? kNaplpsFirstCode
                                 : static_cast<uint8_t>(last_drcs_code_ + 1);
        begin_definition(Collecting::kDrcs, next);
        return;
      }

      Frame& frame = frames_.back();
      uint8_t code = 0;
      if (frame.position < frame.length) {
        code = frame.bytes[frame.position];
        if (is_graphic(code)) {
          ++frame.position;
        } else {
          // §6.2.2.1: "If the character following the DEF MACRO control is not
          // in this range, the entire command ... is in error and is executed
          // as a null operation." The offending byte is left to be executed.
          ++snapshot_.diagnostics.ignored_controls;
          return;
        }
      }

      switch (control) {
        case NaplpsC1::kDefMacro:
          definition_is_transmit_ = false;
          begin_definition(Collecting::kMacro, code);
          return;
        case NaplpsC1::kDefpMacro:
          definition_is_transmit_ = false;
          begin_definition(Collecting::kMacroExecuting, code);
          return;
        case NaplpsC1::kDeftMacro:
          definition_is_transmit_ = true;
          begin_definition(Collecting::kMacro, code);
          return;
        case NaplpsC1::kDefDrcs:
          begin_definition(Collecting::kDrcs, code);
          return;
        default:
          begin_definition(Collecting::kTextureMask, code);
          return;
      }
    }

    case NaplpsC1::kEnd:
      end_definition();
      return;

    // §6.2.7.2, §6.2.7.3: repeat the last graphic character.
    case NaplpsC1::kRepeat: {
      Frame& frame = frames_.back();
      if (frame.position >= frame.length || !have_last_graphic_) {
        ++snapshot_.diagnostics.ignored_controls;
        return;
      }
      const uint8_t count_byte = frame.bytes[frame.position];
      // §6.2.7.2: the count byte must be 4/0 through 7/15, and its low six bits
      // are the count.
      if (count_byte < 0x40) {
        ++snapshot_.diagnostics.ignored_controls;
        return;
      }
      ++frame.position;
      const uint8_t count = static_cast<uint8_t>(count_byte & 0x3F);
      const uint8_t repeated = last_graphic_;
      for (uint8_t i = 0; i < count; ++i) {
        execute_graphic(repeated);
      }
      return;
    }
    case NaplpsC1::kRepeatToEol: {
      if (!have_last_graphic_) {
        ++snapshot_.diagnostics.ignored_controls;
        return;
      }
      // To the last character position along the character path within the
      // active field. Only the two horizontal paths have a defined column count
      // here; the vertical ones repeat to the field edge the same way.
      const double dx = std::fabs(state_.text.character_field.dx);
      if (dx <= 0.0) {
        return;
      }
      const double limit =
          state_.field_origin.x + std::fabs(state_.field_size.dx);
      const uint8_t repeated = last_graphic_;
      // Bounded by the field width in character widths, so a damaged character
      // field cannot make this run away.
      const size_t max_repeats = static_cast<size_t>(1.0 / dx) + 1;
      for (size_t i = 0; i < max_repeats && state_.cursor.x + dx <= limit;
           ++i) {
        execute_graphic(repeated);
      }
      return;
    }

    // §6.2.7.4-5.
    case NaplpsC1::kReverseVideo:
      state_.text.reverse_video = true;
      return;
    case NaplpsC1::kNormalVideo:
      state_.text.reverse_video = false;
      return;

    // §6.2.7.6-10: each sets the character field to a stated size.
    case NaplpsC1::kSmallText:
      state_.text.character_field = NaplpsTextState::small_field();
      return;
    case NaplpsC1::kMediumText:
      state_.text.character_field = NaplpsTextState::medium_field();
      return;
    case NaplpsC1::kNormalText:
      state_.text.character_field = NaplpsTextState::normal_field();
      return;
    case NaplpsC1::kDoubleHeight:
      state_.text.character_field = NaplpsTextState::double_height_field();
      return;
    case NaplpsC1::kDoubleSize:
      state_.text.character_field = NaplpsTextState::double_size_field();
      return;

    // §6.2.8.1: "creates a blink process in which: the blink-from color is the
    // drawing color; the blink-to color is nominal black in color modes 0 and 1
    // or the background color in color mode 2; the on and off intervals are
    // implementation-dependent; and the phase delay is 0."
    case NaplpsC1::kBlinkStart:
      if (state_.colour.mode() == NabtsColourMode::kMappedWithBackground) {
        // The background has a map address of its own, so the process follows
        // a later write to it the way the drawing colour does.
        state_.start_blinking(state_.colour.map_background_address());
      } else {
        state_.start_blinking_to_colour(kNabtsNominalBlack);
      }
      return;
    // §6.2.8.2: "turns off any currently active blink processes utilizing the
    // drawing color as the blink-from color" — the drawing colour's, not every
    // process running.
    case NaplpsC1::kBlinkStop:
      state_.stop_blinking();
      return;

    // §6.2.7.11-16.
    case NaplpsC1::kWordWrapOn:
      state_.text.word_wrap = true;
      return;
    case NaplpsC1::kWordWrapOff:
      state_.text.word_wrap = false;
      return;
    case NaplpsC1::kScrollOn:
      state_.text.scroll = true;
      return;
    case NaplpsC1::kScrollOff:
      state_.text.scroll = false;
      return;
    case NaplpsC1::kUnderlineStart:
      state_.text.underlined = true;
      return;
    case NaplpsC1::kUnderlineStop:
      state_.text.underlined = false;
      return;

    // §6.2.7.17-19.
    case NaplpsC1::kFlashCursor:
      state_.text.cursor_visible = true;
      state_.text.cursor_flashing = true;
      return;
    case NaplpsC1::kSteadyCursor:
      state_.text.cursor_visible = true;
      state_.text.cursor_flashing = false;
      return;
    case NaplpsC1::kCursorOff:
      state_.text.cursor_visible = false;
      state_.text.cursor_flashing = false;
      return;

    // §6.2.6: Table D1 item 12(1) makes these no-ops for the teletext service —
    // "The execution of the PROTECT and UNPROTECT commands shall have no
    // effect" — and §6.2.8.3's extended device controls are outside this layer.
    case NaplpsC1::kProtect:
    case NaplpsC1::kUnprotect:
    case NaplpsC1::kEdc1:
    case NaplpsC1::kEdc2:
    case NaplpsC1::kEdc3:
    case NaplpsC1::kEdc4:
      ++snapshot_.diagnostics.ignored_controls;
      return;
  }
}

void NaplpsInterpreter::execute_graphic(uint8_t byte) {
  const NaplpsGSet set = env_.in_use();
  env_.consume_character();

  switch (set) {
    case NaplpsGSet::kPdi:
      // §5.3.1: b7 clear is an opcode, b7 set is numeric data. Numeric data
      // with no opcode in front of it has nothing to be an operand of.
      if (naplps_is_pdi_opcode(byte)) {
        execute_pdi(byte);
      }
      return;

    case NaplpsGSet::kMacro:
      invoke_macro(byte);
      return;

    case NaplpsGSet::kNull:
      // §4.3.2: "A null set is a set in which all code positions are executed
      // as null operations."
      return;

    case NaplpsGSet::kPrimary:
    case NaplpsGSet::kSupplementary:
    case NaplpsGSet::kMosaic:
    case NaplpsGSet::kDrcs: {
      last_graphic_ = byte;
      have_last_graphic_ = true;

      NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kCharacter);
      primitive.character = byte;
      primitive.repertoire = repertoire_of(set);
      if (set == NaplpsGSet::kDrcs) {
        primitive.drcs_index = static_cast<uint8_t>(byte - kNaplpsFirstCode);
      }
      primitive.origin = state_.cursor;
      primitive.points.push_back(state_.cursor);
      primitive.size = state_.text.character_field;
      primitive.rotation = state_.text.rotation;
      primitive.path = state_.text.path;
      primitive.reverse_video = state_.text.reverse_video;
      primitive.underlined = state_.text.underlined;
      emit(std::move(primitive));

      // §7.2: a composite character is transmitted as a non-spacing mark from
      // the supplementary set followed by the letter it applies to, and Table
      // 27's note has the mark's coded representation "precede those of the
      // characters" it modifies. The pair occupies one character field — that
      // is what makes it one character of the repertoire (§7.1) — so the mark
      // itself must not move the cursor, or every accented letter would come
      // out with a gap in front of it.
      if (set != NaplpsGSet::kSupplementary ||
          !nabts_supplementary_is_nonspacing(byte)) {
        advance_cursor();
      }
      return;
    }
  }
}

// ---------------------------------------------------------------------------
// PDI
// ---------------------------------------------------------------------------

std::vector<uint8_t> NaplpsInterpreter::gather_operands() {
  std::vector<uint8_t> operands;
  Frame& frame = frames_.back();
  while (frame.position < frame.length) {
    const uint8_t byte = frame.bytes[frame.position];
    if (naplps_is_pdi_numeric(byte)) {
      operands.push_back(byte);
      ++frame.position;
      continue;
    }
    // §5.3.1: the transparent controls "do not terminate PDI sequences".
    if (is_c0(byte) && naplps_is_transparent_control(byte)) {
      ++frame.position;
      ++snapshot_.diagnostics.ignored_controls;
      continue;
    }
    // Anything else terminates the sequence and is left to be executed.
    //
    // §5.3.1 also allows a PDI to continue into the operand data of a macro:
    // "The invocation of a macro either from the in-use table or by single
    // shift will not by itself terminate a PDI". That is not followed here —
    // gathering stops at the end of the frame it started in — because a PDI
    // whose operands span a macro boundary would need the operand reader to
    // walk the frame stack, and no observed service does it. A record that did
    // would lose that one PDI's tail rather than desynchronise, since the
    // macro's bytes are then executed in their own right.
    break;
  }
  return operands;
}

void NaplpsInterpreter::execute_pdi(uint8_t opcode) {
  const std::vector<uint8_t> operands = gather_operands();
  const NaplpsPdi pdi = static_cast<NaplpsPdi>(opcode);
  const NaplpsOperandFormat format = state_.domain.format;

  NaplpsOperandReader reader(operands.data(), operands.size(), format);

  switch (pdi) {
    case NaplpsPdi::kReset:
      pdi_reset(reader);
      return;
    case NaplpsPdi::kDomain:
      pdi_domain(reader);
      return;
    case NaplpsPdi::kText:
      pdi_text(reader);
      return;
    case NaplpsPdi::kTexture:
      pdi_texture(reader);
      return;
    case NaplpsPdi::kSetColour:
      pdi_set_colour(reader, format.multi_value_bytes);
      return;
    case NaplpsPdi::kSelectColour:
      pdi_select_colour(reader, format.single_value_bytes);
      return;
    case NaplpsPdi::kBlink:
      pdi_blink(reader, format.single_value_bytes);
      return;
    case NaplpsPdi::kWait:
      pdi_wait(reader);
      return;

    case NaplpsPdi::kPointSetAbs:
    case NaplpsPdi::kPointSetRel:
    case NaplpsPdi::kPointAbs:
    case NaplpsPdi::kPointRel:
      pdi_point(pdi, reader);
      return;

    case NaplpsPdi::kLineAbs:
    case NaplpsPdi::kLineRel:
    case NaplpsPdi::kSetLineAbs:
    case NaplpsPdi::kSetLineRel:
      pdi_line(pdi, reader);
      return;

    case NaplpsPdi::kArcOutlined:
    case NaplpsPdi::kArcFilled:
    case NaplpsPdi::kSetArcOutlined:
    case NaplpsPdi::kSetArcFilled:
      pdi_arc(pdi, reader);
      return;

    case NaplpsPdi::kRectOutlined:
    case NaplpsPdi::kRectFilled:
    case NaplpsPdi::kSetRectOutlined:
    case NaplpsPdi::kSetRectFilled:
      pdi_rect(pdi, reader);
      return;

    case NaplpsPdi::kPolyOutlined:
    case NaplpsPdi::kPolyFilled:
    case NaplpsPdi::kSetPolyOutlined:
    case NaplpsPdi::kSetPolyFilled:
      pdi_poly(pdi, reader);
      return;

    case NaplpsPdi::kField:
      pdi_field(reader);
      return;

    case NaplpsPdi::kIncrPoint:
    case NaplpsPdi::kIncrLine:
    case NaplpsPdi::kIncrPolyFilled:
      pdi_incremental(pdi, reader, operands);
      return;
  }
}

void NaplpsInterpreter::pdi_reset(NaplpsOperandReader& reader) {
  // §5.3.2.9.1: a two-byte fixed operand, executed b1 to b6 of byte 1 then b1
  // to b6 of byte 2. §5.3.2.9.3: no operands is every bit zero, i.e. no action.
  const uint8_t byte1 = reader.empty() ? 0 : reader.read_fixed_byte();
  const uint8_t byte2 = reader.empty() ? 0 : reader.read_fixed_byte();

  const auto bit = [](uint8_t value, int index) {
    // §5.3.2.9's bits are b1 to b6, and the numeric byte's payload is b6 down
    // to b1 in bits 5 down to 0, so b<n> is bit n-1.
    return ((value >> (index - 1)) & 0x1u) != 0;
  };

  // Byte 1, b1: DOMAIN.
  if (bit(byte1, 1)) {
    state_.domain.reset();
  }

  // Byte 1, b3 b2: Table 14's colour-mode reset.
  const int colour_action = (bit(byte1, 3) ? 2 : 0) | (bit(byte1, 2) ? 1 : 0);
  switch (colour_action) {
    case 0:
      break;  // No action.
    case 1:
      state_.colour.select_direct_mode();
      state_.colour.reset_map();
      state_.colour.set_colour(kNabtsNominalWhite);
      break;
    case 2:
      // "Select color mode 1 and set color map to default colors. If this is
      // executed while in color mode 0, then it has the same effect as 11."
      state_.colour.reset_to_mapped_mode(state_.colour.mode() ==
                                         NabtsColourMode::kDirect);
      break;
    default:
      // "Select color mode 1, set color map to default colors, and set the
      // in-use drawing color to white" — white from the default map, not
      // written over its entry 0 (see reset_to_mapped_mode()).
      state_.colour.reset_to_mapped_mode(true);
      break;
  }

  // Byte 1, b6 b5 b4: Table 15's screen and border clear. Only the display-area
  // cases mean anything to a display list; the border has nowhere to appear.
  // Actions 1 and 7 clear the display to nominal black, 2, 5 and 6 to the
  // current drawing colour — recorded so a page that opens on a coloured
  // ground shows it.
  const int screen_action = (bit(byte1, 6) ? 4 : 0) | (bit(byte1, 5) ? 2 : 0) |
                            (bit(byte1, 4) ? 1 : 0);
  if (screen_action == 1 || screen_action == 7) {
    clear_display(kNabtsNominalBlack, -1);
  } else if (screen_action == 2 || screen_action == 5 || screen_action == 6) {
    const int address = state_.colour.mode() == NabtsColourMode::kDirect
                            ? -1
                            : static_cast<int>(state_.colour.drawing_address());
    clear_display(state_.colour.drawing_colour(), address);
  }

  // Byte 2.
  if (bit(byte2, 1)) {
    // §5.3.2.9.3: home the cursor and reset every text parameter.
    state_.text.reset();
    state_.field_origin = NabtsPoint{0.0, 0.0};
    state_.field_size = NabtsSize{1.0, 1.0};
    move_cursor(state_.home_position());
  }
  if (bit(byte2, 2)) {
    // §5.3.2.9.3: "all blink processes are terminated" — every one, not just
    // the drawing colour's.
    state_.blink_from.fill(NaplpsState::BlinkProcess{});
  }
  // b3 protects unprotected fields, which Table D1 item 12(1) makes a no-op for
  // the teletext service.
  if (bit(byte2, 4)) {
    // "all texture attributes are set to their default values. The four
    // programmable texture masks are not cleared."
    state_.texture.reset();
  }
  if (bit(byte2, 5)) {
    state_.clear_macros();
  }
  if (bit(byte2, 6)) {
    state_.clear_drcs();
  }
}

void NaplpsInterpreter::pdi_domain(NaplpsOperandReader& reader) {
  if (reader.empty()) {
    return;
  }
  const uint8_t byte1 = reader.read_fixed_byte();

  // §5.3.2.2.2 Table 4: b2 b1 give the single-value length, 1 to 4 bytes.
  state_.domain.format.single_value_bytes =
      static_cast<size_t>(byte1 & 0x03) + 1;
  // §5.3.2.2.3 Table 5: b5 b4 b3 give the multi-value length, 1 to 8 bytes.
  state_.domain.format.multi_value_bytes =
      static_cast<size_t>((byte1 >> 2) & 0x07) + 1;
  // §5.3.2.2.4: b6 is the dimensionality.
  state_.domain.format.three_dimensional = ((byte1 >> 5) & 0x01) != 0;

  // §5.3.2.2.6: "Note that the new length of the multi-value operands, as set
  // in byte 1, applies to the multi-value logical pel size operand of that
  // DOMAIN command." So the reader has to be rebuilt on the new format.
  if (reader.empty()) {
    // "If the logical pel size operand is omitted, the size of the logical pel
    // shall not be changed."
    return;
  }
  reader.set_format(state_.domain.format);
  const NabtsPoint pel = reader.read_coordinate();
  state_.domain.logical_pel = NabtsSize{pel.x, pel.y};
}

void NaplpsInterpreter::pdi_text(NaplpsOperandReader& reader) {
  // §5.3.2.3.1: a two-byte fixed operand, then a multi-value character field.
  if (reader.empty()) {
    return;
  }
  const uint8_t byte1 = reader.read_fixed_byte();
  state_.text.rotation = static_cast<NabtsCharRotation>(byte1 & 0x03);
  state_.text.path = static_cast<NabtsCharPath>((byte1 >> 2) & 0x03);
  state_.text.intercharacter_spacing =
      static_cast<uint8_t>((byte1 >> 4) & 0x03);

  if (!reader.empty()) {
    const uint8_t byte2 = reader.read_fixed_byte();
    state_.text.interrow_spacing = static_cast<uint8_t>(byte2 & 0x03);
    state_.text.move_attribute = static_cast<uint8_t>((byte2 >> 2) & 0x03);
    state_.text.cursor_style =
        static_cast<NabtsCursorStyle>((byte2 >> 4) & 0x03);
  }

  // §5.3.2.3.9: "If the character field dimensions are omitted from the
  // operand, then the current character field dimensions remain unchanged."
  if (!reader.empty()) {
    const NabtsPoint field = reader.read_coordinate();
    state_.text.character_field = NabtsSize{field.x, field.y};
  }
}

void NaplpsInterpreter::pdi_texture(NaplpsOperandReader& reader) {
  if (reader.empty()) {
    return;
  }
  const uint8_t byte1 = reader.read_fixed_byte();
  // §5.3.2.4.2: b2 b1 line texture; §5.3.2.4.3: b3 highlight; §5.3.2.4.4:
  // b6 b5 b4 texture pattern.
  state_.texture.line_texture = static_cast<NabtsLineTexture>(byte1 & 0x03);
  state_.texture.highlight = ((byte1 >> 2) & 0x01) != 0;
  state_.texture.pattern =
      static_cast<NabtsTexturePattern>((byte1 >> 3) & 0x07);

  // §5.3.2.4.5: "If the mask size operand is not present within the TEXTURE
  // PDI, then the current mask size is not changed."
  if (!reader.empty()) {
    const NabtsPoint mask = reader.read_coordinate();
    state_.texture.mask_size = NabtsSize{mask.x, mask.y};
  }
}

void NaplpsInterpreter::pdi_set_colour(NaplpsOperandReader& reader,
                                       size_t operand_bytes) {
  // §5.3.2.5.1: "If no operand follows a SET COLOR opcode, the transparent
  // color is set."
  if (reader.empty()) {
    state_.colour.set_transparent();
    return;
  }

  // Each colour word sets a colour; §5.3.2.5.1 has additional data repeat the
  // opcode "with the map address incremented prior to the execution of the new
  // opcode", which is how a service loads a whole palette in one command. "This
  // incrementing does not affect the color map address associated with the
  // drawing color", so the walk is held locally rather than in the state, and
  // "subsequent operand data are ignored when the physical limit (all ones) of
  // the implemented color map is reached".
  (void)operand_bytes;
  bool first = true;
  uint32_t repeat_address = 0;
  while (!reader.empty()) {
    const NabtsColour colour = reader.read_colour();
    if (first) {
      state_.colour.set_colour(colour);
      repeat_address = state_.colour.drawing_address();
      first = false;
      continue;
    }
    if (state_.colour.mode() == NabtsColourMode::kDirect) {
      // In mode 0 a repeat is simply SET COLOR again: the last word is the
      // drawing colour, each finding or claiming its map entry as §5.3.2.5.1
      // has it.
      state_.colour.set_colour(colour);
      continue;
    }
    if (!NaplpsColourState::increment_map_address(repeat_address)) {
      break;
    }
    state_.colour.write_map_entry(repeat_address, colour);
  }
}

void NaplpsInterpreter::pdi_select_colour(NaplpsOperandReader& reader,
                                          size_t operand_bytes) {
  // §5.3.2.6: zero operands selects mode 0, one selects mode 1, two select
  // mode 2. Additional data is "reserved for future standardization and shall
  // be ignored".
  const size_t words =
      operand_bytes == 0 ? 0 : reader.remaining() / operand_bytes;
  if (words == 0) {
    state_.colour.select_direct_mode();
    return;
  }
  const uint32_t first = NaplpsColourState::address_from_operand(
      reader.read_single_value(), operand_bytes);
  if (words == 1) {
    state_.colour.select_mapped_mode(first);
    return;
  }
  const uint32_t second = NaplpsColourState::address_from_operand(
      reader.read_single_value(), operand_bytes);
  state_.colour.select_mapped_background_mode(first, second);
}

void NaplpsInterpreter::pdi_blink(NaplpsOperandReader& reader,
                                  size_t operand_bytes) {
  // §5.3.2.7.4: "If no operands follow the blink opcode, then all blink
  // processes utilizing the current drawing color as the blink-from color will
  // be terminated."
  if (reader.empty()) {
    state_.stop_blinking();
    return;
  }

  // §5.3.2.7.2: the blink-from colour is the one in use for drawing, and the
  // process runs on that colour map entry. §5.3.2.7.5 then allows the command
  // to be implicitly repeated for as long as operands keep arriving, "with the
  // address of the blink-from color being automatically incremented (as in
  // 5.3.2.5.1) ... The drawing color is not affected by this incrementing."
  uint32_t blink_from = state_.colour.drawing_address();
  while (true) {
    // §5.3.2.7.3: a blink-to map address, then ON, OFF and start-delay
    // intervals in tenths of a second. A display list carries no time, so the
    // intervals decide only whether a process runs at all; the blink-to address
    // is kept, because it is what the entry alternates to.
    const uint32_t to_address = NaplpsColourState::address_from_operand(
        reader.read_single_value(), operand_bytes);
    const uint8_t on_interval = reader.empty() ? 0 : reader.read_fixed_byte();
    const uint8_t off_interval = reader.empty() ? 0 : reader.read_fixed_byte();
    if (!reader.empty()) {
      (void)reader.read_fixed_byte();  // start delay
    }
    // "An ON or OFF interval of 0 is taken to mean termination of any active
    // blink process on the blink-from/blink-to color pair."
    NaplpsState::BlinkProcess& process =
        state_.blink_from[blink_from % kNabtsColourMapEntries];
    if (on_interval != 0 && off_interval != 0) {
      process.active = true;
      process.to_address =
          static_cast<int16_t>(to_address % kNabtsColourMapEntries);
    } else {
      process = NaplpsState::BlinkProcess{};
    }

    if (reader.empty() ||
        !NaplpsColourState::increment_map_address(blink_from)) {
      return;
    }
  }
}

void NaplpsInterpreter::pdi_wait(NaplpsOperandReader& reader) {
  // §5.3.2.8: a delay in processing. Nothing is drawn and no state changes, so
  // the operands are read past to keep the parser in step.
  while (!reader.empty()) {
    (void)reader.read_fixed_byte();
  }
}

// ---------------------------------------------------------------------------
// Geometric primitives
// ---------------------------------------------------------------------------

NabtsPoint NaplpsInterpreter::resolve(NabtsPoint point) {
  if (naplps_clamp_to_unit_screen(point)) {
    ++snapshot_.diagnostics.out_of_range_coordinates;
  }
  return point;
}

NabtsPoint NaplpsInterpreter::resolve_relative(const NabtsPoint& base,
                                               const NabtsPoint& delta) {
  return resolve(NabtsPoint{base.x + delta.x, base.y + delta.y});
}

void NaplpsInterpreter::pdi_point(NaplpsPdi opcode,
                                  NaplpsOperandReader& reader) {
  const bool relative =
      opcode == NaplpsPdi::kPointSetRel || opcode == NaplpsPdi::kPointRel;
  const bool visible =
      opcode == NaplpsPdi::kPointAbs || opcode == NaplpsPdi::kPointRel;

  // §5.3.2.2.5 has a longer operand repeat the opcode, which for POINT means a
  // run of points.
  while (!reader.empty()) {
    const NabtsPoint word = reader.read_coordinate();
    const NabtsPoint target =
        relative ? resolve_relative(state_.drawing_point, word) : resolve(word);
    move_drawing_point(target);
    if (visible) {
      NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kPoint);
      primitive.origin = target;
      primitive.points.push_back(target);
      emit(std::move(primitive));
    }
    if (reader.truncated()) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
  }
}

void NaplpsInterpreter::pdi_line(NaplpsPdi opcode,
                                 NaplpsOperandReader& reader) {
  const bool relative_end =
      opcode == NaplpsPdi::kLineRel || opcode == NaplpsPdi::kSetLineRel;
  const bool has_start =
      opcode == NaplpsPdi::kSetLineAbs || opcode == NaplpsPdi::kSetLineRel;

  while (!reader.empty()) {
    NabtsPoint start = state_.drawing_point;
    if (has_start) {
      // §5.3.3.2.4: the start point is absolute in both SET forms.
      start = resolve(reader.read_coordinate());
      if (reader.empty()) {
        ++snapshot_.diagnostics.truncated_pdis;
        return;
      }
    }
    const NabtsPoint word = reader.read_coordinate();
    const NabtsPoint end =
        relative_end ? resolve_relative(start, word) : resolve(word);

    NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kLine);
    primitive.origin = start;
    primitive.points.push_back(start);
    primitive.points.push_back(end);
    emit(std::move(primitive));

    // §5.3.3.2.1: "At the completion of drawing a line, the drawing point is
    // coincident with the end point."
    move_drawing_point(end);
    if (reader.truncated()) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
  }
}

void NaplpsInterpreter::pdi_arc(NaplpsPdi opcode, NaplpsOperandReader& reader) {
  const bool filled =
      opcode == NaplpsPdi::kArcFilled || opcode == NaplpsPdi::kSetArcFilled;
  const bool has_start = opcode == NaplpsPdi::kSetArcOutlined ||
                         opcode == NaplpsPdi::kSetArcFilled;

  NabtsPoint start = state_.drawing_point;
  if (has_start) {
    if (reader.empty()) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
    start = resolve(reader.read_coordinate());
  }
  if (reader.empty()) {
    ++snapshot_.diagnostics.truncated_pdis;
    return;
  }

  // §5.3.3.3.2: the intermediate point is relative to the start, and the end
  // point relative to the intermediate. §5.3.3.3.1 adds that "If more points
  // are given, they define a higher level arc, a curvilinear line defined by a
  // spline function", so the run is collected as a control-point list and the
  // renderer decides between circle and spline by its length.
  NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kArc);
  primitive.filled = filled;
  primitive.origin = start;
  primitive.points.push_back(start);

  NabtsPoint previous = start;
  while (!reader.empty() && primitive.points.size() < kNaplpsMaxVertices) {
    const NabtsPoint word = reader.read_coordinate();
    previous = resolve_relative(previous, word);
    primitive.points.push_back(previous);
    if (reader.truncated()) {
      ++snapshot_.diagnostics.truncated_pdis;
      break;
    }
  }

  if (primitive.points.size() < 2) {
    // An arc needs a start and at least the intermediate point (§5.3.3.3.1).
    ++snapshot_.diagnostics.truncated_pdis;
    return;
  }
  if (primitive.points.size() == 2) {
    // §5.3.3.3.1: "If the end point is omitted, it is taken to be coincident
    // with the start point and a circle is drawn." A record drawing a circle
    // saves a whole coordinate block this way, so it is a normal encoding
    // rather than a truncation.
    primitive.points.push_back(start);
  }
  const NabtsPoint end = primitive.points.back();
  emit(std::move(primitive));
  move_drawing_point(end);
}

void NaplpsInterpreter::pdi_rect(NaplpsPdi opcode,
                                 NaplpsOperandReader& reader) {
  const bool filled =
      opcode == NaplpsPdi::kRectFilled || opcode == NaplpsPdi::kSetRectFilled;
  const bool has_start = opcode == NaplpsPdi::kSetRectOutlined ||
                         opcode == NaplpsPdi::kSetRectFilled;

  while (!reader.empty()) {
    NabtsPoint start = state_.drawing_point;
    if (has_start) {
      start = resolve(reader.read_coordinate());
      if (reader.empty()) {
        ++snapshot_.diagnostics.truncated_pdis;
        return;
      }
    }
    // §5.3.3.4.2: the width and height are a coordinate word, and may be
    // negative — which is what puts the origin in any of the four corners.
    const NabtsPoint extent = reader.read_coordinate();

    // The far corner is resolved like any other point, and the size is then
    // taken from it rather than from the operand. §5.3.1 makes a drawing that
    // would leave the unit screen an error handled implementation-dependently,
    // and this clips — so a clipped rectangle must report the extent it was
    // clipped to, or a renderer reading |size| would draw outside the screen
    // while one reading |points| stayed inside it. Real service records do
    // this: the ExtraVision capture has rectangles whose corner lands at x = 1
    // and just past it.
    const NabtsPoint corner =
        resolve(NabtsPoint{start.x + extent.x, start.y + extent.y});

    NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kRectangle);
    primitive.filled = filled;
    primitive.origin = start;
    primitive.size = NabtsSize{corner.x - start.x, corner.y - start.y};
    primitive.points.push_back(start);
    primitive.points.push_back(corner);
    emit(std::move(primitive));

    // §5.3.3.4.1: "At the completion of drawing a rectangle, the drawing point
    // is the start point altered in x only, by the amount of the dx
    // displacement."
    move_drawing_point(resolve(NabtsPoint{start.x + extent.x, start.y}));
    if (reader.truncated()) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
  }
}

void NaplpsInterpreter::pdi_poly(NaplpsPdi opcode,
                                 NaplpsOperandReader& reader) {
  const bool filled =
      opcode == NaplpsPdi::kPolyFilled || opcode == NaplpsPdi::kSetPolyFilled;
  const bool has_start = opcode == NaplpsPdi::kSetPolyOutlined ||
                         opcode == NaplpsPdi::kSetPolyFilled;

  NabtsPoint start = state_.drawing_point;
  if (has_start) {
    if (reader.empty()) {
      ++snapshot_.diagnostics.truncated_pdis;
      return;
    }
    start = resolve(reader.read_coordinate());
  }

  NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kPolygon);
  primitive.filled = filled;
  primitive.origin = start;
  primitive.points.push_back(start);

  // §5.3.3.5.1: "Each (dx, dy) coordinate pair represents a relative
  // displacement from the last vertex (a relative displacement of magnitude 0
  // is ignored). There is implicit closure between the start point and the last
  // vertex".
  NabtsPoint previous = start;
  while (!reader.empty() && primitive.points.size() < kNaplpsMaxVertices) {
    const NabtsPoint word = reader.read_coordinate();
    if (word.x == 0.0 && word.y == 0.0) {
      continue;
    }
    previous = resolve_relative(previous, word);
    primitive.points.push_back(previous);
    if (reader.truncated()) {
      ++snapshot_.diagnostics.truncated_pdis;
      break;
    }
  }

  if (primitive.points.size() < 3) {
    ++snapshot_.diagnostics.truncated_pdis;
    return;
  }
  emit(std::move(primitive));
  // "at the completion of drawing a polygon, the drawing point is coincident
  // with the start point."
  move_drawing_point(start);
}

void NaplpsInterpreter::pdi_field(NaplpsOperandReader& reader) {
  // §5.3.3.6.2: no operands sets the field to the full unit screen with origin
  // (0,0); one operand gives the dimensions with the current drawing point as
  // origin; two give the origin and then the dimensions.
  if (reader.empty()) {
    state_.field_origin = NabtsPoint{0.0, 0.0};
    state_.field_size = NabtsSize{1.0, 1.0};
    move_drawing_point(state_.field_origin);
    return;
  }

  const NabtsPoint first = reader.read_coordinate();
  if (reader.empty()) {
    state_.field_origin = state_.drawing_point;
    state_.field_size = NabtsSize{first.x, first.y};
    return;
  }
  const NabtsPoint extent = reader.read_coordinate();
  state_.field_origin = resolve(first);
  state_.field_size = NabtsSize{extent.x, extent.y};
  // "The drawing point is set to the origin of the field after FIELD has been
  // executed."
  move_drawing_point(state_.field_origin);
}

void NaplpsInterpreter::pdi_incremental(NaplpsPdi opcode,
                                        NaplpsOperandReader& reader,
                                        const std::vector<uint8_t>& operands) {
  if (opcode == NaplpsPdi::kIncrPoint) {
    // §5.3.3.6.3: a string operand of colour specifications deposited
    // raster-sequentially in the active field. In colour mode 0 they are
    // values; in modes 1 and 2 they are map addresses. The whole run is one
    // primitive, because a renderer that walked it one point at a time would
    // need the field geometry anyway.
    NabtsPrimitive primitive =
        make_primitive(NabtsPrimitiveKind::kIncrementalPoints);
    primitive.origin = state_.field_origin;
    primitive.size = state_.field_size;
    primitive.points.push_back(state_.field_origin);
    // The string is indeterminate-length numeric data, decoded left to right,
    // b6 to b1 (§5.3.1).
    for (const uint8_t byte : operands) {
      primitive.incremental_colours.push_back(
          static_cast<uint8_t>(byte & kNaplpsNumericMask));
    }
    emit(std::move(primitive));
    return;
  }

  // §5.3.3.6.4 and §5.3.3.6.5: a multi-value operand giving the increment size,
  // then a string operand of direction codes. Both are compact encodings of a
  // vertex run, and both resolve into the same polyline a LINE or POLYGON run
  // would — so both are emitted as one, which is what lets a renderer draw them
  // without knowing the encoding.
  if (reader.empty()) {
    ++snapshot_.diagnostics.truncated_pdis;
    return;
  }
  const NabtsPoint increment = reader.read_coordinate();

  // Both forms resolve to a vertex run; the fill flag below is what separates
  // an INCREMENTAL LINE from an INCREMENTAL POLYGON.
  NabtsPrimitive primitive = make_primitive(NabtsPrimitiveKind::kPolygon);
  primitive.filled = opcode == NaplpsPdi::kIncrPolyFilled;
  primitive.origin = state_.drawing_point;
  primitive.points.push_back(state_.drawing_point);

  // Each remaining numeric byte carries direction codes; the run walks the
  // drawing point by |increment| per step. The exact sub-field packing of
  // Figures 58 to 61 is not modelled — see the design note in
  // docs-tech/nabts-support-design.md Phase 5 — so a byte is taken as one step
  // whose direction comes from its low three bits. A record using these draws
  // an approximation of its outline rather than the outline.
  NabtsPoint current = state_.drawing_point;
  while (!reader.empty() && primitive.points.size() < kNaplpsMaxVertices) {
    const uint8_t code = reader.read_fixed_byte();
    const int direction = code & 0x07;
    const double dx =
        (direction == 1 || direction == 2 || direction == 3)
            ? increment.x
            : ((direction == 5 || direction == 6 || direction == 7)
                   ? -increment.x
                   : 0.0);
    const double dy =
        (direction == 3 || direction == 4 || direction == 5)
            ? increment.y
            : ((direction == 7 || direction == 0 || direction == 1)
                   ? -increment.y
                   : 0.0);
    current = resolve_relative(current, NabtsPoint{dx, dy});
    primitive.points.push_back(current);
  }

  if (primitive.points.size() >= 2) {
    emit(std::move(primitive));
  }
  move_drawing_point(current);
}

// ---------------------------------------------------------------------------
// Emission and movement
// ---------------------------------------------------------------------------

NabtsPrimitive NaplpsInterpreter::make_primitive(
    NabtsPrimitiveKind kind) const {
  NabtsPrimitive primitive;
  primitive.kind = kind;
  primitive.logical_pel = state_.domain.logical_pel;
  primitive.line_texture = state_.texture.line_texture;
  primitive.texture_pattern = state_.texture.pattern;
  primitive.texture_mask_size = state_.texture.mask_size;
  primitive.highlighted = state_.texture.highlight;
  primitive.colour_mode = state_.colour.mode();
  primitive.colour = state_.colour.drawing_colour();
  primitive.background = state_.colour.background_colour();
  // In the mapped modes the pixel a receiver stores is the address, not the
  // value — the address is kept so run() can resolve it through the map as it
  // finally stood (§5.3.2.5's retroactivity).
  if (state_.colour.mode() != NabtsColourMode::kDirect) {
    primitive.colour_map_address =
        static_cast<int16_t>(state_.colour.drawing_address());
  }
  if (state_.colour.mode() == NabtsColourMode::kMappedWithBackground) {
    primitive.background_map_address =
        static_cast<int16_t>(state_.colour.map_background_address());
  }
  // A blink process belongs to the colour map entry, so what decides this is
  // the colour the primitive is being drawn in — not whether a BLINK command
  // has been seen at some point earlier in the record.
  const NaplpsState::BlinkProcess& blink = state_.blink_process();
  primitive.blinking = blink.active;
  if (blink.active) {
    primitive.blink_to_map_address = blink.to_address;
    primitive.blink_to =
        blink.to_address >= 0
            ? state_.colour.map()[blink.to_address % kNabtsColourMapEntries]
            : blink.to_colour;
  }
  return primitive;
}

void NaplpsInterpreter::emit(NabtsPrimitive primitive) {
  // §6.2.3 and §6.2.4: while a DRCS character or a texture mask is being
  // defined, "all drawing operations affect the DRCS storage buffer rather than
  // the display area".
  if (collecting_ == Collecting::kDrcs ||
      collecting_ == Collecting::kTextureMask) {
    draw_into_definition(primitive);
    return;
  }
  snapshot_.primitives.push_back(std::move(primitive));
}

bool NaplpsInterpreter::stays_on_row(const NabtsPoint& point) const {
  const bool path_is_horizontal = state_.text.path == NabtsCharPath::kRight ||
                                  state_.text.path == NabtsCharPath::kLeft;
  const double across = path_is_horizontal ? point.y - state_.cursor.y
                                           : point.x - state_.cursor.x;
  const double row =
      std::fabs(path_is_horizontal ? state_.text.character_field.dy
                                   : state_.text.character_field.dx);
  // Half a row: nearer than that to where it already is and the origin has not
  // been put on another row, however the coordinate was resolved.
  return std::fabs(across) < std::max(row, kRowHeightFloor) * 0.5;
}

void NaplpsInterpreter::move_drawing_point(const NabtsPoint& point) {
  state_.drawing_point = point;
  // §5.3.2.3.7 Table 10: move together (0) or drawing point leads (2) both take
  // the cursor with it.
  if (state_.text.move_attribute == 0 || state_.text.move_attribute == 2) {
    // The character field origin moved to another row, so §5.3.2.3.6's
    // suppression window closes.
    if (!stays_on_row(point)) {
      wrap_suppress_ = WrapSuppress::kNone;
    }
    state_.cursor = point;
  }
}

void NaplpsInterpreter::move_cursor(const NabtsPoint& point) {
  // §5.3.2.3.6: any movement of the character field origin ends the window in
  // which an explicit APR APD would be suppressed — including the one a
  // displayed character makes, which is what tells a service's own line ending
  // from a real one further down the line. The automatic wrap itself also
  // passes through here; move_cursor_by() re-arms after it.
  wrap_suppress_ = WrapSuppress::kNone;
  state_.cursor = resolve(point);
  // Move together (0) or cursor leads (1).
  if (state_.text.move_attribute == 0 || state_.text.move_attribute == 1) {
    state_.drawing_point = state_.cursor;
  }
}

void NaplpsInterpreter::move_cursor_by(CursorMove move) {
  // §5.3.2.3.4 Table 8 and §5.3.2.3.5 Table 9: both spacings are multiples of
  // the character field dimension lying along the direction of travel — the one
  // parallel to the character path for a movement along it, the one
  // perpendicular for a movement across it. Proportional inter-character
  // spacing (code 3) is font-dependent and §5.3.2.3.4 makes the algorithm
  // implementation-dependent, so it steps one field: the floor the standard
  // guarantees, "at least as many characters per line as would be allowed by
  // the current character field dimensions".
  static constexpr double kCharacterSpacing[4] = {1.0, 1.25, 1.5, 1.0};
  static constexpr double kRowSpacing[4] = {1.0, 1.25, 1.5, 2.0};

  const bool along_path =
      move == CursorMove::kForward || move == CursorMove::kBackward;
  const bool path_is_horizontal = state_.text.path == NabtsCharPath::kRight ||
                                  state_.text.path == NabtsCharPath::kLeft;

  const double character_distance =
      std::fabs(path_is_horizontal ? state_.text.character_field.dx
                                   : state_.text.character_field.dy) *
      kCharacterSpacing[state_.text.intercharacter_spacing & 0x03];
  const double row_distance =
      std::fabs(path_is_horizontal ? state_.text.character_field.dy
                                   : state_.text.character_field.dx) *
      kRowSpacing[state_.text.interrow_spacing & 0x03];
  const double distance = along_path ? character_distance : row_distance;

  // The character path as a unit vector (§5.3.2.3.3 Table 7).
  double path_x = 0.0;
  double path_y = 0.0;
  switch (state_.text.path) {
    case NabtsCharPath::kRight:
      path_x = 1.0;
      break;
    case NabtsCharPath::kLeft:
      path_x = -1.0;
      break;
    case NabtsCharPath::kUp:
      path_y = 1.0;
      break;
    case NabtsCharPath::kDown:
      path_y = -1.0;
      break;
  }

  // Forward along the path, backward 180 degrees from it, down -90 and up +90
  // — rotating the path vector, so all four follow the path wherever TEXT put
  // it. On the default rightward path -90 degrees is straight down, which is
  // what makes APD the line feed.
  double step_x = 0.0;
  double step_y = 0.0;
  switch (move) {
    case CursorMove::kForward:
      step_x = path_x;
      step_y = path_y;
      break;
    case CursorMove::kBackward:
      step_x = -path_x;
      step_y = -path_y;
      break;
    case CursorMove::kDown:
      step_x = path_y;
      step_y = -path_x;
      break;
    case CursorMove::kUp:
      step_x = -path_y;
      step_y = path_x;
      break;
  }

  NabtsPoint next = state_.cursor;
  next.x += step_x * distance;
  next.y += step_y * distance;

  // §5.3.2.3.6's automatic APR APD: a forward movement that would put any part
  // of the character field outside the active field wraps to the start of the
  // next row. Only the forward move wraps here — §6.1.2.1 gives APB the mirror
  // of it, and §6.1.2.3 makes APD's own overflow a scroll-mode question that
  // Table D1 leaves out of the teletext model.
  //
  // The comparison needs a tolerance: a field ending exactly at the edge must
  // not wrap, and a character width like the default 1/40 is not an exact
  // binary fraction, so a line of them lands within an ulp or two of the edge
  // rather than on it. Without the slack a full line wraps one character
  // early.
  constexpr double kFieldEdgeTolerance = 1e-9;
  const double field_right =
      state_.field_origin.x + std::fabs(state_.field_size.dx);
  const bool wrapped = move == CursorMove::kForward &&
                       state_.text.path == NabtsCharPath::kRight &&
                       next.x + std::fabs(state_.text.character_field.dx) >
                           field_right + kFieldEdgeTolerance;
  if (wrapped) {
    next.x = state_.field_origin.x;
    next.y -= row_distance;
  }

  move_cursor(next);
  if (wrapped) {
    // §5.3.2.3.6: the explicit APR APD that a service sends after writing a
    // line flush to its field must now execute as a null operation. Armed
    // after move_cursor(), which clears it.
    wrap_suppress_ = WrapSuppress::kArmed;
  }
}

// ---------------------------------------------------------------------------
// Definitions
// ---------------------------------------------------------------------------

void NaplpsInterpreter::begin_definition(Collecting what, uint8_t code) {
  collecting_ = what;
  definition_body_.clear();
  definition_code_ = code;
  drcs_target_ = nullptr;
  mask_target_ = nullptr;
  defining_frame_index_ = frames_.empty() ? 0 : frames_.size() - 1;
  definition_had_code_ = false;

  if (what == Collecting::kDrcs) {
    uint16_t width = 0;
    uint16_t height = 0;
    state_.drcs_buffer_size(width, height);
    drcs_target_ = state_.begin_drcs(code, width, height);
    if (drcs_target_ == nullptr) {
      // Over the storage budget. The definition is still collected — its
      // drawing redirected into nothing — because falling back to normal
      // execution would paint the character's defining code onto the display,
      // which no receiver refusing the download would show.
      ++snapshot_.diagnostics.storage_refusals;
    }
    // The circular sequence of §6.2.3 continues from here even when the
    // character itself was refused.
    last_drcs_code_ = code;
    have_last_drcs_code_ = true;
    return;
  }

  if (what == Collecting::kTextureMask) {
    // §6.2.4: the code must be 4/1 to 4/4 for mask A to D.
    if (code < 0x41 || code > 0x44) {
      ++snapshot_.diagnostics.ignored_controls;
      collecting_ = Collecting::kNothing;
      return;
    }
    const size_t index = static_cast<size_t>(code - 0x41);
    mask_target_ = &state_.texture_masks[index];
    // Table D1 item 5(3)(b): 16 by 16 stored elements.
    constexpr uint16_t kMaskSide = 16;
    mask_target_->width = kMaskSide;
    mask_target_->height = kMaskSide;
    mask_target_->elements.assign(static_cast<size_t>(kMaskSide) * kMaskSide,
                                  false);
  }
}

void NaplpsInterpreter::end_definition() {
  switch (collecting_) {
    case Collecting::kNothing:
      return;
    case Collecting::kMacro:
    case Collecting::kMacroExecuting:
      if (!state_.define_macro(definition_code_, std::move(definition_body_),
                               definition_is_transmit_)) {
        ++snapshot_.diagnostics.storage_refusals;
      }
      break;
    case Collecting::kDrcs:
      // §6.2.3: "If a DRCS definition is immediately terminated with no
      // intervening presentation layer code, the buffer space allocated to
      // that character is freed."
      if (!definition_had_code_ && drcs_target_ != nullptr) {
        state_.free_drcs(definition_code_);
      }
      [[fallthrough]];
    case Collecting::kTextureMask:
      // §6.2.3: "After the downloading sequence has been terminated, the
      // receiving device reverts to the normal procedure of mapping the unit
      // screen to the physical display screen, with the drawing point reset to
      // (0,0)."
      state_.drawing_point = NabtsPoint{0.0, 0.0};
      break;
  }
  collecting_ = Collecting::kNothing;
  definition_body_.clear();
  drcs_target_ = nullptr;
  mask_target_ = nullptr;
  definition_is_transmit_ = false;
}

void NaplpsInterpreter::draw_into_definition(const NabtsPrimitive& primitive) {
  // §6.2.3: the buffer's lower left corner is the unit screen's, and the larger
  // character-field dimension spans its whole axis. Each element the code
  // writes comes on "unless it is written into with nominal black, in which
  // case it is set to the off state".
  const bool black = primitive.colour.green == 0 && primitive.colour.red == 0 &&
                     primitive.colour.blue == 0 &&
                     !primitive.colour.transparent;

  uint16_t width = 0;
  uint16_t height = 0;
  std::vector<bool>* elements = nullptr;
  if (drcs_target_ != nullptr) {
    width = drcs_target_->width;
    height = drcs_target_->height;
    elements = &drcs_target_->elements;
  } else if (mask_target_ != nullptr) {
    width = mask_target_->width;
    height = mask_target_->height;
    elements = &mask_target_->elements;
  }
  if (elements == nullptr || width == 0 || height == 0) {
    return;
  }

  // The definition is drawn by exactly the rules the display is drawn by:
  // §6.2.3 executes the defining code "in the same manner as if it were being
  // displayed", into the buffer rather than onto the screen. So the buffer is
  // a receiver of its own size and the shared rasteriser fills it — which is
  // what makes an arc in a definition an arc rather than the box around it.
  const NaplpsRenderGrid buffer_grid{static_cast<int>(width),
                                     static_cast<int>(height)};
  NaplpsCellSurface surface(buffer_grid);
  NaplpsRasteriser raster(surface,
                          NaplpsGridMapping::over_unit_screen(buffer_grid));

  NaplpsInk ink;
  ink.colour = primitive.colour;

  switch (primitive.kind) {
    case NabtsPrimitiveKind::kPoint:
      if (!primitive.points.empty()) {
        raster.stamp_pel(primitive.points.front(), primitive.logical_pel, ink);
      }
      break;

    case NabtsPrimitiveKind::kCharacter:
      // A character deposits its own pattern; the definition sets the elements
      // its shape covers.
      raster.deposit_character(primitive, snapshot_.drcs, ink, nullptr);
      break;

    case NabtsPrimitiveKind::kLine:
      raster.stroke_path(primitive.points, primitive.logical_pel,
                         primitive.line_texture, ink);
      break;

    case NabtsPrimitiveKind::kArc: {
      const std::vector<NabtsPoint> outline =
          raster.arc_polyline(primitive.points);
      if (primitive.filled) {
        raster.fill_path(outline, primitive.logical_pel,
                         primitive.texture_pattern, primitive.texture_mask_size,
                         nullptr, ink);
      } else {
        raster.stroke_path(outline, primitive.logical_pel,
                           primitive.line_texture, ink);
      }
      break;
    }

    case NabtsPrimitiveKind::kPolygon:
      if (primitive.filled) {
        raster.fill_path(primitive.points, primitive.logical_pel,
                         primitive.texture_pattern, primitive.texture_mask_size,
                         nullptr, ink);
      } else {
        raster.stroke_path(primitive.points, primitive.logical_pel,
                           primitive.line_texture, ink);
      }
      break;

    case NabtsPrimitiveKind::kRectangle: {
      const NabtsPoint origin = primitive.origin;
      const NabtsPoint far{origin.x + primitive.size.dx,
                           origin.y + primitive.size.dy};
      const std::vector<NabtsPoint> corners = {origin,
                                               NabtsPoint{far.x, origin.y}, far,
                                               NabtsPoint{origin.x, far.y}};
      if (primitive.filled) {
        raster.fill_path(corners, primitive.logical_pel,
                         primitive.texture_pattern, primitive.texture_mask_size,
                         nullptr, ink);
      } else {
        raster.stroke_path(corners, primitive.logical_pel,
                           primitive.line_texture, ink, /*closed=*/true);
      }
      break;
    }

    case NabtsPrimitiveKind::kIncrementalPoints:
      // §5.3.3.6.3's raster, one element per pel, in the definition's own
      // colour: the values it carries address a colour map the buffer has no
      // use for, since an element is only on or off.
      raster.stroke_path(primitive.points, primitive.logical_pel,
                         primitive.line_texture, ink);
      break;
  }

  for (int row = 0; row < surface.height(); ++row) {
    for (int column = 0; column < surface.width(); ++column) {
      if (!surface.at(column, row).painted) {
        continue;
      }
      (*elements)[static_cast<size_t>(row) * width +
                  static_cast<size_t>(column)] = !black;
    }
  }
}

void NaplpsInterpreter::invoke_macro(uint8_t code) {
  // §6.2.2.2: "A macro is considered to be undefined during definition until
  // the definition is terminated. Therefore, if a DEFP MACRO command contains
  // a reference to itself, or if it references another macro which references
  // the one being defined, the reference to the macro being defined is
  // executed as a null operation."
  if (collecting_ == Collecting::kMacroExecuting && code == definition_code_) {
    return;
  }
  const NaplpsMacro* entry = state_.macro(code);
  if (entry == nullptr || !entry->defined) {
    ++snapshot_.diagnostics.unresolved_macros;
    return;
  }
  // §6.2.2.3: a transmit macro "when called, [is] not executed, but [is]
  // transmitted in their entirety to the host computer or to a local
  // application process". There is no host here, so it draws nothing.
  if (entry->transmit) {
    ++snapshot_.diagnostics.ignored_controls;
    return;
  }
  // The standard bounds nesting only by memory. A recovered record can contain
  // a macro that invokes itself — §6.2.2.2 rules that out for DEFP MACRO alone
  // — so the depth is capped and the overrun counted.
  if (frames_.size() > kNaplpsMaxMacroDepth) {
    ++snapshot_.diagnostics.unresolved_macros;
    return;
  }
  frames_.push_back(Frame{entry->code.data(), entry->code.size(), 0});
}

}  // namespace orc
