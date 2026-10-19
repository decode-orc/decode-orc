/*
 * File:        nabts_catalogue_view_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NAPLPS page to display-list conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_catalogue_view.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "nabts_record_catalogue.h"
#include "naplps_render_grid.h"
#include "vbi-services/teletext_page_decoder.h"
#include "vbi-services/teletext_slicer.h"

namespace orc {
namespace {

/// A snapshot holding one primitive, which is enough to check what the list
/// carries alongside its operations.
NabtsPageSnapshot pageWithOneLine(double pel) {
  NabtsPageSnapshot snapshot;
  nabts_default_colour_map(snapshot.colour_map);

  NabtsPrimitive primitive;
  primitive.kind = NabtsPrimitiveKind::kLine;
  primitive.points = {NabtsPoint{0.1, 0.1}, NabtsPoint{0.9, 0.6}};
  primitive.origin = primitive.points.front();
  primitive.logical_pel = NabtsSize{pel, pel};
  snapshot.primitives.push_back(primitive);
  return snapshot;
}

/// A character primitive at |x|, drawn at |appear_ms|.
NabtsPrimitive letter(char character, double x, uint32_t appear_ms) {
  NabtsPrimitive primitive;
  primitive.kind = NabtsPrimitiveKind::kCharacter;
  primitive.character = static_cast<uint8_t>(character);
  primitive.origin = NabtsPoint{x, 0.5};
  primitive.points = {primitive.origin};
  primitive.size = NabtsSize{1.0 / 40.0, 5.0 / 128.0};
  primitive.colour = kNabtsNominalWhite;
  primitive.appear_ms = appear_ms;
  return primitive;
}

// §5.3.2.8 WAIT: a page that writes a word a letter at a time shows it a letter
// at a time, so the letters of one run drawn at different moments are
// separate operations, each with its moment.
TEST(NabtsCatalogueViewTest, TextDrawnAcrossAPauseIsNotOneRun) {
  NabtsPageSnapshot snapshot;
  nabts_default_colour_map(snapshot.colour_map);
  snapshot.primitives = {letter('A', 0.1, 0), letter('B', 0.1 + 1.0 / 40.0, 0),
                         letter('C', 0.1 + 2.0 / 40.0, 700)};

  const CatalogueDisplayList list =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kTwiceVector);
  ASSERT_EQ(list.ops.size(), 2u);
  EXPECT_EQ(list.ops[0].text, "AB");
  EXPECT_EQ(list.ops[0].appear_ms, 0u);
  EXPECT_EQ(list.ops[1].text, "C");
  EXPECT_EQ(list.ops[1].appear_ms, 700u);
  EXPECT_TRUE(list.timed());
}

/// A non-spacing mark of the supplementary set (column 4) at |x|.
NabtsPrimitive mark(uint8_t code, double x, uint32_t appear_ms = 0) {
  NabtsPrimitive primitive = letter(static_cast<char>(code), x, appear_ms);
  primitive.repertoire = NabtsPrimitive::Repertoire::kSupplementary;
  return primitive;
}

/// The text runs a page's characters make in the vector mode.
std::vector<CatalogueDrawOp> text_runs(
    const std::vector<NabtsPrimitive>& primitives) {
  NabtsPageSnapshot snapshot;
  nabts_default_colour_map(snapshot.colour_map);
  snapshot.primitives = primitives;
  std::vector<CatalogueDrawOp> runs;
  for (const CatalogueDrawOp& op :
       nabts_page_display_list(snapshot, NaplpsRenderMode::kTwiceVector).ops) {
    if (op.kind == CatalogueDrawKind::kText) {
      runs.push_back(op);
    }
  }
  return runs;
}

constexpr double kField = 1.0 / 40.0;

// X3.110 §7.2 has a mark's code precede the letter it modifies, and §5.2 has
// the mark leave the cursor where it was: the pair is one character in one
// field. A mark that starts a run is composed onto the letter after it and
// takes no field of its own, so the run's later characters stay where the
// record put them.
TEST(NabtsCatalogueViewTest, AnAccentTakesNoFieldOfItsOwn) {
  const std::vector<CatalogueDrawOp> runs = text_runs(
      {mark(0x42, 0.1), letter('e', 0.1, 0), letter('t', 0.1 + kField, 0),
       letter('e', 0.1 + 2 * kField, 0)});
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].text, "e\u0301te");
  EXPECT_EQ(runs[0].character_count, 3);
  EXPECT_DOUBLE_EQ(runs[0].origin.x, 0.1);
  EXPECT_NEAR(runs[0].advance.dx, kField, 1e-12);
}

// The same within a run: the accented letter is the run's next character.
TEST(NabtsCatalogueViewTest, AnAccentInsideARunKeepsItInStep) {
  const std::vector<CatalogueDrawOp> runs = text_runs(
      {letter('c', 0.1, 0), letter('a', 0.1 + kField, 0),
       letter('f', 0.1 + 2 * kField, 0), mark(0x42, 0.1 + 3 * kField),
       letter('e', 0.1 + 3 * kField, 0), letter('!', 0.1 + 4 * kField, 0)});
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].text, "cafe\u0301!");
  EXPECT_EQ(runs[0].character_count, 5);
  EXPECT_NEAR(runs[0].advance.dx, kField, 1e-12);
}

// Table 27's vector overbar is non-spacing like the rest of column 4 (§5.2):
// a record that sends "S 4/0" before an "@" draws the "@" with the arrow over
// it, in the field the "@" was sent to.
TEST(NabtsCatalogueViewTest, AVectorOverbarComposesLikeAnyMark) {
  const double step = 7.0 / 256.0;
  const std::vector<CatalogueDrawOp> runs =
      text_runs({letter('%', 0.0, 0), letter('@', step, 0),
                 letter(' ', 2 * step, 0), mark(0x40, 3 * step),
                 letter('@', 3 * step, 0), letter('H', 4 * step, 0)});
  ASSERT_EQ(runs.size(), 1u);
  EXPECT_EQ(runs[0].text, "%@ @\u20D7H");
  EXPECT_EQ(runs[0].character_count, 5);
}

// A mark no letter follows is shown on its own: Table 26 codes a diacritical
// mark standing alone as the mark with SPACE, which is the Unicode spacing
// form of the mark — and where Unicode has none, as Table 27's vector overbar
// is, the mark over a no-break space.
TEST(NabtsCatalogueViewTest, AMarkWithNoLetterStandsInItsSpacingForm) {
  const std::vector<CatalogueDrawOp> trailing =
      text_runs({letter('a', 0.1, 0), mark(0x42, 0.1 + kField)});
  ASSERT_EQ(trailing.size(), 1u);
  EXPECT_EQ(trailing[0].text, "a\u00B4");
  EXPECT_EQ(trailing[0].character_count, 2);

  const std::vector<CatalogueDrawOp> spaced =
      text_runs({mark(0x48, 0.1), letter(' ', 0.1, 0)});
  ASSERT_EQ(spaced.size(), 1u);
  EXPECT_EQ(spaced[0].text, "\u00A8");

  const std::vector<CatalogueDrawOp> overbar = text_runs({mark(0x40, 0.1)});
  ASSERT_EQ(overbar.size(), 1u);
  EXPECT_EQ(overbar[0].text, "\u00A0\u20D7");
  EXPECT_EQ(overbar[0].character_count, 1);

  // A letter in another field is not the mark's letter.
  const std::vector<CatalogueDrawOp> elsewhere =
      text_runs({mark(0x42, 0.1), letter('e', 0.5, 0)});
  ASSERT_EQ(elsewhere.size(), 2u);
  EXPECT_EQ(elsewhere[0].text, "\u00B4");
  EXPECT_DOUBLE_EQ(elsewhere[0].origin.x, 0.1);
  EXPECT_EQ(elsewhere[1].text, "e");
}

// What a stage showed stays shown (§5.3.2.8): a mark drawn before a pause
// stands alone then, and the letter after it is drawn plain when it comes.
TEST(NabtsCatalogueViewTest, AMarkBeforeAPauseStandsAlone) {
  const std::vector<CatalogueDrawOp> runs =
      text_runs({mark(0x42, 0.1), letter('e', 0.1, 700)});
  ASSERT_EQ(runs.size(), 2u);
  EXPECT_EQ(runs[0].text, "\u00B4");
  EXPECT_EQ(runs[0].appear_ms, 0u);
  EXPECT_EQ(runs[1].text, "e");
  EXPECT_EQ(runs[1].appear_ms, 700u);
}

// A clear after a pause blacks out what earlier stages drew; what was drawn and
// cleared within one instant was never on screen and is left out.
TEST(NabtsCatalogueViewTest, TheVectorModeReplaysClears) {
  NabtsPageSnapshot snapshot;
  nabts_default_colour_map(snapshot.colour_map);
  snapshot.timeline.superseded = {letter('A', 0.1, 0), letter('B', 0.3, 900)};
  snapshot.timeline.clears = {NabtsDisplayClear{900, 2}};
  snapshot.primitives = {letter('C', 0.5, 900)};

  const CatalogueDisplayList list =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kTwiceVector);
  ASSERT_EQ(list.ops.size(), 3u);
  EXPECT_EQ(list.ops[0].text, "A");
  EXPECT_EQ(list.ops[1].kind, CatalogueDrawKind::kRectangle);
  EXPECT_TRUE(list.ops[1].filled);
  EXPECT_EQ(list.ops[1].appear_ms, 900u);
  EXPECT_EQ(list.ops[1].colour.red, 0);
  EXPECT_EQ(list.ops[2].text, "C");
}

// Both emissions carry the palette, and in the vector mode too a colour names
// its entry only where the entry changes as the page is shown.
TEST(NabtsCatalogueViewTest, TheVectorModeNamesTheEntriesThatChange) {
  constexpr NabtsColour kRed{0, 15, 0, false};
  NabtsPageSnapshot snapshot;
  nabts_default_colour_map(snapshot.timeline.initial_map);
  // Entry 3 is loaded with red a second in; the final map has it.
  nabts_default_colour_map(snapshot.colour_map);
  snapshot.colour_map[3] = kRed;
  NabtsPrimitive changing = letter('A', 0.1, 0);
  changing.colour = kRed;
  changing.colour_map_address = 3;
  NabtsPrimitive steady = letter('B', 0.5, 0);
  steady.colour = snapshot.colour_map[5];
  steady.colour_map_address = 5;
  snapshot.primitives = {changing, steady};
  NabtsPaletteEvent write;
  write.kind = NabtsPaletteEvent::Kind::kMapWrite;
  write.time_ms = 1000;
  write.address = 3;
  write.colour = kRed;
  snapshot.timeline.events.push_back(write);
  snapshot.timeline.end_ms = 1000;

  const CatalogueDisplayList list =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kTwiceVector);
  ASSERT_EQ(list.palette.size(), kNabtsColourMapEntries);
  ASSERT_EQ(list.palette_tracks.size(), kNabtsColourMapEntries);
  ASSERT_EQ(list.ops.size(), 2u);
  // The still colour is the final one, and the entry is named.
  EXPECT_EQ(list.ops[0].colour.red, 255);
  EXPECT_EQ(list.ops[0].colour.palette_index, 3);
  EXPECT_EQ(list.ops[1].colour.palette_index, -1);
  // Grey level 3 of the default map (6 of 15) until the write, red after it.
  const CataloguePaletteTrack& track = list.palette_tracks[3];
  EXPECT_EQ(track.colour_at(0, list.palette[3]).red, 6 * 255 / 15);
  EXPECT_EQ(track.colour_at(1000, list.palette[3]).red, 255);
  EXPECT_EQ(track.next_change_after(0), 1000u);
  EXPECT_EQ(track.next_change_after(1000), kCatalogueNever);
  EXPECT_TRUE(list.palette_tracks[5].steps.empty());
}

// §5.3.3.6.3: INCREMENTAL POINT arrives a row at a time, and each row is a
// colour run whose field is that row — its lower left corner, one pel high,
// the colours left to right however the pel ran.
TEST(NabtsCatalogueViewTest, AnIncrementalRowIsAColourRunOfOneRow) {
  NabtsPageSnapshot snapshot;
  nabts_default_colour_map(snapshot.colour_map);
  NabtsPrimitive row;
  row.kind = NabtsPrimitiveKind::kIncrementalPoints;
  row.origin = NabtsPoint{0.5, 0.25};
  row.logical_pel = NabtsSize{-0.125, 0.0625};  // right to left
  row.points = {NabtsPoint{0.0, 0.0}};
  row.size = NabtsSize{1.0, 1.0};
  row.incremental_colours = {kNabtsNominalWhite, kNabtsNominalBlack,
                             NabtsColour{0, 15, 0, false}};
  row.incremental_addresses = {-1, -1, -1};
  snapshot.primitives.push_back(row);

  const CatalogueDisplayList list =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kTwiceVector);
  ASSERT_EQ(list.ops.size(), 1u);
  const CatalogueDrawOp& op = list.ops.front();
  EXPECT_EQ(op.kind, CatalogueDrawKind::kColourRun);
  EXPECT_DOUBLE_EQ(op.origin.x, 0.5 - 3 * 0.125);
  EXPECT_DOUBLE_EQ(op.origin.y, 0.25);
  EXPECT_DOUBLE_EQ(op.size.dx, 3 * 0.125);
  EXPECT_DOUBLE_EQ(op.size.dy, 0.0625);
  ASSERT_EQ(op.colour_run.size(), 3u);
  // Laid right to left, so the last colour deposited is the leftmost.
  EXPECT_EQ(op.colour_run[0].red, 255);
  EXPECT_EQ(op.colour_run[0].green, 0);
  EXPECT_EQ(op.colour_run[2].green, 255) << "the first colour, rightmost";
}

// The list states the receiver it was resolved against, because X3.110 sizes
// stroke width, texture dots and incremental rasters in that receiver's pixels
// (§5.3.2.2.6) and a renderer given no resolution can only guess at them.
TEST(NabtsCatalogueViewTest, TheDisplayListCarriesTheGridItWasResolvedAgainst) {
  struct Case {
    NaplpsRenderMode mode;
    int width;
    int height;
  };
  // The vector mode is the reference receiver drawn as shapes, so it names that
  // receiver's grid (see naplps_render_grid_test.cpp).
  const std::vector<Case> cases = {{NaplpsRenderMode::kReference, 256, 200},
                                   {NaplpsRenderMode::kTwice, 512, 400},
                                   {NaplpsRenderMode::kThrice, 768, 600},
                                   {NaplpsRenderMode::kTwiceVector, 256, 200}};

  for (const Case& item : cases) {
    const CatalogueDisplayList list =
        nabts_page_display_list(pageWithOneLine(0.0), item.mode);
    EXPECT_EQ(list.nominal_width, item.width)
        << "mode " << naplps_render_mode_name(item.mode);
    EXPECT_EQ(list.nominal_height, item.height)
        << "mode " << naplps_render_mode_name(item.mode);
  }
}

// Table D1 item 10 makes unit y 0 to 0.78125 the visible extent, while §4.2.2
// puts that extent in a 4:3 display area. The two are different numbers and the
// list carries both, which is what lets a renderer draw a receiver's
// rectangular pixel as a rectangle instead of squaring the picture up.
TEST(NabtsCatalogueViewTest, TheVisibleExtentAndTheDisplayShapeAreBothStated) {
  const CatalogueDisplayList list =
      nabts_page_display_list(pageWithOneLine(0.0));
  EXPECT_NEAR(list.aspect_height, 0.78125, 1e-12);
  EXPECT_NEAR(list.display_aspect_height, 0.75, 1e-12);
  EXPECT_NE(list.aspect_height, list.display_aspect_height);
}

// The vector mode hands a renderer the geometry the record described, so the
// page's operations are the record's shapes and the pel stays in unit space:
// turning it into pixels there would bake one receiver into a list whose whole
// point is that it has not chosen one.
TEST(NabtsCatalogueViewTest, TheVectorModeEmitsTheRecordsOwnGeometry) {
  const NabtsPageSnapshot snapshot = pageWithOneLine(0.02);
  const CatalogueDisplayList list =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kTwiceVector);

  ASSERT_EQ(list.ops.size(), 1u);
  EXPECT_EQ(list.ops[0].kind, CatalogueDrawKind::kLine);
  ASSERT_EQ(list.ops[0].points.size(), 2u);
  EXPECT_NEAR(list.ops[0].points[0].x, 0.1, 1e-12);
  EXPECT_NEAR(list.ops[0].pen_size.dx, 0.02, 1e-12);
}

/// A scroll of the area at |origin| of |size| by |by|, in colour mode |mode|
/// with |background| for its colour mode 2 ground.
NabtsPrimitive scrollOf(NabtsPoint origin, NabtsSize size, NabtsPoint by,
                        NabtsColourMode mode = NabtsColourMode::kDirect,
                        NabtsColour background = kNabtsNominalBlack) {
  NabtsPrimitive scroll;
  scroll.kind = NabtsPrimitiveKind::kScroll;
  scroll.origin = origin;
  scroll.size = size;
  scroll.points = {by};
  scroll.colour_mode = mode;
  scroll.background = background;
  return scroll;
}

constexpr double kPixel = 1.0 / 256.0;

// §6.2.7.13: with SCROLL ON a line feed past the foot of the display area
// scrolls it up a line, "and the line scrolled into the display area ... is
// nominal black in color modes 0 and 1 and the background color in color mode
// 2". The vector list moves everything drawn before the scroll by the
// displacement, as the pixel modes move their pixels, and fills the stripe the
// display moved off: one pixel short on its far sides, which the outline a
// renderer traces round a filled figure (one pixel of the list's grid) covers.
TEST(NabtsCatalogueViewTest, AWholeDisplayScrollMovesWhatTheVectorListHolds) {
  NabtsPageSnapshot snapshot = pageWithOneLine(0.0);
  snapshot.primitives.push_back(
      scrollOf(NabtsPoint{0.0, 0.0}, NabtsSize{1.0, kNabtsDisplayAreaHeight},
               NabtsPoint{0.0, 10 * kPixel}));

  const CatalogueDisplayList list =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kTwiceVector);
  ASSERT_EQ(list.ops.size(), 2u);
  EXPECT_EQ(list.ops[0].kind, CatalogueDrawKind::kLine);
  ASSERT_EQ(list.ops[0].points.size(), 2u);
  EXPECT_NEAR(list.ops[0].points[0].y, 0.1 + 10 * kPixel, 1e-12);
  EXPECT_NEAR(list.ops[0].points[1].y, 0.6 + 10 * kPixel, 1e-12);
  EXPECT_NEAR(list.ops[0].origin.y, 0.1 + 10 * kPixel, 1e-12);
  EXPECT_NEAR(list.ops[0].points[0].x, 0.1, 1e-12) << "moved across as well";

  const CatalogueDrawOp& stripe = list.ops[1];
  EXPECT_EQ(stripe.kind, CatalogueDrawKind::kRectangle);
  EXPECT_TRUE(stripe.filled);
  EXPECT_NEAR(stripe.origin.x, 0.0, 1e-12);
  EXPECT_NEAR(stripe.origin.y, 0.0, 1e-12);
  EXPECT_NEAR(stripe.size.dx, 1.0 - kPixel, 1e-12);
  EXPECT_NEAR(stripe.size.dy, 9 * kPixel, 1e-12);
  EXPECT_EQ(stripe.colour, CatalogueColour{});
}

// Scrolled down, the stripe is at the top; in colour mode 2 it is the
// background colour.
TEST(NabtsCatalogueViewTest, AScrollDownFillsTheTopInTheBackgroundInModeTwo) {
  constexpr NabtsColour kBlue{0, 0, 15, false};
  NabtsPageSnapshot snapshot = pageWithOneLine(0.0);
  snapshot.primitives.push_back(
      scrollOf(NabtsPoint{0.0, 0.0}, NabtsSize{1.0, kNabtsDisplayAreaHeight},
               NabtsPoint{0.0, -20 * kPixel},
               NabtsColourMode::kMappedWithBackground, kBlue));

  const CatalogueDisplayList list =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kTwiceVector);
  ASSERT_EQ(list.ops.size(), 2u);
  EXPECT_NEAR(list.ops[0].points[0].y, 0.1 - 20 * kPixel, 1e-12);
  const CatalogueDrawOp& stripe = list.ops[1];
  EXPECT_NEAR(stripe.origin.y, kNabtsDisplayAreaHeight - 20 * kPixel, 1e-12);
  EXPECT_NEAR(stripe.size.dy, 19 * kPixel, 1e-12);
  EXPECT_EQ(stripe.colour.blue, 255);
  EXPECT_EQ(stripe.colour.red, 0);
}

// A list has no operation that moves part of the display, so a scroll of a
// smaller area — an INCREMENTAL POINT field inside the display area — leaves
// what was drawn where it was; the pixel modes scroll it.
TEST(NabtsCatalogueViewTest, AScrollOfPartOfTheDisplayLeavesTheListAsDrawn) {
  NabtsPageSnapshot snapshot = pageWithOneLine(0.0);
  snapshot.primitives.push_back(scrollOf(NabtsPoint{0.25, 0.25},
                                         NabtsSize{0.5, 0.25},
                                         NabtsPoint{0.0, 4 * kPixel}));

  const CatalogueDisplayList list =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kTwiceVector);
  ASSERT_EQ(list.ops.size(), 1u);
  EXPECT_NEAR(list.ops[0].points[0].y, 0.1, 1e-12);
}

// A pixel mode deposits the page into the receiver's frame buffer and emits
// what is on it, so the operations are runs of pixels rather than the shapes
// that put them there. They are rectangles because every renderer of a display
// list already draws those — pixel output needs no new operation kind.
TEST(NabtsCatalogueViewTest, APixelModeEmitsRunsOfPixels) {
  const NabtsPageSnapshot snapshot = pageWithOneLine(0.02);
  const CatalogueDisplayList list =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kReference);

  ASSERT_FALSE(list.ops.empty());
  for (const CatalogueDrawOp& op : list.ops) {
    EXPECT_EQ(op.kind, CatalogueDrawKind::kRectangle);
    EXPECT_TRUE(op.filled);
    // A pixel run has no pen: its extent is its own, not a stroke's.
    EXPECT_EQ(op.pen_size.dx, 0.0);
    EXPECT_EQ(op.pen_size.dy, 0.0);
    // Every run sits inside the unit screen it was rastered from.
    EXPECT_GE(op.origin.x, 0.0);
    EXPECT_GE(op.origin.y, 0.0);
    EXPECT_LE(op.origin.x + op.size.dx, 1.0 + 1e-9);
    EXPECT_LE(op.origin.y + op.size.dy, kNabtsDisplayAreaHeight + 1e-9);
  }
}

// A finer receiver resolves the same page into more, smaller pixels. That is
// the whole observable difference between the pixel modes, and it is what
// X3.110 §5.3.2.2.6's "at least one and possibly many display pixels" comes
// down to once a receiver is named.
TEST(NabtsCatalogueViewTest, AFinerReceiverEmitsSmallerPixels) {
  const NabtsPageSnapshot snapshot = pageWithOneLine(0.02);
  const CatalogueDisplayList coarse =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kReference);
  const CatalogueDisplayList fine =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kTwice);

  ASSERT_FALSE(coarse.ops.empty());
  ASSERT_FALSE(fine.ops.empty());
  // One row of the finer grid is half the height of one of the coarser.
  EXPECT_LT(fine.ops[0].size.dy, coarse.ops[0].size.dy);
  EXPECT_NEAR(fine.ops[0].size.dy, coarse.ops[0].size.dy / 2.0, 1e-9);
}

// The default is the receiver Table D1 requires, so a caller that names no mode
// gets the resolution a decoder of the period displayed.
TEST(NabtsCatalogueViewTest, TheDefaultResolutionIsTheReferenceReceiver) {
  const CatalogueDisplayList list =
      nabts_page_display_list(pageWithOneLine(0.0));
  EXPECT_EQ(list.nominal_width, 256);
  EXPECT_EQ(list.nominal_height, 200);
}

// ---------------------------------------------------------------------------
// Interpretation at catalogue-build time
// ---------------------------------------------------------------------------

/// A dataset holding one presentation record whose data draws |text| through a
/// DRCS definition, so the record exercises the resolution-dependent path.
NabtsAnalysisDataset datasetWithDrcsRecord() {
  NabtsAnalysisDataset data;
  NabtsCataloguedRecord record;
  record.channel = 0x000;
  record.address_text = "000";
  record.record_type = 1;  // §5.2.2.3 non-cyclic presentation
  record.times_seen = 1;
  record.times_intact = 1;
  record.complete = true;
  // ESC 4/3 (DEF DRCS), the character to define, then SO to invoke the PDI set
  // and a filled rectangle over the buffer, then ESC 4/5 (END).
  record.data = {0x1B, 0x43, 0x20, 0x0E, 0x6B, 0x7F, 0x1B, 0x45};
  data.records.push_back(record);
  return data;
}

// The receiver is chosen when the page is browsed, not when the recording is
// read, so changing it takes effect without reading the recording again. X3.110
// §6.2.3 sizes a DRCS storage buffer from the receiver's resolution, which is
// the one thing that used to be fixed by the recovery pass.
TEST(NabtsCatalogueViewTest, TheResolutionAppliesWithoutReRunningRecovery) {
  const NabtsAnalysisDataset data = datasetWithDrcsRecord();

  std::vector<NabtsCataloguedRecord> coarse = data.records;
  nabts_interpret_records(coarse, kNaplpsGridReference);
  std::vector<NabtsCataloguedRecord> fine = data.records;
  nabts_interpret_records(fine, kNaplpsGridTwice);

  ASSERT_EQ(coarse.size(), 1u);
  ASSERT_EQ(fine.size(), 1u);
  ASSERT_FALSE(coarse[0].page.drcs.empty())
      << "the fixture defined no downloadable character";
  ASSERT_FALSE(fine[0].page.drcs.empty());

  // The same definition, stored at each receiver's own resolution.
  EXPECT_GT(fine[0].page.drcs[0].width, coarse[0].page.drcs[0].width);
  EXPECT_GT(fine[0].page.drcs[0].height, coarse[0].page.drcs[0].height);
}

// What a page says is not what it looks like: the resolution decides how it is
// drawn, and the text a reader searches, the captions and the item table have
// to come out the same at every one of them.
TEST(NabtsCatalogueViewTest, TheReadingOfAPageIsTheSameAtEveryResolution) {
  const NabtsAnalysisDataset data = datasetWithDrcsRecord();

  const CatalogueDataset reference =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  for (const NaplpsRenderMode mode :
       {NaplpsRenderMode::kThrice, NaplpsRenderMode::kTwice,
        NaplpsRenderMode::kTwiceVector}) {
    const CatalogueDataset other = build_nabts_catalogue(data, mode);
    ASSERT_EQ(other.items.size(), reference.items.size())
        << "mode " << naplps_render_mode_name(mode);
    for (size_t i = 0; i < other.items.size(); ++i) {
      EXPECT_EQ(other.items[i].id, reference.items[i].id);
      EXPECT_EQ(other.items[i].values, reference.items[i].values);
      EXPECT_EQ(other.items[i].badges, reference.items[i].badges);
    }
    // The text a reader searches and the caption track are read off the page,
    // not off how it is drawn, so they cannot move with the resolution either.
    ASSERT_EQ(other.payloads.size(), reference.payloads.size());
    for (size_t i = 0; i < other.payloads.size(); ++i) {
      EXPECT_EQ(other.payloads[i].companion_text,
                reference.payloads[i].companion_text)
          << "the text a reader searches changed with the resolution";
      EXPECT_EQ(other.payloads[i].table.rows, reference.payloads[i].table.rows);
    }
  }
}

// The four receivers are offered to the reader through the schema, so the host
// can put them on a dropdown without knowing what a receiver is.
TEST(NabtsCatalogueViewTest, TheSchemaOffersTheReceiversAsViewOptions) {
  const CatalogueDataset catalogue =
      build_nabts_catalogue(datasetWithDrcsRecord(), NaplpsRenderMode::kThrice);

  EXPECT_FALSE(catalogue.schema.view_label.empty())
      << "an unlabelled set of options is not offered at all";
  ASSERT_EQ(catalogue.schema.view_options.size(), 4u);

  // Every option names a mode back, so what the host round-trips selects the
  // receiver the reader picked rather than falling back to another.
  for (const auto& option : catalogue.schema.view_options) {
    EXPECT_FALSE(option.label.empty());
    EXPECT_FALSE(option.tooltip.empty());
    EXPECT_EQ(naplps_render_mode_name(naplps_render_mode_from_name(
                  option.id, NaplpsRenderMode::kTwiceVector)),
              option.id)
        << "option '" << option.id << "' names no receiver";
  }

  // And the catalogue says which of them it was built under.
  EXPECT_EQ(catalogue.schema.view_option,
            naplps_render_mode_name(NaplpsRenderMode::kThrice));
}

// ---------------------------------------------------------------------------
// Versions of one address
// ---------------------------------------------------------------------------

/// A presentation record drawing one line.
NabtsAnalysisDataset datasetWithLineRecord() {
  NabtsAnalysisDataset data;
  NabtsCataloguedRecord record;
  record.channel = 0x000;
  record.address_text = "000";
  record.record_type = 1;  // §5.2.2.3 non-cyclic presentation
  record.times_seen = 1;
  record.times_intact = 1;
  record.complete = true;
  // SO invokes the PDI set, LINE ABS (2/8) takes one three-byte coordinate
  // word — the default multi-value length of §5.3.2.2.3 — and SI, the code
  // that ends the command, returns to text as a page would.
  const std::vector<uint8_t> plain = {0x0E, 0x28, 0x49, 0x42, 0x40, 0x0F};
  for (const uint8_t byte : plain) {
    record.data.push_back(teletext_odd_parity_encode(byte));
  }
  data.records.push_back(record);
  return data;
}

/// Two versions of presentation record 000/012 and one version of 000/013.
NabtsAnalysisDataset datasetWithTwoVersions() {
  NabtsAnalysisDataset data;
  for (const auto& [address, version, first, last] :
       std::vector<std::tuple<uint64_t, uint8_t, uint64_t, uint64_t>>{
           {0x012, 0, 10, 40}, {0x012, 2, 50, 90}, {0x013, 0, 10, 90}}) {
    NabtsCataloguedRecord record = datasetWithLineRecord().records.front();
    record.address = address;
    record.version = version;
    char text[16];
    std::snprintf(text, sizeof(text), "%03X", static_cast<unsigned>(address));
    record.address_text = text;
    record.channel_text = std::string("000/") + text;
    record.first_seen_frame = first;
    record.last_seen_frame = last;
    data.records.push_back(record);
  }
  return data;
}

// CEA-516 §5.2.1 makes the version part of a record's identity, so each
// version is a record of its own — and each says it is one of several, as
// does the page the address is listed as, so a revised page does not read as
// two unrelated records.
TEST(NabtsCatalogueViewTest, TheVersionsOfOneAddressAreMarkedInTheList) {
  const CatalogueDataset catalogue = build_nabts_catalogue(
      datasetWithTwoVersions(), NaplpsRenderMode::kReference);

  int marked = 0;
  for (const CatalogueItem& item : catalogue.items) {
    const bool revised = item.id.find("/012") != std::string::npos;
    const bool has_mark = std::find(item.badges.begin(), item.badges.end(),
                                    "2 versions") != item.badges.end();
    EXPECT_EQ(has_mark, revised) << item.id;
    if (revised) {
      ++marked;
      EXPECT_NE(item.tooltip.find("Address 000/012 was transmitted in 2 "
                                  "versions during the recording: v0 (frames "
                                  "11-41) and v2 (frames 51-91)"),
                std::string::npos)
          << item.tooltip;
    }
  }
  EXPECT_EQ(marked, 3);  // the address's page row and its two versions
}

// An address that carried several records outside any More chain — a
// carousel reusing its versions, a page revised as it was re-sent — is listed
// once, as a page whose records are its sub-pages, in the order they first
// arrived rather than in version order.
TEST(NabtsCatalogueViewTest, AnAddressWithSeveralRecordsIsOnePage) {
  NabtsAnalysisDataset data = datasetWithTwoVersions();
  // v2 arrived first; and a second page came under v0 later still.
  data.records[0].first_seen_frame = 60;
  data.records[1].first_seen_frame = 20;
  NabtsCataloguedRecord again = data.records[0];
  again.content = 1;
  again.first_seen_frame = 100;
  data.records.insert(data.records.begin() + 1, again);

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  std::vector<const CatalogueItem*> page;
  const CatalogueItem* row = nullptr;
  for (const CatalogueItem& item : catalogue.items) {
    if (item.id.find("/012") == std::string::npos) {
      continue;
    }
    if (item.parent_id.empty()) {
      row = &item;
    } else {
      page.push_back(&item);
    }
  }
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->values[0], "000/012");
  EXPECT_NE(std::find(row->badges.begin(), row->badges.end(), "3 pages"),
            row->badges.end());
  ASSERT_EQ(page.size(), 3u);
  for (const CatalogueItem* member : page) {
    EXPECT_EQ(member->parent_id, row->id);
  }
  EXPECT_EQ(page[0]->variant_label, "v2");
  EXPECT_EQ(page[1]->variant_label, "v0");
  EXPECT_EQ(page[2]->variant_label, "v0 (2)");
  // The address with one record stays a row of its own.
  for (const CatalogueItem& item : catalogue.items) {
    if (item.id.find("/013") != std::string::npos) {
      EXPECT_TRUE(item.parent_id.empty());
    }
  }
}

/// A page at 000/051 sent as version |version|, first arriving at frame
/// |first|: SI and then |text|, each byte with its odd parity, from a copy that
/// arrived intact.
NabtsCataloguedRecord carouselPage(uint8_t version, uint64_t first,
                                   const std::string& text) {
  NabtsCataloguedRecord record = datasetWithLineRecord().records.front();
  record.address = 0x05100;  // §5.2.5 long form of 051
  record.chain_base_address = record.address;
  record.address_text = "051";
  record.channel_text = "000/051";
  record.record_type = 0;
  record.version = version;
  record.first_seen_frame = first;
  record.last_seen_frame = first + 600;
  record.records_in_message = 1;
  record.data = {teletext_odd_parity_encode(0x0F)};
  for (const char character : text) {
    record.data.push_back(
        teletext_odd_parity_encode(static_cast<uint8_t>(character)));
  }
  return record;
}

/// Two pages of a carousel, long enough to judge.
constexpr char kFirstItem[] =
    "MEDIA WATCH: The networks' fall season opened to the lowest ratings in "
    "a decade, and the first cancellations are expected within the week.";
constexpr char kSecondItem[] =
    "MEDIA WATCH: Cable systems added two million homes this year, and the "
    "largest of them now reaches more viewers than some network affiliates.";

/// The sub-pages of 000/051, with the page row they hang from.
std::pair<const CatalogueItem*, std::vector<const CatalogueItem*>> carouselRows(
    const CatalogueDataset& catalogue) {
  const CatalogueItem* row = nullptr;
  std::vector<const CatalogueItem*> pages;
  for (const CatalogueItem& item : catalogue.items) {
    if (item.id.find("/051") == std::string::npos) {
      continue;
    }
    if (item.parent_id.empty()) {
      row = &item;
    } else {
      pages.push_back(&item);
    }
  }
  return {row, pages};
}

// A service can give a carousel's pages the next version at every pass, even
// where it has fewer pages than versions (CEA-516 §5.2.7.2 Y16), so one page
// came round under several versions. A page sent again is one sub-page,
// labelled with every version it was sent under, in the order its pages first
// arrived.
TEST(NabtsCatalogueViewTest, ACarouselPageSentAgainIsOneSubPage) {
  NabtsAnalysisDataset data;
  data.records = {
      carouselPage(0, 10, kFirstItem), carouselPage(1, 20, kSecondItem),
      carouselPage(2, 30, kFirstItem), carouselPage(3, 40, kSecondItem)};

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  const auto [row, pages] = carouselRows(catalogue);
  ASSERT_NE(row, nullptr);
  EXPECT_NE(std::find(row->badges.begin(), row->badges.end(), "4 versions"),
            row->badges.end());
  EXPECT_NE(std::find(row->badges.begin(), row->badges.end(), "2 pages"),
            row->badges.end());
  ASSERT_EQ(pages.size(), 2u);
  EXPECT_EQ(pages[0]->variant_label, "v0, v2");
  EXPECT_EQ(pages[1]->variant_label, "v1, v3");
  EXPECT_NE(row->tooltip.find("each a sub-page labelled with every version"),
            std::string::npos)
      << row->tooltip;
}

// A page revised as it was re-sent is a page of its own, however little
// changed: the next version of a standings table moves on by a game. A copy of
// the page that failed a byte's parity, or lost a stretch the recording never
// delivered, is that page sent again.
TEST(NabtsCatalogueViewTest, ARevisedPageIsNotThePageSentAgain) {
  std::string standings;
  for (int line = 0; line < 5; ++line) {
    standings +=
        "Montreal.....2-0 (6)  New York.....2 (4)  Boston......1-2 (5)  ";
  }
  // Byte 0 of a record is its SI, so a character of the text is a byte on.
  const size_t won = 1 + standings.find("(6)") + 1;
  const size_t played = 1 + standings.find("2 (4)");
  NabtsCataloguedRecord revised = carouselPage(1, 20, standings);
  revised.data[won] = teletext_odd_parity_encode('7');
  revised.data[played] = teletext_odd_parity_encode('3');
  // A vote of damaged copies: a byte failing its parity, and a lost stretch.
  NabtsCataloguedRecord damaged = carouselPage(2, 30, standings);
  damaged.times_intact = 0;
  damaged.data_present.assign(damaged.data.size(), 1);
  damaged.data[won] ^= 0x80;
  for (size_t i = 100; i < 128; ++i) {
    damaged.data[i] = 0x00;
    damaged.data_present[i] = 0;
  }

  NabtsAnalysisDataset data;
  data.records = {carouselPage(0, 10, standings), revised, damaged};
  const auto [row, pages] =
      carouselRows(build_nabts_catalogue(data, NaplpsRenderMode::kReference));
  ASSERT_EQ(pages.size(), 2u);
  EXPECT_EQ(pages[0]->variant_label, "v0, v2");
  EXPECT_EQ(pages[1]->variant_label, "v1");
}

// A record that arrived intact is as long as its page, so a longer record that
// begins with it — a quiz page whose next version adds the answer — is a page
// of its own, though every byte the two share agrees.
TEST(NabtsCatalogueViewTest, APageThatGrewIsNotThePageItGrewFrom) {
  const std::string question = std::string(kFirstItem) + " Who said it?";
  NabtsAnalysisDataset data;
  data.records = {carouselPage(0, 10, question),
                  carouselPage(1, 20, question + " Answer: the critic.")};
  const auto [row, pages] =
      carouselRows(build_nabts_catalogue(data, NaplpsRenderMode::kReference));
  ASSERT_EQ(pages.size(), 2u);
  EXPECT_EQ(pages[0]->variant_label, "v0");
  EXPECT_EQ(pages[1]->variant_label, "v1");
}

// A page is judged by the bytes both records received, and a record that lost
// most of its own could be any page of its carousel: it stays a sub-page of
// its own. Where the same page is sent again, the record with the most intact
// copies is the one drawn.
TEST(NabtsCatalogueViewTest, ARecordMostlyLostIsNotTakenForAnother) {
  NabtsCataloguedRecord damaged = carouselPage(0, 10, kFirstItem);
  damaged.times_intact = 0;
  damaged.data_present.assign(damaged.data.size(), 1);
  NabtsCataloguedRecord lost = carouselPage(1, 20, kFirstItem);
  lost.times_intact = 0;
  lost.data_present.assign(lost.data.size(), 0);
  for (size_t i = 0; i < lost.data.size() / 3; ++i) {
    lost.data_present[i] = 1;
  }
  NabtsAnalysisDataset data;
  data.records = {damaged, lost, carouselPage(2, 30, kFirstItem)};

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  const auto [row, pages] = carouselRows(catalogue);
  ASSERT_EQ(pages.size(), 2u);
  EXPECT_EQ(pages[0]->variant_label, "v0, v2");
  EXPECT_EQ(pages[0]->id, "000/051 v2") << "the intact copy is drawn";
  EXPECT_EQ(pages[1]->variant_label, "v1");
}

// A record kept although its address never arrived undamaged says so, and
// only that one: the rest were named as transmitted.
TEST(NabtsCatalogueViewTest, ARecordKeptWithoutAnAttestedAddressIsMarked) {
  NabtsAnalysisDataset data = datasetWithTwoVersions();
  for (NabtsCataloguedRecord& record : data.records) {
    record.times_attested = record.address == 0x013 ? 0 : record.times_seen;
  }
  data.summary.identity_reconciliation.identities_kept = 1;

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  for (const CatalogueItem& item : catalogue.items) {
    const bool kept = item.id.find("/013") != std::string::npos;
    const bool marked = std::find(item.badges.begin(), item.badges.end(),
                                  "unconfirmed") != item.badges.end();
    EXPECT_EQ(marked, kept) << item.id;
    EXPECT_EQ(item.tooltip.find("never arrived undamaged") != std::string::npos,
              kept)
        << item.id;
  }
}

// A recording none of whose addresses arrived undamaged gives the
// reconciliation nothing to judge by (vbi_identity_reconciliation_applies()),
// so its records are listed as recovered: each of them is marked unconfirmed,
// with that reason rather than the recurring record's, and the headline says
// why.
TEST(NabtsCatalogueViewTest, WithNoAddressAttestedEveryRecordIsMarked) {
  NabtsAnalysisDataset data = datasetWithTwoVersions();
  for (NabtsCataloguedRecord& record : data.records) {
    record.times_attested = 0;
  }
  data.summary.frames_analysed = 90;
  data.summary.identity_reconciliation.withheld = true;

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  int marked = 0;
  for (const CatalogueItem& item : catalogue.items) {
    if (std::find(item.badges.begin(), item.badges.end(), "unconfirmed") ==
        item.badges.end()) {
      continue;
    }
    ++marked;
    EXPECT_NE(item.tooltip.find("nor did any other address"), std::string::npos)
        << item.tooltip;
  }
  EXPECT_GE(marked, 3) << "each of the three records";
  EXPECT_NE(catalogue.summary.headline.find(
                "every record is listed marked unconfirmed"),
            std::string::npos)
      << catalogue.summary.headline;
}

// Records removed as another page's packets misaddressed to a channel the
// service never used are counted where the list is, as the folded ones are.
TEST(NabtsCatalogueViewTest, TheHeadlineCountsRecordsRemovedAsMisaddressed) {
  NabtsAnalysisDataset data = datasetWithTwoVersions();
  data.summary.frames_analysed = 90;
  data.summary.identity_reconciliation.identities_misaddressed = 2;
  std::string headline =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference)
          .summary.headline;
  EXPECT_NE(headline.find("2 records on data channels the service never "
                          "used were pages' packets misaddressed there"),
            std::string::npos)
      << headline;

  data.summary.identity_reconciliation.identities_misaddressed = 1;
  headline = build_nabts_catalogue(data, NaplpsRenderMode::kReference)
                 .summary.headline;
  EXPECT_NE(headline.find("one record on a data channel the service never "
                          "used"),
            std::string::npos)
      << headline;
}

// Lines that carry another signal behind NABTS's run-in and framing code
// (nabts_lines_carry_no_nabts()) list nothing, and say why rather than leave
// an empty list that reads as a failed recovery.
TEST(NabtsCatalogueViewTest, LinesThatCarryNoNabtsSaySo) {
  NabtsAnalysisDataset data;
  data.summary.frames_analysed = 90;
  data.summary.packets_recovered = 4377;
  data.summary.packets_prefix_whole = 2;
  data.summary.not_nabts = true;

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  EXPECT_TRUE(catalogue.items.empty());
  const std::string& headline = catalogue.summary.headline;
  EXPECT_EQ(headline.rfind("No NABTS in 90 frames", 0), 0u) << headline;
  EXPECT_NE(headline.find("of the 4377 packets read, a whole prefix arrived "
                          "on only 2"),
            std::string::npos)
      << headline;
}

/// A presentation record at short address |address| sent as |count| NULs,
/// each with the odd parity §3.3 gives a type-zero group's data bytes.
NabtsCataloguedRecord emptyRecord(uint16_t address, uint8_t type,
                                  size_t count) {
  char text[8];
  std::snprintf(text, sizeof(text), "%03X", address);
  NabtsCataloguedRecord record;
  record.channel = 0x000;
  record.address = static_cast<uint64_t>(address) << 8;  // §5.2.5 long form
  record.chain_base_address = record.address;
  record.address_text = text;
  record.channel_text = std::string("000/") + text;
  record.record_type = type;
  record.times_seen = 4;
  record.times_intact = 4;
  record.records_in_message = 1;
  record.complete = true;
  record.data.assign(count, 0x80);
  return record;
}

/// The item and payload for the record whose identity contains |address|.
std::pair<const CatalogueItem*, const CataloguePayload*> find(
    const CatalogueDataset& catalogue, const std::string& address) {
  for (size_t i = 0; i < catalogue.items.size(); ++i) {
    if (catalogue.items[i].id.find(address) != std::string::npos) {
      return {&catalogue.items[i], &catalogue.payloads[i]};
    }
  }
  return {nullptr, nullptr};
}

bool hasBadge(const CatalogueItem& item, const std::string& badge) {
  return std::find(item.badges.begin(), item.badges.end(), badge) !=
         item.badges.end();
}

// One identity can carry more than one content over a recording — a
// carousel's versions come round again with new pages — and each is a record of
// its own. The second is numbered after the identity, and its id reduces to the
// name its record file is exported under.
TEST(NabtsCatalogueViewTest, ASecondContentUnderOneIdentityIsNumbered) {
  NabtsAnalysisDataset data;
  auto first = emptyRecord(0x100, 0, 11);
  auto second = emptyRecord(0x100, 0, 14);
  second.content = 1;
  data.records = {first, second};

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  // The address's page row, then its two pages as sub-pages.
  ASSERT_EQ(catalogue.items.size(), 3u);
  EXPECT_EQ(catalogue.items[1].id, "000/100 v0");
  EXPECT_EQ(catalogue.items[2].id, "000/100 v0 (2)");
  EXPECT_EQ(catalogue.items[2].variant_label, "v0 (2)");
  EXPECT_EQ(catalogue.items[2].parent_id, catalogue.items[0].id);
  EXPECT_EQ(nabts_record_key(second), "000-100-v0-2");
}

// A record sent as nothing but NULs draws nothing (X3.110 §6.1.4), and a
// service can send such records for what their headers say: the cyclic marker
// F00 heading every cycle (CEA-516 §5.2.7.7), an empty Support Record FFF
// (§5.2.7.12), and records 015 to 019 that hand the receiver straight on to
// 020 (§5.2.7.8). Each is named for its role instead of drawn as a blank page,
// and stays in the list.
TEST(NabtsCatalogueViewTest, ARecordSentEmptyIsNamedForItsRole) {
  NabtsAnalysisDataset data;
  auto marker = emptyRecord(0xF00, 3, 11);
  marker.cyclic_marker = true;
  auto support = emptyRecord(0xFFF, 0, 11);
  support.support_record = true;
  // Auto Acquire, More Record named in the header (§5.2.8.4).
  auto named = emptyRecord(0x015, 0, 6);
  named.auto_acquire = true;
  named.more_redefined = true;
  named.has_more_address = true;
  named.more_channel = 0x000;
  named.more_address = 0x02000;
  // Auto Acquire, More Record by the More Flag (§5.2.7.6): 000001601.
  auto flagged = emptyRecord(0x016, 0, 6);
  flagged.auto_acquire = true;
  flagged.more = true;
  const auto bare = emptyRecord(0x911, 0, 6);
  data.records = {named, flagged, bare, marker, support};

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  const std::vector<std::tuple<std::string, std::string, std::string>> expected{
      {"/F00", "Cyclic Marker", "§5.2.7.7"},
      {"/FFF", "Support Record", "§5.2.7.12"},
      {"/015", "Auto Acquire → 020", "§5.2.7.8"},
      {"/016", "Auto Acquire → 000001601", "§5.2.7.8"},
      {"/911", "Empty record", "§6.1.4"},
  };
  for (const auto& [address, badge, clause] : expected) {
    const auto [item, payload] = find(catalogue, address);
    ASSERT_NE(item, nullptr) << address << " left the list";
    EXPECT_TRUE(hasBadge(*item, badge)) << address;
    EXPECT_EQ(payload->kind, CataloguePayload::Kind::kText)
        << address << " is still drawn as a blank page";
    EXPECT_NE(payload->document.text.find(clause), std::string::npos)
        << payload->document.text;
  }
}

// A record whose classification sequence has two pointer bytes is one a Full
// Service Standard receiver ignores entirely (CEA-516 §8.5.2.7.1) — both
// emulated receivers refuse such pages — and the
// reserved flag bytes the service set are its own. The page is still drawn,
// and its information says both.
TEST(NabtsCatalogueViewTest, ARecordOutsideTheFullServiceStandardSaysSo) {
  NabtsAnalysisDataset data;
  auto nbc = emptyRecord(0x001, 0, 0);
  nbc.data = {0x41, 0x42};  // two characters: a page, not an empty record
  nbc.classification_pointers = 2;
  nbc.reserved_flags = {"Y11 = 4", "Y22 = 1"};
  auto fss = emptyRecord(0x002, 0, 0);
  fss.data = {0x41, 0x42};
  fss.classification_pointers = 1;
  data.records = {nbc, fss};

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  const auto [item, payload] = find(catalogue, "/001");
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(payload->kind, CataloguePayload::Kind::kDisplayList);
  EXPECT_NE(payload->condition.find("outside the Full Service Standard (2 "
                                    "classification pointer bytes)"),
            std::string::npos)
      << payload->condition;
  EXPECT_NE(payload->condition.find("reserved flags Y11 = 4, Y22 = 1"),
            std::string::npos)
      << payload->condition;
  EXPECT_NE(item->tooltip.find("§8.5.2.7.1"), std::string::npos);
  EXPECT_NE(item->tooltip.find("Y11 = 4, Y22 = 1"), std::string::npos);

  const auto [fss_item, fss_payload] = find(catalogue, "/002");
  ASSERT_NE(fss_item, nullptr);
  EXPECT_EQ(fss_payload->condition.find("Full Service Standard"),
            std::string::npos);
  EXPECT_EQ(fss_item->tooltip.find("§8.5.2.7.1"), std::string::npos);
}

// NULs standing in for bytes no copy delivered are a lost page, not an empty
// one; nor is a record with a link missing. Both are drawn as recovered.
TEST(NabtsCatalogueViewTest, ARecordThatLostItsBytesIsNotNamedEmpty) {
  NabtsAnalysisDataset data;
  auto lost = emptyRecord(0x020, 0, 11);
  lost.cyclic_marker = true;
  lost.data.assign(11, 0x00);
  lost.data_present.assign(11, 0);
  auto short_series = emptyRecord(0x021, 0, 11);
  short_series.complete = false;
  data.records = {lost, short_series};

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  for (const std::string address : {"/020", "/021"}) {
    const auto [item, payload] = find(catalogue, address);
    ASSERT_NE(item, nullptr) << address;
    EXPECT_EQ(payload->kind, CataloguePayload::Kind::kDisplayList) << address;
    EXPECT_FALSE(hasBadge(*item, "Cyclic Marker")) << address;
    EXPECT_FALSE(hasBadge(*item, "Empty record")) << address;
  }
}

/// A presentation record at short address |address| carrying |bytes| as sent,
/// each with its odd parity, from a copy that arrived intact.
NabtsCataloguedRecord sentRecord(uint16_t address,
                                 const std::vector<uint8_t>& bytes) {
  NabtsCataloguedRecord record = emptyRecord(address, 0, 0);
  for (const uint8_t byte : bytes) {
    record.data.push_back(teletext_odd_parity_encode(byte));
  }
  return record;
}

std::vector<uint8_t> bytesOf(const std::string& text) {
  return {text.begin(), text.end()};
}

// A service can send, every ten seconds and at address EF0, a record whose
// code only resets the decoder: NSR with the cursor at row 0 column 0,
// twice, SO twice, RESET with no operand bytes — which X3.110 §5.3.2.9.3 reads
// as resetting nothing — SI twice and twenty NULs. It draws nothing, and is
// named for that rather than drawn as a blank page. A RESET that clears the
// screen, or a character after the resets, is a page and is drawn.
TEST(NabtsCatalogueViewTest, ARecordThatOnlyResetsTheDecoderIsNamedSo) {
  std::vector<uint8_t> resets{0x1F, 0x40, 0x40, 0x1F, 0x40, 0x40,
                              0x0E, 0x0E, 0x20, 0x0F, 0x0F};
  resets.insert(resets.end(), 20, 0x00);
  // RESET with operand 4/1: the domain to its defaults, harmless in itself,
  // but asking for something; 5/0: clear the display to black (Table 15).
  const std::vector<uint8_t> clears{0x1F, 0x40, 0x40, 0x0E,
                                    0x20, 0x50, 0x40, 0x0F};
  const std::vector<uint8_t> writes{0x1F, 0x40, 0x40, 0x41};
  NabtsAnalysisDataset data;
  data.records = {sentRecord(0xEF0, resets), sentRecord(0xEF1, clears),
                  sentRecord(0xEF2, writes)};

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  const auto [item, payload] = find(catalogue, "/EF0");
  ASSERT_NE(item, nullptr) << "the record left the list";
  EXPECT_TRUE(hasBadge(*item, "Reset only"));
  EXPECT_EQ(payload->kind, CataloguePayload::Kind::kText);
  EXPECT_NE(payload->document.text.find("§5.3.2.9.3"), std::string::npos)
      << payload->document.text;
  EXPECT_NE(payload->document.text.find("1F 40 40 1F 40 40 0E 0E 20 0F 0F"),
            std::string::npos)
      << payload->document.text;
  for (const std::string address : {"/EF1", "/EF2"}) {
    const auto [page, page_payload] = find(catalogue, address);
    ASSERT_NE(page, nullptr) << address;
    EXPECT_FALSE(hasBadge(*page, "Reset only")) << address;
    EXPECT_EQ(page_payload->kind, CataloguePayload::Kind::kDisplayList)
        << address;
  }
}

// A service can carry other services' data under presentation record types,
// such as a Dow Jones test page or a news wire in plain ASCII. Read as
// NAPLPS it is a page of stray characters, overprinted where it runs past the
// foot of the screen, so it is listed as the text it is, with a note of why.
TEST(NabtsCatalogueViewTest, PlainTextSentAsAPageIsListedAsText) {
  NabtsAnalysisDataset data;
  data.records = {sentRecord(
      0x020, bytesOf("TEST PAGE 000001\r\n\r\nThis is a test news item from\r\n"
                     "Dow Jones and Company, Inc.\r\n\r\nThis is a test of the "
                     "vertical\r\nblanking interval as a vehicle\r\n"))};

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  const auto [item, payload] = find(catalogue, "/020");
  ASSERT_NE(item, nullptr);
  EXPECT_TRUE(hasBadge(*item, "Not NAPLPS"));
  EXPECT_EQ(payload->kind, CataloguePayload::Kind::kText);
  EXPECT_TRUE(payload->document.monospace);
  const std::string& text = payload->document.text;
  EXPECT_NE(text.find("not NAPLPS"), std::string::npos) << text;
  EXPECT_NE(text.find("plain text"), std::string::npos) << text;
  EXPECT_NE(text.find("This is a test news item from\nDow Jones"),
            std::string::npos)
      << text;
}

// Binary data — such as a block at 000400000 — carries
// transmission and device controls as often as random bytes do, which X3.110
// §6.1.4-6.1.5 gives no effect on a page: it is listed in hexadecimal.
TEST(NabtsCatalogueViewTest, BinaryDataSentAsAPageIsListedInHexadecimal) {
  // Two transmission controls (STX, DC1) in every sixteen bytes.
  const std::vector<uint8_t> block = {0x02, 0x41, 0x5C, 0x7E, 0x11, 0x23,
                                      0x0B, 0x47, 0x31, 0x6D, 0x1C, 0x35,
                                      0x48, 0x2A, 0x7A, 0x0E};
  std::vector<uint8_t> bytes;
  for (int i = 0; i < 8; ++i) {
    bytes.insert(bytes.end(), block.begin(), block.end());
  }
  NabtsAnalysisDataset data;
  data.records = {sentRecord(0x021, bytes)};

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  const auto [item, payload] = find(catalogue, "/021");
  ASSERT_NE(item, nullptr);
  EXPECT_TRUE(hasBadge(*item, "Not NAPLPS"));
  EXPECT_EQ(payload->kind, CataloguePayload::Kind::kText);
  const std::string& text = payload->document.text;
  EXPECT_NE(text.find("binary data"), std::string::npos) << text;
  EXPECT_NE(
      text.find("000000  02 41 5C 7E 11 23 0B 47 31 6D 1C 35 48 2A 7A 0E"),
      std::string::npos)
      << text;
}

// Text in CR LF lines framed by link controls — a DLE or DC1 and a character
// every line, with a stray SO among them — is listed as text too.
TEST(NabtsCatalogueViewTest, FramedTextSentAsAPageIsListedAsText) {
  std::string message;
  for (int line = 0; line < 16; ++line) {
    message += std::string(line % 2 == 0 ? "\x10V" : "\x11^") +
               "news and quotation information\r\n";
  }
  message += "\x0EQ";
  NabtsAnalysisDataset data;
  data.records = {sentRecord(0x022, bytesOf(message))};

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  const auto [item, payload] = find(catalogue, "/022");
  ASSERT_NE(item, nullptr);
  EXPECT_TRUE(hasBadge(*item, "Not NAPLPS"));
  EXPECT_NE(payload->document.text.find("16 CR LF lines"), std::string::npos)
      << payload->document.text;
  EXPECT_NE(payload->document.text.find("<10>Vnews and quotation"),
            std::string::npos)
      << payload->document.text;
}

/// A Dow Jones test frame at 000200000, in |framing| (1 to 8), laid out as
/// every copy received was: five opening
/// bytes and the frame's length, then blocks of 27 bytes, the first opening
/// with the framing number five times and the block count in five digits,
/// each a Hamming 8/4 byte, then |text|, NULs to whole blocks, the checks the
/// framing adds, and an end-of-file code and fill. Seven-bit codes, for
/// sentRecord() to give their parity; a Hamming 8/4 codeword has odd parity
/// already, so it comes back as itself.
std::vector<uint8_t> dowJonesFrame(int framing, const std::string& text) {
  struct Layout {
    std::vector<size_t> runs;  // characters, each run followed by its check
    bool run_checks;
    size_t data_blocks;   // before each set of check blocks
    size_t check_blocks;  // 1: exclusive OR of each column; 2: its sum
  };
  const std::vector<Layout> layouts = {
      {{27}, false, 0, 0}, {{25}, true, 0, 0},         {{27}, false, 3, 1},
      {{27}, false, 5, 2}, {{25}, true, 5, 2},         {{25}, true, 8, 2},
      {{25}, true, 13, 2}, {{5, 5, 5, 4}, true, 5, 2},
  };
  const Layout& layout = layouts[static_cast<size_t>(framing - 1)];
  const auto hamming = [](size_t digit) {
    return static_cast<uint8_t>(
        teletext_hamming84_encode(static_cast<uint8_t>(digit)) & 0x7F);
  };
  // A check byte: the low seven bits of |value|, 1 sent as 7/15.
  const auto check = [](uint32_t value) {
    const auto seven = static_cast<uint8_t>(value & 0x7F);
    return seven == 1 ? uint8_t{0x7F} : seven;
  };
  size_t per_block = 0;
  for (const size_t run : layout.runs) {
    per_block += run;
  }
  size_t data_blocks = (10 + text.size() + per_block - 1) / per_block;
  if (layout.data_blocks > 0) {
    data_blocks = (data_blocks + layout.data_blocks - 1) / layout.data_blocks *
                  layout.data_blocks;
  }
  const size_t blocks =
      data_blocks + (layout.data_blocks > 0 ? data_blocks / layout.data_blocks *
                                                  layout.check_blocks
                                            : 0);
  std::vector<uint8_t> characters(5, hamming(static_cast<size_t>(framing)));
  for (size_t place = 10000; place > 0; place /= 10) {
    characters.push_back(hamming(blocks / place % 10));
  }
  characters.insert(characters.end(), text.begin(), text.end());
  characters.resize(data_blocks * per_block, 0x00);

  const auto laid_out = [&](const std::vector<uint8_t>& block) {
    std::vector<uint8_t> out;
    size_t at = 0;
    for (const size_t run : layout.runs) {
      uint32_t sum = 0;
      for (size_t c = 0; c < run; ++c) {
        out.push_back(block[at + c]);
        sum += block[at + c];
      }
      at += run;
      if (layout.run_checks) {
        out.push_back(check(sum >> 7));
        out.push_back(check(sum));
      }
    }
    return out;
  };
  const size_t length = blocks * 27;
  std::vector<uint8_t> bytes = {0x7D,
                                0x21,
                                0x14,
                                0x00,
                                0x40,
                                static_cast<uint8_t>(length & 0x7F),
                                static_cast<uint8_t>(length >> 8)};
  std::vector<std::vector<uint8_t>> group;
  for (size_t b = 0; b < data_blocks; ++b) {
    const std::vector<uint8_t> block(
        characters.begin() + static_cast<std::ptrdiff_t>(b * per_block),
        characters.begin() + static_cast<std::ptrdiff_t>((b + 1) * per_block));
    const std::vector<uint8_t> sent = laid_out(block);
    bytes.insert(bytes.end(), sent.begin(), sent.end());
    group.push_back(block);
    if (group.size() != layout.data_blocks) {
      continue;
    }
    std::vector<uint8_t> high(per_block);
    std::vector<uint8_t> low(per_block);
    std::vector<uint8_t> exclusive(per_block);
    for (size_t c = 0; c < per_block; ++c) {
      uint32_t sum = 0;
      uint32_t either = 0;
      for (const std::vector<uint8_t>& member : group) {
        sum += member[c];
        either ^= member[c];
      }
      high[c] = check(sum >> 7);
      low[c] = check(sum);
      exclusive[c] = check(either);
    }
    for (const std::vector<uint8_t>& checks :
         layout.check_blocks == 1
             ? std::vector<std::vector<uint8_t>>{exclusive}
             : std::vector<std::vector<uint8_t>>{high, low}) {
      const std::vector<uint8_t> sent_checks = laid_out(checks);
      bytes.insert(bytes.end(), sent_checks.begin(), sent_checks.end());
    }
    group.clear();
  }
  bytes.push_back(0x1A);
  bytes.insert(bytes.end(), 54, 'A');
  bytes.insert(bytes.end(), 24, 0x00);
  return bytes;
}

const std::string kDowJonesText =
    "TEST PAGE 000001\r\n\r\nThis is a test news item from\r\nDow Jones and "
    "Company, Inc.\r\n\r\nThis is a test of the vertical\r\nblanking interval "
    "as a vehicle\r\nfor delivery of news and quotation\r\ninformation.\r\n\r\n"
    "If this test shows that the VBI\r\ndelivery method can provide an\r\n"
    "accurate and timely pathway\r\n";

// Dow Jones's test page came in eight framings, and the check bytes each adds
// to the text — sums of its codes, small numbers that read as STX, ETX and EOT
// among it — are what made it look like a wire-service message in link
// controls. Each framing is taken apart by its own header into the text it
// carries, its checks accounted for.
TEST(NabtsCatalogueViewTest, ADowJonesTestFrameIsListedAsTheTextItCarries) {
  std::string expected = kDowJonesText;
  for (size_t at = expected.find("\r\n"); at != std::string::npos;
       at = expected.find("\r\n", at)) {
    expected.replace(at, 2, "\n");
  }
  for (int framing = 1; framing <= 8; ++framing) {
    NabtsAnalysisDataset data;
    data.records = {sentRecord(0x020, dowJonesFrame(framing, kDowJonesText))};

    const CatalogueDataset catalogue =
        build_nabts_catalogue(data, NaplpsRenderMode::kReference);
    const auto [item, payload] = find(catalogue, "/020");
    ASSERT_NE(item, nullptr) << framing;
    EXPECT_TRUE(hasBadge(*item, "Not NAPLPS")) << framing;
    EXPECT_EQ(payload->kind, CataloguePayload::Kind::kText) << framing;
    const std::string& listing = payload->document.text;
    EXPECT_NE(listing.find("Dow Jones"), std::string::npos) << listing;
    EXPECT_NE(listing.find("framing " + std::to_string(framing) + " of eight"),
              std::string::npos)
        << listing;
    EXPECT_NE(listing.find(framing == 1 ? "no checks at all" : " of them hold"),
              std::string::npos)
        << listing;
    const size_t body = listing.find("\n\n");
    ASSERT_NE(body, std::string::npos) << listing;
    EXPECT_EQ(listing.substr(body + 2), expected) << framing;
  }
}

// A character misread into another that passes its parity is caught by the
// checks that cover it, and the note says how many held; a block the
// recording lost is a stretch of the text no copy delivered.
TEST(NabtsCatalogueViewTest, ADowJonesFrameCountsTheChecksThatHold) {
  std::vector<uint8_t> bytes = dowJonesFrame(5, kDowJonesText);
  // "This is a test news item" — the 'n' of "news", in the frame's second
  // block (its first row check and its first group's column check).
  const size_t news = std::string(bytes.begin(), bytes.end()).find("news");
  ASSERT_NE(news, std::string::npos);
  bytes[news] = 'o';
  NabtsCataloguedRecord damaged = sentRecord(0x021, bytes);
  NabtsCataloguedRecord lost =
      sentRecord(0x022, dowJonesFrame(5, kDowJonesText));
  lost.data_present.assign(lost.data.size(), 1);
  const size_t lost_at = 7 + 27 * 3;  // the fourth block, whole
  for (size_t i = lost_at; i < lost_at + 27; ++i) {
    lost.data[i] = 0x00;
    lost.data_present[i] = 0;
  }
  NabtsAnalysisDataset data;
  data.records = {damaged, lost};

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  const auto [damaged_item, damaged_payload] = find(catalogue, "/021");
  ASSERT_NE(damaged_item, nullptr);
  const std::string& listing = damaged_payload->document.text;
  EXPECT_NE(listing.find("whose bytes arrived hold"), std::string::npos)
      << listing;
  EXPECT_NE(listing.find("This is a test oews item"), std::string::npos)
      << listing;

  const auto [lost_item, lost_payload] = find(catalogue, "/022");
  ASSERT_NE(lost_item, nullptr);
  EXPECT_TRUE(hasBadge(*lost_item, "Not NAPLPS"));
  EXPECT_NE(lost_payload->document.text.find("of them hold"), std::string::npos)
      << lost_payload->document.text;
  EXPECT_NE(lost_payload->document.text.find("[25 bytes lost]"),
            std::string::npos)
      << lost_payload->document.text;
}

// A record that arrived whole and draws nothing at all is named for its role
// as a record of NULs is: an Auto Acquire record can open with a shift, a
// colour mode and text attributes, and hand the receiver straight on to
// another page.
TEST(NabtsCatalogueViewTest, ARecordThatDrawsNothingIsNamedForItsRole) {
  // SO, SELECT COLOR, TEXT with its five operand bytes, SO, SI, SO, then NULs.
  std::vector<uint8_t> stub = {0x0E, 0x3E, 0x22, 0x40, 0x40, 0x40,
                               0x41, 0x72, 0x0E, 0x0F, 0x0E};
  stub.resize(27, 0x00);
  NabtsCataloguedRecord entry = sentRecord(0x001, stub);
  entry.auto_acquire = true;
  entry.more_redefined = true;
  entry.has_more_address = true;
  entry.more_channel = 0x000;
  entry.more_address = 0x000003909;
  NabtsCataloguedRecord bare = sentRecord(0x002, stub);
  // One that defines a macro and draws nothing yet is no empty record: SO,
  // DEF MACRO 2/0 holding a point, END.
  NabtsCataloguedRecord defines = sentRecord(
      0x003, {0x0E, 0x1B, 0x40, 0x20, 0x24, 0x40, 0x40, 0x40, 0x1B, 0x45});
  NabtsAnalysisDataset data;
  data.records = {entry, bare, defines};

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  const std::vector<std::pair<std::string, std::string>> expected{
      {"/001", "Auto Acquire → 000003909"},
      {"/002", "Empty record"},
  };
  for (const auto& [address, badge] : expected) {
    const auto [item, payload] = find(catalogue, address);
    ASSERT_NE(item, nullptr) << address;
    EXPECT_TRUE(hasBadge(*item, badge)) << address;
    EXPECT_EQ(payload->kind, CataloguePayload::Kind::kText) << address;
    EXPECT_NE(payload->document.text.find("27 bytes arrived whole and draw "
                                          "nothing"),
              std::string::npos)
        << payload->document.text;
  }
  const auto [definer, definer_payload] = find(catalogue, "/003");
  ASSERT_NE(definer, nullptr);
  EXPECT_FALSE(hasBadge(*definer, "Empty record"));
  EXPECT_EQ(definer_payload->kind, CataloguePayload::Kind::kDisplayList);
}

// A page is never taken for data: one that draws a line and then sets its
// text in CR LF lines, the continuation of a More chain that only adds text
// to the display its predecessor left, and a record too short to judge.
TEST(NabtsCatalogueViewTest, APageIsNotTakenForData) {
  std::vector<uint8_t> page = {0x0E, 0x28, 0x49, 0x42, 0x40, 0x0F};
  for (int line = 0; line < 20; ++line) {
    const std::vector<uint8_t> row = bytesOf("TOP 20 ROW OF THE TABLE\r\n");
    page.insert(page.end(), row.begin(), row.end());
  }
  // Page 026 names its More Record by the More Flag (§5.2.7.6): 000002601,
  // which only adds text to the display 026 leaves.
  NabtsCataloguedRecord story =
      sentRecord(0x026, {0x0E, 0x28, 0x49, 0x42, 0x40, 0x0F});
  story.more = true;
  NabtsCataloguedRecord continuation = sentRecord(
      0x026, bytesOf("the rest of the story, which the page before it "
                     "began.\r\nIt adds text to the display that page left."
                     "\r\n"));
  continuation.address = 0x02601;
  continuation.chain_base_address = continuation.address;
  continuation.address_text = "000002601";
  continuation.channel_text = "000/000002601";
  NabtsAnalysisDataset data;
  data.records = {sentRecord(0x023, page),
                  sentRecord(0x025, bytesOf("A SHORT LINE\r\nAND ANOTHER\r\n")),
                  story, continuation};

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  for (const std::string address : {"/023", "/025", "/000002601"}) {
    const auto [item, payload] = find(catalogue, address);
    ASSERT_NE(item, nullptr) << address;
    EXPECT_FALSE(hasBadge(*item, "Not NAPLPS")) << address;
    EXPECT_EQ(payload->kind, CataloguePayload::Kind::kDisplayList) << address;
  }
}

}  // namespace
// A page whose record lost a stretch the recording never delivered, and a
// reference recording that carried the same page whole.
struct LostStretch {
  NabtsAnalysisDataset data;
  std::vector<uint8_t> sent;
};
LostStretch datasetWithLostStretch() {
  // SO, then twenty LINE ABS (2/8) commands to points that differ, each a
  // three-byte coordinate word (§5.3.2.2.3's default multi-value length).
  std::vector<uint8_t> plain = {0x0E};
  for (uint8_t i = 0; i < 20; ++i) {
    plain.insert(plain.end(), {0x28, static_cast<uint8_t>(0x41 + i),
                               static_cast<uint8_t>(0x60 - i),
                               static_cast<uint8_t>(0x44 + (i % 5))});
  }
  LostStretch out;
  for (const uint8_t byte : plain) {
    out.sent.push_back(teletext_odd_parity_encode(byte));
  }
  NabtsCataloguedRecord record;
  record.channel = 0x000;
  record.address_text = "000";
  record.channel_text = "000/000";
  record.record_type = 1;  // §5.2.2.3 non-cyclic presentation
  record.times_seen = 3;
  record.complete = false;
  record.data = out.sent;
  record.data_present.assign(record.data.size(), 1);
  for (size_t i = 37; i < 45; ++i) {  // Lost with its packet.
    record.data[i] = 0;
    record.data_present[i] = 0;
  }
  out.data.records.push_back(record);
  out.data.references.push_back(
      NabtsReferenceRecord{"capture_b", "000-000-v0", out.sent, {}});
  return out;
}

TEST(NabtsCatalogueViewTest, TheCatalogueOffersTheReconstructionToggle) {
  const CatalogueDataset off = build_nabts_catalogue(
      datasetWithLostStretch().data, NaplpsRenderMode::kReference,
      /*reconstruct=*/false);
  const CatalogueDataset on = build_nabts_catalogue(
      datasetWithLostStretch().data, NaplpsRenderMode::kReference,
      /*reconstruct=*/true);
  ASSERT_EQ(on.schema.toggles.size(), 3u);
  ASSERT_EQ(off.schema.toggles.size(), 3u);
  EXPECT_EQ(on.schema.toggles.front().id, kNabtsReconstructToggleId);
  EXPECT_TRUE(on.schema.toggles.front().active);
  EXPECT_FALSE(off.schema.toggles.front().active);
  EXPECT_FALSE(on.schema.toggles.front().label.empty());
  EXPECT_FALSE(on.schema.toggles.front().tooltip.empty());
}

