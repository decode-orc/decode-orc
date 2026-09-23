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

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "naplps_lint.h"

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
  // Later, longer, but damaged.
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x70, 60), /*complete=*/true,
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
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x70, 60), /*complete=*/true,
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

// Replacing like with like would only churn, so an identical later copy is
// counted and otherwise ignored.
TEST(NabtsRecordCatalogue, AnEquallyGoodCopyDoesNotDisplaceTheFirst) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 30)), 0);
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x70, 30)), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, bytes(0x40, 30));
  EXPECT_EQ(records[0].times_seen, 2u);
  EXPECT_EQ(records[0].times_intact, 2u);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Combining damaged copies
////////////////////////////////////////////////////////////////////////////////////////////

/// |value| in the low seven bits with b8 set to give the odd parity CEA-516
/// §3.3 requires of every data byte of a type-zero group.
constexpr uint8_t odd(uint8_t value) {
  uint8_t bits = 0;
  for (int bit = 0; bit < 7; ++bit) {
    bits = static_cast<uint8_t>(bits + ((value >> bit) & 1));
  }
  return static_cast<uint8_t>((bits % 2 == 0) ? (value | 0x80) : value);
}

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
  // Parity-clean at the contested position and the newest copy, so without the
  // rule it takes a position the record's own copies could not agree on.
  catalogue.merge(damaged(odd_text(kOtherRecordText)), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, odd_text(kRecordText));
}

// Where most of the copies disagree with the provisional reading there is no
// record for the rest to be outliers of, so the vote is left as it was rather
// than emptied of everything that reached it.
TEST(NabtsRecordCatalogue, NothingIsDroppedWhenTheCopiesHaveNoMajority) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(odd_text(kRecordText)), 0);
  catalogue.merge(damaged(odd_text(kOtherRecordText)), 1);
  catalogue.merge(damaged(odd_text(kThirdRecordText)), 2);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  ASSERT_EQ(records[0].data.size(), odd_text(kRecordText).size());
  // Position 2 is one no two copies agree on, so it still falls to the newest.
  EXPECT_EQ(records[0].data[2], odd(kThirdRecordText[2]));
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
// What the vote leaves behind, and what the grammar makes of what it could not
// settle
////////////////////////////////////////////////////////////////////////////////////////////

/// A code position in the standard's column/row notation.
constexpr uint8_t code(int column, int row) {
  return static_cast<uint8_t>((column << 4) | row);
}

/// A numeric data byte carrying |payload| in b6-b1 (§5.3.1, Figure 10).
constexpr uint8_t numeric(uint8_t payload) {
  return static_cast<uint8_t>(0x40 | (payload & 0x3F));
}

/// §4.3.2's shift out, which invokes the PDI set into GL.
constexpr uint8_t kSo = 0x0E;

/// One three-byte coordinate word carrying |x| and |y| (Figure 11), three bits
/// of each component per byte, most significant first. A value below 0x100
/// leaves the sign bit clear and so names a point inside the unit screen.
std::vector<uint8_t> coordinate(uint16_t x, uint16_t y) {
  std::vector<uint8_t> out;
  for (int field = 2; field >= 0; --field) {
    const uint8_t shift = static_cast<uint8_t>(field * 3);
    const uint8_t packed = static_cast<uint8_t>((((x >> shift) & 0x7u) << 3) |
                                                ((y >> shift) & 0x7u));
    out.push_back(numeric(packed));
  }
  return out;
}

/// A record that draws two lines: SO invokes the PDI set, then LINE ABS (2/8)
/// takes two absolute coordinate words, the second repeating the opcode
/// (§5.3.2.2.5). Parity applied, which is how a record arrives.
std::vector<uint8_t> line_record() {
  std::vector<uint8_t> plain = {kSo, code(2, 8)};
  for (const std::vector<uint8_t>& word :
       {coordinate(0x040, 0x050), coordinate(0x0C0, 0x0D0)}) {
    plain.insert(plain.end(), word.begin(), word.end());
  }
  std::vector<uint8_t> out;
  out.reserve(plain.size());
  for (const uint8_t byte : plain) {
    out.push_back(odd(byte));
  }
  return out;
}

/// Errors the linter finds in |record|, from a general reset.
uint32_t lint_errors(const std::vector<uint8_t>& record) {
  orc::NaplpsLinter linter;
  return linter.lint(record).findings.errors;
}

/// |record| with the byte at |offset| moved out of the numeric columns by a
/// two-bit error. Two bits, so CEA-516 §3.3's odd parity still holds and the
/// recovery has no way of knowing the byte is wrong — which is what leaves the
/// vote with two candidates it cannot separate.
std::vector<uint8_t> break_operand(std::vector<uint8_t> record, size_t offset) {
  record[offset] = static_cast<uint8_t>(record[offset] ^ 0x60);
  return record;
}

