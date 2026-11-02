/*
 * File:        eia608_decoder.cpp
 * Module:      orc-sdk-support
 * Purpose:     EIA-608 Closed Caption Decoder implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/support/eia608_decoder.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>

namespace orc {

namespace {

// The cells a CaptionBuffer gives the two-byte characters (see
// eia608_cells_to_utf8()).
constexpr uint8_t kSpecialCells = 0x80;
constexpr uint8_t kExtendedCells = 0x90;
constexpr uint8_t kLastCell = 0xCF;

// The special characters 11h 30h-3Fh [CTA-608-E Table 49]; 39h is the
// transparent space, a space for text.
constexpr std::array<char32_t, 16> kSpecialCharacters = {
    0x00AE, 0x00B0, 0x00BD, 0x00BF, 0x2122, 0x00A2, 0x00A3, 0x266A,
    0x00E0, 0x0020, 0x00E8, 0x00E2, 0x00EA, 0x00EE, 0x00F4, 0x00FB};

// The extended characters 12h 20h-3Fh then 13h 20h-3Fh [CTA-608-E Tables
// 5-10]: Spanish, miscellaneous, French, Portuguese, German, Danish. The box
// drawing characters are cc_decoder's choice (the Unlicense; lib/cc_decode.py,
// EXTENDED_PORTUGUESE_GERMAN_DANISH) for the full-height bar and the corners.
constexpr std::array<char32_t, 64> kExtendedCharacters = {
    0x00C1, 0x00C9, 0x00D3, 0x00DA, 0x00DC, 0x00FC, 0x2018, 0x00A1,
    0x002A, 0x0027, 0x2014, 0x00A9, 0x2120, 0x2022, 0x201C, 0x201D,
    0x00C0, 0x00C2, 0x00C7, 0x00C8, 0x00CA, 0x00CB, 0x00EB, 0x00CE,
    0x00CF, 0x00EF, 0x00D4, 0x00D9, 0x00F9, 0x00DB, 0x00AB, 0x00BB,
    0x00C3, 0x00E3, 0x00CD, 0x00CC, 0x00EC, 0x00D2, 0x00F2, 0x00D5,
    0x00F5, 0x007B, 0x007D, 0x005C, 0x005E, 0x005F, 0x007C, 0x007E,
    0x00C4, 0x00E4, 0x00D6, 0x00F6, 0x00DF, 0x00A5, 0x00A4, 0x2502,
    0x00C5, 0x00E5, 0x00D8, 0x00F8, 0x250C, 0x2510, 0x2514, 0x2518};

// A character of the basic set [CTA-608-E Table 50]: ASCII but in ten places.
char32_t basic_character(uint8_t code) {
  switch (code) {
    case 0x2A:
      return 0x00E1;  // a acute
    case 0x5C:
      return 0x00E9;  // e acute
    case 0x5E:
      return 0x00ED;  // i acute
    case 0x5F:
      return 0x00F3;  // o acute
    case 0x60:
      return 0x00FA;  // u acute
    case 0x7B:
      return 0x00E7;  // c cedilla
    case 0x7C:
      return 0x00F7;  // division sign
    case 0x7D:
      return 0x00D1;  // N tilde
    case 0x7E:
      return 0x00F1;  // n tilde
    case 0x7F:
      return 0x25A0;  // solid block
    default:
      return code;
  }
}

void append_utf8(std::string& out, char32_t code_point) {
  if (code_point < 0x80) {
    out += static_cast<char>(code_point);
  } else if (code_point < 0x800) {
    out += static_cast<char>(0xC0 | (code_point >> 6));
    out += static_cast<char>(0x80 | (code_point & 0x3F));
  } else {
    out += static_cast<char>(0xE0 | (code_point >> 12));
    out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (code_point & 0x3F));
  }
}

}  // namespace

std::string eia608_cells_to_utf8(const std::string& cells) {
  std::string out;
  out.reserve(cells.size());
  for (const char c : cells) {
    const auto cell = static_cast<uint8_t>(c);
    if (cell >= 0x20 && cell < kSpecialCells) {
      append_utf8(out, basic_character(cell));
    } else if (cell >= kSpecialCells && cell < kExtendedCells) {
      append_utf8(out, kSpecialCharacters[cell - kSpecialCells]);
    } else if (cell >= kExtendedCells && cell <= kLastCell) {
      append_utf8(out, kExtendedCharacters[cell - kExtendedCells]);
    }
  }
  return out;
}

// CaptionBuffer implementation

CaptionBuffer::CaptionBuffer() : current_row_(MAX_ROWS - 1), current_col_(0) {
  clear();
}

void CaptionBuffer::clear() {
  // The cursor stays: a decoder remembers one caption cursor, and no erasure
  // moves it [47 CFR 15.119(f), CTA-608-E C.15] — though never past the 32nd
  // column, which is as far as a cursor goes [CTA-608-E C.13]. (A row is let
  // run on past it, but what follows an erasure starts on the screen.)
  for (auto& row : rows_) {
    row.clear();
  }
  current_col_ = std::min(current_col_, MAX_COLS - 1);
}

void CaptionBuffer::write_char(char c) {
  if (current_row_ >= MAX_ROWS) {
    current_row_ = MAX_ROWS - 1;
  }

  // Ensure row has enough space
  if (rows_[current_row_].length() < current_col_) {
    rows_[current_row_].resize(current_col_, ' ');
  }

  // Allow text beyond MAX_COLS - we're converting to mov_text which doesn't
  // have column limits Original EIA-608 has 32 columns, but captions can be
  // longer for proper display
  if (current_col_ >= rows_[current_row_].length()) {
    rows_[current_row_] += c;
  } else {
    rows_[current_row_][current_col_] = c;
  }
  current_col_++;
}

void CaptionBuffer::set_cursor(size_t row, size_t col) {
  current_row_ = (row < MAX_ROWS) ? row : MAX_ROWS - 1;
  current_col_ = (col < MAX_COLS) ? col : 0;

  // Ensure the row has space up to this column
  if (rows_[current_row_].length() < current_col_) {
    rows_[current_row_].resize(current_col_, ' ');
  }
}

void CaptionBuffer::restore_cursor(size_t row, size_t col) {
  current_row_ = (row < MAX_ROWS) ? row : MAX_ROWS - 1;
  current_col_ = std::min(col, MAX_COLS - 1);
}

void CaptionBuffer::next_row() {
  if (current_row_ < MAX_ROWS - 1) {
    current_row_++;
  }
  current_col_ = 0;
}

void CaptionBuffer::backspace() {
  if (current_col_ > 0) {
    --current_col_;
  }
}

std::string CaptionBuffer::render_row(size_t row) const {
  if (row >= MAX_ROWS) {
    return {};
  }
  std::string line = rows_[row];
  while (!line.empty() && line.back() == ' ') {
    line.pop_back();
  }
  return eia608_cells_to_utf8(line);
}

std::string CaptionBuffer::render() const {
  // Rows are joined with newlines and keep their leading indent: EIA-608
  // places text by row and column, and running the rows together loses the
  // layout. Two services sharing one screen (the reason the caller should be
  // demultiplexing at all) read as one run-on sentence when joined by spaces,
  // and a text-service page of columns collapses into nonsense.
  std::string result;
  for (size_t row_idx = 0; row_idx < rows_.size(); ++row_idx) {
    const std::string line = render_row(row_idx);
    if (line.empty()) {
      continue;
    }
    if (!result.empty()) {
      result += '\n';
    }
    result += line;
  }
  return result;
}

void CaptionBuffer::roll_up() {
  // Shift all rows up by one
  for (size_t i = 0; i < MAX_ROWS - 1; ++i) {
    rows_[i] = std::move(rows_[i + 1]);
  }

  // Clear bottom row
  rows_[MAX_ROWS - 1].clear();
  current_row_ = MAX_ROWS - 1;
  current_col_ = 0;
}

void CaptionBuffer::roll_up_window(size_t base_row, size_t depth) {
  base_row = std::min(base_row, MAX_ROWS - 1);
  const size_t top = (base_row + 1 >= depth) ? base_row + 1 - depth : 0;
  for (size_t row = top; row < base_row; ++row) {
    rows_[row] = std::move(rows_[row + 1]);
  }
  rows_[base_row].clear();
  current_row_ = base_row;
  current_col_ = 0;
}

void CaptionBuffer::move_window(size_t from_base, size_t to_base,
                                size_t depth) {
  std::array<std::string, MAX_ROWS> moved;
  for (size_t i = 0; i < depth && i <= from_base && i <= to_base; ++i) {
    if (from_base - i < MAX_ROWS && to_base - i < MAX_ROWS) {
      moved[to_base - i] = std::move(rows_[from_base - i]);
    }
  }
  rows_ = std::move(moved);
}

void CaptionBuffer::clear_rows_above(size_t first_row) {
  for (size_t row = 0; row < first_row && row < MAX_ROWS; ++row) {
    rows_[row].clear();
  }
}

// EIA608Decoder implementation

EIA608Decoder::EIA608Decoder()
    : mode_(CaptionMode::POP_ON),
      rollup_rows_(2),
      base_row_(CaptionBuffer::MAX_ROWS - 1),
      current_time_(0.0) {}

EIA608ControlCode EIA608Decoder::decode_control_code(uint8_t byte1,
                                                     uint8_t byte2) {
  // EIA-608 control codes are TWO-BYTE sequences
  // Miscellaneous Control Codes (Table 52), byte2 = 0x20-0x2F:
  //   Data channel 1: 0x14 (field 1), 0x15 (field 2)
  //   Data channel 2: 0x1C (field 1), 0x1D (field 2)
  // The decoder is fed one demultiplexed service, so which channel and field
  // the pair named has already been acted on; all four spellings mean the same
  // command here.

  if (byte1 == 0x14 || byte1 == 0x15 || byte1 == 0x1C || byte1 == 0x1D) {
    // Miscellaneous control codes
    if (byte2 >= 0x20 && byte2 <= 0x2F) {
      switch (byte2) {
        case 0x20:
          return EIA608ControlCode::RCL;  // Resume Caption Loading
        case 0x25:
          return EIA608ControlCode::RU2;  // Roll-Up 2 rows
        case 0x26:
          return EIA608ControlCode::RU3;  // Roll-Up 3 rows
        case 0x27:
          return EIA608ControlCode::RU4;  // Roll-Up 4 rows
        case 0x29:
          return EIA608ControlCode::RDC;  // Resume Direct Captioning
        case 0x2A:
          return EIA608ControlCode::TR;  // Text Restart
        case 0x2B:
          return EIA608ControlCode::RTD;  // Resume Text Display
        case 0x2C:
          return EIA608ControlCode::EDM;  // Erase Displayed Memory
        case 0x2D:
          return EIA608ControlCode::CR;  // Carriage Return
        case 0x2E:
          return EIA608ControlCode::ENM;  // Erase Non-displayed Memory
        case 0x2F:
          return EIA608ControlCode::EOC;  // End of Caption
        default:
          break;  // Other codes like BS, FON, etc. - ignore for now
      }
    }
  }

  // PACs (Preamble Address Codes) are handled separately in decode_pac()

  return EIA608ControlCode::UNKNOWN;
}

bool EIA608Decoder::decode_pac(uint8_t byte1, uint8_t byte2, size_t& row,
                               size_t& col) {
  // PAC format per Table 53:
  // byte1 determines which row pair (see table below)
  // byte2 bit 5 (0x20) determines even/odd within the pair
  // byte2 bits 0-3 encode indent/color

  if (byte2 < 0x40 || byte2 > 0x7F) {
    return false;  // Not a valid PAC
  }

  // Map byte1 to base row using EIA-608 row numbers
  // Table 53 mapping for Data Channel 1; data channel 2 spells each first
  // byte with 08h set (19h for 11h). The decoder is fed one demultiplexed
  // service, so the channel has already been acted on: CC2's and TEXT2's
  // codes place the cursor as CC1's and TEXT1's do.
  // These values represent row numbers that need to be converted to 0-based
  // indices
  int base_row = -1;
  switch (byte1 & ~0x08U) {
    case 0x11:
      base_row = (byte2 & 0x20) ? 2 : 1;
      break;  // rows 1-2
    case 0x12:
      base_row = (byte2 & 0x20) ? 4 : 3;
      break;  // rows 3-4
    case 0x15:
      base_row = (byte2 & 0x20) ? 6 : 5;
      break;  // rows 5-6
    case 0x16:
      base_row = (byte2 & 0x20) ? 8 : 7;
      break;  // rows 7-8
    case 0x17:
      base_row = (byte2 & 0x20) ? 10 : 9;
      break;  // rows 9-10
    case 0x10:
      base_row = (byte2 & 0x20) ? 12 : 11;
      break;  // row 11 (40h-5Fh only; the line 21 rules refuse 60h-7Fh)
    case 0x13:
      // Rows 12 and 13 [CTA-608-E Table 53]. They were taken for 13 and 14,
      // which put a two-row caption on rows 13 and 14 onto one row, its
      // second line written over its first. cc_decoder (the Unlicense;
      // lib/cc_decode.py, CC1_PREAMBLE_COLS) reads them as rows 12 and 13.
      base_row = (byte2 & 0x20) ? 13 : 12;
      break;
    case 0x14:
      base_row = (byte2 & 0x20) ? 15 : 14;
      break;  // rows 14-15
    default:
      return false;
  }

  if (base_row < 1) {
    return false;
  }

  // Convert 1-based row number to 0-based array index
  // EIA-608 rows are numbered 1-15, our array uses indices 0-14
  row = static_cast<size_t>(base_row - 1);

  // Decode indent from bits 0-3 of byte2
  // 0x40-0x4F or 0x60-0x6F: colored text with no indent
  // 0x50-0x5E or 0x70-0x7E: white text with indent
  if ((byte2 & 0x10)) {  // Indent PACs
    int indent_code = (byte2 & 0x0E) >> 1;
    col = static_cast<size_t>(indent_code) * 4;  // 0, 4, 8, 12, 16, 20, 24, 28
  } else {
    col = 0;  // Color/attribute PACs start at column 0
  }

  return true;
}

void EIA608Decoder::process_bytes(double timestamp, uint8_t byte1,
                                  uint8_t byte2) {
  current_time_ = timestamp;

  // Check if this is a control code or PAC (byte1 in 0x10-0x1F range)
  if (byte1 >= 0x10 && byte1 <= 0x1F) {
    // First try to decode as a control code
    EIA608ControlCode code = decode_control_code(byte1, byte2);
    if (code != EIA608ControlCode::UNKNOWN) {
      handle_control_code(code);
      return;
    }

    // Not a control code, try PAC (Preamble Address Code)
    size_t row, col;
    if (decode_pac(byte1, byte2, row, col)) {
      // Set cursor position in the appropriate buffer
      if (mode_ == CaptionMode::POP_ON) {
        nondisplayed_.set_cursor(row, col);
      } else if (mode_ == CaptionMode::ROLL_UP) {
        // In roll-up a PAC names the base row: a different one moves the
        // window there intact [47 CFR 15.119(f)(1)(ii), CTA-608-E C.7].
        move_roll_up_window(row);
        displayed_.set_cursor(base_row_, col);
      } else if (mode_ == CaptionMode::TEXT) {
        // CTA-608-E Section 7.4: in Text Mode a PAC moves the cursor to its
        // indent on the current row, ignoring the row it addresses; the rows
        // are the scrolling page's to fill. A text service can send every
        // line as a carriage return and a PAC to row 1, which taken as an
        // address wrote each line over the one before it. cc_decoder (the
        // Unlicense; lib/cc_decode.py, get_caption_text()) reads it so too.
        note_text_rewind(col);
        displayed_.set_cursor(displayed_.get_row(), col);
      } else {
        displayed_.set_cursor(row, col);
      }
      return;
    }

    // A special character [CTA-608-E Table 49] takes a column as any
    // character does: the music note round a song's words, the accented
    // letters the basic set lacks.
    if ((byte1 == 0x11 || byte1 == 0x19) && byte2 >= 0x30 && byte2 <= 0x3F) {
      handle_printable_char(static_cast<char>(kSpecialCells + (byte2 - 0x30)));
      return;
    }

    // An extended character [CTA-608-E §6.4.2, Tables 5-10] follows the
    // standard character a receiver without the extended set shows in its
    // place, and takes that character's column: "the cursor moves to the
    // left one column position (unless the Extended Character is the first
    // character on a row), erasing any character which may be in that
    // location".
    const auto set = static_cast<uint8_t>(byte1 & ~0x08U);
    if ((set == 0x12 || set == 0x13) && byte2 >= 0x20 && byte2 <= 0x3F) {
      handle_printable_char(
          static_cast<char>(kExtendedCells + (set == 0x13 ? 0x20 : 0x00) +
                            (byte2 - 0x20)),
          /*replaces_previous=*/true);
      return;
    }

    // A mid-row code (CTA-608-E Table 51) sets the colour or italics of what
    // follows it and takes a column itself, as a space does: "the cursor moves
    // automatically one column to the right after each character or Mid-Row
    // Code received" [CTA-608-E Section 7.4], and a DER deletes "displayable
    // characters, transparent spaces, and mid-row codes" from a row
    // [CTA-608-E Section B.3]. Captions put them round the words they
    // italicise ("THE", italics, "ENTERPRISE", plain, "IS"), and text services
    // between words; dropped, the words ran together. cc_decoder
    // (the Unlicense; lib/cc_decode.py, handle_style() and handle_tab()) adds
    // the same spaces.
    if ((byte1 == 0x11 || byte1 == 0x19) && byte2 >= 0x20 && byte2 <= 0x2F) {
      handle_printable_char(' ');
      return;
    }

    // A tab offset moves the cursor one to three columns right without
    // writing anything [CTA-608-E Table 52]; captions and text services
    // indent with it in steps finer than a PAC's four columns.
    if ((byte1 == 0x17 || byte1 == 0x1F) && byte2 >= 0x21 && byte2 <= 0x23) {
      CaptionBuffer& buffer =
          (mode_ == CaptionMode::POP_ON) ? nondisplayed_ : displayed_;
      const size_t column = std::min<size_t>(buffer.get_col() + (byte2 - 0x20),
                                             CaptionBuffer::MAX_COLS - 1);
      buffer.set_cursor(buffer.get_row(), column);
    }
    return;
  }

  // Handle printable characters (0x20-0x7F)
  if (byte1 >= 0x20 && byte1 <= 0x7F) {
    handle_printable_char(static_cast<char>(byte1));
  }

  if (byte2 >= 0x20 && byte2 <= 0x7F) {
    handle_printable_char(static_cast<char>(byte2));
  }
}

