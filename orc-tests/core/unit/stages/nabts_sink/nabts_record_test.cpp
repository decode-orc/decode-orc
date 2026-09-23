/*
 * File:        nabts_record_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for NABTS record headers, message linking and
 *              application function descriptors (CEA-516 §5, §7)
 *
 * Covers: short and long record addresses and the equivalence §5.2.5 requires
 * of them, the record link, the classification sequence's pointer and flag
 * bytes, header extension fields, linked series arriving out of order, the
 * reserved addresses of §7.1.5, and the function descriptors of §7.2.2.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_record.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "nabts_test_builders.h"

namespace orc_unit_test {
namespace {

using nabts::flags;
using nabts::hamming;
using nabts::make_classification;
using nabts::make_header_extension;
using nabts::make_link;
using nabts::make_long_address;
using nabts::make_short_address;
using nabts::parity;
using nabts::record_designator;

/// Append |tail| to |head| and return it, so a header can be built inline.
std::vector<uint8_t> operator+(std::vector<uint8_t> head,
                               const std::vector<uint8_t>& tail) {
  head.insert(head.end(), tail.begin(), tail.end());
  return head;
}

orc::NabtsRecordHeader decode(const std::vector<uint8_t>& bytes) {
  return orc::nabts_decode_record_header(bytes.data(), bytes.size());
}

/// A minimal header: RT, RD with nothing announced, and a short address.
std::vector<uint8_t> minimal_header(uint8_t type, uint16_t address) {
  return std::vector<uint8_t>{hamming(type), hamming(record_designator(
                                                 false, false, false, false))} +
         make_short_address(address);
}

/// Odd-parity record data, so a test can tell a record's data from its header.
std::vector<uint8_t> record_data(std::initializer_list<uint8_t> values) {
  std::vector<uint8_t> out;
  for (const uint8_t value : values) {
    out.push_back(parity(value));
  }
  return out;
}

////////////////////////////////////////////////////////////////////////////////////////////
// The record header
////////////////////////////////////////////////////////////////////////////////////////////

// §5.2.1: RT, RD and the three address bytes are always present, and a header
// that announced nothing else ends after them.
TEST(NabtsRecordHeader, ReadsTheFixedFiveBytes) {
  const auto header =
      decode(minimal_header(orc::kNabtsRecordTypeApplication, 0x1A4));

  ASSERT_TRUE(header.valid);
  EXPECT_EQ(header.type, orc::kNabtsRecordTypeApplication);
  EXPECT_EQ(header.header_bytes, orc::kNabtsRecordHeaderMinBytes);
  EXPECT_FALSE(header.linked);
  EXPECT_FALSE(header.classification.present);
  EXPECT_TRUE(header.extensions.empty());
  EXPECT_EQ(header.address.text(), "1A4");
  EXPECT_FALSE(header.address.long_form);
}

// §5.2.5: a receiver must treat short address PQR and long address 0000PQR00 as
// the same record, so the two compare equal however they were transmitted.
TEST(NabtsRecordHeader, TreatsAShortAddressAsItsLongEquivalent) {
  const auto short_form = decode(minimal_header(0, 0x1A4));
  const auto long_form = decode(
      std::vector<uint8_t>{
          hamming(0), hamming(record_designator(true, false, false, false))} +
      make_long_address(0x0001A400ULL));

  ASSERT_TRUE(short_form.valid);
  ASSERT_TRUE(long_form.valid);
  EXPECT_EQ(short_form.address, long_form.address);
  // How it was transmitted is remembered, because that is what a report shows.
  EXPECT_FALSE(short_form.address.long_form);
  EXPECT_TRUE(long_form.address.long_form);
  EXPECT_EQ(short_form.address.text(), "1A4");
  EXPECT_EQ(long_form.address.text(), "00001A400");
}

// A long address carrying something no short address could keeps all nine
// digits.
TEST(NabtsRecordHeader, KeepsALongAddressThatIsNotShortExpressible) {
  const auto header = decode(
      std::vector<uint8_t>{
          hamming(0), hamming(record_designator(true, false, false, false))} +
      make_long_address(0x123456789ULL));

  ASSERT_TRUE(header.valid);
  EXPECT_TRUE(header.address.long_form);
  EXPECT_EQ(header.address.text(), "123456789");
  EXPECT_EQ(header.header_bytes, 2u + orc::kNabtsLongAddressNibbles);
}

// §5.2.6: L1 b8 says whether more linked records follow, and the remaining
// seven information bits are the order within the series, L1 the more
// significant.
TEST(NabtsRecordHeader, ReadsTheRecordLinkOrderAndContinuationFlag) {
  struct Case {
    uint8_t order;
    bool more;
  };
  const Case cases[] = {
      {0, true}, {1, true}, {5, false}, {0x7F, false}, {0x40, true}};

  for (const Case& test_case : cases) {
    const auto header = decode(
        std::vector<uint8_t>{
            hamming(0), hamming(record_designator(false, true, false, false))} +
        make_short_address(0x001) + make_link(test_case.order, test_case.more));

    ASSERT_TRUE(header.valid) << "order " << static_cast<int>(test_case.order);
    EXPECT_TRUE(header.linked);
    EXPECT_EQ(header.link_order, test_case.order);
    EXPECT_EQ(header.more_links, test_case.more);
    EXPECT_EQ(header.header_bytes, orc::kNabtsRecordHeaderMinBytes + 2u);
  }
}

// §5.2.7.2 assigns meanings to the six flag bytes of the first group, with Y16
// carrying the version number as a whole nibble.
TEST(NabtsRecordHeader, ReadsTheClassificationFlagBytes) {
  const auto header = decode(
      std::vector<uint8_t>{
          hamming(0), hamming(record_designator(false, false, true, false))} +
      make_short_address(0x001) +
      make_classification(
          /*y11=*/0, /*y12=*/0,
          // Y13: b8 caption, b6 delay, b4 index
          /*y13=*/flags(true, false, true, false),
          // Y14: b8 more, b6 cyclic marker, b4 auto acquire, b2 support needed
          /*y14=*/flags(true, true, false, false),
          // Y15: b8 priority, b6 alarm, b4 update, b2 support record
          /*y15=*/flags(false, true, true, false),
          /*y16=*/7));

  ASSERT_TRUE(header.valid);
  const auto& c = header.classification;
  EXPECT_TRUE(c.present);
  EXPECT_TRUE(c.caption);
  EXPECT_FALSE(c.delay);
  EXPECT_TRUE(c.index);
  EXPECT_TRUE(c.more);
  EXPECT_TRUE(c.cyclic_marker);
  EXPECT_FALSE(c.auto_acquire);
  EXPECT_FALSE(c.support_needed);
  EXPECT_FALSE(c.priority);
  EXPECT_TRUE(c.alarm);
  EXPECT_TRUE(c.update);
  EXPECT_FALSE(c.support_record);
  EXPECT_TRUE(c.version_present);
  EXPECT_EQ(c.version, 7u);
}

