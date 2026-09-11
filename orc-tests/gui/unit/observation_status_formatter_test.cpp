/*
 * File:        observation_status_formatter_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 (gui-logic) tests for the background-observation status
 *              line formatting helper (Task 5.4)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "observation_status_formatter.h"

#include <gtest/gtest.h>

namespace orc::gui {
namespace {

TEST(ObservationStatusFormatter, IdleWorkloadYieldsEmptyMessage) {
  EXPECT_EQ(formatObservationStatus(/*active=*/false, /*percent=*/0), "");
  // An inactive workload is empty regardless of a stale percentage.
  EXPECT_EQ(formatObservationStatus(/*active=*/false, /*percent=*/57), "");
}

TEST(ObservationStatusFormatter, ActiveWorkloadShowsPercentage) {
  EXPECT_EQ(formatObservationStatus(/*active=*/true, /*percent=*/0),
            "Computing observations\xE2\x80\xA6 0%");
  EXPECT_EQ(formatObservationStatus(/*active=*/true, /*percent=*/42),
            "Computing observations\xE2\x80\xA6 42%");
  EXPECT_EQ(formatObservationStatus(/*active=*/true, /*percent=*/100),
            "Computing observations\xE2\x80\xA6 100%");
}

TEST(ObservationStatusFormatter, PercentageIsClampedToRange) {
  EXPECT_EQ(formatObservationStatus(/*active=*/true, /*percent=*/150),
            "Computing observations\xE2\x80\xA6 100%");
  EXPECT_EQ(formatObservationStatus(/*active=*/true, /*percent=*/-5),
            "Computing observations\xE2\x80\xA6 0%");
}

TEST(ObservationStatusFormatter, ChecksVersusComputesByComputedFlag) {
  // A batch that has not computed anything yet is only verifying coverage of
  // already-stored frames and must say so.
  EXPECT_EQ(formatObservationStatus(/*active=*/true, /*percent=*/10,
                                    /*computing=*/false),
            "Checking observations\xE2\x80\xA6 10%");
  EXPECT_EQ(formatObservationStatus(/*active=*/true, /*percent=*/10,
                                    /*computing=*/true),
            "Computing observations\xE2\x80\xA6 10%");
  // Idle stays empty in both modes.
  EXPECT_EQ(formatObservationStatus(/*active=*/false, /*percent=*/10,
                                    /*computing=*/false),
            "");
}

TEST(ObservationStatusFormatter, SaysWhyProgressStopsWhileSweepsAreHeld) {
  // Held-back sweep work leaves the percentage sitting still. Without a reason
  // that reads as a hang, so the pause outranks both of the other verbs.
  EXPECT_EQ(formatObservationStatus(/*active=*/true, /*percent=*/42,
                                    /*computing=*/true, /*sweep_paused=*/true),
            "Observations paused during playback\xE2\x80\xA6 42%");
  EXPECT_EQ(formatObservationStatus(/*active=*/true, /*percent=*/42,
                                    /*computing=*/false, /*sweep_paused=*/true),
            "Observations paused during playback\xE2\x80\xA6 42%");
  // Nothing outstanding: still nothing to say.
  EXPECT_EQ(formatObservationStatus(/*active=*/false, /*percent=*/42,
                                    /*computing=*/true, /*sweep_paused=*/true),
            "");
}

TEST(ObservationStatusFormatter, RoundsCompletionFraction) {
  EXPECT_EQ(roundObservationPercent(0, 0), 100);  // empty batch is complete
  EXPECT_EQ(roundObservationPercent(0, 4), 0);
  EXPECT_EQ(roundObservationPercent(1, 4), 25);
  EXPECT_EQ(roundObservationPercent(1, 3), 33);  // 33.3 -> 33
  EXPECT_EQ(roundObservationPercent(2, 3), 67);  // 66.6 -> 67 (rounds up)
  EXPECT_EQ(roundObservationPercent(4, 4), 100);
  EXPECT_EQ(roundObservationPercent(5, 4), 100);  // never exceeds 100
}

}  // namespace
}  // namespace orc::gui
