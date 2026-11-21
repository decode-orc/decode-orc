/*
 * File:        sink_read_pacer_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     SinkReadPacer holds a reader that runs ahead of the slowest,
 *              never holds the slowest, and lets go on detach and release
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../orc/core/include/sink_read_pacer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace orc_unit_test {
namespace {

using orc::SinkReadPacer;
using namespace std::chrono_literals;

// A read of |id| by |reader| on its own thread, which reports when it has
// passed the pacer.
class PendingRead {
 public:
  PendingRead(SinkReadPacer& pacer, SinkReadPacer::Reader reader,
              orc::FrameID id)
      : thread_([this, &pacer, reader, id] {
          pacer.before_read(reader, id);
          passed_ = true;
        }) {}
  ~PendingRead() {
    if (thread_.joinable()) thread_.join();
  }

  // Whether the read has passed within |wait|.
  bool passed_within(std::chrono::milliseconds wait) {
    const auto until = std::chrono::steady_clock::now() + wait;
    while (!passed_ && std::chrono::steady_clock::now() < until) {
      std::this_thread::sleep_for(1ms);
    }
    return passed_;
  }

 private:
  std::atomic<bool> passed_{false};
  std::thread thread_;
};

}  // namespace

TEST(SinkReadPacerTest, ReadsWithinTheWindowOfTheSlowestPass) {
  SinkReadPacer pacer(0, 4, 100);
  const auto a = pacer.add_reader();
  pacer.add_reader();
  for (orc::FrameID id = 0; id <= 4; ++id) {
    pacer.before_read(a, id);  // returns at once
  }
}

TEST(SinkReadPacerTest, TheLeaderWaitsUntilTheSlowestCatchesUp) {
  SinkReadPacer pacer(0, 4, 100);
  const auto a = pacer.add_reader();
  const auto b = pacer.add_reader();
  pacer.before_read(a, 4);

  PendingRead ahead(pacer, a, 6);
  EXPECT_FALSE(ahead.passed_within(50ms));
  pacer.before_read(b, 1);  // 6 is still more than 4 past 1
  EXPECT_FALSE(ahead.passed_within(50ms));
  pacer.before_read(b, 2);
  EXPECT_TRUE(ahead.passed_within(5s));
}

TEST(SinkReadPacerTest, TheSlowestNeverWaits) {
  SinkReadPacer pacer(0, 4, 100);
  const auto a = pacer.add_reader();
  const auto b = pacer.add_reader();
  pacer.before_read(a, 4);
  // b is the slowest, so even a read far ahead of a passes.
  pacer.before_read(b, 50);
  // Now a is the slowest.
  pacer.before_read(a, 60);
}

TEST(SinkReadPacerTest, DetachingTheSlowestReleasesTheLeader) {
  SinkReadPacer pacer(0, 4, 100);
  const auto a = pacer.add_reader();
  const auto b = pacer.add_reader();
  pacer.before_read(a, 4);

  PendingRead ahead(pacer, a, 20);
  EXPECT_FALSE(ahead.passed_within(50ms));
  pacer.detach(b);
  EXPECT_TRUE(ahead.passed_within(5s));
}

TEST(SinkReadPacerTest, ReleaseLetsEveryReadThrough) {
  SinkReadPacer pacer(0, 4, 100);
  const auto a = pacer.add_reader();
  const auto b = pacer.add_reader();
  const auto c = pacer.add_reader();
  pacer.before_read(a, 4);
  pacer.before_read(b, 4);

  PendingRead first(pacer, a, 30);
  PendingRead second(pacer, b, 40);
  EXPECT_FALSE(first.passed_within(50ms));
  pacer.release();
  EXPECT_TRUE(first.passed_within(5s));
  EXPECT_TRUE(second.passed_within(5s));
  pacer.before_read(c, 1000);  // and later reads never wait
}

TEST(SinkReadPacerTest, ADetachedReaderNeverWaits) {
  SinkReadPacer pacer(0, 4, 100);
  const auto a = pacer.add_reader();
  pacer.add_reader();
  pacer.before_read(a, 4);
  pacer.detach(a);
  pacer.before_read(a, 500);
}

TEST(SinkReadPacerTest, AReaderThatStartsAgainHoldsTheOthersFromThere) {
  SinkReadPacer pacer(0, 4, 100);
  const auto a = pacer.add_reader();
  const auto b = pacer.add_reader();
  pacer.before_read(b, 500);  // the slowest, so it passes
  pacer.before_read(a, 500);
  // b's read of frame 0 is a new pass: it is far behind its last read.
  pacer.before_read(b, 0);

  PendingRead ahead(pacer, a, 501);
  EXPECT_FALSE(ahead.passed_within(50ms));
  pacer.before_read(b, 497);
  EXPECT_TRUE(ahead.passed_within(5s));
}

TEST(SinkReadPacerTest, ReadersStartAtTheFirstFrame) {
  SinkReadPacer pacer(1000, 4, 100);
  const auto a = pacer.add_reader();
  pacer.add_reader();  // set up, not reading yet
  pacer.before_read(a, 1004);
  PendingRead ahead(pacer, a, 1010);
  EXPECT_FALSE(ahead.passed_within(50ms));
  pacer.release();
  EXPECT_TRUE(ahead.passed_within(5s));
}

}  // namespace orc_unit_test
