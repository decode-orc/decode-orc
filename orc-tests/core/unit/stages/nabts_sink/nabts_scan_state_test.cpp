/*
 * File:        nabts_scan_state_test.cpp
 * Module:      orc-tests
 * Purpose:     What a NABTS pass learns about a recording, and when it is
 *              willing to act on it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_scan_state.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

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
void feed(NabtsPhaseTracker& tracker, size_t count, double lock) {
  for (size_t i = 0; i < count; ++i) {
    tracker.observe(locked_at(lock));
  }
}

////////////////////////////////////////////////////////////////////////////////////////////
// NabtsPhaseTracker
////////////////////////////////////////////////////////////////////////////////////////////

// Until enough lines have locked there is nothing to pin to, and an invalid
// hint is what tells the slicer to sweep the whole window.
TEST(NabtsPhaseTracker, WithholdsAHintUntilEnoughLinesHaveLocked) {
  NabtsPhaseTracker tracker;
  EXPECT_FALSE(tracker.hint().valid);

  feed(tracker, NabtsPhaseTracker::kLocksBeforeHinting - 1, 150.0);
  EXPECT_FALSE(tracker.hint().valid);

  tracker.observe(locked_at(150.0));
  EXPECT_TRUE(tracker.hint().valid);
}

// A recording whose data lines all start at the same sample pins to it, with
// the floor radius rather than the zero spread the locks actually showed: a
// window of no width would reject the first line that jittered by a sample.
TEST(NabtsPhaseTracker, PinsToTheAgreedPositionWithTheFloorRadius) {
  NabtsPhaseTracker tracker;
  feed(tracker, NabtsPhaseTracker::kLocksBeforeHinting, 150.0);

  const TeletextPhaseHint hint = tracker.hint();
  ASSERT_TRUE(hint.valid);
  EXPECT_DOUBLE_EQ(hint.centre, 150.0);
  EXPECT_DOUBLE_EQ(hint.radius, NabtsPhaseTracker::kMinRadiusSamples);
}

// A source with real jitter pins to the middle of the spread and widens to
// cover it, so every lock it has seen is inside the window it offers.
TEST(NabtsPhaseTracker, WidensToCoverTheSpreadOfTheLocksItHasSeen) {
  NabtsPhaseTracker tracker;
  for (size_t i = 0; i < NabtsPhaseTracker::kLocksBeforeHinting; ++i) {
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
TEST(NabtsPhaseTracker, WithholdsAHintWhenTheLocksDisagreeTooWidely) {
  NabtsPhaseTracker tracker;
  for (size_t i = 0; i < NabtsPhaseTracker::kLocksBeforeHinting; ++i) {
    tracker.observe(locked_at((i % 2 == 0) ? 120.0 : 180.0));
  }

  EXPECT_FALSE(tracker.hint().valid);
}

// A lock the later gates rejected is as likely to be noise as signal, so it is
// not evidence of where the data starts.
TEST(NabtsPhaseTracker, IgnoresLinesThatYieldedNoPacket) {
  NabtsPhaseTracker tracker;
  for (size_t i = 0; i < NabtsPhaseTracker::kLocksBeforeHinting * 2; ++i) {
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
TEST(NabtsPhaseTracker, FollowsAPositionThatMovesPartWayThrough) {
  NabtsPhaseTracker tracker;
  feed(tracker, NabtsPhaseTracker::kLockWindow, 120.0);
  ASSERT_DOUBLE_EQ(tracker.hint().centre, 120.0);

  feed(tracker, NabtsPhaseTracker::kLockWindow, 180.0);
  const TeletextPhaseHint hint = tracker.hint();
  ASSERT_TRUE(hint.valid);
  EXPECT_DOUBLE_EQ(hint.centre, 180.0);
}

////////////////////////////////////////////////////////////////////////////////////////////
// NabtsScanSnapshot — the masking rules, as a worker sees them
////////////////////////////////////////////////////////////////////////////////////////////

// A snapshot of a pass that has learned nothing, which is also the default, is
// the behaviour of a caller that wants no learning at all: read everything.
TEST(NabtsScanSnapshot, ReadsEverythingByDefault) {
  const NabtsScanSnapshot snapshot;
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
TEST(NabtsScanSnapshot, ReadsEveryLineWhileLearning) {
  NabtsScanSnapshot snapshot;
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
TEST(NabtsScanSnapshot, SkipsLinesThatNeverCarriedAPacket) {
  NabtsScanSnapshot snapshot;
  snapshot.mask_lines = true;
  snapshot.line_options.learn_frames = 2;
  snapshot.line_options.recheck_interval = 0;
  snapshot.line_alive[0][7] = true;

  EXPECT_TRUE(snapshot.should_probe(5, 0, 7));
  EXPECT_FALSE(snapshot.should_probe(5, 0, 8));
}

// The mask is per field: a service that uses different lines in each field
// must not have one field's lines decided by the other's.
TEST(NabtsScanSnapshot, MasksEachFieldSeparately) {
  NabtsScanSnapshot snapshot;
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
TEST(NabtsScanSnapshot, ReadsTheFullWindowOnRecheckFrames) {
  NabtsScanSnapshot snapshot;
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
TEST(NabtsScanSnapshot, AZeroRecheckIntervalNeverReadsInFullAgain) {
  NabtsScanSnapshot snapshot;
  snapshot.mask_lines = true;
  snapshot.line_options.learn_frames = 1;
  snapshot.line_options.recheck_interval = 0;

  for (uint64_t frame = 1; frame < 1000; ++frame) {
    ASSERT_FALSE(snapshot.reads_full_window(frame)) << "frame " << frame;
  }
}

// A caller that widened its window past what the mask can index gets the
// behaviour it had before rather than a silently mis-indexed mask.
TEST(NabtsScanSnapshot, NeverMasksLinesItCannotTrack) {
  NabtsScanSnapshot snapshot;
  snapshot.mask_lines = true;
  snapshot.line_options.learn_frames = 0;
  snapshot.line_options.recheck_interval = 0;

  EXPECT_TRUE(snapshot.should_probe(5, 0, kNabtsMaxTrackedFieldLine + 1));
  EXPECT_TRUE(snapshot.should_probe(5, 0, -1));
}

// Pinning off leaves the hint unread, whatever it holds — which is what the
// parameter has to mean if turning it off is to restore the full sweep.
TEST(NabtsScanSnapshot, WithholdsTheHintWhenPinningIsOff) {
  NabtsScanSnapshot snapshot;
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
// NabtsScanState — advancing from one snapshot to the next
////////////////////////////////////////////////////////////////////////////////////////////

// What a pass takes in becomes what the next block of it is sliced against,
// and a pass always acts on it: every snapshot pins the sweep and masks the
// lines, which stand aside on their own until there is something to act on.
TEST(NabtsScanState, SnapshotCarriesWhatThePassHasLearned) {
  NabtsScanState state(/*calibrate_clock=*/false);

  const NabtsScanSnapshot before = state.snapshot();
  EXPECT_TRUE(before.pin_phase);
  EXPECT_TRUE(before.mask_lines);
  EXPECT_FALSE(before.hint.valid);
  EXPECT_FALSE(before.line_alive[0][7]);

  for (size_t i = 0; i < NabtsPhaseTracker::kLocksBeforeHinting; ++i) {
    state.observe(0, 7, locked_at(150.0));
  }

  const NabtsScanSnapshot after = state.snapshot();
  EXPECT_TRUE(after.hint.valid);
  EXPECT_DOUBLE_EQ(after.hint.centre, 150.0);
  EXPECT_TRUE(after.line_alive[0][7]);
  EXPECT_FALSE(after.line_alive[1][7]);
}