void EIA608Decoder::handle_printable_char(char c, bool replaces_previous) {
  CaptionBuffer& buffer =
      (mode_ == CaptionMode::POP_ON) ? nondisplayed_ : displayed_;
  const bool replaces = replaces_previous && buffer.get_col() > 0;
  if (replaces) {
    buffer.backspace();
  }
  buffer.write_char(c);

  switch (mode_) {
    case CaptionMode::POP_ON:
      break;

    case CaptionMode::ROLL_UP:
      ensure_rollup_cue_started();
      break;

    case CaptionMode::PAINT_ON:
      ensure_painton_cue_started();
      break;

    case CaptionMode::TEXT:
      ensure_text_cue_started();
      break;
  }
}

void EIA608Decoder::handle_control_code(EIA608ControlCode code) {
  switch (code) {
    case EIA608ControlCode::RCL:
      // Resume Caption Loading selects pop-on style and does nothing else: a
      // caption on screen stays, and the next characters are loaded into the
      // non-displayed memory [CTA-608-E C.10].
      mode_ = CaptionMode::POP_ON;
      break;

    case EIA608ControlCode::EOC:
      // The duplicate copy an encoder sends of every control pair is
      // suppressed upstream by EIA608ServiceDemux, which can tell a repeat
      // from a second genuine End of Caption because it looks at adjacency
      // within the service rather than at elapsed time.
      end_of_caption();
      break;

    case EIA608ControlCode::EDM:
      close_all_cues();
      displayed_.clear();
      break;

    case EIA608ControlCode::ENM:
      nondisplayed_.clear();
      break;

    case EIA608ControlCode::CR:
      if (mode_ == CaptionMode::ROLL_UP) {
        roll_up();
      } else if (mode_ == CaptionMode::TEXT) {
        text_new_line();
      } else if (mode_ == CaptionMode::POP_ON) {
        // In Pop-On mode, CR moves to the next row for multi-line captions
        nondisplayed_.next_row();
      } else if (mode_ == CaptionMode::PAINT_ON) {
        displayed_.next_row();
      }
      break;

    case EIA608ControlCode::RU2:
    case EIA608ControlCode::RU3:
    case EIA608ControlCode::RU4:
      start_roll_up((code == EIA608ControlCode::RU2)   ? 2
                    : (code == EIA608ControlCode::RU3) ? 3
                                                       : 4);
      break;

    case EIA608ControlCode::RDC:
      // Resume Direct Captioning selects paint-on style and nothing else
      // [CTA-608-E C.10]. An encoder sends it before each burst of a paint-on
      // caption [CTA-608-E B.8.2], which stays one cue while it grows.
      if (mode_ != CaptionMode::PAINT_ON) {
        close_all_cues();
      }
      mode_ = CaptionMode::PAINT_ON;
      break;

    case EIA608ControlCode::TR:
      // Text Restart: erase the text memory and write from the top of the page
      // [CTA-608-E Table 52].
      close_all_cues();
      mode_ = CaptionMode::TEXT;
      displayed_.clear();
      displayed_.set_cursor(0, 0);
      break;

    case EIA608ControlCode::RTD:
      // Resume Text Display: carry on where the text service left off, from
      // the same cursor position [CTA-608-E 7.4]. A row interrupted by
      // captions is the row's cue still, finished when its carriage return
      // comes: the text service sends RTD, a PAC back to the row's indent and
      // the row again from its start [CTA-608-E D.3], which the cursor
      // writes over what it had. Closed here, the part before the
      // interruption was a cue of its own, and the row came out twice: a
      // fragment, then the whole row.
      if (mode_ != CaptionMode::TEXT) {
        close_all_cues();
      }
      mode_ = CaptionMode::TEXT;
      break;

    default:
      break;
  }
}