// §5.2.7.2: an absent flag byte is read as though it were zero, so a
// classification sequence announcing no flag pairs leaves every flag clear
// without being a fault.
TEST(NabtsRecordHeader, AnEmptyClassificationSequenceLeavesEveryFlagClear) {
  const auto header = decode(
      std::vector<uint8_t>{
          hamming(0), hamming(record_designator(false, false, true, false))} +
      make_short_address(0x001) +
      // Pointer byte with no pairs announced and no successor.
      std::vector<uint8_t>{hamming(flags(false, false, false, false))});

  ASSERT_TRUE(header.valid);
  EXPECT_TRUE(header.classification.present);
  EXPECT_FALSE(header.classification.caption);
  EXPECT_FALSE(header.classification.version_present);
  EXPECT_EQ(header.header_bytes, orc::kNabtsRecordHeaderMinBytes + 1u);
}

// §5.2.7.1: pointer byte b8 announces a further pointer byte, and the flag
// bytes of any group after the first have meanings the standard reserves. They
// are walked over — the header's length depends on them — and counted.
TEST(NabtsRecordHeader, WalksASecondPointerByteAndCountsItsReservedFlags) {
  const auto header = decode(
      std::vector<uint8_t>{
          hamming(0), hamming(record_designator(false, false, true, false))} +
      make_short_address(0x001) +
      std::vector<uint8_t>{
          // Y01: b8 set (another pointer follows), b2 set (Y11/Y12 present).
          hamming(flags(true, false, false, true)), hamming(0), hamming(0),
          // Y02: no successor, b2 set (Y21/Y22 present).
          hamming(flags(false, false, false, true)), hamming(0xF),
          hamming(0xF)} +
      record_data({0x41}));

  ASSERT_TRUE(header.valid);
  // Y11 and Y12 are reserved too (§5.2.7.2), so all four are counted.
  EXPECT_EQ(header.classification.reserved_flag_bytes, 4u);
  EXPECT_EQ(header.header_bytes, orc::kNabtsRecordHeaderMinBytes + 6u);
}

