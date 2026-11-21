/*
 * File:        eia608_decoder.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     EIA-608 Closed Caption Decoder for timed text conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_EIA608_DECODER_H
#define ORC_EIA608_DECODER_H

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace orc {

/**
 * @brief EIA-608 Control Codes
 */
enum class EIA608ControlCode {
  RCL,  // Resume Caption Loading (Pop-On)
  EOC,  // End of Caption (swap buffers + display Pop-On)
  EDM,  // Erase Displayed Memory
  ENM,  // Erase Non-displayed Memory
  CR,   // Carriage Return (Roll-Up)
  RU2,  // Roll-Up 2 rows
  RU3,  // Roll-Up 3 rows
  RU4,  // Roll-Up 4 rows
  RDC,  // Resume Direct Captioning (Paint-On)
  TR,   // Text Restart (clear text memory, start the text service)
  RTD,  // Resume Text Display (continue the text service)
  UNKNOWN
};

/**
 * @brief Caption display mode
 */
enum class CaptionMode {
  POP_ON,    // Pop-On captions (prepare in buffer, then display all at once)
  ROLL_UP,   // Roll-Up captions (scroll up with new text)
  PAINT_ON,  // Paint-On captions (characters appear as received)
  TEXT       // Text service (T1-T4): a rolling page over the whole 15 rows
};

/**
 * @brief Timed caption cue
 */
struct CaptionCue {
  double start_time;  // Start time in seconds
  double end_time;    // End time in seconds
  std::string text;   // Caption text (may contain newlines)

  CaptionCue(double start, double end, std::string txt)
      : start_time(start), end_time(end), text(std::move(txt)) {}
};

/**
 * @brief A run of caption cells as UTF-8 text
 *
 * A CaptionBuffer holds a byte a column: a character of the basic set as it
 * was transmitted (20h-7Fh), or a two-byte character as a byte of its own —
 * the special characters (11h 30h-3Fh, CTA-608-E Table 49) as 80h-8Fh, the
 * extended characters (12h 20h-3Fh and 13h 20h-3Fh, CTA-608-E Tables 5-10) as
 * 90h-AFh and B0h-CFh. The basic set is ASCII but in ten places: 2Ah is "á",
 * 5Ch "é", 5Eh "í", 5Fh "ó", 60h "ú", 7Bh "ç", 7Ch "÷", 7Dh "Ñ", 7Eh "ñ", and
 * 7Fh the solid block [CTA-608-E Table 50, 47 CFR 15.119(g)]. Any other byte
 * is dropped.
 */
std::string eia608_cells_to_utf8(const std::string& cells);

/**
 * @brief Caption buffer (15 rows x 32 columns for EIA-608)
 */
class CaptionBuffer {
 public:
  static constexpr size_t MAX_ROWS = 15;
  static constexpr size_t MAX_COLS = 32;

  CaptionBuffer();

  /// Erase every row. The cursor stays where it was, an erasure moving no
  /// cursor [47 CFR 15.119(f), CTA-608-E C.15], but no further right than
  /// the 32nd column [CTA-608-E C.13].
  void clear();
  void write_char(char c);
  void set_cursor(size_t row, size_t col);
  /// The cursor where another memory left it, no further right than the
  /// 32nd column [CTA-608-E C.13].
  void restore_cursor(size_t row, size_t col);
  void next_row();  // Move to next row (for CR in Pop-On mode)
  /// Move the cursor one column left, unless it is in the row's first.
  void backspace();

  /**
   * @brief The buffer as UTF-8 text, one line per row that carries anything
   *
   * Rows are joined with newlines, not spaces: EIA-608 positions text by row
   * and column, and a caption or a text-service page whose rows are run
   * together loses the layout that made it readable — two services sharing one
   * row read as one sentence, and a table of scores collapses into its own
   * caption. Trailing spaces go; *leading* spaces are kept, because they are
   * the indent the transmission asked for.
   */
  std::string render() const;

  /// UTF-8 text of one row, trailing spaces removed, leading indent kept.
  std::string render_row(size_t row) const;

  void roll_up();

  /**
   * @brief Scroll a roll-up window: the |depth| rows ending at |base_row|
   *
   * The window's top row is erased, each other row moves up one, the base
   * row is left blank and the cursor goes to its first column [47 CFR
   * 15.119(f)(1)(iii)]. Rows outside the window are left alone.
   */
  void roll_up_window(size_t base_row, size_t depth);

  /**
   * @brief Move a roll-up window to a new base row, intact
   *
   * The |depth| rows ending at |from_base| move to end at |to_base|, without
   * erasing [47 CFR 15.119(f)(1)(ii), CTA-608-E C.7]; whatever else the
   * buffer held is erased, as a roll-up display has only its window.
   */
  void move_window(size_t from_base, size_t to_base, size_t depth);