void EIA608Decoder::open_popon_cue() {
  std::string text = displayed_.render();
  if (text.empty()) {
    return;
  }

  ORC_LOG_DEBUG("EIA608Decoder: Opening cue with text: '{}'", text);

  auto cue = std::make_shared<CaptionCue>(current_time_, -1.0, text);
  active_cues_.push_back(cue);
}

// No longer needed - we use close_all_cues() instead
// void EIA608Decoder::close_popon_cue()

void EIA608Decoder::swap_buffers() { std::swap(displayed_, nondisplayed_); }

void EIA608Decoder::ensure_rollup_cue_started() {
  if (active_cues_.empty()) {
    std::string text = displayed_.render();
    auto cue = std::make_shared<CaptionCue>(current_time_, -1.0, text);
    active_cues_.push_back(cue);
  } else {
    // Update existing cue text
    active_cues_[0]->text = displayed_.render();
  }
}

void EIA608Decoder::roll_up() {
  // Close current cue and emit it
  if (!active_cues_.empty()) {
    auto cue = active_cues_[0];
    cue->end_time = current_time_;
    emit_cue(*cue);
    active_cues_.clear();
  }

  // Scroll the window, and only the window: its top row is erased and the
  // base row left blank [47 CFR 15.119(f)(1)(iii)]. Scrolling the whole
  // display kept every row ever written, so each cue of a roll-up caption
  // carried the rows before it as well as its 2, 3 or 4 (on one news
  // broadcast, 372 words where cc_decoder, which keeps the window, has 162).
  displayed_.roll_up_window(base_row_, static_cast<size_t>(rollup_rows_));
}