// The receiver's pace is offered beside it, and moves when a page's figures
// appear without changing what they are: the same ops, later.
TEST(NabtsCatalogueViewTest, ReceiverPaceDelaysTheFiguresAndDrawsTheSame) {
  const NabtsAnalysisDataset data = datasetWithLostStretch().data;
  const CatalogueDataset still =
      build_nabts_catalogue(data, NaplpsRenderMode::kTwiceVector,
                            /*reconstruct=*/false, /*receiver_pace=*/false);
  const CatalogueDataset paced =
      build_nabts_catalogue(data, NaplpsRenderMode::kTwiceVector,
                            /*reconstruct=*/false, /*receiver_pace=*/true);
  ASSERT_EQ(paced.schema.toggles.size(), 3u);
  EXPECT_EQ(paced.schema.toggles[1].id, kNabtsReceiverPaceToggleId);
  EXPECT_TRUE(paced.schema.toggles[1].active);
  EXPECT_FALSE(still.schema.toggles[1].active);
  EXPECT_FALSE(paced.schema.toggles[1].tooltip.empty());

  ASSERT_EQ(still.payloads.size(), paced.payloads.size());
  bool later = false;
  for (size_t i = 0; i < still.payloads.size(); ++i) {
    const auto& a = still.payloads[i].display_list.ops;
    const auto& b = paced.payloads[i].display_list.ops;
    ASSERT_EQ(a.size(), b.size());
    for (size_t j = 0; j < a.size(); ++j) {
      EXPECT_EQ(a[j].kind, b[j].kind);
      EXPECT_EQ(a[j].points.size(), b[j].points.size());
      EXPECT_LE(a[j].appear_ms, b[j].appear_ms);
      later = later || b[j].appear_ms > a[j].appear_ms;
    }
  }
  EXPECT_TRUE(later);
}