// The address a mis-corrected digit invents is indistinguishable from a real
// one further down, so the header has to carry whether §5.2.1's identity was
// received or inferred (issue #267).
TEST(NabtsRecordHeader, AttestsAnIdentityThatArrivedAsCodewords) {
  const auto clean = minimal_header(0, 0x123) + record_data({0x41});
  ASSERT_TRUE(decode(clean).valid);
  EXPECT_TRUE(decode(clean).identity_attested);

  // RT, RD and the three address bytes all bear on which record this is: RD
  // decides how long the address is, so a corrected one is as much a doubt
  // about the identity as a corrected digit.
  for (size_t byte = 0; byte < orc::kNabtsRecordHeaderMinBytes; ++byte) {
    auto damaged = clean;
    damaged[byte] = static_cast<uint8_t>(damaged[byte] ^ 0x01);
    const auto header = decode(damaged);
    ASSERT_TRUE(header.valid) << "byte " << byte;
    EXPECT_FALSE(header.identity_attested) << "byte " << byte;
  }
}

// The version is part of §5.2.1's identity, so the classification sequence
// carrying it is inside what has to have arrived cleanly.
TEST(NabtsRecordHeader, AttestationCoversTheClassificationSequence) {
  const auto clean =
      std::vector<uint8_t>{
          hamming(0), hamming(record_designator(false, false, true, false))} +
      make_short_address(0x001) +
      make_classification(0, 0, 0, 0, 0, /*y16=*/6) + record_data({0x41});
  ASSERT_TRUE(decode(clean).valid);
  EXPECT_TRUE(decode(clean).identity_attested);

  // The Y16 byte, which is the version.
  auto damaged = clean;
  const size_t y16 = orc::kNabtsRecordHeaderMinBytes + 6;
  damaged[y16] = static_cast<uint8_t>(damaged[y16] ^ 0x01);
  const auto header = decode(damaged);
  ASSERT_TRUE(header.valid);
  EXPECT_EQ(header.classification.version, 6u);
  EXPECT_FALSE(header.identity_attested);
}

// A header extension says where the record's data starts, not which record it
// is, so a corrected byte in one is not a doubt about the identity.
TEST(NabtsRecordHeader, AttestationIgnoresTheHeaderExtension) {
  auto packet =
      std::vector<uint8_t>{
          hamming(0), hamming(record_designator(false, false, false, true))} +
      make_short_address(0x001) +
      make_header_extension(/*meaning=*/2, {0x1, 0x2, 0x3}, /*more=*/false) +
      record_data({0x41});
  // The extension introducer is the first byte past the fixed five.
  packet[orc::kNabtsRecordHeaderMinBytes] =
      static_cast<uint8_t>(packet[orc::kNabtsRecordHeaderMinBytes] ^ 0x01);

  const auto header = decode(packet);
  ASSERT_TRUE(header.valid);
  EXPECT_TRUE(header.identity_attested);
}

// §5.2.8: each field is an introducer, a size, and that many data bytes; the
// introducer's b8 says whether another field follows.
TEST(NabtsRecordHeader, ReadsChainedHeaderExtensionFields) {
  const auto header = decode(
      std::vector<uint8_t>{
          hamming(0), hamming(record_designator(false, false, false, true))} +
      make_short_address(0x001) +
      // Meaning 2 — redefinition of the Next record address — with the three
      // address digits §5.2.8.4 gives for decimal value 3.
      make_header_extension(/*meaning=*/2, {0x1, 0x2, 0x3}, /*more=*/true) +
      make_header_extension(/*meaning=*/4, {0x0}, /*more=*/false));

  ASSERT_TRUE(header.valid);
  ASSERT_EQ(header.extensions.size(), 2u);
  EXPECT_EQ(header.extensions[0].meaning, 2u);
  EXPECT_EQ(header.extensions[0].size, 3u);
  EXPECT_EQ(header.extensions[0].data, std::vector<uint8_t>({0x1, 0x2, 0x3}));
  EXPECT_EQ(header.extensions[1].meaning, 4u);
  EXPECT_EQ(header.extensions[1].size, 1u);
  // 5 fixed + (2 + 3) + (2 + 1)
  EXPECT_EQ(header.header_bytes, 13u);
}

