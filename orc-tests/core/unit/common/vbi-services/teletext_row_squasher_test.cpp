/*
 * File:        teletext_row_squasher_test.cpp
 * Module:      orc-tests/core/unit
 * Purpose:     Tests for the teletext row squasher (multi-copy combination)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi-services/teletext_row_squasher.h"

#include <gtest/gtest.h>

#include <string>

#include "vbi-services/teletext_page_decoder.h"

namespace orc {
namespace tests {
namespace {

constexpr TeletextPageKey kPage{1, 0x00, 0};

// Build a row of odd-parity display bytes from ASCII text, space-padded.
TeletextRowBytes row_of(const std::string& text) {
  TeletextRowBytes bytes{};
  for (size_t i = 0; i < kTeletextRowBytes; ++i) {
    const char c = i < text.size() ? text[i] : ' ';
    bytes[i] = teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return bytes;
}

// Read a squashed row back as text, stripping parity.
std::string text_of(const TeletextRowBytes& bytes) {
  std::string text;
  for (const uint8_t byte : bytes) {
    text.push_back(static_cast<char>(byte & 0x7F));
  }
  while (!text.empty() && text.back() == ' ') {
    text.pop_back();
  }
  return text;
}

// Uniform per-byte confidence, for tests that vary how sure a whole copy is.
TeletextRowConfidence confidence_of(float value) {
  TeletextRowConfidence confidence{};
  confidence.fill(value);
  return confidence;
}

// Record one copy of row 1 of kPage, every byte of it recovered with
// |confidence|.
void add_copy(TeletextRowSquasher& squasher, const TeletextRowBytes& bytes,
              int64_t source, float confidence) {
  const TeletextRowConfidence weights = confidence_of(confidence);
  squasher.add_row(kPage, 1, bytes, source, &weights);
}

}  // namespace

TEST(TeletextRowSquasher, SingleCopyIsReturnedUnchanged) {
  TeletextRowSquasher squasher;
  const auto row = row_of("HELLO");
  squasher.add_row(kPage, 1, row, /*source=*/0);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ(*squashed, row);
  EXPECT_EQ(squasher.copy_count(kPage, 1), 1u);
}

TEST(TeletextRowSquasher, UnseenRowHasNoResult) {
  TeletextRowSquasher squasher;
  EXPECT_FALSE(squasher.squashed_row(kPage, 1).has_value());
  EXPECT_EQ(squasher.copy_count(kPage, 1), 0u);
}

// The worked example from vhs-teletext's HOW_IT_WORKS: three copies of a row
// damaged in different places combine to the original.
TEST(TeletextRowSquasher, MostFrequentByteWinsAcrossCopies) {
  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 1, row_of("HELLO"), 0);
  squasher.add_row(kPage, 1, row_of("HELLP"), 1);
  squasher.add_row(kPage, 1, row_of("MELLO"), 2);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ(text_of(*squashed), "HELLO");
}

// Odd parity (EN 300 706 §8.1) detects every single-bit error, so a byte that
// fails it is known to be corrupt and must not win however often it appears.
TEST(TeletextRowSquasher, ParityCleanByteBeatsAMoreFrequentDamagedOne) {
  TeletextRowSquasher squasher;
  auto damaged = row_of("XELLO");
  damaged[0] ^= 0x01;  // break odd parity on the leading byte
  ASSERT_FALSE(teletext_odd_parity_valid(damaged[0]));

  squasher.add_row(kPage, 1, damaged, 0);
  squasher.add_row(kPage, 1, damaged, 1);
  squasher.add_row(kPage, 1, row_of("HELLO"), 2);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ(text_of(*squashed), "HELLO")
      << "the twice-seen but parity-damaged byte outvoted the clean one";
}

