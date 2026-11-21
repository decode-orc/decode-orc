/*
 * File:        segment_measurements_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the Stacker's per-segment measurements: one
 *              measurement per segment, and no reader of another segment
 *              held up by it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>

#include "../../../../orc/plugins/stages/stacker/stacker_stage.h"

namespace orc_unit_test {
namespace {

using orc::SegmentMeasurements;
using namespace std::chrono_literals;

// A measurement that holds until released.
class HeldMeasurement {
 public:
  int operator()() {
    started_.set_value();
    release_.get_future().wait();
    ++calls;
    return 42;
  }
  void wait_started() { started_.get_future().wait(); }
  void release() { release_.set_value(); }

  std::atomic<int> calls{0};

 private:
  std::promise<void> started_;
  std::promise<void> release_;
};

TEST(SegmentMeasurementsTest, AMeasuredSegmentIsReadWhileAnotherIsMeasured) {
  SegmentMeasurements<int> segments;
  EXPECT_EQ(segments.get(0, [] { return 7; }), 7);

  HeldMeasurement held;
  auto measuring =
      std::async(std::launch::async, [&] { return segments.get(1, held); });
  held.wait_started();

  // Segment 0 is read, and segment 2 measured, while segment 1 is held.
  auto reading = std::async(std::launch::async, [&] {
    return segments.get(0, []() -> int { throw std::logic_error("again"); });
  });
  ASSERT_EQ(reading.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(reading.get(), 7);
  auto other = std::async(std::launch::async,
                          [&] { return segments.get(2, [] { return 9; }); });
  ASSERT_EQ(other.wait_for(5s), std::future_status::ready);
  EXPECT_EQ(other.get(), 9);
  EXPECT_FALSE(segments.measured(1));

  held.release();
  EXPECT_EQ(measuring.get(), 42);
  EXPECT_TRUE(segments.measured(1));
}

TEST(SegmentMeasurementsTest, ASegmentIsMeasuredOnceForEveryReader) {
  SegmentMeasurements<int> segments;
  HeldMeasurement held;
  auto first =
      std::async(std::launch::async, [&] { return segments.get(3, held); });
  held.wait_started();

  std::atomic<int> second_calls{0};
  auto second = std::async(std::launch::async, [&] {
    return segments.get(3, [&] {
      ++second_calls;
      return 0;
    });
  });
  // The second reader waits for the first's measurement.
  EXPECT_EQ(second.wait_for(100ms), std::future_status::timeout);

  held.release();
  EXPECT_EQ(first.get(), 42);
  EXPECT_EQ(second.get(), 42);
  EXPECT_EQ(held.calls, 1);
  EXPECT_EQ(second_calls, 0);
}

TEST(SegmentMeasurementsTest, AMeasurementMayReadAnotherSegment) {
  SegmentMeasurements<int> segments;
  const int value =
      segments.get(5, [&] { return segments.get(4, [] { return 1; }) + 1; });
  EXPECT_EQ(value, 2);
  EXPECT_TRUE(segments.measured(4));
}

TEST(SegmentMeasurementsTest, AFailedMeasurementLeavesTheSegmentUnmeasured) {
  SegmentMeasurements<int> segments;
  EXPECT_THROW(
      segments.get(0, []() -> int { throw std::runtime_error("read failed"); }),
      std::runtime_error);
  EXPECT_FALSE(segments.measured(0));
  EXPECT_EQ(segments.get(0, [] { return 3; }), 3);
}

}  // namespace
}  // namespace orc_unit_test
