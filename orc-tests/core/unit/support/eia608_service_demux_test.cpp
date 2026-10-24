/*
 * File:        eia608_service_demux_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the EIA-608 line 21 service demultiplexer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/support/eia608_decoder.h>
#include <orc/support/eia608_service_demux.h>

#include <string>
#include <utility>
#include <vector>

namespace orc_unit_test {
namespace {

using orc::EIA608Service;
using orc::EIA608ServiceDemux;

// Field 1 control-code first bytes. Bit 3 is the data channel: 0x14 is a
// channel 1 miscellaneous control code, 0x1C is its channel 2 counterpart.
constexpr uint8_t kMiscC1 = 0x14;
constexpr uint8_t kMiscC2 = 0x1C;

// Miscellaneous control codes (CTA-608-E Table 52)
constexpr uint8_t kRCL = 0x20;  // Resume Caption Loading -> caption service
constexpr uint8_t kEDM = 0x2C;  // Erase Displayed Memory
constexpr uint8_t kENM = 0x2E;  // Erase Non-Displayed Memory
constexpr uint8_t kCR = 0x2D;   // Carriage Return
constexpr uint8_t kEOC = 0x2F;  // End of Caption
constexpr uint8_t kTR = 0x2A;   // Text Restart -> text service
constexpr uint8_t kRTD = 0x2B;  // Resume Text Display -> text service

// One byte pair of the field 1 stream.
struct Pair {
  uint8_t byte1;
  uint8_t byte2;
};

// Push a stream through the demux and collect the characters that survived.
std::string routed_text(EIA608Service target, const std::vector<Pair>& stream,
                        bool suppress_repeated_controls = true) {
  EIA608ServiceDemux demux(target, suppress_repeated_controls);
  std::string out;
  for (const Pair& pair : stream) {
    if (!demux.accept(0, pair.byte1, pair.byte2)) {
      continue;
    }
    if (pair.byte1 < 0x20) {
      continue;  // a control pair, not text
    }
    out += static_cast<char>(pair.byte1);
    out += static_cast<char>(pair.byte2);
  }
  return out;
}

// The failure the sink was reported for: a caption service and a text service
// sharing field 1, their characters alternating pair for pair.
std::vector<Pair> interleaved_stream() {
  return {
      {kMiscC1, kRCL},  // CC1 takes channel 1 into captioning
      {kMiscC2, kTR},   // T2 takes channel 2 into its text service
      {'H', 'I'},       // ... belongs to whichever channel spoke last: C2 = T2
      {kMiscC1, kRCL},  // back to channel 1
      {'D', 'A'},       // CC1
      {kMiscC2, kRTD},  // channel 2 again
      {'S', 'C'},       // T2
      {kMiscC1, kRCL}, {'D', 'Y'},  // CC1
  };
}

}  // namespace

TEST(EIA608ServiceDemuxTest, NamesRoundTrip) {
  for (const auto service :
       {EIA608Service::CC1, EIA608Service::CC2, EIA608Service::CC3,
        EIA608Service::CC4, EIA608Service::T1, EIA608Service::T2,
        EIA608Service::T3, EIA608Service::T4}) {
    const auto parsed =
        orc::eia608_service_from_name(orc::eia608_service_name(service));
    ASSERT_TRUE(parsed.has_value()) << orc::eia608_service_name(service);
    EXPECT_EQ(*parsed, service);
  }

  // The parameter surfaces spell the text services TEXT1-TEXT4.
  EXPECT_EQ(orc::eia608_service_from_name("TEXT1"), EIA608Service::T1);
  EXPECT_EQ(orc::eia608_service_from_name("TEXT2"), EIA608Service::T2);
  EXPECT_FALSE(orc::eia608_service_from_name("CC9").has_value());
}

TEST(EIA608ServiceDemuxTest, FieldAndTextClassification) {
  EXPECT_EQ(orc::eia608_service_field(EIA608Service::CC1), 0);
  EXPECT_EQ(orc::eia608_service_field(EIA608Service::T2), 0);
  EXPECT_EQ(orc::eia608_service_field(EIA608Service::CC3), 1);
  EXPECT_EQ(orc::eia608_service_field(EIA608Service::T4), 1);

  EXPECT_FALSE(orc::eia608_service_is_text(EIA608Service::CC1));
  EXPECT_TRUE(orc::eia608_service_is_text(EIA608Service::T1));
}

TEST(EIA608ServiceDemuxTest, SeparatesACaptionServiceFromATextService) {
  // This is issue #273: read undemultiplexed, the stream reads "HIDASCDY".
  EXPECT_EQ(routed_text(EIA608Service::CC1, interleaved_stream()), "DADY");
  EXPECT_EQ(routed_text(EIA608Service::T2, interleaved_stream()), "HISC");

  // Neither of the other two field 1 services said anything.
  EXPECT_EQ(routed_text(EIA608Service::CC2, interleaved_stream()), "");
  EXPECT_EQ(routed_text(EIA608Service::T1, interleaved_stream()), "");
}

TEST(EIA608ServiceDemuxTest, CharactersFollowTheChannelOfTheLastControlPair) {
  const std::vector<Pair> stream = {
      {kMiscC1, kRCL}, {'A', 'B'}, {kMiscC2, kRCL}, {'C', 'D'}, {'E', 'F'},
  };

  EXPECT_EQ(routed_text(EIA608Service::CC1, stream), "AB");
  // Both character pairs after the channel 2 control code belong to CC2.
  EXPECT_EQ(routed_text(EIA608Service::CC2, stream), "CDEF");
}

TEST(EIA608ServiceDemuxTest, TextRestartAndCaptionCodesSwitchWithinAChannel) {
  const std::vector<Pair> stream = {
      {kMiscC1, kRCL},  // channel 1 -> captioning
      {'A', 'B'},       // CC1
      {kMiscC1, kTR},   // channel 1 -> text
      {'C', 'D'},       // T1
      {kMiscC1, kRCL},  // channel 1 -> captioning again
      {'E', 'F'},       // CC1
  };

  EXPECT_EQ(routed_text(EIA608Service::CC1, stream), "ABEF");
  EXPECT_EQ(routed_text(EIA608Service::T1, stream), "CD");
}

TEST(EIA608ServiceDemuxTest, CodesThatAreNeitherLeaveTheSelectionAlone) {
  // CR belongs to whichever service the channel is already on, and EDM, which
  // erases the captions' memory, leaves a text stream running [CTA-608-E
  // C.16]: neither pulls the channel back to captioning.
  const std::vector<Pair> stream = {
      {kMiscC1, kTR},
      {kMiscC1, kCR},
      {kMiscC1, kEDM},
      {'A', 'B'},
  };

  EXPECT_EQ(routed_text(EIA608Service::T1, stream), "AB");
  EXPECT_EQ(routed_text(EIA608Service::CC1, stream), "");
}

// End of Caption flips the channel's caption memories even in the midst of a
// Text Mode stream [CTA-608-E 7.7, C.16]: routed to the text service, the
// caption it was sent to put up never was. The text stream keeps the channel:
// a text service can send EDM, EOC, EDM inside it, and the rows after a lost
// RTD are still text.
TEST(EIA608ServiceDemuxTest, EndOfCaptionReachesTheCaptionsInTheMidstOfText) {
  const std::vector<Pair> stream = {
      {kMiscC1, kTR},  {'A', 'B'}, {kMiscC1, kEDM}, {kMiscC1, kEOC},
      {kMiscC1, kEDM}, {'C', 'D'}, {kMiscC1, kRTD}, {'E', 'F'},
  };
  EXPECT_EQ(routed_text(EIA608Service::T1, stream), "ABCDEF");
  EXPECT_EQ(routed_text(EIA608Service::CC1, stream), "");

  EIA608ServiceDemux captions(EIA608Service::CC1);
  EXPECT_FALSE(captions.accept(0, kMiscC1, kTR));
  EXPECT_TRUE(captions.accept(0, kMiscC1, kEOC));
  EXPECT_FALSE(captions.accept(0, 'G', 'H'));
}

// Erase Displayed Memory and Erase Non-Displayed Memory erase the channel's
// caption memories even in the midst of a Text Mode stream, which carries on
// [CTA-608-E 7.7, C.16]: the captions' decoder gets them, the text's does not.
TEST(EIA608ServiceDemuxTest, EraseCommandsReachTheCaptionsInTheMidstOfText) {
  EIA608ServiceDemux captions(EIA608Service::CC1);
  EIA608ServiceDemux text(EIA608Service::T1);
  const auto both = [&](uint8_t byte1, uint8_t byte2) {
    const bool to_captions = captions.accept(0, byte1, byte2);
    const bool to_text = text.accept(0, byte1, byte2);
    return std::make_pair(to_captions, to_text);
  };
  EXPECT_EQ(both(kMiscC1, kTR), std::make_pair(false, true));
  EXPECT_EQ(both('A', 'B'), std::make_pair(false, true));
  EXPECT_EQ(both(kMiscC1, kEDM), std::make_pair(true, false));
  EXPECT_EQ(both(kMiscC1, kENM), std::make_pair(true, false));
  EXPECT_EQ(text.last_service(), EIA608Service::CC1);
  EXPECT_EQ(both('C', 'D'), std::make_pair(false, true));
  EXPECT_EQ(text.last_service(), EIA608Service::T1);
}

TEST(EIA608ServiceDemuxTest, NullPaddingAndXdsBelongToNoService) {
  EIA608ServiceDemux demux(EIA608Service::CC1);

  EXPECT_FALSE(demux.accept(0, 0x00, 0x00));
  EXPECT_FALSE(demux.last_service().has_value());

  // XDS class byte on field 2.
  EXPECT_FALSE(demux.accept(1, 0x01, 0x02));
  EXPECT_FALSE(demux.last_service().has_value());
}

TEST(EIA608ServiceDemuxTest, TheTwoFieldsAreTrackedIndependently) {
  EIA608ServiceDemux demux(EIA608Service::CC1);

  // Field 2 selecting channel 2 must not move field 1's selection.
  EXPECT_TRUE(demux.accept(0, kMiscC1, kRCL));
  EXPECT_FALSE(demux.accept(1, kMiscC2, kRCL));
  EXPECT_TRUE(demux.accept(0, 'A', 'B'));
  EXPECT_EQ(demux.last_service(), EIA608Service::CC1);
}

TEST(EIA608ServiceDemuxTest, FieldTwoServicesAreRoutedToo) {
  EIA608ServiceDemux demux(EIA608Service::CC3);

  EXPECT_TRUE(demux.accept(1, kMiscC1, kRCL));
  EXPECT_TRUE(demux.accept(1, 'A', 'B'));
  // The same bytes on field 1 are CC1, not CC3.
  EXPECT_FALSE(demux.accept(0, kMiscC1, kRCL));
  EXPECT_FALSE(demux.accept(0, 'C', 'D'));
}

TEST(EIA608ServiceDemuxTest, DropsTheDuplicateCopyOfAControlPair) {
  // An encoder sends every control pair twice so that one lost to noise still
  // arrives; acted on twice, a carriage return scrolls two lines.
  const std::vector<Pair> stream = {
      {kMiscC1, kRCL}, {kMiscC1, kRCL}, {'A', 'B'},
      {kMiscC1, kEOC}, {kMiscC1, kEOC},
  };

  EIA608ServiceDemux demux(EIA608Service::CC1);
  int control_pairs = 0;
  for (const Pair& pair : stream) {
    if (demux.accept(0, pair.byte1, pair.byte2) && pair.byte1 < 0x20) {
      ++control_pairs;
    }
  }
  EXPECT_EQ(control_pairs, 2);
}

TEST(EIA608ServiceDemuxTest, ThreeIdenticalControlPairsAreTwoCommands) {
  // The rule is "ignore the second of two identical consecutive pairs", not
  // "ignore every repeat": a third copy is a fresh command.
  EIA608ServiceDemux demux(EIA608Service::CC1);
  EXPECT_TRUE(demux.accept(0, kMiscC1, kCR));
  EXPECT_FALSE(demux.accept(0, kMiscC1, kCR));
  EXPECT_TRUE(demux.accept(0, kMiscC1, kCR));
}

TEST(EIA608ServiceDemuxTest, ACharacterPairBreaksTheDuplicateRun) {
  EIA608ServiceDemux demux(EIA608Service::CC1);
  EXPECT_TRUE(demux.accept(0, kMiscC1, kCR));
  EXPECT_TRUE(demux.accept(0, 'A', 'B'));
  // Not adjacent to the first any more, so this is a second carriage return.
  EXPECT_TRUE(demux.accept(0, kMiscC1, kCR));
}

TEST(EIA608ServiceDemuxTest,
     AnotherServicesPairDoesNotBreakTheDuplicateRunOfThisOne) {
  // The duplicate follows immediately within its own service; in a
  // multiplexed stream the other services' pairs sit between the two copies,
  // which is why the suppression is applied after routing and not before.
  EIA608ServiceDemux demux(EIA608Service::CC1);
  EXPECT_TRUE(demux.accept(0, kMiscC1, kCR));    // the carriage return
  EXPECT_FALSE(demux.accept(0, kMiscC2, kRCL));  // CC2's business
  EXPECT_FALSE(demux.accept(0, 'X', 'Y'));       // CC2's characters
  EXPECT_FALSE(demux.accept(0, kMiscC1, kCR));   // ... its duplicate copy
  EXPECT_TRUE(demux.accept(0, kMiscC1, kCR));    // and a second one
}

TEST(EIA608ServiceDemuxTest, VerbatimModeKeepsTheDuplicates) {
  // The SCC writer records the stream as transmitted.
  EIA608ServiceDemux demux(EIA608Service::CC1,
                           /*suppress_repeated_controls=*/false);
  EXPECT_TRUE(demux.accept(0, kMiscC1, kCR));
  EXPECT_TRUE(demux.accept(0, kMiscC1, kCR));
}

