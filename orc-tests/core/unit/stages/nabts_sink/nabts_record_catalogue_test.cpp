/*
 * File:        nabts_record_catalogue_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the bounded NABTS record catalogue
 *
 * Covers: the {channel, address, version} identity CEA-516 §5.2.1 gives a
 * record, which copy of a repeated record is kept, the appearance counts a
 * reader compares to judge a recording, application function rendering, and the
 * cap that bounds a run by the size of the service rather than the length of
 * the recording.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_record_catalogue.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "nabts_data_group.h"
#include "nabts_test_builders.h"

namespace orc_unit_test {
namespace {

/// The long form of short address |short_address| (§5.2.5: 0000PQR00).
constexpr uint64_t long_of(uint16_t short_address) {
  return static_cast<uint64_t>(short_address) << 8;
}

/**
 * @brief A message as the record assembler would deliver it
 *
 * Built directly rather than decoded from bytes: these tests are about what the
 * catalogue does with a message, so stating the message is clearer than
 * encoding one and hoping it came back as intended. nabts_record_test.cpp is
 * where the decoding is pinned down.
 */
orc::NabtsMessage message(uint16_t channel, uint16_t short_address,
                          uint8_t version, std::vector<uint8_t> data,
                          bool complete = true, bool intact = true,
                          bool aligned = true) {
  orc::NabtsMessage out;
  // The assembler never emits an incomplete message as aligned: a missing link
  // takes bytes out of the middle of the concatenation, so everything after it
  // moves earlier (see NabtsMessage::aligned).
  out.aligned = aligned && complete;
  out.channel = channel;
  out.address.value = long_of(short_address);
  out.address.long_form = false;
  out.type = orc::kNabtsRecordTypeCyclicPresentation;
  out.version = version;
  out.classification.present = true;
  out.classification.version_present = true;
  out.classification.version = version;
  out.data = std::move(data);
  out.records = 1;
  out.links_expected = 1;
  out.complete = complete;
  out.intact = intact;
  return out;
}

/// |count| distinguishable bytes starting at |first|.
std::vector<uint8_t> bytes(uint8_t first, size_t count) {
  std::vector<uint8_t> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    out.push_back(static_cast<uint8_t>(first + i));
  }
  return out;
}

////////////////////////////////////////////////////////////////////////////////////////////
// Identity
////////////////////////////////////////////////////////////////////////////////////////////

/**
 * @brief Catalogued records with their presentation code run
 *
 * Recovery catalogues the record data; running it is a separate step, because
 * what a page looks like depends on the receiver being emulated (X3.110 §6.2.3
 * sizes DRCS storage from it). A test that reads a page therefore presents the
 * records first, exactly as a browser does.
 */
std::vector<orc::NabtsCataloguedRecord> presented(
    const orc::NabtsRecordCatalogue& catalogue) {
  std::vector<orc::NabtsCataloguedRecord> records = catalogue.records();
  orc::nabts_interpret_records(records, orc::kNaplpsGridReference);
  return records;
}

// §7.1.2: a cyclic service brings the same record round throughout a recording,
// so a hundred copies are one catalogue entry seen a hundred times.
TEST(NabtsRecordCatalogue, CataloguesRepeatedCopiesAsOneRecord) {
  orc::NabtsRecordCatalogue catalogue;
  for (uint64_t frame = 0; frame < 5; ++frame) {
    catalogue.merge(message(0x000, 0x1A4, 2, bytes(0x40, 30)), frame);
  }

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].times_seen, 5u);
  EXPECT_EQ(records[0].times_intact, 5u);
  EXPECT_EQ(records[0].first_seen_frame, 0u);
  EXPECT_EQ(records[0].last_seen_frame, 4u);
  EXPECT_EQ(records[0].address_text, "1A4");
  EXPECT_EQ(records[0].channel_text, "000/1A4");
}

// §5.2.1 makes the version part of a record's identity: a service that revises
// a page increments Y16, and the old and new are different records sharing an
// address.
TEST(NabtsRecordCatalogue, KeepsVersionsOfOneAddressApart) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x1A4, 1, bytes(0x40, 10)), 0);
  catalogue.merge(message(0x000, 0x1A4, 2, bytes(0x50, 20)), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].version, 1u);
  EXPECT_EQ(records[0].data.size(), 10u);
  EXPECT_EQ(records[1].version, 2u);
  EXPECT_EQ(records[1].data.size(), 20u);
}

// §3.2.3 makes the packet address the data channel, and the same record address
// on two channels is two records.
TEST(NabtsRecordCatalogue, KeepsChannelsApart) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 10)), 0);
  catalogue.merge(message(0x200, 0x001, 0, bytes(0x50, 10)), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].channel, 0x000u);
  EXPECT_EQ(records[1].channel, 0x200u);
}

// Ordered by {channel, address, version}, which is how a reader would list a
// service.
TEST(NabtsRecordCatalogue, ListsRecordsInChannelThenAddressThenVersionOrder) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x200, 0x001, 0, bytes(0x40, 4)), 0);
  catalogue.merge(message(0x000, 0x1A4, 3, bytes(0x40, 4)), 1);
  catalogue.merge(message(0x000, 0x1A4, 1, bytes(0x40, 4)), 2);
  catalogue.merge(message(0x000, 0x002, 0, bytes(0x40, 4)), 3);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 4u);
  EXPECT_EQ(records[0].channel_text, "000/002");
  EXPECT_EQ(records[1].channel_text, "000/1A4");
  EXPECT_EQ(records[1].version, 1u);
  EXPECT_EQ(records[2].channel_text, "000/1A4");
  EXPECT_EQ(records[2].version, 3u);
  EXPECT_EQ(records[3].channel_text, "200/001");
}

// §5.2.5's equivalence is the address value's business, so a record transmitted
// once with a short address and once with its long equivalent is one record.
TEST(NabtsRecordCatalogue, TreatsAShortAndItsLongAddressAsOneRecord) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x1A4, 0, bytes(0x40, 10)), 0);

  auto long_form = message(0x000, 0x1A4, 0, bytes(0x40, 10));
  long_form.address.long_form = true;
  catalogue.merge(long_form, 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].times_seen, 2u);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Which copy is kept
////////////////////////////////////////////////////////////////////////////////////////////

// A clean copy is never replaced by a damaged one, so a service recorded off
// air keeps the copy that arrived cleanly rather than the last one before the
// tape ran out.
TEST(NabtsRecordCatalogue, ACleanCopyIsNotReplacedByADamagedOne) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 30)), 0);
  // Later and longer — the same page with bytes after it that no clean copy
  // carried — but damaged.
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 60), /*complete=*/false,
                          /*intact=*/false),
                  1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, bytes(0x40, 30));
  EXPECT_EQ(records[0].times_seen, 2u);
  EXPECT_EQ(records[0].times_intact, 1u);
}

// And a damaged copy is replaced the moment a clean one arrives, however short.
TEST(NabtsRecordCatalogue, ADamagedCopyIsReplacedByACleanOne) {
  // The same page with five bytes failing their parity (0x41 has two bits set).
  auto battered = bytes(0x40, 30);
  for (size_t i = 0; i < 5; ++i) {
    battered[i] = 0x41;
  }
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, battered, /*complete=*/true,
                          /*intact=*/false),
                  0);
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 30)), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, bytes(0x40, 30));
  EXPECT_TRUE(records[0].complete);
}

// An incomplete copy is damaged for this purpose: §5.2.6 makes a message the
// whole linked series, and a series missing a link is missing record data.
TEST(NabtsRecordCatalogue, AnIncompleteCopyLosesToACompleteOne) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x70, 60), /*complete=*/false),
                  0);
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 30)), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, bytes(0x40, 30));
}

// Among copies of equal quality the longer one carries more of the record.
TEST(NabtsRecordCatalogue, AmongEqualQualityCopiesTheLongerWins) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 20), /*complete=*/false),
                  0);
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x70, 40), /*complete=*/false),
                  1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, bytes(0x70, 40));
}

// Replacing like with like would only churn, so a later copy of the same page
// as good as the first is counted and otherwise ignored — here the page re-sent
// with one byte changed, which is still the same page.
TEST(NabtsRecordCatalogue, AnEquallyGoodCopyDoesNotDisplaceTheFirst) {
  auto resent = bytes(0x40, 30);
  resent[5] = 0x70;
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 30)), 0);
  catalogue.merge(message(0x000, 0x001, 0, resent), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, bytes(0x40, 30));
  EXPECT_EQ(records[0].times_seen, 2u);
  EXPECT_EQ(records[0].times_intact, 2u);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Combining damaged copies
////////////////////////////////////////////////////////////////////////////////////////////

/// |value| with the odd parity CEA-516 §3.3 requires of every data byte.
uint8_t odd(uint8_t value) { return nabts::parity(value); }

/// A damaged copy: complete and aligned, so it votes, but not undamaged.
orc::NabtsMessage damaged(std::vector<uint8_t> data) {
  return message(0x000, 0x001, 0, std::move(data), /*complete=*/true,
                 /*intact=*/false);
}

/// A damaged copy with holes: |present| marks, per byte, whether it arrived.
orc::NabtsMessage holed(std::vector<uint8_t> data,
                        std::vector<uint8_t> present) {
  auto out = damaged(std::move(data));
  out.present = std::move(present);
  return out;
}

/// A damaged copy the detector measured: |confidence| is 0-255 per byte.
orc::NabtsMessage measured(std::vector<uint8_t> data,
                           std::vector<uint8_t> confidence) {
  auto out = damaged(std::move(data));
  out.confidence = std::move(confidence);
  return out;
}

// The point of the exercise: a byte no single copy has right is recovered from
// the copies that do, position by position.
TEST(NabtsRecordCatalogue, VotesDamagedCopiesIntoOneBestEstimate) {
  orc::NabtsRecordCatalogue catalogue;
  // Each copy is wrong in a different place; between them every position has a
  // majority.
  catalogue.merge(damaged({odd('N'), odd('B'), odd('!')}), 0);
  catalogue.merge(damaged({odd('N'), odd('?'), odd('C')}), 1);
  catalogue.merge(damaged({odd('N'), odd('B'), odd('C')}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data,
            (std::vector<uint8_t>{odd('N'), odd('B'), odd('C')}));
  EXPECT_EQ(records[0].copies_voted, 3u);
  EXPECT_EQ(records[0].times_intact, 0u);
}

// §3.3's odd parity detects every single-bit error, so a byte that fails it is
// known to be corrupt and loses to a parity-clean candidate however often it
// arrived.
TEST(NabtsRecordCatalogue, AParityCleanByteBeatsAMoreNumerousCorruptOne) {
  orc::NabtsRecordCatalogue catalogue;
  const uint8_t corrupt = static_cast<uint8_t>(odd('A') ^ 0x01);
  catalogue.merge(damaged({corrupt}), 0);
  catalogue.merge(damaged({corrupt}), 1);
  catalogue.merge(damaged({odd('A')}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('A')}));
}

// Where every copy of a position is damaged there is no clean candidate to
// prefer, and the most numerous reading is still the best available one.
TEST(NabtsRecordCatalogue, FallsBackToTheCommonestByteWhereEveryCopyIsDamaged) {
  orc::NabtsRecordCatalogue catalogue;
  const uint8_t a = static_cast<uint8_t>(odd('A') ^ 0x01);
  const uint8_t b = static_cast<uint8_t>(odd('A') ^ 0x02);
  catalogue.merge(damaged({a}), 0);
  catalogue.merge(damaged({b}), 1);
  catalogue.merge(damaged({a}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{a}));
}

/// A record's worth of odd-parity data bytes from ASCII text.
std::vector<uint8_t> odd_text(const char* text) {
  std::vector<uint8_t> out;
  for (const char* c = text; *c != '\0'; ++c) {
    out.push_back(odd(static_cast<uint8_t>(*c)));
  }
  return out;
}

// Records long enough to be judged and unlike enough that a copy of one agrees
// with a copy of another only by coincidence — four positions in twenty-nine.
constexpr char kRecordText[] = "REAGAN BUDGET DRAWS GOP ANGER";
constexpr char kOtherRecordText[] = "FEEDER CATTLE PRICES MONDAY U";
constexpr char kThirdRecordText[] = "RASPBERRY CHICKEN AND POTATOS";

// A mis-corrected packet address (§3.2.2) assembles a packet of some other
// record into this one, and reconcile_identities() folds in copies filed under
// an identity the recording never named. Either way the intruder is a clean
// read of real data — only not of this record — so parity and confidence have
// nothing to say about it, and the damage it does is at the positions where the
// copies of the record are split.
TEST(NabtsRecordCatalogue, AnIntruderDoesNotDecideAContestedPosition) {
  auto split = odd_text(kRecordText);
  split[0] = static_cast<uint8_t>(split[0] ^ 0x01);  // parity broken (§3.3)

  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(odd_text(kRecordText)), 0);
  catalogue.merge(damaged(split), 1);
  // Parity-clean at the contested position and the newest copy, so voted in it
  // would take a position the record's own copies could not agree on. It
  // carries a content of its own, and is catalogued as one.
  catalogue.merge(damaged(odd_text(kOtherRecordText)), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].data, odd_text(kRecordText));
  EXPECT_EQ(records[0].content, 0u);
  EXPECT_EQ(records[0].copies_voted, 2u);
  EXPECT_EQ(records[1].data, odd_text(kOtherRecordText));
  EXPECT_EQ(records[1].content, 1u);
}

// Where most of the copies disagree with the provisional reading there is no
// record for the rest to be outliers of, so the vote is left as it was rather
// than emptied of everything that reached it.
TEST(NabtsRecordCatalogue, NothingIsDroppedWhenTheCopiesHaveNoMajority) {
  const auto copy = [](const char* text) {
    const auto data = odd_text(text);
    return orc::NabtsRecordCopy{data, std::vector<uint8_t>(data.size(), 1), {}};
  };
  const orc::NabtsVoteResult voted = orc::nabts_vote_record(
      {copy(kRecordText), copy(kOtherRecordText), copy(kThirdRecordText)});
  ASSERT_EQ(voted.data.size(), odd_text(kRecordText).size());
  // Position 2 is one no two copies agree on, so it still falls to the newest.
  EXPECT_EQ(voted.data[2], odd(kThirdRecordText[2]));
}

// A carousel's sixteen versions (§5.2.7.2 Y16) come round again with new pages
// on them, so one identity carries three pages here. Each is catalogued as a
// record of its own, numbered in the order it first arrived; voted into one,
// the three would come out a per-byte blend of all of them.
TEST(NabtsRecordCatalogue, ThreeContentsUnderOneIdentityAreThreeRecords) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(odd_text(kRecordText)), 0);
  catalogue.merge(damaged(odd_text(kOtherRecordText)), 1);
  catalogue.merge(damaged(odd_text(kRecordText)), 2);
  catalogue.merge(damaged(odd_text(kThirdRecordText)), 3);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].data, odd_text(kRecordText));
  EXPECT_EQ(records[0].times_seen, 2u);
  EXPECT_EQ(records[0].content, 0u);
  EXPECT_EQ(records[1].data, odd_text(kOtherRecordText));
  EXPECT_EQ(records[1].first_seen_frame, 1u);
  EXPECT_EQ(records[1].content, 1u);
  EXPECT_EQ(records[2].data, odd_text(kThirdRecordText));
  EXPECT_EQ(records[2].content, 2u);
  EXPECT_EQ(orc::nabts_record_key(records[0]), "000-001-v0");
  EXPECT_EQ(orc::nabts_record_key(records[1]), "000-001-v0-2");
  EXPECT_EQ(orc::nabts_record_key(records[2]), "000-001-v0-3");
}