// Task 4.1: parity is recomputable from the data, but what the recovery knew
// beyond it dies at the vote unless the vote says so. The masks are what let
// the repair pass tell a byte that arrived and is wrong from one that never
// arrived at all.
TEST(NabtsRecordCatalogue, TheVoteKeepsWhatItKnewAboutEachByte) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(holed({odd('A'), 0x00, odd('C')}, {1, 0, 1}), 0);
  catalogue.merge(holed({odd('A'), 0x00, odd('C')}, {1, 0, 1}), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  ASSERT_EQ(records[0].data_present.size(), records[0].data.size());
  ASSERT_EQ(records[0].data_confidence.size(), records[0].data.size());

  // The middle byte no copy ever delivered, so the NUL standing there is a
  // filler and the mask says so.
  EXPECT_NE(records[0].data_present[0], 0);
  EXPECT_EQ(records[0].data_present[1], 0);
  EXPECT_NE(records[0].data_present[2], 0);

  // Both copies agreed, and nothing measured them, so the bytes that did arrive
  // came out as sure as a vote can be.
  EXPECT_EQ(records[0].data_confidence[0], 255);
  EXPECT_EQ(records[0].data_confidence[2], 255);
}

// A detector that was unsure of a byte no other copy contradicts has still said
// something, and the figure it said it at is the figure that survives.
TEST(NabtsRecordCatalogue, ADoubtfulByteNoCopyContradictsKeepsItsOwnFigure) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(measured({odd('A'), odd('B')}, {40, 255}), 0);
  catalogue.merge(measured({odd('A'), odd('B')}, {40, 255}), 1);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data_confidence[0], 40);
  EXPECT_EQ(records[0].data_confidence[1], 255);
}

// A record that arrived whole is not a record with evidence against it: no
// vote was held, nothing is in doubt, and the masks say nothing rather than
// saying something reassuring.
TEST(NabtsRecordCatalogue, AnIntactRecordCarriesNoDamageEvidence) {
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(message(0x000, 0x001, 0, bytes(0x40, 10)), 0);

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_TRUE(records[0].data_present.empty());
  EXPECT_TRUE(records[0].data_confidence.empty());
  EXPECT_EQ(records[0].vote_positions_contested, 0u);
}

// Task 4.2: two copies disagreeing once each leave the position to whichever
// arrived last, which is a coin toss. Here one of the two candidates breaks the
// PDI structure of §5.3.1 and the other does not, so the grammar knows the
// answer the weights could not reach.
TEST(NabtsRecordCatalogue, TheGrammarSettlesAPositionTheCopiesLeaveLevel) {
  const std::vector<uint8_t> clean = line_record();
  const std::vector<uint8_t> broken = break_operand(clean, 3);
  // The premise: both readings are parity-clean, so the vote's own gate cannot
  // separate them, and only one of them parses.
  ASSERT_EQ(clean.size(), broken.size());
  ASSERT_EQ(lint_errors(clean), 0u);
  ASSERT_GT(lint_errors(broken), 0u);

  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(clean), 0);
  catalogue.merge(damaged(broken), 1);  // the most recent copy, so the default

  const auto records = catalogue.records(/*grammar_assisted_vote=*/true);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, clean)
      << "the vote kept the reading that breaks the PDI structure";
  EXPECT_EQ(records[0].vote_positions_contested, 1u);
  EXPECT_EQ(records[0].vote_positions_adjudicated, 1u);
}

// And with the tie-break off the vote is what it always was: the position goes
// to the most recent copy, and the run still says the position was level.
TEST(NabtsRecordCatalogue, WithoutTheTieBreakALevelPositionGoesToRecency) {
  const std::vector<uint8_t> clean = line_record();
  const std::vector<uint8_t> broken = break_operand(clean, 3);

  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(clean), 0);
  catalogue.merge(damaged(broken), 1);

  const auto records = catalogue.records(/*grammar_assisted_vote=*/false);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, broken);
  EXPECT_EQ(records[0].vote_positions_contested, 1u);
  EXPECT_EQ(records[0].vote_positions_adjudicated, 0u);
}

