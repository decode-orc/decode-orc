/*
 * File:        naplps_palette_timeline_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NAPLPS palette timeline: map writes over
 * time, blink processes, and the loops they settle into
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "naplps_palette_timeline.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "naplps_interpreter.h"

namespace orc {
namespace {

// Colours a test can tell apart at a glance.
constexpr NabtsColour kRed{0, 15, 0, false};
constexpr NabtsColour kGreen{15, 0, 0, false};
constexpr NabtsColour kBlue{0, 0, 15, false};
constexpr NabtsColour kYellow{15, 15, 0, false};

/// A timeline over the default map, which tests then add events to.
NabtsPresentationTimeline default_timeline() {
  NabtsPresentationTimeline timeline;
  nabts_default_colour_map(timeline.initial_map);
  return timeline;
}

void write(NabtsPresentationTimeline& timeline, int64_t time, uint8_t address,
           const NabtsColour& colour) {
  NabtsPaletteEvent event;
  event.kind = NabtsPaletteEvent::Kind::kMapWrite;
  event.time_ms = time;
  event.address = address;
  event.colour = colour;
  timeline.events.push_back(event);
}

/// Define a process that starts its first ON interval at |first_on|.
uint32_t blink(NabtsPresentationTimeline& timeline, int64_t defined,
               int64_t first_on, uint8_t from, int16_t to, uint32_t on,
               uint32_t off) {
  NabtsBlinkProcess process;
  process.from = from;
  process.to_address = to;
  process.on_ms = on;
  process.off_ms = off;
  process.defined_ms = defined;
  process.first_on_ms = first_on;
  const auto index = static_cast<uint32_t>(timeline.blinks.size());
  timeline.blinks.push_back(process);
  NabtsPaletteEvent event;
  event.kind = NabtsPaletteEvent::Kind::kBlinkStart;
  event.time_ms = defined;
  event.process = index;
  timeline.events.push_back(event);
  return index;
}

void end(NabtsPresentationTimeline& timeline, int64_t time, uint32_t index) {
  timeline.blinks[index].ended_ms = time;
  NabtsPaletteEvent event;
  event.kind = NabtsPaletteEvent::Kind::kBlinkEnd;
  event.time_ms = time;
  event.process = index;
  timeline.events.push_back(event);
}

/// The colour a track gives at |time|, the way CataloguePaletteTrack reads it.
NabtsColour colour_at(const NaplpsPaletteTrack& track, uint32_t time) {
  uint32_t t = time;
  if (track.loop_period_ms != 0 &&
      time >= track.loop_start_ms + track.loop_period_ms) {
    t = track.loop_start_ms +
        (time - track.loop_start_ms) % track.loop_period_ms;
  }
  NabtsColour at = track.steps.front().colour;
  for (const NaplpsColourStep& step : track.steps) {
    if (step.time_ms > t) {
      break;
    }
    at = step.colour;
  }
  return at;
}

TEST(NaplpsPaletteTimeline, AMapNothingTouchesHasNoTracks) {
  const NaplpsPaletteTimeline palette =
      naplps_palette_timeline(default_timeline());
  EXPECT_FALSE(palette.changes());
  EXPECT_FALSE(palette.approximated);
}

// Writes made before anything is shown are simply the colours the page is
// shown in: the final map already says so, and there is nothing to play.
TEST(NaplpsPaletteTimeline, WritesAtTheStartAreNotAChange) {
  NabtsPresentationTimeline timeline = default_timeline();
  write(timeline, 0, 3, kRed);
  write(timeline, 0, 3, kBlue);
  EXPECT_FALSE(naplps_palette_timeline(timeline).changes());
}

// §5.3.2.5: a write is "immediately reflected in the color of all pixels whose
// associated color map address points to the color map entry that has been
// changed" — so a write after a pause changes the entry then.
TEST(NaplpsPaletteTimeline, AWriteAfterAPauseChangesTheEntryThen) {
  NabtsPresentationTimeline timeline = default_timeline();
  write(timeline, 0, 3, kRed);
  write(timeline, 1500, 3, kBlue);
  timeline.end_ms = 1500;

  const NaplpsPaletteTimeline palette = naplps_palette_timeline(timeline);
  const NaplpsPaletteTrack& track = palette.tracks[3];
  ASSERT_EQ(track.steps.size(), 2u);
  EXPECT_EQ(track.steps[0].time_ms, 0u);
  EXPECT_EQ(track.steps[0].colour, kRed);
  EXPECT_EQ(track.steps[1].time_ms, 1500u);
  EXPECT_EQ(track.steps[1].colour, kBlue);
  EXPECT_EQ(track.loop_period_ms, 0u) << "nothing about a write repeats";
  for (size_t entry = 0; entry < kNabtsColourMapEntries; ++entry) {
    if (entry != 3) {
      EXPECT_FALSE(palette.tracks[entry].changes()) << "entry " << entry;
    }
  }
}

// §5.3.2.7.2: the blink-to colour for the ON interval, the blink-from colour
// for the OFF interval, "starting with the ON interval" — and on for as long
// as the page is shown, which is a loop one period long.
TEST(NaplpsPaletteTimeline, ABlinkLoopsBetweenItsTwoColours) {
  NabtsPresentationTimeline timeline = default_timeline();
  write(timeline, 0, 3, kRed);
  write(timeline, 0, 6, kYellow);
  blink(timeline, 0, 0, 3, 6, 400, 600);

  const NaplpsPaletteTimeline palette = naplps_palette_timeline(timeline);
  const NaplpsPaletteTrack& track = palette.tracks[3];
  ASSERT_TRUE(track.changes());
  EXPECT_EQ(track.loop_period_ms, 1000u);
  EXPECT_EQ(colour_at(track, 0), kYellow);
  EXPECT_EQ(colour_at(track, 399), kYellow);
  EXPECT_EQ(colour_at(track, 400), kRed);
  EXPECT_EQ(colour_at(track, 999), kRed);
  EXPECT_EQ(colour_at(track, 1000), kYellow);
  // An hour on it is still in step.
  EXPECT_EQ(colour_at(track, 3600000 + 450), kRed);
  EXPECT_EQ(colour_at(track, 3600000 + 50), kYellow);
}

// A start delay puts the first ON interval off; until then the entry keeps
// its own colour.
TEST(NaplpsPaletteTimeline, ABlinkWaitsForItsFirstOnInterval) {
  NabtsPresentationTimeline timeline = default_timeline();
  write(timeline, 0, 3, kRed);
  write(timeline, 0, 6, kYellow);
  blink(timeline, 0, 500, 3, 6, 400, 600);

  const NaplpsPaletteTimeline palette = naplps_palette_timeline(timeline);
  const NaplpsPaletteTrack& track = palette.tracks[3];
  EXPECT_EQ(colour_at(track, 0), kRed);
  EXPECT_EQ(colour_at(track, 499), kRed);
  EXPECT_EQ(colour_at(track, 500), kYellow);
  EXPECT_EQ(colour_at(track, 900), kRed);
  EXPECT_EQ(colour_at(track, 1500), kYellow);
}

// §5.3.2.7.4: "The original blink-from color shall be restored ... when all
// blink processes using that blink-from color are terminated" — even part way
// through an ON interval — and nothing loops once none is running.
TEST(NaplpsPaletteTimeline, EndingTheLastProcessRestoresTheEntry) {
  NabtsPresentationTimeline timeline = default_timeline();
  write(timeline, 0, 3, kRed);
  write(timeline, 0, 6, kYellow);
  const uint32_t process = blink(timeline, 0, 0, 3, 6, 400, 600);
  end(timeline, 1200, process);
  timeline.end_ms = 1200;

  const NaplpsPaletteTimeline palette = naplps_palette_timeline(timeline);
  const NaplpsPaletteTrack& track = palette.tracks[3];
  EXPECT_EQ(colour_at(track, 1100), kYellow);
  EXPECT_EQ(colour_at(track, 1200), kRed);
  EXPECT_EQ(colour_at(track, 100000), kRed);
  EXPECT_EQ(track.loop_period_ms, 0u);
}

// "(unless it has been changed explicitly by a SET COLOR command)": what is
// restored is the entry's colour as last written.
TEST(NaplpsPaletteTimeline, ARestoredEntryTakesItsLatestWrite) {
  NabtsPresentationTimeline timeline = default_timeline();
  write(timeline, 0, 3, kRed);
  write(timeline, 0, 6, kYellow);
  const uint32_t process = blink(timeline, 0, 0, 3, 6, 400, 600);
  write(timeline, 1100, 3, kBlue);
  end(timeline, 1200, process);
  timeline.end_ms = 1200;

  const NaplpsPaletteTimeline palette = naplps_palette_timeline(timeline);
  const NaplpsPaletteTrack& track = palette.tracks[3];
  EXPECT_EQ(colour_at(track, 1100), kBlue)
      << "a write puts the entry's colour in whatever interval it lands";
  EXPECT_EQ(colour_at(track, 1200), kBlue);
}

// §5.3.2.7.2: "If multiple blink processes have ON or OFF intervals that expire
// simultaneously, they are processed sequentially starting with the most
// recently defined blink process and ending with the least recently defined" —
// so of two processes blinking one entry to different colours, the older one
// has the last word.
TEST(NaplpsPaletteTimeline, SimultaneousProcessesRunNewestFirst) {
  NabtsPresentationTimeline timeline = default_timeline();
  write(timeline, 0, 3, kRed);
  write(timeline, 0, 5, kGreen);
  write(timeline, 0, 6, kYellow);
  blink(timeline, 0, 0, 3, 5, 400, 600);  // older
  blink(timeline, 0, 0, 3, 6, 400, 600);  // newer

  const NaplpsPaletteTimeline palette = naplps_palette_timeline(timeline);
  const NaplpsPaletteTrack& track = palette.tracks[3];
  EXPECT_EQ(colour_at(track, 1000), kGreen);
  EXPECT_EQ(colour_at(track, 1400), kRed);
  EXPECT_EQ(colour_at(track, 5000), kGreen);
}

// "each blink process takes as its input the color map that resulted from the
// previously executed blink process" — a process copying from an entry that a
// newer process has just blinked copies the blinked colour.
TEST(NaplpsPaletteTimeline, AProcessCopiesWhatANewerOneJustWrote) {
  NabtsPresentationTimeline timeline = default_timeline();
  write(timeline, 0, 3, kRed);
  write(timeline, 0, 5, kGreen);
  write(timeline, 0, 6, kYellow);
  blink(timeline, 0, 0, 3, 5, 400, 600);  // older: 3 takes 5
  blink(timeline, 0, 0, 5, 6, 400, 600);  // newer: 5 takes 6

  const NaplpsPaletteTimeline palette = naplps_palette_timeline(timeline);
  EXPECT_EQ(colour_at(palette.tracks[5], 1000), kYellow);
  EXPECT_EQ(colour_at(palette.tracks[3], 1000), kYellow)
      << "3 copied 5 before the newer process had written it";
}

// The first ON intervals of processes defined together start together, and
// run newest first like any others — as the Norpak TTX 650 runs them — so the
// older process copies what the newer one has just written from the start.
TEST(NaplpsPaletteTimeline, ProcessesDefinedTogetherStartNewestFirst) {
  NabtsPresentationTimeline timeline = default_timeline();
  write(timeline, 0, 3, kRed);
  write(timeline, 0, 5, kGreen);
  write(timeline, 0, 6, kYellow);
  blink(timeline, 0, 0, 3, 5, 400, 600);  // older: 3 takes 5
  blink(timeline, 0, 0, 5, 6, 400, 600);  // newer: 5 takes 6

  const NaplpsPaletteTimeline palette = naplps_palette_timeline(timeline);
  EXPECT_EQ(colour_at(palette.tracks[5], 0), kYellow);
  EXPECT_EQ(colour_at(palette.tracks[3], 0), kYellow);
}

// Processes that copy between their entries cycle together, so they loop
// together, over the least common multiple of their periods.
TEST(NaplpsPaletteTimeline, JoinedProcessesLoopOverTheirCommonMultiple) {
  NabtsPresentationTimeline timeline = default_timeline();
  write(timeline, 0, 3, kRed);
  write(timeline, 0, 5, kGreen);
  write(timeline, 0, 6, kYellow);
  blink(timeline, 0, 0, 3, 5, 100, 200);  // period 300
  blink(timeline, 0, 0, 5, 6, 200, 300);  // period 500

  const NaplpsPaletteTimeline palette = naplps_palette_timeline(timeline);
  EXPECT_EQ(palette.tracks[3].loop_period_ms, 1500u);
  EXPECT_EQ(palette.tracks[5].loop_period_ms, 1500u);
  EXPECT_FALSE(palette.approximated);
}

// Processes with nothing between them loop on their own periods.
TEST(NaplpsPaletteTimeline, IndependentBlinksLoopSeparately) {
  NabtsPresentationTimeline timeline = default_timeline();
  write(timeline, 0, 3, kRed);
  write(timeline, 0, 9, kBlue);
  blink(timeline, 0, 0, 3, 6, 400, 600);
  blink(timeline, 0, 0, 9, 6, 300, 1000);

  const NaplpsPaletteTimeline palette = naplps_palette_timeline(timeline);
  EXPECT_EQ(palette.tracks[3].loop_period_ms, 1000u);
  EXPECT_EQ(palette.tracks[9].loop_period_ms, 1300u);
}

// A looping track has a step exactly where its loop starts, which is what a
// renderer folding time back into the loop lands on.
TEST(NaplpsPaletteTimeline, ALoopStartsOnAStep) {
  NabtsPresentationTimeline timeline = default_timeline();
  write(timeline, 0, 3, kRed);
  write(timeline, 0, 6, kYellow);
  write(timeline, 2250, 6, kBlue);
  blink(timeline, 0, 0, 3, 6, 400, 600);
  timeline.end_ms = 2250;

  const NaplpsPaletteTimeline palette = naplps_palette_timeline(timeline);
  const NaplpsPaletteTrack& track = palette.tracks[3];
  ASSERT_NE(track.loop_period_ms, 0u);
  EXPECT_GE(track.loop_start_ms, 2250u) << "the loop began before the page "
                                           "had finished changing the map";
  bool on_step = false;
  for (const NaplpsColourStep& step : track.steps) {
    on_step = on_step || step.time_ms == track.loop_start_ms;
    EXPECT_LT(step.time_ms, track.loop_start_ms + track.loop_period_ms);
  }
  EXPECT_TRUE(on_step);
  // After the write the process blinks to the new colour.
  EXPECT_EQ(colour_at(track, 3000), kBlue);
  EXPECT_EQ(colour_at(track, 3500), kRed);
}

// A process carried in from an earlier record keeps its phase: defined 1.2 s
// before this record began with a one-second period, it is 0.2 s into its
// cycle when the record starts.
TEST(NaplpsPaletteTimeline, ACarriedProcessKeepsItsPhase) {
  NabtsPresentationTimeline timeline = default_timeline();
  timeline.initial_map[3] = kRed;
  timeline.initial_map[6] = kYellow;
  blink(timeline, -1200, -1200, 3, 6, 400, 600);

  const NaplpsPaletteTimeline palette = naplps_palette_timeline(timeline);
  const NaplpsPaletteTrack& track = palette.tracks[3];
  EXPECT_EQ(colour_at(track, 0), kYellow);
  EXPECT_EQ(colour_at(track, 199), kYellow);
  EXPECT_EQ(colour_at(track, 200), kRed);
  EXPECT_EQ(colour_at(track, 800), kYellow);
}

// A blink-to colour that is not an entry (§6.2.8.1's nominal black).
TEST(NaplpsPaletteTimeline, ABlinkToAColourUsesTheColour) {
  NabtsPresentationTimeline timeline = default_timeline();
  write(timeline, 0, 3, kRed);
  const uint32_t process = blink(timeline, 0, 0, 3, -1, 300, 1000);
  timeline.blinks[process].to_colour = kNabtsNominalBlack;

  const NaplpsPaletteTimeline palette = naplps_palette_timeline(timeline);
  const NaplpsPaletteTrack& track = palette.tracks[3];
  EXPECT_EQ(colour_at(track, 100), kNabtsNominalBlack);
  EXPECT_EQ(colour_at(track, 300), kRed);
  EXPECT_EQ(track.loop_period_ms, 1300u);
}

// Periods whose common multiple is longer than a loop may be are cut to the
// fastest that fit, and the timeline says so.
TEST(NaplpsPaletteTimeline, AnOverlongLoopIsApproximated) {
  NabtsPresentationTimeline timeline = default_timeline();
  write(timeline, 0, 3, kRed);
  // Periods of 6.1, 6.3 and 5.9 s, joined through entry 3: 61 x 63 x 59
  // tenths is far past ten minutes.
  blink(timeline, 0, 0, 3, 4, 3000, 3100);
  blink(timeline, 0, 0, 4, 3, 3000, 3300);
  blink(timeline, 0, 0, 3, 5, 2900, 3000);

  const NaplpsPaletteTimeline palette = naplps_palette_timeline(timeline);
  EXPECT_TRUE(palette.approximated);
  EXPECT_LE(palette.tracks[3].loop_period_ms, kNaplpsMaxLoopMs);
}

// End to end: the interpreter's timeline for a record that blinks, run through
// the evaluator — the "TELETEXT is Animation!" shape of a figure twinkling
// between two map entries, and the blink-to entry written only after the
// process started.
TEST(NaplpsPaletteTimeline, ARecordThatBlinksPlaysThroughTheEvaluator) {
  const auto numeric = [](uint8_t payload) {
    return static_cast<uint8_t>(0x40 | (payload & 0x3F));
  };
  const std::vector<uint8_t> record = {
      0x0E,  // SO: the PDI set
      static_cast<uint8_t>(NaplpsPdi::kDomain), numeric(0),
      static_cast<uint8_t>(NaplpsPdi::kSelectColour), numeric(0b001100),
      static_cast<uint8_t>(NaplpsPdi::kSetColour), numeric(0b000111),
      static_cast<uint8_t>(NaplpsPdi::kBlink), numeric(0b011000), numeric(4),
      numeric(6), numeric(0),
      // Only now is entry 6 given its colour.
      static_cast<uint8_t>(NaplpsPdi::kSelectColour), numeric(0b011000),
      static_cast<uint8_t>(NaplpsPdi::kSetColour), numeric(0b110000)};

  NaplpsInterpreter interpreter;
  const NabtsPageSnapshot snapshot = interpreter.run(record);
  const NaplpsPaletteTimeline palette =
      naplps_palette_timeline(snapshot.timeline);
  const NaplpsPaletteTrack& track = palette.tracks[3];
  ASSERT_TRUE(track.changes());
  // From the second ON interval on, entry 3 takes entry 6's new colour: the
  // process "substitutes the current contents of another entry", read at the
  // start of each ON interval.
  EXPECT_EQ(colour_at(track, 1100), snapshot.colour_map[6]);
  EXPECT_EQ(colour_at(track, 1500), snapshot.colour_map[3]);
  EXPECT_EQ(colour_at(track, 10100), snapshot.colour_map[6]);
}

}  // namespace
}  // namespace orc