// Every optional sub-group at once, in the order §5.2.1 lists them, so a
// mis-ordered read would put the record data in the wrong place.
TEST(NabtsRecordHeader, ReadsEverySubGroupInTheOrderTheStandardListsThem) {
  const auto data = record_data({0x48, 0x49});
  const auto bytes =
      std::vector<uint8_t>{hamming(orc::kNabtsRecordTypePriorityPresentation),
                           hamming(record_designator(true, true, true, true))} +
      make_long_address(0x123456789ULL) + make_link(3, /*more=*/true) +
      make_classification(0, 0, flags(true, false, false, false), 0, 0, 4) +
      make_header_extension(1, {0x7}) + data;
  const auto header = decode(bytes);

  ASSERT_TRUE(header.valid);
  EXPECT_EQ(header.type, orc::kNabtsRecordTypePriorityPresentation);
  EXPECT_EQ(header.address.text(), "123456789");
  EXPECT_EQ(header.link_order, 3u);
  EXPECT_TRUE(header.more_links);
  EXPECT_TRUE(header.classification.caption);
  EXPECT_EQ(header.classification.version, 4u);
  ASSERT_EQ(header.extensions.size(), 1u);
  // 2 + 9 address + 2 link + 7 classification + 3 extension = 23
  EXPECT_EQ(header.header_bytes, 23u);
  EXPECT_EQ(bytes.size() - header.header_bytes, data.size());
}

// A header whose bytes ran out part way through is reported invalid: the
// alternative is a record whose data begins at a guess.
TEST(NabtsRecordHeader, RefusesAHeaderThatRanOutOfBytes) {
  // Announces an address extension but carries only the short address.
  const auto truncated =
      std::vector<uint8_t>{
          hamming(0), hamming(record_designator(true, false, false, false))} +
      make_short_address(0x001);
  EXPECT_FALSE(decode(truncated).valid);

  // Announces a header extension whose data bytes are not there.
  auto short_extension =
      std::vector<uint8_t>{
          hamming(0), hamming(record_designator(false, false, false, true))} +
      make_short_address(0x001) +
      std::vector<uint8_t>{hamming(1), hamming(6)};  // ES says six, none follow
  EXPECT_FALSE(decode(short_extension).valid);

  EXPECT_FALSE(decode({}).valid);
  EXPECT_FALSE(orc::nabts_decode_record_header(nullptr, 8).valid);
}

