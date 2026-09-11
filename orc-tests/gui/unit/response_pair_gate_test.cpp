/*
 * File:        response_pair_gate_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests for the paired-response ordering guard
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "response_pair_gate.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

namespace orc::gui {
namespace {

using Gate = ResponsePairGate<std::string>;

TEST(ResponsePairGate, WaitsForBothHalvesOfAFrame) {
  Gate gate;
  gate.expectPair(1, 2);

  EXPECT_FALSE(gate.deliver(1, "first").has_value());

  const auto reading = gate.deliver(2, "second");
  ASSERT_TRUE(reading.has_value());
  EXPECT_EQ(reading->first, "first");
  EXPECT_EQ(reading->second, "second");
  EXPECT_TRUE(reading->has_second);
}

// The two requests are answered independently and nothing orders them.
TEST(ResponsePairGate, AcceptsTheTwoHalvesInEitherOrder) {
  Gate gate;
  gate.expectPair(1, 2);

  EXPECT_FALSE(gate.deliver(2, "second").has_value());

  const auto reading = gate.deliver(1, "first");
  ASSERT_TRUE(reading.has_value());
  EXPECT_EQ(reading->first, "first");
  EXPECT_EQ(reading->second, "second");
}

TEST(ResponsePairGate, CompletesASingleFieldModeOnOneResponse) {
  Gate gate;
  gate.expectSingle(7);

  const auto reading = gate.deliver(7, "only");
  ASSERT_TRUE(reading.has_value());
  EXPECT_EQ(reading->first, "only");
  EXPECT_FALSE(reading->has_second);
}

// The regression this gate exists for. During playback the GUI thread asks
// about the next frame from inside the preview callback, before the event loop
// has delivered the answers to the previous one - and with the vectorscope
// open that callback is long enough that this happens every single frame. A
// gate keyed on the frame currently being asked about accepted none of them,
// and the dialogue stopped updating for as long as the scope was open.
TEST(ResponsePairGate, KeepsUpdatingWhileQuestionsOutrunAnswers) {
  Gate gate;
  std::uint64_t next_id = 1;
  std::uint64_t answered_first = 0;
  std::uint64_t answered_second = 0;
  int readings = 0;

  for (int frame = 0; frame < 20; ++frame) {
    // The previous frame's answers arrive only now, one frame late.
    const std::uint64_t due_first = answered_first;
    const std::uint64_t due_second = answered_second;

    answered_first = next_id++;
    answered_second = next_id++;
    gate.expectPair(answered_first, answered_second);

    if (due_first != 0) {
      EXPECT_FALSE(gate.deliver(due_first, "f").has_value());
      if (gate.deliver(due_second, "s").has_value()) {
        ++readings;
      }
    }
  }

  EXPECT_EQ(readings, 19);
}

// A frame the user has moved past must not overwrite one already shown, even
// when its answers turn up afterwards.
TEST(ResponsePairGate, DropsAFrameALaterOneHasOvertaken) {
  Gate gate;
  gate.expectPair(1, 2);
  gate.expectPair(3, 4);

  gate.deliver(3, "new-first");
  const auto newer = gate.deliver(4, "new-second");
  ASSERT_TRUE(newer.has_value());

  EXPECT_FALSE(gate.deliver(1, "old-first").has_value());
  EXPECT_FALSE(gate.deliver(2, "old-second").has_value())
      << "an overtaken frame put the dialogue backwards";
}

// Completing a frame retires the older sets with it: their answers can only
// ever be overtaken, and holding them would leak.
TEST(ResponsePairGate, RetiresOlderSetsWhenAFrameCompletes) {
  Gate gate;
  gate.expectPair(1, 2);
  gate.expectPair(3, 4);
  gate.expectPair(5, 6);
  ASSERT_EQ(gate.outstanding(), 3u);

  gate.deliver(5, "a");
  ASSERT_TRUE(gate.deliver(6, "b").has_value());
  EXPECT_EQ(gate.outstanding(), 0u);
}

TEST(ResponsePairGate, IgnoresAResponseItNeverAskedFor) {
  Gate gate;
  gate.expectPair(1, 2);
  EXPECT_FALSE(gate.deliver(99, "stranger").has_value());

  gate.deliver(1, "first");
  EXPECT_TRUE(gate.deliver(2, "second").has_value());
}

TEST(ResponsePairGate, IgnoresARepeatOfAHalfAlreadyIn) {
  Gate gate;
  gate.expectPair(1, 2);
  EXPECT_FALSE(gate.deliver(1, "first").has_value());
  EXPECT_FALSE(gate.deliver(1, "first again").has_value());

  const auto reading = gate.deliver(2, "second");
  ASSERT_TRUE(reading.has_value());
  EXPECT_EQ(reading->first, "first");
}

// Asking about frame after frame while nothing is answered must not grow
// without bound; the oldest questions are never going to be answered.
TEST(ResponsePairGate, BoundsHowManyUnansweredFramesItHolds) {
  Gate gate;
  for (std::uint64_t frame = 0; frame < 100; ++frame) {
    gate.expectPair(frame * 2 + 1, frame * 2 + 2);
  }
  EXPECT_LE(gate.outstanding(), 8u);

  // The newest frame still completes.
  gate.deliver(199, "a");
  EXPECT_TRUE(gate.deliver(200, "b").has_value());
}

TEST(ResponsePairGate, ResetAcceptsAnyFrameAgain) {
  Gate gate;
  gate.expectPair(10, 11);
  gate.deliver(10, "a");
  ASSERT_TRUE(gate.deliver(11, "b").has_value());

  gate.reset();
  EXPECT_EQ(gate.outstanding(), 0u);

  // Ids that would have been stale before the reset are admitted now.
  gate.expectPair(1, 2);
  gate.deliver(1, "c");
  EXPECT_TRUE(gate.deliver(2, "d").has_value());
}

// Leaving a node clears the gate, and the answers the old node's requests were
// going to produce must not arrive on the cleared dialogue afterwards.
TEST(ResponsePairGate, ResetAbandonsTheAnswersStillInFlight) {
  Gate gate;
  gate.expectPair(1, 2);
  gate.deliver(1, "half");

  gate.reset();

  EXPECT_FALSE(gate.deliver(2, "other half").has_value());
  EXPECT_FALSE(gate.deliver(1, "half again").has_value());
}

// ---------------------------------------------------------------------------
// The observer dialogues' payload
//
// The VBI dialogue takes one view model per field; the Video Parameter and
// NTSC observer dialogues take a field id and an availability flag alongside
// theirs, and show nothing unless every field of the frame was observed. The
// gate carries whatever the consumer needs, so these check the shape those
// dialogues actually use.
// ---------------------------------------------------------------------------

namespace {

struct Observation {
  int field_id = 0;
  bool available = false;
};

using ObservationGate = ResponsePairGate<Observation>;

}  // namespace

TEST(ResponsePairGate, CarriesEachFieldsIdWithItsOwnAnswer) {
  ObservationGate gate;
  gate.expectPair(1, 2);

  // Out of order, which is how they arrive: the two requests are answered
  // independently.
  gate.deliver(2, Observation{101, true});
  const auto reading = gate.deliver(1, Observation{100, true});

  ASSERT_TRUE(reading.has_value());
  EXPECT_EQ(reading->first.field_id, 100);
  EXPECT_EQ(reading->second.field_id, 101);
}

// Half a frame's observations is not a frame; the dialogues clear rather than
// show a reading for one field as though it were the whole frame.
TEST(ResponsePairGate, ReportsAFrameWhoseSecondFieldWasNotObserved) {
  ObservationGate gate;
  gate.expectPair(1, 2);

  gate.deliver(1, Observation{100, true});
  const auto reading = gate.deliver(2, Observation{101, false});

  ASSERT_TRUE(reading.has_value())
      << "the caller decides what an unobserved field means, so the set still "
         "completes";
  EXPECT_TRUE(reading->first.available);
  EXPECT_FALSE(reading->second.available);
}

// Switching the preview between a frame mode and a field mode used to be held
// in a separate flag, so an answer from a frame-mode question that arrived
// after the switch was read as a field-mode reading. The mode now travels with
// the set it belongs to.
TEST(ResponsePairGate, KeepsEachSetsOwnIdeaOfHowManyFieldsItCovers) {
  ObservationGate gate;
  gate.expectPair(1, 2);  // frame mode
  gate.expectSingle(3);   // ...then the user switches to a field mode

  const auto single = gate.deliver(3, Observation{300, true});
  ASSERT_TRUE(single.has_value());
  EXPECT_FALSE(single->has_second);

  // The frame-mode pair is older than what has been shown, so it is dropped
  // rather than read as a field-mode answer.
  gate.deliver(1, Observation{100, true});
  EXPECT_FALSE(gate.deliver(2, Observation{101, true}).has_value());
}

}  // namespace
}  // namespace orc::gui
