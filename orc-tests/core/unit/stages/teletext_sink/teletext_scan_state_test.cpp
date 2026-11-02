/*
 * File:        teletext_scan_state_test.cpp
 * Module:      orc-tests
 * Purpose:     What a teletext pass learns about a recording, and when it is
 *              willing to act on it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_scan_state.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace orc {
namespace {

////////////////////////////////////////////////////////////////////////////////////////////
// Helpers
////////////////////////////////////////////////////////////////////////////////////////////

// A line that locked at |lock| and yielded a packet.
TeletextLineResult locked_at(double lock) {
  TeletextLineResult result;
  result.valid = true;
  result.lock_sample = lock;
  return result;
}

// A line that locked at |lock| but was rejected by a later gate.
TeletextLineResult rejected_at(double lock) {
  TeletextLineResult result;
  result.valid = false;
  result.lock_sample = lock;
  return result;
}

// Feed |count| locks all at |lock|.
void feed(TeletextPhaseTracker& tracker, size_t count, double lock) {
  for (size_t i = 0; i < count; ++i) {
    tracker.observe(locked_at(lock));
  }
}

////////////////////////////////////////////////////////////////////////////////////////////
// TeletextPhaseTracker
////////////////////////////////////////////////////////////////////////////////////////////

// Until enough lines have locked there is nothing to pin to, and an invalid
// hint is what tells the slicer to sweep the whole window.
TEST(TeletextPhaseTracker, WithholdsAHintUntilEnoughLinesHaveLocked) {
  TeletextPhaseTracker tracker;
  EXPECT_FALSE(tracker.hint().valid);

  feed(tracker, TeletextPhaseTracker::kLocksBeforeHinting - 1, 150.0);
  EXPECT_FALSE(tracker.hint().valid);

  tracker.observe(locked_at(150.0));
  EXPECT_TRUE(tracker.hint().valid);
}

// A recording whose data lines all start at the same sample pins to it, with
// the floor radius rather than the zero spread the locks actually showed: a
// window of no width would reject the first line that jittered by a sample.
TEST(TeletextPhaseTracker, PinsToTheAgreedPositionWithTheFloorRadius) {
  TeletextPhaseTracker tracker;
  feed(tracker, TeletextPhaseTracker::kLocksBeforeHinting, 150.0);

  const TeletextPhaseHint hint = tracker.hint();
  ASSERT_TRUE(hint.valid);
  EXPECT_DOUBLE_EQ(hint.centre, 150.0);
  EXPECT_DOUBLE_EQ(hint.radius, TeletextPhaseTracker::kMinRadiusSamples);
}

// A source with real jitter pins to the middle of the spread and widens to
// cover it, so every lock it has seen is inside the window it offers.
TEST(TeletextPhaseTracker, WidensToCoverTheSpreadOfTheLocksItHasSeen) {
  TeletextPhaseTracker tracker;
  for (size_t i = 0; i < TeletextPhaseTracker::kLocksBeforeHinting; ++i) {
    tracker.observe(locked_at((i % 2 == 0) ? 146.0 : 154.0));
  }

  const TeletextPhaseHint hint = tracker.hint();
  ASSERT_TRUE(hint.valid);
  EXPECT_DOUBLE_EQ(hint.centre, 150.0);
  EXPECT_GE(hint.centre + hint.radius, 154.0);
  EXPECT_LE(hint.centre - hint.radius, 146.0);
}

// Locks that disagree by more than a pin's worth are not a pin, and offering
// one anyway would cost every line a wasted first attempt.
TEST(TeletextPhaseTracker, WithholdsAHintWhenTheLocksDisagreeTooWidely) {
  TeletextPhaseTracker tracker;
  for (size_t i = 0; i < TeletextPhaseTracker::kLocksBeforeHinting; ++i) {
    tracker.observe(locked_at((i % 2 == 0) ? 120.0 : 180.0));
  }

  EXPECT_FALSE(tracker.hint().valid);
}

// A lock the later gates rejected is as likely to be noise as signal, so it is
// not evidence of where the data starts.
TEST(TeletextPhaseTracker, IgnoresLinesThatYieldedNoPacket) {
  TeletextPhaseTracker tracker;
  for (size_t i = 0; i < TeletextPhaseTracker::kLocksBeforeHinting * 2; ++i) {
    tracker.observe(rejected_at(150.0));
  }
  EXPECT_FALSE(tracker.hint().valid);
  EXPECT_EQ(tracker.locks_seen(), 0u);

  TeletextLineResult never_locked;
  never_locked.valid = true;
  never_locked.lock_sample = -1.0;
  tracker.observe(never_locked);
  EXPECT_EQ(tracker.locks_seen(), 0u);
}

// The lock window is a running one, so a recording whose data start moves part
// way through — a tape spliced from two transfers — follows the new position
// rather than straddling both.
TEST(TeletextPhaseTracker, FollowsAPositionThatMovesPartWayThrough) {
  TeletextPhaseTracker tracker;
  feed(tracker, TeletextPhaseTracker::kLockWindow, 120.0);
  ASSERT_DOUBLE_EQ(tracker.hint().centre, 120.0);

  feed(tracker, TeletextPhaseTracker::kLockWindow, 180.0);
  const TeletextPhaseHint hint = tracker.hint();
  ASSERT_TRUE(hint.valid);
  EXPECT_DOUBLE_EQ(hint.centre, 180.0);
}

////////////////////////////////////////////////////////////////////////////////////////////
// TeletextScanSnapshot — the masking rules, as a worker sees them
////////////////////////////////////////////////////////////////////////////////////////////

// A snapshot of a pass that has learned nothing, which is also the default, is
// the behaviour of a caller that wants no learning at all: read everything.
TEST(TeletextScanSnapshot, ReadsEverythingByDefault) {
  const TeletextScanSnapshot snapshot;
  EXPECT_FALSE(snapshot.pin_phase);
  EXPECT_FALSE(snapshot.mask_lines);
  EXPECT_FALSE(snapshot.acquisition_hint().valid);
  for (uint64_t frame : {0ULL, 1ULL, 1000ULL}) {
    EXPECT_TRUE(snapshot.reads_full_window(frame));
    EXPECT_TRUE(snapshot.should_probe(frame, 0, 7));
  }
}

// The learning frames read everything, because a mask built from one frame
// would be a mask built from whichever lines that frame happened to carry.
TEST(TeletextScanSnapshot, ReadsEveryLineWhileLearning) {
  TeletextScanSnapshot snapshot;
  snapshot.mask_lines = true;
  snapshot.line_options.learn_frames = 10;
  snapshot.line_options.recheck_interval = 0;

  for (uint64_t frame = 0; frame < 10; ++frame) {
    EXPECT_TRUE(snapshot.reads_full_window(frame)) << "frame " << frame;
    EXPECT_TRUE(snapshot.should_probe(frame, 0, 7));
  }
  EXPECT_FALSE(snapshot.reads_full_window(10));
}

// After the learning frames, only the lines the mask marks are read.
TEST(TeletextScanSnapshot, SkipsLinesThatNeverCarriedAPacket) {
  TeletextScanSnapshot snapshot;
  snapshot.mask_lines = true;
  snapshot.line_options.learn_frames = 2;
  snapshot.line_options.recheck_interval = 0;
  snapshot.line_alive[0][7] = true;

  EXPECT_TRUE(snapshot.should_probe(5, 0, 7));
  EXPECT_FALSE(snapshot.should_probe(5, 0, 8));
}

// The mask is per field: a service that uses different lines in each field
// must not have one field's lines decided by the other's.
TEST(TeletextScanSnapshot, MasksEachFieldSeparately) {
  TeletextScanSnapshot snapshot;
  snapshot.mask_lines = true;
  snapshot.line_options.learn_frames = 0;
  snapshot.line_options.recheck_interval = 0;
  snapshot.line_alive[0][7] = true;
  snapshot.line_alive[1][9] = true;

  EXPECT_TRUE(snapshot.should_probe(5, 0, 7));
  EXPECT_FALSE(snapshot.should_probe(5, 1, 7));
  EXPECT_FALSE(snapshot.should_probe(5, 0, 9));
  EXPECT_TRUE(snapshot.should_probe(5, 1, 9));
}

// A service can start part way into a recording, so the full window is read
// again periodically and a line that comes alive is found.
TEST(TeletextScanSnapshot, ReadsTheFullWindowOnRecheckFrames) {
  TeletextScanSnapshot snapshot;
  snapshot.mask_lines = true;
  snapshot.line_options.learn_frames = 2;
  snapshot.line_options.recheck_interval = 5;

  EXPECT_FALSE(snapshot.reads_full_window(3));
  EXPECT_FALSE(snapshot.should_probe(3, 0, 8));
  EXPECT_TRUE(snapshot.reads_full_window(5));
  EXPECT_TRUE(snapshot.should_probe(5, 0, 8));
  EXPECT_TRUE(snapshot.should_probe(10, 0, 8));
}

// A zero interval is the caller saying "the learning frames are the whole of
// it", which is the only way to get a mask that never re-reads.
TEST(TeletextScanSnapshot, AZeroRecheckIntervalNeverReadsInFullAgain) {
  TeletextScanSnapshot snapshot;
  snapshot.mask_lines = true;
  snapshot.line_options.learn_frames = 1;
  snapshot.line_options.recheck_interval = 0;

  for (uint64_t frame = 1; frame < 1000; ++frame) {
    ASSERT_FALSE(snapshot.reads_full_window(frame)) << "frame " << frame;
  }
}

// A caller that widened its window past what the mask can index gets the
// behaviour it had before rather than a silently mis-indexed mask.
TEST(TeletextScanSnapshot, NeverMasksLinesItCannotTrack) {
  TeletextScanSnapshot snapshot;
  snapshot.mask_lines = true;
  snapshot.line_options.learn_frames = 0;
  snapshot.line_options.recheck_interval = 0;

  EXPECT_TRUE(snapshot.should_probe(5, 0, kTeletextMaxTrackedFieldLine + 1));
  EXPECT_TRUE(snapshot.should_probe(5, 0, -1));
}

// Pinning off leaves the hint unread, whatever it holds — which is what the
// parameter has to mean if turning it off is to restore the full sweep.
TEST(TeletextScanSnapshot, WithholdsTheHintWhenPinningIsOff) {
  TeletextScanSnapshot snapshot;
  snapshot.hint.valid = true;
  snapshot.hint.centre = 150.0;
  snapshot.hint.radius = 3.0;

  snapshot.pin_phase = false;
  EXPECT_FALSE(snapshot.acquisition_hint().valid);
  snapshot.pin_phase = true;
  EXPECT_TRUE(snapshot.acquisition_hint().valid);
  EXPECT_DOUBLE_EQ(snapshot.acquisition_hint().centre, 150.0);
}

////////////////////////////////////////////////////////////////////////////////////////////
// TeletextScanState — advancing from one snapshot to the next
////////////////////////////////////////////////////////////////////////////////////////////

// What a pass takes in becomes what the next block of it is sliced against,
// and a pass always acts on it: every snapshot pins the sweep and masks the
// lines, which stand aside on their own until there is something to act on.
TEST(TeletextScanState, SnapshotCarriesWhatThePassHasLearned) {
  TeletextScanState state(/*calibrate_clock=*/false);

  const TeletextScanSnapshot before = state.snapshot();
  EXPECT_TRUE(before.pin_phase);
  EXPECT_TRUE(before.mask_lines);
  EXPECT_FALSE(before.hint.valid);
  EXPECT_FALSE(before.line_alive[0][7]);

  for (size_t i = 0; i < TeletextPhaseTracker::kLocksBeforeHinting; ++i) {
    state.observe(0, 7, locked_at(150.0));
  }

  const TeletextScanSnapshot after = state.snapshot();
  EXPECT_TRUE(after.hint.valid);
  EXPECT_DOUBLE_EQ(after.hint.centre, 150.0);
  EXPECT_TRUE(after.line_alive[0][7]);
  EXPECT_FALSE(after.line_alive[1][7]);
}

