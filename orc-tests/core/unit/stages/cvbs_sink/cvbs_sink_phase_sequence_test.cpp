/*
 * File:        cvbs_sink_phase_sequence_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the CVBS sink's colour-sequence measurement
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/cvbs_sink/cvbs_sink_phase_sequence.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace orc_unit_test {
namespace {

using orc::ColourPhaseSequenceCheck;
using orc::VideoSystem;

// Feed |frames| consecutive frames of a continuous sequence starting with the
// given first-field phase ID.
void feed_continuous(ColourPhaseSequenceCheck& check, VideoSystem system,
                     int32_t first_phase, int frames) {
  const int32_t n = (system == VideoSystem::NTSC) ? 4 : 8;
  int32_t phase = first_phase;
  for (int i = 0; i < frames; ++i) {
    const int32_t field2 = (phase % n) + 1;
    check.observe(phase, field2);
    phase = ((phase - 1 + 2) % n) + 1;
  }
}

}  // namespace

TEST(CVBSSinkPhaseSequence, ContinuousPALSequenceIsLockedAndContinuous) {
  ColourPhaseSequenceCheck check(VideoSystem::PAL);
  feed_continuous(check, VideoSystem::PAL, 1, 20);

  EXPECT_TRUE(check.phase_locked());
  EXPECT_EQ(check.sequence_continuous(), std::optional<bool>(true));
  EXPECT_EQ(check.discontinuities(), 0u);
  EXPECT_EQ(check.frames_measured(), 20u);
  EXPECT_EQ(check.frames_seen(), 20u);
  EXPECT_STREQ(check.signal_state_preset(), "STANDARD_STABLE_LOCKED");
}

TEST(CVBSSinkPhaseSequence, ContinuousNTSCSequenceIsLockedAndContinuous) {
  ColourPhaseSequenceCheck check(VideoSystem::NTSC);
  feed_continuous(check, VideoSystem::NTSC, 1, 12);

  EXPECT_TRUE(check.phase_locked());
  EXPECT_EQ(check.sequence_continuous(), std::optional<bool>(true));
  EXPECT_STREQ(check.signal_state_preset(), "STANDARD_STABLE_LOCKED");
}

TEST(CVBSSinkPhaseSequence, PALMUsesTheEightFieldSequence) {
  ColourPhaseSequenceCheck check(VideoSystem::PAL_M);
  feed_continuous(check, VideoSystem::PAL_M, 3, 9);

  EXPECT_TRUE(check.phase_locked());
  EXPECT_EQ(check.sequence_continuous(), std::optional<bool>(true));
}

TEST(CVBSSinkPhaseSequence, SingleFrameHasNothingToBreak) {
  ColourPhaseSequenceCheck check(VideoSystem::PAL);
  check.observe(5, 6);

  EXPECT_TRUE(check.phase_locked());
  EXPECT_EQ(check.sequence_continuous(), std::optional<bool>(true));
}

// CVBS file format spec v1.6.0: a discontinuity is a property of the content
// and must not downgrade the preset — every measured frame is still sampled
// at the standard phase points, so the file stays LOCKED and the break is
// reported through sequence_continuous instead.
TEST(CVBSSinkPhaseSequence, DiscontinuityMarksTheFileDiscontinuousNotUnlocked) {
  ColourPhaseSequenceCheck check(VideoSystem::PAL);
  // Frames 0-2 continuous (1/2, 3/4, 5/6), then frame 3 jumps to 1/2 where
  // 7/8 was expected.
  check.observe(1, 2);
  check.observe(3, 4);
  check.observe(5, 6);
  check.observe(1, 2);

  EXPECT_TRUE(check.phase_locked());
  EXPECT_EQ(check.sequence_continuous(), std::optional<bool>(false));
  EXPECT_EQ(check.discontinuities(), 1u);
  ASSERT_TRUE(check.first_discontinuity_frame().has_value());
  EXPECT_EQ(*check.first_discontinuity_frame(), 3u);
  EXPECT_STREQ(check.signal_state_preset(), "STANDARD_STABLE_LOCKED");
}

TEST(CVBSSinkPhaseSequence, OneJumpCostsOneDiscontinuity) {
  ColourPhaseSequenceCheck check(VideoSystem::PAL);
  check.observe(1, 2);
  check.observe(3, 4);
  // Jump: 1/2 instead of 5/6, then continuous from there.
  check.observe(1, 2);
  check.observe(3, 4);
  check.observe(5, 6);

  EXPECT_EQ(check.discontinuities(), 1u);
  EXPECT_EQ(*check.first_discontinuity_frame(), 2u);
}

TEST(CVBSSinkPhaseSequence, MismatchedFieldsWithinAFrameIsADiscontinuity) {
  ColourPhaseSequenceCheck check(VideoSystem::PAL);
  check.observe(1, 3);  // field 2 should be phase 2

  EXPECT_TRUE(check.phase_locked());
  EXPECT_EQ(check.sequence_continuous(), std::optional<bool>(false));
  EXPECT_EQ(check.discontinuities(), 1u);
  EXPECT_EQ(*check.first_discontinuity_frame(), 0u);
}

// Blank leader, lead-in and black frames have no burst. They still advance the
// sequence by two fields each, so the expected phase is projected across them
// rather than treated as a break.
TEST(CVBSSinkPhaseSequence, UnmeasurableFramesAreProjectedAcross) {
  ColourPhaseSequenceCheck check(VideoSystem::PAL);
  check.observe(1, 2);
  check.observe(-1, -1);
  check.observe(-1, -1);
  check.observe(7, 8);  // 1 + 2*3 fields = 7

  EXPECT_TRUE(check.phase_locked());
  EXPECT_EQ(check.sequence_continuous(), std::optional<bool>(true));
  EXPECT_EQ(check.frames_seen(), 4u);
  EXPECT_EQ(check.frames_measured(), 2u);
}

TEST(CVBSSinkPhaseSequence, ABreakAcrossAGapIsStillCaught) {
  ColourPhaseSequenceCheck check(VideoSystem::PAL);
  check.observe(1, 2);
  check.observe(-1, -1);
  check.observe(1, 2);  // expected 5/6 after a one-frame gap

  EXPECT_EQ(check.sequence_continuous(), std::optional<bool>(false));
  EXPECT_EQ(check.discontinuities(), 1u);
  EXPECT_EQ(*check.first_discontinuity_frame(), 2u);
}

// A file in which no burst could be measured cannot support a phase-lock
// claim, and its continuity is unknown rather than false.
TEST(CVBSSinkPhaseSequence, NothingMeasurableIsUnlockedWithUnknownContinuity) {
  ColourPhaseSequenceCheck check(VideoSystem::PAL);
  for (int i = 0; i < 5; ++i) check.observe(-1, -1);

  EXPECT_FALSE(check.phase_locked());
  EXPECT_EQ(check.sequence_continuous(), std::nullopt);
  EXPECT_EQ(check.frames_measured(), 0u);
  EXPECT_EQ(check.discontinuities(), 0u);
  EXPECT_STREQ(check.signal_state_preset(), "STANDARD_STABLE_UNLOCKED");
  EXPECT_NE(check.summary().find("unmeasurable"), std::string::npos);
}

TEST(CVBSSinkPhaseSequence, NoFramesAtAllIsNotLocked) {
  ColourPhaseSequenceCheck check(VideoSystem::PAL);

  EXPECT_FALSE(check.phase_locked());
  EXPECT_EQ(check.sequence_continuous(), std::nullopt);
  EXPECT_EQ(check.frames_seen(), 0u);
}

TEST(CVBSSinkPhaseSequence, OutOfRangePhaseIdsAreTreatedAsUnmeasurable) {
  ColourPhaseSequenceCheck check(VideoSystem::NTSC);
  check.observe(5, 6);  // beyond the NTSC 4-field sequence

  EXPECT_EQ(check.frames_measured(), 0u);
  EXPECT_EQ(check.discontinuities(), 0u);
}

TEST(CVBSSinkPhaseSequence, UnknownSystemCannotBeVerified) {
  ColourPhaseSequenceCheck check(VideoSystem::Unknown);
  feed_continuous(check, VideoSystem::PAL, 1, 5);

  EXPECT_FALSE(check.phase_locked());
  EXPECT_EQ(check.sequence_continuous(), std::nullopt);
  EXPECT_EQ(check.frames_seen(), 5u);
  EXPECT_EQ(check.frames_measured(), 0u);
  EXPECT_NE(check.summary().find("unknown video system"), std::string::npos);
}

TEST(CVBSSinkPhaseSequence, SummaryNamesTheFirstBreak) {
  ColourPhaseSequenceCheck check(VideoSystem::PAL);
  check.observe(1, 2);
  check.observe(1, 2);

  const std::string summary = check.summary();
  EXPECT_NE(summary.find("output frame 1"), std::string::npos);
  EXPECT_NE(summary.find("sequence_continuous = FALSE"), std::string::npos);
  EXPECT_NE(summary.find("STANDARD_STABLE_LOCKED"), std::string::npos);
  EXPECT_NE(summary.find("disc mapping"), std::string::npos);
}

}  // namespace orc_unit_test