// Two pages that happen to agree on much of their bytes — the same template,
// a different advertisement — are still told apart by their length when both
// arrived intact: a record's length is its content's. (The copies were damaged
// here before: a copy that arrived complete is not known to be as long as its
// record — see ACopyCutShortAtItsTailIsTheSamePage — so only intact copies
// are told apart by length now.)
TEST(NabtsRecordCatalogue, IntactCopiesOfDifferentLengthsAreDifferentPages) {
  std::string longer_text = std::string(kRecordText) + " AND MORE BESIDES";
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, odd_text(kRecordText)), 0);
  catalogue.merge(message(0x000, 0x001, 0, odd_text(longer_text.c_str())), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[1].data, odd_text(longer_text.c_str()));
}

/// |count| odd-parity bytes of printable text drawn from |seed|: pages of two
/// seeds agree at a position about one time in ninety-five.
std::vector<uint8_t> page(uint32_t seed, size_t count) {
  std::vector<uint8_t> out;
  out.reserve(count);
  uint32_t state = seed * 2654435761u + 1u;
  for (size_t i = 0; i < count; ++i) {
    state = state * 1664525u + 1013904223u;
    out.push_back(odd(static_cast<uint8_t>(0x20 + (state >> 24) % 95)));
  }
  return out;
}

/// |data| with the bytes from |from| up to |to| misread the way a recording
/// misreads most often: two neighbouring bits, which leaves odd parity as it
/// was.
std::vector<uint8_t> misread_span(std::vector<uint8_t> data, size_t from,
                                  size_t to) {
  for (size_t i = from; i < to && i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>(data[i] ^ 0x03);
  }
  return data;
}

// A group whose last packets were lost ends at the next synchronizing packet on
// its channel, so a copy can arrive complete — every link of it — and still
// be short of its record. That is the commonest damage of all, and it is not a
// different page: judged by its length, it split 16,842 copies from their page
// across the recordings.
TEST(NabtsRecordCatalogue, ACopyCutShortAtItsTailIsTheSamePage) {
  const auto whole = page(1, 120);
  const std::vector<uint8_t> cut(whole.begin(), whole.begin() + 66);
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(whole), 0);
  catalogue.merge(damaged(whole), 1);
  catalogue.merge(damaged(cut), 2);
  // And a copy that did arrive intact is not told apart from one cut short.
  catalogue.merge(message(0x000, 0x001, 0, whole), 3);
  catalogue.merge(damaged(cut), 4);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].times_seen, 5u);
  EXPECT_EQ(records[0].data, whole);
}

// The copy a content keeps is the longest rather than the best, and its
// damage can fall exactly where a new copy's does not. A copy is the content's
// if it agrees with any copy held for the content's vote.
TEST(NabtsRecordCatalogue, ACopyIsJudgedAgainstEveryCopyHeldForItsContent) {
  const auto text = page(2, 40);
  // Damaged in its first half, and four bytes longer, so it becomes the copy
  // the content keeps.
  auto longer = misread_span(text, 0, 20);
  for (const uint8_t byte : page(3, 4)) {
    longer.push_back(byte);
  }

  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(text), 0);
  catalogue.merge(damaged(longer), 1);
  // Damaged in its second half: it agrees with the kept copy nowhere, and with
  // the first copy on half of it.
  catalogue.merge(damaged(misread_span(text, 20, 40)), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].times_seen, 3u);
  EXPECT_EQ(records[0].copies_voted, 3u);
}

// A packet lost without the continuity index showing it moves every byte after
// it by a block, further than a misread header moves a whole copy. Window by
// window the copy still lines up with its page.
TEST(NabtsRecordCatalogue, ACopyAPacketOutOfStepIsTheSamePage) {
  const auto whole = page(4, 216);
  std::vector<uint8_t> closed_up(whole.begin(), whole.begin() + 30);
  closed_up.insert(closed_up.end(), whole.begin() + 58, whole.end());

  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(whole), 0);
  catalogue.merge(damaged(closed_up), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].times_seen, 2u);
}

// Two copies damaged in different halves of a page agree with each other
// nowhere, so the second opens a content of its own; the undamaged copies after
// them agree with both. Once the last copy is in, what the copies held for the
// first content say is the page, and the second content is joined to it.
TEST(NabtsRecordCatalogue, ContentsTheLaterCopiesShowToBeOnePageAreJoined) {
  const auto text = page(5, 60);
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(misread_span(text, 0, 30)), 0);
  catalogue.merge(damaged(misread_span(text, 30, 60)), 1);
  catalogue.merge(damaged(text), 2);
  catalogue.merge(damaged(text), 3);
  ASSERT_EQ(catalogue.records().size(), 2u);

  catalogue.reconcile_identities();
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].times_seen, 4u);
  EXPECT_EQ(records[0].content, 0u);
  EXPECT_EQ(records[0].data, text);
}

// A content joined to an earlier one leaves no gap in the numbering: what is
// left is numbered from zero in the order it first arrived.
TEST(NabtsRecordCatalogue, ContentsLeftAfterAJoinAreNumberedInArrivalOrder) {
  const auto text = page(6, 60);
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(misread_span(text, 0, 30)), 0);
  catalogue.merge(damaged(misread_span(text, 30, 60)), 1);
  catalogue.merge(damaged(page(7, 60)), 2);
  catalogue.merge(damaged(text), 3);
  catalogue.merge(damaged(text), 4);
  ASSERT_EQ(catalogue.records().size(), 3u);

  catalogue.reconcile_identities();
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].content, 0u);
  EXPECT_EQ(records[0].data, text);
  EXPECT_EQ(records[1].content, 1u);
  EXPECT_EQ(records[1].data, page(7, 60));
  EXPECT_EQ(orc::nabts_record_key(records[1]), "000-001-v0-2");
}

/// |whole| as a copy whose group was given |extra| bytes of holes at |at| for
/// blocks it never lost: every byte after them |extra| late, and the copy as
/// long as its record.
orc::NabtsMessage late_by(const std::vector<uint8_t>& whole, size_t at,
                          size_t extra) {
  std::vector<uint8_t> data(whole.begin(),
                            whole.begin() + static_cast<std::ptrdiff_t>(at));
  data.insert(data.end(), extra, 0x00);
  data.insert(data.end(), whole.begin() + static_cast<std::ptrdiff_t>(at),
              whole.end() - static_cast<std::ptrdiff_t>(extra));
  std::vector<uint8_t> present(data.size(), 1);
  std::fill(present.begin() + static_cast<std::ptrdiff_t>(at),
            present.begin() + static_cast<std::ptrdiff_t>(at + extra), 0);
  return holed(std::move(data), std::move(present));
}

// A group given holes for two blocks it never lost — a continuity index
// misread where lines went by — puts every byte after them two blocks late,
// further than the windows look, so the copy opens a content of its own. Seen
// once, it is looked for that far again once the last copy is in, and is the
// page.
TEST(NabtsRecordCatalogue, ASingleSightingTwoBlocksLateIsJoinedToItsPage) {
  const auto whole = page(8, 300);
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(whole), 0);
  catalogue.merge(damaged(whole), 1);
  catalogue.merge(late_by(whole, 92, 54), 2);
  ASSERT_EQ(catalogue.records().size(), 2u);

  catalogue.reconcile_identities();
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].times_seen, 3u);
  EXPECT_EQ(records[0].data, whole);
}

// Seen twice the same way it is no different: every content of an identity is
// looked for that far (kNabtsJoinWindowSlip): a page can arrive twice two
// blocks late.
TEST(NabtsRecordCatalogue, AContentSeenTwiceTwoBlocksLateIsJoinedToItsPage) {
  const auto whole = page(9, 300);
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(whole), 0);
  catalogue.merge(late_by(whole, 92, 54), 1);
  catalogue.merge(late_by(whole, 92, 54), 2);
  catalogue.reconcile_identities();
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].times_seen, 3u);
}

// And a single sighting of another page is not taken for the page however far
// its windows look.
TEST(NabtsRecordCatalogue, ASingleSightingOfAnotherPageStaysApart) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(page(10, 300)), 0);
  catalogue.merge(damaged(page(10, 300)), 1);
  catalogue.merge(late_by(page(11, 300), 92, 54), 2);
  catalogue.reconcile_identities();
  EXPECT_EQ(catalogue.records().size(), 2u);
}

// A page re-sent with a few bytes changed — a score updated — is the same page
// for this purpose: it agrees with the one before on nearly everything, and
// the copies vote together.
TEST(NabtsRecordCatalogue, AFewChangedBytesAreTheSamePage) {
  auto updated = odd_text(kRecordText);
  updated[3] = odd('X');
  updated[4] = odd('Y');
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(odd_text(kRecordText)), 0);
  catalogue.merge(damaged(odd_text(kRecordText)), 1);
  catalogue.merge(damaged(updated), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, odd_text(kRecordText));
  EXPECT_EQ(records[0].times_seen, 3u);
}

// A misread record header starts a copy's data a few bytes early or late — here
// four Hamming 8/4 bytes of the header taken for data, as an RD whose announcer
// bits were mis-corrected does. Every byte of that copy then disagrees with the
// others, so as it stands it is an outlier; slid back into line it is a copy of
// the record like any other, and it supplies the byte the rest never received.
TEST(NabtsRecordCatalogue,
     ACopyWhoseDataStartedInTheWrongPlaceVotesSlidIntoLine) {
  constexpr size_t kHole = 10;
  auto holed_text = odd_text(kRecordText);
  std::vector<uint8_t> present(holed_text.size(), 1);
  holed_text[kHole] = 0x00;
  present[kHole] = 0;

  std::vector<uint8_t> slipped = {0x15, 0x02, 0x15, 0x15};
  const auto text = odd_text(kRecordText);
  slipped.insert(slipped.end(), text.begin(), text.end());

  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(holed(holed_text, present), 0);
  catalogue.merge(holed(holed_text, present), 1);
  catalogue.merge(damaged(slipped), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, odd_text(kRecordText));
  ASSERT_EQ(records[0].data_present.size(), text.size());
  EXPECT_EQ(records[0].data_present[kHole], 1u);
}

// A copy the tape damaged in many places is still a copy of the record: the
// rule is about copies that agree with almost none of it.
TEST(NabtsRecordCatalogue, ABadlyDamagedCopyStillVotes) {
  auto battered = odd_text(kRecordText);
  for (size_t i = 0; i < 10; ++i) {
    battered[i] = static_cast<uint8_t>(battered[i] ^ 0x01);
  }

  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(odd_text(kRecordText)), 0);
  catalogue.merge(damaged(battered), 1);
  catalogue.merge(damaged(battered), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, odd_text(kRecordText))
      << "a copy the recording damaged was mistaken for a copy of another "
         "record";
}

// A copy whose packets were lost has bytes missing from the middle of it
// (§3.2.4), so it no longer lines up with the others and must not vote — one
// admitted would corrupt every position after its hole.
TEST(NabtsRecordCatalogue, AMisalignedCopyDoesNotVote) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged({odd('N'), odd('B'), odd('C')}), 0);
  // Two misaligned copies agreeing on a different byte would carry the vote if
  // they were let into it.
  for (uint64_t frame = 1; frame <= 2; ++frame) {
    catalogue.merge(message(0x000, 0x001, 0, {odd('X'), odd('Y'), odd('Z')},
                            /*complete=*/true,
                            /*intact=*/false, /*aligned=*/false),
                    frame);
  }

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data,
            (std::vector<uint8_t>{odd('N'), odd('B'), odd('C')}));
  EXPECT_EQ(records[0].copies_voted, 1u);
  EXPECT_EQ(records[0].times_seen, 3u);
}

// The record is as long as the length its copies agree on; one cut short still
// votes over the bytes it does have.
TEST(NabtsRecordCatalogue, TakesTheLengthTheCopiesAgreeOnAndLetsAShortOneVote) {
  orc::NabtsRecordCatalogue catalogue;
  const uint8_t corrupt = static_cast<uint8_t>(odd('B') ^ 0x01);
  catalogue.merge(damaged({odd('N'), corrupt, odd('C')}), 0);
  catalogue.merge(damaged({odd('N'), odd('B'), odd('C')}), 1);
  catalogue.merge(damaged({odd('N'), odd('B')}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data,
            (std::vector<uint8_t>{odd('N'), odd('B'), odd('C')}));
}

// An undamaged copy is the record, so it is shown as it arrived rather than
// voted against the damaged copies that came before it.
TEST(NabtsRecordCatalogue, AnUndamagedCopyEndsTheVote) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged({odd('X'), odd('X'), odd('X')}), 0);
  catalogue.merge(damaged({odd('X'), odd('X'), odd('X')}), 1);
  catalogue.merge(message(0x000, 0x001, 0, {odd('N'), odd('B'), odd('C')}), 2);
  catalogue.merge(damaged({odd('X'), odd('X'), odd('X')}), 3);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data,
            (std::vector<uint8_t>{odd('N'), odd('B'), odd('C')}));
  EXPECT_EQ(records[0].copies_voted, 0u);
  EXPECT_EQ(records[0].times_intact, 1u);
}

// A carousel runs for the length of the recording, so the copies retained are
// bounded — and it is the most recent that are kept.
TEST(NabtsRecordCatalogue, RetainsOnlyTheMostRecentCopiesForTheVote) {
  orc::NabtsRecordCatalogue catalogue(/*max_records=*/8, /*max_copies=*/2);
  catalogue.merge(damaged({odd('A')}), 0);
  catalogue.merge(damaged({odd('B')}), 1);
  catalogue.merge(damaged({odd('C')}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].copies_voted, 2u);
  // 'A' has been evicted; 'B' and 'C' each have one vote, and the tie goes to
  // the more recent.
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('C')}));
}

// By default the most recent sixty-four copies vote: on the damaged tapes the
// vote keeps improving past thirty-two (kMaxCopiesPerRecord).
TEST(NabtsRecordCatalogue, VotesFromTheSixtyFourMostRecentCopiesByDefault) {
  orc::NabtsRecordCatalogue catalogue;
  for (uint64_t frame = 0; frame < 70; ++frame) {
    catalogue.merge(damaged({odd('N'), odd('B'), odd('C')}), frame);
  }

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].copies_voted, 64u);
  EXPECT_EQ(records[0].times_seen, 70u);
}