// The tie-break only ever looks at positions the weights left level. A position
// one candidate wins outright is not reopened because the grammar would have
// preferred the loser — the evidence beats the argument.
TEST(NabtsRecordCatalogue,
     TheGrammarIsNotAskedAboutAPositionTheWeightsSettled) {
  const std::vector<uint8_t> clean = line_record();
  const std::vector<uint8_t> broken = break_operand(clean, 3);

  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged(broken), 0);
  catalogue.merge(damaged(broken), 1);
  catalogue.merge(damaged(clean), 2);

  const auto records = catalogue.records(/*grammar_assisted_vote=*/true);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, broken);
  EXPECT_EQ(records[0].vote_positions_contested, 0u);
}

// Where both candidates leave the record equally well formed the grammar has no
// preference, and a tie-break that guessed anyway would be inventing a record
// rather than recovering one.
TEST(NabtsRecordCatalogue, TheGrammarDeclinesWhereBothReadingsParse) {
  // Two graphic characters. Either is a perfectly good character, so the
  // linter faults neither.
  orc::NabtsRecordCatalogue catalogue;
  catalogue.merge(damaged({odd('A')}), 0);
  catalogue.merge(damaged({odd('B')}), 1);

  const auto records = catalogue.records(/*grammar_assisted_vote=*/true);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, (std::vector<uint8_t>{odd('B')}));
  EXPECT_EQ(records[0].vote_positions_contested, 1u);
  EXPECT_EQ(records[0].vote_positions_adjudicated, 0u);
}

// An application record's data is function descriptors (§7.2.2), not NAPLPS.
// Grading it against the NAPLPS grammar would be reading it in a language it is
// not written in, so the tie-break leaves it to the weights.
TEST(NabtsRecordCatalogue, TheGrammarIsNotAskedAboutAnApplicationRecord) {
  const std::vector<uint8_t> clean = line_record();
  const std::vector<uint8_t> broken = break_operand(clean, 3);

  orc::NabtsRecordCatalogue catalogue;
  auto first = damaged(clean);
  first.type = orc::kNabtsRecordTypeApplication;
  auto second = damaged(broken);
  second.type = orc::kNabtsRecordTypeApplication;
  catalogue.merge(first, 0);
  catalogue.merge(second, 1);

  const auto records = catalogue.records(/*grammar_assisted_vote=*/true);
  ASSERT_EQ(records.size(), 1u);
  EXPECT_EQ(records[0].data, broken);
  EXPECT_EQ(records[0].vote_positions_adjudicated, 0u);
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
// note; the reference CBS recording carries a four-record ring — must come
// out as one chain, not as one single-member "chain" per member: every member
// takes the smallest address as the base.
TEST(NabtsRecordCatalogue, ARingOfMoreLinksSharesOneCanonicalBase) {
  orc::NabtsRecordCatalogue catalogue;

  auto first = long_address_message(0x000, 0x4409, 0, draw_letter('X'));
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
// trusted, so they decide even when they are outnumbered.
TEST(NabtsRecordCatalogue, TheAttestedAppearancesDecideTheFlags) {
  orc::NabtsRecordCatalogue catalogue;
  for (int index = 0; index < 2; ++index) {
    auto copy = message(0x000, 0x001, 0, bytes(0x40, 10), true, false);
    copy.identity_attested = true;
    copy.classification.caption = true;
    catalogue.merge(copy, static_cast<uint64_t>(index));
  }
  for (int index = 2; index < 6; ++index) {
    auto copy = message(0x000, 0x001, 0, bytes(0x40, 10), true, false);
    catalogue.merge(copy, static_cast<uint64_t>(index));
  }

  const auto records = catalogue.records();
  ASSERT_EQ(records.size(), 1u);
  EXPECT_TRUE(records[0].caption);
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

// A service numbering its records densely offers two equally good explanations
// for a misreading, and neither is evidence of anything.
TEST(NabtsRecordCatalogueIdentities, LeaveAnAmbiguousMisreadingUnfolded) {
  orc::NabtsRecordCatalogue catalogue;
  carousel(catalogue, 0x000, 0x003, 6, bytes(0x40, 8), /*appearances=*/20,
           /*attested=*/true);
  carousel(catalogue, 0x000, 0x007, 6, bytes(0x50, 8), /*appearances=*/20,
           /*attested=*/true, /*first_frame=*/100);
  // 000/005 is one digit from both.
  carousel(catalogue, 0x000, 0x005, 6, bytes(0x50, 8), /*appearances=*/2,
           /*attested=*/false, /*first_frame=*/900);

  const auto reconciliation = catalogue.reconcile_identities();
  EXPECT_EQ(reconciliation.identities_folded, 0u);
  EXPECT_EQ(reconciliation.identities_dropped, 1u);
  EXPECT_EQ(catalogue.records().size(), 2u);
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

}  // namespace
}  // namespace orc_unit_test
