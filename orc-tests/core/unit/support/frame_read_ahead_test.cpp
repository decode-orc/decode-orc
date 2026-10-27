/*
 * File:        frame_read_ahead_test.cpp
 * Module:      orc-tests/core/unit
 * Purpose:     Tests for FrameReadAhead: which frames its workers fetch, how
 *              far ahead of the caller, and that it stops cleanly
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/support/frame_read_ahead.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <set>
#include <stdexcept>
#include <vector>

namespace orc {
namespace tests {
namespace {

// Records every fetch. A fetch can be held at a gate until the test opens
// it; waits give up after a while so a broken helper fails rather than hangs.
class FetchLog {
 public:
  explicit FetchLog(bool gated = false) : open_(!gated) {}

  FrameReadAhead::Fetch fetch() {
    return [this](FrameID id) {
      std::unique_lock<std::mutex> lock(mutex_);
      calls_.push_back(id);
      changed_.notify_all();
      changed_.wait_for(lock, kPatience, [this] { return open_; });
    };
  }

  void open() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
    changed_.notify_all();
  }

  // Waits until |count| fetches have started.
  bool wait_for_calls(std::size_t count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, kPatience,
                             [&] { return calls_.size() >= count; });
  }

  std::vector<FrameID> calls() {
    std::lock_guard<std::mutex> lock(mutex_);
    return calls_;
  }

 private:
  static constexpr std::chrono::seconds kPatience{10};

  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<FrameID> calls_;
  bool open_;
};

std::set<FrameID> as_set(const std::vector<FrameID>& ids) {
  return {ids.begin(), ids.end()};
}

TEST(FrameReadAhead, FetchesTheWindowAheadOfTheCallerOnceEach) {
  FetchLog log;
  {
    FrameReadAhead read_ahead(10, 100, log.fetch(), /*threads=*/3,
                              /*window=*/4);
    ASSERT_TRUE(log.wait_for_calls(4));
  }
  // The caller reads frame 10 itself; nothing past 14 until it moves on.
  const auto calls = log.calls();
  EXPECT_EQ(calls.size(), 4u);
  EXPECT_EQ(as_set(calls), (std::set<FrameID>{11, 12, 13, 14}));
}

TEST(FrameReadAhead, SkipsFramesTheCallerHasPassed) {
  FetchLog log(/*gated=*/true);
  {
    FrameReadAhead read_ahead(10, 100, log.fetch(), /*threads=*/1,
                              /*window=*/4);
    ASSERT_TRUE(log.wait_for_calls(1));
    read_ahead.reached(16);
    log.open();
    ASSERT_TRUE(log.wait_for_calls(5));
  }
  EXPECT_EQ(log.calls(), (std::vector<FrameID>{11, 17, 18, 19, 20}));
}

TEST(FrameReadAhead, StopsAtTheEndOfTheRange) {
  FetchLog log;
  {
    FrameReadAhead read_ahead(0, 3, log.fetch(), /*threads=*/4,
                              /*window=*/16);
    ASSERT_TRUE(log.wait_for_calls(3));
  }
  EXPECT_EQ(as_set(log.calls()), (std::set<FrameID>{1, 2, 3}));
  EXPECT_EQ(log.calls().size(), 3u);
}

TEST(FrameReadAhead, DestroyingItPartWayJoinsItsWorkers) {
  FetchLog log;
  {
    FrameReadAhead read_ahead(0, 1000, log.fetch(), /*threads=*/4,
                              /*window=*/8);
    ASSERT_TRUE(log.wait_for_calls(8));
    // The workers are now waiting for the caller, which never comes.
  }
  const auto calls = log.calls();
  EXPECT_EQ(calls.size(), 8u);
  // Nothing runs once the helper has gone.
  EXPECT_EQ(log.calls(), calls);
}

TEST(FrameReadAhead, AFailingFetchDoesNotStopTheOthers) {
  FetchLog log;
  auto record = log.fetch();
  {
    FrameReadAhead read_ahead(
        0, 4,
        [&record](FrameID id) {
          record(id);
          if (id == 2) throw std::runtime_error("unreadable frame");
        },
        /*threads=*/1, /*window=*/8);
    ASSERT_TRUE(log.wait_for_calls(4));
  }
  EXPECT_EQ(log.calls(), (std::vector<FrameID>{1, 2, 3, 4}));
}

TEST(FrameReadAhead, NoThreadsFetchesNothing) {
  FetchLog log;
  {
    FrameReadAhead read_ahead(0, 10, log.fetch(), /*threads=*/0);
    read_ahead.reached(5);
  }
  EXPECT_TRUE(log.calls().empty());
}

TEST(FrameReadAhead, DefaultWindowIsTwicePerThreadUpToThirtyTwo) {
  EXPECT_EQ(FrameReadAhead::default_window(1), 2u);
  EXPECT_EQ(FrameReadAhead::default_window(8), 16u);
  EXPECT_EQ(FrameReadAhead::default_window(64), 32u);
}

}  // namespace
}  // namespace tests
}  // namespace orc
