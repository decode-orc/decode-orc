/*
 * File:        efm_confidence_stack_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for sync-anchored confidence stacking of EFM t-values
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/stacker/efm_confidence_stack.h"

#include <gtest/gtest.h>
#include <orc/stage/video_frame_representation.h>

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

namespace orc_unit_test {
namespace {

// IEC 60908 Section 20.2: 588 channel bits per frame, opening with T11+T11.
constexpr int kChannelFrameBits = 588;
constexpr int kSyncBits = 22;
constexpr int kBodyBits = kChannelFrameBits - kSyncBits;

// A legal channel-frame body of the requested bit length. T4 runs throughout
// with a longer final run to make up the remainder, so no two adjacent runs
// are ever T11 and no accidental frame sync appears inside the body.
std::vector<int> make_body(int bits = kBodyBits) {
  std::vector<int> runs;
  int remaining = bits;
  while (remaining > 11) {
    runs.push_back(4);
    remaining -= 4;
  }
  runs.push_back(remaining);
  return runs;
}

// Pack a channel frame: the T11+T11 sync followed by |body|, every run
// carrying |doubt|.
std::vector<uint8_t> pack_frame(const std::vector<int>& body,
                                uint8_t doubt = 0) {
  EXPECT_EQ(std::accumulate(body.begin(), body.end(), 0), kBodyBits);
  std::vector<uint8_t> packed;
  packed.push_back(orc::efm_pack(11, doubt));
  packed.push_back(orc::efm_pack(11, doubt));
  for (int run : body) {
    packed.push_back(orc::efm_pack(static_cast<uint8_t>(run), doubt));
  }
  return packed;
}

std::vector<uint8_t> pack_stream(const std::vector<int>& body, int frames,
                                 uint8_t doubt = 0) {
  std::vector<uint8_t> packed;
  for (int i = 0; i < frames; ++i) {
    const std::vector<uint8_t> frame = pack_frame(body, doubt);
    packed.insert(packed.end(), frame.begin(), frame.end());
  }
  return packed;
}

// Only the channel frames bounded by a sync at both ends are combined, so a
// frame under test needs a plain frame after it to close the slot.
std::vector<uint8_t> pack_frame_then_plain(const std::vector<int>& body,
                                           uint8_t doubt = 0) {
  std::vector<uint8_t> packed = pack_frame(body, doubt);
  const std::vector<uint8_t> plain = pack_frame(make_body(), doubt);
  packed.insert(packed.end(), plain.begin(), plain.end());
  return packed;
}

// A legal body distinct from every other variant, so that pairing frames from
// two sources incorrectly shows up as disagreement rather than passing
// unnoticed.
std::vector<int> body_variant(int index) {
  std::vector<int> runs = make_body(kBodyBits - 12);
  const int split = 3 + (index % 7);
  runs.insert(runs.begin(), {split, 12 - split});
  return runs;
}

// Number of t-values in one packed channel frame built from |body|.
size_t frame_length(const std::vector<int>& body) { return body.size() + 2; }

// Channel-frame bodies with distinct opening runs, so that pairing frames from
// two captures incorrectly cannot pass unnoticed. The three leading runs sum
// to 24 and each stays within T3-T11, which gives 52 distinct openings.
std::vector<std::vector<int>> distinct_openings() {
  std::vector<std::vector<int>> openings;
  for (int a = 3; a <= 11; ++a) {
    for (int b = 3; b <= 11; ++b) {
      const int c = 24 - a - b;
      if (c >= 3 && c <= 11) {
        openings.push_back({a, b, c});
      }
    }
  }
  return openings;
}

std::vector<int> body_unique(size_t index) {
  const std::vector<std::vector<int>> openings = distinct_openings();
  std::vector<int> runs = make_body(kBodyBits - 24);
  const std::vector<int>& opening = openings[index % openings.size()];
  runs.insert(runs.begin(), opening.begin(), opening.end());
  return runs;
}

// A run of channel frames carrying disc content |first| onwards.
std::vector<uint8_t> pack_run(size_t first, size_t count, uint8_t doubt = 0) {
  std::vector<uint8_t> packed;
  for (size_t i = 0; i < count; ++i) {
    const std::vector<uint8_t> frame =
        pack_frame(body_unique(first + i), doubt);
    packed.insert(packed.end(), frame.begin(), frame.end());
  }
  return packed;
}

std::vector<uint8_t> tvalues_of(const std::vector<uint8_t>& packed) {
  std::vector<uint8_t> values;
  values.reserve(packed.size());
  for (uint8_t byte : packed) {
    values.push_back(orc::efm_tvalue(byte));
  }
  return values;
}

// Every T11+T11 sync in |packed| must sit exactly 588 bits after the last.
void expect_intact_sync_grid(const std::vector<uint8_t>& packed) {
  int bit = 0;
  int previous_sync = -1;
  for (size_t i = 0; i + 1 < packed.size(); ++i) {
    if (orc::efm_tvalue(packed[i]) == 11 &&
        orc::efm_tvalue(packed[i + 1]) == 11) {
      if (previous_sync >= 0) {
        EXPECT_EQ(bit - previous_sync, kChannelFrameBits)
            << "sync grid broken at t-value " << i;
      }
      previous_sync = bit;
      bit += 22;
      ++i;
      continue;
    }
    bit += orc::efm_tvalue(packed[i]);
  }
}

void expect_legal_run_lengths(const std::vector<uint8_t>& packed) {
  for (uint8_t byte : packed) {
    const uint8_t value = orc::efm_tvalue(byte);
    // IEC 60908 Section 20.1: the (2,10) run-length limit bounds runs to
    // T3-T11.
    EXPECT_GE(value, 3);
    EXPECT_LE(value, 11);
  }
}

}  // namespace

// A lone source was not stacked with anything, so both its t-values and the
// doubt its producer attached to them stand unaltered.
TEST(EfmConfidenceStackTest, SingleSourceIsPassedThroughUnchanged) {
  const std::vector<uint8_t> source = pack_stream(make_body(), 3, 7);
  EXPECT_EQ(orc::stack_efm_confidence({source}, 0), source);
}

TEST(EfmConfidenceStackTest, NoSourcesYieldsNothing) {
  EXPECT_TRUE(orc::stack_efm_confidence({}, 0).empty());
}

// Without a frame sync there is no axis to combine on, so the reference is
// returned rather than a blend made on an assumption that does not hold.
TEST(EfmConfidenceStackTest, StreamWithoutASyncFallsBackToTheReference) {
  const std::vector<uint8_t> a = {orc::efm_pack(3, 0), orc::efm_pack(4, 0)};
  const std::vector<uint8_t> b = {orc::efm_pack(5, 0), orc::efm_pack(6, 0)};
  EXPECT_EQ(orc::stack_efm_confidence({a, b}, 1), b);
}

TEST(EfmConfidenceStackTest, AgreeingSourcesReproduceTheStreamWithNoDoubt) {
  const std::vector<uint8_t> source = pack_stream(make_body(), 3);
  const auto stacked = orc::stack_efm_confidence({source, source}, 0);

  EXPECT_EQ(tvalues_of(stacked), tvalues_of(source));
  for (uint8_t byte : stacked) {
    EXPECT_EQ(orc::efm_doubt(byte), 0);
  }
}

// Unanimity among sources that all distrusted what they read is not a reason
// to trust it: the output keeps the doubt its vouchers had.
TEST(EfmConfidenceStackTest, AgreeingSourcesKeepTheDoubtTheyVouchedWith) {
  const std::vector<uint8_t> source = pack_stream(make_body(), 2, 6);
  const auto stacked = orc::stack_efm_confidence({source, source}, 0);

  ASSERT_FALSE(stacked.empty());
  for (uint8_t byte : stacked) {
    EXPECT_EQ(orc::efm_doubt(byte), 6);
  }
}

// The lowest doubt among the sources that reported a run is the one that
// carries: one capture being sure of a reading the others agree with is the
// evidence that matters.
TEST(EfmConfidenceStackTest, TakesTheLowestDoubtOfTheSourcesThatAgree) {
  const std::vector<uint8_t> confident = pack_stream(make_body(), 2, 0);
  const std::vector<uint8_t> unsure = pack_stream(make_body(), 2, 11);
  const auto stacked = orc::stack_efm_confidence({unsure, confident}, 0);

  const size_t combined = frame_length(make_body());
  ASSERT_GE(stacked.size(), combined);
  for (size_t i = 0; i < combined; ++i) {
    EXPECT_EQ(orc::efm_doubt(stacked[i]), 0) << "at t-value " << i;
  }
}

namespace {

// Two sources that split the same 12 bits differently: one reads T3 then T9,
// the other T9 then T3. Their arithmetic mean is T6+T6, a reading neither of
// them made.
struct DisagreeingPair {
  std::vector<uint8_t> low_then_high;
  std::vector<uint8_t> high_then_low;
  size_t contested_index = 0;
};

DisagreeingPair make_disagreeing_pair(uint8_t low_doubt, uint8_t high_doubt) {
  std::vector<int> body = make_body(kBodyBits - 12);
  std::vector<int> first = body;
  std::vector<int> second = body;
  first.insert(first.begin(), {3, 9});
  second.insert(second.begin(), {9, 3});

  DisagreeingPair pair;
  pair.low_then_high = pack_frame_then_plain(first, low_doubt);
  pair.high_then_low = pack_frame_then_plain(second, high_doubt);
  pair.contested_index = 2;  // straight after the two sync runs
  return pair;
}

}  // namespace

// The headline property: a stacked t-value is always one a source actually
// reported. An average would put a T6 here that neither capture saw and that
// the EFM demodulator would then decode with full confidence.
TEST(EfmConfidenceStackTest, NeverEmitsAValueNoSourceReported) {
  const DisagreeingPair pair = make_disagreeing_pair(0, 0);
  const auto stacked =
      orc::stack_efm_confidence({pair.low_then_high, pair.high_then_low}, 0);

  ASSERT_GT(stacked.size(), pair.contested_index + 1);
  const uint8_t first = orc::efm_tvalue(stacked[pair.contested_index]);
  const uint8_t second = orc::efm_tvalue(stacked[pair.contested_index + 1]);
  EXPECT_EQ(first + second, 12);
  EXPECT_TRUE((first == 3 && second == 9) || (first == 9 && second == 3))
      << "got T" << static_cast<int>(first) << " then T"
      << static_cast<int>(second);
}

TEST(EfmConfidenceStackTest, TheMoreConfidentSourceWinsAContestedTransition) {
  const DisagreeingPair trusted_low = make_disagreeing_pair(0, 12);
  auto stacked = orc::stack_efm_confidence(
      {trusted_low.low_then_high, trusted_low.high_then_low}, 0);
  EXPECT_EQ(orc::efm_tvalue(stacked[trusted_low.contested_index]), 3);
  EXPECT_EQ(orc::efm_tvalue(stacked[trusted_low.contested_index + 1]), 9);

  const DisagreeingPair trusted_high = make_disagreeing_pair(12, 0);
  stacked = orc::stack_efm_confidence(
      {trusted_high.low_then_high, trusted_high.high_then_low}, 0);
  EXPECT_EQ(orc::efm_tvalue(stacked[trusted_high.contested_index]), 9);
  EXPECT_EQ(orc::efm_tvalue(stacked[trusted_high.contested_index + 1]), 3);
}

// Disagreement is information, and it is passed on: the runs the sources
// argued over come out doubted even though both producers were sure, while
// the runs they agreed on stay trusted.
TEST(EfmConfidenceStackTest, ContestedRunsCarryDoubtAndAgreedRunsDoNot) {
  const DisagreeingPair pair = make_disagreeing_pair(0, 12);
  const auto stacked =
      orc::stack_efm_confidence({pair.low_then_high, pair.high_then_low}, 0);

  ASSERT_GT(stacked.size(), pair.contested_index + 2);
  EXPECT_GT(orc::efm_doubt(stacked[pair.contested_index]), 0);
  EXPECT_GT(orc::efm_doubt(stacked[pair.contested_index + 1]), 0);
  EXPECT_EQ(orc::efm_doubt(stacked[0]), 0);
  EXPECT_EQ(orc::efm_doubt(stacked.back()), 0);
}

// The reason for anchoring on the sync grid at all. One capture reads a single
// T8 as T4+T4, which leaves it with one more t-value than the others for the
// rest of the frame. Combining by sample index would pair every later run with
// its neighbour's; on the bit axis the extra transition is simply outvoted and
// nothing downstream of it moves.
TEST(EfmConfidenceStackTest, AnInsertionInOneSourceDoesNotDisturbTheRest) {
  std::vector<int> body = make_body(kBodyBits - 8);
  std::vector<int> clean = body;
  clean.insert(clean.begin(), 8);
  std::vector<int> split = body;
  split.insert(split.begin(), {4, 4});

  const std::vector<uint8_t> a = pack_frame_then_plain(clean);
  const std::vector<uint8_t> b = pack_frame_then_plain(split);

  const auto stacked = orc::stack_efm_confidence({a, a, b}, 0);
  EXPECT_EQ(tvalues_of(stacked), tvalues_of(a));
  expect_intact_sync_grid(stacked);
}

// The mirror case: one capture misses a transition and merges two runs.
TEST(EfmConfidenceStackTest, ADeletionInOneSourceDoesNotDisturbTheRest) {
  std::vector<int> body = make_body(kBodyBits - 8);
  std::vector<int> clean = body;
  clean.insert(clean.begin(), {4, 4});
  std::vector<int> merged = body;
  merged.insert(merged.begin(), 8);

  const std::vector<uint8_t> a = pack_frame_then_plain(clean);
  const std::vector<uint8_t> b = pack_frame_then_plain(merged);

  const auto stacked = orc::stack_efm_confidence({a, a, b}, 0);
  EXPECT_EQ(tvalues_of(stacked), tvalues_of(a));
}

// A capture that missed its own first sync starts a channel frame later than
// the others. Every frame here carries distinct content, so pairing a source's
// frames with the neighbours they happen to be numbered alongside would show
// up as disagreement; the output staying unanimous is the evidence that the
// realignment found the right frames.
TEST(EfmConfidenceStackTest, RealignsASourceThatMissedItsFirstSync) {
  std::vector<uint8_t> reference;
  for (int frame = 0; frame < 5; ++frame) {
    const std::vector<uint8_t> packed = pack_frame(body_variant(frame));
    reference.insert(reference.end(), packed.begin(), packed.end());
  }

  // Same disc content, but the leading sync reads as two T10s and a T2, so the
  // source's own first detected sync is one channel frame in.
  std::vector<uint8_t> shifted = reference;
  shifted[0] = orc::efm_pack(10, 15);
  shifted[1] = orc::efm_pack(10, 15);
  shifted.insert(shifted.begin() + 2, orc::efm_pack(2, 15));

  const auto stacked = orc::stack_efm_confidence({reference, shifted}, 0);
  EXPECT_EQ(tvalues_of(stacked), tvalues_of(reference));
  expect_intact_sync_grid(stacked);

  // Frames one to three are the slots both sources hold; they agreed on every
  // run, so nothing in them is doubted.
  const size_t first = frame_length(body_variant(0));
  const size_t last = first + frame_length(body_variant(1)) +
                      frame_length(body_variant(2)) +
                      frame_length(body_variant(3));
  ASSERT_GE(stacked.size(), last);
  for (size_t i = first; i < last; ++i) {
    EXPECT_EQ(orc::efm_doubt(stacked[i]), 0) << "at t-value " << i;
  }
}

// Captures of the same disc do not begin at the same place on it. Aligning
// them by video frame leaves a steady fraction of a frame - the Domesday
// National A sides sit 1.123 frames apart, tens of channel frames even once
// the whole-frame part is taken out - so the offset between two sources has to
// be found rather than assumed. Here two captures start eighteen channel
// frames further along the disc than the reference does, and still correct a
// misreading in it.
TEST(EfmConfidenceStackTest, FindsALargeOffsetBetweenCapturesOfTheSameDisc) {
  const size_t kShift = 18;
  const size_t kFrames = 30;
  const size_t kDamaged = 20;  // a frame both later captures also cover

  const std::vector<uint8_t> intact = pack_run(0, kFrames);
  std::vector<uint8_t> reference = intact;

  // Misread one run of frame |kDamaged| as two, the way a spurious transition
  // would: from here on the reference holds one more t-value than the others.
  size_t at = 0;
  for (size_t f = 0; f < kDamaged; ++f) {
    at += frame_length(body_unique(f));
  }
  at += 2 + 3;  // past the sync and the three distinct opening runs
  ASSERT_EQ(orc::efm_tvalue(reference[at]), 4);
  reference[at] = orc::efm_pack(1, 0);
  reference.insert(reference.begin() + static_cast<std::ptrdiff_t>(at) + 1,
                   orc::efm_pack(3, 0));

  const std::vector<uint8_t> later = pack_run(kShift, kFrames);
  const auto stacked = orc::stack_efm_confidence({reference, later, later}, 0);

  EXPECT_EQ(tvalues_of(stacked), tvalues_of(intact));
  expect_intact_sync_grid(stacked);
  expect_legal_run_lengths(stacked);
}

// t-values outside any complete channel frame - the partial frames at the head
// and tail of a video frame's chunk - are the reference's, untouched.
TEST(EfmConfidenceStackTest, MaterialOutsideTheSyncGridIsCarriedOverVerbatim) {
  const std::vector<uint8_t> lead = {orc::efm_pack(5, 3), orc::efm_pack(7, 9),
                                     orc::efm_pack(4, 0)};
  std::vector<uint8_t> a = lead;
  const std::vector<uint8_t> frames = pack_stream(make_body(), 3);
  a.insert(a.end(), frames.begin(), frames.end());
  a.push_back(orc::efm_pack(6, 2));

  const auto stacked = orc::stack_efm_confidence({a, a}, 0);
  ASSERT_GE(stacked.size(), lead.size());
  EXPECT_TRUE(std::equal(lead.begin(), lead.end(), stacked.begin()));
  EXPECT_EQ(stacked.back(), orc::efm_pack(6, 2));
}

// However badly the sources disagree, what comes out is a legal EFM stream:
// runs within T3-T11 and an intact 588-bit sync grid. A combiner that can emit
// an illegal channel frame is worse than useless - the demodulator would lose
// frames the individual captures decoded perfectly well.
TEST(EfmConfidenceStackTest, OutputIsAlwaysALegalStreamHoweverSourcesDisagree) {
  std::vector<std::vector<uint8_t>> sources;
  for (int variant = 0; variant < 4; ++variant) {
    std::vector<int> runs = make_body(kBodyBits - 12);
    // Each source splits the same twelve bits its own way, and doubts it.
    runs.insert(runs.begin(), {3 + variant, 9 - variant});
    sources.push_back(
        pack_frame_then_plain(runs, static_cast<uint8_t>(variant * 4)));
  }

  const auto stacked = orc::stack_efm_confidence(sources, 0);
  expect_legal_run_lengths(stacked);
  expect_intact_sync_grid(stacked);
  EXPECT_EQ(std::accumulate(stacked.begin(), stacked.end(), 0,
                            [](int total, uint8_t byte) {
                              return total + orc::efm_tvalue(byte);
                            }),
            std::accumulate(sources[0].begin(), sources[0].end(), 0,
                            [](int total, uint8_t byte) {
                              return total + orc::efm_tvalue(byte);
                            }));
}

}  // namespace orc_unit_test