// §7.2.2's descriptors are read out of the record data, so a vote that recovers
// a byte has to be reflected in them too.
TEST(NabtsRecordCatalogue, RereadsApplicationDescriptorsFromTheVotedData) {
  orc::NabtsRecordCatalogue catalogue;
  const auto application = [](std::vector<uint8_t> data) {
    auto out = damaged(std::move(data));
    out.type = orc::kNabtsRecordTypeApplication;
    return out;
  };
  // 2/0 with arguments "AB", the second copy damaged in the 'B'.
  catalogue.merge(application({odd(0x20), odd('A'), odd('B')}), 0);
  catalogue.merge(
      application({odd(0x20), odd('A'), static_cast<uint8_t>(odd('B') ^ 0x01)}),
      1);
  catalogue.merge(application({odd(0x20), odd('A'), odd('B')}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  ASSERT_EQ(records[0].functions.size(), 1u);
  EXPECT_EQ(records[0].functions[0].code, "2/0");
  EXPECT_EQ(records[0].functions[0].arguments, "AB");
}

// A lost packet's bytes are held open as a hole rather than closing up, so a
// copy abstains where it received nothing and still votes either side of it.
// This is what lets a recording that never received any one copy whole have
// every position decided by whichever copies did receive it.
TEST(NabtsRecordCatalogue, ACopyAbstainsWhereItsPacketsWereLost) {
  orc::NabtsRecordCatalogue catalogue;
  // Two copies, each holding a hole where the other has the byte.
  catalogue.merge(holed({odd('N'), 0x00, odd('C')}, {1, 0, 1}), 0);
  catalogue.merge(holed({odd('N'), odd('B'), 0x00}, {1, 1, 0}), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data,
            (std::vector<uint8_t>{odd('N'), odd('B'), odd('C')}));
}

// A hole never outvotes a byte, however many copies share it: a copy that
// received nothing at a position has nothing to say about it.
TEST(NabtsRecordCatalogue, HolesDoNotOutvoteTheOneCopyThatArrived) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(holed({0x00}, {0}), 0);
  catalogue.merge(holed({0x00}, {0}), 1);
  catalogue.merge(holed({odd('A')}, {1}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('A')}));
}

// Where no copy received a position at all it stays NUL, which X3.110 §6.1.4
// makes a transparent control — so the hole costs the drawing nothing beyond
// the bytes that were actually lost.
TEST(NabtsRecordCatalogue, APositionNoCopyReceivedIsLeftAsNul) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(holed({odd('N'), 0x00}, {1, 0}), 0);
  catalogue.merge(holed({odd('N'), 0x00}, {1, 0}), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('N'), 0x00}));
}

/// |whole| as a copy that lost the 27-byte block at |at| and holds it open.
orc::NabtsMessage lost_block(const std::vector<uint8_t>& whole, size_t at) {
  std::vector<uint8_t> data = whole;
  std::vector<uint8_t> present(whole.size(), 1);
  for (size_t i = at; i < at + 27; ++i) {
    data[i] = 0x00;
    present[i] = 0;
  }
  return holed(std::move(data), std::move(present));
}

/// |whole| as a copy whose group closed up over the 27-byte block at |at|: the
/// block is gone without a hole, and every byte after it is 27 bytes early.
orc::NabtsMessage closed_up(const std::vector<uint8_t>& whole, size_t at) {
  std::vector<uint8_t> data(whole.begin(),
                            whole.begin() + static_cast<std::ptrdiff_t>(at));
  data.insert(data.end(), whole.begin() + static_cast<std::ptrdiff_t>(at + 27),
              whole.end());
  return damaged(std::move(data));
}

// A recording that loses every packet on one line loses the same blocks from
// every copy of a record — except a copy whose group closed up over a block it
// lost without a hole. It agrees with the rest up to that block and holds the
// next one where the others hold nothing, so it was the only voter there and
// the record held the block 27 bytes early, its text repeated part-way through
// a word. The stretch stays lost.
TEST(NabtsRecordCatalogue, AClosedUpCopyDoesNotFillTheBlockTheOthersLost) {
  const auto whole = page(12, 216);
  orc::NabtsRecordCatalogue catalogue;
  for (uint64_t frame = 0; frame < 3; ++frame) {
    catalogue.merge(lost_block(whole, 135), frame);
  }
  catalogue.merge(closed_up(whole, 135), 3);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  ASSERT_EQ(records[0].data.size(), whole.size());
  for (size_t i = 0; i < whole.size(); ++i) {
    const uint8_t expected = i >= 135 && i < 162 ? 0x00 : whole[i];
    EXPECT_EQ(records[0].data[i], expected) << "byte " << i;
  }
}

// A copy that did receive the block lines up with the others before it, and
// fills it — even where its bytes after the block were misread, since it does
// not hold the record's bytes a block early there either.
TEST(NabtsRecordCatalogue, ACopyThatReceivedTheLostBlockStillFillsIt) {
  const auto whole = page(13, 216);
  orc::NabtsRecordCatalogue catalogue;
  for (uint64_t frame = 0; frame < 3; ++frame) {
    catalogue.merge(lost_block(whole, 135), frame);
  }
  catalogue.merge(damaged(misread_span(whole, 162, 180)), 3);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, whole);
}

// An absent mask is the ordinary case of a copy that lost nothing, and must not
// read as a copy that received nothing.
TEST(NabtsRecordCatalogue, AnAbsentMaskMeansEveryByteArrived) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(holed({0x00, 0x00}, {0, 0}), 0);
  catalogue.merge(damaged({odd('N'), odd('B')}), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('N'), odd('B')}));
}

// A copy contributes how sure the detector was of its byte rather than one vote
// apiece, so a value read cleanly outweighs more copies of one the detector
// nearly decided the other way.
TEST(NabtsRecordCatalogue, AConfidentByteOutweighsMoreNumerousDoubtfulOnes) {
  orc::NabtsRecordCatalogue catalogue;
  // Two barely-decided copies of 'X' against one clear reading of 'A'. Both
  // values carry correct parity, so only the weighting separates them.
  catalogue.merge(measured({odd('X')}, {10}), 0);
  catalogue.merge(measured({odd('X')}, {10}), 1);
  catalogue.merge(measured({odd('A')}, {255}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('A')}));
}

// And the weighting never overrides the parity gate: a byte known to be corrupt
// loses to a parity-clean one however sure the detector was of it.
TEST(NabtsRecordCatalogue, ConfidenceDoesNotOverrideTheParityGate) {
  orc::NabtsRecordCatalogue catalogue;
  const uint8_t corrupt = static_cast<uint8_t>(odd('A') ^ 0x01);
  catalogue.merge(measured({corrupt}, {255}), 0);
  catalogue.merge(measured({corrupt}, {255}), 1);
  catalogue.merge(measured({odd('A')}, {1}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('A')}));
}

/// A damaged copy whose mask says, per byte, what the suffix check made of the
/// block it arrived in: |level| for every byte.
orc::NabtsMessage checked(std::vector<uint8_t> data, uint8_t level) {
  std::vector<uint8_t> levels(data.size(), level);
  return holed(std::move(data), std::move(levels));
}

constexpr uint8_t kArrived = orc::kNabtsByteArrived;
constexpr uint8_t kVouched = orc::kNabtsByteVouched;

/// |text| with byte |position| misread as a recording misreads most often: two
/// neighbouring bits, which leaves odd parity as it was.
std::vector<uint8_t> misread(const char* text, size_t position) {
  auto out = odd_text(text);
  out[position] = static_cast<uint8_t>(out[position] ^ 0x03);
  return out;
}

// A recording misreads the same bits the same way copy after copy, and byte
// parity passes the commonest misreading, so copies that agree on a byte are no
// evidence against one whose block the suffix check vouched for (CEA-516
// §3.4).
TEST(NabtsRecordCatalogue, AVouchedByteBeatsAnyNumberOfCopiesTheCheckFailed) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(checked(misread(kRecordText, 5), kArrived), 0);
  catalogue.merge(checked(misread(kRecordText, 5), kArrived), 1);
  catalogue.merge(checked(odd_text(kRecordText), kVouched), 2);
  catalogue.merge(checked(misread(kRecordText, 5), kArrived), 3);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, odd_text(kRecordText));
  EXPECT_EQ(records[0].data_present,
            std::vector<uint8_t>(records[0].data.size(), kVouched))
      << "the record's mask says the check vouched for every byte it settled";
}

// The mask a vote hands on says which bytes the check vouched for, so what
// reads the record afterwards can believe them as the vote did. A byte the
// copies only agree on is a byte that arrived.
TEST(NabtsRecordCatalogue, TheVotedMaskSaysWhichBytesTheCheckVouchedFor) {
  auto levels = std::vector<uint8_t>(odd_text(kRecordText).size(), kArrived);
  std::fill(levels.begin(), levels.begin() + 10, kVouched);
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(holed(odd_text(kRecordText), levels), 0);
  catalogue.merge(checked(odd_text(kRecordText), kArrived), 1);
  catalogue.merge(checked(odd_text(kRecordText), kArrived), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data_present, levels);
}

// A record seen once is its copy, the check's marks and all.
TEST(NabtsRecordCatalogue, ASingleCopyKeepsWhatTheCheckMadeOfItsBytes) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(checked(odd_text(kRecordText), kVouched), 0);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data_present,
            std::vector<uint8_t>(odd_text(kRecordText).size(), kVouched));
}

// Where the check passed more than one value, the vouched copies alone weigh
// them: a copy the check failed takes no side, even where it agrees with one.
TEST(NabtsRecordCatalogue, VouchedValuesAreWeighedByVouchedCopiesAlone) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(checked(misread(kRecordText, 5), kVouched), 0);
  catalogue.merge(checked(odd_text(kRecordText), kVouched), 1);
  catalogue.merge(checked(odd_text(kRecordText), kVouched), 2);
  catalogue.merge(checked(misread(kRecordText, 5), kArrived), 3);
  catalogue.merge(checked(misread(kRecordText, 5), kArrived), 4);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, odd_text(kRecordText));
}

// The check vouches for a block as received, not for the record it was filed
// under. A vouched copy of an earlier version of the record agrees with too
// little of what the copies mostly say to outrank them, and the vote is the
// one parity and weight alone would hold.
TEST(NabtsRecordCatalogue, AVouchedCopyOfAnotherVersionDoesNotTakeOver) {
  constexpr char kEarlier[] = "REAGAN BUDGET DRAWS DEM PRAISE";
  constexpr char kLater[] = "REAGAN BUDGET DRAWS GOP ANGER!";
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(checked(odd_text(kEarlier), kVouched), 0);
  catalogue.merge(checked(odd_text(kLater), kArrived), 1);
  catalogue.merge(checked(odd_text(kLater), kArrived), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, odd_text(kLater));
}

// A copy slipped by a misread header votes slid into line, and what the check
// made of each of its bytes goes with it.
TEST(NabtsRecordCatalogue, ASlidCopyKeepsWhatTheCheckMadeOfEachByte) {
  std::vector<uint8_t> slipped = {0x15, 0x02, 0x15, 0x15};
  const auto text = odd_text(kRecordText);
  slipped.insert(slipped.end(), text.begin(), text.end());

  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(checked(misread(kRecordText, 5), kArrived), 0);
  catalogue.merge(checked(misread(kRecordText, 5), kArrived), 1);
  catalogue.merge(checked(slipped, kVouched), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, text);
}

// The threshold detector has no path metric and so cannot express doubt. Its
// copies must weigh as much as a confident measurement rather than as nothing,
// or a run without MLSE would have every copy counting for zero.
TEST(NabtsRecordCatalogue, AnUnmeasuredCopyVotesAtFullWeight) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(measured({odd('X')}, {200}), 0);
  catalogue.merge(damaged({odd('A')}), 1);  // no confidence measured at all

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  // 255 (unmeasured) beats the 200 the other copy was measured at.
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('A')}));
}

// A byte the detector could not decide at all still says more than a copy that
// has no byte at this position.
TEST(NabtsRecordCatalogue, AZeroConfidenceByteStillBeatsAHole) {
  orc::NabtsRecordCatalogue catalogue;
  auto doubtful = measured({odd('A')}, {0});
  catalogue.merge(doubtful, 0);
  catalogue.merge(holed({0x00}, {0}), 1);
  catalogue.merge(holed({0x00}, {0}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('A')}));
}

////////////////////////////////////////////////////////////////////////////////////////////
// What the vote leaves behind
////////////////////////////////////////////////////////////////////////////////////////////

// Parity is recomputable from the data, but which bytes never arrived dies at
// the vote unless the vote says so. The mask is what lets the reconstruction
// tell a byte that arrived from the filler standing in for one that never did.
TEST(NabtsRecordCatalogue, TheVoteKeepsWhatItKnewAboutEachByte) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(holed({odd('A'), 0x00, odd('C')}, {1, 0, 1}), 0);
  catalogue.merge(holed({odd('A'), 0x00, odd('C')}, {1, 0, 1}), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  ASSERT_EQ(records[0].data_present.size(), records[0].data.size());

  // The middle byte no copy ever delivered, so the NUL standing there is a
  // filler and the mask says so.
  EXPECT_NE(records[0].data_present[0], 0);
  EXPECT_EQ(records[0].data_present[1], 0);
  EXPECT_NE(records[0].data_present[2], 0);
}

// A record that arrived whole is not a record with evidence against it: no
// vote was held, nothing is in doubt, and the mask says nothing rather than
// saying something reassuring.
TEST(NabtsRecordCatalogue, AnIntactRecordCarriesNoDamageEvidence) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 10)), 0);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_TRUE(records[0].data_present.empty());
}

////////////////////////////////////////////////////////////////////////////////////////////
// What the entry carries
////////////////////////////////////////////////////////////////////////////////////////////

TEST(NabtsRecordCatalogue, CarriesTheClassificationFlagsWorthListing) {
  orc::NabtsRecordCatalogue catalogue;
  auto flagged = message(0x000, 0x001, 0, bytes(0x40, 10));
  flagged.type = orc::kNabtsRecordTypePriorityPresentation;
  flagged.classification.caption = true;
  flagged.classification.cyclic_marker = true;
  flagged.classification.priority = true;
  flagged.classification.alarm = true;
  flagged.classification.update = true;
  flagged.classification.support_record = true;
  flagged.classification.index = true;
  flagged.classification.more = true;
  catalogue.merge(flagged, 0);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].record_type, orc::kNabtsRecordTypePriorityPresentation);
  EXPECT_TRUE(records[0].caption);
  EXPECT_TRUE(records[0].cyclic_marker);
  EXPECT_TRUE(records[0].priority);
  EXPECT_TRUE(records[0].alarm);
  EXPECT_TRUE(records[0].update);
  EXPECT_TRUE(records[0].support_record);
  EXPECT_TRUE(records[0].index);
  EXPECT_TRUE(records[0].more);
}

TEST(NabtsRecordCatalogue, CarriesTheReservedPurposeOfAnAddress) {
  orc::NabtsRecordCatalogue catalogue;
  auto reserved = message(0xA00, 0x000, 0, bytes(0x40, 10));
  reserved.reserved_purpose = "Start of captioning";
  catalogue.merge(reserved, 0);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].reserved_purpose, "Start of captioning");
}

// §7.2.2's function descriptors are rendered for a reader: the code in the
// standard's own column/row notation, and the arguments as text.
TEST(NabtsRecordCatalogue, RendersApplicationFunctionDescriptorsAsText) {
  orc::NabtsRecordCatalogue catalogue;
  auto application = message(0x000, 0xFFE, 0, {});
  application.type = orc::kNabtsRecordTypeApplication;
  application.functions.push_back(
      orc::NabtsFunctionDescriptor{0x20, {0x41, 0x42, 0x43}});
  application.functions.push_back(orc::NabtsFunctionDescriptor{0x31, {}});
  catalogue.merge(application, 0);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  ASSERT_EQ(records[0].functions.size(), 2u);
  EXPECT_EQ(records[0].functions[0].code, "2/0");
  EXPECT_TRUE(records[0].functions[0].control);
  EXPECT_EQ(records[0].functions[0].arguments, "ABC");
  EXPECT_EQ(records[0].functions[1].code, "3/1");
  EXPECT_FALSE(records[0].functions[1].control);
  EXPECT_TRUE(records[0].functions[1].arguments.empty());
}

