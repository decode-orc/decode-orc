/*
 * File:        throttled_ring_reader_test.cpp
 * Module:      orc-tests/core/unit
 * Purpose:     Tests for ThrottledRingReader's kEof/kFailed distinction —
 *              the piece behind cvbs_stream_source/tbc_stream_source's
 *              unbounded (frame_count == 0) live-capture support
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/support/throttled_ring_reader.h>

#include <cstddef>
#include <string>
#include <vector>

namespace orc {
namespace tests {
namespace {

using orc::pipe_io::ProduceStatus;
using orc::pipe_io::ThrottledRingReader;

TEST(ThrottledRingReader, CleanEofStopsTheReaderWithoutFailing) {
  // Produces frames 0 and 1, then reports a clean end of input on frame 2 —
  // the shape cvbs_stream_source/tbc_stream_source's producers use once
  // frame_count is left at 0 (unbounded) and the real source runs out.
  ThrottledRingReader<int> reader(
      [](std::size_t frame_index, std::vector<int>& out, std::string&) {
        if (frame_index >= 2) return ProduceStatus::kEof;
        out = {static_cast<int>(frame_index)};
        return ProduceStatus::kOk;
      },
      /*frame_count=*/1000, /*buffer_frames=*/4);

  ASSERT_NE(reader.get_frame(0), nullptr);
  EXPECT_EQ(*reader.get_frame(0), 0);
  ASSERT_NE(reader.get_frame(1), nullptr);
  EXPECT_EQ(*reader.get_frame(1), 1);

  // Frame 2 was declared possible (frame_count=1000) but never arrives.
  EXPECT_EQ(reader.get_frame(2), nullptr);
  EXPECT_TRUE(reader.is_eof());
  EXPECT_FALSE(reader.failed());
  EXPECT_TRUE(reader.last_error().empty());

  // Every later id is equally unreachable, without blocking.
  EXPECT_EQ(reader.get_frame(500), nullptr);
  EXPECT_TRUE(reader.is_eof());
}

TEST(ThrottledRingReader, GenuineFailureIsDistinctFromEof) {
  ThrottledRingReader<int> reader(
      [](std::size_t frame_index, std::vector<int>& out, std::string& error) {
        if (frame_index >= 1) {
          error = "simulated read failure";
          return ProduceStatus::kError;
        }
        out = {42};
        return ProduceStatus::kOk;
      },
      /*frame_count=*/1000, /*buffer_frames=*/4);

  ASSERT_NE(reader.get_frame(0), nullptr);
  EXPECT_EQ(reader.get_frame(1), nullptr);
  EXPECT_TRUE(reader.failed());
  EXPECT_FALSE(reader.is_eof());
  EXPECT_EQ(reader.last_error(), "simulated read failure");
}

TEST(ThrottledRingReader, EofAfterZeroFramesLeavesEverythingUnreachable) {
  ThrottledRingReader<int> reader(
      [](std::size_t, std::vector<int>&, std::string&) {
        return ProduceStatus::kEof;
      },
      /*frame_count=*/1000, /*buffer_frames=*/4);

  EXPECT_EQ(reader.get_frame(0), nullptr);
  EXPECT_TRUE(reader.is_eof());
  EXPECT_FALSE(reader.failed());
}

}  // namespace
}  // namespace tests
}  // namespace orc