TEST(TeletextRowSquasher, FallsBackToPlainModeWhenEveryCopyIsDamaged) {
  TeletextRowSquasher squasher;
  auto damaged_a = row_of("A");
  damaged_a[0] ^= 0x01;
  auto damaged_b = row_of("B");
  damaged_b[0] ^= 0x01;
  ASSERT_FALSE(teletext_odd_parity_valid(damaged_a[0]));
  ASSERT_FALSE(teletext_odd_parity_valid(damaged_b[0]));

  squasher.add_row(kPage, 1, damaged_a, 0);
  squasher.add_row(kPage, 1, damaged_a, 1);
  squasher.add_row(kPage, 1, damaged_b, 2);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ((*squashed)[0], damaged_a[0]);  // the more frequent of the two
}

// A previewer rebuilding a sliding window re-reads the same recovered line
// many times; those repeats must not stuff the ballot.
TEST(TeletextRowSquasher, RepeatedSourceReplacesRatherThanRecounts) {
  TeletextRowSquasher squasher;
  for (int i = 0; i < 10; ++i) {
    squasher.add_row(kPage, 1, row_of("WRONG"), /*source=*/7);
  }
  squasher.add_row(kPage, 1, row_of("RIGHT"), /*source=*/8);
  squasher.add_row(kPage, 1, row_of("RIGHT"), /*source=*/9);

  EXPECT_EQ(squasher.copy_count(kPage, 1), 3u);
  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ(text_of(*squashed), "RIGHT");
}

// A different sub-code is a different page (EN 300 706 §9.3.1.2): merging
// rotating sub-pages would blend unrelated content.
TEST(TeletextRowSquasher, SubPagesAreKeptApart) {
  TeletextRowSquasher squasher;
  const TeletextPageKey sub1{1, 0x00, 1};
  const TeletextPageKey sub2{1, 0x00, 2};
  squasher.add_row(sub1, 1, row_of("SUBPAGE ONE"), 0);
  squasher.add_row(sub2, 1, row_of("SUBPAGE TWO"), 1);

  EXPECT_EQ(text_of(*squasher.squashed_row(sub1, 1)), "SUBPAGE ONE");
  EXPECT_EQ(text_of(*squasher.squashed_row(sub2, 1)), "SUBPAGE TWO");
  EXPECT_EQ(squasher.page_count(), 2u);
}

TEST(TeletextRowSquasher, OldestCopyIsDroppedBeyondTheBound) {
  TeletextRowSquasher::Options options;
  options.max_copies_per_row = 3;
  TeletextRowSquasher squasher(options);

  squasher.add_row(kPage, 1, row_of("OLD"), 0);
  squasher.add_row(kPage, 1, row_of("NEW"), 1);
  squasher.add_row(kPage, 1, row_of("NEW"), 2);
  squasher.add_row(kPage, 1, row_of("NEW"), 3);

  EXPECT_EQ(squasher.copy_count(kPage, 1), 3u);
  EXPECT_EQ(text_of(*squasher.squashed_row(kPage, 1)), "NEW");
}

TEST(TeletextRowSquasher, LeastRecentlyUpdatedPageIsEvicted) {
  TeletextRowSquasher::Options options;
  options.max_pages = 2;
  TeletextRowSquasher squasher(options);

  squasher.add_row({1, 0x00, 0}, 1, row_of("FIRST"), 0);
  squasher.add_row({1, 0x01, 0}, 1, row_of("SECOND"), 1);
  squasher.add_row({1, 0x02, 0}, 1, row_of("THIRD"), 2);

  EXPECT_EQ(squasher.page_count(), 2u);
  EXPECT_FALSE(squasher.squashed_row({1, 0x00, 0}, 1).has_value());
  EXPECT_TRUE(squasher.squashed_row({1, 0x02, 0}, 1).has_value());
}

// Row 0 is the page header, whose display bytes carry a real-time clock
// (§9.3.1.4) that legitimately differs between transmissions.
TEST(TeletextRowSquasher, HeaderAndOutOfRangeRowsAreNotStored) {
  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 0, row_of("HEADER"), 0);
  squasher.add_row(kPage, 25, row_of("ENHANCEMENT"), 1);

  EXPECT_EQ(squasher.copy_count(kPage, 0), 0u);
  EXPECT_EQ(squasher.copy_count(kPage, 25), 0u);
  EXPECT_EQ(squasher.page_count(), 0u);
}

