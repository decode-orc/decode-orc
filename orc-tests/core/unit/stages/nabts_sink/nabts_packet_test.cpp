/*
 * File:        nabts_packet_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for NABTS data packet decoding (CEA-516 §3)
 *
 * Covers: the Hamming 8/4 packet prefix and what an uncorrectable byte in it
 * costs, every suffix length and the data-block extent each leaves, and the
 * product code the longitudinal parity byte and the per-byte parity bits form
 * together.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_packet.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "nabts_test_builders.h"

namespace orc_unit_test {
namespace {

using nabts::make_packet;
using nabts::parity;
using nabts::parity_bytes;
using orc::NabtsBlockIntegrity;
using orc::NabtsSuffixKind;

orc::NabtsPacket decode(const std::vector<uint8_t>& packet) {
  return orc::nabts_decode_packet(packet.data(), packet.size());
}

////////////////////////////////////////////////////////////////////////////////////////////
// The prefix
////////////////////////////////////////////////////////////////////////////////////////////

// §3.2.3 makes the packet address the data channel number, P1 the most
// significant of three hexadecimal digits.
TEST(NabtsPacket, Prefix_ReadsTheChannelAddressMostSignificantDigitFirst) {
  const auto packet = make_packet(0xA5C, 7, /*synchronizing=*/false,
                                  NabtsSuffixKind::kNone, {});
  const auto decoded = decode(packet);

  ASSERT_TRUE(decoded.valid);
  EXPECT_EQ(decoded.channel, 0xA5Cu);
  EXPECT_EQ(decoded.continuity, 7u);
}

// §3.2.5: b2 tells a synchronizing packet from a standard one, and b4 whether
// the block is full.
TEST(NabtsPacket, Prefix_ReadsTheStructureByteFlags) {
  const auto standard = decode(make_packet(0x000, 0, /*synchronizing=*/false,
                                           NabtsSuffixKind::kNone, {}));
  EXPECT_FALSE(standard.synchronizing);
  EXPECT_FALSE(standard.not_full);

  const auto sync = decode(make_packet(0x000, 0, /*synchronizing=*/true,
                                       NabtsSuffixKind::kNone, {},
                                       /*not_full=*/true));
  EXPECT_TRUE(sync.synchronizing);
  EXPECT_TRUE(sync.not_full);
}

// §3.2.2's Hamming code corrects every single-bit error, so a packet with one
// bit wrong in its prefix still decodes to the address that was transmitted.
TEST(NabtsPacket, Prefix_CorrectsASingleBitErrorInEveryByte) {
  const auto clean = make_packet(0x123, 9, /*synchronizing=*/true,
                                 NabtsSuffixKind::kLongitudinal, {});

  for (size_t byte = 0; byte < orc::kNabtsPrefixBytes; ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      auto damaged = clean;
      damaged[byte] = static_cast<uint8_t>(damaged[byte] ^ (1u << bit));
      const auto decoded = decode(damaged);

      ASSERT_TRUE(decoded.valid) << "byte " << byte << " bit " << bit;
      EXPECT_EQ(decoded.channel, 0x123u) << "byte " << byte << " bit " << bit;
      EXPECT_EQ(decoded.continuity, 9u) << "byte " << byte << " bit " << bit;
      EXPECT_TRUE(decoded.synchronizing) << "byte " << byte << " bit " << bit;
    }
  }
}

// A two-bit error is detected but not correctable (§3.2.2), and a prefix that
// did not decode says nothing at all: there is no channel to file the packet
// under and no known extent to read its data from.
TEST(NabtsPacket, Prefix_RejectsTheWholePacketOnAnUncorrectableByte) {
  const auto clean = make_packet(0x123, 0, /*synchronizing=*/false,
                                 NabtsSuffixKind::kNone, {});

  for (size_t byte = 0; byte < orc::kNabtsPrefixBytes; ++byte) {
    auto damaged = clean;
    damaged[byte] = static_cast<uint8_t>(damaged[byte] ^ 0x03);
    EXPECT_FALSE(decode(damaged).valid) << "byte " << byte;
  }
}

