/*
 * File:        response_sequence_gate_test.cpp
 * Module:      orc-gui tests
 * Purpose:     Ordering guard for asynchronous render-coordinator responses
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "response_sequence_gate.h"

#include <gtest/gtest.h>

namespace orc::gui {
namespace {

TEST(ResponseSequenceGate, AdmitsTheFirstResponse) {
  ResponseSequenceGate gate;
  EXPECT_EQ(gate.newestAdmitted(), 0u);
  EXPECT_TRUE(gate.admit(1));
  EXPECT_EQ(gate.newestAdmitted(), 1u);
}

TEST(ResponseSequenceGate, AdmitsResponsesInIncreasingOrder) {
  ResponseSequenceGate gate;
  EXPECT_TRUE(gate.admit(4));
  EXPECT_TRUE(gate.admit(5));
  EXPECT_TRUE(gate.admit(9));
}

TEST(ResponseSequenceGate, DropsAResponseALaterOneHasOvertaken) {
  ResponseSequenceGate gate;
  ASSERT_TRUE(gate.admit(9));
  EXPECT_FALSE(gate.admit(8)) << "an older extraction must not redraw over a "
                                 "newer one";
  EXPECT_EQ(gate.newestAdmitted(), 9u);
}

TEST(ResponseSequenceGate, DropsARepeatOfTheNewestResponse) {
  ResponseSequenceGate gate;
  ASSERT_TRUE(gate.admit(3));
  EXPECT_FALSE(gate.admit(3));
}

TEST(ResponseSequenceGate, KeepsAdmittingWhileRequestsOutrunResponses) {
  // The regression this gate exists for: during playback the GUI thread
  // issues the next frame's request from inside the preview callback, before
  // the event loop delivers the response to the previous one. Every response
  // therefore arrives while a newer request is already in flight, and a gate
  // keyed on the in-flight request would admit none of them.
  ResponseSequenceGate gate;
  std::uint64_t in_flight = 0;
  int admitted = 0;
  for (std::uint64_t frame = 1; frame <= 20; ++frame) {
    const std::uint64_t response = in_flight;  // answer to the previous frame
    in_flight = frame;                         // ...requested before it lands
    if (response != 0 && gate.admit(response)) {
      ++admitted;
    }
  }
  EXPECT_EQ(admitted, 19);
}

TEST(ResponseSequenceGate, ResetAdmitsAnyResponseAgain) {
  ResponseSequenceGate gate;
  ASSERT_TRUE(gate.admit(100));
  gate.reset();
  EXPECT_EQ(gate.newestAdmitted(), 0u);
  EXPECT_TRUE(gate.admit(1));
}

}  // namespace
}  // namespace orc::gui
