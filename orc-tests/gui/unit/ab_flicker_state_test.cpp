/*
 * File:        ab_flicker_state_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     ABFlickerState: alternation, hold and resume, manual side
 *              selection and interval changes while running
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "ab_flicker_state.h"

#include <gtest/gtest.h>

namespace gui_unit_test {

using orc::gui::ABFlickerState;
using orc::gui::ABSide;

TEST(ABFlickerStateTest, StartsOnARunningAtTheDefaultInterval) {
  const ABFlickerState state;
  EXPECT_EQ(state.shownSide(), ABSide::A);
  EXPECT_TRUE(state.isRunning());
  EXPECT_FALSE(state.isHeld());
  EXPECT_EQ(state.intervalMs(), ABFlickerState::kDefaultIntervalMs);
}

TEST(ABFlickerStateTest, EachTickShowsTheOtherSide) {
  ABFlickerState state;
  EXPECT_TRUE(state.tick());
  EXPECT_EQ(state.shownSide(), ABSide::B);
  EXPECT_TRUE(state.tick());
  EXPECT_EQ(state.shownSide(), ABSide::A);
  EXPECT_TRUE(state.tick());
  EXPECT_EQ(state.shownSide(), ABSide::B);
}

TEST(ABFlickerStateTest, HoldStopsOnTheSideShownAndResumeCarriesOnFromIt) {
  ABFlickerState state;
  state.tick();
  state.hold();
  EXPECT_FALSE(state.isRunning());
  EXPECT_FALSE(state.tick());
  EXPECT_FALSE(state.tick());
  EXPECT_EQ(state.shownSide(), ABSide::B);

  state.resume();
  EXPECT_TRUE(state.isRunning());
  EXPECT_TRUE(state.tick());
  EXPECT_EQ(state.shownSide(), ABSide::A);
}

TEST(ABFlickerStateTest, ToggleHoldIsWhatSpaceDoes) {
  ABFlickerState state;
  state.toggleHold();
  EXPECT_TRUE(state.isHeld());
  state.toggleHold();
  EXPECT_FALSE(state.isHeld());
  EXPECT_TRUE(state.isRunning());
}

TEST(ABFlickerStateTest, ShowingASideWhileRunningHoldsThere) {
  ABFlickerState state;
  state.show(ABSide::B);
  EXPECT_EQ(state.shownSide(), ABSide::B);
  EXPECT_TRUE(state.isHeld());
  EXPECT_FALSE(state.tick());
  EXPECT_EQ(state.shownSide(), ABSide::B);

  state.show(ABSide::A);
  EXPECT_EQ(state.shownSide(), ABSide::A);
  EXPECT_FALSE(state.tick());

  state.resume();
  EXPECT_TRUE(state.tick());
  EXPECT_EQ(state.shownSide(), ABSide::B);
}

TEST(ABFlickerStateTest, ChangingTheIntervalWhileRunningKeepsItRunning) {
  ABFlickerState state;
  state.tick();
  state.setIntervalMs(500);
  EXPECT_EQ(state.intervalMs(), 500u);
  EXPECT_TRUE(state.isRunning());
  EXPECT_EQ(state.shownSide(), ABSide::B);
  EXPECT_TRUE(state.tick());
  EXPECT_EQ(state.shownSide(), ABSide::A);
}

TEST(ABFlickerStateTest, AZeroIntervalIsOffAndANewIntervalStartsItAgain) {
  ABFlickerState state;
  state.setIntervalMs(0);
  EXPECT_FALSE(state.isRunning());
  EXPECT_FALSE(state.isHeld());
  EXPECT_FALSE(state.tick());
  EXPECT_EQ(state.shownSide(), ABSide::A);

  state.setIntervalMs(1000);
  EXPECT_TRUE(state.isRunning());
  EXPECT_TRUE(state.tick());
}

TEST(ABFlickerStateTest, AHeldFlickerStaysHeldAcrossAnIntervalChange) {
  ABFlickerState state;
  state.hold();
  state.setIntervalMs(500);
  EXPECT_FALSE(state.isRunning());
  state.setIntervalMs(0);
  state.setIntervalMs(800);
  EXPECT_TRUE(state.isHeld());
  EXPECT_FALSE(state.tick());
}

TEST(ABFlickerStateTest, SwapMovesTheShownSideWithThePicture) {
  ABFlickerState state;
  state.swap();
  EXPECT_EQ(state.shownSide(), ABSide::B);
  EXPECT_TRUE(state.isRunning());
  state.show(ABSide::A);
  state.swap();
  EXPECT_EQ(state.shownSide(), ABSide::B);
  EXPECT_TRUE(state.isHeld());
}

}  // namespace gui_unit_test
