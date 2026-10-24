/*
 * File:        nabts_record_library_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NABTS record library: filling a lost stretch
 *              of a record from another record whose bytes around it agree
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_record_library.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace orc {
namespace {

/// |count| bytes of varied numeric data (4/0 to 7/15), different for each
/// |seed| — the kind of operand stream a template page is made of.
std::vector<uint8_t> varied(size_t count, uint32_t seed) {
  std::vector<uint8_t> out(count);
  uint32_t state = seed * 2654435761u + 1;
  for (auto& byte : out) {
    state = state * 1664525u + 1013904223u;
    byte = static_cast<uint8_t>(0x40 + ((state >> 24) & 0x3F));
  }
  return out;
}

/// |data| with [start, end) lost: zeroed, and marked absent.
struct Holed {
  std::vector<uint8_t> data;
  std::vector<uint8_t> present;
};
Holed with_hole(std::vector<uint8_t> data, size_t start, size_t end) {
  std::vector<uint8_t> present(data.size(), 1);
  for (size_t i = start; i < end; ++i) {
    data[i] = 0;
    present[i] = 0;
  }
  return {std::move(data), std::move(present)};
}

std::vector<uint8_t> slice(const std::vector<uint8_t>& data, size_t start,
                           size_t end) {
  return {data.begin() + static_cast<std::ptrdiff_t>(start),
          data.begin() + static_cast<std::ptrdiff_t>(end)};
}

// The ordinary case: another recording sent the same template whole.
TEST(NabtsRecordLibrary, FillsALostStretchFromARecordThatAgreesAroundIt) {
  const std::vector<uint8_t> page = varied(300, 1);
  NabtsRecordLibrary library;
  library.add({"reference", "000-051-v0", page, {}});
  library.build();

  const Holed lost = with_hole(page, 120, 147);
  const NabtsLibraryAnswer answer =
      library.fill(lost.data, lost.present, 120, 147, SIZE_MAX);

  ASSERT_TRUE(answer.fill.has_value());
  EXPECT_EQ(answer.fill->bytes, slice(page, 120, 147));
  EXPECT_EQ(answer.fill->record, 0u);
  EXPECT_EQ(answer.fill->context_left, kNabtsLibraryMaxContextBytes);
  EXPECT_EQ(answer.fill->context_right, kNabtsLibraryMaxContextBytes);
  EXPECT_EQ(answer.fill->agreeing_records, 1u);
}

// The same template at a different offset in the other record: pages share a
// logo or a map, not their whole layout.
TEST(NabtsRecordLibrary, FindsTheStretchWhereverItLiesInTheOtherRecord) {
  const std::vector<uint8_t> shared = varied(200, 2);
  std::vector<uint8_t> reference = varied(77, 3);
  reference.insert(reference.end(), shared.begin(), shared.end());
  NabtsRecordLibrary library;
  library.add({"other", "000-010-v2", reference, {}});
  library.build();

  std::vector<uint8_t> page = varied(40, 4);
  page.insert(page.end(), shared.begin(), shared.end());
  const Holed lost = with_hole(page, 120, 147);
  const NabtsLibraryAnswer answer =
      library.fill(lost.data, lost.present, 120, 147, SIZE_MAX);

  ASSERT_TRUE(answer.fill.has_value());
  EXPECT_EQ(answer.fill->bytes, slice(page, 120, 147));
}

TEST(NabtsRecordLibrary, RefusesAFillTheContextCannotVouchFor) {
  // Agreement a little short of what a fill needs, split across both sides:
  // enough to anchor, too little to trust.
  const size_t side = kNabtsLibraryTrustedContextBytes / 2 - 2;
  ASSERT_GE(side, kNabtsLibraryAnchorBytes + 6);
  const size_t start = 100;
  const size_t end = 127;
  const std::vector<uint8_t> page = varied(260, 5);
  std::vector<uint8_t> reference = page;
  for (size_t i = 0; i < start - side; ++i) {
    reference[i] ^= 0x01;
  }
  for (size_t i = end + side; i < reference.size(); ++i) {
    reference[i] ^= 0x01;
  }
  NabtsRecordLibrary library;
  library.add({"other", "k", reference, {}});
  library.build();

  const Holed lost = with_hole(page, start, end);
  const NabtsLibraryAnswer answer =
      library.fill(lost.data, lost.present, start, end, SIZE_MAX);

  EXPECT_FALSE(answer.fill.has_value());
  EXPECT_EQ(answer.refusal, NabtsLibraryRefusal::kTooLittleContext);
}

// A run of one repeated operand agrees with every other such run, so it
// vouches for nothing about what lay between.
TEST(NabtsRecordLibrary, RefusesContextThatIsOneByteRepeated) {
  std::vector<uint8_t> page(64, 0x40);
  const std::vector<uint8_t> middle = varied(27, 6);
  page.insert(page.end(), middle.begin(), middle.end());
  page.insert(page.end(), 64, 0x40);
  NabtsRecordLibrary library;
  library.add({"other", "k", page, {}});
  library.build();

  const Holed lost = with_hole(page, 64, 91);
  EXPECT_FALSE(
      library.fill(lost.data, lost.present, 64, 91, SIZE_MAX).fill.has_value());
}

TEST(NabtsRecordLibrary, RefusesWhenRecordingsDisagreeAboutTheLostBytes) {
  const std::vector<uint8_t> page = varied(300, 7);
  std::vector<uint8_t> other = page;
  other[130] ^= 0x01;  // Same surroundings, one byte different inside.
  NabtsRecordLibrary library;
  library.add({"a", "k", page, {}});
  library.add({"b", "k", other, {}});
  library.build();

  const Holed lost = with_hole(page, 120, 147);
  const NabtsLibraryAnswer answer =
      library.fill(lost.data, lost.present, 120, 147, SIZE_MAX);

  EXPECT_FALSE(answer.fill.has_value());
  EXPECT_EQ(answer.refusal, NabtsLibraryRefusal::kAmbiguous);
}

// A stretch several pages share turns up in a dozen records and a stray
// reading of it in one; the fill kNabtsLibraryDominantRatio times as many
// records offer is the one sent.
TEST(NabtsRecordLibrary, TakesTheFillFarMoreRecordsOffer) {
  const std::vector<uint8_t> page = varied(300, 11);
  std::vector<uint8_t> stray = page;
  stray[130] ^= 0x01;  // Same surroundings, one byte different inside.
  NabtsRecordLibrary library;
  for (size_t i = 0; i < kNabtsLibraryDominantRatio; ++i) {
    library.add({"template", "k" + std::to_string(i), page, {}});
  }
  library.add({"stray", "s", stray, {}});
  library.build();

  const Holed lost = with_hole(page, 120, 147);
  const NabtsLibraryAnswer answer =
      library.fill(lost.data, lost.present, 120, 147, SIZE_MAX);

  ASSERT_TRUE(answer.fill.has_value());
  EXPECT_EQ(answer.fill->bytes, slice(page, 120, 147));
  EXPECT_EQ(answer.fill->agreeing_records, kNabtsLibraryDominantRatio);
}

// A lead short of that settles nothing, and a policy of zero turns the rule
// off.
TEST(NabtsRecordLibrary, StillRefusesWhenNoFillDominates) {
  const std::vector<uint8_t> page = varied(300, 12);
  std::vector<uint8_t> stray = page;
  stray[130] ^= 0x01;
  NabtsRecordLibrary short_lead;
  for (size_t i = 0; i + 1 < kNabtsLibraryDominantRatio; ++i) {
    short_lead.add({"template", "k" + std::to_string(i), page, {}});
  }
  short_lead.add({"stray", "s", stray, {}});
  short_lead.build();
  const Holed lost = with_hole(page, 120, 147);
  const NabtsLibraryAnswer answer =
      short_lead.fill(lost.data, lost.present, 120, 147, SIZE_MAX);
  EXPECT_FALSE(answer.fill.has_value());
  EXPECT_EQ(answer.refusal, NabtsLibraryRefusal::kAmbiguous);

  NabtsRecordLibrary long_lead;
  for (size_t i = 0; i < 2 * kNabtsLibraryDominantRatio; ++i) {
    long_lead.add({"template", "k" + std::to_string(i), page, {}});
  }
  long_lead.add({"stray", "s", stray, {}});
  long_lead.build();
  NabtsLibraryPolicy off;
  off.dominant_ratio = 0;
  EXPECT_FALSE(long_lead.fill(lost.data, lost.present, 120, 147, SIZE_MAX, off)
                   .fill.has_value());
}

TEST(NabtsRecordLibrary, PrefersTheRecordThatAgreesFarFurther) {
  const std::vector<uint8_t> page = varied(300, 8);
  // Agrees for 24 bytes each side, and offers different bytes inside.
  std::vector<uint8_t> near = page;
  near[130] ^= 0x01;
  for (size_t i = 0; i < 96; ++i) {
    near[i] ^= 0x02;
  }
  for (size_t i = 171; i < near.size(); ++i) {
    near[i] ^= 0x02;
  }
  NabtsRecordLibrary library;
  library.add({"near", "k", near, {}});
  library.add({"far", "k", page, {}});
  library.build();

  const Holed lost = with_hole(page, 120, 147);
  const NabtsLibraryAnswer answer =
      library.fill(lost.data, lost.present, 120, 147, SIZE_MAX);

  ASSERT_TRUE(answer.fill.has_value());
  EXPECT_EQ(answer.fill->bytes, slice(page, 120, 147));
  EXPECT_EQ(answer.fill->record, 1u);
}

TEST(NabtsRecordLibrary, NeverOffersARecordItsOwnBytes) {
  const std::vector<uint8_t> page = varied(300, 9);
  const Holed lost = with_hole(page, 120, 147);
  NabtsRecordLibrary library;
  // The record itself, as the recording holds it — the case of a library built
  // from the recording's own records.
  library.add({"this recording", "k", lost.data, lost.present});
  library.build();

  EXPECT_FALSE(
      library.fill(lost.data, lost.present, 120, 147, 0).fill.has_value());
}

// A reference that lost the same stretch has nothing to offer for it, however
// well it agrees around it.
TEST(NabtsRecordLibrary, NeverFillsFromBytesTheReferenceLostItself) {
  const std::vector<uint8_t> page = varied(300, 10);
  const Holed reference = with_hole(page, 130, 140);
  NabtsRecordLibrary library;
  library.add({"other", "k", reference.data, reference.present});
  library.build();

  const Holed lost = with_hole(page, 120, 147);
  EXPECT_FALSE(library.fill(lost.data, lost.present, 120, 147, SIZE_MAX)
                   .fill.has_value());
}

// A reference read from a record file carries no mask, so its lost packets
// are recognisable only as runs of 0x00 — which are never matched on.
TEST(NabtsRecordLibrary, TakesZeroBytesInAReferenceAsLost) {
  const std::vector<uint8_t> page = varied(300, 11);
  std::vector<uint8_t> reference = page;
  std::fill(reference.begin() + 125, reference.begin() + 135, 0);
  NabtsRecordLibrary library;
  library.add({"other", "k", reference, {}});
  library.build();

  const Holed lost = with_hole(page, 120, 147);
  EXPECT_FALSE(library.fill(lost.data, lost.present, 120, 147, SIZE_MAX)
                   .fill.has_value());
}

TEST(NabtsRecordLibrary, ReadsContextOnlyFromBytesThatArrived) {
  const std::vector<uint8_t> page = varied(300, 12);
  NabtsRecordLibrary library;
  library.add({"other", "k", page, {}});
  library.build();

  // A second loss 20 bytes to the left of the first leaves only 20 bytes of
  // left context there, which with a full right side is still enough.
  Holed lost = with_hole(page, 120, 147);
  for (size_t i = 90; i < 100; ++i) {
    lost.data[i] = 0;
    lost.present[i] = 0;
  }
  const NabtsLibraryAnswer answer =
      library.fill(lost.data, lost.present, 120, 147, SIZE_MAX);

  ASSERT_TRUE(answer.fill.has_value());
  EXPECT_EQ(answer.fill->context_left, 20u);
  EXPECT_EQ(answer.fill->bytes, slice(page, 120, 147));
}

TEST(NabtsRecordLibrary, NeedsAnAnchorsWorthOfContextOnBothSides) {
  const std::vector<uint8_t> page = varied(300, 13);
  NabtsRecordLibrary library;
  library.add({"other", "k", page, {}});
  library.build();

  // Lost from the record's fifth byte: too little on the left to anchor.
  const Holed lost = with_hole(page, 5, 32);
  const NabtsLibraryAnswer answer =
      library.fill(lost.data, lost.present, 5, 32, SIZE_MAX);
  EXPECT_FALSE(answer.fill.has_value());
  EXPECT_EQ(answer.refusal, NabtsLibraryRefusal::kNoMatch);
}

// Bytes received wrong come in bursts. Asking for one, the others beside it
// are stepped over rather than ending its context, so each is answered from
// the agreement all round the burst.
TEST(NabtsRecordLibrary, CorrectsAWrongByteBesideOthersTheRecordingGotWrong) {
  const std::vector<uint8_t> page = varied(300, 21);
  NabtsRecordLibrary library;
  library.add({"other", "k", page, {}});
  library.build();

  std::vector<uint8_t> received = page;
  std::vector<uint8_t> wrong(page.size(), 0);
  for (const size_t at : {140u, 142u, 145u}) {
    received[at] ^= 0x01;
    wrong[at] = 1;
  }
  for (const size_t at : {140u, 142u, 145u}) {
    const NabtsLibraryAnswer answer =
        library.correct(received, {}, wrong, at, at + 1, SIZE_MAX);
    ASSERT_TRUE(answer.fill.has_value()) << "byte " << at;
    EXPECT_EQ(answer.fill->bytes, slice(page, at, at + 1)) << "byte " << at;
  }

  // As a lost stretch would be asked, the neighbours end the context at once.
  std::vector<uint8_t> present(page.size(), 1);
  present[142] = 0;
  present[145] = 0;
  EXPECT_FALSE(
      library.fill(received, present, 140, 141, SIZE_MAX).fill.has_value());
}

// Only the bytes marked wrong are stepped over: one real difference beside a
// wrong byte still ends its context.
TEST(NabtsRecordLibrary, StepsOverOnlyTheBytesMarkedWrong) {
  const std::vector<uint8_t> page = varied(300, 22);
  NabtsRecordLibrary library;
  library.add({"other", "k", page, {}});
  library.build();

  std::vector<uint8_t> received = page;
  std::vector<uint8_t> wrong(page.size(), 0);
  received[150] ^= 0x01;
  wrong[150] = 1;
  received[151] ^= 0x03;  // differs, and is not marked: context ends here
  received[149] ^= 0x03;
  EXPECT_FALSE(library.correct(received, {}, wrong, 150, 151, SIZE_MAX)
                   .fill.has_value());
}

}  // namespace
}  // namespace orc
