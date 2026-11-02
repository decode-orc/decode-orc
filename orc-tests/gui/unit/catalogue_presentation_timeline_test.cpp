/*
 * File:        catalogue_presentation_timeline_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests for reading the timing of a catalogue display list
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "catalogue_presentation_timeline.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gui_unit_test {
namespace {

const orc::CatalogueColour kBlue{0, 140, 220};
const orc::CatalogueColour kYellow{255, 210, 0};
const orc::CatalogueColour kRed{255, 0, 0};
const orc::CatalogueColour kWhite{255, 255, 255};
const orc::CatalogueColour kBlack{0, 0, 0};

/// |colour| as the entry |index| of the palette, the way a source that stores
/// colour map addresses hands it over.
orc::CatalogueColour inEntry(orc::CatalogueColour colour, int16_t index) {
  colour.palette_index = index;
  return colour;
}

/// A colour as "r,g,b", so a mismatch reads as two colours.
std::string rgb(const orc::CatalogueColour& colour) {
  return std::to_string(colour.red) + "," + std::to_string(colour.green) + "," +
         std::to_string(colour.blue);
}

/// An operation that appears at |appear_ms|.
orc::CatalogueDrawOp opAt(uint32_t appear_ms) {
  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kRectangle;
  op.filled = true;
  op.colour = kWhite;
  op.appear_ms = appear_ms;
  return op;
}

/// A track holding each colour from its time on, with no loop.
orc::CataloguePaletteTrack holdingTrack(
    std::initializer_list<std::pair<uint32_t, orc::CatalogueColour>> steps) {
  orc::CataloguePaletteTrack track;
  for (const auto& [time_ms, colour] : steps) {
    track.steps.push_back(orc::CatalogueColourStep{time_ms, colour});
  }
  return track;
}

/// A blink: |from| until |settle_ms|, where the cycle starts, then |to| for
/// |off_ms| and |from| for |on_ms|, round and round.
orc::CataloguePaletteTrack blinkTrack(const orc::CatalogueColour& from,
                                      const orc::CatalogueColour& to,
                                      uint32_t settle_ms, uint32_t off_ms,
                                      uint32_t on_ms) {
  orc::CataloguePaletteTrack track;
  if (settle_ms > 0) {
    track.steps.push_back(orc::CatalogueColourStep{0, from});
  }
  track.steps.push_back(orc::CatalogueColourStep{settle_ms, to});
  track.steps.push_back(orc::CatalogueColourStep{settle_ms + off_ms, from});
  track.loop_start_ms = settle_ms;
  track.loop_period_ms = off_ms + on_ms;
  return track;
}

}  // namespace

// A default timeline — no list at all — has nothing to play.
TEST(CataloguePresentationTimeline, AnEmptyTimelineHoldsStill) {
  const CataloguePresentationTimeline timeline;
  EXPECT_FALSE(timeline.timed());
  EXPECT_FALSE(timeline.nextChangeAfter(0).has_value());
  EXPECT_EQ(timeline.settledAt(), std::optional<uint32_t>(0));
  EXPECT_EQ(rgb(timeline.colourAt(inEntry(kRed, 0), 500)), rgb(kRed));
}

// Every operation at the start and no track: the list is a still picture.
TEST(CataloguePresentationTimeline, AListWithNoTimingIsNotTimed) {
  orc::CatalogueDisplayList list;
  list.ops = {opAt(0), opAt(0)};
  list.palette = {kBlue, kYellow};

  const CataloguePresentationTimeline timeline(list);
  EXPECT_FALSE(timeline.timed());
  EXPECT_FALSE(timeline.nextChangeAfter(0).has_value());
  EXPECT_EQ(timeline.settledAt(), std::optional<uint32_t>(0));
}

// A page drawn in stages: each operation is on screen from its own time, and
// the view wakes once per stage and not in between.
TEST(CataloguePresentationTimeline, OperationsAppearAtTheirOwnTime) {
  orc::CatalogueDisplayList list;
  list.ops = {opAt(0), opAt(500), opAt(500), opAt(1200)};

  const CataloguePresentationTimeline timeline(list);
  EXPECT_TRUE(timeline.timed());

  EXPECT_TRUE(CataloguePresentationTimeline::drawnAt(list.ops[0], 0));
  EXPECT_FALSE(CataloguePresentationTimeline::drawnAt(list.ops[1], 499));
  EXPECT_TRUE(CataloguePresentationTimeline::drawnAt(list.ops[1], 500));
  EXPECT_FALSE(CataloguePresentationTimeline::drawnAt(list.ops[3], 1199));
  EXPECT_TRUE(CataloguePresentationTimeline::drawnAt(list.ops[3], 1200));

  // Two operations of one stage are one change, not two.
  EXPECT_EQ(timeline.nextChangeAfter(0), std::optional<uint32_t>(500));
  EXPECT_EQ(timeline.nextChangeAfter(499), std::optional<uint32_t>(500));
  EXPECT_EQ(timeline.nextChangeAfter(500), std::optional<uint32_t>(1200));
  EXPECT_FALSE(timeline.nextChangeAfter(1200).has_value())
      << "a drawing with every stage on screen has nothing left to change";
  EXPECT_EQ(timeline.settledAt(), std::optional<uint32_t>(1200));
}

// The stages are read off the operations whatever order they are listed in:
// the lookup is over times, not list positions.
TEST(CataloguePresentationTimeline, StageTimesDoNotDependOnListOrder) {
  orc::CatalogueDisplayList list;
  list.ops = {opAt(900), opAt(0), opAt(300)};

  const CataloguePresentationTimeline timeline(list);
  EXPECT_EQ(timeline.nextChangeAfter(0), std::optional<uint32_t>(300));
  EXPECT_EQ(timeline.nextChangeAfter(300), std::optional<uint32_t>(900));
  EXPECT_EQ(timeline.settledAt(), std::optional<uint32_t>(900));
}

// An entry takes each step's colour from the step's time until the next one,
// and a track that does not loop holds its last step for good.
TEST(CataloguePresentationTimeline, ATrackGivesItsEntryTheColourOfTheTime) {
  orc::CatalogueDisplayList list;
  list.ops = {opAt(0)};
  list.palette = {kWhite, kBlue};
  list.palette_tracks.resize(2);
  list.palette_tracks[1] =
      holdingTrack({{0, kRed}, {300, kYellow}, {800, kBlue}});

  const CataloguePresentationTimeline timeline(list);
  EXPECT_TRUE(timeline.timed());

  const orc::CatalogueColour drawn = inEntry(kBlue, 1);
  EXPECT_EQ(rgb(timeline.colourAt(drawn, 0)), rgb(kRed));
  EXPECT_EQ(rgb(timeline.colourAt(drawn, 299)), rgb(kRed));
  EXPECT_EQ(rgb(timeline.colourAt(drawn, 300)), rgb(kYellow));
  EXPECT_EQ(rgb(timeline.colourAt(drawn, 800)), rgb(kBlue));
  EXPECT_EQ(rgb(timeline.colourAt(drawn, 60000)), rgb(kBlue));

  // An entry with no steps on its track keeps the colour drawn in it.
  EXPECT_EQ(rgb(timeline.colourAt(inEntry(kWhite, 0), 300)), rgb(kWhite));

  EXPECT_EQ(timeline.nextChangeAfter(0), std::optional<uint32_t>(300));
  EXPECT_EQ(timeline.nextChangeAfter(300), std::optional<uint32_t>(800));
  EXPECT_FALSE(timeline.nextChangeAfter(800).has_value());
  EXPECT_EQ(timeline.settledAt(), std::optional<uint32_t>(800));
}

// Only a colour that names an entry follows the entry. A plain value with the
// same RGB is a different colour that happens to look alike, and it holds.
TEST(CataloguePresentationTimeline, APlainColourDoesNotFollowAnEntry) {
  orc::CatalogueDisplayList list;
  list.palette = {kBlue};
  list.palette_tracks = {blinkTrack(kBlue, kYellow, 0, 400, 600)};

  const CataloguePresentationTimeline timeline(list);
  EXPECT_EQ(rgb(timeline.colourAt(inEntry(kBlue, 0), 100)), rgb(kYellow));
  EXPECT_EQ(rgb(timeline.colourAt(kBlue, 100)), rgb(kBlue));
}

// A colour naming an entry that has no track to follow — none at all, or one
// without steps — is its own still RGB, not whatever the palette holds.
TEST(CataloguePresentationTimeline, AnEntryWithoutATrackIsItsOwnStillColour) {
  orc::CatalogueDisplayList list;
  list.palette = {kWhite, kWhite, kWhite};
  // Entry 0 has an empty track, and entries 1 and 2 none at all.
  list.palette_tracks.resize(1);

  const CataloguePresentationTimeline timeline(list);
  EXPECT_FALSE(timeline.timed());
  EXPECT_EQ(rgb(timeline.colourAt(inEntry(kRed, 0), 100)), rgb(kRed));
  EXPECT_EQ(rgb(timeline.colourAt(inEntry(kRed, 2), 100)), rgb(kRed));
  EXPECT_EQ(rgb(timeline.colourAt(inEntry(kRed, 40), 100)), rgb(kRed))
      << "an entry past the end of the palette";
}

// A blink settles into a cycle once the page has drawn, and the cycle runs for
// as long as the page is shown: a time any number of periods on reads as the
// same point of the first pass.
TEST(CataloguePresentationTimeline, ALoopingTrackWrapsRoundItsPeriod) {
  orc::CatalogueDisplayList list;
  list.ops = {opAt(0), opAt(1000)};
  list.palette = {kBlue};
  // Blue while the page draws, then from 1000: yellow for 400, blue for 600.
  list.palette_tracks = {blinkTrack(kBlue, kYellow, 1000, 400, 600)};

  const CataloguePresentationTimeline timeline(list);
  const orc::CatalogueColour drawn = inEntry(kBlue, 0);

  EXPECT_EQ(rgb(timeline.colourAt(drawn, 999)), rgb(kBlue));
  EXPECT_EQ(rgb(timeline.colourAt(drawn, 1000)), rgb(kYellow));
  EXPECT_EQ(rgb(timeline.colourAt(drawn, 1399)), rgb(kYellow));
  EXPECT_EQ(rgb(timeline.colourAt(drawn, 1400)), rgb(kBlue));
  EXPECT_EQ(rgb(timeline.colourAt(drawn, 1999)), rgb(kBlue));
  // Round the loop.
  EXPECT_EQ(rgb(timeline.colourAt(drawn, 2000)), rgb(kYellow));
  EXPECT_EQ(rgb(timeline.colourAt(drawn, 2400)), rgb(kBlue));
  EXPECT_EQ(rgb(timeline.colourAt(drawn, 1000 + 1000 * 1000 + 100)),
            rgb(kYellow));

  // Each change of the cycle is a wake, including the one across the wrap.
  EXPECT_EQ(timeline.nextChangeAfter(0), std::optional<uint32_t>(1000));
  EXPECT_EQ(timeline.nextChangeAfter(1000), std::optional<uint32_t>(1400));
  EXPECT_EQ(timeline.nextChangeAfter(1400), std::optional<uint32_t>(2000));
  EXPECT_EQ(timeline.nextChangeAfter(1999), std::optional<uint32_t>(2000));
  EXPECT_EQ(timeline.nextChangeAfter(2000), std::optional<uint32_t>(2400));
  EXPECT_EQ(timeline.nextChangeAfter(2450), std::optional<uint32_t>(3000));

  EXPECT_FALSE(timeline.settledAt().has_value())
      << "a blink runs for as long as the page is shown";
}

// Entries blink on their own timing, so the next change is whichever of them —
// or of the stages still to draw — comes first.
TEST(CataloguePresentationTimeline, TheNextChangeIsTheEarliestOfAll) {
  orc::CatalogueDisplayList list;
  list.ops = {opAt(0), opAt(700)};
  list.palette = {kBlue, kWhite};
  list.palette_tracks = {blinkTrack(kBlue, kYellow, 0, 500, 500),
                         blinkTrack(kWhite, kBlack, 0, 300, 900)};

  const CataloguePresentationTimeline timeline(list);
  // Entry 1 turns back at 300, entry 0 at 500, the second stage lands at 700,
  // entry 0 turns again at 1000 and entry 1 at 1200.
  EXPECT_EQ(timeline.nextChangeAfter(0), std::optional<uint32_t>(300));
  EXPECT_EQ(timeline.nextChangeAfter(300), std::optional<uint32_t>(500));
  EXPECT_EQ(timeline.nextChangeAfter(500), std::optional<uint32_t>(700));
  EXPECT_EQ(timeline.nextChangeAfter(700), std::optional<uint32_t>(1000));
  EXPECT_EQ(timeline.nextChangeAfter(1000), std::optional<uint32_t>(1200));

  EXPECT_EQ(rgb(timeline.colourAt(inEntry(kBlue, 0), 600)), rgb(kBlue));
  EXPECT_EQ(rgb(timeline.colourAt(inEntry(kWhite, 1), 600)), rgb(kWhite));
}

// A track that stops changing settles the list no earlier than its last step,
// even when every stage has long been drawn.
TEST(CataloguePresentationTimeline, AHoldingTrackSettlesAtItsLastStep) {
  orc::CatalogueDisplayList list;
  list.ops = {opAt(0), opAt(700)};
  list.palette = {kBlue};
  list.palette_tracks = {holdingTrack({{0, kRed}, {2000, kBlue}})};

  const CataloguePresentationTimeline timeline(list);
  EXPECT_EQ(timeline.settledAt(), std::optional<uint32_t>(2000));
  EXPECT_EQ(timeline.nextChangeAfter(700), std::optional<uint32_t>(2000));
  EXPECT_FALSE(timeline.nextChangeAfter(2000).has_value());
}

// A period with no step inside the repeating part has nothing to repeat, so
// the track holds rather than keeping the view awake for nothing.
TEST(CataloguePresentationTimeline, ALoopWithNoStepInsideItHolds) {
  orc::CatalogueDisplayList list;
  list.palette = {kBlue};
  orc::CataloguePaletteTrack track = holdingTrack({{0, kRed}, {400, kBlue}});
  track.loop_start_ms = 500;
  track.loop_period_ms = 1000;
  list.palette_tracks = {track};

  const CataloguePresentationTimeline timeline(list);
  EXPECT_EQ(timeline.settledAt(), std::optional<uint32_t>(400));
  EXPECT_FALSE(timeline.nextChangeAfter(400).has_value());
}

// The SDK's times are 32-bit milliseconds. A change the arithmetic would put
// past the end of them is never reached, and must not read as one that
// already has been — a view acting on it would wake over and over.
TEST(CataloguePresentationTimeline, AChangePastTheEndOfTimeIsNeverDue) {
  orc::CatalogueDisplayList list;
  list.palette = {kBlue};
  list.palette_tracks = {blinkTrack(kBlue, kYellow, 0, 500, 500)};

  const CataloguePresentationTimeline timeline(list);
  // A whole number of periods, 1295 short of the end of the range, so the
  // next step, 500 on, is still inside it.
  const uint32_t near_the_end = 4294966000u;
  EXPECT_EQ(timeline.nextChangeAfter(near_the_end),
            std::optional<uint32_t>(near_the_end + 500));
  // Also a whole number of periods, but 295 short of the end, so the next
  // step would land past it.
  EXPECT_FALSE(timeline.nextChangeAfter(4294967000u).has_value());
}

// A list that settles has no cycle to go round; one that blinks repeats from
// the moment its last stage is up and its loops have all begun.
TEST(CataloguePresentationTimeline, ABlinkingListRepeatsFromItsLastStage) {
  orc::CatalogueDisplayList settled;
  settled.ops = {opAt(0), opAt(700)};
  EXPECT_FALSE(CataloguePresentationTimeline(settled).cycle().has_value());

  orc::CatalogueDisplayList list;
  list.ops = {opAt(0), opAt(1500)};
  list.palette = {kBlue};
  // Blue while the page draws, then from 1000: yellow for 400, blue for 600.
  list.palette_tracks = {blinkTrack(kBlue, kYellow, 1000, 400, 600)};

  const auto cycle = CataloguePresentationTimeline(list).cycle();
  ASSERT_TRUE(cycle.has_value());
  EXPECT_EQ(cycle->start_ms, 1500u)
      << "the last stage is still to come at 1000";
  EXPECT_EQ(cycle->period_ms, 1000u);
}

// Blinks with different periods only line up again after a period every one
// of them divides, and a held step taken late moves the start of the cycle on.
TEST(CataloguePresentationTimeline, BlinksGoRoundTogetherOverACommonPeriod) {
  orc::CatalogueDisplayList list;
  list.ops = {opAt(0)};
  list.palette = {kBlue, kWhite, kRed};
  list.palette_tracks = {blinkTrack(kBlue, kYellow, 0, 300, 300),
                         blinkTrack(kWhite, kBlack, 200, 500, 500),
                         holdingTrack({{0, kRed}, {900, kBlue}})};

  const auto cycle = CataloguePresentationTimeline(list).cycle();
  ASSERT_TRUE(cycle.has_value());
  EXPECT_EQ(cycle->start_ms, 900u);
  EXPECT_EQ(cycle->period_ms, 3000u) << "600 and 1000 meet every 3000";
}

// Periods that share no multiple within kLongestCycleMs give the cycle the
// longest of them instead, which every blink goes round at least once.
TEST(CataloguePresentationTimeline, BlinksThatNeverMeetTakeTheLongestPeriod) {
  orc::CatalogueDisplayList list;
  list.ops = {opAt(0)};
  list.palette = {kBlue, kWhite};
  // 7,001 and 9,973 ms: both prime, so they meet only every 69.8 million.
  list.palette_tracks = {blinkTrack(kBlue, kYellow, 0, 3000, 4001),
                         blinkTrack(kWhite, kBlack, 0, 4973, 5000)};

  const auto cycle = CataloguePresentationTimeline(list).cycle();
  ASSERT_TRUE(cycle.has_value());
  EXPECT_EQ(cycle->period_ms, 9973u);
}

// The display on screen went up with the latest change at or before the time:
// a stage, or a step of a blink counted round its loop as often as it has
// gone. Stepping back through a presentation reads it.
TEST(CataloguePresentationTimeline, TheDisplayWentUpWithTheLatestChange) {
  orc::CatalogueDisplayList list;
  list.ops = {opAt(0), opAt(500), opAt(1200)};
  list.palette = {kBlue};
  // From the start: yellow for 600, blue for 400, round and round.
  list.palette_tracks = {blinkTrack(kBlue, kYellow, 0, 600, 400)};

  const CataloguePresentationTimeline timeline(list);
  EXPECT_EQ(timeline.changeAtOrBefore(0), 0u);
  EXPECT_EQ(timeline.changeAtOrBefore(499), 0u);
  EXPECT_EQ(timeline.changeAtOrBefore(500), 500u);
  EXPECT_EQ(timeline.changeAtOrBefore(650), 600u);
  EXPECT_EQ(timeline.changeAtOrBefore(1100), 1000u)
      << "the loop's first step, taken again a period on";
  EXPECT_EQ(timeline.changeAtOrBefore(1250), 1200u);
  EXPECT_EQ(timeline.changeAtOrBefore(10650), 10600u);

  // A still list has had no change since the start.
  orc::CatalogueDisplayList still;
  still.ops = {opAt(0)};
  EXPECT_EQ(CataloguePresentationTimeline(still).changeAtOrBefore(5000), 0u);
}

namespace {

/// A filled rectangle over the whole of a 256 by 200 list's display area,
/// swept onto the screen over |reveal_ms| before |appear_ms|, from unit y
/// |from| to |to|.
orc::CatalogueDrawOp sweptAt(uint32_t appear_ms, uint32_t reveal_ms,
                             double from, double to) {
  orc::CatalogueDrawOp op = opAt(appear_ms);
  op.origin = orc::CataloguePoint{0.0, 0.0};
  op.size = orc::CatalogueSize{1.0, 0.78125};
  op.reveal_duration_ms = reveal_ms;
  op.reveal_from_y = from;
  op.reveal_to_y = to;
  return op;
}

/// A list on the reference receiver's grid, 256 by 200 over unit y 0.78125.
orc::CatalogueDisplayList referenceList() {
  orc::CatalogueDisplayList list;
  list.aspect_height = 0.78125;
  list.nominal_width = 256;
  list.nominal_height = 200;
  return list;
}

/// A row of the reference grid in unit y.
constexpr double kRow = 0.78125 / 200.0;

}  // namespace

// A figure its source drew a row at a time is on screen from the start of its
// sweep, and whole from its appear time; before the sweep starts it is not
// drawn at all.
TEST(CataloguePresentationTimeline, ASweptOperationIsDrawnFromItsSweepsStart) {
  orc::CatalogueDisplayList list = referenceList();
  list.ops = {sweptAt(1000, 200, 0.78125, 0.0)};
  const CataloguePresentationTimeline timeline(list);
  const orc::CatalogueDrawOp& op = list.ops[0];

  EXPECT_EQ(CataloguePresentationTimeline::revealStart(op), 800u);
  EXPECT_FALSE(CataloguePresentationTimeline::drawnAt(op, 799));
  EXPECT_TRUE(CataloguePresentationTimeline::drawnAt(op, 800));
  EXPECT_FALSE(timeline.revealLineAt(op, 799).has_value());
  EXPECT_TRUE(timeline.revealLineAt(op, 800).has_value());
  EXPECT_TRUE(timeline.revealLineAt(op, 999).has_value());
  EXPECT_FALSE(timeline.revealLineAt(op, 1000).has_value())
      << "whole from its appear time";
}

// The sweep moves on once a field of NTSC, 1001/60 ms, and a whole row of the
// grid at a time: 200 rows over 200 ms is a row a millisecond, so the line
// stands 16 rows down at the first field and 33 at the second.
TEST(CataloguePresentationTimeline, ASweepMovesOnAFieldAtATimeByWholeRows) {
  orc::CatalogueDisplayList list = referenceList();
  list.ops = {sweptAt(1000, 200, 0.78125, 0.0)};
  const CataloguePresentationTimeline timeline(list);
  const std::vector<CataloguePresentationTimeline::RevealStep> steps =
      timeline.revealSteps(list.ops[0]);

  const std::vector<uint32_t> fields{800, 816, 833, 850, 866, 883,
                                     900, 916, 933, 950, 966, 983};
  ASSERT_EQ(steps.size(), fields.size());
  for (size_t i = 0; i < steps.size(); ++i) {
    EXPECT_EQ(steps[i].time_ms, fields[i]) << i;
    const double rows = (0.78125 - steps[i].line_y) / kRow;
    EXPECT_NEAR(rows, std::round(rows), 1e-9) << "a whole row, at " << i;
    EXPECT_NEAR(rows, std::floor((fields[i] - 800.0) + 1e-6), 1e-9) << i;
  }
  // Between two fields the line stays where the first left it.
  EXPECT_EQ(timeline.revealLineAt(list.ops[0], 830),
            std::optional<double>(steps[1].line_y));
  EXPECT_EQ(timeline.revealLineAt(list.ops[0], 833),
            std::optional<double>(steps[2].line_y));
}

// A sweep up the screen starts from the bottom of its band.
TEST(CataloguePresentationTimeline, ASweepUpStartsAtTheBottom) {
  orc::CatalogueDisplayList list = referenceList();
  list.ops = {sweptAt(500, 100, 0.25, 0.5)};
  const CataloguePresentationTimeline timeline(list);
  const auto steps = timeline.revealSteps(list.ops[0]);
  ASSERT_FALSE(steps.empty());
  EXPECT_DOUBLE_EQ(steps.front().line_y, 0.25);
  for (size_t i = 1; i < steps.size(); ++i) {
    EXPECT_GT(steps[i].line_y, steps[i - 1].line_y) << i;
    EXPECT_LT(steps[i].line_y, 0.5) << i;
  }
}

// Each field of a sweep is a change of the screen, which the view wakes for
// and the transport steps through; the operation settles at its appear time.
TEST(CataloguePresentationTimeline, EachFieldOfASweepIsAChange) {
  orc::CatalogueDisplayList list = referenceList();
  list.ops = {opAt(0), sweptAt(1000, 200, 0.78125, 0.0), opAt(1500)};
  const CataloguePresentationTimeline timeline(list);

  EXPECT_EQ(timeline.nextChangeAfter(0), std::optional<uint32_t>(816))
      << "the sweep's start shows nothing of it yet";
  EXPECT_EQ(timeline.nextChangeAfter(816), std::optional<uint32_t>(833));
  EXPECT_EQ(timeline.nextChangeAfter(983), std::optional<uint32_t>(1000));
  EXPECT_EQ(timeline.nextChangeAfter(1000), std::optional<uint32_t>(1500));
  EXPECT_EQ(timeline.changeAtOrBefore(840), 833u);
  EXPECT_EQ(timeline.changeAtOrBefore(1200), 1000u);
  EXPECT_EQ(timeline.settledAt(), std::optional<uint32_t>(1500));
}

// The runs of one figure share its sweep, and a sweep is read once for all of
// them: the changes are the same as for one operation.
TEST(CataloguePresentationTimeline, OperationsSharingASweepChangeTogether) {
  orc::CatalogueDisplayList list = referenceList();
  for (int row = 0; row < 50; ++row) {
    orc::CatalogueDrawOp run = sweptAt(1000, 200, 50 * kRow, 0.0);
    run.origin = orc::CataloguePoint{0.0, row * kRow};
    run.size = orc::CatalogueSize{1.0, kRow};
    list.ops.push_back(run);
  }
  const CataloguePresentationTimeline timeline(list);
  const CataloguePresentationTimeline alone([&] {
    orc::CatalogueDisplayList one = referenceList();
    one.ops = {list.ops.front()};
    return one;
  }());
  for (uint32_t t = 0; t < 1100; t += 7) {
    EXPECT_EQ(timeline.nextChangeAfter(t), alone.nextChangeAfter(t)) << t;
  }
}

// A list that names no grid sweeps by the fraction of its band the reveal has
// gone through, unsnapped.
TEST(CataloguePresentationTimeline, WithoutAGridASweepIsNotSnappedToRows) {
  orc::CatalogueDisplayList list;
  list.ops = {sweptAt(1000, 100, 1.0, 0.0)};
  const CataloguePresentationTimeline timeline(list);
  const auto line = timeline.revealLineAt(list.ops[0], 950);
  ASSERT_TRUE(line.has_value());
  // The field in force at 950 ms began 50 ms into the sweep's 100.
  EXPECT_DOUBLE_EQ(*line, 0.5);
}

}  // namespace gui_unit_test