TEST(EIA608ServiceDemuxTest, ResetForgetsTheSelection) {
  EIA608ServiceDemux demux(EIA608Service::CC2);
  EXPECT_TRUE(demux.accept(0, kMiscC2, kRCL));
  EXPECT_TRUE(demux.accept(0, 'A', 'B'));

  demux.reset();
  // Channel 1 is the state a stream starts in, so the same characters now
  // belong to CC1.
  EXPECT_FALSE(demux.accept(0, 'C', 'D'));
}

TEST(EIA608ServiceDemuxTest, ParityBitsAreIgnoredWhenRouting) {
  EIA608ServiceDemux demux(EIA608Service::CC2);
  // 0x1C with odd parity applied is 0x9C.
  EXPECT_TRUE(demux.accept(0, 0x9C, 0xA0));
}

// The demux exists to be put in front of the decoder; this is the pairing the
// sink, the video sink and the preview dialog all make.
TEST(EIA608ServiceDemuxTest, DecodingOneServiceOfAnInterleavedStream) {
  const std::vector<Pair> stream = {
      {kMiscC1, kRCL},  // CC1: pop-on
      {kMiscC1, 0x70},  // PAC: bottom row, column 0
      {'D', 'A'},       //
      {kMiscC2, kTR},   // T2 butts in
      {'X', 'X'},       // ... with its own text
      {kMiscC1, 0x00},  // channel 1 again (a PAC-shaped pair; not decoded)
      {'D', 'Y'},       // CC1 resumes
      {kMiscC1, kEOC},  // CC1: display it
      {kMiscC1, kEDM},  // CC1: and take it away again
  };

  orc::EIA608ServiceDemux demux(EIA608Service::CC1);
  orc::EIA608Decoder decoder;
  double time = 0.0;
  for (const Pair& pair : stream) {
    time += 1.0 / 29.97;
    if (demux.accept(0, pair.byte1, pair.byte2)) {
      decoder.process_bytes(time, pair.byte1, pair.byte2);
    }
  }

  const auto cues = decoder.finalize(time + 1.0);
  ASSERT_EQ(cues.size(), 1u);
  // "XX" belonged to the text service and must not be in the caption.
  EXPECT_EQ(cues[0].text, "DADY");
}

}  // namespace orc_unit_test
