/*
 * File:        cvbs_signal_state_notice_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 (gui-logic) tests for the CVBS "source is not
 *              colour-phase locked" advisory helper
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "cvbs_signal_state_notice.h"

#include <gtest/gtest.h>

namespace orc::gui {
namespace {

TEST(CVBSSignalStateNotice, LockedSourceNeedsNoNotice) {
  EXPECT_EQ(cvbsSignalStateNotice("STANDARD_TBC_LOCKED"), "");
}

// States the source stage refuses outright get an error from the stage; an
// advisory here would only be confusing.
TEST(CVBSSignalStateNotice, RejectedStatesGetNoNotice) {
  EXPECT_EQ(cvbsSignalStateNotice("STANDARD_RAW"), "");
  EXPECT_EQ(cvbsSignalStateNotice("NONSTANDARD_TBC_UNLOCKED"), "");
  EXPECT_EQ(cvbsSignalStateNotice(""), "");
}

TEST(CVBSSignalStateNotice, UnlockedSourceIsExplainedAndAllowed) {
  const std::string note = cvbsSignalStateNotice("STANDARD_TBC_UNLOCKED");
  ASSERT_FALSE(note.empty());
  EXPECT_NE(note.find("STANDARD_TBC_UNLOCKED"), std::string::npos);
  // Says what it means, that the source still works, and what to do about it.
  EXPECT_NE(note.find("skipped or jumped"), std::string::npos);
  EXPECT_NE(note.find("load and decode normally"), std::string::npos);
  EXPECT_NE(note.find("Disc Mapper"), std::string::npos);
}

}  // namespace
}  // namespace orc::gui
