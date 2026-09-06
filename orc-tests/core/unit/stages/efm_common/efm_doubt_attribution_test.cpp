/*
 * File:        efm_doubt_attribution_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for issue #307 - carrying the producer's per-t-value
 *              doubt through EFM framing and attributing it to F3 symbols
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/video_frame_representation.h>

#include <cstdint>
#include <vector>

#include "dec_channeltof3frame.h"
#include "dec_tvaluestochannel.h"
#include "efm_constants.h"

namespace {

// A whole EFM channel frame's worth of T3 pulses: 588 / 3 = 196 exactly
// (IEC 60908 §18 - a channel frame is 588 channel bits). Using a single
// T-value throughout makes the bit position of every T-value trivially
// predictable: T-value k occupies channel bits [3k, 3k+2].
constexpr int kTValue = 3;
constexpr int kTValuesPerFrame = efm::kEfmFrameChannelBits / kTValue;  // 196

std::vector<uint8_t> uniformFrame(uint8_t doubt) {
  return std::vector<uint8_t>(kTValuesPerFrame, orc::efm_pack(kTValue, doubt));
}

// The 32 data symbols start at channel bit 44 and repeat every 17 bits
// (14 symbol bits + 3 merging bits).
constexpr int kFirstDataBit = 44;
constexpr int kBitsPerSymbol = 17;

F3Frame decodeOneFrame(const std::vector<uint8_t>& tValues,
                       bool collectDoubt = true) {
  ChannelToF3Frame decoder;
  decoder.setCollectDoubt(collectDoubt);
  decoder.pushFrame(tValues);
  EXPECT_TRUE(decoder.isReady());
  return decoder.popFrame();
}

// ---------------------------------------------------------------------------
// T-value framing
// ---------------------------------------------------------------------------

// The framing state machine reads T-values out of packed bytes, so it must
// match a sync pair on the T-value alone - a T11 the producer distrusts is
// still a T11. If it compared whole bytes, a doubted sync would be invisible
// and the frame would be lost.
TEST(EfmDoubtFraming, FindsASyncHeaderCarryingANonZeroDoubtNibble) {
  TvaluesToChannel framer;

  // Two frames of T3 bracketed by doubted T11+T11 sync pairs, plus enough
  // trailing input to push the state machine past its buffer watermark.
  std::vector<uint8_t> stream;
  const uint8_t doubtedSync = orc::efm_pack(efm::kSyncSymbolT11, 15);
  for (int frame = 0; frame < 4; ++frame) {
    stream.push_back(doubtedSync);
    stream.push_back(doubtedSync);
    // 588 - 22 sync bits = 566 bits; 188 T3s plus a T2 pad is close enough
    // for the framer's 550-600 bit acceptance band.
    for (int i = 0; i < 188; ++i) {
      stream.push_back(orc::efm_pack(kTValue, 0));
    }
  }
  framer.pushFrame(stream);

  ASSERT_TRUE(framer.isReady());
  const std::vector<uint8_t> frame = framer.popFrame();
  ASSERT_GE(frame.size(), 2u);
  // The frame starts at the sync pair, doubt nibble intact.
  EXPECT_EQ(frame[0], doubtedSync);
  EXPECT_EQ(frame[1], doubtedSync);
}

// ---------------------------------------------------------------------------
// Symbol attribution
// ---------------------------------------------------------------------------

// An .efm from a producer with no confidence information is all-zero doubt.
// Such a frame must leave the doubt vector unset, so nothing downstream pays
// for information that does not exist.
TEST(EfmDoubtAttribution, LeavesDoubtUnsetWhenEveryTValueIsTrusted) {
  const F3Frame frame = decodeOneFrame(uniformFrame(0));
  EXPECT_TRUE(frame.doubtData().empty());
}

// Attributing the doubt is per-T-value work in the hottest loop of the decode,
// so it must stay off until something downstream asks for it. A decoder that
// was never told to collect it must produce the same frames as before the
// doubt existed.
TEST(EfmDoubtAttribution, CollectsNoDoubtUnlessAskedTo) {
  const F3Frame frame = decodeOneFrame(uniformFrame(15), false);
  EXPECT_TRUE(frame.doubtData().empty());

  const F3Frame asked = decodeOneFrame(uniformFrame(15), true);
  EXPECT_EQ(asked.doubtData().size(), 32u);
  EXPECT_EQ(asked.data(), frame.data());
  EXPECT_EQ(asked.errorData(), frame.errorData());
}

TEST(EfmDoubtAttribution, AttributesUniformDoubtToEverySymbol) {
  const F3Frame frame = decodeOneFrame(uniformFrame(15));

  const std::vector<uint8_t>& doubt = frame.doubtData();
  ASSERT_EQ(doubt.size(), 32u);
  for (size_t symbol = 0; symbol < doubt.size(); ++symbol) {
    EXPECT_EQ(doubt[symbol], 15) << "symbol " << symbol;
  }
}

// A single doubted T-value must reach only the symbols whose channel bits it
// actually contributed to. T-value 20 spans bits 60-62, which lie inside
// symbol 1 (bits 61-74) and no other.
TEST(EfmDoubtAttribution, ConfinesDoubtToTheSymbolsTheTValueOverlaps) {
  std::vector<uint8_t> tValues = uniformFrame(0);
  constexpr int kDoubtedTValue = 20;
  tValues[kDoubtedTValue] = orc::efm_pack(kTValue, 11);

  // The bits this T-value produced, and the symbol range they fall in.
  const int firstBit = kDoubtedTValue * kTValue;
  const int lastBit = firstBit + kTValue - 1;
  ASSERT_EQ(firstBit, 60);
  ASSERT_EQ(lastBit, 62);

  const F3Frame frame = decodeOneFrame(tValues);
  const std::vector<uint8_t>& doubt = frame.doubtData();
  ASSERT_EQ(doubt.size(), 32u);

  for (int symbol = 0; symbol < 32; ++symbol) {
    const int symbolFirst = kFirstDataBit + symbol * kBitsPerSymbol;
    const int symbolLast = symbolFirst + 13;
    const bool overlaps = symbolFirst <= lastBit && symbolLast >= firstBit;
    EXPECT_EQ(doubt[symbol], overlaps ? 11 : 0) << "symbol " << symbol;
  }
  // Guard the fixture itself: exactly one symbol should be affected.
  EXPECT_EQ(doubt[1], 11);
  EXPECT_EQ(doubt[0], 0);
  EXPECT_EQ(doubt[2], 0);
}

// A 14-bit symbol is wrong if any one of its bits is wrong, so a symbol built
// from a trusted and a distrusted T-value takes the larger doubt, not a mean.
TEST(EfmDoubtAttribution, TakesTheGreatestDoubtOfTheContributingTValues) {
  std::vector<uint8_t> tValues = uniformFrame(0);
  // Symbol 1 spans bits 61-74, i.e. T-values 20 through 24.
  tValues[21] = orc::efm_pack(kTValue, 4);
  tValues[22] = orc::efm_pack(kTValue, 12);
  tValues[23] = orc::efm_pack(kTValue, 7);

  const F3Frame frame = decodeOneFrame(tValues);
  ASSERT_EQ(frame.doubtData().size(), 32u);
  EXPECT_EQ(frame.doubtData()[1], 12);
}

// The bit count that decides whether a frame is well-formed must read the
// T-value only; counting the packed byte would multiply every doubted pulse
// and make clean frames look like gross overshoots.
TEST(EfmDoubtAttribution, DoesNotCountTheDoubtNibbleAsChannelBits) {
  // Same frame twice, once trusted and once wholly distrusted: the decoded
  // symbols must be identical.
  const F3Frame trusted = decodeOneFrame(uniformFrame(0));
  const F3Frame distrusted = decodeOneFrame(uniformFrame(15));

  EXPECT_EQ(trusted.data(), distrusted.data());
  EXPECT_EQ(trusted.errorData(), distrusted.errorData());
  EXPECT_EQ(trusted.f3FrameType(), distrusted.f3FrameType());
  EXPECT_EQ(trusted.subcodeByte(), distrusted.subcodeByte());
}

}  // namespace