// The TU-2000x's pace is offered as a third box, and plays whether or not
// Receiver pace is ticked: every combination of the two means one receiver,
// and each box reports the state it was asked for.
TEST(NabtsCatalogueViewTest, TheTu2000xPaceIsOfferedBesideReceiverPace) {
  const NabtsAnalysisDataset data = datasetWithLostStretch().data;
  const CatalogueDataset tu =
      build_nabts_catalogue(data, NaplpsRenderMode::kTwiceVector,
                            /*reconstruct=*/false, /*receiver_pace=*/false,
                            /*tu2000x_pace=*/true);
  ASSERT_EQ(tu.schema.toggles.size(), 3u);
  EXPECT_EQ(tu.schema.toggles[2].id, kNabtsTu2000xPaceToggleId);
  EXPECT_TRUE(tu.schema.toggles[2].active);
  EXPECT_FALSE(tu.schema.toggles[1].active);
  EXPECT_FALSE(tu.schema.toggles[2].label.empty());
  EXPECT_FALSE(tu.schema.toggles[2].tooltip.empty());

  EXPECT_EQ(naplps_receiver_pace_for(false, false), NaplpsReceiverPace::kOff);
  EXPECT_EQ(naplps_receiver_pace_for(true, false),
            NaplpsReceiverPace::kNorpakTtx650);
  EXPECT_EQ(naplps_receiver_pace_for(false, true),
            NaplpsReceiverPace::kPanasonicTu2000x);
  EXPECT_EQ(naplps_receiver_pace_for(true, true),
            NaplpsReceiverPace::kPanasonicTu2000x);

  // The two paces time the same page differently, and draw the same.
  const CatalogueDataset ttx =
      build_nabts_catalogue(data, NaplpsRenderMode::kTwiceVector,
                            /*reconstruct=*/false, /*receiver_pace=*/true);
  ASSERT_EQ(tu.payloads.size(), ttx.payloads.size());
  bool differs = false;
  for (size_t i = 0; i < tu.payloads.size(); ++i) {
    const auto& a = ttx.payloads[i].display_list.ops;
    const auto& b = tu.payloads[i].display_list.ops;
    ASSERT_EQ(a.size(), b.size());
    for (size_t j = 0; j < a.size(); ++j) {
      EXPECT_EQ(a[j].kind, b[j].kind);
      differs = differs || a[j].appear_ms != b[j].appear_ms;
    }
  }
  EXPECT_TRUE(differs);
}