  /// Erase the rows above |first_row|: the rows a roll-up window of fewer
  /// rows turns off [47 CFR 15.119(f)(1)(iv)].
  void clear_rows_above(size_t first_row);

  /**
   * @brief The buffer's rows as written, top row first
   *
   * A byte a column, as eia608_cells_to_utf8() reads them. Rows are not
   * padded, and write_char() deliberately allows a row to run past MAX_COLS;
   * a caller presenting the 32-column EIA-608 display grid clips them.
   */
  const std::array<std::string, MAX_ROWS>& rows() const { return rows_; }

  // Getters for debugging
  size_t get_row() const { return current_row_; }
  size_t get_col() const { return current_col_; }

 private:
  std::array<std::string, MAX_ROWS> rows_;
  size_t current_row_;
  size_t current_col_;
};

/**
 * @brief EIA-608 Closed Caption Decoder
 *
 * Converts raw EIA-608 byte pairs to timed text cues suitable for mov_text.
 * Handles the three caption modes (Pop-On, Roll-Up, Paint-On) and the text
 * services' rolling page.
 *
 * It decodes ONE service. Line 21 carries four of them multiplexed together,
 * so unless the recording used only one, put an EIA608ServiceDemux
 * (<orc/support/eia608_service_demux.h>) in front and feed this only the pairs
 * it accepts — otherwise two services' text lands in one buffer.
 */
class EIA608Decoder {
 public:
  EIA608Decoder();
  ~EIA608Decoder() = default;

  /**
   * @brief Process a pair of EIA-608 bytes at a given timestamp
   * @param timestamp Time in seconds
   * @param byte1 First data byte (with parity bit removed)
   * @param byte2 Second data byte (with parity bit removed)
   */
  void process_bytes(double timestamp, uint8_t byte1, uint8_t byte2);

  /**
   * @brief Finalize decoding and get all emitted cues
   * @param end_time Final timestamp to close any open cues
   * @return Vector of timed caption cues
   */
  std::vector<CaptionCue> finalize(double end_time);

  /**
   * @brief Get currently accumulated cues (without finalizing)
   */
  const std::vector<CaptionCue>& get_cues() const { return emitted_cues_; }

  /**
   * @brief The caption that would be on screen right now
   *
   * In Pop-On mode this is the buffer the last End of Caption code swapped in;
   * in Roll-Up, Paint-On and Text modes it is the buffer being written to.
   * Erase Displayed Memory empties it. Unlike the cue list, which only gains a
   * caption once that caption has been taken off screen, this is the decoder's
   * live display state — which is what a viewer following the stream needs.
   *
   * Reference into the decoder; the contents change on the next
   * process_bytes() call.
   */
  const CaptionBuffer& displayed() const { return displayed_; }

  /// Caption mode the stream last selected (RCL, RU2-RU4, RDC or TR/RTD)
  CaptionMode mode() const { return mode_; }

  /// Window height the stream last selected for Roll-Up mode (2, 3 or 4)
  int rollup_rows() const { return rollup_rows_; }

  /// The bottom row of the roll-up window, 0-based (14 is row 15)
  size_t base_row() const { return base_row_; }

 private:
  // Internal state
  CaptionMode mode_;
  CaptionBuffer displayed_;
  CaptionBuffer nondisplayed_;
  int rollup_rows_;
  // The roll-up window's bottom row, 0-based [47 CFR 15.119(f)(1)(i)].
  size_t base_row_;
  double current_time_;

  std::vector<std::shared_ptr<CaptionCue>> active_cues_;
  std::vector<CaptionCue> emitted_cues_;

  // What a text row read before a PAC sent the cursor back along it to write
  // it again, and when (see note_text_rewind()).
  struct TextRewind {
    double start;
    double end;
    std::string text;
  };
  std::vector<TextRewind> text_rewinds_;

  // Processing helpers
  EIA608ControlCode decode_control_code(uint8_t byte1, uint8_t byte2);
  bool decode_pac(uint8_t byte1, uint8_t byte2, size_t& row, size_t& col);
  void handle_control_code(EIA608ControlCode code);
  // |replaces_previous|: an extended character, which takes the place of the
  // character before it.
  void handle_printable_char(char c, bool replaces_previous = false);

  // Mode-specific handlers
  void open_popon_cue();
  void close_popon_cue();
  void swap_buffers();
  void ensure_rollup_cue_started();
  void roll_up();
  void start_roll_up(int rows);
  void move_roll_up_window(size_t row);
  void end_of_caption();
  void ensure_painton_cue_started();
  void ensure_text_cue_started();
  void text_new_line();
  void note_text_rewind(size_t col);

  // Cue management
  void close_all_cues();
  void emit_cue(const CaptionCue& cue);
};

}  // namespace orc

#endif  // ORC_EIA608_DECODER_H