// A snapshot taken earlier is a value, so it keeps describing what was known
// when it was taken. That is the whole reason the workers are given one.
TEST(NabtsScanState, AnEarlierSnapshotIsUnaffectedByLaterLearning) {
  NabtsScanState state(/*calibrate_clock=*/false);
  const NabtsScanSnapshot taken_first = state.snapshot();

  for (size_t i = 0; i < NabtsPhaseTracker::kLocksBeforeHinting; ++i) {
    state.observe(0, 7, locked_at(150.0));
  }

  EXPECT_FALSE(taken_first.hint.valid);
  EXPECT_FALSE(taken_first.line_alive[0][7]);
  EXPECT_TRUE(state.snapshot().hint.valid);
}

// The options a pass was built with reach the workers through the snapshot,
// since deciding whether a frame is masked is their side of the arrangement.
TEST(NabtsScanState, SnapshotCarriesTheLineOptions) {
  NabtsLineTrackerOptions options;
  options.learn_frames = 7;
  options.recheck_interval = 11;
  const NabtsScanState state(/*calibrate_clock=*/false, options);

  const NabtsScanSnapshot snapshot = state.snapshot();
  EXPECT_EQ(snapshot.line_options.learn_frames, 7u);
  EXPECT_EQ(snapshot.line_options.recheck_interval, 11u);
}