namespace {

/// A page at a receiver's pace: a filled rectangle drawn from 100 ms to 300 ms
/// over rows 0.25 to 0.5 of unit y, swept the way |sweep| says, then a line
/// and a letter that each took a few milliseconds.
NabtsPageSnapshot pacedPage(NabtsFillSweep sweep) {
  NabtsPageSnapshot snapshot;
  nabts_default_colour_map(snapshot.colour_map);
  snapshot.timeline.fill_sweep = sweep;
  NabtsPrimitive fill;
  fill.kind = NabtsPrimitiveKind::kRectangle;
  fill.filled = true;
  fill.origin = NabtsPoint{0.25, 0.25};
  fill.size = NabtsSize{0.5, 0.25};
  fill.points = {fill.origin, NabtsPoint{0.75, 0.5}};
  fill.colour = kNabtsNominalWhite;
  fill.appear_ms = 300;
  fill.draw_ms = 200;
  NabtsPrimitive line;
  line.kind = NabtsPrimitiveKind::kLine;
  line.points = {NabtsPoint{0.1, 0.1}, NabtsPoint{0.9, 0.6}};
  line.origin = line.points.front();
  line.colour = kNabtsNominalWhite;
  line.appear_ms = 360;
  line.draw_ms = 57;
  NabtsPrimitive character = letter('A', 0.1, 364);
  character.draw_ms = 3;
  snapshot.primitives = {fill, line, character};
  snapshot.timeline.end_ms = 364;
  return snapshot;
}

}  // namespace

