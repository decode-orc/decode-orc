/*
 * File:        closed_caption_cues_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the CC1 cues the video sink embeds, taken from
 *              line 21 as the Closed Caption Sink takes it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "../../../../orc/plugins/stages/sinks/common/closed_caption_cues.h"

#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace orc_unit_test {
namespace {

constexpr double kFramesPerSecond = 30000.0 / 1001.0;

// CTA-608-E Table 52, data channel 1 (field 1's spelling).
constexpr uint8_t kMisc = 0x14;
constexpr uint8_t kResumeCaptionLoading = 0x20;
constexpr uint8_t kEndOfCaption = 0x2F;
constexpr uint8_t kPacRow15 = 0x70;  // Table 53: row 15, column 0

struct Field {
  uint8_t data0 = 0;
  uint8_t data1 = 0;
  bool parity0 = true;
  bool parity1 = true;
};

// The closed caption observer's observations of one field.
void observe(orc::ObservationContext& context, uint64_t field,
             const Field& pair) {
  const orc::FieldID id(field);
  context.set(id, "closed_caption", "present", true);
  context.set(id, "closed_caption", "data0", static_cast<int32_t>(pair.data0));
  context.set(id, "closed_caption", "data1", static_cast<int32_t>(pair.data1));
  context.set(id, "closed_caption", "parity0_valid", pair.parity0);
  context.set(id, "closed_caption", "parity1_valid", pair.parity1);
}

// A pop-on caption reading |text|, one pair a frame from |first_frame| on the
// stored field |stored_field|, then null padding to |frames|; the other field
// carries null padding throughout. Returns the frame after the caption.
uint64_t send_caption(orc::ObservationContext& context, uint64_t first_frame,
                      int stored_field, const std::string& text) {
  std::vector<Field> pairs = {{kMisc, kResumeCaptionLoading},
                              {kMisc, kPacRow15}};
  for (size_t i = 0; i < text.size(); i += 2) {
    pairs.push_back(
        {static_cast<uint8_t>(text[i]),
         static_cast<uint8_t>(i + 1 < text.size() ? text[i + 1] : ' ')});
  }
  pairs.push_back({kMisc, kEndOfCaption});
  uint64_t frame = first_frame;
  for (const Field& pair : pairs) {
    observe(context, frame * 2 + static_cast<uint64_t>(stored_field), pair);
    observe(context, frame * 2 + static_cast<uint64_t>(1 - stored_field), {});
    ++frame;
  }
  return frame;
}

// Null padding on both fields of frames [first, last).
void send_padding(orc::ObservationContext& context, uint64_t first,
                  uint64_t last) {
  for (uint64_t frame = first; frame < last; ++frame) {
    observe(context, frame * 2, {});
    observe(context, frame * 2 + 1, {});
  }
}

std::vector<std::string> texts(const orc::ClosedCaptionCues& captions) {
  std::vector<std::string> out;
  for (const auto& cue : captions.cues) {
    out.push_back(cue.text);
  }
  return out;
}

TEST(ClosedCaptionCues, DecodesCC1FromTheFirstStoredField) {
  orc::ObservationContext context;
  const uint64_t end = send_caption(context, 0, 0, "HI");
  send_padding(context, end, end + 30);

  const auto captions =
      orc::closed_caption_cues(context, 0, (end + 30) * 2, kFramesPerSecond);
  EXPECT_FALSE(captions.fields_reversed);
  EXPECT_EQ(texts(captions), std::vector<std::string>{"HI"});
}

// A capture whose first stored field is television field 2 carries CC1 on the
// second stored field, spelled as field 1 spells its control codes.
TEST(ClosedCaptionCues, FindsCC1OnAReversedCapture) {
  orc::ObservationContext context;
  uint64_t frame = 0;
  // Five captions of one length: a pop-on caption is written over what the
  // memory it is loaded into last held.
  for (const char* text : {"ONE", "TWO", "SIX", "TEN", "FEW"}) {
    frame = send_caption(context, frame, 1, text);
  }
  send_padding(context, frame, frame + 30);

  const auto captions =
      orc::closed_caption_cues(context, 0, (frame + 30) * 2, kFramesPerSecond);
  EXPECT_TRUE(captions.fields_reversed);
  EXPECT_EQ(texts(captions),
            (std::vector<std::string>{"ONE", "TWO", "SIX", "TEN", "FEW"}));
}

// A pair whose first byte failed its parity may have been a control code and
// is not decoded; a character whose partner failed is decoded alone.
TEST(ClosedCaptionCues, TakesPairsByTheirParity) {
  orc::ObservationContext context;
  send_padding(context, 0, 10);
  observe(context, 20, {kMisc, kResumeCaptionLoading});
  observe(context, 22, {kMisc, kPacRow15});
  observe(context, 24, {'A', 'B', false, true});
  observe(context, 26, {'C', 'D', true, false});
  observe(context, 28, {'E', 'F'});
  observe(context, 30, {kMisc, kEndOfCaption});
  send_padding(context, 16, 40);

  const auto captions =
      orc::closed_caption_cues(context, 0, 80, kFramesPerSecond);
  EXPECT_EQ(texts(captions), std::vector<std::string>{"CEF"});
}

// A line of noise that reads as a defined pair, alone in a field that carries
// no line 21 data, is not decoded.
TEST(ClosedCaptionCues, IgnoresAPairStandingAlone) {
  orc::ObservationContext context;
  observe(context, 0, {kMisc, kResumeCaptionLoading});
  observe(context, 200, {'X', 'Y'});
  observe(context, 400, {kMisc, kEndOfCaption});

  const auto captions =
      orc::closed_caption_cues(context, 0, 500, kFramesPerSecond);
  EXPECT_TRUE(captions.cues.empty());
  EXPECT_EQ(captions.pairs, 0u);
}

}  // namespace
}  // namespace orc_unit_test