// ---------------------------------------------------------------------------
// Confidence-weighted voting
//
// Copies recovered from tape are not equally believable. Where the detector can
// say how sure it was of a byte, the vote counts that instead of heads.
// ---------------------------------------------------------------------------

TEST(TeletextRowSquasher, ConfidentMinorityBeatsAnUncertainMajority) {
  TeletextRowSquasher squasher;
  add_copy(squasher, row_of("WRONG"), 0, 0.2F);
  add_copy(squasher, row_of("WRONG"), 1, 0.2F);
  add_copy(squasher, row_of("RIGHT"), 2, 0.9F);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ(text_of(*squashed), "RIGHT")
      << "two barely-decided copies outvoted one the detector was sure of";
}

TEST(TeletextRowSquasher, UncertainMinorityStillLosesToAConfidentMajority) {
  // The weighting is not a licence for one copy to overrule the transmission:
  // two confident copies still carry more weight than one.
  TeletextRowSquasher squasher;
  add_copy(squasher, row_of("RIGHT"), 0, 0.6F);
  add_copy(squasher, row_of("RIGHT"), 1, 0.6F);
  add_copy(squasher, row_of("WRONG"), 2, 1.0F);

  EXPECT_EQ(text_of(*squasher.squashed_row(kPage, 1)), "RIGHT");
}

TEST(TeletextRowSquasher, ParityStillOutranksConfidence) {
  // Parity is certain where confidence is only graded: a byte known to be
  // corrupt cannot win however sure the detector was of the bits it read.
  TeletextRowSquasher squasher;
  auto damaged = row_of("XELLO");
  damaged[0] ^= 0x01;
  ASSERT_FALSE(teletext_odd_parity_valid(damaged[0]));

  add_copy(squasher, damaged, 0, 1.0F);
  add_copy(squasher, damaged, 1, 1.0F);
  add_copy(squasher, row_of("HELLO"), 2, 0.1F);

  EXPECT_EQ(text_of(*squasher.squashed_row(kPage, 1)), "HELLO");
}

TEST(TeletextRowSquasher, CopiesWithoutConfidenceVoteAtFullWeight) {
  // A caller that cannot say — a threshold-detected packet, or an observation
  // stored before confidences existed — must not be discounted against one
  // that can.
  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 1, row_of("RIGHT"), 0);
  add_copy(squasher, row_of("WRONG"), 1, 0.9F);
  EXPECT_EQ(text_of(*squasher.squashed_row(kPage, 1)), "RIGHT");

  // Against a copy that measured itself as certain the two weigh the same, and
  // the tie falls to the newest — the rule an unweighted vote always used.
  TeletextRowSquasher tied;
  tied.add_row(kPage, 1, row_of("OLDER"), 0);
  add_copy(tied, row_of("NEWER"), 1, 1.0F);
  EXPECT_EQ(text_of(*tied.squashed_row(kPage, 1)), "NEWER");
}

TEST(TeletextRowSquasher, WeightsAreCountedPerByteNotPerCopy) {
  // Confidence is per byte, and a copy read well in one place and badly in
  // another contributes accordingly rather than as a whole: here neither copy
  // wins outright, and the row that comes out is taken from both.
  TeletextRowSquasher squasher;
  auto first_strong = confidence_of(0.1F);
  first_strong[0] = 0.9F;
  auto last_strong = confidence_of(0.1F);
  last_strong[4] = 0.9F;

  squasher.add_row(kPage, 1, row_of("HELLO"), 0, &first_strong);
  squasher.add_row(kPage, 1, row_of("XELLX"), 1, &last_strong);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ(text_of(*squashed), "HELLX");
}

