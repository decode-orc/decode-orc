/*
 * File:        catalogue_playback_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests for the transport a timed display list is played
 *              with
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "catalogue_playback.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace gui_unit_test {
namespace {

/// A clock the test moves by hand, so playback never waits on real time.
class HandClock : public ICatalogueAnimationClock {
 public:
  int64_t nowMs() const override { return now_ms; }

  int64_t now_ms = 0;
};

const orc::CatalogueColour kWhite{255, 255, 255};
const orc::CatalogueColour kBlack{0, 0, 0};

// The staged page: stages at 0, kSecondStageMs and kLastStageMs, nothing
// after.
constexpr uint32_t kSecondStageMs = 800;
constexpr uint32_t kLastStageMs = 2000;
constexpr uint32_t kStagedLengthMs =
    kLastStageMs + CataloguePlayback::kFinalHoldMs;

// The blinking page: a first stage, a second at kSecondStageMs, and an entry
// that blinks from the start with a period of kBlinkPeriodMs — so from the
// second stage on the whole screen repeats.
constexpr uint32_t kBlinkOnMs = 600;
constexpr uint32_t kBlinkPeriodMs = 1000;
constexpr uint32_t kBlinkingLengthMs = kSecondStageMs + kBlinkPeriodMs;

orc::CatalogueDrawOp opAt(uint32_t appear_ms) {
  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kRectangle;
  op.filled = true;
  op.colour = kWhite;
  op.appear_ms = appear_ms;
  return op;
}

CataloguePresentationTimeline stagedTimeline() {
  orc::CatalogueDisplayList list;
  list.ops = {opAt(0), opAt(kSecondStageMs), opAt(kLastStageMs)};
  return CataloguePresentationTimeline(list);
}

CataloguePresentationTimeline blinkingTimeline() {
  orc::CatalogueDisplayList list;
  list.ops = {opAt(0), opAt(kSecondStageMs)};
  list.palette = {kWhite};
  orc::CataloguePaletteTrack track;
  track.steps = {orc::CatalogueColourStep{0, kWhite},
                 orc::CatalogueColourStep{kBlinkOnMs, kBlack}};
  track.loop_start_ms = 0;
  track.loop_period_ms = kBlinkPeriodMs;
  list.palette_tracks = {track};
  return CataloguePresentationTimeline(list);
}

}  // namespace

// A list is played from its start as soon as it is taken up, at the clock's
// own rate; only differences between clock readings mean anything.
TEST(CataloguePlayback, PlaysFromTheStartAtTheClocksRate) {
  auto clock = std::make_shared<HandClock>();
  clock->now_ms = 90000;
  CataloguePlayback playback(clock);
  playback.load(stagedTimeline());

  EXPECT_EQ(playback.time(), 0u);
  EXPECT_TRUE(playback.playing());
  clock->now_ms += 300;
  EXPECT_EQ(playback.time(), 300u);
}

// A drawing that settles is held finished for kFinalHoldMs, and then it is
// over: the transport stops there, and playing it again starts it afresh.
TEST(CataloguePlayback, ASettlingPresentationStopsAtItsEnd) {
  auto clock = std::make_shared<HandClock>();
  CataloguePlayback playback(clock);
  playback.load(stagedTimeline());
  EXPECT_EQ(playback.length(), kStagedLengthMs);
  EXPECT_FALSE(playback.cycleStart().has_value());

  clock->now_ms += kLastStageMs;
  EXPECT_TRUE(playback.playing()) << "the finished drawing is held a while";
  clock->now_ms += 60000;
  EXPECT_EQ(playback.time(), kStagedLengthMs);
  EXPECT_FALSE(playback.playing());
  EXPECT_FALSE(playback.nextJumpBack().has_value());

  playback.play();
  EXPECT_EQ(playback.time(), 0u) << "played again once over, it starts again";
  EXPECT_TRUE(playback.playing());
}

// Looping, the presentation starts again from its start each time it ends,
// for as long as it is played.
TEST(CataloguePlayback, ALoopingPresentationStartsAgain) {
  auto clock = std::make_shared<HandClock>();
  CataloguePlayback playback(clock);
  playback.load(stagedTimeline());
  playback.setLooping(true);
  EXPECT_EQ(playback.nextJumpBack(), std::optional<uint32_t>(kStagedLengthMs));

  clock->now_ms += 3 * kStagedLengthMs + 250;
  EXPECT_EQ(playback.time(), 250u);
  EXPECT_TRUE(playback.playing());
}

// Not looping, a page that blinks is held as a receiver held it: it goes round
// its cycle from the moment its last stage is up, for as long as it is shown,
// and never stops.
TEST(CataloguePlayback, ABlinkingPresentationGoesRoundItsCycle) {
  auto clock = std::make_shared<HandClock>();
  CataloguePlayback playback(clock);
  playback.load(blinkingTimeline());
  EXPECT_EQ(playback.length(), kBlinkingLengthMs);
  EXPECT_EQ(playback.cycleStart(), std::optional<uint32_t>(kSecondStageMs));
  EXPECT_EQ(playback.nextJumpBack(),
            std::optional<uint32_t>(kBlinkingLengthMs));

  clock->now_ms += kBlinkingLengthMs;
  EXPECT_EQ(playback.time(), kSecondStageMs)
      << "the end of the cycle is its start again";
  clock->now_ms += 5 * kBlinkPeriodMs + 300;
  EXPECT_EQ(playback.time(), kSecondStageMs + 300);
  EXPECT_TRUE(playback.playing());

  // Looping, it goes back to the first stage instead.
  playback.setLooping(true);
  clock->now_ms += kBlinkingLengthMs - (kSecondStageMs + 300) + 100;
  EXPECT_EQ(playback.time(), 100u);
}

// Paused, the presentation stays where it was whatever the clock does, and
// played again it goes on from there.
TEST(CataloguePlayback, PausingHoldsWhereTheClockHadGot) {
  auto clock = std::make_shared<HandClock>();
  CataloguePlayback playback(clock);
  playback.load(stagedTimeline());

  clock->now_ms += 300;
  playback.pause();
  EXPECT_FALSE(playback.playing());
  EXPECT_FALSE(playback.nextJumpBack().has_value());
  clock->now_ms += 5000;
  EXPECT_EQ(playback.time(), 300u);

  playback.play();
  clock->now_ms += 200;
  EXPECT_EQ(playback.time(), 500u);
}

// A seek moves the presentation and leaves it playing or held as it was, as
// dragging a slider does.
TEST(CataloguePlayback, ASeekKeepsThePresentationPlayingOrHeld) {
  auto clock = std::make_shared<HandClock>();
  CataloguePlayback playback(clock);
  playback.load(stagedTimeline());

  playback.seek(1500);
  clock->now_ms += 100;
  EXPECT_EQ(playback.time(), 1600u);
  EXPECT_TRUE(playback.playing());

  playback.pause();
  playback.seek(400);
  clock->now_ms += 100;
  EXPECT_EQ(playback.time(), 400u);
  EXPECT_FALSE(playback.playing());

  // Held at its very end, a presentation stays there, even one that loops.
  playback.setLooping(true);
  playback.seek(kStagedLengthMs);
  EXPECT_EQ(playback.time(), kStagedLengthMs);
}

// A time past the end is where playing on from the start would be by then:
// round the cycle of a page that blinks, and the end of one that settles.
TEST(CataloguePlayback, ASeekPastTheEndLandsWherePlayingOnWould) {
  auto clock = std::make_shared<HandClock>();
  CataloguePlayback playback(clock);

  playback.load(blinkingTimeline());
  playback.pause();
  playback.seek(5 * kBlinkPeriodMs + kBlinkOnMs + 10);
  // 5,610 ms is 4,810 into the cycle that starts at 800, which is 810 into
  // its fifth time round: the same moment of the blink, 610 into its period.
  EXPECT_EQ(playback.time(), 1610u);

  playback.load(stagedTimeline());
  playback.pause();
  playback.seek(10 * kStagedLengthMs);
  EXPECT_EQ(playback.time(), kStagedLengthMs);
}

// The speed scales the clock's run and nothing else: where the presentation
// stands does not jump when it changes, and the clock time a stretch of the
// presentation takes scales with it, rounded up so a wake is never early.
TEST(CataloguePlayback, TheSpeedScalesTheClock) {
  auto clock = std::make_shared<HandClock>();
  CataloguePlayback playback(clock);
  playback.load(stagedTimeline());

  clock->now_ms += 100;
  playback.setSpeed(2.0);
  EXPECT_EQ(playback.time(), 100u) << "changing speed moved the presentation";
  clock->now_ms += 300;
  EXPECT_EQ(playback.time(), 700u);
  EXPECT_EQ(playback.clockMsFor(kSecondStageMs - 700), 50);

  playback.setSpeed(0.25);
  EXPECT_EQ(playback.time(), 700u);
  clock->now_ms += 400;
  EXPECT_EQ(playback.time(), 800u);
  EXPECT_EQ(playback.clockMsFor(3), 12);

  playback.setSpeed(4.0);
  EXPECT_EQ(playback.clockMsFor(3), 1) << "a wake rounded down comes early";

  playback.setSpeed(0.0);
  EXPECT_EQ(playback.speed(), 4.0) << "a speed of nothing is no speed";
}

// A speed and a loop chosen once hold for the next list taken up, as a
// reader stepping through pages expects.
TEST(CataloguePlayback, TheSpeedAndLoopCarryToTheNextList) {
  auto clock = std::make_shared<HandClock>();
  CataloguePlayback playback(clock);
  playback.load(stagedTimeline());
  playback.setSpeed(2.0);
  playback.setLooping(true);

  playback.load(blinkingTimeline());
  EXPECT_EQ(playback.speed(), 2.0);
  EXPECT_TRUE(playback.looping());
  EXPECT_EQ(playback.time(), 0u);
  EXPECT_TRUE(playback.playing());
}

// A list with nothing timed has nothing to play.
TEST(CataloguePlayback, AnUntimedListHasNothingToPlay) {
  auto clock = std::make_shared<HandClock>();
  CataloguePlayback playback(clock);
  orc::CatalogueDisplayList list;
  list.ops = {opAt(0)};
  playback.load(CataloguePresentationTimeline(list));

  EXPECT_EQ(playback.length(), 0u);
  EXPECT_FALSE(playback.playing());
  clock->now_ms += 1000;
  EXPECT_EQ(playback.time(), 0u);
  playback.seek(500);
  EXPECT_EQ(playback.time(), 0u);
}

}  // namespace gui_unit_test