// Skipped lines are counted so a run can report what the mask bought; the
// workers tally them per field and the pass adds them up in its own order.
TEST(NabtsScanState, CountsTheLinesTheMaskSkipped) {
  NabtsScanState state(/*calibrate_clock=*/false);
  EXPECT_EQ(state.lines().lines_skipped(), 0u);
  state.lines().add_skipped(3);
  state.lines().add_skipped(4);
  EXPECT_EQ(state.lines().lines_skipped(), 7u);
}

// A service sending four packets a field on lines 14 to 17 can reach a tape
// that keeps only 14, 15 and 17 of the second field, so the channel's index
// steps by two from line 15 to line 17. A loss is trusted once it has been
// shown a few times.
TEST(NabtsRoutineLosses, LearnsALossTheStepsShowBetweenTwoPositions) {
  NabtsRoutineLosses losses;
  const NabtsRoutineLosses::Position from{1, 15};
  const NabtsRoutineLosses::Position to{1, 17};
  EXPECT_EQ(losses.routine_loss(from, to), 0);
  for (uint32_t seen = 1; seen <= NabtsRoutineLosses::kLossesToTrust; ++seen) {
    losses.observe(from, to, 2);
    EXPECT_EQ(losses.routine_loss(from, to),
              seen == NabtsRoutineLosses::kLossesToTrust ? 1 : 0)
        << seen;
  }
  // The other field's packets, and the reverse pair, have shown nothing.
  EXPECT_EQ(losses.routine_loss({0, 15}, {0, 17}), 0);
  EXPECT_EQ(losses.routine_loss(to, from), 0);
}

// The first field keeps only lines 14 and 15, so from its line 15 to the next
// field's line 14 the index steps by three: two packets lost, one of them on a
// line the pass reads and one on a line it does not.
TEST(NabtsRoutineLosses, LearnsALossOfSeveralPacketsAcrossAField) {
  NabtsRoutineLosses losses;
  for (int i = 0; i < 3; ++i) {
    losses.observe({0, 15}, {1, 14}, 3);
  }
  EXPECT_EQ(losses.routine_loss({0, 15}, {1, 14}), 2);
}

// A service that sends nothing on a tape's test-signal lines steps by one, and
// a misread among them makes no routine. The loss must be seen on more than
// half of the steps between the two positions.
TEST(NabtsRoutineLosses, ALossSeenOnHalfTheStepsOrFewerIsNotRoutine) {
  NabtsRoutineLosses losses;
  const NabtsRoutineLosses::Position from{0, 15};
  const NabtsRoutineLosses::Position to{0, 17};
  for (int i = 0; i < 4; ++i) {
    losses.observe(from, to, 2);
    losses.observe(from, to, 1);
  }
  EXPECT_EQ(losses.routine_loss(from, to), 0);
  losses.observe(from, to, 2);
  EXPECT_EQ(losses.routine_loss(from, to), 1);
}

// A step of zero is the index repeating itself — a test signal that decodes as
// the same packet every frame, or a misread — and never a routine loss of
// fifteen packets.
TEST(NabtsRoutineLosses, AnIndexRepeatingItselfIsNeverRoutine) {
  NabtsRoutineLosses losses;
  for (int i = 0; i < 8; ++i) {
    losses.observe({0, 17}, {0, 17}, 0);
  }
  EXPECT_EQ(losses.routine_loss({0, 17}, {0, 17}), 0);
}

// The report lists the pairs of positions whose loss a group was told of, with
// the loss as it stands; a routine the groups never needed telling of — its
// packets lost on lines the pass reads anyway — is not listed.
TEST(NabtsRoutineLosses, ListsThePairsTheGroupsWereToldOf) {
  NabtsRoutineLosses losses;
  for (int i = 0; i < 3; ++i) {
    losses.observe({1, 15}, {1, 17}, 2);
    losses.observe({0, 15}, {1, 14}, 3);
  }
  EXPECT_TRUE(losses.told().empty());
  losses.note_told({0, 15}, {1, 14});
  losses.note_told({0, 15}, {1, 14});
  const std::vector<NabtsRoutineLosses::Routine> told = losses.told();
  ASSERT_EQ(told.size(), 1u);
  EXPECT_EQ(told[0].from.field, 0u);
  EXPECT_EQ(told[0].from.line, 15);
  EXPECT_EQ(told[0].to.field, 1u);
  EXPECT_EQ(told[0].to.line, 14);
  EXPECT_EQ(told[0].loss, 2);
}

}  // namespace
}  // namespace orc