// §7.2.2 confines arguments to the printable range 2/0 to 7/15, so a byte
// outside it is damage and is shown as its value rather than as a control
// character in the middle of a listing.
TEST(NabtsRecordCatalogue, ShowsAnUnprintableArgumentAsItsHexadecimalValue) {
  orc::NabtsRecordCatalogue catalogue;
  auto application = message(0x000, 0xFFE, 0, {});
  application.type = orc::kNabtsRecordTypeApplication;
  application.functions.push_back(
      orc::NabtsFunctionDescriptor{0x20, {0x41, 0x07, 0x42, 0x7F}});
  catalogue.merge(application, 0);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  ASSERT_EQ(records[0].functions.size(), 1u);
  EXPECT_EQ(records[0].functions[0].arguments, "A<07>B<7F>");
}

////////////////////////////////////////////////////////////////////////////////////////////
// The cap
////////////////////////////////////////////////////////////////////////////////////////////

// The cap bounds a run by the size of the service rather than the length of the
// recording. Reaching it drops the least recently seen record and says so,
// since a catalogue silently missing records would read as a service that never
// sent them.
TEST(NabtsRecordCatalogue, DropsTheLeastRecentlySeenRecordAtTheCap) {
  orc::NabtsRecordCatalogue catalogue(/*max_records=*/3);

  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 4)), 0);
  catalogue.merge(message(0x000, 0x002, 0, bytes(0x40, 4)), 1);
  catalogue.merge(message(0x000, 0x003, 0, bytes(0x40, 4)), 2);
  EXPECT_EQ(catalogue.size(), 3u);
  EXPECT_FALSE(catalogue.truncated());

  // Touch 001 so 002 becomes the oldest, then overflow.
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 4)), 3);
  catalogue.merge(message(0x000, 0x004, 0, bytes(0x40, 4)), 4);

  EXPECT_EQ(catalogue.size(), 3u);
  EXPECT_TRUE(catalogue.truncated());
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].address_text, "001");
  EXPECT_EQ(records[1].address_text, "003");
  EXPECT_EQ(records[2].address_text, "004");
}

// A damaged tape fills the catalogue with misread identities long before it
// holds a thousand pages, so at the cap an identity no appearance named as
// transmitted is dropped before one that was named, however recently seen.
TEST(NabtsRecordCatalogue, AtTheCapAnIdentityNeverNamedAsTransmittedGoesFirst) {
  orc::NabtsRecordCatalogue catalogue(/*max_records=*/3);
  const auto named = [](uint16_t address) {
    auto out = message(0x000, address, 0, bytes(0x40, 4));
    out.identity_attested = true;
    return out;
  };
  catalogue.merge(named(0x001), 0);
  catalogue.merge(message(0x000, 0x701, 0, bytes(0x40, 4)), 1);
  catalogue.merge(named(0x002), 2);
  catalogue.merge(named(0x003), 3);

  EXPECT_TRUE(catalogue.truncated());
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 3u);
  EXPECT_EQ(records[0].address_text, "001");
  EXPECT_EQ(records[1].address_text, "002");
  EXPECT_EQ(records[2].address_text, "003");
}

TEST(NabtsRecordCatalogue, IsEmptyBeforeAnythingIsMerged) {
  const orc::NabtsRecordCatalogue catalogue;
  EXPECT_EQ(catalogue.size(), 0u);
  EXPECT_FALSE(catalogue.truncated());
  EXPECT_TRUE(catalogue.records().empty());
}

// A cap of zero would be a catalogue that can hold nothing and so truncates for
// ever; one is the floor.
TEST(NabtsRecordCatalogue, HoldsAtLeastOneRecordWhateverTheCapAsksFor) {
  orc::NabtsRecordCatalogue catalogue(/*max_records=*/0);
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 4)), 0);
  EXPECT_EQ(catalogue.size(), 1u);
  EXPECT_FALSE(catalogue.truncated());
}

////////////////////////////////////////////////////////////////////////////////////////////
// The Support Record (§5.2.7.9, §8.7.1.4)
////////////////////////////////////////////////////////////////////////////////////////////

/// NAPLPS defining a macro at 2/0 whose body switches to text and draws 'X':
/// ESC DEF-MACRO, name, SI 'X', ESC END.
std::vector<uint8_t> macro_definition() {
  return {0x1B, 0x40, 0x20, 0x0F, 'X', 0x1B, 0x45};
}

/// NAPLPS invoking the macro at 2/0: designate the macro set into G1
/// (ESC 2/9 7/10), SO, name.
std::vector<uint8_t> macro_invocation() {
  return {0x1B, 0x29, 0x7A, 0x0E, 0x20};
}

// §8.7.1.4: "the Support Record contain[s] one or more macro definitions ...
// invoked directly by other Presentation Records in the same Data Channel",
// and §5.2.7.9 has the receiver execute it before a record whose
// Support-Needed Flag is set. A page invoking a support-record macro must
// therefore draw what the macro defines.
TEST(NabtsRecordCatalogue, ASupportRecordsMacrosReachThePagesThatNeedThem) {
  orc::NabtsRecordCatalogue catalogue;

  auto support = message(0x000, 0xFFF, 0, macro_definition());
  support.classification.support_record = true;
  catalogue.merge(support, 0);

  auto page = message(0x000, 0x100, 0, macro_invocation());
  page.classification.support_needed = true;
  catalogue.merge(page, 1);

  const auto records = presented(catalogue);
  ASSERT_EQ(records.size(), 2u);
  // Map order is ascending by address, so the page comes first.
  const auto& page_record = records[0];
  ASSERT_EQ(page_record.address_text, "100");
  ASSERT_EQ(page_record.page.primitives.size(), 1u)
      << "the support record's macro never reached the page";
  EXPECT_EQ(page_record.page.primitives[0].character, 'X');
  EXPECT_EQ(page_record.page.diagnostics.unresolved_macros, 0u);
}

// §5.2.7.9: "If the Support-Needed Flag is 0 or absent, then the Support
// Record shall not be processed before the Record is processed" — a page that
// does not ask for support is presented without it.
TEST(NabtsRecordCatalogue, APageWithoutTheFlagIsPresentedWithoutSupport) {
  orc::NabtsRecordCatalogue catalogue;

  auto support = message(0x000, 0xFFF, 0, macro_definition());
  support.classification.support_record = true;
  catalogue.merge(support, 0);

  catalogue.merge(message(0x000, 0x100, 0, macro_invocation()), 1);

  const auto records = presented(catalogue);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_TRUE(records[0].page.primitives.empty());
  EXPECT_EQ(records[0].page.diagnostics.unresolved_macros, 1u);
}

// The support record serves pages in its own Data Channel; a page on another
// channel asking for support has none to draw on (§8.7.1.4: "one unique
// Record per Data Channel").
TEST(NabtsRecordCatalogue, SupportDoesNotLeakAcrossDataChannels) {
  orc::NabtsRecordCatalogue catalogue;

  auto support = message(0x000, 0xFFF, 0, macro_definition());
  support.classification.support_record = true;
  catalogue.merge(support, 0);

  auto page = message(0x200, 0x100, 0, macro_invocation());
  page.classification.support_needed = true;
  catalogue.merge(page, 1);

  const auto records = presented(catalogue);
  ASSERT_EQ(records.size(), 2u);
  const auto& page_record =
      records[0].address_text == "100" ? records[0] : records[1];
  EXPECT_TRUE(page_record.page.primitives.empty());
  EXPECT_EQ(page_record.page.diagnostics.unresolved_macros, 1u);
}

////////////////////////////////////////////////////////////////////////////////////////////
// More chains (§5.2.7.6, §5.2.7.8)
////////////////////////////////////////////////////////////////////////////////////////////

/// NAPLPS drawing one character from the primary set: SI, the character.
std::vector<uint8_t> draw_letter(char letter) {
  return {0x0F, static_cast<uint8_t>(letter)};
}

/// The same on a cleared screen: CS (X3.110 §6.1.6.3) first, as a page that
/// stands on its own opens.
std::vector<uint8_t> clear_and_draw_letter(char letter) {
  return {0x0C, 0x0F, static_cast<uint8_t>(letter)};
}

/// A message at the nine-digit long address |address| (§5.2.5).
orc::NabtsMessage long_address_message(uint16_t channel, uint64_t address,
                                       uint8_t version,
                                       std::vector<uint8_t> data) {
  orc::NabtsMessage out = message(channel, 0, version, std::move(data));
  out.address.value = address;
  out.address.long_form = true;
  return out;
}

// §5.2.7.6 links a record whose More Flag is set to the record at its long
// address plus one, and §5.2.7.8 presents that successor "after the completion
// of the presentation of the current Record" — over the standing display. The
// continuation's page must therefore carry what its predecessor drew.
TEST(NabtsRecordCatalogue, AMoreRecordIsPresentedOverItsPredecessorsDisplay) {
  orc::NabtsRecordCatalogue catalogue;

  auto base = message(0x000, 0x044, 0, draw_letter('X'));
  base.classification.more = true;
  catalogue.merge(base, 0);
  catalogue.merge(long_address_message(0x000, 0x4401, 0, draw_letter('Y')), 1);

  const auto records = presented(catalogue);
  ASSERT_EQ(records.size(), 2u);

  const auto& first = records[0];
  EXPECT_EQ(first.chain_base_address, 0x4400u);
  EXPECT_EQ(first.chain_position, 0u);
  ASSERT_EQ(first.page.primitives.size(), 1u);
  EXPECT_EQ(first.page.primitives[0].character, 'X');

  const auto& continuation = records[1];
  EXPECT_EQ(continuation.chain_base_address, 0x4400u);
  EXPECT_EQ(continuation.chain_position, 1u);
  ASSERT_EQ(continuation.page.primitives.size(), 2u)
      << "the base record's drawing did not reach the continuation";
  EXPECT_EQ(continuation.page.primitives[0].character, 'X');
  EXPECT_EQ(continuation.page.primitives[1].character, 'Y');
}

// §5.2.7.6: the chain exists only where the predecessor declares it. A record
// at a continuation-shaped address whose predecessor has no More Flag stands
// alone.
TEST(NabtsRecordCatalogue, ARecordWithoutAMoreFlaggedPredecessorStandsAlone) {
  orc::NabtsRecordCatalogue catalogue;

  catalogue.merge(message(0x000, 0x044, 0, draw_letter('X')), 0);
  catalogue.merge(long_address_message(0x000, 0x4401, 0, draw_letter('Y')), 1);

  const auto records = presented(catalogue);
  ASSERT_EQ(records.size(), 2u);
  const auto& lone = records[1];
  EXPECT_EQ(lone.chain_base_address, 0x4401u);
  EXPECT_EQ(lone.chain_position, 0u);
  ASSERT_EQ(lone.page.primitives.size(), 1u);
  EXPECT_EQ(lone.page.primitives[0].character, 'Y');
}

// §7.3.4 consults a header extension naming the More Record explicitly
// (§5.2.8.4, EI meaning 001) before the More Flag — and a service may use the
// extension alone, as the reference CBS ExtraVision recording does: its
// chains carry no More Flags at all.
TEST(NabtsRecordCatalogue, AMoreHeaderExtensionLinksAChainWithoutTheFlag) {
  orc::NabtsRecordCatalogue catalogue;

  auto base = message(0x000, 0x044, 0, draw_letter('X'));
  // EI meaning 1, nine data nibbles: long address 000004401.
  base.extensions.push_back(
      orc::NabtsHeaderExtension{1, 9, {0, 0, 0, 0, 0, 4, 4, 0, 1}});
  catalogue.merge(base, 0);
  catalogue.merge(long_address_message(0x000, 0x4401, 0, draw_letter('Y')), 1);

  const auto records = presented(catalogue);
  ASSERT_EQ(records.size(), 2u);
  const auto& continuation = records[1];
  EXPECT_EQ(continuation.chain_base_address, 0x4400u);
  EXPECT_EQ(continuation.chain_position, 1u);
  ASSERT_EQ(continuation.page.primitives.size(), 2u)
      << "the extension-linked predecessor's drawing did not carry";
  EXPECT_EQ(continuation.page.primitives[0].character, 'X');
}

// §5.2.8.4 ES 6 names the data channel as well as a short address. Naming the
// record's own channel, it is the More Record, and §7.3.4 fetches it in place
// of the More Flag's long-address-plus-one even where that record exists.
TEST(NabtsRecordCatalogue, ASameChannelMoreExtensionIsTheMoreAddress) {
  orc::NabtsRecordCatalogue catalogue;

  auto base = message(0x000, 0x044, 0, draw_letter('X'));
  base.classification.more = true;
  // More Record: channel 000, short address 050.
  base.extensions.push_back(
      orc::NabtsHeaderExtension{1, 6, {0x0, 0x0, 0x0, 0x0, 0x5, 0x0}});
  catalogue.merge(base, 0);
  catalogue.merge(message(0x000, 0x050, 0, draw_letter('Y')), 1);
  catalogue.merge(long_address_message(0x000, 0x4401, 0, draw_letter('Z')), 2);

  const auto records = presented(catalogue);
  ASSERT_EQ(records.size(), 3u);
  const auto find = [&records](uint64_t address) {
    for (const auto& record : records) {
      if (record.address == address) {
        return record;
      }
    }
    return orc::NabtsCataloguedRecord{};
  };

  const auto head = find(0x4400);
  EXPECT_TRUE(head.more_redefined);
  EXPECT_TRUE(head.has_more_address);
  EXPECT_EQ(head.more_channel, 0x000u);
  EXPECT_EQ(head.more_address, 0x5000u);

  const auto named = find(0x5000);
  EXPECT_EQ(named.chain_base_address, 0x4400u);
  EXPECT_EQ(named.chain_position, 1u);
  ASSERT_EQ(named.page.primitives.size(), 2u)
      << "the named More Record was not presented over its predecessor";
  EXPECT_EQ(named.page.primitives[0].character, 'X');

  const auto algorithmic = find(0x4401);
  EXPECT_EQ(algorithmic.chain_position, 0u)
      << "the More Flag's address was followed despite the extension";
  EXPECT_EQ(algorithmic.page.primitives.size(), 1u);
}