namespace {

/// A filled rectangle over the middle of the screen in |colour|, drawn with a
/// pel of |pel| at |appear_ms|.
NabtsPrimitive middleSquare(NabtsColour colour, uint32_t appear_ms,
                            double pel = 0.0) {
  NabtsPrimitive square;
  square.kind = NabtsPrimitiveKind::kRectangle;
  square.filled = true;
  square.origin = NabtsPoint{0.25, 0.25};
  square.size = NabtsSize{0.5, 0.25};
  square.points = {square.origin, NabtsPoint{0.75, 0.5}};
  square.logical_pel = NabtsSize{pel, pel};
  square.colour = colour;
  square.appear_ms = appear_ms;
  return square;
}

}  // namespace

// A page that draws a figure and, at the same moment, draws it again in the
// same place in another colour showed only the second on a receiver; the
// vector list carries only that one, so the first cannot fringe it. Drawn
// after a pause, or with another pel, both are seen and both are listed.
TEST(NabtsCatalogueViewTest, AFigureDrawnAgainInPlaceAtOnceIsListedOnce) {
  constexpr NabtsColour kRed{0, 15, 0, false};
  NabtsPageSnapshot snapshot;
  nabts_default_colour_map(snapshot.colour_map);
  snapshot.primitives = {middleSquare(kNabtsNominalWhite, 0),
                         middleSquare(kRed, 0)};
  CatalogueDisplayList list =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kTwiceVector);
  ASSERT_EQ(list.ops.size(), 1u);
  EXPECT_EQ(list.ops[0].colour.red, 255);
  EXPECT_EQ(list.ops[0].colour.green, 0);

  snapshot.primitives = {middleSquare(kNabtsNominalWhite, 0),
                         middleSquare(kRed, 700)};
  snapshot.timeline.end_ms = 700;
  list = nabts_page_display_list(snapshot, NaplpsRenderMode::kTwiceVector);
  EXPECT_EQ(list.ops.size(), 2u);

  snapshot.primitives = {middleSquare(kNabtsNominalWhite, 0, 0.01),
                         middleSquare(kRed, 0)};
  snapshot.timeline.end_ms = 0;
  list = nabts_page_display_list(snapshot, NaplpsRenderMode::kTwiceVector);
  EXPECT_EQ(list.ops.size(), 2u);
}