// A snapshot taken earlier is a value, so it keeps describing what was known
// when it was taken. That is the whole reason the workers are given one.
TEST(TeletextScanState, AnEarlierSnapshotIsUnaffectedByLaterLearning) {
  TeletextScanState state(/*calibrate_clock=*/false);
  const TeletextScanSnapshot taken_first = state.snapshot();

  for (size_t i = 0; i < TeletextPhaseTracker::kLocksBeforeHinting; ++i) {
    state.observe(0, 7, locked_at(150.0));
  }

  EXPECT_FALSE(taken_first.hint.valid);
  EXPECT_FALSE(taken_first.line_alive[0][7]);
  EXPECT_TRUE(state.snapshot().hint.valid);
}

// The options a pass was built with reach the workers through the snapshot,
// since deciding whether a frame is masked is their side of the arrangement.
TEST(TeletextScanState, SnapshotCarriesTheLineOptions) {
  TeletextLineTrackerOptions options;
  options.learn_frames = 7;
  options.recheck_interval = 11;
  const TeletextScanState state(/*calibrate_clock=*/false, options);

  const TeletextScanSnapshot snapshot = state.snapshot();
  EXPECT_EQ(snapshot.line_options.learn_frames, 7u);
  EXPECT_EQ(snapshot.line_options.recheck_interval, 11u);
}

// Skipped lines are counted so a run can report what the mask bought; the
// workers tally them per field and the pass adds them up in its own order.
TEST(TeletextScanState, CountsTheLinesTheMaskSkipped) {
  TeletextScanState state(/*calibrate_clock=*/false);
  EXPECT_EQ(state.lines().lines_skipped(), 0u);
  state.lines().add_skipped(3);
  state.lines().add_skipped(4);
  EXPECT_EQ(state.lines().lines_skipped(), 7u);
}

}  // namespace
}  // namespace orc