// §7.3.4 reaches the More Flag only where no header extension redefines the
// More Record. An extension naming a record in another data channel, or a
// macro, still does — so the algorithmic successor is not this page's More
// Record, although neither is followed here.
TEST(NabtsRecordCatalogue, AnyMoreExtensionSuppressesTheMoreFlagsAddress) {
  const auto successor_position = [](orc::NabtsHeaderExtension field) {
    orc::NabtsRecordCatalogue catalogue;
    auto base = message(0x000, 0x044, 0, draw_letter('X'));
    base.classification.more = true;
    base.extensions.push_back(std::move(field));
    catalogue.merge(base, 0);
    catalogue.merge(long_address_message(0x000, 0x4401, 0, draw_letter('Y')),
                    1);
    const auto records = presented(catalogue);
    return records.size() == 2 ? records[1].chain_position : 99u;
  };

  // More Record 111 in channel 0A0 (ES 6).
  EXPECT_EQ(successor_position(orc::NabtsHeaderExtension{
                1, 6, {0x0, 0xA, 0x0, 0x1, 0x1, 0x1}}),
            0u);
  // More Record in channel 200 at a long address (ES 12).
  EXPECT_EQ(successor_position(orc::NabtsHeaderExtension{
                1, 12, {0x2, 0x0, 0x0, 0, 0, 0, 0, 0, 4, 4, 0, 1}}),
            0u);
  // Macro 6/3 as the More function (ES 2).
  EXPECT_EQ(successor_position(orc::NabtsHeaderExtension{1, 2, {6, 3}}), 0u);
  // A macro out of range is "as if the Header Extension Field did not exist"
  // (§5.2.8.4), so the More Flag is followed after all.
  EXPECT_EQ(successor_position(orc::NabtsHeaderExtension{1, 2, {1, 3}}), 1u);
  // A Next Record extension redefines nothing about More.
  EXPECT_EQ(
      successor_position(orc::NabtsHeaderExtension{2, 3, {0x0, 0x2, 0x0}}), 1u);
}

// §7.3.4: for the More Flag's algorithmic address, "the last two digits shall
// be regarded as decimal numbers" — 09 steps to 10, not to 0A.
TEST(NabtsRecordCatalogue, TheAlgorithmicMoreAddressCountsInDecimal) {
  orc::NabtsRecordCatalogue catalogue;

  auto ninth = long_address_message(0x000, 0x4409, 0, draw_letter('X'));
  ninth.classification.more = true;
  catalogue.merge(ninth, 0);
  catalogue.merge(long_address_message(0x000, 0x4410, 0, draw_letter('Y')), 1);

  const auto records = presented(catalogue);
  ASSERT_EQ(records.size(), 2u);
  const auto& continuation = records[1];
  EXPECT_EQ(continuation.chain_position, 1u)
      << "09 must step to 10 in decimal, not to 0A in hex";
  ASSERT_EQ(continuation.page.primitives.size(), 2u);
  EXPECT_EQ(continuation.page.primitives[0].character, 'X');
}

// A closed loop of More links — the rotating set of §7.3.4's Random More
// note; a service can carry a four-record ring — comes out as
// one chain, not as one single-member "chain" per member: only a link forward
// continues a page, so the link from the last member back to the first ends
// nothing, and every member takes the smallest address as the base.
TEST(NabtsRecordCatalogue, ARingOfMoreLinksSharesOneCanonicalBase) {
  orc::NabtsRecordCatalogue catalogue;

  auto first =
      long_address_message(0x000, 0x4409, 0, clear_and_draw_letter('X'));
  first.extensions.push_back(
      orc::NabtsHeaderExtension{1, 9, {0, 0, 0, 0, 0, 4, 5, 0, 9}});
  catalogue.merge(first, 0);
  auto second = long_address_message(0x000, 0x4509, 0, draw_letter('Y'));
  second.extensions.push_back(
      orc::NabtsHeaderExtension{1, 9, {0, 0, 0, 0, 0, 4, 4, 0, 9}});
  catalogue.merge(second, 1);

  const auto records = presented(catalogue);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].chain_base_address, 0x4409u);
  EXPECT_EQ(records[0].chain_position, 0u);
  EXPECT_EQ(records[1].chain_base_address, 0x4409u)
      << "each ring member walked to a different stop";
  EXPECT_EQ(records[1].chain_position, 1u);
  // The base is presented alone; the other member over the base's display.
  EXPECT_EQ(records[0].page.primitives.size(), 1u);
  EXPECT_EQ(records[1].page.primitives.size(), 2u);
}

// Stories can name the Master Index (000) as their More Record. MORE
// on such a page returns the reader to the index, which stands on its own and
// is reached by its number too, so it is presented as itself — not over the
// story, and not as a page of the story's chain.
TEST(NabtsRecordCatalogue, AMoreLinkBackToAnEarlierPageContinuesNothing) {
  orc::NabtsRecordCatalogue catalogue;

  catalogue.merge(message(0x000, 0x000, 0, clear_and_draw_letter('I')), 0);
  auto story = message(0x000, 0x021, 0, draw_letter('S'));
  // More Record: channel 000, short address 000.
  story.extensions.push_back(
      orc::NabtsHeaderExtension{1, 6, {0x0, 0x0, 0x0, 0x0, 0x0, 0x0}});
  catalogue.merge(story, 1);

  const auto records = presented(catalogue);
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].address, 0x0000u);
  EXPECT_EQ(records[0].chain_position, 0u);
  ASSERT_EQ(records[0].page.primitives.size(), 1u)
      << "the index was presented over the story that named it";
  EXPECT_EQ(records[0].page.primitives[0].character, 'I');
}

/// NAPLPS writing |rows| one under another: SI, then each row's characters,
/// with APR APD (X3.110 §6.1.2.3, §6.1.2.4) between rows; on a cleared screen
/// when |clear| is set.
std::vector<uint8_t> draw_rows(const std::vector<std::string>& rows,
                               bool clear) {
  std::vector<uint8_t> out;
  if (clear) {
    out.push_back(0x0C);
  }
  out.push_back(0x0F);
  for (size_t i = 0; i < rows.size(); ++i) {
    if (i > 0) {
      out.push_back(0x0D);
      out.push_back(0x0A);
    }
    out.insert(out.end(), rows[i].begin(), rows[i].end());
  }
  return out;
}

const orc::NabtsCataloguedRecord* at_address(
    const std::vector<orc::NabtsCataloguedRecord>& records, uint64_t address) {
  for (const auto& record : records) {
    if (record.address == address) {
      return &record;
    }
  }
  return nullptr;
}

// A quiz: the board (711) clears the screen and offers
// its questions by number; a question (750) starts on the board, marks its
// square and offers its answers by number; an answer (753) starts on the
// question. The Panasonic TU-2000x drew each over the screen the viewer asked
// for it from, so each is presented over the pages that lead to it.
TEST(NabtsRecordCatalogue, APageStartingOnTheScreenIsDrawnOverThePageBefore) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(
      message(0x000, 0x711, 0, draw_rows({"PUZZLE", "150 750 950"}, true)), 0);
  catalogue.merge(
      message(0x000, 0x750, 0, draw_rows({"751 Yes", "753 No"}, false)), 1);
  catalogue.merge(message(0x000, 0x753, 0, draw_letter('R')), 2);

  const auto records = presented(catalogue);
  const auto* board = at_address(records, long_of(0x711));
  const auto* question = at_address(records, long_of(0x750));
  const auto* answer = at_address(records, long_of(0x753));
  ASSERT_NE(board, nullptr);
  ASSERT_NE(question, nullptr);
  ASSERT_NE(answer, nullptr);

  EXPECT_TRUE(board->presented_over.empty());
  EXPECT_EQ(question->presented_over, (std::vector<std::string>{"000/711 v0"}));
  EXPECT_EQ(answer->presented_over,
            (std::vector<std::string>{"000/711 v0", "000/750 v0"}));
  // The answer's page holds the board and the question beneath its own mark.
  ASSERT_GT(answer->page.primitives.size(), question->page.primitives.size());
  ASSERT_GT(question->page.primitives.size(), board->page.primitives.size());
  EXPECT_EQ(answer->page.primitives.front().character, 'P');
  EXPECT_EQ(answer->page.primitives.back().character, 'R');
  // The board's own offers are what the board lists, not what it is under.
  EXPECT_EQ(question->offered_pages,
            (std::vector<uint64_t>{long_of(0x751), long_of(0x753)}));
}

// A page nothing offers by number is reached by NEXT from the page before it
// (the TU-2000x's NEXT takes the next page by number), so it is drawn over
// that page.
TEST(NabtsRecordCatalogue, APageNothingOffersIsDrawnOverThePageBeforeIt) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x711, 0, clear_and_draw_letter('B')), 0);
  catalogue.merge(message(0x000, 0x712, 0, draw_letter('Q')), 1);

  const auto records = presented(catalogue);
  const auto* next = at_address(records, long_of(0x712));
  ASSERT_NE(next, nullptr);
  EXPECT_EQ(next->presented_over, (std::vector<std::string>{"000/711 v0"}));
  ASSERT_EQ(next->page.primitives.size(), 2u);
  EXPECT_EQ(next->page.primitives[0].character, 'B');
}

// A page that clears the screen stands on its own however it was reached; so
// does one sent with two classification pointer bytes, which no surviving
// receiver accepts, so how a receiver changed page under it
// is not known (§8.5.2.7.1).
TEST(NabtsRecordCatalogue, APageThatClearsOrThatNoReceiverShowedStandsAlone) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(
      message(0x000, 0x711, 0, draw_rows({"PUZZLE", "150 750 950"}, true)), 0);
  catalogue.merge(message(0x000, 0x150, 0, clear_and_draw_letter('Q')), 1);
  auto outside = message(0x000, 0x750, 0, draw_letter('N'));
  outside.classification.pointer_bytes = 2;
  catalogue.merge(outside, 2);

  const auto records = presented(catalogue);
  for (const uint16_t address : {0x150, 0x750}) {
    const auto* page = at_address(records, long_of(address));
    ASSERT_NE(page, nullptr);
    EXPECT_TRUE(page->presented_over.empty()) << address;
    EXPECT_EQ(page->page.primitives.size(), 1u) << address;
  }
}

// Rotating highlights: the News card's page (3909) clears and draws the
// frame, and its MORE — a link back — names
// the Finance card (0109), which redraws only the card. A ring of More links
// is presented as one chain from its lowest address, so the Finance card heads
// it; it is drawn over the page whose MORE brings it up.
TEST(NabtsRecordCatalogue, ACardAMoreLinkBackBringsUpIsDrawnOverItsPage) {
  orc::NabtsRecordCatalogue catalogue;
  auto news =
      long_address_message(0x000, 0x3909, 0, clear_and_draw_letter('H'));
  news.extensions.push_back(
      orc::NabtsHeaderExtension{1, 9, {0, 0, 0, 0, 0, 0, 1, 0, 9}});
  catalogue.merge(news, 0);
  auto finance = long_address_message(0x000, 0x0109, 0, draw_letter('F'));
  finance.extensions.push_back(
      orc::NabtsHeaderExtension{1, 9, {0, 0, 0, 0, 0, 3, 9, 0, 9}});
  catalogue.merge(finance, 1);

  const auto records = presented(catalogue);
  const auto* card = at_address(records, 0x0109);
  ASSERT_NE(card, nullptr);
  EXPECT_EQ(card->chain_position, 0u);
  EXPECT_EQ(card->presented_over,
            (std::vector<std::string>{"000/000003909 v0"}));
  ASSERT_EQ(card->page.primitives.size(), 2u);
  EXPECT_EQ(card->page.primitives[0].character, 'H');
  // The News card continues the chain and clears, so what the chain was drawn
  // over is no part of it.
  const auto* frame = at_address(records, 0x3909);
  ASSERT_NE(frame, nullptr);
  EXPECT_EQ(frame->chain_position, 1u);
  EXPECT_TRUE(frame->presented_over.empty());
  ASSERT_EQ(frame->page.primitives.size(), 1u);
  EXPECT_EQ(frame->page.primitives[0].character, 'H');
}

// A page asked for from a More Record was asked for with the More Record on
// screen over the members of its chain before it (§5.2.7.8), so it is drawn
// over all of them.
TEST(NabtsRecordCatalogue, APageAMoreRecordOffersIsDrawnOverItsChain) {
  orc::NabtsRecordCatalogue catalogue;
  auto base = message(0x000, 0x020, 0, clear_and_draw_letter('B'));
  base.classification.more = true;
  catalogue.merge(base, 0);
  catalogue.merge(long_address_message(0x000, 0x2001, 0,
                                       draw_rows({"See", "150 250"}, false)),
                  1);
  catalogue.merge(message(0x000, 0x150, 0, draw_letter('Q')), 2);

  const auto records = presented(catalogue);
  const auto* page = at_address(records, long_of(0x150));
  ASSERT_NE(page, nullptr);
  EXPECT_EQ(page->presented_over,
            (std::vector<std::string>{"000/000002001 v0"}));
  ASSERT_FALSE(page->page.primitives.empty());
  EXPECT_EQ(page->page.primitives.front().character, 'B');
  EXPECT_EQ(page->page.primitives.back().character, 'Q');
}

