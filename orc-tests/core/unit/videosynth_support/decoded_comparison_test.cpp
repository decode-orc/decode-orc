/*
 * File:        decoded_comparison_test.cpp
 * Module:      orc-core-unit-tests
 * Purpose:     The videosynth suite's comparison of raw YUV444P16 decodes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <decoded_comparison.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using orc_videosynth::DecodedFile;

// Two frames of 2-sample planes: Y, U, V of frame 0, then of frame 1.
DecodedFile two_frames(uint16_t frame1_v) {
  return DecodedFile({10, 10, 20, 20, 30, 30, 10, 10, 20, 20, frame1_v, 30}, 2);
}

TEST(VideosynthDecodedComparison, splitsFramesIntoThreePlanes) {
  const auto d = two_frames(30);
  EXPECT_EQ(d.frame_count(), 2u);
  EXPECT_EQ(d.plane_samples(), 2u);
  EXPECT_EQ(d.plane(1, 2)[0], 30);
  EXPECT_EQ(d.plane(0, 1)[1], 20);
}

TEST(VideosynthDecodedComparison, rmsIsPerPlaneInPercentOfFullScale) {
  const auto a = two_frames(30);
  const auto b = two_frames(30 + 655);
  const auto all = orc_videosynth::plane_rms(a, b, 0, 2);
  ASSERT_TRUE(all.has_value());
  EXPECT_EQ(all->y, 0.0);
  EXPECT_EQ(all->u, 0.0);
  // One of four V samples differs by 655 codes.
  EXPECT_NEAR(all->v, std::sqrt(655.0 * 655.0 / 4.0) / 65535.0 * 100.0, 1e-12);
  const auto first = orc_videosynth::plane_rms(a, b, 0, 1);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->v, 0.0);
  EXPECT_FALSE(orc_videosynth::plane_rms(a, b, 1, 2).has_value());
}

}  // namespace