void EIA608Decoder::start_roll_up(int rows) {
  if (mode_ != CaptionMode::ROLL_UP) {
    // Selecting roll-up style erases a pop-on or paint-on caption from both
    // memories, and with no roll-up caption on screen the window's base row
    // is row 15, the cursor at its first column [47 CFR 15.119(f)(1)(ii),
    // (x), CTA-608-E C.10].
    close_all_cues();
    displayed_.clear();
    nondisplayed_.clear();
    base_row_ = CaptionBuffer::MAX_ROWS - 1;
    displayed_.set_cursor(base_row_, 0);
  } else if (rows < rollup_rows_) {
    // A shallower window turns its top rows off, and they are erased [47 CFR
    // 15.119(f)(1)(iv)]. Otherwise a roll-up command leaves the cursor where
    // it was, as a roll-up caption resumes after an interruption [47 CFR
    // 15.119(f)(1)(ix), CTA-608-E C.15].
    const auto depth = static_cast<size_t>(rows);
    displayed_.clear_rows_above(base_row_ + 1 >= depth ? base_row_ + 1 - depth
                                                       : 0);
  }
  mode_ = CaptionMode::ROLL_UP;
  rollup_rows_ = rows;
}

void EIA608Decoder::move_roll_up_window(size_t row) {
  // A base row the window does not fit above gives way to the window's depth
  // [CTA-608-E C.4]: no caption text is lost.
  const auto depth = static_cast<size_t>(rollup_rows_);
  const size_t base = std::max(row, depth - 1);
  if (base != base_row_) {
    displayed_.move_window(base_row_, base, depth);
    base_row_ = base;
  }
}

