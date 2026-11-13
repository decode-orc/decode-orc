/*
 * File:        frame_claim_window_test.cpp
 * Module:      orc-core-unit-tests
 * Purpose:     Unit tests for the video sink's bound on decoded-but-unwritten
 *              frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/sinks/common/frame_claim_window.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace orc_unit_test {
namespace {

// The export's worker loop in miniature: claim, "decode", deposit, and write
// whatever is next in order. Frame |slow| takes far longer than the rest.
struct ExportRun {
  int64_t frames_held_max = 0;
  std::vector<int64_t> written;
};

ExportRun run_export(int workers, int64_t frames, int64_t max_ahead,
                     int64_t slow, std::chrono::milliseconds slow_time) {
  orc::FrameClaimWindow window(0, max_ahead);
  std::mutex output_mutex;
  std::map<int64_t, int64_t> held;  // decoded, not yet written
  int64_t next_to_write = 0;
  ExportRun run;

  const auto worker = [&] {
    for (;;) {
      const auto claim =
          window.claim([] { return false; },
                       [&](int64_t candidate) { return candidate >= frames; });
      if (!claim || *claim >= frames) return;
      if (*claim == slow) std::this_thread::sleep_for(slow_time);

      std::lock_guard<std::mutex> lock(output_mutex);
      held[*claim] = *claim;
      run.frames_held_max =
          std::max(run.frames_held_max, static_cast<int64_t>(held.size()));
      while (held.count(next_to_write) != 0) {
        run.written.push_back(held[next_to_write]);
        held.erase(next_to_write);
        ++next_to_write;
      }
      window.written(next_to_write);
    }
  };

  std::vector<std::thread> threads;
  for (int i = 0; i < workers; ++i) threads.emplace_back(worker);
  for (auto& t : threads) t.join();
  return run;
}

}  // namespace

// One deliberately slow frame: the other workers stop claiming once the
// window is full, so the frames held behind it never pass the bound, and
// every frame is still written in order.
TEST(FrameClaimWindowTest, SlowFrameKeepsHeldFramesWithinTheBound) {
  constexpr int kWorkers = 8;
  constexpr int64_t kFrames = 400;
  constexpr int64_t kMaxAhead = 2 * kWorkers;
  const auto run = run_export(kWorkers, kFrames, kMaxAhead, /*slow=*/20,
                              std::chrono::milliseconds(300));

  EXPECT_LE(run.frames_held_max, kMaxAhead);
  ASSERT_EQ(static_cast<int64_t>(run.written.size()), kFrames);
  for (int64_t i = 0; i < kFrames; ++i) EXPECT_EQ(run.written[i], i);
}

// A stop request releases every waiting claim.
TEST(FrameClaimWindowTest, StopReleasesWaitingClaims) {
  orc::FrameClaimWindow window(0, 1);
  ASSERT_EQ(window.claim([] { return false; }), 0);  // window now full
  std::atomic<bool> stop{false};
  std::thread waiter([&] {
    EXPECT_FALSE(window.claim([&] { return stop.load(); }).has_value());
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  stop.store(true);
  window.wake();
  waiter.join();
}

// The frame past the end is handed out at once, so a worker can learn the
// stream is over while the window is full.
TEST(FrameClaimWindowTest, UnboundedClaimIsNotHeldToTheBound) {
  orc::FrameClaimWindow window(0, 1);
  ASSERT_EQ(window.claim([] { return false; }), 0);
  const auto past_end = window.claim(
      [] { return false; }, [](int64_t candidate) { return candidate >= 1; });
  EXPECT_EQ(past_end, 1);
}

}  // namespace orc_unit_test