TEST(TeletextRowSquasher, ReplacedCopyBringsItsNewConfidence) {
  // Re-reading a line replaces the copy; its confidence has to travel with it,
  // or a re-read would leave the old weight voting for the new bytes.
  TeletextRowSquasher squasher;
  add_copy(squasher, row_of("WRONG"), /*source=*/7, 0.9F);
  add_copy(squasher, row_of("RIGHT"), /*source=*/8, 0.5F);
  ASSERT_EQ(text_of(*squasher.squashed_row(kPage, 1)), "WRONG");

  add_copy(squasher, row_of("WRONG"), /*source=*/7, 0.1F);
  EXPECT_EQ(text_of(*squasher.squashed_row(kPage, 1)), "RIGHT");
  EXPECT_EQ(squasher.copy_count(kPage, 1), 2u);
}

// ---------------------------------------------------------------------------
// Column ranges: a service that carries one display row in more than one packet
// (525-line WST sends columns 32-39 separately) records each part on its own
// ---------------------------------------------------------------------------

// A copy of columns 32-39 only, holding |text| there.
TeletextRowBytes tail_of(const std::string& text) {
  TeletextRowBytes bytes{};
  for (size_t i = 0; i < 8; ++i) {
    const char c = i < text.size() ? text[i] : ' ';
    bytes[32 + i] = teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return bytes;
}

TEST(TeletextRowSquasher, PartialCopiesVoteOnlyWithinTheirColumns) {
  TeletextRowSquasher squasher;
  // Two copies of the row's own columns and two of its extension columns. The
  // extension copies hold zero everywhere else and must not drag those columns
  // down; the row copies hold zero from 32 on and must not reach the tail.
  squasher.add_row(kPage, 1, row_of("HEAD"), 0, nullptr, 0, 32);
  squasher.add_row(kPage, 1, row_of("HEAD"), 1, nullptr, 0, 32);
  squasher.add_row(kPage, 1, tail_of("TAIL"), 2, nullptr, 32, 8);
  squasher.add_row(kPage, 1, tail_of("TAIL"), 3, nullptr, 32, 8);

  TeletextRowCoverage covered{};
  const auto squashed = squasher.squashed_row(kPage, 1, &covered);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ(text_of(*squashed), "HEAD" + std::string(28, ' ') + "TAIL");
  for (size_t column = 0; column < kTeletextRowBytes; ++column) {
    EXPECT_TRUE(covered[column]) << "column " << column;
  }
}

TEST(TeletextRowSquasher, ColumnsNoCopySpokeForAreReportedUncovered) {
  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 1, row_of("HEAD"), 0, nullptr, 0, 32);
  squasher.add_row(kPage, 1, row_of("HEAD"), 1, nullptr, 0, 32);

  TeletextRowCoverage covered{};
  ASSERT_TRUE(squasher.squashed_row(kPage, 1, &covered).has_value());
  for (size_t column = 0; column < 32; ++column) {
    EXPECT_TRUE(covered[column]) << "column " << column;
  }
  for (size_t column = 32; column < kTeletextRowBytes; ++column) {
    EXPECT_FALSE(covered[column]) << "column " << column;
  }
}

TEST(TeletextRowSquasher, ASingleCopyReportsOnlyItsOwnColumns) {
  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 1, tail_of("TAIL"), 0, nullptr, 32, 8);

  TeletextRowCoverage covered{};
  ASSERT_TRUE(squasher.squashed_row(kPage, 1, &covered).has_value());
  EXPECT_FALSE(covered[0]);
  EXPECT_TRUE(covered[32]);
  EXPECT_TRUE(covered[39]);
}

TEST(TeletextRowSquasher, ADamagedPartialCopyLosesToTheCleanOnes) {
  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 1, tail_of("RECOVERY"), 0, nullptr, 32, 8);
  TeletextRowBytes damaged = tail_of("RECOVERY");
  damaged[32] ^= 0x01;  // breaks odd parity
  damaged[33] ^= 0x01;
  squasher.add_row(kPage, 1, damaged, 1, nullptr, 32, 8);
  squasher.add_row(kPage, 1, tail_of("RECOVERY"), 2, nullptr, 32, 8);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  // Only the extension columns were spoken for; the rest hold no candidate.
  EXPECT_EQ(text_of(*squashed).substr(32), "RECOVERY");
}

