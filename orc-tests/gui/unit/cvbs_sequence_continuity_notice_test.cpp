/*
 * File:        cvbs_sequence_continuity_notice_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 (gui-logic) tests for the CVBS "source sequence is not
 *              continuous" advisory helper
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "cvbs_sequence_continuity_notice.h"

#include <gtest/gtest.h>

#include <optional>

namespace orc::gui {
namespace {

TEST(CVBSSequenceContinuityNotice, ContinuousSourceNeedsNoNotice) {
  EXPECT_EQ(cvbsSequenceContinuityNotice(true), "");
}

// NULL means continuity was not assessed; nagging on every such file would
// drown the one advisory that matters.
TEST(CVBSSequenceContinuityNotice, UnknownContinuityGetsNoNotice) {
  EXPECT_EQ(cvbsSequenceContinuityNotice(std::nullopt), "");
}

TEST(CVBSSequenceContinuityNotice, DiscontinuousSourceIsExplainedAndAllowed) {
  const std::string note = cvbsSequenceContinuityNotice(false);
  ASSERT_FALSE(note.empty());
  EXPECT_NE(note.find("sequence_continuous = FALSE"), std::string::npos);
  // Says what it means, that the source still works, and what to do about it.
  EXPECT_NE(note.find("skipped or jumped"), std::string::npos);
  EXPECT_NE(note.find("load and decode normally"), std::string::npos);
  EXPECT_NE(note.find("Disc Mapper"), std::string::npos);
}

}  // namespace
}  // namespace orc::gui
