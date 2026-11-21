/*
 * File:        frame_cache_test.cpp
 * Module:      orc-tests/core/unit
 * Purpose:     Tests for FrameCache: eviction under a pinned pointer, one
 *              build per key, failed builds, and capacity from the readers'
 *              span
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/support/frame_cache.h>
#include <orc/support/frame_read_ahead.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace orc {
namespace tests {
namespace {

using Samples = std::vector<int16_t>;
using Cache = FrameCache<Samples>;

constexpr auto kPatience = std::chrono::seconds(10);

// Holds a build until the test opens it.
class Gate {
 public:
  void enter() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++entered_;
    changed_.notify_all();
    changed_.wait_for(lock, kPatience, [this] { return open_; });
  }
  bool wait_entered(int count) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, kPatience,
                             [&] { return entered_ >= count; });
  }
  void open() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_ = true;
    }
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  int entered_ = 0;
  bool open_ = false;
};

// Waits until |cache| has a build of some key under way, by polling, which
// is all a test can see of a waiter that is blocked.
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(50)); }

TEST(FrameCacheTest, BuildsMissingFrameAndServesItAfterwards) {
  Cache cache(4);
  int builds = 0;
  const auto build = [&] {
    ++builds;
    return Samples{1, 2, 3};
  };
  const auto first = cache.get(7, build);
  const auto second = cache.get(7, build);
  ASSERT_TRUE(first);
  EXPECT_EQ(first, second);
  EXPECT_EQ(builds, 1);
  EXPECT_EQ(cache.find(7), first);
  EXPECT_EQ(cache.find(8), nullptr);
}

TEST(FrameCacheTest, EvictsLeastRecentlyUsedAtCapacity) {
  Cache cache(2);
  cache.get(1, [] { return Samples{1}; });
  cache.get(2, [] { return Samples{2}; });
  (void)cache.find(1);  // 2 is now the least recently used
  cache.get(3, [] { return Samples{3}; });
  EXPECT_EQ(cache.size(), 2u);
  EXPECT_NE(cache.find(1), nullptr);
  EXPECT_EQ(cache.find(2), nullptr);
  EXPECT_NE(cache.find(3), nullptr);
}

TEST(FrameCacheTest, PinnedPointerOutlivesEviction) {
  Cache cache(1);
  auto entry = cache.get(1, [] { return Samples(1000, 42); });
  const int16_t* data = cache.pin(entry, entry->data());
  entry.reset();
  // Evict frame 1 while the pointer is held.
  cache.get(2, [] { return Samples(1000, 7); });
  ASSERT_EQ(cache.find(1), nullptr);
  for (size_t i = 0; i < 1000; ++i) ASSERT_EQ(data[i], 42);
}

TEST(FrameCacheTest, SlotsPinIndependently) {
  Cache cache(1);
  auto luma = cache.get(1, [] { return Samples(100, 1); });
  const int16_t* luma_data = cache.pin(luma, luma->data(), 0);
  luma.reset();
  auto chroma = cache.get(2, [] { return Samples(100, 2); });
  const int16_t* chroma_data = cache.pin(chroma, chroma->data(), 1);
  chroma.reset();
  cache.get(3, [] { return Samples(100, 3); });  // evicts both
  EXPECT_EQ(luma_data[99], 1);
  EXPECT_EQ(chroma_data[99], 2);
}

TEST(FrameCacheTest, TwoThreadsWantingOneFrameBuildItOnce) {
  Cache cache(4);
  Gate gate;
  std::atomic<int> builds{0};
  const auto build = [&] {
    ++builds;
    gate.enter();
    return Samples{5};
  };
  Cache::Entry a, b;
  std::thread first([&] { a = cache.get(9, build); });
  ASSERT_TRUE(gate.wait_entered(1));
  std::thread second([&] { b = cache.get(9, build); });
  settle();
  gate.open();
  first.join();
  second.join();
  EXPECT_EQ(builds.load(), 1);
  ASSERT_TRUE(a);
  EXPECT_EQ(a, b);
}

TEST(FrameCacheTest, FailedBuildReleasesWaitersAndOneOfThemBuilds) {
  Cache cache(4);
  Gate gate;
  std::atomic<int> builds{0};
  bool first_threw = false;
  Cache::Entry second_entry;
  std::thread first([&] {
    try {
      cache.get(3, [&]() -> Samples {
        ++builds;
        gate.enter();
        throw std::runtime_error("read failed");
      });
    } catch (const std::runtime_error&) {
      first_threw = true;
    }
  });
  ASSERT_TRUE(gate.wait_entered(1));
  std::thread second([&] {
    second_entry = cache.get(3, [&] {
      ++builds;
      return Samples{8};
    });
  });
  settle();
  gate.open();
  first.join();
  second.join();
  EXPECT_TRUE(first_threw);
  ASSERT_TRUE(second_entry);
  EXPECT_EQ((*second_entry)[0], 8);
  EXPECT_EQ(builds.load(), 2);
}

TEST(FrameCacheTest, NullBuildIsNotCached) {
  Cache cache(4);
  int builds = 0;
  const auto build = [&]() -> Cache::Entry {
    ++builds;
    return nullptr;
  };
  EXPECT_EQ(cache.get(1, build), nullptr);
  EXPECT_EQ(cache.get(1, build), nullptr);
  EXPECT_EQ(builds, 2);
  EXPECT_EQ(cache.size(), 0u);
}

TEST(FrameCacheTest, InsertKeepsTheCachedEntry) {
  Cache cache(4);
  const auto kept = cache.insert(1, std::make_shared<const Samples>(1, 1));
  const auto again = cache.insert(1, std::make_shared<const Samples>(1, 2));
  EXPECT_EQ(kept, again);
  EXPECT_EQ((*cache.find(1))[0], 1);
}

TEST(FrameCacheTest, ManyThreadsAtCapacityTwoReadOnlyLiveFrames) {
  // The ORC_FRAME_CACHE_FRAMES=2 case in miniature: readers keep pointers
  // across other reads while frames are evicted under them.
  Cache cache(2);
  std::vector<std::thread> readers;
  std::atomic<bool> wrong{false};
  for (int t = 0; t < 8; ++t) {
    readers.emplace_back([&, t] {
      for (int round = 0; round < 200; ++round) {
        const FrameID id = static_cast<FrameID>((round + t) % 7);
        auto entry = cache.get(
            id, [id] { return Samples(512, static_cast<int16_t>(id)); });
        const int16_t* data = cache.pin(entry, entry->data());
        entry.reset();
        (void)cache.get(id + 100, [] { return Samples(512, -1); });
        if (data[511] != static_cast<int16_t>(id)) wrong = true;
      }
    });
  }
  for (auto& reader : readers) reader.join();
  EXPECT_FALSE(wrong.load());
  EXPECT_LE(cache.size(), 2u);
}

// ----------------------------------------------------------------------------
// Capacity
// ----------------------------------------------------------------------------

TEST(FrameCacheCapacityTest, CoversTheSpanOfItsReaders) {
  for (const unsigned threads : {4u, 16u, 64u}) {
    const unsigned workers = std::min(threads, kMaxAutoFrameWorkers);
    const size_t capacity = frame_cache_capacity_for(workers);
    const size_t span =
        2 * workers + kMaxDecoderLookAroundFrames +
        static_cast<size_t>(FrameReadAhead::default_window(threads)) +
        kStackerReachFrames;
    EXPECT_GE(capacity, span) << threads << " threads";
    // A sink given that bound keeps that many workers.
    EXPECT_EQ(frame_workers_within(capacity), workers) << threads;
  }
}

TEST(FrameCacheCapacityTest, SixtyFourThreadSinkIsCappedToItsCaches) {
  // A host with 64 threads starts at most kMaxAutoFrameWorkers workers, and
  // an explicit request for 64 is cut to what the caches were sized for.
  const size_t bound = frame_cache_capacity_for(kMaxAutoFrameWorkers);
  EXPECT_EQ(frame_workers_within(bound), kMaxAutoFrameWorkers);
  EXPECT_LT(frame_workers_within(bound), 64u);
}

TEST(FrameCacheCapacityTest, SmallBoundsLeaveOneWorker) {
  EXPECT_EQ(frame_workers_within(0), 1u);
  EXPECT_EQ(frame_workers_within(frame_cache_capacity_for(1)), 1u);
  EXPECT_EQ(frame_workers_within(frame_cache_capacity_for(2)), 2u);
  EXPECT_EQ(frame_workers_within(frame_cache_capacity_for(2) + 1), 2u);
}

TEST(FrameCacheCapacityTest, NominalCapacityIsAboveTheReadAheadWindow) {
  EXPECT_GT(nominal_frame_cache_capacity(), kMaxReadAheadFrames);
  EXPECT_LE(FrameReadAhead::default_window(1024),
            static_cast<FrameID>(kMaxReadAheadFrames));
  EXPECT_LE(default_frame_workers(), kMaxAutoFrameWorkers);
}

}  // namespace
}  // namespace tests
}  // namespace orc