// At a receiver's pace a fill is swept onto the screen over the time it took,
// down its rows on the TU-2000x and up them on the TTX 650; a line and a
// character appear whole when drawn.
TEST(NabtsCatalogueViewTest, AFillIsSweptOnTheWayItsReceiverDrewIt) {
  const CatalogueDisplayList down = nabts_page_display_list(
      pacedPage(NabtsFillSweep::kTopDown), NaplpsRenderMode::kTwiceVector);
  ASSERT_EQ(down.ops.size(), 3u);
  EXPECT_EQ(down.ops[0].appear_ms, 300u);
  EXPECT_EQ(down.ops[0].reveal_duration_ms, 200u);
  EXPECT_DOUBLE_EQ(down.ops[0].reveal_from_y, 0.5);
  EXPECT_DOUBLE_EQ(down.ops[0].reveal_to_y, 0.25);
  EXPECT_EQ(down.ops[1].reveal_duration_ms, 0u) << "a line appears whole";
  EXPECT_EQ(down.ops[2].reveal_duration_ms, 0u) << "a character appears whole";

  const CatalogueDisplayList up = nabts_page_display_list(
      pacedPage(NabtsFillSweep::kBottomUp), NaplpsRenderMode::kTwiceVector);
  EXPECT_DOUBLE_EQ(up.ops[0].reveal_from_y, 0.25);
  EXPECT_DOUBLE_EQ(up.ops[0].reveal_to_y, 0.5);
}

// The pixel modes sweep the runs of the rows the fill changed, together.
TEST(NabtsCatalogueViewTest, APixelPageSweepsTheRowsAFillChanged) {
  const CatalogueDisplayList list = nabts_page_display_list(
      pacedPage(NabtsFillSweep::kTopDown), NaplpsRenderMode::kReference);
  ASSERT_FALSE(list.ops.empty());
  size_t swept = 0;
  double lowest = 1.0;
  double highest = 0.0;
  for (const CatalogueDrawOp& op : list.ops) {
    if (op.appear_ms != 300) {
      EXPECT_EQ(op.reveal_duration_ms, 0u);
      continue;
    }
    ++swept;
    EXPECT_EQ(op.reveal_duration_ms, 200u);
    EXPECT_DOUBLE_EQ(op.reveal_from_y, list.ops.front().reveal_from_y);
    EXPECT_DOUBLE_EQ(op.reveal_to_y, list.ops.front().reveal_to_y);
    lowest = std::min(lowest, op.origin.y);
    highest = std::max(highest, op.origin.y + op.size.dy);
  }
  // A run a row: the fill's 64 rows and the row its top edge is in, swept from
  // the top of the highest to the foot of the lowest.
  EXPECT_GE(swept, 64u);
  EXPECT_LE(swept, 66u);
  EXPECT_DOUBLE_EQ(list.ops.front().reveal_from_y, highest);
  EXPECT_DOUBLE_EQ(list.ops.front().reveal_to_y, lowest);
}

// A clear at a receiver's pace sweeps the whole display area.
TEST(NabtsCatalogueViewTest, AClearSweepsTheWholeDisplay) {
  NabtsPageSnapshot snapshot;
  nabts_default_colour_map(snapshot.colour_map);
  snapshot.timeline.fill_sweep = NabtsFillSweep::kBottomUp;
  snapshot.timeline.superseded = {letter('A', 0.1, 3)};
  snapshot.timeline.clears = {NabtsDisplayClear{172, 1, 169}};
  snapshot.primitives = {letter('B', 0.5, 175)};

  const CatalogueDisplayList list =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kTwiceVector);
  ASSERT_EQ(list.ops.size(), 3u);
  EXPECT_EQ(list.ops[1].kind, CatalogueDrawKind::kRectangle);
  EXPECT_EQ(list.ops[1].appear_ms, 172u);
  EXPECT_EQ(list.ops[1].reveal_duration_ms, 169u);
  EXPECT_DOUBLE_EQ(list.ops[1].reveal_from_y, 0.0);
  EXPECT_DOUBLE_EQ(list.ops[1].reveal_to_y, kNabtsDisplayAreaHeight);

  const CatalogueDisplayList pixels =
      nabts_page_display_list(snapshot, NaplpsRenderMode::kReference);
  bool cleared = false;
  for (const CatalogueDrawOp& op : pixels.ops) {
    if (op.appear_ms == 172) {
      cleared = true;
      EXPECT_EQ(op.reveal_duration_ms, 169u);
      EXPECT_DOUBLE_EQ(op.reveal_from_y, 0.0);
      EXPECT_NEAR(op.reveal_to_y, kNabtsDisplayAreaHeight, 1e-12);
    }
  }
  EXPECT_TRUE(cleared);
}

