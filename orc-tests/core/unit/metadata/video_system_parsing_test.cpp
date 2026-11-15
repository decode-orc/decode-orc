/*
 * File:        video_system_parsing_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     Unit tests for video system name parsing with SQLite vs JSON
 * constraints
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/common_types.h>

// Note: lddecodemetadata.h is internal/private and not exported via public
// headers. parseVideoSystemName() is declared in lddecodemetadata.h but cannot
// be included here without adding a public forward declaration or exposing
// internals. For this test suite, we focus on testing
// video_system_from_string() which is the public-facing function for SQLite
// metadata parsing. The JSON fallback parsing is tested indirectly through the
// JSON reader integration.

namespace orc {

// Tests for video_system_from_string() - used for SQLite metadata
class VideoSystemFromStringTest : public ::testing::Test {};

TEST_F(VideoSystemFromStringTest, Accepts_CanonicalPal) {
  EXPECT_EQ(video_system_from_string("PAL"), VideoSystem::PAL);
}

TEST_F(VideoSystemFromStringTest, Accepts_CanonicalNtsc) {
  EXPECT_EQ(video_system_from_string("NTSC"), VideoSystem::NTSC);
}

TEST_F(VideoSystemFromStringTest, Accepts_CanonicalPalM) {
  EXPECT_EQ(video_system_from_string("PAL_M"), VideoSystem::PAL_M);
}

TEST_F(VideoSystemFromStringTest, Accepts_HyphenatedPalM) {
  // Both "PAL_M" and "PAL-M" are accepted as aliases for PAL_M.
  EXPECT_EQ(video_system_from_string("PAL-M"), VideoSystem::PAL_M);
}

TEST_F(VideoSystemFromStringTest, Rejects_UnknownFormat) {
  EXPECT_EQ(video_system_from_string("MESECAM"), VideoSystem::Unknown);
  EXPECT_EQ(video_system_from_string("PAL_L"), VideoSystem::Unknown);
  EXPECT_EQ(video_system_from_string(""), VideoSystem::Unknown);
  EXPECT_EQ(video_system_from_string("pal_m"),
            VideoSystem::Unknown);  // case-sensitive
}

TEST_F(VideoSystemFromStringTest, AcceptsCanonicalAndHyphenatedPalM) {
  // Both "PAL_M" and "PAL-M" are accepted; lowercase variants are not.
  EXPECT_EQ(video_system_from_string("PAL_M"), VideoSystem::PAL_M);
  EXPECT_EQ(video_system_from_string("PAL-M"), VideoSystem::PAL_M);
  EXPECT_EQ(video_system_from_string("pal_m"), VideoSystem::Unknown);
  EXPECT_EQ(video_system_from_string("Pal_M"), VideoSystem::Unknown);
}

}  // namespace orc