// §5.2.8.2 meanings 010 and 011 name the records a page's NEXT and INDEX keys
// fetch. Those in the page's own data channel are kept; a macro, or a record
// in another channel, is a key the catalogue cannot follow.
TEST(NabtsRecordCatalogue, KeepsTheNextAndIndexRecordsTheHeaderNames) {
  orc::NabtsRecordCatalogue catalogue;
  auto page = message(0x000, 0x011, 0, draw_letter('W'));
  page.extensions.push_back(orc::NabtsHeaderExtension{2, 3, {0, 1, 3}});
  page.extensions.push_back(orc::NabtsHeaderExtension{3, 3, {0, 1, 0}});
  catalogue.merge(page, 0);
  auto elsewhere = message(0x000, 0x012, 0, draw_letter('X'));
  elsewhere.extensions.push_back(orc::NabtsHeaderExtension{2, 2, {2, 1}});
  elsewhere.extensions.push_back(
      orc::NabtsHeaderExtension{3, 6, {0, 0, 1, 0, 1, 0}});
  catalogue.merge(elsewhere, 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_TRUE(records[0].has_next_address);
  EXPECT_EQ(records[0].next_address, long_of(0x013));
  EXPECT_TRUE(records[0].has_index_address);
  EXPECT_EQ(records[0].index_address, long_of(0x010));
  EXPECT_FALSE(records[1].has_next_address);
  EXPECT_FALSE(records[1].has_index_address);
}

// A chain follows the newest version of each member (§5.2.7.6 addresses the
// record, not a version of it), so a revised predecessor is what the
// continuation is presented over.
TEST(NabtsRecordCatalogue, AChainFollowsTheNewestVersionOfEachMember) {
  orc::NabtsRecordCatalogue catalogue;

  auto old_base = message(0x000, 0x044, 1, draw_letter('A'));
  old_base.classification.more = true;
  catalogue.merge(old_base, 0);
  auto new_base = message(0x000, 0x044, 2, draw_letter('B'));
  new_base.classification.more = true;
  catalogue.merge(new_base, 1);
  catalogue.merge(long_address_message(0x000, 0x4401, 0, draw_letter('Y')), 2);

  const auto records = presented(catalogue);
  ASSERT_EQ(records.size(), 3u);
  const auto& continuation = records[2];
  ASSERT_EQ(continuation.page.primitives.size(), 2u);
  EXPECT_EQ(continuation.page.primitives[0].character, 'B')
      << "the continuation was presented over an outdated predecessor";
}

// An identity can carry more than one content, and a chain is followed through
// the content that continues it. A map overlay left in the carousel from an
// earlier design of the weather pages can still be sent under one identity
// beside a page whose More Flag carries the story on; taken for the chain, the
// overlay left the pages after it drawn without the page they continue.
TEST(NabtsRecordCatalogue, AChainFollowsTheContentOfAnIdentityThatContinuesIt) {
  orc::NabtsRecordCatalogue catalogue;

  auto base = message(0x000, 0x044, 0, draw_letter('X'));
  base.classification.more = true;
  catalogue.merge(base, 0);
  auto second_page =
      long_address_message(0x000, 0x4401, 0, odd_text(kRecordText));
  second_page.classification.more = true;
  catalogue.merge(second_page, 1);
  // Another page under the same identity, continuing into nothing: the latest
  // content at the address.
  catalogue.merge(
      long_address_message(0x000, 0x4401, 0, odd_text(kOtherRecordText)), 2);
  catalogue.merge(long_address_message(0x000, 0x4402, 0, draw_letter('Y')), 3);

  const auto records = presented(catalogue);
  ASSERT_EQ(records.size(), 4u);
  ASSERT_EQ(records[1].content, 0u);
  ASSERT_EQ(records[2].content, 1u);
  const auto& third_page = records[3];
  ASSERT_EQ(third_page.address, 0x4402u);
  EXPECT_EQ(third_page.chain_base_address, 0x4400u);
  EXPECT_EQ(third_page.chain_position, 2u);
  ASSERT_GE(third_page.page.primitives.size(), 3u)
      << "the third page was not presented over the pages it continues";
  EXPECT_EQ(third_page.page.primitives.front().character, 'X');
  EXPECT_EQ(third_page.page.primitives[1].character, kRecordText[0]);
  EXPECT_EQ(third_page.page.primitives.back().character, 'Y');
}

// A version no appearance ever named as transmitted — kept because it recurred
// with content of its own — does not displace one that was at the head of a
// chain. Otherwise a page filed under a misread version of page 004 would
// take its place, and the story page 004 continues would be presented
// alone.
TEST(NabtsRecordCatalogue, AnUnconfirmedVersionDoesNotDisplaceAConfirmedOne) {
  orc::NabtsRecordCatalogue catalogue;

  auto base = message(0x000, 0x044, 0, draw_letter('A'));
  base.classification.more = true;
  base.identity_attested = true;
  catalogue.merge(base, 0);
  catalogue.merge(message(0x000, 0x044, 6, draw_letter('S')), 1);
  auto continuation = long_address_message(0x000, 0x4401, 0, draw_letter('Y'));
  continuation.identity_attested = true;
  catalogue.merge(continuation, 2);

  const auto records = presented(catalogue);
  ASSERT_EQ(records.size(), 3u);
  const auto& page = records[2];
  EXPECT_EQ(page.chain_position, 1u);
  ASSERT_EQ(page.page.primitives.size(), 2u)
      << "the continuation was not presented over the confirmed version";
  EXPECT_EQ(page.page.primitives[0].character, 'A');
  EXPECT_EQ(page.page.primitives[1].character, 'Y');
}

// The same holds for the channel's Support Record: an unconfirmed version with
// the Support Record Flag does not stand in for the confirmed one.
TEST(NabtsRecordCatalogue, AnUnconfirmedSupportRecordDoesNotDisplaceTheRecord) {
  orc::NabtsRecordCatalogue catalogue;

  auto support = message(0x000, 0xFFF, 0, macro_definition());
  support.classification.support_record = true;
  support.identity_attested = true;
  catalogue.merge(support, 0);
  auto misread = message(0x000, 0xFFF, 3, bytes(0x40, 4));
  misread.classification.support_record = true;
  catalogue.merge(misread, 1);

  auto page = message(0x000, 0x100, 0, macro_invocation());
  page.classification.support_needed = true;
  page.identity_attested = true;
  catalogue.merge(page, 2);

  const auto records = presented(catalogue);
  ASSERT_EQ(records.size(), 3u);
  const auto& page_record = records[0];
  ASSERT_EQ(page_record.address_text, "100");
  ASSERT_EQ(page_record.page.primitives.size(), 1u)
      << "the page ran without the confirmed support record's macro";
  EXPECT_EQ(page_record.page.primitives[0].character, 'X');
}

////////////////////////////////////////////////////////////////////////////////////////////
// Identity attestation (issue #267)
////////////////////////////////////////////////////////////////////////////////////////////
//
// Every digit of §5.2.1's identity arrives in its own Hamming 8/4 byte, and
// ETSI EN 300 706 §8.2 gives that code minimum distance 4 — so three bit errors
// resolve silently into a neighbouring codeword. The record that happens to is
// not damaged: it is copied, to a channel, address or version the service never
// transmitted, and only whether the identity arrived as transmitted tells the
// copy from the original.

/// |message()|, with a note of whether the arrival named the record as
/// transmitted.
// Every classification flag is a bit of a Hamming 8/4 byte (§5.2.7), so a
// burst sets one as silently as it moves an address. One copy of a page
// arriving with the caption flag misread must not put the page in the caption
// track, nor one with the support-record flag make it the channel's Support
// Record.
TEST(NabtsRecordCatalogue, AFlagOnlyAMisreadCopyCarriedIsNotTheRecords) {
  orc::NabtsRecordCatalogue catalogue;
  for (int index = 0; index < 3; ++index) {
    auto copy = message(0x000, 0x001, 0, bytes(0x40, 10), true, false);
    copy.identity_attested = true;
    catalogue.merge(copy, static_cast<uint64_t>(index));
  }
  auto misread = message(0x000, 0x001, 0, bytes(0x40, 12), true, false);
  misread.classification.caption = true;
  misread.classification.support_record = true;
  catalogue.merge(misread, 3);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_FALSE(records[0].caption);
  EXPECT_FALSE(records[0].support_record);
}

// Appearances that named the record as transmitted are the ones whose flags are
// trusted, so they decide even when they are outnumbered. (The Caption Flag is
// not the one to show it with: an attested arrival carrying it belongs to the
// caption service, which is kept apart — see nabts_caption_stream_test.cpp.)
TEST(NabtsRecordCatalogue, TheAttestedAppearancesDecideTheFlags) {
  orc::NabtsRecordCatalogue catalogue;
  for (int index = 0; index < 2; ++index) {
    auto copy = message(0x000, 0x001, 0, bytes(0x40, 10), true, false);
    copy.identity_attested = true;
    copy.classification.index = true;
    catalogue.merge(copy, static_cast<uint64_t>(index));
  }
  for (int index = 2; index < 6; ++index) {
    auto copy = message(0x000, 0x001, 0, bytes(0x40, 10), true, false);
    catalogue.merge(copy, static_cast<uint64_t>(index));
  }

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_TRUE(records[0].index);
}

// A recording that never named the record as transmitted still has a majority
// to go by.
TEST(NabtsRecordCatalogue, WithNoAttestedAppearanceTheFlagsFollowMostOfThem) {
  orc::NabtsRecordCatalogue catalogue;
  for (int index = 0; index < 3; ++index) {
    auto copy = message(0x000, 0x001, 0, bytes(0x40, 10), true, false);
    copy.classification.caption = index != 0;
    copy.classification.more = index == 0;
    catalogue.merge(copy, static_cast<uint64_t>(index));
  }

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_TRUE(records[0].caption);
  EXPECT_FALSE(records[0].more);
}

// How many pointer bytes the classification sequence had (§5.2.7.1) and which
// reserved flag bytes it set (§5.2.7.2) are Hamming 8/4 like the flags, and
// are read the same way: as most of the attested appearances carried them.
TEST(NabtsRecordCatalogue, TheClassificationFormIsVotedLikeTheFlags) {
  orc::NabtsRecordCatalogue catalogue;
  const auto copy = [](uint8_t pointers, uint8_t y11, bool attested) {
    auto out = message(0x000, 0x001, 0, bytes(0x40, 10), true, false);
    out.identity_attested = attested;
    out.classification.present = true;
    out.classification.pointer_bytes = pointers;
    if (y11 != 0) {
      out.classification.reserved_flags = {orc::NabtsReservedFlag{1, 1, y11}};
    }
    return out;
  };
  // The two-byte form, Y01 = F with Y02 following and Y11 = 4, named as
  // transmitted three times; one attested copy read Y11 as 6, and damaged
  // copies read the sequence as one pointer byte.
  for (uint64_t frame = 0; frame < 3; ++frame) {
    catalogue.merge(copy(2, 0x4, true), frame);
  }
  catalogue.merge(copy(2, 0x6, true), 3);
  for (uint64_t frame = 4; frame < 10; ++frame) {
    catalogue.merge(copy(1, 0x0, false), frame);
  }

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].classification_pointers, 2u);
  EXPECT_EQ(records[0].reserved_flags, std::vector<std::string>{"Y11 = 4"});
}

// The record type is a Hamming 8/4 byte of the header like the flags (§5.2.2),
// and a type §5.2.2 reserves is one no service sent: a copy carrying one had
// its header misread, names its identity as transmitted no more than a
// corrected copy does, and is outvoted by the copies that read the type right.
TEST(NabtsRecordCatalogue, AReservedRecordTypeIsAMisreadAndIsVotedOut) {
  orc::NabtsRecordCatalogue catalogue;
  // The first copy, and the longest, so the one the record keeps.
  auto misread_type = message(0x000, 0x060, 1, page(8, 50), true, false);
  misread_type.type = 7;
  misread_type.identity_attested = true;
  catalogue.merge(misread_type, 0);
  for (uint64_t frame = 1; frame <= 3; ++frame) {
    auto copy = message(0x000, 0x060, 1, page(8, 48), true, false);
    copy.identity_attested = true;
    catalogue.merge(copy, frame);
  }

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].record_type, orc::kNabtsRecordTypeCyclicPresentation);
  EXPECT_EQ(records[0].times_attested, 3u);
  EXPECT_EQ(records[0].times_seen, 4u);
}

orc::NabtsMessage attested_message(uint16_t channel, uint16_t short_address,
                                   uint8_t version, std::vector<uint8_t> data,
                                   bool attested) {
  auto out = message(channel, short_address, version, std::move(data));
  out.identity_attested = attested;
  return out;
}

/// |appearances| copies of one record, none of which arrived intact, so the
/// catalogue holds them for the vote.
void carousel(orc::NabtsRecordCatalogue& catalogue, uint16_t channel,
              uint16_t short_address, uint8_t version,
              std::vector<uint8_t> data, int appearances, bool attested,
              uint64_t first_frame = 0) {
  for (int index = 0; index < appearances; ++index) {
    auto copy = message(channel, short_address, version, data,
                        /*complete=*/true, /*intact=*/false);
    copy.identity_attested = attested;
    catalogue.merge(copy, first_frame + static_cast<uint64_t>(index));
  }
}

TEST(NabtsRecordCatalogueIdentities, AttestedAppearancesAreCounted) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(attested_message(0x000, 0x009, 7, bytes(0x40, 8), true), 0);
  catalogue.merge(attested_message(0x000, 0x009, 7, bytes(0x40, 8), true), 1);
  catalogue.merge(attested_message(0x000, 0x009, 7, bytes(0x40, 8), false), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].times_seen, 3u);
  EXPECT_EQ(records[0].times_attested, 2u);
}

// The shape of the reported bug: 000/009 v7 brought round throughout the
// recording, and 000/709 v7 — the same page, one address digit away — never
// once received as transmitted.
TEST(NabtsRecordCatalogueIdentities, FoldUnattestedRecordIntoItsOneNeighbour) {
  orc::NabtsRecordCatalogue catalogue;
  carousel(catalogue, 0x000, 0x009, 7, bytes(0x40, 8), /*appearances=*/40,
           /*attested=*/true);
  carousel(catalogue, 0x000, 0x709, 7, bytes(0x40, 8), /*appearances=*/2,
           /*attested=*/false, /*first_frame=*/900);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_EQ(reconciliation.identities_seen, 2u);
  EXPECT_EQ(reconciliation.identities_unattested, 1u);
  EXPECT_EQ(reconciliation.identities_folded, 1u);
  EXPECT_EQ(reconciliation.appearances_folded, 2u);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].address, long_of(0x009));
  EXPECT_EQ(records[0].times_seen, 42u);
  EXPECT_EQ(records[0].last_seen_frame, 901u);
}

// §5.2.7.2 has a revision replace the version before it, so a version seen a
// couple of times while another version of its address went on being sent all
// round it, carrying that version's bytes, is that version misread — even
// where its header arrived all codewords and so names it as transmitted: a
// page seen hundreds of times was also listed under another version from two
// damaged sightings in the middle of a tape.
TEST(NabtsRecordCatalogueIdentities, FoldAVersionSeenOnlyInsideAnothersSpan) {
  orc::NabtsRecordCatalogue catalogue;
  const auto scores = page(14, 200);
  carousel(catalogue, 0x000, 0x031, 0x2, scores, /*appearances=*/40,
           /*attested=*/true);
  carousel(catalogue, 0x000, 0x031, 0xE, misread_span(scores, 60, 64),
           /*appearances=*/2, /*attested=*/true, /*first_frame=*/10);
  ASSERT_EQ(catalogue.records().size(), 2u);

  catalogue.reconcile_identities();
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].version, 0x2);
  EXPECT_EQ(records[0].times_seen, 42u);
  EXPECT_EQ(records[0].data, scores);
}

// A revision is seen after the version it replaced rather than inside its span,
// and a carousel's versions are seen about as often as one another: neither is
// a misreading.
TEST(NabtsRecordCatalogueIdentities, KeepAVersionSeenAfterAnotherOrAsOften) {
  orc::NabtsRecordCatalogue catalogue;
  carousel(catalogue, 0x000, 0x031, 0x2, page(15, 200), 40, true);
  carousel(catalogue, 0x000, 0x031, 0x3, page(15, 200), 2, true,
           /*first_frame=*/40);
  carousel(catalogue, 0x000, 0x032, 0x2, page(16, 200), 40, true);
  carousel(catalogue, 0x000, 0x032, 0x5, page(16, 200), 10, true,
           /*first_frame=*/5);

  catalogue.reconcile_identities();
  EXPECT_EQ(catalogue.records().size(), 4u);
}

// A misread channel digit is the same failure one level up: it opens a data
// group on a channel the service never used.
TEST(NabtsRecordCatalogueIdentities, FoldAMisreadChannelDigit) {
  orc::NabtsRecordCatalogue catalogue;
  carousel(catalogue, 0x000, 0x021, 6, bytes(0x40, 8), /*appearances=*/30,
           /*attested=*/true);
  carousel(catalogue, 0x700, 0x021, 6, bytes(0x40, 8), /*appearances=*/3,
           /*attested=*/false, /*first_frame=*/900);

  EXPECT_EQ(catalogue.reconcile_identities().identities_folded, 1u);
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].channel, 0x000);
  EXPECT_EQ(records[0].times_seen, 33u);
}

// Folding rather than discarding is the point: the misread copies are bytes of
// the record that were received, so they join its vote.
TEST(NabtsRecordCatalogueIdentities, FoldedCopiesJoinTheVote) {
  orc::NabtsRecordCatalogue catalogue;
  // Two copies of the attested record, each damaged in a different byte, and
  // three of the misread one carrying the byte both of them lost.
  carousel(catalogue, 0x000, 0x009, 7, {0x41, 0x99, 0x43}, /*appearances=*/2,
           /*attested=*/true);
  carousel(catalogue, 0x000, 0x709, 7, {0x41, 0x42, 0x43}, /*appearances=*/3,
           /*attested=*/false, /*first_frame=*/900);

  ASSERT_EQ(catalogue.reconcile_identities().identities_folded, 1u);
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].copies_voted, 5u);
  ASSERT_EQ(records[0].data.size(), 3u);
  EXPECT_EQ(records[0].data[1], 0x42)
      << "the misread copies outvoted the byte both attested copies had wrong";
}

