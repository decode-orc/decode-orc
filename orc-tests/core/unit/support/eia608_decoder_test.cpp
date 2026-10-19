/*
 * File:        eia608_decoder_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the EIA-608 caption decoder's rendering and its
 *              text-service mode
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/support/eia608_decoder.h>

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace orc_unit_test {
namespace {

constexpr uint8_t kMisc = 0x14;  // Channel 1 miscellaneous control code
constexpr uint8_t kRCL = 0x20;   // Resume Caption Loading (pop-on)
constexpr uint8_t kRU2 = 0x25;   // Roll-Up Captions, 2 rows
constexpr uint8_t kRU3 = 0x26;   // Roll-Up Captions, 3 rows
constexpr uint8_t kRU4 = 0x27;   // Roll-Up Captions, 4 rows
constexpr uint8_t kRDC = 0x29;   // Resume Direct Captioning (paint-on)
constexpr uint8_t kEDM = 0x2C;   // Erase Displayed Memory
constexpr uint8_t kCR = 0x2D;    // Carriage Return
constexpr uint8_t kEOC = 0x2F;   // End of Caption
constexpr uint8_t kTR = 0x2A;    // Text Restart
constexpr uint8_t kRTD = 0x2B;   // Resume Text Display

// Preamble address codes (CTA-608-E Table 53), channel 1. The first byte
// selects a pair of rows, bit 5 of the second picks one of the pair, and its
// indent field moves the cursor along in steps of four columns.
//
// 0x11 addresses rows 1 and 2, which are indices 0 and 1.
std::pair<uint8_t, uint8_t> pac(int row_index, int indent_steps) {
  const bool odd_of_pair = (row_index % 2) == 1;
  const uint8_t byte2 = static_cast<uint8_t>(
      0x50 | (odd_of_pair ? 0x20 : 0x00) | ((indent_steps & 0x07) << 1));
  return {0x11, byte2};
}

// Row 14 (index 13), column 0 — one above the bottom of the display.
std::pair<uint8_t, uint8_t> pac_row14() { return {0x14, 0x50}; }

// A preamble address code for row |row| (1-15), channel 1, at |indent_steps|
// times four columns [CTA-608-E Table 53]: the first byte names a pair of rows
// (row 11 alone), 20h in the second the pair's second row.
std::pair<uint8_t, uint8_t> pac_at(int row, int indent_steps) {
  static constexpr std::array<uint8_t, 16> kFirst = {
      0x00, 0x11, 0x11, 0x12, 0x12, 0x15, 0x15, 0x16,
      0x16, 0x17, 0x17, 0x10, 0x13, 0x13, 0x14, 0x14};
  const bool second_of_pair =
      row <= 10 ? (row % 2) == 0 : (row == 13 || row == 15);
  return {kFirst[static_cast<size_t>(row)],
          static_cast<uint8_t>((second_of_pair ? 0x60 : 0x40) | 0x10 |
                               ((indent_steps & 0x07) << 1))};
}

// Row 15 (index 14), the bottom row, at |indent_steps| times four columns.
std::pair<uint8_t, uint8_t> pac_row15(int indent_steps = 0) {
  return {0x14, static_cast<uint8_t>(0x70 | ((indent_steps & 0x07) << 1))};
}

// Characters as an encoder sends them, two to a pair, the last padded with a
// null.
std::vector<std::pair<uint8_t, uint8_t>> chars(const std::string& text) {
  std::vector<std::pair<uint8_t, uint8_t>> pairs;
  for (size_t i = 0; i < text.size(); i += 2) {
    pairs.emplace_back(
        static_cast<uint8_t>(text[i]),
        i + 1 < text.size() ? static_cast<uint8_t>(text[i + 1]) : uint8_t{0});
  }
  return pairs;
}

void feed(orc::EIA608Decoder& decoder, double& time,
          const std::vector<std::pair<uint8_t, uint8_t>>& pairs) {
  for (const auto& pair : pairs) {
    time += 1.0 / 29.97;
    decoder.process_bytes(time, pair.first, pair.second);
  }
}

std::vector<std::string> texts(const std::vector<orc::CaptionCue>& cues) {
  std::vector<std::string> out;
  for (const auto& cue : cues) {
    out.push_back(cue.text);
  }
  return out;
}

void feed(orc::EIA608Decoder& decoder, double& time,
          std::initializer_list<std::pair<uint8_t, uint8_t>> pairs) {
  for (const auto& pair : pairs) {
    time += 1.0 / 29.97;
    decoder.process_bytes(time, pair.first, pair.second);
  }
}

}  // namespace

TEST(CaptionBufferTest, RowsAreSeparateLinesAndKeepTheirIndent) {
  orc::CaptionBuffer buffer;
  buffer.set_cursor(13, 4);
  buffer.write_char('H');
  buffer.write_char('I');
  buffer.set_cursor(14, 0);
  buffer.write_char('Y');
  buffer.write_char('O');

  // Rows joined by newlines, not spaces: run together, two rows of a
  // text-service page read as one sentence and the columns stop lining up.
  EXPECT_EQ(buffer.render(), "    HI\nYO");
  EXPECT_EQ(buffer.render_row(13), "    HI");
  EXPECT_EQ(buffer.render_row(14), "YO");
  EXPECT_EQ(buffer.render_row(0), "");
}

TEST(CaptionBufferTest, RenderRowTrimsTrailingSpacesOnly) {
  orc::CaptionBuffer buffer;
  buffer.set_cursor(0, 2);
  buffer.write_char('A');
  buffer.write_char(' ');
  buffer.write_char(' ');

  EXPECT_EQ(buffer.render_row(0), "  A");
}

TEST(EIA608DecoderTest, PopOnCaptionKeepsItsRowsApart) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time,
       {{kMisc, kRCL},
        pac_row14(),
        {'H', 'I'},
        {kMisc, kCR},  // pop-on: move down a row for the second line
        {'Y', 'O'},
        {kMisc, kEOC},
        {kMisc, kEDM}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "HI\nYO");
}

TEST(EIA608DecoderTest, TextRestartSelectsTheTextServiceMode) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time, {{kMisc, kTR}, {'A', 'B'}});

  EXPECT_EQ(decoder.mode(), orc::CaptionMode::TEXT);
}

TEST(EIA608DecoderTest, TextServicePageFillsDownwardsAndKeepsColumns) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  // The shape a text service such as a scores page uses: a label at column 0
  // and a figure indented across the row, one line per carriage return.
  feed(decoder, time,
       {{kMisc, kTR},  // start the text service at the top of the page
        {'C', 'H'},    // row 0, columns 0-1
        pac(0, 4),     // same row, column 16
        {'3', '6'},    //
        {kMisc, kCR},  // end of the line
        {'H', 'O'},    // row 1
        pac(1, 4),     // same row, column 16
        {'2', '1'},    //
        {kMisc, kCR}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 2u);
  // The indent survives: a page of listings or scores is only readable while
  // its columns line up.
  EXPECT_EQ(cues[0].text, "CH              36");
  EXPECT_EQ(cues[1].text, "HO              21");
}

// CTA-608-E Section 7.4: a PAC in Text Mode moves the cursor along the current
// row only. A text service can send every line as a carriage return, a PAC
// addressing row 1 and the line's text; followed as an address, the PAC put
// each line over the one before it.
TEST(EIA608DecoderTest, TextServicePacsIndentTheCurrentRowOnly) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time,
       {{kMisc, kTR},
        pac(0, 0),  // row 1, column 0: the first row anyway
        {'L', 'I'},
        {'S', 'T'},
        {kMisc, kCR},
        pac(0, 0),  // row 1 again, which in Text Mode means this row
        {'D', 'A'},
        {kMisc, kCR},
        pac(0, 1),  // row 1 again, indented by four columns
        {'X', 0x00},
        {kMisc, kCR}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 3u);
  EXPECT_EQ(cues[0].text, "LIST");
  EXPECT_EQ(cues[1].text, "DA");
  EXPECT_EQ(cues[2].text, "    X");

  const auto& rows = decoder.displayed().rows();
  EXPECT_EQ(rows[0], "LIST");
  EXPECT_EQ(rows[1], "DA");
  EXPECT_EQ(rows[2], "    X");
}

// CTA-608-E Section 7.4: in Text Mode a mid-row code takes a column as a space
// does. A text service can separate words with them: "CALLED", a mid-row
// code, "PLUS" reads "CALLED PLUS", not "CALLEDPLUS".
TEST(EIA608DecoderTest, TextServiceMidRowCodesTakeAColumn) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time,
       {{kMisc, kTR},
        {'C', 'A'},
        {0x11, 0x25},  // mid-row code: white italics
        {'P', 'L'},
        {0x11, 0x20},  // mid-row code: white
        {'U', 'S'},
        {kMisc, kCR}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "CA PL US");
}

// A mid-row code takes a column in a caption too: "THE", italics,
// "ENTERPRISE", plain, "IS" reads "THE ENTERPRISE IS" [CTA-608-E Section 7.4,
// Section B.3], not "THEENTERPRISEIS".
TEST(EIA608DecoderTest, CaptionMidRowCodesTakeAColumn) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time,
       {{kMisc, kRCL},
        pac_row14(),
        {'H', 'I'},
        {0x11, 0x2E},  // mid-row code: italics
        {'Y', 'O'},
        {0x11, 0x20},  // mid-row code: white
        {'!', 0x00},
        {kMisc, kEOC},
        {kMisc, kEDM}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "HI YO !");
}

// A tab offset moves the cursor one to three columns right and writes nothing
// [CTA-608-E Table 52].
TEST(EIA608DecoderTest, TabOffsetsMoveTheCursorRight) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time,
       {{kMisc, kTR},
        {'A', 0x00},
        {0x17, 0x22},  // tab offset 2
        {'B', 0x00},
        {0x17, 0x21},  // tab offset 1
        {'C', 0x00},
        {kMisc, kCR}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "A  B C");
}

TEST(EIA608DecoderTest, TextServiceScrollsOnceThePageIsFull) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time, {{kMisc, kTR}});
  // Write one character per row for the whole 15-row page, then two more.
  for (int line = 0; line < 17; ++line) {
    const char ch = static_cast<char>('A' + line);
    feed(decoder, time, {{static_cast<uint8_t>(ch), 0x00}, {kMisc, kCR}});
  }

  // Nothing is lost from the cue list — every completed line was emitted.
  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 17u);
  EXPECT_EQ(cues.front().text, "A");
  EXPECT_EQ(cues.back().text, "Q");

  // The page holds the last lines written: A, B and C scrolled off the top,
  // and the carriage return after Q left the bottom row waiting for the next.
  const auto& rows = decoder.displayed().rows();
  EXPECT_EQ(rows[0], "D");
  EXPECT_EQ(rows[orc::CaptionBuffer::MAX_ROWS - 2], "Q");
  EXPECT_EQ(rows[orc::CaptionBuffer::MAX_ROWS - 1], "");
}

TEST(EIA608DecoderTest, TwoEndOfCaptionCodesAreTwoCaptions) {
  // The decoder used to suppress a second End of Caption arriving within a
  // tenth of a second, which at 29.97 fps merged captions three frames apart.
  // Suppressing the encoder's duplicate copy is the demux's job now, and it
  // does it by adjacency rather than by elapsed time.
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time,
       {{kMisc, kRCL},
        {'A', 'A'},
        {kMisc, kEOC},
        {'B', 'B'},
        {kMisc, kEOC},
        {kMisc, kEDM}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 2u);
  EXPECT_EQ(cues[0].text, "AA");
  // No PAC placed the second caption: it went on from the column the first
  // left the cursor at [CTA-608-E C.15].
  EXPECT_EQ(cues[1].text, "  BB");
}

// A roll-up caption's cue is its window: the base row and the rows above it up
// to the depth RU2, RU3 or RU4 chose [47 CFR 15.119(f)(1)]. Each carriage
// return scrolls the window, erasing its top row; scrolling the whole display
// had each cue carry every row before it (on one news broadcast, 372 words
// where the captions held 162).
TEST(EIA608DecoderTest, RollUpCuesHoldOnlyTheirWindow) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time, {{kMisc, kRU2}, {kMisc, kCR}, pac_row15()});
  for (const char* row : {"IRAQ", "LAUNCHES", "A NEW", "PLAN"}) {
    feed(decoder, time, chars(row));
    feed(decoder, time, {{kMisc, kRU2}, {kMisc, kCR}, pac_row15()});
  }

  const auto cues = decoder.finalize(time + 1.0);
  EXPECT_THAT(texts(cues),
              testing::ElementsAre("IRAQ", "IRAQ\nLAUNCHES", "LAUNCHES\nA NEW",
                                   "A NEW\nPLAN"));
  EXPECT_EQ(decoder.base_row(), 14u);
}

// A PAC naming another base row moves the window there intact [47 CFR
// 15.119(f)(1)(ii), CTA-608-E C.7], and the carriage returns scroll it there.
TEST(EIA608DecoderTest, RollUpWindowMovesWithItsBaseRow) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time, {{kMisc, kRU3}, pac_row15()});
  feed(decoder, time, chars("ONE"));
  feed(decoder, time, {{kMisc, kCR}});
  feed(decoder, time, chars("TWO"));
  feed(decoder, time, {pac_at(10, 2)});  // row 10, column 8
  feed(decoder, time, chars("TOO"));

  const auto& rows = decoder.displayed().rows();
  EXPECT_EQ(decoder.base_row(), 9u);
  EXPECT_EQ(rows[8], "ONE");
  EXPECT_EQ(rows[9], "TWO     TOO");
  EXPECT_EQ(rows[13], "");
  EXPECT_EQ(rows[14], "");

  feed(decoder, time, {{kMisc, kCR}});
  EXPECT_EQ(rows[7], "ONE");
  EXPECT_EQ(rows[8], "TWO     TOO");
  EXPECT_EQ(rows[9], "");
  feed(decoder, time, chars("THREE"));
  feed(decoder, time, {{kMisc, kCR}});
  // Three rows deep: ONE has scrolled off the top of the window.
  EXPECT_EQ(rows[6], "");
  EXPECT_EQ(rows[7], "TWO     TOO");
  EXPECT_EQ(rows[8], "THREE");
  EXPECT_EQ(rows[9], "");
}

// A base row the window cannot fit above gives way to the window's depth
// [CTA-608-E C.4]; and a shallower window erases the rows it turns off [47 CFR
// 15.119(f)(1)(iv)].
TEST(EIA608DecoderTest, RollUpDepthOutranksTheBaseRow) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time, {{kMisc, kRU4}, pac(1, 0)});  // row 2: no room for four
  EXPECT_EQ(decoder.base_row(), 3u);
  feed(decoder, time, chars("A"));
  feed(decoder, time, {{kMisc, kCR}});
  feed(decoder, time, chars("B"));
  feed(decoder, time, {{kMisc, kCR}});
  feed(decoder, time, chars("C"));
  const auto& rows = decoder.displayed().rows();
  EXPECT_EQ(rows[1], "A");
  EXPECT_EQ(rows[2], "B");
  EXPECT_EQ(rows[3], "C");

  feed(decoder, time, {{kMisc, kRU2}});
  EXPECT_EQ(rows[1], "");
  EXPECT_EQ(rows[2], "B");
  EXPECT_EQ(rows[3], "C");
}

// Selecting roll-up erases a pop-on caption from both memories [47 CFR
// 15.119(f)(1)(x)].
TEST(EIA608DecoderTest, RollUpErasesAPopOnCaption) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time, {{kMisc, kRCL}, pac_row14()});
  feed(decoder, time, chars("SHOWN"));
  feed(decoder, time, {{kMisc, kEOC}, {kMisc, kRCL}, pac_row14()});
  feed(decoder, time, chars("LOADED"));
  feed(decoder, time, {{kMisc, kRU2}, {kMisc, kCR}, pac_row15()});
  feed(decoder, time, chars("ROLLING"));
  feed(decoder, time, {{kMisc, kCR}});

  const auto cues = decoder.finalize(time + 1.0);
  EXPECT_THAT(texts(cues), testing::ElementsAre("SHOWN", "ROLLING"));
}

// A paint-on caption is the displayed memory as it is painted, its PACs
// placing the rows [47 CFR 15.119(f)(3)]; appended character by character, a
// two-row caption ran its rows together.
TEST(EIA608DecoderTest, PaintOnCueIsTheDisplayAsPainted) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time, {{kMisc, kEDM}, {kMisc, kRDC}, pac_row14()});
  feed(decoder, time, chars("WE WERE DRINKING"));
  feed(decoder, time, {pac_row15()});
  feed(decoder, time, chars("SO MUCH JUICE"));
  // A second burst of the same caption, RDC first as B.8.2 has it.
  feed(decoder, time, {{kMisc, kRDC}, {0x14, 0x7A}});  // row 15, column 20
  feed(decoder, time, chars("!"));
  feed(decoder, time, {{kMisc, kEDM}});

  const auto cues = decoder.finalize(time + 1.0);
  EXPECT_THAT(texts(cues),
              testing::ElementsAre("WE WERE DRINKING\nSO MUCH JUICE       !"));
}

// End of Caption swaps the memories and erases neither [47 CFR 15.119(f)]:
// the caption it took off the screen comes back on the next one.
TEST(EIA608DecoderTest, EndOfCaptionSwapsTheMemoriesWithoutErasing) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time, {{kMisc, kRCL}, pac_row15()});
  feed(decoder, time, chars("AGAIN"));
  feed(decoder, time, {{kMisc, kEOC}});
  feed(decoder, time, {{kMisc, kEOC}});
  feed(decoder, time, {{kMisc, kEOC}});
  feed(decoder, time, {{kMisc, kEDM}});

  const auto cues = decoder.finalize(time + 1.0);
  EXPECT_THAT(texts(cues), testing::ElementsAre("AGAIN", "AGAIN"));
}

// One caption cursor is remembered, not one for each memory, and neither an
// End of Caption nor an erasure moves it [CTA-608-E C.15]. A stream can come
// back from a commercial with an End of Caption, a few characters and only
// then a PAC to row 15: a cursor reset to the start of row 15 put those
// characters where the next row overwrote them.
TEST(EIA608DecoderTest, EndOfCaptionKeepsTheCaptionCursor) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time, {{kMisc, kRCL}, pac_row15(1)});
  feed(decoder, time, chars("ON THE WRONG LIST."));
  feed(decoder, time, {{kMisc, kEDM}, {kMisc, kEOC}});
  feed(decoder, time, chars("Woman ]"));
  feed(decoder, time, {{0x14, 0x6E}});  // row 15, italics, column 0
  feed(decoder, time, chars("OH, I CAN'T."));
  feed(decoder, time, {{kMisc, kEDM}, {kMisc, kEOC}, {kMisc, kEDM}});

  const auto cues = decoder.finalize(time + 1.0);
  EXPECT_THAT(texts(cues),
              testing::ElementsAre("    ON THE WRONG LIST.",
                                   "OH, I CAN'T.          Woman ]"));
}

// The cursor a row ran past the 32nd column with is carried no further than
// the 32nd column [CTA-608-E C.13]: what follows starts on the screen.
TEST(EIA608DecoderTest, TheRememberedCursorStaysOnTheScreen) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time, {{kMisc, kRCL}, pac_row15()});
  feed(decoder, time, chars(std::string(40, 'A')));
  feed(decoder, time, {{kMisc, kEOC}});
  feed(decoder, time, chars("B"));
  feed(decoder, time, {{kMisc, kEOC}, {kMisc, kEDM}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 2u);
  EXPECT_EQ(cues[1].text, std::string(31, ' ') + "B");
}

// A text row interrupted by captions is sent again after Resume Text Display
// and a PAC back to its indent [CTA-608-E 7.4, D.3]: it is one row, written
// over itself, not a fragment and then the row.
TEST(EIA608DecoderTest, TextRowResentAfterAnInterruptionIsOneCue) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time, {{kMisc, kTR}, pac(0, 0)});
  feed(decoder, time, chars("pro-Abacha gro"));
  feed(decoder, time, {{kMisc, kRTD}, pac(0, 0)});
  feed(decoder, time, chars("pro-Abacha group, which scored"));
  feed(decoder, time, {{kMisc, kCR}});
  feed(decoder, time, chars("just over"));
  feed(decoder, time, {{kMisc, kRTD}});  // resumed, no PAC: carry on
  feed(decoder, time, chars(" 23 percent."));
  feed(decoder, time, {{kMisc, kCR}});

  const auto cues = decoder.finalize(time + 1.0);
  EXPECT_THAT(texts(cues),
              testing::ElementsAre("pro-Abacha group, which scored",
                                   "just over 23 percent."));
}

// When the row sent again says something else — damaged — what it read before
// was on screen too, and is kept.
TEST(EIA608DecoderTest, TextRowResentDifferentlyKeepsWhatItReadBefore) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time, {{kMisc, kTR}, pac(0, 0)});
  feed(decoder, time, chars("ABC DEF"));
  feed(decoder, time, {{kMisc, kRTD}, pac(0, 0)});
  feed(decoder, time, chars("ABX"));
  feed(decoder, time, {{kMisc, kCR}});

  const auto cues = decoder.finalize(time + 1.0);
  EXPECT_THAT(texts(cues), testing::ElementsAre("ABC DEF", "ABX DEF"));
  ASSERT_EQ(cues.size(), 2u);
  EXPECT_LE(cues[0].end_time, cues[1].start_time);
}

// 13h addresses rows 12 and 13 [CTA-608-E Table 53]: a caption on rows 13 and
// 14 keeps both its lines.
TEST(EIA608DecoderTest, PacsAddressRowsTwelveAndThirteen) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time,
       {{kMisc, kRCL},
        {0x13, 0x70},  // row 13, column 0
        {'H', 'I'},
        {0x14, 0x50},  // row 14, column 0
        {'Y', 'O'},
        {0x13, 0x52},  // row 12, column 4
        {'U', 'P'},
        {kMisc, kEOC},
        {kMisc, kEDM}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "    UP\nHI\nYO");
}

// Data channel 2 spells its preamble address codes with 08h set in the first
// byte [CTA-608-E Table 53]: CC2 and TEXT2 are placed as CC1 and TEXT1 are.
TEST(EIA608DecoderTest, DataChannel2PacsPlaceTheCursorToo) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time,
       {{0x1C, kRCL},
        {0x1C, 0x50},  // row 14, column 0, channel 2's spelling
        {'H', 'I'},
        {0x19, 0x52},  // row 1, column 4
        {'Y', 'O'},
        {0x1C, kEOC},
        {0x1C, kEDM}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "    YO\nHI");
}

// The music note and the other special characters [CTA-608-E Table 49] take a
// column each, and come out as UTF-8.
TEST(EIA608DecoderTest, SpecialCharactersAreWrittenAsUtf8) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time,
       {{kMisc, kRCL},
        pac_row14(),
        {0x11, 0x37},  // music note
        {' ', 'L'},
        {'A', ' '},
        {0x11, 0x37},  // music note
        {0x11, 0x30},  // registered mark
        {0x19, 0x31},  // degree sign, data channel 2's spelling
        {kMisc, kEOC},
        {kMisc, kEDM}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "\u266A LA \u266A\u00AE\u00B0");
}

// An extended character takes the place of the standard character sent before
// it for receivers without the extended set [CTA-608-E §6.4.2] — unless it is
// the first character on its row.
TEST(EIA608DecoderTest, ExtendedCharactersReplaceTheCharacterBeforeThem) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time,
       {{kMisc, kRCL},
        pac_row14(),
        {0x12, 0x2E},  // opening double quotes, first on the row
        {'U', 0x00},
        {0x12, 0x24},  // U with diaeresis, in place of the U
        {'B', 'E'},
        {'R', '"'},
        {0x1A, 0x2F},  // closing double quotes, data channel 2's spelling
        {kMisc, kEOC},
        {kMisc, kEDM}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "\u201C\u00DCBER\u201D");
}

// In paint-on mode the cue is the display as the characters arrive, and an
// extended character still takes the place of the one before it.
TEST(EIA608DecoderTest, ExtendedCharactersReplaceInPaintOnCues) {
  orc::EIA608Decoder decoder;
  double time = 0.0;

  feed(decoder, time,
       {{kMisc, 0x29},  // Resume Direct Captioning
        pac_row14(),
        {'N', 'o'},
        {'e', 'l'},
        {kMisc, kEDM}});
  feed(decoder, time,
       {{kMisc, 0x29}, pac_row14(), {'N', 'o'}, {'e', 0x00}, {0x12, 0x36}});

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 2u);
  EXPECT_EQ(cues[0].text, "Noel");
  EXPECT_EQ(cues[1].text, "No\u00EB");
}

// The basic set is ASCII but in ten places; 7Fh is the solid block a receiver
// shows for a character whose parity failed [CTA-608-E Table 50].
TEST(EIA608DecoderTest, TheBasicSetIsNotQuiteAscii) {
  EXPECT_EQ(orc::eia608_cells_to_utf8("*\\^_`{|}~"),
            "\u00E1\u00E9\u00ED\u00F3\u00FA\u00E7\u00F7\u00D1\u00F1");
  EXPECT_EQ(orc::eia608_cells_to_utf8("A\x7F"), "A\u25A0");
  EXPECT_EQ(orc::eia608_cells_to_utf8("HI there!"), "HI there!");
  // Bytes no cell holds are dropped.
  const std::string stray = {'A', '\x01', '\xF0', 'B'};
  EXPECT_EQ(orc::eia608_cells_to_utf8(stray), "AB");
}

}  // namespace orc_unit_test