// The channel a mis-corrected address digit invents is indistinguishable from
// a real one downstream, so the packet has to carry whether its address was
// received or inferred (issue #267).
TEST(NabtsPacket, Prefix_AttestsAnAddressThatArrivedAsCodewords) {
  const auto clean = make_packet(0x123, 9, /*synchronizing=*/true,
                                 NabtsSuffixKind::kLongitudinal, {});
  EXPECT_TRUE(decode(clean).address_attested);

  // A corrected address byte decodes to the right channel and says so.
  for (size_t byte = 0; byte < 3; ++byte) {
    auto damaged = clean;
    damaged[byte] = static_cast<uint8_t>(damaged[byte] ^ 0x01);
    const auto decoded = decode(damaged);
    ASSERT_TRUE(decoded.valid) << "byte " << byte;
    EXPECT_EQ(decoded.channel, 0x123u) << "byte " << byte;
    EXPECT_FALSE(decoded.address_attested) << "byte " << byte;
  }
}

// A prefix whose five bytes all needed correction is how a test-signal line
// read as data comes out (the sink refuses such a packet); one clean byte is
// enough to take it for a damaged packet.
TEST(NabtsPacket, Prefix_TellsWhenEveryByteNeededCorrection) {
  const auto clean = make_packet(0x123, 9, /*synchronizing=*/true,
                                 NabtsSuffixKind::kLongitudinal, {});
  EXPECT_FALSE(orc::nabts_prefix_every_byte_corrected(clean.data()));

  auto damaged = clean;
  for (size_t byte = 0; byte < orc::kNabtsPrefixBytes; ++byte) {
    damaged[byte] = static_cast<uint8_t>(damaged[byte] ^ 0x01);
  }
  ASSERT_TRUE(decode(damaged).valid) << "every byte still corrects";
  EXPECT_TRUE(orc::nabts_prefix_every_byte_corrected(damaged.data()));

  for (size_t byte = 0; byte < orc::kNabtsPrefixBytes; ++byte) {
    auto one_clean = damaged;
    one_clean[byte] = clean[byte];
    EXPECT_FALSE(orc::nabts_prefix_every_byte_corrected(one_clean.data()))
        << "byte " << byte;
  }
}

TEST(NabtsPacket, Prefix_TellsWhenEveryByteArrivedWhole) {
  const auto clean = make_packet(0x123, 9, /*synchronizing=*/true,
                                 NabtsSuffixKind::kLongitudinal, {});
  EXPECT_TRUE(orc::nabts_prefix_every_byte_whole(clean.data()));

  for (size_t byte = 0; byte < orc::kNabtsPrefixBytes; ++byte) {
    auto one_corrected = clean;
    one_corrected[byte] = static_cast<uint8_t>(one_corrected[byte] ^ 0x01);
    ASSERT_TRUE(decode(one_corrected).valid) << "byte " << byte;
    EXPECT_FALSE(orc::nabts_prefix_every_byte_whole(one_corrected.data()))
        << "byte " << byte;
  }
}

// The share of whole prefixes as recordings measured it: at least 72 % on
// every recording of NABTS, at most 0.09 % on the lines that carry another
// signal behind its run-in and framing code, and nothing said of a recording
// with too few packets to tell.
TEST(NabtsPacket, ServiceCensus_RarelyWholePrefixesAreAnotherSignal) {
  EXPECT_FALSE(orc::nabts_lines_carry_no_nabts(35095, 25453));
  EXPECT_TRUE(orc::nabts_lines_carry_no_nabts(43772, 2));  // random bytes
  EXPECT_TRUE(orc::nabts_lines_carry_no_nabts(5658, 5));   // framing 0x36
  EXPECT_TRUE(orc::nabts_lines_carry_no_nabts(1595, 0));   // another framing

  EXPECT_FALSE(orc::nabts_lines_carry_no_nabts(30, 0)) << "too few to tell";
  EXPECT_FALSE(orc::nabts_lines_carry_no_nabts(
      orc::kNabtsServiceCensusMinimumPackets - 1, 0));
  EXPECT_TRUE(orc::nabts_lines_carry_no_nabts(
      orc::kNabtsServiceCensusMinimumPackets, 0));

  EXPECT_TRUE(orc::nabts_lines_carry_no_nabts(1000, 99));
  EXPECT_FALSE(orc::nabts_lines_carry_no_nabts(1000, 100));
}

