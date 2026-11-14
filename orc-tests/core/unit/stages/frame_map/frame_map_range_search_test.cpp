/*
 * File:        frame_map_range_search_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the frame map range binary-chop search
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../../orc/plugins/stages/frame_map/frame_map_range_search.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace {

using orc::frame_map_range::CancelCheck;
using orc::frame_map_range::find_first_frame_with_picture;
using orc::frame_map_range::find_last_frame_with_picture;
using orc::frame_map_range::PictureNumberProbe;
using orc::frame_map_range::SearchOutcome;
using orc::frame_map_range::SearchStatus;

// Probe backed by a vector of per-frame picture numbers starting at
// first_frame; counts invocations so tests can assert logarithmic behaviour.
class VectorProbe {
 public:
  VectorProbe(int64_t first_frame, std::vector<std::optional<int32_t>> pictures)
      : first_frame_(first_frame), pictures_(std::move(pictures)) {}

  PictureNumberProbe fn() {
    return [this](int64_t frame) -> std::optional<int32_t> {
      ++probe_count_;
      const int64_t index = frame - first_frame_;
      if (index < 0 || index >= static_cast<int64_t>(pictures_.size())) {
        return std::nullopt;
      }
      return pictures_[static_cast<size_t>(index)];
    };
  }

  int64_t first_frame() const { return first_frame_; }
  int64_t last_frame() const {
    return first_frame_ + static_cast<int64_t>(pictures_.size()) - 1;
  }
  size_t probe_count() const { return probe_count_; }

 private:
  int64_t first_frame_;
  std::vector<std::optional<int32_t>> pictures_;
  size_t probe_count_ = 0;
};

CancelCheck never_cancelled() {
  return [] { return false; };
}

// Strictly increasing numbering: picture = base + frame offset.
std::vector<std::optional<int32_t>> linear_pictures(size_t count,
                                                    int32_t base) {
  std::vector<std::optional<int32_t>> pictures(count);
  for (size_t i = 0; i < count; ++i) {
    pictures[i] = base + static_cast<int32_t>(i);
  }
  return pictures;
}

TEST(FrameMapRangeSearch, FindsFirstMatch_InMonotonicSequence) {
  VectorProbe probe(0, linear_pictures(100000, 1000));

  SearchOutcome outcome =
      find_first_frame_with_picture(probe.first_frame(), probe.last_frame(),
                                    54321, probe.fn(), never_cancelled());

  EXPECT_EQ(outcome.status, SearchStatus::Found);
  EXPECT_EQ(outcome.frame, 53321);
}

TEST(FrameMapRangeSearch, UsesLogarithmicProbes_OnLargeSource) {
  VectorProbe probe(0, linear_pictures(1000000, 1));

  SearchOutcome outcome =
      find_first_frame_with_picture(probe.first_frame(), probe.last_frame(),
                                    777777, probe.fn(), never_cancelled());

  EXPECT_EQ(outcome.status, SearchStatus::Found);
  EXPECT_EQ(outcome.frame, 777776);
  // Two anchors + ~log2(1e6) chop probes + refinement; a sequential hunt
  // would take ~778k probes.
  EXPECT_LT(probe.probe_count(), 100u);
}

TEST(FrameMapRangeSearch, HonoursNonZeroFirstFrame) {
  VectorProbe probe(5000, linear_pictures(10000, 200));

  SearchOutcome outcome =
      find_first_frame_with_picture(probe.first_frame(), probe.last_frame(),
                                    250, probe.fn(), never_cancelled());

  EXPECT_EQ(outcome.status, SearchStatus::Found);
  EXPECT_EQ(outcome.frame, 5050);
}

TEST(FrameMapRangeSearch, FindsTarget_AtRangeBoundaries) {
  VectorProbe probe(0, linear_pictures(1000, 100));

  SearchOutcome first = find_first_frame_with_picture(
      0, probe.last_frame(), 100, probe.fn(), never_cancelled());
  EXPECT_EQ(first.status, SearchStatus::Found);
  EXPECT_EQ(first.frame, 0);

  SearchOutcome last = find_first_frame_with_picture(
      0, probe.last_frame(), 1099, probe.fn(), never_cancelled());
  EXPECT_EQ(last.status, SearchStatus::Found);
  EXPECT_EQ(last.frame, 999);
}

TEST(FrameMapRangeSearch, FindsFirstOfRepeatedRun_ForStartAddress) {
  // Player hesitation: picture 500 repeats across frames 40-44.
  std::vector<std::optional<int32_t>> pictures = linear_pictures(100, 460);
  for (size_t i = 40; i <= 44; ++i) {
    pictures[i] = 500;
  }
  for (size_t i = 45; i < 100; ++i) {
    pictures[i] = 501 + static_cast<int32_t>(i - 45);
  }
  VectorProbe probe(0, std::move(pictures));

  SearchOutcome outcome =
      find_first_frame_with_picture(probe.first_frame(), probe.last_frame(),
                                    500, probe.fn(), never_cancelled());

  EXPECT_EQ(outcome.status, SearchStatus::Found);
  EXPECT_EQ(outcome.frame, 40);
}

TEST(FrameMapRangeSearch, FindsLastOfRepeatedRun_ForEndAddress) {
  std::vector<std::optional<int32_t>> pictures = linear_pictures(100, 460);
  for (size_t i = 40; i <= 44; ++i) {
    pictures[i] = 500;
  }
  for (size_t i = 45; i < 100; ++i) {
    pictures[i] = 501 + static_cast<int32_t>(i - 45);
  }
  VectorProbe probe(0, std::move(pictures));

  SearchOutcome outcome =
      find_last_frame_with_picture(probe.first_frame(), probe.last_frame(), 500,
                                   probe.fn(), never_cancelled());

  EXPECT_EQ(outcome.status, SearchStatus::Found);
  EXPECT_EQ(outcome.frame, 44);
}

TEST(FrameMapRangeSearch, SkipsDropouts_WhenVbiUnreadable) {
  // Unreadable stretches around the target: frames 4990-5009 have no VBI
  // except the target frame itself.
  std::vector<std::optional<int32_t>> pictures = linear_pictures(10000, 1);
  for (size_t i = 4990; i < 5010; ++i) {
    pictures[i] = std::nullopt;
  }
  pictures[5000] = 5001;
  VectorProbe probe(0, std::move(pictures));

  SearchOutcome outcome =
      find_first_frame_with_picture(probe.first_frame(), probe.last_frame(),
                                    5001, probe.fn(), never_cancelled());

  EXPECT_EQ(outcome.status, SearchStatus::Found);
  EXPECT_EQ(outcome.frame, 5000);
}

TEST(FrameMapRangeSearch, SkipsUnreadableLeadInAndLeadOut) {
  std::vector<std::optional<int32_t>> pictures = linear_pictures(1000, 100);
  for (size_t i = 0; i < 50; ++i) {
    pictures[i] = std::nullopt;
  }
  for (size_t i = 950; i < 1000; ++i) {
    pictures[i] = std::nullopt;
  }
  VectorProbe probe(0, std::move(pictures));

  SearchOutcome outcome =
      find_first_frame_with_picture(probe.first_frame(), probe.last_frame(),
                                    600, probe.fn(), never_cancelled());

  EXPECT_EQ(outcome.status, SearchStatus::Found);
  EXPECT_EQ(outcome.frame, 500);
}

TEST(FrameMapRangeSearch, ReturnsNotFound_WhenTargetOutsideDiscNumbering) {
  VectorProbe probe(0, linear_pictures(1000, 500));

  SearchOutcome before = find_first_frame_with_picture(
      0, probe.last_frame(), 100, probe.fn(), never_cancelled());
  EXPECT_EQ(before.status, SearchStatus::NotFound);

  SearchOutcome after = find_first_frame_with_picture(
      0, probe.last_frame(), 9999, probe.fn(), never_cancelled());
  EXPECT_EQ(after.status, SearchStatus::NotFound);
}

TEST(FrameMapRangeSearch, ReturnsNotFound_WhenTargetInSkippedGap) {
  // Player skipped forward: numbering jumps 199 -> 300 at frame 100.
  std::vector<std::optional<int32_t>> pictures(1000);
  for (size_t i = 0; i < 100; ++i) {
    pictures[i] = 100 + static_cast<int32_t>(i);
  }
  for (size_t i = 100; i < 1000; ++i) {
    pictures[i] = 300 + static_cast<int32_t>(i - 100);
  }
  VectorProbe probe(0, std::move(pictures));

  SearchOutcome outcome =
      find_first_frame_with_picture(probe.first_frame(), probe.last_frame(),
                                    250, probe.fn(), never_cancelled());

  EXPECT_EQ(outcome.status, SearchStatus::NotFound);
}

TEST(FrameMapRangeSearch, ReturnsNotFound_WhenNoVbiReadable) {
  VectorProbe probe(0, std::vector<std::optional<int32_t>>(500, std::nullopt));

  SearchOutcome outcome =
      find_first_frame_with_picture(probe.first_frame(), probe.last_frame(), 42,
                                    probe.fn(), never_cancelled());

  EXPECT_EQ(outcome.status, SearchStatus::NotFound);
}

TEST(FrameMapRangeSearch, ReturnsCancelled_WhenCancelRequested) {
  VectorProbe probe(0, linear_pictures(100000, 1));

  int polls = 0;
  CancelCheck cancel_after_three = [&polls] { return ++polls > 3; };

  SearchOutcome outcome =
      find_first_frame_with_picture(probe.first_frame(), probe.last_frame(),
                                    54321, probe.fn(), cancel_after_three);

  EXPECT_EQ(outcome.status, SearchStatus::Cancelled);
}

TEST(FrameMapRangeSearch, FindsLastMatch_WhenTargetIsFinalPicture) {
  VectorProbe probe(0, linear_pictures(1000, 100));

  SearchOutcome outcome =
      find_last_frame_with_picture(probe.first_frame(), probe.last_frame(),
                                   1099, probe.fn(), never_cancelled());

  EXPECT_EQ(outcome.status, SearchStatus::Found);
  EXPECT_EQ(outcome.frame, 999);
}

}  // namespace