void EIA608Decoder::end_of_caption() {
  // Whatever style put it up, the caption on screen ends here.
  close_all_cues();

  // The memories swap and neither is erased [47 CFR 15.119(f), (f)(2)], and
  // pop-on style is selected [CTA-608-E C.10, C.11]. The cursor stays where
  // the last character went: a decoder remembers one caption cursor, the
  // last memory written's, not one per memory [CTA-608-E C.15]. A stream can
  // come back from a commercial with the End of Caption, a few characters and
  // only then a PAC for the next row: a reset cursor put those characters at
  // the start of row 15, where the next row overwrote them.
  const CaptionBuffer& written =
      (mode_ == CaptionMode::POP_ON) ? nondisplayed_ : displayed_;
  const size_t row = written.get_row();
  const size_t col = written.get_col();
  swap_buffers();
  nondisplayed_.restore_cursor(row, col);
  mode_ = CaptionMode::POP_ON;

  open_popon_cue();
}

void EIA608Decoder::ensure_painton_cue_started() {
  // A paint-on caption is the displayed memory as its characters arrive,
  // placed by the PACs and the cursor as any other [47 CFR 15.119(f)(3)].
  // Appending each character to the cue lost both, and a two-row caption ran
  // its rows together.
  const std::string text = displayed_.render();
  if (active_cues_.empty()) {
    active_cues_.push_back(
        std::make_shared<CaptionCue>(current_time_, -1.0, text));
    return;
  }
  active_cues_[0]->text = text;
}