TEST(TeletextRowSquasher, CopyCountIgnoresCopiesThatOnlyExtendTheRow) {
  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 1, row_of("HEAD"), 0, nullptr, 0, 32);
  squasher.add_row(kPage, 1, tail_of("TAIL"), 1, nullptr, 32, 8);
  squasher.add_row(kPage, 1, tail_of("TAIL"), 2, nullptr, 32, 8);

  EXPECT_EQ(squasher.copy_count(kPage, 1), 1u);
}

TEST(TeletextRowSquasher, CopyCountAtAColumnCountsTheCopiesThatSpeakForIt) {
  // The packets that extend a row are counted where they speak: a consumer
  // weighing the columns an extension carries asks how often those arrived.
  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 1, row_of("HEAD"), 0, nullptr, 0, 32);
  squasher.add_row(kPage, 1, row_of("HEAD"), 1, nullptr, 0, 32);
  squasher.add_row(kPage, 1, tail_of("TAIL"), 2, nullptr, 32, 8);

  EXPECT_EQ(squasher.copy_count(kPage, 1, 0), 2u);
  EXPECT_EQ(squasher.copy_count(kPage, 1, 31), 2u);
  EXPECT_EQ(squasher.copy_count(kPage, 1, 32), 1u);
  EXPECT_EQ(squasher.copy_count(kPage, 1, 39), 1u);
  EXPECT_EQ(squasher.copy_count(kPage, 2, 32), 0u);
}

TEST(TeletextRowSquasher, EvictionKeepsBothHalvesOfASplitRow) {
  TeletextRowSquasher::Options options;
  options.max_copies_per_row = 4;
  TeletextRowSquasher squasher(options);

  // The row's own columns saturate the bound; its extension columns arrive
  // afterwards and must not be locked out, nor evict the row itself.
  for (int64_t source = 0; source < 8; ++source) {
    squasher.add_row(kPage, 1, row_of("HEAD"), source, nullptr, 0, 32);
  }
  squasher.add_row(kPage, 1, tail_of("TAIL"), 100, nullptr, 32, 8);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ(text_of(*squashed), "HEAD" + std::string(28, ' ') + "TAIL");
  EXPECT_EQ(squasher.copy_count(kPage, 1), 4u);
}

// Rows a service really transmits, forty columns wide, so a copy of one agrees
// with a copy of another only by coincidence — five positions in forty here.
const char kRowText[] = "REAGAN BUDGET DRAWS REPUBLICAN ANGER TODA";
const char kOtherRowText[] = "FEEDER CATTLE PRICES FOR MONDAY OPENED AT";
const char kThirdRowText[] = "RASPBERRY CHICKEN WITH NEW POTATOES TASTE";

// A page whose header was lost leaves the one before it open, and the MRAG is
// Hamming 8/4 and mis-corrects on a burst — either way a packet of some other
// page is filed here. It cannot be told apart by parity or by confidence: it is
// a clean read of a real row, only not of this one. What gives it away is that
// it agrees with almost none of what the copies of this row say.
//
// The damage it does is at the positions where the copies of the row are split
// — a byte one of them lost to parity is a byte the intruder can win — so that
// is where the rule has to hold.
TEST(TeletextRowSquasher, AnIntruderDoesNotDecideAContestedPosition) {
  auto damaged = row_of(kRowText);
  damaged[0] ^= 0x01;  // break odd parity on the leading byte (§8.1)
  ASSERT_FALSE(teletext_odd_parity_valid(damaged[0]));

  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 1, row_of(kRowText), 0);
  squasher.add_row(kPage, 1, damaged, 1);
  // Its leading byte is parity-clean and it is the newest copy, so without the
  // rule it takes the position the row's own copies could not agree on.
  squasher.add_row(kPage, 1, row_of(kOtherRowText), 2);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ(text_of(*squashed), std::string(kRowText).substr(0, 40))
      << "the intruder broke a tie it should not have been in";
}