// §5.2.1 makes every header byte Hamming 8/4, so an uncorrectable one anywhere
// in the header costs the whole header.
TEST(NabtsRecordHeader, RefusesAHeaderWithAnUncorrectableByte) {
  const auto clean = minimal_header(0, 0x1A4);
  for (size_t byte = 0; byte < clean.size(); ++byte) {
    auto damaged = clean;
    damaged[byte] = static_cast<uint8_t>(damaged[byte] ^ 0x03);
    EXPECT_FALSE(decode(damaged).valid) << "byte " << byte;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////
// Reserved addresses (§7.1.5)
////////////////////////////////////////////////////////////////////////////////////////////

TEST(NabtsReservedPurpose, RecognisesTheAddressesTheStandardReserves) {
  const auto address_of = [](uint16_t short_address) {
    return decode(minimal_header(0, short_address)).address;
  };

  EXPECT_EQ(orc::nabts_reserved_purpose(0x000, address_of(0x000)),
            "Master Index Page and power-up Record");
  EXPECT_EQ(orc::nabts_reserved_purpose(0x000, address_of(0xFFE)),
            "Service Application Record");
  EXPECT_EQ(orc::nabts_reserved_purpose(0xA00, address_of(0x000)),
            "Start of captioning");
  EXPECT_EQ(orc::nabts_reserved_purpose(0xB00, address_of(0x000)),
            "Start of Flash");

  // FFF is the support record on every channel, which is why it is tested
  // first in the implementation.
  EXPECT_EQ(orc::nabts_reserved_purpose(0x000, address_of(0xFFF)),
            "Support Record");
  EXPECT_EQ(orc::nabts_reserved_purpose(0x7C3, address_of(0xFFF)),
            "Support Record");

  // The same address on another channel is not reserved.
  EXPECT_TRUE(orc::nabts_reserved_purpose(0x001, address_of(0x000)).empty());
  EXPECT_TRUE(orc::nabts_reserved_purpose(0x000, address_of(0x123)).empty());
}

// The reserved addresses are short ones, and §5.2.5's equivalence means a long
// address that reduces to one is the same record.
TEST(NabtsReservedPurpose, MatchesALongAddressThatReducesToAReservedOne) {
  const auto header = decode(
      std::vector<uint8_t>{
          hamming(0), hamming(record_designator(true, false, false, false))} +
      make_long_address(0x000000000ULL));
  ASSERT_TRUE(header.valid);
  EXPECT_EQ(orc::nabts_reserved_purpose(0xA00, header.address),
            "Start of captioning");
}

////////////////////////////////////////////////////////////////////////////////////////////
// Message linking (§5.2.6)
////////////////////////////////////////////////////////////////////////////////////////////

/// Collects the messages an assembler emits, feeding it groups built from
/// header bytes and record data.
class MessageHarness {
 public:
  MessageHarness() {
    assembler_.set_message_callback([this](const orc::NabtsMessage& message) {
      messages_.push_back(message);
    });
  }

  void feed(uint16_t channel, const std::vector<uint8_t>& header,
            const std::vector<uint8_t>& data, bool intact = true,
            uint8_t group_type = orc::kNabtsBroadcastGroupType,
            bool attested = true) {
    orc::NabtsDataGroup group;
    group.channel = channel;
    group.channel_attested = attested;
    group.header.valid = true;
    group.header.attested = attested;
    group.header.type = group_type;
    group.outcome = intact ? orc::NabtsGroupOutcome::kComplete
                           : orc::NabtsGroupOutcome::kUnfinished;
    group.data = header;
    group.data.insert(group.data.end(), data.begin(), data.end());
    assembler_.add_group(group);
  }

  const std::vector<orc::NabtsMessage>& messages() const { return messages_; }
  const orc::NabtsRecordStats& stats() const { return assembler_.stats(); }
  void flush() { assembler_.flush(); }

 private:
  orc::NabtsRecordAssembler assembler_;
  std::vector<orc::NabtsMessage> messages_;
};

// §5.2.6: a message may consist of a single unlinked record, which is therefore
// complete the moment it arrives.
TEST(NabtsRecordAssembler, AnUnlinkedRecordIsAMessageOnItsOwn) {
  MessageHarness harness;
  const auto data = record_data({0x41, 0x42, 0x43});
  harness.feed(0x100, minimal_header(0, 0x1A4), data);

  ASSERT_EQ(harness.messages().size(), 1u);
  const auto& message = harness.messages()[0];
  EXPECT_EQ(message.channel, 0x100u);
  EXPECT_EQ(message.address.text(), "1A4");
  EXPECT_TRUE(message.complete);
  EXPECT_TRUE(message.intact);
  EXPECT_EQ(message.records, 1u);
  EXPECT_EQ(message.data, data);
  EXPECT_EQ(harness.stats().unlinked_records, 1u);
}

// §5.2.6: a set of linked records constitutes a message, joined in link order.
TEST(NabtsRecordAssembler, JoinsALinkedSeriesInLinkOrder) {
  MessageHarness harness;
  const auto first = record_data({0x41});
  const auto second = record_data({0x42});
  const auto third = record_data({0x43});

  const auto header = [](uint8_t order, bool more) {
    return std::vector<uint8_t>{hamming(0), hamming(record_designator(
                                                false, true, false, false))} +
           make_short_address(0x001) + make_link(order, more);
  };

  harness.feed(0x000, header(0, true), first);
  harness.feed(0x000, header(1, true), second);
  EXPECT_TRUE(harness.messages().empty());  // the series has not ended yet
  harness.feed(0x000, header(2, false), third);

  ASSERT_EQ(harness.messages().size(), 1u);
  const auto& message = harness.messages()[0];
  EXPECT_TRUE(message.complete);
  EXPECT_EQ(message.records, 3u);
  EXPECT_EQ(message.links_expected, 3u);
  EXPECT_EQ(message.data, first + second + third);
  EXPECT_EQ(harness.stats().linked_records, 3u);
}

// A recording that starts part way through a carousel sees the middle of a
// series before its beginning, so arrival order cannot be relied on.
TEST(NabtsRecordAssembler, JoinsALinkedSeriesThatArrivedOutOfOrder) {
  MessageHarness harness;
  const auto first = record_data({0x41});
  const auto second = record_data({0x42});
  const auto third = record_data({0x43});

  const auto header = [](uint8_t order, bool more) {
    return std::vector<uint8_t>{hamming(0), hamming(record_designator(
                                                false, true, false, false))} +
           make_short_address(0x001) + make_link(order, more);
  };

  // Last first, then the middle, then the first.
  harness.feed(0x000, header(2, false), third);
  harness.feed(0x000, header(1, true), second);
  EXPECT_TRUE(harness.messages().empty());
  harness.feed(0x000, header(0, true), first);

  ASSERT_EQ(harness.messages().size(), 1u);
  EXPECT_TRUE(harness.messages()[0].complete);
  // Concatenated in link order, not arrival order.
  EXPECT_EQ(harness.messages()[0].data, first + second + third);
}

// §5.2.6: every record after the first defers to the first record's
// classification sequence and header extension, and its own are to be ignored.
TEST(NabtsRecordAssembler, TakesTheClassificationFromTheFirstLinkedRecord) {
  MessageHarness harness;

  const auto first_header =
      std::vector<uint8_t>{
          hamming(0), hamming(record_designator(false, true, true, false))} +
      make_short_address(0x001) + make_link(0, /*more=*/true) +
      make_classification(0, 0, flags(true, false, false, false), 0, 0,
                          /*y16=*/3);
  // The second record names a different version and a different caption flag,
  // both of which must be ignored.
  const auto second_header =
      std::vector<uint8_t>{
          hamming(0), hamming(record_designator(false, true, true, false))} +
      make_short_address(0x001) + make_link(1, /*more=*/false) +
      make_classification(0, 0, flags(false, false, false, false), 0, 0,
                          /*y16=*/9);

  harness.feed(0x000, first_header, record_data({0x41}));
  harness.feed(0x000, second_header, record_data({0x42}));

  // The two records name different versions, which §5.2.1 makes part of a
  // record's identity — so they open two series rather than one, and neither
  // completes. Flushed, both are delivered partial.
  harness.flush();
  ASSERT_EQ(harness.messages().size(), 2u);
  for (const auto& message : harness.messages()) {
    EXPECT_FALSE(message.complete);
  }
}

// A series whose records agree on their version joins, and the first record's
// classification is the message's.
TEST(NabtsRecordAssembler, AMessageCarriesTheFirstRecordsVersionAndFlags) {
  MessageHarness harness;
  const auto classification =
      make_classification(0, 0, flags(true, false, false, false),
                          flags(false, true, false, false), 0, /*y16=*/3);
  const auto header = [&classification](uint8_t order, bool more) {
    return std::vector<uint8_t>{
               hamming(orc::kNabtsRecordTypeNoncyclicPresentation),
               hamming(record_designator(false, true, true, false))} +
           make_short_address(0x001) + make_link(order, more) + classification;
  };

  harness.feed(0x000, header(0, true), record_data({0x41}));
  harness.feed(0x000, header(1, false), record_data({0x42}));

  ASSERT_EQ(harness.messages().size(), 1u);
  const auto& message = harness.messages()[0];
  EXPECT_TRUE(message.complete);
  EXPECT_EQ(message.version, 3u);
  EXPECT_EQ(message.type, orc::kNabtsRecordTypeNoncyclicPresentation);
  EXPECT_TRUE(message.classification.caption);
  EXPECT_TRUE(message.classification.cyclic_marker);
}

TEST(NabtsRecordAssembler, FlushDeliversAnIncompleteSeriesAsPartial) {
  MessageHarness harness;
  const auto header =
      std::vector<uint8_t>{
          hamming(0), hamming(record_designator(false, true, false, false))} +
      make_short_address(0x001) + make_link(0, /*more=*/true);
  harness.feed(0x000, header, record_data({0x41}));

  EXPECT_TRUE(harness.messages().empty());
  harness.flush();
  ASSERT_EQ(harness.messages().size(), 1u);
  EXPECT_FALSE(harness.messages()[0].complete);
  EXPECT_EQ(harness.stats().messages_partial, 1u);

  harness.flush();  // idempotent
  EXPECT_EQ(harness.messages().size(), 1u);
}

// A group carrying damaged or missing packets still yields its record, marked
// so a caller can prefer a clean copy of it.
TEST(NabtsRecordAssembler, MarksAMessageFromADamagedGroupAsNotIntact) {
  MessageHarness harness;
  harness.feed(0x000, minimal_header(0, 0x001), record_data({0x41}),
               /*intact=*/false);

  ASSERT_EQ(harness.messages().size(), 1u);
  EXPECT_TRUE(harness.messages()[0].complete);
  EXPECT_FALSE(harness.messages()[0].intact);
}

// §4.3: only a type-zero group's data is a teletext record. Guessing a record
// header out of another type would invent records that were never transmitted.
TEST(NabtsRecordAssembler, IgnoresAGroupOfANonTeletextType) {
  MessageHarness harness;
  harness.feed(0x000, minimal_header(0, 0x001), record_data({0x41}),
               /*intact=*/true, orc::kNabtsPrivateGroupType);

  EXPECT_TRUE(harness.messages().empty());
  EXPECT_EQ(harness.stats().non_teletext_groups, 1u);
  EXPECT_EQ(harness.stats().records_seen, 0u);
}

// A channel carrying another application's groups is a service the recording
// really has, and one a reader is owed the name of; so the groups that arrived
// clean are counted by channel and type.
TEST(NabtsRecordAssembler, CountsAnotherServicesCleanGroupsByChannelAndType) {
  MessageHarness harness;
  for (int i = 0; i < 3; ++i) {
    harness.feed(0xCFD, {}, record_data({0x41}), /*intact=*/true,
                 orc::kNabtsPrivateGroupType);
  }
  harness.feed(0x123, {}, record_data({0x41}), /*intact=*/true, 7);

  const auto& foreign = harness.stats().attested_foreign_groups;
  ASSERT_EQ(foreign.size(), 2u);
  EXPECT_EQ(foreign.at({0xCFD, orc::kNabtsPrivateGroupType}), 3u);
  EXPECT_EQ(foreign.at({0x123, 7}), 1u);
  EXPECT_NE(harness.stats().summary().find("channel CFD, 3 clean group(s) of "
                                           "type 15 (private use)"),
            std::string::npos);
}

// Noise corrects into a non-zero group type by the hundred on a poor transfer.
// It never arrives clean, so it is counted as not teletext and no more: naming
// it as a service would send a reader looking for one that is not there.
TEST(NabtsRecordAssembler, DoesNotNameAServiceFromACorrectedGroup) {
  MessageHarness harness;
  harness.feed(0xCFD, {}, record_data({0x41}), /*intact=*/true,
               orc::kNabtsPrivateGroupType, /*attested=*/false);

  EXPECT_EQ(harness.stats().non_teletext_groups, 1u);
  EXPECT_TRUE(harness.stats().attested_foreign_groups.empty());
}

TEST(NabtsRecordAssembler, CountsAGroupWhoseRecordHeaderDidNotDecode) {
  MessageHarness harness;
  auto header = minimal_header(0, 0x001);
  header[0] = static_cast<uint8_t>(header[0] ^ 0x03);  // two bits
  harness.feed(0x000, header, record_data({0x41}));

  EXPECT_TRUE(harness.messages().empty());
  EXPECT_EQ(harness.stats().header_failures, 1u);
}

// The open-series bound is what stops a recording full of damaged headers from
// accumulating them. An evicted series is still delivered: a partial message is
// worth listing, and dropping it silently would make a busy recording look
// sparse.
TEST(NabtsRecordAssembler, EvictsTheOldestSeriesRatherThanGrowingWithoutBound) {
  MessageHarness harness;
  const auto header = [](uint16_t address) {
    return std::vector<uint8_t>{hamming(0), hamming(record_designator(
                                                false, true, false, false))} +
           make_short_address(address) + make_link(0, /*more=*/true);
  };

  for (size_t i = 0; i <= orc::kNabtsMaxOpenMessages; ++i) {
    harness.feed(0x000, header(static_cast<uint16_t>(0x100 + i)),
                 record_data({0x41}));
  }

  // The oldest was evicted, and delivered rather than dropped.
  ASSERT_EQ(harness.messages().size(), 1u);
  EXPECT_FALSE(harness.messages()[0].complete);
  EXPECT_EQ(harness.messages()[0].address.text(), "100");
  EXPECT_EQ(harness.stats().messages_evicted, 1u);

  harness.flush();
  EXPECT_EQ(harness.messages().size(), orc::kNabtsMaxOpenMessages + 1);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Application function descriptors (§7.2.2)
////////////////////////////////////////////////////////////////////////////////////////////

// §7.2.2: a descriptor is a function code, its arguments and the delimiter
// 0/13; nulls are ignored and a leading delimiter is recommended practice.
TEST(NabtsApplicationRecord, SplitsDescriptorsOnTheFunctionDelimiter) {
  const auto data =
      record_data({0x0D, 0x20, 0x41, 0x42, 0x0D, 0x31, 0x43, 0x0D});
  const auto functions = orc::nabts_decode_application_record(data);

  ASSERT_EQ(functions.size(), 2u);
  EXPECT_EQ(functions[0].code, 0x20u);
  EXPECT_EQ(functions[0].code_text(), "2/0");
  EXPECT_TRUE(functions[0].is_control());
  EXPECT_FALSE(functions[0].is_information());
  EXPECT_EQ(functions[0].arguments, std::vector<uint8_t>({0x41, 0x42}));

  EXPECT_EQ(functions[1].code, 0x31u);
  EXPECT_EQ(functions[1].code_text(), "3/1");
  EXPECT_FALSE(functions[1].is_control());
  EXPECT_TRUE(functions[1].is_information());
  EXPECT_EQ(functions[1].arguments, std::vector<uint8_t>({0x43}));
}

// §7.2.3.1: a descriptor of a function code and delimiter only, with no
// arguments, asks the receiver to restore that function's initial state.
TEST(NabtsApplicationRecord, ReadsAnArgumentlessDescriptorAsAStateReset) {
  const auto functions =
      orc::nabts_decode_application_record(record_data({0x22, 0x0D}));

  ASSERT_EQ(functions.size(), 1u);
  EXPECT_EQ(functions[0].code, 0x22u);
  EXPECT_TRUE(functions[0].resets_state());
}

TEST(NabtsApplicationRecord, IgnoresNullCodes) {
  const auto functions = orc::nabts_decode_application_record(
      record_data({0x00, 0x00, 0x20, 0x00, 0x41, 0x00, 0x0D}));

  ASSERT_EQ(functions.size(), 1u);
  EXPECT_EQ(functions[0].code, 0x20u);
  EXPECT_EQ(functions[0].arguments, std::vector<uint8_t>({0x41}));
}

// A record whose final delimiter was lost still yielded a descriptor.
TEST(NabtsApplicationRecord, ClosesADescriptorWhoseDelimiterWasLost) {
  const auto functions =
      orc::nabts_decode_application_record(record_data({0x20, 0x41, 0x42}));

  ASSERT_EQ(functions.size(), 1u);
  EXPECT_EQ(functions[0].arguments, std::vector<uint8_t>({0x41, 0x42}));
}

// §7.2.2 forbids 0/1-0/12 and 0/14-1/15 in an application record, so one here
// is damage. It ends the descriptor it appeared in rather than being swallowed
// into its arguments, so the descriptors before and after it survive intact.
//
// The cost is stated rather than hidden: the argument that followed the damaged
// byte becomes a descriptor of its own, so a record with one damaged byte lists
// one spurious argumentless descriptor. That is affordable only because these
// are listed for a reader and never executed — see NabtsRecordFunction in
// vbi-services/vbi_analysis_results.h. Dropping the rest of the record instead
// would lose real descriptors to local damage, which is the worse trade.
TEST(NabtsApplicationRecord,
     AnIllegalCodeEndsItsDescriptorWithoutLosingTheRest) {
  const auto functions = orc::nabts_decode_application_record(
      record_data({0x20, 0x41, 0x05, 0x42, 0x0D, 0x30, 0x43, 0x0D}));

  ASSERT_EQ(functions.size(), 3u);
  EXPECT_EQ(functions[0].code, 0x20u);
  EXPECT_EQ(functions[0].arguments, std::vector<uint8_t>({0x41}));
  // The spurious one: 0x42 was an argument, and the damaged byte before it made
  // it look like a function code.
  EXPECT_EQ(functions[1].code, 0x42u);
  EXPECT_TRUE(functions[1].arguments.empty());
  // The descriptor after the damage is untouched.
  EXPECT_EQ(functions[2].code, 0x30u);
  EXPECT_EQ(functions[2].arguments, std::vector<uint8_t>({0x43}));
}

TEST(NabtsApplicationRecord, IsEmptyForARecordWithNoDescriptors) {
  EXPECT_TRUE(orc::nabts_decode_application_record({}).empty());
  EXPECT_TRUE(
      orc::nabts_decode_application_record(record_data({0x0D, 0x0D})).empty());
}

// §7.2.2 makes the application record's function descriptors the whole of its
// content, so a message of that type carries them and a presentation record
// does not.
TEST(NabtsRecordAssembler,
     DecodesFunctionDescriptorsOnlyForAnApplicationRecord) {
  MessageHarness application;
  application.feed(0x000,
                   minimal_header(orc::kNabtsRecordTypeApplication, 0xFFE),
                   record_data({0x0D, 0x20, 0x41, 0x0D}));
  ASSERT_EQ(application.messages().size(), 1u);
  ASSERT_EQ(application.messages()[0].functions.size(), 1u);
  EXPECT_EQ(application.messages()[0].functions[0].code_text(), "2/0");
  EXPECT_EQ(application.messages()[0].reserved_purpose,
            "Service Application Record");

  MessageHarness presentation;
  presentation.feed(
      0x000, minimal_header(orc::kNabtsRecordTypeCyclicPresentation, 0x001),
      record_data({0x0D, 0x20, 0x41, 0x0D}));
  ASSERT_EQ(presentation.messages().size(), 1u);
  EXPECT_TRUE(presentation.messages()[0].functions.empty());
}

// §5.2.2: types 0, 1 and 3 are presentation records whose data is NAPLPS; type
// 2 is the application record.
TEST(NabtsRecordType, NamesThePresentationTypes) {
  EXPECT_TRUE(
      orc::nabts_type_is_presentation(orc::kNabtsRecordTypeCyclicPresentation));
  EXPECT_TRUE(orc::nabts_type_is_presentation(
      orc::kNabtsRecordTypeNoncyclicPresentation));
  EXPECT_TRUE(orc::nabts_type_is_presentation(
      orc::kNabtsRecordTypePriorityPresentation));
  EXPECT_FALSE(
      orc::nabts_type_is_presentation(orc::kNabtsRecordTypeApplication));
  for (uint8_t type = 4; type <= 15; ++type) {
    EXPECT_FALSE(orc::nabts_type_is_presentation(type)) << "type " << type;
  }
}

}  // namespace
}  // namespace orc_unit_test