// A service numbering its records densely offers two addresses a misreading
// could have come from; the content says which. (The pages are long enough to
// be compared now: their votes must share kNabtsIdentityMinJudged positions
// received with good parity, where eight bytes of 0x40 upward used to do.)
TEST(NabtsRecordCatalogueIdentities,
     AMisreadingFoldsIntoTheNeighbourItMatches) {
  orc::NabtsRecordCatalogue catalogue;
  carousel(catalogue, 0x000, 0x003, 6, page(10, 100), /*appearances=*/20,
           /*attested=*/true);
  carousel(catalogue, 0x000, 0x007, 6, page(11, 100), /*appearances=*/20,
           /*attested=*/true, /*first_frame=*/100);
  // 000/005 is one digit from both, and carries 000/007's page.
  carousel(catalogue, 0x000, 0x005, 6, page(11, 100), /*appearances=*/2,
           /*attested=*/false, /*first_frame=*/900);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_EQ(reconciliation.identities_folded, 1u);
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[1].address, long_of(0x007));
  EXPECT_EQ(records[1].times_seen, 22u);
}

// One sighting matching neither neighbour is too little to go on.
TEST(NabtsRecordCatalogueIdentities, DropASingleSightingBetweenTwoNeighbours) {
  orc::NabtsRecordCatalogue catalogue;
  carousel(catalogue, 0x000, 0x003, 6, bytes(0x40, 8), /*appearances=*/20,
           /*attested=*/true);
  carousel(catalogue, 0x000, 0x007, 6, bytes(0x50, 8), /*appearances=*/20,
           /*attested=*/true, /*first_frame=*/100);
  carousel(catalogue, 0x000, 0x005, 6, bytes(0x60, 8), /*appearances=*/1,
           /*attested=*/false, /*first_frame=*/900);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_EQ(reconciliation.identities_folded, 0u);
  EXPECT_EQ(reconciliation.identities_dropped, 1u);
  EXPECT_EQ(catalogue.records().size(), 2u);
}

// The NBC tapes: a record whose address is corrupted the same way on every
// appearance recurs under that one address with a page nothing else carries.
// (A page of its own now has to hold kNabtsIdentityMinJudged bytes that pass
// their parity, and copies that agree with one another; eight bytes of 0x55
// upward do neither, and would fold into 000/050 a digit away.)
TEST(NabtsRecordCatalogueIdentities, KeepARecurringRecordOfItsOwn) {
  orc::NabtsRecordCatalogue catalogue;
  carousel(catalogue, 0x000, 0x050, 0, page(12, 100), /*appearances=*/20,
           /*attested=*/true);
  carousel(catalogue, 0x000, 0x060, 0, page(13, 100), /*appearances=*/5,
           /*attested=*/false, /*first_frame=*/900);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_EQ(reconciliation.identities_kept, 1u);
  EXPECT_EQ(reconciliation.appearances_kept, 5u);
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[1].address, long_of(0x060));
  EXPECT_EQ(records[1].times_attested, 0u);
  EXPECT_EQ(records[1].times_seen, 5u);
}

/// |appearances| damaged copies of |data| under one identity, each with the
/// bytes from |lost_from| up to |lost_to| lost — a hole held open — and of
/// record type |type|.
void holed_carousel(orc::NabtsRecordCatalogue& catalogue, uint16_t channel,
                    uint16_t short_address, uint8_t version,
                    std::vector<uint8_t> data, size_t lost_from, size_t lost_to,
                    int appearances, bool attested, uint64_t first_frame,
                    uint8_t type = orc::kNabtsRecordTypeCyclicPresentation) {
  std::vector<uint8_t> present(data.size(), 1);
  for (size_t i = lost_from; i < lost_to && i < data.size(); ++i) {
    data[i] = 0x00;
    present[i] = 0;
  }
  for (int index = 0; index < appearances; ++index) {
    auto copy = message(channel, short_address, version, data,
                        /*complete=*/true, /*intact=*/false);
    copy.present = present;
    copy.identity_attested = attested;
    copy.type = type;
    catalogue.merge(copy, first_frame + static_cast<uint64_t>(index));
  }
}

// A damaged tape can read digit 0 as 7 in every Hamming byte, so copies of
// page 060 v1 arrived as 000/760 v1, some typed 7 as well, never once named as
// transmitted. Their copies and the page's lost different stretches:
// compared byte for byte over the longer copy, holes and all, they matched
// nothing and were kept as a page of their own. Over the positions both votes
// received they are one page, and fold into it.
TEST(NabtsRecordCatalogueIdentities,
     AMisreadIdentityFoldsIntoItsPageByTheVote) {
  const auto text = page(14, 160);
  orc::NabtsRecordCatalogue catalogue;
  holed_carousel(catalogue, 0x000, 0x060, 1, text, 0, 40, /*appearances=*/3,
                 /*attested=*/true, /*first_frame=*/0);
  holed_carousel(catalogue, 0x000, 0x760, 1, text, 100, 160,
                 /*appearances=*/2, /*attested=*/false, /*first_frame=*/900,
                 /*type=*/7);
  holed_carousel(catalogue, 0x000, 0x760, 1, text, 100, 160,
                 /*appearances=*/1, /*attested=*/false, /*first_frame=*/950);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_EQ(reconciliation.identities_folded, 1u);
  EXPECT_EQ(reconciliation.identities_kept, 0u);
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].address, long_of(0x060));
  EXPECT_EQ(records[0].times_seen, 6u);
  EXPECT_EQ(records[0].record_type, orc::kNabtsRecordTypeCyclicPresentation);
  // Between them the two sets of copies received every byte.
  EXPECT_EQ(records[0].data, text);
}

// Misread copies of several pages under one name agree with the vote they make
// up between them, each in its own stretch, and with one another nowhere. That
// is not a page anyone was sent, and it is not kept as one.
TEST(NabtsRecordCatalogueIdentities, ABlendOfSeveralPagesIsNotKept) {
  orc::NabtsRecordCatalogue catalogue;
  carousel(catalogue, 0x000, 0x050, 0, page(15, 150), /*appearances=*/10,
           /*attested=*/true);
  // 7E0/017: two digits from anything named, so no neighbour to fold into.
  // Three copies of three pages, each received only in its own third.
  for (int third = 0; third < 3; ++third) {
    const size_t from = static_cast<size_t>(third) * 50;
    auto copy = message(0x7E0, 0x017, 0, page(16 + third, 150),
                        /*complete=*/true, /*intact=*/false);
    copy.present.assign(150, 0);
    for (size_t i = 0; i < 150; ++i) {
      if (i >= from && i < from + 50) {
        copy.present[i] = 1;
      } else {
        copy.data[i] = 0x00;
      }
    }
    catalogue.merge(copy, 900 + static_cast<uint64_t>(third));
  }
  ASSERT_EQ(catalogue.records().size(), 2u)
      << "copies with no byte in common are not told apart as they arrive";

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_EQ(reconciliation.identities_kept, 0u);
  EXPECT_EQ(reconciliation.identities_dropped, 1u);
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].address, long_of(0x050));
}

// A record whose copies read a type §5.2.2 reserves, and never named as
// transmitted, is not kept as a page of its own: both decoders of the day
// refuse the type, and no service sent it.
TEST(NabtsRecordCatalogueIdentities, AReservedTypeIsNeverKeptAsAPage) {
  orc::NabtsRecordCatalogue catalogue;
  carousel(catalogue, 0x000, 0x050, 0, page(19, 100), /*appearances=*/10,
           /*attested=*/true);
  holed_carousel(catalogue, 0x7E0, 0x700, 0, page(20, 100), 0, 0,
                 /*appearances=*/3, /*attested=*/false, /*first_frame=*/900,
                 /*type=*/7);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_EQ(reconciliation.identities_kept, 0u);
  EXPECT_EQ(reconciliation.identities_dropped, 1u);
  EXPECT_EQ(catalogue.records().size(), 1u);
}

// A false lock on noise names a channel and address nothing else is near.
TEST(NabtsRecordCatalogueIdentities, DropAMisreadingWithNoNeighbour) {
  orc::NabtsRecordCatalogue catalogue;
  carousel(catalogue, 0x000, 0x009, 7, bytes(0x40, 8), /*appearances=*/20,
           /*attested=*/true);
  carousel(catalogue, 0xFFF, 0xEEE, 3, bytes(0x50, 8), /*appearances=*/1,
           /*attested=*/false, /*first_frame=*/900);

  EXPECT_EQ(catalogue.reconcile_identities().identities_dropped, 1u);
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].channel, 0x000);
}

// On an undamaged recording every identity arrives as transmitted, so the pass
// has nothing to do and must not disturb the catalogue.
TEST(NabtsRecordCatalogueIdentities, LeaveAnUndamagedCatalogueAlone) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(attested_message(0x000, 0x003, 6, bytes(0x40, 8), true), 0);
  catalogue.merge(attested_message(0x000, 0x007, 6, bytes(0x50, 8), true), 1);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_FALSE(reconciliation.acted());
  EXPECT_FALSE(reconciliation.withheld);
  EXPECT_TRUE(reconciliation.summary("record").empty());
  EXPECT_EQ(catalogue.records().size(), 2u);
}

// A recording so damaged that nothing at all was received as transmitted
// offers no baseline to judge the rest against.
TEST(NabtsRecordCatalogueIdentities, WithholdWhenNothingWasAttested) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(attested_message(0x000, 0x003, 6, bytes(0x40, 8), false), 0);
  catalogue.merge(attested_message(0x000, 0x007, 6, bytes(0x50, 8), false), 1);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_TRUE(reconciliation.withheld);
  EXPECT_FALSE(reconciliation.acted());
  EXPECT_EQ(catalogue.records().size(), 2u);
}

////////////////////////////////////////////////////////////////////////////////
// Cataloguing a packet stream the sink wrote
////////////////////////////////////////////////////////////////////////////////

/// |copies| cyclic transmissions of one presentation record: a data group per
/// copy, split over packets with no suffix (28-byte data blocks), the packets
/// of the channel counting their continuity index on from copy to copy. The
/// packet at |drop| of each copy (counted from its synchronizing packet) is
/// left out, as a recording that never received it would leave it.
std::vector<uint8_t> record_stream(const std::vector<uint8_t>& data, int copies,
                                   int drop = -1) {
  // §5.2.1: RT 1 (non-cyclic presentation), a designator announcing nothing
  // further, and a short address.
  std::vector<uint8_t> record{
      nabts::hamming(1),
      nabts::hamming(nabts::record_designator(false, false, false, false))};
  const std::vector<uint8_t> address = nabts::make_short_address(0x1A4);
  record.insert(record.end(), address.begin(), address.end());
  record.insert(record.end(), data.begin(), data.end());

  const size_t block = orc::kNabtsMaxDataBlockBytes;
  const size_t total = orc::kNabtsGroupHeaderBytes + record.size();
  const size_t packets = (total + block - 1) / block;
  const uint16_t final_bytes =
      static_cast<uint16_t>(total - (packets - 1) * block);
  std::vector<uint8_t> group = nabts::make_group_header(
      0, static_cast<uint16_t>(packets - 1), final_bytes);
  group.insert(group.end(), record.begin(), record.end());

  std::vector<uint8_t> stream;
  uint8_t continuity = 0;
  for (int copy = 0; copy < copies; ++copy) {
    for (size_t p = 0; p < packets; ++p) {
      const size_t from = p * block;
      const std::vector<uint8_t> chunk(
          group.begin() + static_cast<std::ptrdiff_t>(from),
          group.begin() + static_cast<std::ptrdiff_t>(
                              std::min(group.size(), from + block)));
      const std::vector<uint8_t> packet = nabts::make_packet(
          0x000, continuity, /*synchronizing=*/p == 0,
          orc::NabtsSuffixKind::kNone, chunk, /*not_full=*/p + 1 == packets);
      continuity = static_cast<uint8_t>((continuity + 1) % 16);
      if (static_cast<int>(p) == drop) {
        continue;
      }
      stream.insert(stream.end(), packet.begin(), packet.end());
    }
  }
  return stream;
}

std::vector<uint8_t> numbered_data(size_t count) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < count; ++i) {
    out.push_back(nabts::parity(static_cast<uint8_t>(0x40 + (i % 64))));
  }
  return out;
}

// A reference recording is known only by the packet stream a run wrote, and is
// read back the way the run read the air.
TEST(NabtsCatalogueStream, CataloguesAStreamAsTheSinkWould) {
  const std::vector<uint8_t> data = numbered_data(90);
  const std::vector<orc::NabtsCataloguedRecord> records =
      orc::nabts_catalogue_stream(record_stream(data, 3));

  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records.front().address_text, "1A4");
  EXPECT_TRUE(orc::nabts_type_is_presentation(records.front().record_type));
  EXPECT_EQ(records.front().times_seen, 3u);
  EXPECT_EQ(records.front().data, data);
}

// Every copy lost the same packet, so the vote has nothing for its bytes — and
// the record says which they are, which is what a reconstruction needs.
TEST(NabtsCatalogueStream, SaysWhichBytesNoCopyDelivered) {
  const std::vector<uint8_t> data = numbered_data(90);
  const std::vector<orc::NabtsCataloguedRecord> records =
      orc::nabts_catalogue_stream(record_stream(data, 3, /*drop=*/2));

  ASSERT_EQ(records.size(), 1u);
  const orc::NabtsCataloguedRecord& record = records.front();
  ASSERT_EQ(record.data_present.size(), record.data.size());
  // The group's data starts after its eight header bytes and the record's
  // five; the third packet's block is bytes 56 to 83 of the group.
  const size_t lost_from =
      2 * orc::kNabtsMaxDataBlockBytes - orc::kNabtsGroupHeaderBytes - 5;
  for (size_t i = 0; i < record.data.size(); ++i) {
    const bool lost =
        i >= lost_from && i < lost_from + orc::kNabtsMaxDataBlockBytes;
    EXPECT_EQ(record.data_present[i] == 0, lost) << i;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////
// Two pages on one layout
////////////////////////////////////////////////////////////////////////////////////////////

/// The layout a carousel builds its pages on: what they share.
const std::vector<uint8_t>& ratings_layout() {
  static const std::vector<uint8_t> layout = page(40, 360);
  return layout;
}

/// A page on ratings_layout(): the layout's bytes everywhere but three
/// twenty-byte fields, which hold |text|.
std::vector<uint8_t> on_layout(const char* text) {
  auto out = ratings_layout();
  const std::string fields(text);
  size_t next = 0;
  for (const size_t field : {60u, 160u, 260u}) {
    for (size_t i = 0; i < 20; ++i) {
      out[field + i] =
          odd(static_cast<uint8_t>(fields[next++ % fields.size()]));
    }
  }
  return out;
}

constexpr char kNineOClock[] =
    "CBS 17.4 28 KNOTS LANDING NBC 19.1 30 L.A. LAW ";
constexpr char kEightOClock[] = "ABC 6.3 9 OUR WORLD CBS 7.8 12 SHELL GAME ";

// Two pages built on one layout agree on every byte of it — here five in six —
// as copies of one page do, and voted together they come out a per-byte blend
// of both.
// Each carries text the other does not, in stretches both arrived with
// cleanly, and that is what keeps them two contents.
TEST(NabtsRecordCatalogue, TwoPagesOfOneLayoutAreTwoContents) {
  const auto nine = on_layout(kNineOClock);
  const auto eight = on_layout(kEightOClock);
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(nine), 0);
  catalogue.merge(damaged(eight), 1);
  catalogue.merge(damaged(nine), 2);
  catalogue.merge(damaged(eight), 3);
  catalogue.reconcile_identities();

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].data, nine);
  EXPECT_EQ(records[0].times_seen, 2u);
  EXPECT_EQ(records[1].data, eight);
  EXPECT_EQ(records[1].times_seen, 2u);
}