// Without a receiver's pace nothing is swept: the lists are as they were.
TEST(NabtsCatalogueViewTest, WithoutAPaceNothingIsSwept) {
  const NabtsAnalysisDataset data = datasetWithLostStretch().data;
  for (const NaplpsRenderMode mode :
       {NaplpsRenderMode::kReference, NaplpsRenderMode::kTwiceVector}) {
    const CatalogueDataset still = build_nabts_catalogue(data, mode);
    for (const CataloguePayload& payload : still.payloads) {
      for (const CatalogueDrawOp& op : payload.display_list.ops) {
        EXPECT_EQ(op.reveal_duration_ms, 0u);
      }
    }
  }
}

// The reference holds the page whole, so the lost stretch comes back from it
// and the page says so.
TEST(NabtsCatalogueViewTest,
     ReconstructionRebuildsALostStretchAndSaysFromWhere) {
  const LostStretch lost = datasetWithLostStretch();
  const CatalogueDataset on = build_nabts_catalogue(
      lost.data, NaplpsRenderMode::kReference, /*reconstruct=*/true);
  const CatalogueDataset whole = [&] {
    NabtsAnalysisDataset data = lost.data;
    data.records.front().data = lost.sent;
    data.records.front().data_present.clear();
    data.references.clear();
    return build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  }();

  ASSERT_FALSE(on.payloads.empty());
  const std::string& condition = on.payloads.front().condition;
  EXPECT_NE(condition.find("8 bytes lost in the recording"), std::string::npos)
      << condition;
  EXPECT_NE(condition.find("8 bytes rebuilt from capture_b"), std::string::npos)
      << condition;
  const std::vector<std::string>& badges = on.items.front().badges;
  EXPECT_NE(std::find(badges.begin(), badges.end(), "~"), badges.end());
  // Drawn as the page that was sent.
  ASSERT_FALSE(whole.payloads.empty());
  EXPECT_EQ(on.payloads.front().display_list.ops.size(),
            whole.payloads.front().display_list.ops.size());

  bool noticed = false;
  for (const std::string& notice : on.summary.notices) {
    noticed = noticed || notice.rfind("Reconstruction on:", 0) == 0;
  }
  EXPECT_TRUE(noticed);
}

TEST(NabtsCatalogueViewTest, WithoutReconstructionAPageReadsAsRecovered) {
  const CatalogueDataset off = build_nabts_catalogue(
      datasetWithLostStretch().data, NaplpsRenderMode::kReference);
  ASSERT_FALSE(off.payloads.empty());
  EXPECT_EQ(off.payloads.front().condition.find("rebuilt"), std::string::npos);
  const std::vector<std::string>& badges = off.items.front().badges;
  EXPECT_EQ(std::find(badges.begin(), badges.end(), "~"), badges.end());
  for (const std::string& notice : off.summary.notices) {
    EXPECT_NE(notice.rfind("Reconstruction on:", 0), 0u) << notice;
  }
}

// ---------------------------------------------------------------------------
// Pages played on keys (X3.110 §5.5, Table D1 item 7)
// ---------------------------------------------------------------------------

namespace {

/// A page as a game moves its cursor: SO, POINT SET ABS (0.5,0.25),
/// then DEF MACRO 2/9 "POINT REL (+1/16,0)" and DEF MACRO 2/7 "POINT REL
/// (-1/16,0)", each closed by END. It draws nothing until a key is pressed.
NabtsAnalysisDataset datasetWithWalkingPage() {
  NabtsAnalysisDataset data;
  NabtsCataloguedRecord record;
  record.channel = 0x000;
  record.address = 0x035;
  record.address_text = "035";
  record.channel_text = "000/035";
  record.record_type = 0;  // §5.2.2.2 cyclic presentation
  record.times_seen = 1;
  record.times_intact = 1;
  record.complete = true;
  const std::vector<uint8_t> plain = {
      0x0E, 0x24, 0x51, 0x40, 0x40,                    // POINT SET ABS
      0x1B, 0x40, 0x29, 0x27, 0x40, 0x50, 0x40,        // DEF MACRO 2/9
      0x1B, 0x40, 0x27, 0x27, 0x78, 0x70, 0x40, 0x1B,  // DEF MACRO 2/7
      0x45};                                           // END
  for (const uint8_t byte : plain) {
    record.data.push_back(teletext_odd_parity_encode(byte));
  }
  data.records.push_back(record);
  return data;
}

/// The x of every point a display list draws, in order.
std::vector<double> point_xs(const CatalogueDisplayList& list) {
  std::vector<double> xs;
  for (const CatalogueDrawOp& op : list.ops) {
    if (op.kind == CatalogueDrawKind::kPoint) {
      xs.push_back(op.origin.x);
    }
  }
  return xs;
}

}  // namespace

// A page that links keys says which, so the host can offer them, and a service
// that has such a page names its keypad.
TEST(NabtsCatalogueViewTest, APageThatLinksKeysListsThemForTheKeypad) {
  const CatalogueDataset catalogue = build_nabts_catalogue(
      datasetWithWalkingPage(), NaplpsRenderMode::kTwiceVector);
  ASSERT_EQ(catalogue.items.size(), 1u);
  EXPECT_EQ(catalogue.items[0].keys, (std::vector<std::string>{"7", "9"}));
  const std::vector<std::string>& badges = catalogue.items[0].badges;
  EXPECT_NE(std::find(badges.begin(), badges.end(), kNabtsKeypadGameBadge),
            badges.end());
  EXPECT_EQ(catalogue.schema.keypad_label, kNabtsKeypadLabel);

  // A page that links none offers none; its catalogue still names the
  // keypad, whose digits ask for a page by number.
  const CatalogueDataset plain = build_nabts_catalogue(
      datasetWithLineRecord(), NaplpsRenderMode::kTwiceVector);
  ASSERT_FALSE(plain.items.empty());
  EXPECT_TRUE(plain.items[0].keys.empty());
  EXPECT_EQ(plain.schema.keypad_label, kNabtsKeypadLabel);
}

// The page played is the page after every key pressed on it, in order, each
// starting where the last left the drawing point; no keys is the page as
// listed, and an id that names no page plays nothing.
TEST(NabtsCatalogueViewTest, APlayedPageIsThePageAfterEveryKeyPressed) {
  const NabtsAnalysisDataset data = datasetWithWalkingPage();
  const std::string id =
      build_nabts_catalogue(data, NaplpsRenderMode::kTwiceVector).items[0].id;

  const CataloguePayload walked = build_nabts_played_payload(
      data, id, {"9", "9", "9"}, NaplpsRenderMode::kTwiceVector);
  ASSERT_EQ(walked.kind, CataloguePayload::Kind::kDisplayList);
  EXPECT_EQ(point_xs(walked.display_list),
            (std::vector<double>{0.5625, 0.625, 0.6875}));

  const CataloguePayload back = build_nabts_played_payload(
      data, id, {"9", "7"}, NaplpsRenderMode::kTwiceVector);
  EXPECT_EQ(point_xs(back.display_list), (std::vector<double>{0.5625, 0.5}));

  const CataloguePayload fresh =
      build_nabts_played_payload(data, id, {}, NaplpsRenderMode::kTwiceVector);
  ASSERT_EQ(fresh.kind, CataloguePayload::Kind::kDisplayList);
  EXPECT_TRUE(point_xs(fresh.display_list).empty());

  EXPECT_EQ(build_nabts_played_payload(data, "nothing", {"9"}).kind,
            CataloguePayload::Kind::kNone);
}

// Every receiver the viewer offers answers a played page, the pixel ones as
// the pixels they light; and at a receiver's pace the last key's drawing is
// what plays, what the keys before it drew standing from the start.
TEST(NabtsCatalogueViewTest, APlayedPageIsDrawnForTheReceiverAndPaceOnShow) {
  const NabtsAnalysisDataset data = datasetWithWalkingPage();
  const std::string id =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference).items[0].id;

  const CataloguePayload pixels =
      build_nabts_played_payload(data, id, {"9"}, NaplpsRenderMode::kReference);
  ASSERT_EQ(pixels.kind, CataloguePayload::Kind::kDisplayList);
  EXPECT_TRUE(pixels.display_list.pixel_aligned);
  EXPECT_FALSE(pixels.display_list.ops.empty());

  const CataloguePayload paced = build_nabts_played_payload(
      data, id, {"9", "9"}, NaplpsRenderMode::kTwiceVector,
      /*reconstruct=*/false, /*receiver_pace=*/true);
  std::vector<uint32_t> appear;
  for (const CatalogueDrawOp& op : paced.display_list.ops) {
    if (op.kind == CatalogueDrawKind::kPoint) {
      appear.push_back(op.appear_ms);
    }
  }
  ASSERT_EQ(appear.size(), 2u);
  EXPECT_EQ(appear[0], 0u) << "the first key's point stands from the start";
  EXPECT_GT(appear[1], 0u) << "the last key's point is drawn at its pace";
}

namespace {

/// A presentation record at long address |address| that writes |rows| one
/// under another, on a cleared screen when |clear| is set, each byte with its
/// odd parity, as the sink catalogues one.
NabtsCataloguedRecord pageRecord(uint64_t address,
                                 const std::vector<std::string>& rows,
                                 bool clear) {
  NabtsCataloguedRecord record = datasetWithLineRecord().records.front();
  record.address = address;
  record.chain_base_address = address;
  char text[16];
  if ((address & 0xFFu) == 0 && (address >> 20) == 0) {
    std::snprintf(text, sizeof(text), "%03X",
                  static_cast<unsigned>(address >> 8));
  } else {
    std::snprintf(text, sizeof(text), "%09llX",
                  static_cast<unsigned long long>(address));
  }
  record.address_text = text;
  record.channel_text = std::string("000/") + text;
  record.record_type = 0;
  record.first_seen_frame = 10;
  record.last_seen_frame = 900;
  std::vector<uint8_t> plain;
  if (clear) {
    plain.push_back(0x0C);  // CS
  }
  plain.push_back(0x0F);  // SI
  for (size_t i = 0; i < rows.size(); ++i) {
    if (i > 0) {
      plain.push_back(0x0D);  // APR
      plain.push_back(0x0A);  // APD
    }
    plain.insert(plain.end(), rows[i].begin(), rows[i].end());
  }
  record.data.clear();
  for (const uint8_t byte : plain) {
    record.data.push_back(teletext_odd_parity_encode(byte));
  }
  return record;
}

const CatalogueItem* itemWithId(const CatalogueDataset& catalogue,
                                const std::string& id) {
  for (const CatalogueItem& item : catalogue.items) {
    if (item.id == id) {
      return &item;
    }
  }
  return nullptr;
}

/// The column of the list called |column| for |item|.
std::string columnValue(const CatalogueDataset& catalogue,
                        const CatalogueItem& item, const std::string& column) {
  for (size_t i = 0; i < catalogue.schema.columns.size(); ++i) {
    if (catalogue.schema.columns[i].id == column && i < item.values.size()) {
      return item.values[i];
    }
  }
  return {};
}

/// Where |item|'s link of |kind| leads, or empty.
std::string linkTarget(const CatalogueItem& item, CatalogueLink::Kind kind) {
  for (const CatalogueLink& link : item.links) {
    if (link.kind == kind) {
      return link.target_id;
    }
  }
  return {};
}

}  // namespace

// A viewer reached a page by typing its number, and the Panasonic TU-2000x
// set what was typed right-aligned at A7 of the long address: three digits for
// a short address, four for a carousel's long-address pages, the whole nine for
// a More Record. Each item answers to that number in the find box.
TEST(NabtsCatalogueViewTest, AnItemAnswersToTheNumberAKeypadTook) {
  NabtsAnalysisDataset data;
  data.records = {pageRecord(0x00001200, {"A"}, true),
                  pageRecord(0x00198500, {"B"}, true),
                  pageRecord(0x00000201, {"C"}, true)};
  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  EXPECT_EQ(catalogue.schema.find_label, "Page:");
  for (const auto& [id, number] :
       std::vector<std::pair<std::string, std::string>>{
           {"000/012 v0", "012"},
           {"000/000198500 v0", "1985"},
           {"000/000000201 v0", "000000201"}}) {
    const CatalogueItem* item = itemWithId(catalogue, id);
    ASSERT_NE(item, nullptr) << id;
    EXPECT_EQ(item->find_aliases, (std::vector<std::string>{number})) << id;
  }
}

// CEA-516 §5.2.8.2: a page's header extension names the records its NEXT and
// INDEX keys fetch; where it names none, NEXT took the next page by number and
// INDEX the Master Index, as the TU-2000x had them. The page a page names as
// its index heads a section, which the list says beside each page.
TEST(NabtsCatalogueViewTest, AKeyLeadsWhereTheHeaderSaysElseByNumber) {
  NabtsAnalysisDataset data;
  data.records = {pageRecord(0x00000000, {"INDEX"}, true),
                  pageRecord(0x00001000, {"WEATHER"}, true),
                  pageRecord(0x00001100, {"MAP"}, true),
                  pageRecord(0x00001200, {"TEMPERATURES"}, true),
                  pageRecord(0x00001300, {"TRAVEL"}, true)};
  data.records[2].has_next_address = true;
  data.records[2].next_address = 0x00001300;
  data.records[2].has_index_address = true;
  data.records[2].index_address = 0x00001000;
  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);

  const CatalogueItem* map = itemWithId(catalogue, "000/011 v0");
  const CatalogueItem* temperatures = itemWithId(catalogue, "000/012 v0");
  const CatalogueItem* weather = itemWithId(catalogue, "000/010 v0");
  ASSERT_NE(map, nullptr);
  ASSERT_NE(temperatures, nullptr);
  ASSERT_NE(weather, nullptr);
  EXPECT_EQ(linkTarget(*map, CatalogueLink::Kind::kNext), "000/013 v0");
  EXPECT_EQ(linkTarget(*map, CatalogueLink::Kind::kIndex), "000/010 v0");
  EXPECT_EQ(linkTarget(*temperatures, CatalogueLink::Kind::kNext),
            "000/013 v0");
  EXPECT_EQ(linkTarget(*temperatures, CatalogueLink::Kind::kIndex),
            "000/000 v0");
  EXPECT_EQ(columnValue(catalogue, *map, "section"), "010");
  EXPECT_EQ(columnValue(catalogue, *weather, "section"), "010");
  EXPECT_EQ(columnValue(catalogue, *temperatures, "section"), "");
}