void EIA608Decoder::text_new_line() {
  // Close the line the carriage return ends, then move down the page. A text
  // service fills its fifteen rows from the top and only scrolls once the
  // bottom row is used, unlike a Roll-Up window which is always at the bottom.
  close_all_cues();
  if (displayed_.get_row() + 1 < CaptionBuffer::MAX_ROWS) {
    displayed_.next_row();
  } else {
    displayed_.roll_up();
  }
}

void EIA608Decoder::ensure_text_cue_started() {
  // A text service delivers a line at a time, ended by the carriage return
  // that scrolls the page. The cue is that line rather than the whole page:
  // emitting the page on every roll would repeat all fifteen rows for each new
  // one. Its leading indent is kept, so a page of columns still lines up when
  // the cues are read one under the other.
  const std::string line = displayed_.render_row(displayed_.get_row());
  if (active_cues_.empty()) {
    active_cues_.push_back(
        std::make_shared<CaptionCue>(current_time_, -1.0, line));
    return;
  }
  active_cues_[0]->text = line;
}

void EIA608Decoder::note_text_rewind(size_t col) {
  // A PAC sending the cursor back along a text row that holds something
  // starts the row again: a text service sends a row a caption interrupted
  // again from its start [CTA-608-E D.3]. What the row read is kept until
  // the row ends (close_all_cues()).
  if (active_cues_.empty() || col >= displayed_.get_col() ||
      active_cues_[0]->text.empty()) {
    return;
  }
  const double start = text_rewinds_.empty() ? active_cues_[0]->start_time
                                             : text_rewinds_.back().end;
  text_rewinds_.push_back({start, current_time_, active_cues_[0]->text});
}