// Only the three address bytes bear on which channel this is: the continuity
// index and the structure byte say what the packet is, not who it belongs to.
TEST(NabtsPacket, Prefix_AttestationIgnoresTheContinuityAndStructureBytes) {
  auto packet = make_packet(0x123, 9, /*synchronizing=*/true,
                            NabtsSuffixKind::kLongitudinal, {});
  packet[3] = static_cast<uint8_t>(packet[3] ^ 0x01);
  const auto decoded = decode(packet);
  ASSERT_TRUE(decoded.valid);
  EXPECT_TRUE(decoded.address_attested);
}

TEST(NabtsPacket, Prefix_RefusesAShortBuffer) {
  const auto clean = make_packet(0x000, 0, /*synchronizing=*/false,
                                 NabtsSuffixKind::kNone, {});
  EXPECT_FALSE(orc::nabts_decode_packet(clean.data(), clean.size() - 1).valid);
  EXPECT_FALSE(orc::nabts_decode_packet(nullptr, orc::kNabtsPacketBytes).valid);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Suffix length and the data block
////////////////////////////////////////////////////////////////////////////////////////////

// §3.3: the data block is zero, 26, 27 or 28 bytes, decided by the suffix
// length §3.4 codes into PS b8/b6.
TEST(NabtsPacket, Suffix_EachCodeLeavesItsOwnDataBlockLength) {
  struct Case {
    NabtsSuffixKind suffix;
    size_t data_length;
  };
  const Case cases[] = {
      {NabtsSuffixKind::kNone, 28},
      {NabtsSuffixKind::kLongitudinal, 27},
      {NabtsSuffixKind::kLongitudinalPlusReserved, 26},
      {NabtsSuffixKind::kBundle, 0},
  };

  for (const Case& test_case : cases) {
    const auto decoded = decode(
        make_packet(0x000, 0, /*synchronizing=*/false, test_case.suffix, {}));
    ASSERT_TRUE(decoded.valid);
    EXPECT_EQ(decoded.suffix, test_case.suffix);
    EXPECT_EQ(decoded.data_length, test_case.data_length);
    EXPECT_EQ(decoded.carries_data(), test_case.data_length > 0);
  }
}

TEST(NabtsPacket, DataBlock_IsReturnedAsTransmitted) {
  const auto payload = parity_bytes({0x41, 0x42, 0x43, 0x44});
  const auto decoded =
      decode(make_packet(0x000, 0, /*synchronizing=*/false,
                         NabtsSuffixKind::kLongitudinal, payload));

  ASSERT_TRUE(decoded.valid);
  ASSERT_EQ(decoded.data_length, 27u);
  for (size_t i = 0; i < payload.size(); ++i) {
    EXPECT_EQ(decoded.data[i], payload[i]) << "byte " << i;
  }
}

// The suffix comes back with the block, past its data-block length: a data
// group's packets share one suffix length, so a group can read the block of a
// packet whose PS byte was misread at the width the rest of the group has.
TEST(NabtsPacket, DataBlock_KeepsTheSuffixBytesAfterTheBlock) {
  const auto payload = parity_bytes({0x41, 0x42, 0x43, 0x44});
  const auto packet = make_packet(0x000, 0, /*synchronizing=*/false,
                                  NabtsSuffixKind::kLongitudinal, payload);
  const auto decoded = decode(packet);

  ASSERT_TRUE(decoded.valid);
  ASSERT_EQ(decoded.data_length, 27u);
  EXPECT_EQ(decoded.data[27], packet[orc::kNabtsPacketBytes - 1]);
}

// §3.4 leaves the 28-byte suffix's error protection "reserved for future
// standardization", so the packet is counted and its contents skipped — but it
// is still a valid packet, because §3.4 requires its continuity index to be
// maintained like any other's.
TEST(NabtsPacket, BundlePacket_IsValidAndCarriesNoDataBlock) {
  const auto decoded = decode(make_packet(0x0FF, 11, /*synchronizing=*/false,
                                          NabtsSuffixKind::kBundle, {}));

  ASSERT_TRUE(decoded.valid);
  EXPECT_EQ(decoded.suffix, NabtsSuffixKind::kBundle);
  EXPECT_EQ(decoded.data_length, 0u);
  EXPECT_FALSE(decoded.carries_data());
  EXPECT_EQ(decoded.continuity, 11u);
  // Nothing was checked, because there is no stated method to check it with.
  EXPECT_EQ(decoded.integrity, NabtsBlockIntegrity::kUnchecked);
}

////////////////////////////////////////////////////////////////////////////////////////////
// The suffix product code
////////////////////////////////////////////////////////////////////////////////////////////

// A packet with no suffix carries no check of its own, which §3.4's 0,0 code
// makes a legal packet rather than a fault.
TEST(NabtsPacket, Integrity_NoSuffixIsUncheckedRatherThanClean) {
  const auto decoded = decode(make_packet(0x000, 0, /*synchronizing=*/false,
                                          NabtsSuffixKind::kNone,
                                          parity_bytes({0x31, 0x32, 0x33})));
  ASSERT_TRUE(decoded.valid);
  EXPECT_EQ(decoded.integrity, NabtsBlockIntegrity::kUnchecked);
}

TEST(NabtsPacket, Integrity_AnUndamagedBlockIsClean) {
  for (const auto suffix : {NabtsSuffixKind::kLongitudinal,
                            NabtsSuffixKind::kLongitudinalPlusReserved}) {
    const auto decoded =
        decode(make_packet(0x000, 0, /*synchronizing=*/false, suffix,
                           parity_bytes({0x48, 0x65, 0x6C, 0x6C, 0x6F})));
    ASSERT_TRUE(decoded.valid);
    EXPECT_EQ(decoded.integrity, NabtsBlockIntegrity::kClean);
  }
}

// §3.4: the longitudinal byte says which bit is wrong and the per-byte parity
// says which byte, so between them any single-bit error is located and put
// back. Every bit of every byte of the data block and the suffix is tried.
TEST(NabtsPacket, Integrity_CorrectsASingleBitErrorAnywhereInTheBlock) {
  const auto payload = parity_bytes({0x54, 0x65, 0x73, 0x74, 0x21});
  const auto clean = make_packet(0x000, 0, /*synchronizing=*/false,
                                 NabtsSuffixKind::kLongitudinal, payload);
  const auto expected = decode(clean);
  ASSERT_EQ(expected.integrity, NabtsBlockIntegrity::kClean);

  for (size_t byte = orc::kNabtsPrefixBytes; byte < orc::kNabtsPacketBytes;
       ++byte) {
    for (int bit = 0; bit < 8; ++bit) {
      auto damaged = clean;
      damaged[byte] = static_cast<uint8_t>(damaged[byte] ^ (1u << bit));
      const auto decoded = decode(damaged);

      ASSERT_TRUE(decoded.valid);
      EXPECT_EQ(decoded.integrity, NabtsBlockIntegrity::kCorrected)
          << "byte " << byte << " bit " << bit;
      // Repaired back to what was transmitted, whichever byte it landed in.
      for (size_t i = 0; i < decoded.data_length; ++i) {
        EXPECT_EQ(decoded.data[i], expected.data[i])
            << "byte " << byte << " bit " << bit << " data " << i;
      }
    }
  }
}

// Two bits in different bytes fail two parities, and two bits in one byte leave
// its parity intact while putting two bits in the syndrome. §3.4 promises
// detection of both and correction of neither.
TEST(NabtsPacket, Integrity_DetectsATwoBitErrorWithoutGuessing) {
  const auto payload = parity_bytes({0x54, 0x65, 0x73, 0x74});
  const auto clean = make_packet(0x000, 0, /*synchronizing=*/false,
                                 NabtsSuffixKind::kLongitudinal, payload);

  // Two bits, two different bytes.
  auto across = clean;
  across[orc::kNabtsPrefixBytes + 0] ^= 0x01;
  across[orc::kNabtsPrefixBytes + 1] ^= 0x02;
  EXPECT_EQ(decode(across).integrity, NabtsBlockIntegrity::kUncorrectable);

  // Two bits, one byte.
  auto within = clean;
  within[orc::kNabtsPrefixBytes + 2] ^= 0x03;
  EXPECT_EQ(decode(within).integrity, NabtsBlockIntegrity::kUncorrectable);
}

// An uncorrectable block is handed back as transmitted rather than withheld: a
// group missing a few bytes of one block still assembles into a record, and the
// caller is told which blocks to distrust.
TEST(NabtsPacket, Integrity_AnUncorrectableBlockIsStillReturned) {
  const auto payload = parity_bytes({0x61, 0x62, 0x63, 0x64});
  auto damaged = make_packet(0x000, 0, /*synchronizing=*/false,
                             NabtsSuffixKind::kLongitudinal, payload);
  damaged[orc::kNabtsPrefixBytes + 0] ^= 0x01;
  damaged[orc::kNabtsPrefixBytes + 1] ^= 0x02;

  const auto decoded = decode(damaged);
  ASSERT_TRUE(decoded.valid);
  EXPECT_EQ(decoded.integrity, NabtsBlockIntegrity::kUncorrectable);
  EXPECT_EQ(decoded.data[0], static_cast<uint8_t>(payload[0] ^ 0x01));
  EXPECT_EQ(decoded.data[1], static_cast<uint8_t>(payload[1] ^ 0x02));
  EXPECT_EQ(decoded.data[2], payload[2]);
}

// The one-byte suffix has the longitudinal check as the packet's last byte, and
// the two-byte suffix puts a byte "subject to further study" before it (§3.4).
// That byte is covered by the check without being read from, so an error in it
// is found and corrected like any other.
TEST(NabtsPacket, Integrity_CoversTheReservedSuffixByteOfTheTwoByteForm) {
  const auto payload = parity_bytes({0x70, 0x71, 0x72});
  auto damaged =
      make_packet(0x000, 0, /*synchronizing=*/false,
                  NabtsSuffixKind::kLongitudinalPlusReserved, payload);
  const auto expected = decode(damaged);
  ASSERT_EQ(expected.integrity, NabtsBlockIntegrity::kClean);

  damaged[orc::kNabtsPacketBytes - 2] ^= 0x10;
  const auto decoded = decode(damaged);
  EXPECT_EQ(decoded.integrity, NabtsBlockIntegrity::kCorrected);
  // The correction landed in the suffix, so the data block is untouched.
  for (size_t i = 0; i < decoded.data_length; ++i) {
    EXPECT_EQ(decoded.data[i], expected.data[i]) << "data " << i;
  }
}

////////////////////////////////////////////////////////////////////////////////////////////
// Detector confidence
////////////////////////////////////////////////////////////////////////////////////////////

// The slicer measures confidence over the whole 33-byte packet; the data block
// starts after the five prefix bytes, so the two are offset by that much.
TEST(NabtsPacket, Confidence_IsTakenFromTheDataBlockBytes) {
  const auto payload = parity_bytes({0x41, 0x42, 0x43});
  const auto packet = make_packet(0x000, 0, /*synchronizing=*/false,
                                  NabtsSuffixKind::kNone, payload);

  orc::TeletextPacketConfidence confidence{};
  confidence.fill(0.0F);
  confidence[orc::kNabtsPrefixBytes + 0] = 1.0F;
  confidence[orc::kNabtsPrefixBytes + 1] = 0.5F;
  confidence[orc::kNabtsPrefixBytes + 2] = 0.0F;

  const auto decoded =
      orc::nabts_decode_packet(packet.data(), packet.size(), &confidence);
  ASSERT_TRUE(decoded.valid);
  EXPECT_EQ(decoded.confidence[0], 255);
  EXPECT_EQ(decoded.confidence[1], 128);
  EXPECT_EQ(decoded.confidence[2], 0);
}

// The threshold detector decides each bit on one sample and has no path metric
// to compare, so it measures nothing. A detector that cannot say it is unsure
// has not said so, and its bytes carry full confidence rather than none — which
// is what stops a run without MLSE from weighing every byte at zero.
TEST(NabtsPacket, Confidence_IsFullWhereNothingMeasuredIt) {
  const auto payload = parity_bytes({0x41, 0x42, 0x43});
  const auto packet = make_packet(0x000, 0, /*synchronizing=*/false,
                                  NabtsSuffixKind::kNone, payload);

  const auto decoded = orc::nabts_decode_packet(packet.data(), packet.size());
  ASSERT_TRUE(decoded.valid);
  for (size_t i = 0; i < decoded.data_length; ++i) {
    EXPECT_EQ(decoded.confidence[i], 255) << "byte " << i;
  }
}

}  // namespace
}  // namespace orc_unit_test