// A service that flags its section indexes instead (§5.2.7.2 Y13), for
// example one every five pages, puts a page in the section of the flagged page
// at or below it, which is where INDEX returns.
TEST(NabtsCatalogueViewTest, AFlaggedIndexHeadsThePagesAfterIt) {
  NabtsAnalysisDataset data;
  data.records = {pageRecord(0x00000000, {"CONTENTS"}, true),
                  pageRecord(0x00000100, {"FITNESS"}, true),
                  pageRecord(0x00000200, {"RUNNING"}, true),
                  pageRecord(0x00000600, {"VITAMINS"}, true),
                  pageRecord(0x00000700, {"B VITAMINS"}, true)};
  for (const size_t index : {0, 1, 3}) {
    data.records[index].index = true;
  }
  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  const CatalogueItem* running = itemWithId(catalogue, "000/002 v0");
  const CatalogueItem* vitamins = itemWithId(catalogue, "000/007 v0");
  ASSERT_NE(running, nullptr);
  ASSERT_NE(vitamins, nullptr);
  EXPECT_EQ(columnValue(catalogue, *running, "section"), "001");
  EXPECT_EQ(columnValue(catalogue, *vitamins, "section"), "006");
  EXPECT_EQ(linkTarget(*running, CatalogueLink::Kind::kIndex), "000/001 v0");
}

// The numbers a page offers — a board's squares, a quiz's answers — are links
// to the pages of those numbers the recording carried.
TEST(NabtsCatalogueViewTest, ANumberAPageOffersIsALinkToThatPage) {
  NabtsAnalysisDataset data;
  data.records = {pageRecord(0x00015000, {"QUESTION"}, true),
                  pageRecord(0x00025000, {"QUESTION"}, true),
                  pageRecord(0x00071100, {"PUZZLE", "150 250 350"}, true)};
  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  const CatalogueItem* board = itemWithId(catalogue, "000/711 v0");
  ASSERT_NE(board, nullptr);
  std::vector<std::pair<std::string, std::string>> offered;
  for (const CatalogueLink& link : board->links) {
    if (link.kind == CatalogueLink::Kind::kReference) {
      offered.emplace_back(link.label, link.target_id);
    }
  }
  // 350 was not carried, so it leads nowhere.
  EXPECT_EQ(offered, (std::vector<std::pair<std::string, std::string>>{
                         {"150", "000/150 v0"}, {"250", "000/250 v0"}}));
}

// A page that starts on the screen it was asked from is drawn over the page
// that offers it, and its condition line says so.
TEST(NabtsCatalogueViewTest, APageDrawnOverAnotherSaysWhich) {
  NabtsAnalysisDataset data;
  data.records = {pageRecord(0x00071100, {"PUZZLE", "150 750"}, true),
                  pageRecord(0x00075000, {"751 Yes", "752 No"}, false)};
  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  for (size_t i = 0; i < catalogue.items.size(); ++i) {
    if (catalogue.items[i].id != "000/750 v0") {
      continue;
    }
    EXPECT_NE(catalogue.payloads[i].condition.find("drawn over 000/711 v0"),
              std::string::npos)
        << catalogue.payloads[i].condition;
    EXPECT_NE(catalogue.items[i].tooltip.find("TU-2000x"), std::string::npos);
    return;
  }
  ADD_FAILURE() << "000/750 v0 was not listed";
}

// A quiz played by page number: a board
// whose numbers bring up questions drawn over it, each question's numbers its
// answers drawn over the question, and a copy of a question the board's NEXT
// leads to, which offers the same answers. Every page of it is marked a game;
// an index that leads to one page drawn over it is not a quiz.
TEST(NabtsCatalogueViewTest, ThePagesOfAQuizPlayedByNumberAreMarked) {
  NabtsAnalysisDataset data;
  data.records = {
      pageRecord(0x00071100, {"PUZZLE", "150 250"}, true),
      pageRecord(0x00015000, {"GIFT?", "151 silk", "152 paper"}, false),
      pageRecord(0x00015100, {"WRONG"}, false),
      pageRecord(0x00015200, {"RIGHT!"}, false),
      pageRecord(0x00025000, {"PLAY?", "251 Cats", "252 Annie"}, false),
      pageRecord(0x00025100, {"RIGHT!"}, false),
      pageRecord(0x00025200, {"WRONG"}, false),
      pageRecord(0x00071200, {"GIFT?", "151 silk", "152 paper"}, false),
      pageRecord(0x00004000, {"SPOTLIGHT", "41.Rock News", "44.Alice"}, true),
      pageRecord(0x00004100, {"ROCK NEWS"}, true),
      pageRecord(0x00004400, {"ALICE"}, false)};
  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  for (const char* id :
       {"000/711 v0", "000/150 v0", "000/151 v0", "000/152 v0", "000/250 v0",
        "000/251 v0", "000/252 v0", "000/712 v0"}) {
    const CatalogueItem* item = itemWithId(catalogue, id);
    ASSERT_NE(item, nullptr) << id;
    EXPECT_TRUE(hasBadge(*item, kNabtsQuizGameBadge)) << id;
    EXPECT_NE(item->tooltip.find("quiz played by page number"),
              std::string::npos)
        << id;
  }
  const CatalogueItem* answer = itemWithId(catalogue, "000/151 v0");
  EXPECT_NE(answer->tooltip.find("It answers 000/150 v0"), std::string::npos)
      << answer->tooltip;
  for (const char* id : {"000/040 v0", "000/041 v0", "000/044 v0"}) {
    const CatalogueItem* item = itemWithId(catalogue, id);
    ASSERT_NE(item, nullptr) << id;
    EXPECT_TRUE(item->badges.empty()) << id;
  }
}

// A page row whose sub-pages hold a game carries the mark too: the list
// shows the row, not the sub-pages.
TEST(NabtsCatalogueViewTest, APageRowIsMarkedForTheGameItsSubPagesHold) {
  NabtsAnalysisDataset data = datasetWithWalkingPage();
  NabtsCataloguedRecord revised = data.records.back();
  revised.version = 1;
  revised.first_seen_frame += 1000;
  revised.last_seen_frame += 1000;
  data.records.push_back(revised);
  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  ASSERT_FALSE(catalogue.items.empty());
  EXPECT_TRUE(catalogue.items[0].parent_id.empty());
  EXPECT_TRUE(hasBadge(catalogue.items[0], kNabtsKeypadGameBadge));
}

// Where a page's header names no index, the Master Index's list of sections
// puts it in one: the nearest start at or below it — an entry of the list, or
// an index the pages name — where the pages from there run on to it. An entry
// no page names takes the index its range's pages name; the list's last
// entry, and a page beyond a run of unused numbers, take none. INDEX still
// leads where the decoder took it.
TEST(NabtsCatalogueViewTest, TheMasterIndexListPutsAPageInASection) {
  NabtsAnalysisDataset data;
  data.records = {
      pageRecord(0x00000000,
                 {"Index Guide", " 2.News     50.Kids",
                  "10.Weather   60.NewsCaps", "100.About the service"},
                 true),
      pageRecord(0x00000100, {"HIGHLIGHTS"}, true),
      pageRecord(0x00000300, {"NEWS UPDATE"}, true),
      pageRecord(0x00000700, {"AN ADVERTISEMENT"}, true),
      pageRecord(0x00001000, {"WEATHER"}, true),
      pageRecord(0x00001100, {"MAP"}, true),
      pageRecord(0x00006000, {"NEWSCAPS"}, true),
      pageRecord(0x00006100, {"CAPTIONS"}, true),
      pageRecord(0x00008000, {"ANOTHER CYCLE"}, true),
      pageRecord(0x00010000, {"WHAT IS IT?"}, true),
      pageRecord(0x00015000, {"A QUESTION"}, true)};
  data.records[2].has_index_address = true;
  data.records[2].index_address = 0x00000100;
  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  const auto section = [&catalogue](const std::string& id) {
    const CatalogueItem* item = itemWithId(catalogue, id);
    return item == nullptr ? std::string("missing")
                           : columnValue(catalogue, *item, "section");
  };
  EXPECT_EQ(section("000/003 v0"), "001") << "named by its header";
  EXPECT_EQ(section("000/007 v0"), "001")
      << "2.News, whose page names 001 as its index";
  EXPECT_EQ(section("000/011 v0"), "010") << "10.Weather";
  EXPECT_EQ(section("000/061 v0"), "060");
  EXPECT_EQ(section("000/080 v0"), "") << "18 numbers on from NewsCaps";
  EXPECT_EQ(section("000/100 v0"), "100");
  EXPECT_EQ(section("000/150 v0"), "") << "nothing runs on from 100";
  EXPECT_EQ(section("000/000 v0"), "");
  const CatalogueItem* advert = itemWithId(catalogue, "000/007 v0");
  ASSERT_NE(advert, nullptr);
  EXPECT_EQ(linkTarget(*advert, CatalogueLink::Kind::kIndex), "000/000 v0");
  EXPECT_NE(advert->tooltip.find("Master Index's list"), std::string::npos);
}

// A list that counts on from 1 is a ranking, not a list of sections.
TEST(NabtsCatalogueViewTest, ARankingOnTheMasterIndexListsNoSections) {
  NabtsAnalysisDataset data;
  data.records = {
      pageRecord(0x00000000,
                 {"TOP FIVE", "1.Dallas", "2.Dynasty", "3.Cheers", "4.Alf"},
                 true),
      pageRecord(0x00000500, {"A PAGE"}, true)};
  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  const CatalogueItem* page = itemWithId(catalogue, "000/005 v0");
  ASSERT_NE(page, nullptr);
  EXPECT_EQ(columnValue(catalogue, *page, "section"), "");
}

// A datacast and the files it carried are items of their own: what the
// datacast holds as text, a picture drawn as one raster of its pixels, and a
// page of HTML as the text it shows.
TEST(NabtsCatalogueViewTest, ListsDatacastsAndTheFilesTheyCarried) {
  NabtsAnalysisDataset data;
  NabtsDatacastListing listing;
  listing.channel = 0x242;
  listing.summary = "242: 100 data row(s)";
  listing.payload_bytes = 2600;
  listing.text = "SEVERE THUNDERSTORM WATCH";
  data.datacasts.push_back(listing);

  // A 1 x 1 GIF whose one pixel is entry 1 of a two-colour table.
  NabtsCarriedFile picture;
  picture.channel = 0x242;
  picture.transfer = "file transfer";
  picture.name = "3ec84bfe";
  picture.format = "gif";
  picture.copies = 2;
  picture.data = {'G', 'I', 'F', '8', '9', 'a', 1, 0,    1,    0, 0x80, 0,
                  0,   0,   0,   0,   255, 0,   0, 0x2C, 0,    0, 0,    0,
                  1,   0,   1,   0,   0,   2,   2, 0x4C, 0x01, 0, 0x3B};
  NabtsCarriedFile memo;
  memo.channel = 0x243;
  memo.transfer = "file carousel";
  memo.name = "035J0036.HTM";
  memo.format = "html";
  memo.copies = 3;
  memo.deciphered = true;
  const std::string html = "<html><p>Timings&nbsp;for 5/20</p></html>";
  memo.data.assign(html.begin(), html.end());
  data.carried_files = {picture, memo};

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  const auto payload_of =
      [&catalogue](const std::string& section,
                   const std::string& address) -> const CataloguePayload* {
    for (size_t i = 0; i < catalogue.items.size(); ++i) {
      if (columnValue(catalogue, catalogue.items[i], "section") == section &&
          columnValue(catalogue, catalogue.items[i], "address") == address) {
        return &catalogue.payloads[i];
      }
    }
    return nullptr;
  };

  const CataloguePayload* datacast = payload_of("Datacasts", "242");
  ASSERT_NE(datacast, nullptr);
  EXPECT_EQ(datacast->kind, CataloguePayload::Kind::kText);
  EXPECT_NE(datacast->document.text.find("SEVERE THUNDERSTORM WATCH"),
            std::string::npos);

  const CataloguePayload* image = payload_of("Datacast files", "242/3ec84bfe");
  ASSERT_NE(image, nullptr);
  ASSERT_EQ(image->kind, CataloguePayload::Kind::kDisplayList);
  EXPECT_TRUE(image->display_list.pixel_aligned);
  ASSERT_EQ(image->display_list.ops.size(), 1u);
  const CatalogueDrawOp& raster = image->display_list.ops[0];
  EXPECT_EQ(raster.kind, CatalogueDrawKind::kColourRun);
  ASSERT_EQ(raster.colour_run.size(), 1u);
  EXPECT_EQ(raster.colour_run[0].red, 255);

  const CataloguePayload* page =
      payload_of("Datacast files", "243/035J0036.HTM");
  ASSERT_NE(page, nullptr);
  EXPECT_EQ(page->kind, CataloguePayload::Kind::kText);
  EXPECT_NE(page->document.text.find("Timings for 5/20"), std::string::npos);
  EXPECT_EQ(page->document.text.find('<'), std::string::npos);
}

// A binary file is shown as a dump of its bytes and the text it holds: a
// document's paragraphs, each a line, and no run too short or too unlike
// words to be text.
TEST(NabtsCatalogueViewTest, ShowsTheTextABinaryFileHolds) {
  NabtsAnalysisDataset data;
  NabtsCarriedFile document;
  document.channel = 0x010;
  document.transfer = "framed service";
  document.name = "notes.doc";
  document.format = "binary";
  document.copies = 1;
  const std::string body =
      "The packets were sent in turn.\rEach row carries its own code.";
  document.data = {0xD0, 0xCF, 0x11, 0xE0, 0x00, 'Q', 'X', '7', 0x01};
  document.data.insert(document.data.end(), body.begin(), body.end());
  const std::string noise = "@@@@@@@@@@@@@@@@@@@@@@@@ x";
  document.data.push_back(0x00);
  document.data.insert(document.data.end(), noise.begin(), noise.end());
  document.data.push_back(0x00);
  data.carried_files = {document};

  const CatalogueDataset catalogue =
      build_nabts_catalogue(data, NaplpsRenderMode::kReference);
  std::string text;
  for (size_t i = 0; i < catalogue.items.size(); ++i) {
    if (columnValue(catalogue, catalogue.items[i], "address") ==
        "010/notes.doc") {
      text = catalogue.payloads[i].document.text;
    }
  }
  EXPECT_NE(text.find("00000000  d0 cf 11 e0"), std::string::npos) << text;
  EXPECT_NE(text.find("The packets were sent in turn.\nEach row carries its "
                      "own code.\n"),
            std::string::npos)
      << text;
  EXPECT_EQ(text.find("@@@@@@@@@@@@@@@@@@@@@@@@ x\n"), std::string::npos)
      << text;
}

}  // namespace orc