void EIA608Decoder::close_all_cues() {
  // A text row written again over itself is one cue when it ends reading as
  // it began and went on: what it read before is where it reads now. When
  // the second sending says something else — a damaged line, or a carriage
  // return lost between two rows — what the row read before was on screen,
  // and is a cue of its own.
  if (!text_rewinds_.empty()) {
    const std::string row =
        active_cues_.empty() ? std::string() : active_cues_[0]->text;
    double row_start = -1.0;
    for (const TextRewind& rewind : text_rewinds_) {
      if (row.compare(0, rewind.text.size(), rewind.text) != 0) {
        emit_cue(CaptionCue(rewind.start, rewind.end, rewind.text));
        row_start = rewind.end;
      }
    }
    if (row_start >= 0.0 && !active_cues_.empty()) {
      active_cues_[0]->start_time = row_start;
    }
    text_rewinds_.clear();
  }
  for (auto& cue : active_cues_) {
    cue->end_time = current_time_;
    emit_cue(*cue);
  }
  active_cues_.clear();
}

void EIA608Decoder::emit_cue(const CaptionCue& cue) {
  // Validate cue before emitting
  if (cue.end_time <= cue.start_time) {
    return;
  }

  if (cue.text.empty()) {
    return;
  }

  // Drop leading and trailing blank lines, and any trailing whitespace, but
  // leave the indent at the start of a line alone: it is the column the
  // transmission placed the text at, and a text service's page only reads
  // correctly while its columns still line up.
  std::string text = cue.text;
  while (!text.empty() && (text.front() == '\n' || text.front() == '\r')) {
    text.erase(text.begin());
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back()))) {
    text.pop_back();
  }

  if (text.find_first_not_of(" \t\n\r") == std::string::npos) {
    return;
  }

  emitted_cues_.emplace_back(cue.start_time, cue.end_time, text);
}

std::vector<CaptionCue> EIA608Decoder::finalize(double end_time) {
  current_time_ = end_time;
  close_all_cues();
  return emitted_cues_;
}

}  // namespace orc