// Damage does not look like another page: a garbled stretch fails parity in
// every other byte, and two misread bits leave single bytes wrong. A copy
// damaged both ways is still a copy of its page, and votes with it.
TEST(NabtsRecordCatalogue, AGarbledCopyOfAPageOnALayoutIsStillThatPage) {
  const auto nine = on_layout(kNineOClock);
  auto garbled = nine;
  for (size_t i = 60; i < 100; ++i) {
    // One bit off (failing parity) and two bits off (passing it) in turn.
    garbled[i] = static_cast<uint8_t>(garbled[i] ^ (i % 2 == 0 ? 0x01 : 0x03));
  }
  for (const size_t i : {20u, 200u, 300u, 340u}) {
    garbled[i] = static_cast<uint8_t>(garbled[i] ^ 0x03);
  }
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(nine), 0);
  catalogue.merge(damaged(garbled), 1);
  catalogue.merge(damaged(nine), 2);
  catalogue.reconcile_identities();

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, nine);
  EXPECT_EQ(records[0].times_seen, 3u);
}

// A copy can carry stretches of its own page moved by different amounts: here
// thirty bytes read seven late and forty-five read thirteen early. Every byte
// of such a stretch is found again in the page (kNabtsDisplacementReach), and
// the search for where it came from tries the offset that explained the byte
// before it first and every offset when that fails, so a copy moved two ways
// is still a copy of its page.
TEST(NabtsRecordCatalogue, ACopyWithStretchesMovedTwoWaysIsStillThatPage) {
  const auto whole = page(12, 600);
  auto moved = whole;
  for (size_t i = 0; i < 30; ++i) {
    moved[100 + i] = whole[107 + i];
  }
  for (size_t i = 0; i < 45; ++i) {
    moved[300 + i] = whole[287 + i];
  }
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(whole), 0);
  catalogue.merge(damaged(moved), 1);
  catalogue.merge(damaged(whole), 2);
  catalogue.reconcile_identities();

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].times_seen, 3u);
  EXPECT_EQ(records[0].data, whole);
}

// Bytes the suffix check vouched for in both copies are no damage where they
// differ (CEA-516 §3.4), short of the two misread bits down one column the
// check passes, which leave a few bytes in a hundred differing. One single
// byte in ten differing that way is two pages; the same bytes differing where
// the check vouched for neither may be damage, and the copies are one page.
TEST(NabtsRecordCatalogue, BytesTheCheckVouchedForThatDifferAreTwoPages) {
  const auto first = ratings_layout();
  auto second = first;
  for (size_t i = 5; i < second.size(); i += 10) {
    second[i] = static_cast<uint8_t>(second[i] ^ 0x03);
  }

  orc::NabtsRecordCatalogue vouched;
  vouched.merge(checked(first, kVouched), 0);
  vouched.merge(checked(second, kVouched), 1);
  vouched.merge(checked(first, kVouched), 2);
  vouched.merge(checked(second, kVouched), 3);
  vouched.reconcile_identities();
  EXPECT_EQ(vouched.records().size(), 2u);

  orc::NabtsRecordCatalogue arrived;
  arrived.merge(checked(first, kArrived), 0);
  arrived.merge(checked(second, kArrived), 1);
  arrived.merge(checked(first, kArrived), 2);
  arrived.merge(checked(second, kArrived), 3);
  arrived.reconcile_identities();
  EXPECT_EQ(arrived.records().size(), 1u);
}

/// |copy| with the bytes from |from| to |to| (half-open ranges) marked as the
/// detector's wide reading changed them (kNabtsByteReread), and the rest as
/// bytes that arrived.
orc::NabtsMessage reread_in(
    std::vector<uint8_t> copy,
    std::initializer_list<std::pair<size_t, size_t>> ranges) {
  std::vector<uint8_t> levels(copy.size(), orc::kNabtsByteArrived);
  for (const auto& [from, to] : ranges) {
    std::fill(levels.begin() + static_cast<std::ptrdiff_t>(from),
              levels.begin() + static_cast<std::ptrdiff_t>(to),
              orc::kNabtsByteReread);
  }
  return holed(std::move(copy), std::move(levels));
}

// The detector's wide reading makes a stretch the tape damaged pass its
// parity, and where it reads the stretch wrongly, a copy of a page differs
// from the page there in bytes that pass parity — the evidence that tells two
// pages on one layout apart. Those bytes passed because they were read to
// (kNabtsByteReread), so they are no evidence either way: the copy is the
// page's and votes with it. Arrived passing their parity, the same bytes
// would be another page's.
TEST(NabtsRecordCatalogue, BytesTheWideReadingMadePassAreNoEvidenceOfAPage) {
  const auto nine = on_layout(kNineOClock);
  const auto eight = on_layout(kEightOClock);
  auto misread = nine;
  for (const size_t field : {160u, 260u}) {
    std::copy(eight.begin() + static_cast<std::ptrdiff_t>(field),
              eight.begin() + static_cast<std::ptrdiff_t>(field + 20),
              misread.begin() + static_cast<std::ptrdiff_t>(field));
  }

  orc::NabtsRecordCatalogue reread;
  reread.merge(damaged(nine), 0);
  reread.merge(reread_in(misread, {{160, 180}, {260, 280}}), 1);
  reread.merge(damaged(nine), 2);
  reread.reconcile_identities();
  const auto records = reread.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].times_seen, 3u);
  EXPECT_EQ(records[0].data, nine);

  orc::NabtsRecordCatalogue arrived;
  arrived.merge(damaged(nine), 0);
  arrived.merge(damaged(misread), 1);
  arrived.merge(damaged(nine), 2);
  arrived.reconcile_identities();
  EXPECT_EQ(arrived.records().size(), 2u);
}

// A byte the wide reading changed votes with the bytes that pass their parity
// but is never vouched for, though the rest of its block's check passed: the
// check passes because the reading was taken for passing it. Here the copies
// the check failed outvote it.
TEST(NabtsRecordCatalogue, AByteTheWideReadingChangedIsNeverVouchedFor) {
  const auto text = odd_text(kRecordText);
  auto wide = text;
  wide[5] = static_cast<uint8_t>(wide[5] ^ 0x03);
  std::vector<uint8_t> levels(text.size(), kVouched);
  levels[5] = orc::kNabtsByteReread;
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(holed(wide, levels), 0);
  catalogue.merge(checked(text, kArrived), 1);
  catalogue.merge(checked(text, kArrived), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, text);
  EXPECT_EQ(records[0].data_present[5], kArrived);
  EXPECT_EQ(records[0].data_present[4], kVouched);
}

// Over a byte every other copy holds failing its parity, the wide reading's
// byte wins as any parity-clean byte would — the errors it mends are the ones
// every copy shares — and the record's mask says the value is the wide
// reading's alone (kNabtsByteReread), for the comparisons made of the record
// afterwards to read it as they read the copy.
TEST(NabtsRecordCatalogue, AValueOnlyTheWideReadingReadIsMarkedSoInTheVote) {
  const auto text = odd_text(kRecordText);
  auto corrupt = text;
  corrupt[5] = static_cast<uint8_t>(corrupt[5] ^ 0x01);
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(checked(corrupt, kArrived), 0);
  catalogue.merge(checked(corrupt, kArrived), 1);
  catalogue.merge(reread_in(text, {{5, 6}}), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, text);
  EXPECT_EQ(records[0].data_present[5], orc::kNabtsByteReread);
  EXPECT_EQ(records[0].data_present[4], kArrived);
}

// The three-block windows of kNabtsJoinWindowSlip find another page on the
// layout as readily as the page itself: it still carries text the page does
// not, and stays a content of its own.
TEST(NabtsRecordCatalogue, AnotherPageOnTheLayoutTwoBlocksLateStaysApart) {
  const auto nine = on_layout(kNineOClock);
  const auto eight = on_layout(kEightOClock);
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(nine), 0);
  catalogue.merge(damaged(nine), 1);
  catalogue.merge(late_by(eight, 92, 54), 2);
  catalogue.merge(late_by(eight, 92, 54), 3);
  catalogue.reconcile_identities();
  EXPECT_EQ(catalogue.records().size(), 2u);
}

// A copy given a hole for two packets it never lost, the rest of its group
// marked absent, keeps a few dozen bytes: some where they belong, the rest two
// blocks late. Laid over the page, the windows lend those bytes to several
// windows at slips that do not line them up; laid over the copy, each is read
// once, and the copy is the page's, where before such copies were listed as
// pages of their own.
TEST(NabtsRecordCatalogue,
     ACopyThatKeptFewBytesTwoBlocksLateIsJoinedToItsPage) {
  const auto whole = page(12, 600);
  std::vector<uint8_t> data(whole.size(), 0x00);
  std::vector<uint8_t> present(whole.size(), 0);
  for (size_t i = 0; i < 38; ++i) {
    data[i] = whole[i];
    present[i] = 1;
  }
  for (size_t i = 94; i < 150; ++i) {
    data[i] = whole[i - 56];
    present[i] = 1;
  }
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(whole), 0);
  catalogue.merge(damaged(whole), 1);
  catalogue.merge(holed(data, present), 2);
  catalogue.reconcile_identities();

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].times_seen, 3u);
  EXPECT_EQ(records[0].data, whole);
}

/// A copy of |data| under version |version| whose packet address had to be
/// corrected, its record header read as transmitted where |header_attested|.
orc::NabtsMessage corrected_channel(uint8_t version, std::vector<uint8_t> data,
                                    bool header_attested) {
  auto out = message(0x000, 0x086, version, std::move(data),
                     /*complete=*/true, /*intact=*/false);
  out.identity_attested = false;
  out.header_attested = header_attested;
  return out;
}

// A second ratings page arrived once, as 000198600 vF, its packet address
// corrected into the service's channel. Its
// content matched page v1's layout and it was folded into v1 and voted into it
// byte by byte. It is another page on that layout, and its record header —
// address and version — arrived as transmitted: a page sent under a version of
// its own, listed as one.
TEST(NabtsRecordCatalogueIdentities,
     APageSentOnceUnderItsOwnVersionIsNotFoldedIntoItsNeighbour) {
  const auto nine = on_layout(kNineOClock);
  const auto eight = on_layout(kEightOClock);
  orc::NabtsRecordCatalogue catalogue;
  carousel(catalogue, 0x000, 0x086, 1, nine, 2, /*attested=*/true);
  catalogue.merge(corrected_channel(0xF, eight, /*header_attested=*/true), 2);
  catalogue.reconcile_identities();

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 2u);
  EXPECT_EQ(records[0].version, 1u);
  EXPECT_EQ(records[0].data, nine);
  EXPECT_EQ(records[1].version, 0xFu);
  EXPECT_EQ(records[1].data, eight);
  EXPECT_EQ(records[1].times_attested, 0u);
}

// Without its header to vouch for the version, a single sighting of another
// page is too little to list — but it is still not voted into its neighbour.
TEST(NabtsRecordCatalogueIdentities,
     AnotherPageSeenOnceIsDroppedRatherThanBlendedIntoItsNeighbour) {
  const auto nine = on_layout(kNineOClock);
  const auto eight = on_layout(kEightOClock);
  orc::NabtsRecordCatalogue catalogue;
  carousel(catalogue, 0x000, 0x086, 1, nine, 2, /*attested=*/true);
  catalogue.merge(corrected_channel(0xF, eight, /*header_attested=*/false), 2);
  const auto reconciliation = catalogue.reconcile_identities();

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, nine);
  EXPECT_EQ(records[0].times_seen, 2u);
  EXPECT_EQ(reconciliation.identities_dropped, 1u);
}

// A misreading of the page itself is the page, header or no header: its copy
// joins the vote.
TEST(NabtsRecordCatalogueIdentities, AMisreadCopyOfThePageIsStillFoldedIn) {
  const auto nine = on_layout(kNineOClock);
  orc::NabtsRecordCatalogue catalogue;
  carousel(catalogue, 0x000, 0x086, 1, nine, 2, /*attested=*/true);
  catalogue.merge(corrected_channel(0xF, nine, /*header_attested=*/true), 2);
  catalogue.reconcile_identities();

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].version, 1u);
  EXPECT_EQ(records[0].times_seen, 3u);
}

// A page's synchronizing packet misaddressed — four wrong bits carry one
// Hamming 8/4 codeword onto another, so the address can arrive as sent —
// opens a group on a data channel the service never used, under the page's
// own record header, and takes in the blocks of whatever else is misaddressed
// there. Named as transmitted or not, it is no page: seen once, beside the
// page seen a hundred times or more.
TEST(NabtsRecordCatalogueIdentities,
     APagesPacketsMisaddressedToAnotherChannelAreRemoved) {
  orc::NabtsRecordCatalogue catalogue;
  carousel(catalogue, 0x000, 0x001, 2, page(1, 120), /*appearances=*/40,
           /*attested=*/true);
  carousel(catalogue, 0x000, 0x002, 2, page(2, 120), /*appearances=*/40,
           /*attested=*/true);
  carousel(catalogue, 0x00E, 0x001, 2, page(3, 120), /*appearances=*/1,
           /*attested=*/true, /*first_frame=*/900);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_EQ(reconciliation.identities_misaddressed, 1u);
  EXPECT_EQ(reconciliation.appearances_misaddressed, 1u);
  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 2u);
  for (const auto& record : records) {
    EXPECT_EQ(record.channel, 0x000);
  }
}

// A channel of the service's own is not one it never used: caption records
// on A00 arrive intact under an address no page carries; a second
// channel's page under a number the first channel's also uses comes round as
// often; and a record seen once that arrived intact is what it says it is.
TEST(NabtsRecordCatalogueIdentities, RecordsOfAChannelTheServiceUsesStay) {
  orc::NabtsRecordCatalogue catalogue;
  for (uint16_t address = 0x001; address <= 0x003; ++address) {
    carousel(catalogue, 0x000, address, 2,
             page(static_cast<uint32_t>(address), 120), /*appearances=*/40,
             /*attested=*/true);
  }
  for (uint64_t frame = 100; frame < 120; ++frame) {
    catalogue.merge(attested_message(0xA00, 0xCC0, 0, page(5, 14), true),
                    frame);
  }
  carousel(catalogue, 0x001, 0x001, 2, page(6, 120), /*appearances=*/30,
           /*attested=*/true);
  catalogue.merge(attested_message(0x00E, 0x002, 2, page(7, 120), true), 900);

  EXPECT_EQ(catalogue.reconcile_identities().identities_misaddressed, 0u);
  EXPECT_EQ(catalogue.records().size(), 6u);
}

}  // namespace
}  // namespace orc_unit_test
