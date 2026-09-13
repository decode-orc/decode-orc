/*
 * File:        pipe_io_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the "-" stdio convention helpers and the
 *              bounded producer/consumer queue in orc/support/pipe_io.h
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/support/pipe_io.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace orc::pipe_io;

// ---------------------------------------------------------------------------
// is_pipe_path
// ---------------------------------------------------------------------------

TEST(PipeIoIsPipePath, DashToken_IsAPipe) { EXPECT_TRUE(is_pipe_path("-")); }

// A path that cannot exist is neither the "-" token nor a real named pipe;
// this exercises the is_fifo() fallback without depending on any fixture.
TEST(PipeIoIsPipePath, OrdinaryNonExistentPath_IsNotAPipe) {
  EXPECT_FALSE(is_pipe_path("this/path/does/not/exist.cvbs"));
}

TEST(PipeIoIsPipePath, EmptyPath_IsNotAPipe) { EXPECT_FALSE(is_pipe_path("")); }

// ---------------------------------------------------------------------------
// to_libav_io_url
// ---------------------------------------------------------------------------

TEST(PipeIoToLibavIoUrl, Dash_TranslatesToPipeZero_ForInput) {
  EXPECT_EQ(to_libav_io_url("-", StdioDirection::INPUT), "pipe:0");
}

TEST(PipeIoToLibavIoUrl, Dash_TranslatesToPipeOne_ForOutput) {
  EXPECT_EQ(to_libav_io_url("-", StdioDirection::OUTPUT), "pipe:1");
}

TEST(PipeIoToLibavIoUrl, RegularPath_IsReturnedUnchanged) {
  EXPECT_EQ(to_libav_io_url("/tmp/capture.cvbs", StdioDirection::OUTPUT),
            "/tmp/capture.cvbs");
  EXPECT_EQ(to_libav_io_url("udp://239.0.0.1:1234", StdioDirection::OUTPUT),
            "udp://239.0.0.1:1234");
}

// ---------------------------------------------------------------------------
// BoundedPipeQueue
// ---------------------------------------------------------------------------

TEST(BoundedPipeQueue, PushThenPop_ReturnsSameItem) {
  BoundedPipeQueue<std::string> queue(4);
  queue.push("chunk-0");

  std::string out;
  ASSERT_TRUE(queue.pop(out));
  EXPECT_EQ(out, "chunk-0");
}

TEST(BoundedPipeQueue, Pop_DrainsRemainingItems_AfterCloseProducer) {
  BoundedPipeQueue<int> queue(4);
  queue.push(1);
  queue.push(2);
  queue.close_producer();

  int out = 0;
  ASSERT_TRUE(queue.pop(out));
  EXPECT_EQ(out, 1);
  ASSERT_TRUE(queue.pop(out));
  EXPECT_EQ(out, 2);

  // Drained and closed: pop() now returns false without blocking.
  EXPECT_FALSE(queue.pop(out));
  EXPECT_FALSE(queue.failed());
}

TEST(BoundedPipeQueue, Pop_ReturnsFalseImmediately_WhenClosedAndEmpty) {
  BoundedPipeQueue<int> queue(4);
  queue.close_producer();

  int out = 0;
  EXPECT_FALSE(queue.pop(out));
}

TEST(BoundedPipeQueue, Fail_UnblocksAWaitingPop) {
  BoundedPipeQueue<int> queue(4);
  std::atomic<bool> pop_returned{false};
  std::atomic<bool> pop_result{true};

  std::thread consumer([&] {
    int out = 0;
    pop_result.store(queue.pop(out));
    pop_returned.store(true);
  });

  // Give the consumer a chance to block in pop() on the empty queue before
  // failing it — a race either way is harmless: fail() is safe to call
  // before or after the consumer starts waiting.
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  queue.fail();
  consumer.join();

  EXPECT_TRUE(pop_returned.load());
  EXPECT_FALSE(pop_result.load());
  EXPECT_TRUE(queue.failed());
}

TEST(BoundedPipeQueue, Fail_UnblocksAWaitingPush) {
  BoundedPipeQueue<int> queue(1);
  queue.push(1);  // fill the one slot

  std::atomic<bool> push_returned{false};
  std::thread producer([&] {
    queue.push(2);  // blocks: queue is at capacity
    push_returned.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(push_returned.load());

  queue.fail();
  producer.join();

  EXPECT_TRUE(push_returned.load());
  EXPECT_TRUE(queue.failed());
}

TEST(BoundedPipeQueue, Push_UnblocksOnceConsumerMakesRoom) {
  BoundedPipeQueue<int> queue(1);
  queue.push(1);  // fill the one slot

  std::atomic<bool> push_returned{false};
  std::thread producer([&] {
    queue.push(2);  // blocks until the consumer pops the first item
    push_returned.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(push_returned.load());

  int out = 0;
  ASSERT_TRUE(queue.pop(out));
  EXPECT_EQ(out, 1);

  producer.join();
  EXPECT_TRUE(push_returned.load());

  ASSERT_TRUE(queue.pop(out));
  EXPECT_EQ(out, 2);
}