// Where most of the copies disagree with the provisional row there is no row
// for the rest to be outliers of — the page is being assembled out of intruders
// and no vote among them is worth more than another, so the vote is left as it
// was rather than emptying the row of everything that reached it.
TEST(TeletextRowSquasher, NothingIsDroppedWhenTheCopiesHaveNoMajority) {
  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 1, row_of(kRowText), 0);
  squasher.add_row(kPage, 1, row_of(kOtherRowText), 1);
  squasher.add_row(kPage, 1, row_of(kThirdRowText), 2);

  TeletextRowCoverage covered{};
  const auto squashed = squasher.squashed_row(kPage, 1, &covered);
  ASSERT_TRUE(squashed.has_value());
  for (size_t i = 0; i < kTeletextRowBytes; ++i) {
    EXPECT_TRUE(covered[i]) << "position " << i << " lost every copy it had";
  }
  // Position 2 is one no two copies agree on, so it still falls to the newest,
  // which is what a plain Level 1 decoder would be showing.
  EXPECT_EQ((*squashed)[2], teletext_odd_parity_encode(kThirdRowText[2]));
}

// Two copies cannot tell which of them is the intruder, so neither is dropped.
TEST(TeletextRowSquasher, TwoDisagreeingCopiesStillBothVote) {
  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 1, row_of(kRowText), 0);
  squasher.add_row(kPage, 1, row_of(kOtherRowText), 1);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  // Ties go to the newest copy, as they did before outlier rejection existed.
  EXPECT_EQ(text_of(*squashed), std::string(kOtherRowText).substr(0, 40));
}

// A copy of the row damaged in several places is still a copy of the row: the
// rule is about copies that agree with almost none of it.
TEST(TeletextRowSquasher, ABadlyDamagedCopyIsStillACopy) {
  auto damaged = row_of(kRowText);
  for (size_t i = 0; i < 16; ++i) {
    damaged[i] ^= 0x01;  // break odd parity over the first sixteen bytes
  }

  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 1, row_of(kRowText), 0);
  squasher.add_row(kPage, 1, damaged, 1);
  squasher.add_row(kPage, 1, damaged, 2);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ(text_of(*squashed), std::string(kRowText).substr(0, 40))
      << "a copy the tape damaged was mistaken for a copy of another row";
}

// Each half of a split row is judged among its own copies: an intruder across
// the eight extension columns is neither weighed against the thirty-two the
// display packet brought nor rescued by them.
TEST(TeletextRowSquasher, EachHalfOfASplitRowJudgesItsOwnCopies) {
  const auto head = row_of(std::string(kRowText).substr(0, 32));
  TeletextRowBytes tail{};
  TeletextRowBytes intruder{};
  for (size_t i = 0; i < 8; ++i) {
    tail[32 + i] =
        teletext_odd_parity_encode(static_cast<uint8_t>(" IN 1984"[i]));
    intruder[32 + i] =
        teletext_odd_parity_encode(static_cast<uint8_t>("XKQZWJVG"[i]));
  }

  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 1, head, 0, nullptr, 0, 32);
  squasher.add_row(kPage, 1, head, 1, nullptr, 0, 32);
  squasher.add_row(kPage, 1, tail, 2, nullptr, 32, 8);
  squasher.add_row(kPage, 1, tail, 3, nullptr, 32, 8);
  squasher.add_row(kPage, 1, intruder, 4, nullptr, 32, 8);

  const auto squashed = squasher.squashed_row(kPage, 1);
  ASSERT_TRUE(squashed.has_value());
  EXPECT_EQ(text_of(*squashed),
            std::string(kRowText).substr(0, 32) + " IN 1984");
}

TEST(TeletextRowSquasher, ClearDropsEverything) {
  TeletextRowSquasher squasher;
  squasher.add_row(kPage, 1, row_of("TEXT"), 0);
  ASSERT_EQ(squasher.page_count(), 1u);

  squasher.clear();

  EXPECT_EQ(squasher.page_count(), 0u);
  EXPECT_FALSE(squasher.squashed_row(kPage, 1).has_value());
}

}  // namespace tests
}  // namespace orc
