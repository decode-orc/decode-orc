/*
 * File:        naplps_receiver_pace_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the receiver pace: how long the Norpak TTX 650
 * took over each command and figure of a NAPLPS page
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "naplps_receiver_pace.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "naplps_code_env.h"
#include "naplps_pdi.h"

namespace orc {
namespace {

/// A unit-screen length of |pixels| of the reference receiver.
constexpr double px(double pixels) { return pixels / 256.0; }

constexpr NaplpsReceiverPace kTtx = NaplpsReceiverPace::kNorpakTtx650;
constexpr NaplpsReceiverPace kTu = NaplpsReceiverPace::kPanasonicTu2000x;

NabtsPrimitive character(uint8_t code, double width_px, double height_px) {
  NabtsPrimitive figure;
  figure.kind = NabtsPrimitiveKind::kCharacter;
  figure.character = code;
  figure.repertoire = NabtsPrimitive::Repertoire::kPrimary;
  figure.size = NabtsSize{px(width_px), px(height_px)};
  return figure;
}

NabtsPrimitive line(const NabtsPoint& from, const NabtsPoint& to) {
  NabtsPrimitive figure;
  figure.kind = NabtsPrimitiveKind::kLine;
  figure.points = {from, to};
  return figure;
}

NabtsPrimitive rectangle(const NabtsPoint& origin, const NabtsSize& size,
                         bool filled) {
  NabtsPrimitive figure;
  figure.kind = NabtsPrimitiveKind::kRectangle;
  figure.filled = filled;
  figure.origin = origin;
  figure.size = size;
  figure.points = {origin, {origin.x + size.dx, origin.y + size.dy}};
  return figure;
}

// A character in a cell the font ROM holds is copied from it: 3.4 ms, SPACE
// 1.55 ms, and 6.5 ms in colour mode 2, where its background is drawn too.
TEST(NaplpsReceiverPace, ACharacterFromTheRomTakesItsOwnTime) {
  EXPECT_EQ(naplps_pace_figure_us(kTtx, character('A', 6.4, 10.0)), 3400);
  EXPECT_EQ(naplps_pace_figure_us(kTtx, character(' ', 6.4, 10.0)), 1550);
  NabtsPrimitive with_background = character('A', 6.4, 10.0);
  with_background.colour_mode = NabtsColourMode::kMappedWithBackground;
  EXPECT_EQ(naplps_pace_figure_us(kTtx, with_background), 6500);
  // A SPACE drawn with a background is as dear as any character.
  with_background.character = ' ';
  EXPECT_EQ(naplps_pace_figure_us(kTtx, with_background), 6500);
}

// A face the field holds only once is copied from the ROM at the ROM's speed,
// however much of the field it leaves over — the TTX 650's normal face in 6 by
// 13 and 10 by 10 fields, its larger face in 8 by 12 and 11 by 12 ones (3.5
// and 4.0 ms a character through its firmware).
TEST(NaplpsReceiverPace, AFaceTheFieldHoldsOnceIsCopiedFromTheRom) {
  EXPECT_EQ(naplps_pace_figure_us(kTtx, character('A', 6.0, 13.0)), 3400);
  EXPECT_EQ(naplps_pace_figure_us(kTtx, character('A', 10.0, 10.0)), 3400);
  EXPECT_EQ(naplps_pace_figure_us(kTtx, character('A', 8.0, 12.0)), 4000);
  EXPECT_EQ(naplps_pace_figure_us(kTtx, character('A', 11.0, 12.0)), 4000);
  // Twice the normal face's cell is the face drawn scaled.
  EXPECT_EQ(naplps_pace_figure_us(kTtx, character('A', 12.0, 20.0)), 18000);
}

// A larger cell is drawn scaled, row by row: 10 ms and 0.4 ms a row of the
// cell, so 16 x 24 takes 19.6 ms (19.8 measured), and no cell is taller than
// the display.
TEST(NaplpsReceiverPace, AScaledCharacterCostsByItsHeight) {
  EXPECT_EQ(naplps_pace_figure_us(kTtx, character('A', 16.0, 24.0)), 19600);
  EXPECT_EQ(naplps_pace_figure_us(kTtx, character('A', 255.0, 199.0)), 89600);
  EXPECT_EQ(naplps_pace_figure_us(kTtx, character('A', 255.0, 400.0)),
            naplps_pace_figure_us(kTtx, character('A', 255.0, 200.0)));
  EXPECT_EQ(naplps_pace_figure_us(kTtx, character(' ', 64.0, 96.0)), 2000);
}

// A horizontal run is a fill and nearly free; every other pixel is plotted.
// With the command's own 3.5 ms, a 255-pixel horizontal line is the 7.3 ms the
// TTX 650 took (7.4 measured), and a 199-pixel vertical one about 64.
TEST(NaplpsReceiverPace, ALineCostsByHowMuchOfItIsNotHorizontal) {
  const int64_t pdi = naplps_pace_pdi_us(kTtx, 0x28);
  EXPECT_EQ(pdi, 3500);
  // Within a microsecond: the per-pixel costs are not whole microseconds.
  EXPECT_NEAR(
      pdi + naplps_pace_figure_us(kTtx, line({0.0, 0.5}, {px(255), 0.5})), 7341,
      1);
  EXPECT_NEAR(
      pdi + naplps_pace_figure_us(kTtx, line({0.5, 0.0}, {0.5, px(199)})),
      64210, 1);
  // A sloping line by its longer axis.
  EXPECT_EQ(naplps_pace_figure_us(kTtx, line({0.0, 0.0}, {px(190), px(60)})),
            3000 + 300 * 190);
}

// A filled rectangle is a run per row and its pixels, as far as it is on the
// display: the whole display area takes the 214 ms measured, and one hanging
// off the top right corner pays only for its 40 visible rows and 2,560 pixels.
TEST(NaplpsReceiverPace, AFilledRectangleCostsByItsRowsAndPixelsOnScreen) {
  const NabtsPrimitive whole =
      rectangle({0.0, 0.0}, {1.0, kNabtsDisplayAreaHeight}, true);
  EXPECT_NEAR(
      naplps_pace_pdi_us(kTtx, 0x31) + naplps_pace_figure_us(kTtx, whole),
      214360, 1);
  const NabtsPrimitive corner = rectangle({0.75, 0.625}, {0.5, 0.25}, true);
  EXPECT_NEAR(naplps_pace_figure_us(kTtx, corner), 3500 + 320 * 40 + 7168, 1);
  // Outlined, its two horizontal sides are runs and its two vertical sides are
  // plotted.
  const NabtsPrimitive outline =
      rectangle({0.25, 0.25}, {px(100), px(50)}, false);
  EXPECT_EQ(naplps_pace_figure_us(kTtx, outline), 6500 + 3 * 200 + 290 * 100);
}

// An arc is plotted a pixel at a time along its length, which is what makes a
// circle the dearest figure there is: one of radius 90 takes most of a second.
TEST(NaplpsReceiverPace, AnArcCostsByItsLength) {
  NabtsPrimitive circle;
  circle.kind = NabtsPrimitiveKind::kArc;
  // Through three points of a circle of radius 90 pixels, round to the start.
  circle.points = {{0.1, 0.4}, {0.1 + 2.0 * px(90), 0.4}, {0.1, 0.4}};
  const int64_t outlined = naplps_pace_figure_us(kTtx, circle);
  EXPECT_GT(outlined, 600000);
  EXPECT_LT(outlined, 900000);
  circle.filled = true;
  EXPECT_GT(naplps_pace_figure_us(kTtx, circle), outlined);
}

// Commands that draw nothing still took the receiver its time, and a clear
// the same in every colour mode.
TEST(NaplpsReceiverPace, CommandsThatDrawNothingStillTakeTime) {
  EXPECT_EQ(
      naplps_pace_pdi_us(kTtx, static_cast<uint8_t>(NaplpsPdi::kSetColour)),
      3100);
  EXPECT_EQ(naplps_pace_pdi_us(kTtx, static_cast<uint8_t>(NaplpsPdi::kText)),
            10800);
  EXPECT_EQ(naplps_pace_pdi_us(kTtx, static_cast<uint8_t>(NaplpsPdi::kWait)),
            2000);
  EXPECT_EQ(naplps_pace_pdi_us(kTtx, 0x7F), 0);
  EXPECT_EQ(naplps_pace_c0_us(kTtx, kNaplpsSo), 300);
  EXPECT_EQ(naplps_pace_c0_us(kTtx, kNaplpsApr), 900);
  EXPECT_EQ(naplps_pace_c0_us(kTtx, kNaplpsNsr), 9800);
  // ESC is charged as its C1 control and CS as the clear.
  EXPECT_EQ(naplps_pace_c0_us(kTtx, kNaplpsEsc), 0);
  EXPECT_EQ(naplps_pace_c0_us(kTtx, kNaplpsCs), 0);
  EXPECT_EQ(naplps_pace_clear_us(kTtx), 169000);
  EXPECT_EQ(
      naplps_pace_c1_us(kTtx, static_cast<uint8_t>(NaplpsC1::kDoubleSize)),
      9500);
  EXPECT_EQ(
      naplps_pace_c1_us(kTtx, static_cast<uint8_t>(NaplpsC1::kBlinkStart)),
      1800);
  EXPECT_EQ(naplps_pace_c1_us(kTtx, static_cast<uint8_t>(NaplpsC1::kRepeat)),
            1000);
}

// --- The Panasonic TU-2000x -------------------------------------------------

// The TU-2000x copied a character from its ROM in 4.7 ms (its larger face in
// 4.5), a SPACE in 3.5 and a character with its background in colour mode 2
// in 5.5.
TEST(NaplpsReceiverPace, TheTu2000xCopiesACharacterInItsOwnTime) {
  EXPECT_EQ(naplps_pace_figure_us(kTu, character('A', 6.4, 10.0)), 4700);
  EXPECT_EQ(naplps_pace_figure_us(kTu, character(' ', 6.4, 10.0)), 3500);
  EXPECT_EQ(naplps_pace_figure_us(kTu, character('A', 8.0, 12.0)), 4500);
  NabtsPrimitive with_background = character('A', 6.4, 10.0);
  with_background.colour_mode = NabtsColourMode::kMappedWithBackground;
  EXPECT_EQ(naplps_pace_figure_us(kTu, with_background), 5500);
}

// The TU-2000x picks the face that leaves the fewest rows of the field over,
// and copies it from the ROM however tall the field — 6 by 20, the normal face
// stretched down, took it 6.1 ms against 4.6 at 6 by 10, and 12 by 12 is its
// larger face once across — but runs a face it draws at twice its width
// through its scaling routine, by the cell's area up to 9,600 pixels: 12 by
// 20 took 20 to 28 ms, 64 by 96 467 ms, and nothing took more than 826 ms.
TEST(NaplpsReceiverPace, TheTu2000xScalesAFaceOnlyAtTwiceItsWidth) {
  EXPECT_EQ(naplps_pace_figure_us(kTu, character('A', 6.0, 20.0)), 4700);
  EXPECT_EQ(naplps_pace_figure_us(kTu, character('A', 12.0, 12.0)), 4500);
  EXPECT_EQ(naplps_pace_figure_us(kTu, character('A', 12.0, 20.0)),
            4600 + 80 * 240);
  EXPECT_EQ(naplps_pace_figure_us(kTu, character('A', 64.0, 96.0)),
            4600 + 80 * 6144);
  EXPECT_EQ(naplps_pace_figure_us(kTu, character('A', 255.0, 199.0)),
            4600 + 80 * 9600);
  EXPECT_EQ(naplps_pace_figure_us(kTu, character(' ', 64.0, 64.0)),
            3500 + 4 * 4096);
  // The TTX 650 scales the same 6 by 20 field, its face being twice as tall.
  EXPECT_EQ(naplps_pace_figure_us(kTtx, character('A', 6.0, 20.0)),
            10000 + 400 * 20);
}

// The TU-2000x plotted every pixel of a line, a sloping one at 0.65 ms and
// one on an axis at 0.18, and an outlined rectangle round its perimeter.
TEST(NaplpsReceiverPace, TheTu2000xPlotsEveryPixelOfALine) {
  EXPECT_NEAR(naplps_pace_figure_us(kTu, line({0.0, 0.5}, {px(255), 0.5})),
              1600 + 180 * 255, 1);
  EXPECT_NEAR(naplps_pace_figure_us(kTu, line({0.5, 0.0}, {0.5, px(199)})),
              1600 + 180 * 199, 1);
  EXPECT_NEAR(naplps_pace_figure_us(kTu, line({0.0, 0.0}, {px(190), px(60)})),
              1600 + 650 * 190, 1);
  const NabtsPrimitive outline =
      rectangle({0.25, 0.25}, {px(100), px(50)}, false);
  EXPECT_NEAR(naplps_pace_figure_us(kTu, outline), 1700 + 180 * 300, 1);
}

// It filled a rectangle by its rows and pixels — 0.77 ms a row, so the whole
// display area in the 260 ms measured — and a polygon at 10 ms a vertex and
// 1 ms a row, against the TTX 650's 4.1 and 0.6, but drew an arc at a little
// over half the TTX 650's cost.
TEST(NaplpsReceiverPace, TheTu2000xFillsByRowsAndDrawsArcsFaster) {
  const NabtsPrimitive whole =
      rectangle({0.0, 0.0}, {1.0, kNabtsDisplayAreaHeight}, true);
  EXPECT_NEAR(naplps_pace_figure_us(kTu, whole), 3700 + 770 * 200 + 2 * 51200,
              1);
  // 102.4 rows and 5,242.88 pixels.
  NabtsPrimitive triangle;
  triangle.kind = NabtsPrimitiveKind::kPolygon;
  triangle.filled = true;
  triangle.points = {{0.2, 0.1}, {0.6, 0.1}, {0.4, 0.5}};
  EXPECT_NEAR(naplps_pace_figure_us(kTu, triangle),
              3 * 10000 + 1000 * 102.4 + 8 * 5242.88, 1);
  EXPECT_NEAR(naplps_pace_figure_us(kTtx, triangle),
              3 * 4100 + 600 * 102.4 + 3 * 5242.88, 1);
  NabtsPrimitive circle;
  circle.kind = NabtsPrimitiveKind::kArc;
  circle.points = {{0.1, 0.4}, {0.1 + 2.0 * px(90), 0.4}, {0.1, 0.4}};
  const int64_t tu_arc = naplps_pace_figure_us(kTu, circle);
  const int64_t ttx_arc = naplps_pace_figure_us(kTtx, circle);
  EXPECT_GT(tu_arc, ttx_arc / 2);
  EXPECT_LT(tu_arc, 2 * ttx_arc / 3);
}

// It loaded the colour map in the vertical blank, so SET COLOR, BLINK and
// BLINK START wait for the next field: half of one on average among a page's
// other commands. It cleared the screen in 70 ms.
TEST(NaplpsReceiverPace, TheTu2000xWaitsForTheFieldToLoadAColour) {
  EXPECT_EQ(
      naplps_pace_pdi_us(kTu, static_cast<uint8_t>(NaplpsPdi::kSetColour)),
      8400);
  EXPECT_EQ(naplps_pace_pdi_us(kTu, static_cast<uint8_t>(NaplpsPdi::kBlink)),
            8500);
  EXPECT_EQ(naplps_pace_c1_us(kTu, static_cast<uint8_t>(NaplpsC1::kBlinkStart)),
            8400);
  EXPECT_EQ(naplps_pace_c1_us(kTu, static_cast<uint8_t>(NaplpsC1::kBlinkStop)),
            8400);
  EXPECT_EQ(naplps_pace_pdi_us(kTu, static_cast<uint8_t>(NaplpsPdi::kText)),
            3900);
  EXPECT_EQ(naplps_pace_pdi_us(kTu, 0x31), 2300);
  EXPECT_EQ(naplps_pace_c0_us(kTu, kNaplpsNsr), 3700);
  EXPECT_EQ(naplps_pace_c1_us(kTu, static_cast<uint8_t>(NaplpsC1::kDoubleSize)),
            2100);
  EXPECT_EQ(naplps_pace_clear_us(kTu), 70000);
  EXPECT_EQ(naplps_pace_macro_us(kTu), 1000);
  EXPECT_EQ(naplps_pace_definition_byte_us(kTu), 200);
}

// The two receivers swept a fill and a clear across the screen opposite ways.
TEST(NaplpsReceiverPace, EachReceiverSweepsAFillItsOwnWay) {
  EXPECT_EQ(naplps_pace_fill_sweep(kTtx), NabtsFillSweep::kBottomUp);
  EXPECT_EQ(naplps_pace_fill_sweep(kTu), NabtsFillSweep::kTopDown);
}

// With the pace off nothing takes any time.
TEST(NaplpsReceiverPace, NothingTakesTimeWithThePaceOff) {
  constexpr NaplpsReceiverPace kOff = NaplpsReceiverPace::kOff;
  EXPECT_EQ(naplps_pace_figure_us(kOff, character('A', 6.4, 10.0)), 0);
  EXPECT_EQ(naplps_pace_figure_us(
                kOff, rectangle({0.0, 0.0}, {1.0, 0.5}, /*filled=*/true)),
            0);
  EXPECT_EQ(naplps_pace_pdi_us(kOff, 0x31), 0);
  EXPECT_EQ(naplps_pace_c0_us(kOff, kNaplpsSo), 0);
  EXPECT_EQ(naplps_pace_c1_us(kOff, 0x46), 0);
  EXPECT_EQ(naplps_pace_clear_us(kOff), 0);
  EXPECT_EQ(naplps_pace_macro_us(kOff), 0);
  EXPECT_EQ(naplps_pace_definition_byte_us(kOff), 0);
}

}  // namespace
}  // namespace orc
