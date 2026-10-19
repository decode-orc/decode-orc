/*
 * File:        nabts_data_group_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for NABTS data group reassembly (CEA-516 §4)
 *
 * Covers: the eight-byte group header, reassembly across every suffix length,
 * the final-non-zero-block trim, loss detection through the continuity index,
 * and the bounds that keep a noisy stream from allocating without limit.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_data_group.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "nabts_test_builders.h"

namespace orc_unit_test {
namespace {

using nabts::make_group_header;
using nabts::make_packet;
using nabts::parity;
using nabts::parity_bytes;
using orc::NabtsGroupOutcome;
using orc::NabtsSuffixKind;

/// A run of |count| distinguishable odd-parity data bytes starting at |first|.
std::vector<uint8_t> data_run(uint8_t first, size_t count) {
  std::vector<uint8_t> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    out.push_back(parity(static_cast<uint8_t>((first + i) & 0x7F)));
  }
  return out;
}

/// Collects the groups an assembler emits.
class Harness {
 public:
  Harness() {
    assembler_.set_group_callback(
        [this](const orc::NabtsDataGroup& group) { groups_.push_back(group); });
    assembler_.set_loose_run_callback(
        [this](const orc::NabtsLooseRun& run) { loose_.push_back(run); });
  }

  /// Feed one packet, decoded as the sink would decode it.
  void feed(const std::vector<uint8_t>& packet) {
    assembler_.add_packet(
        orc::nabts_decode_packet(packet.data(), packet.size()));
  }

  /**
   * @brief Feed a whole group: its synchronizing packet, then its blocks
   *
   * @param channel   Packet address (§3.2.3)
   * @param first_ci  Continuity index of the synchronizing packet, which the
   *                  standard leaves unrelated to what went before (§3.2.4)
   * @param blocks    Data block of each packet after the synchronizing one
   * @param header    Group header bytes, so a test can state a size that does
   *                  not match |blocks| and see what happens
   * @param sync_data Data block bytes of the synchronizing packet, after its
   *                  eight header bytes
   */
  void feed_group(uint16_t channel, uint8_t first_ci,
                  const std::vector<std::vector<uint8_t>>& blocks,
                  const std::vector<uint8_t>& header,
                  const std::vector<uint8_t>& sync_data,
                  NabtsSuffixKind suffix = NabtsSuffixKind::kNone) {
    std::vector<uint8_t> sync_block = header;
    sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());
    feed(make_packet(channel, first_ci, /*synchronizing=*/true, suffix,
                     sync_block));

    uint8_t continuity = first_ci;
    for (const auto& block : blocks) {
      continuity =
          static_cast<uint8_t>((continuity + 1) % orc::kNabtsContinuityModulus);
      feed(make_packet(channel, continuity, /*synchronizing=*/false, suffix,
                       block));
    }
  }

  /// Feed one packet already decoded, for a test that marks it as the sink
  /// would after the detector read it (NabtsPacket::read_under_check).
  void feed_decoded(const orc::NabtsPacket& packet) {
    assembler_.add_packet(packet);
  }

  /// A line that could have carried a packet and yielded none.
  void empty_lines(size_t count) {
    for (size_t i = 0; i < count; ++i) {
      assembler_.add_empty_line();
    }
  }

  /// A line read only to see whether data had started on it, which yielded
  /// none: one the stream has never been seen to carry data on.
  void probed_lines(size_t count) {
    for (size_t i = 0; i < count; ++i) {
      assembler_.add_empty_line(/*carries_data=*/false);
    }
  }

  const std::vector<orc::NabtsDataGroup>& groups() const { return groups_; }
  const std::vector<orc::NabtsLooseRun>& loose() const { return loose_; }
  const orc::NabtsGroupStats& stats() const { return assembler_.stats(); }
  void flush() { assembler_.flush(); }

 private:
  orc::NabtsGroupAssembler assembler_;
  std::vector<orc::NabtsDataGroup> groups_;
  std::vector<orc::NabtsLooseRun> loose_;
};

////////////////////////////////////////////////////////////////////////////////////////////
// The group header
////////////////////////////////////////////////////////////////////////////////////////////

// §4.2.5 and §4.2.6 concatenate each two-byte field with the first byte the
// more significant, giving a number 0 through 255.
TEST(NabtsGroupHeader, ConcatenatesTheSizeAndFinalBlockBytePairs) {
  const auto header = make_group_header(/*type=*/0, /*further_blocks=*/0x43,
                                        /*final_block_bytes=*/0x1C,
                                        /*continuity=*/5, /*repetition=*/2,
                                        /*routing=*/9);
  const auto decoded =
      orc::nabts_decode_group_header(header.data(), header.size());

  ASSERT_TRUE(decoded.valid);
  EXPECT_EQ(decoded.type, 0u);
  EXPECT_EQ(decoded.continuity, 5u);
  EXPECT_EQ(decoded.repetition, 2u);
  EXPECT_EQ(decoded.further_blocks, 0x43u);
  EXPECT_EQ(decoded.final_block_bytes, 0x1Cu);
  EXPECT_EQ(decoded.routing, 9u);
}

// §4.2.1 makes all eight bytes Hamming 8/4, so one uncorrectable byte leaves
// the group with no known extent and no known type: the header is
// all-or-nothing.
TEST(NabtsGroupHeader, RejectsTheWholeHeaderOnAnUncorrectableByte) {
  const auto clean = make_group_header(0, 3, 28);
  for (size_t byte = 0; byte < orc::kNabtsGroupHeaderBytes; ++byte) {
    auto damaged = clean;
    damaged[byte] = static_cast<uint8_t>(damaged[byte] ^ 0x03);
    EXPECT_FALSE(
        orc::nabts_decode_group_header(damaged.data(), damaged.size()).valid)
        << "byte " << byte;
  }
}

// A byte one bit wrong still decodes, to the value it was sent as — but a byte
// three bits wrong decodes too, to a neighbouring value, and nothing about the
// result says which happened. Only a header that arrived as codewords vouches
// for what it says.
TEST(NabtsGroupHeader, IsAttestedOnlyWhenEveryByteArrivedAsACodeword) {
  const auto clean = make_group_header(orc::kNabtsPrivateGroupType, 3, 20);
  const auto header =
      orc::nabts_decode_group_header(clean.data(), clean.size());
  ASSERT_TRUE(header.valid);
  EXPECT_TRUE(header.attested);

  auto corrected = clean;
  corrected[7] = static_cast<uint8_t>(corrected[7] ^ 0x01);  // one bit
  const auto repaired =
      orc::nabts_decode_group_header(corrected.data(), corrected.size());
  ASSERT_TRUE(repaired.valid);
  EXPECT_EQ(repaired.type, orc::kNabtsPrivateGroupType);
  EXPECT_FALSE(repaired.attested);
}

TEST(NabtsGroupHeader, RefusesAShortBuffer) {
  const auto clean = make_group_header(0, 0, 28);
  EXPECT_FALSE(
      orc::nabts_decode_group_header(clean.data(), clean.size() - 1).valid);
  EXPECT_FALSE(orc::nabts_decode_group_header(nullptr, 8).valid);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Reassembly
////////////////////////////////////////////////////////////////////////////////////////////

// A group of one packet is complete the moment it arrives: §4.2.5 counts the
// blocks *after* the synchronizing packet's own, so zero means there are none.
TEST(NabtsGroupAssembler, ASingleBlockGroupCompletesOnItsSynchronizingPacket) {
  Harness harness;
  const auto payload = data_run(0x41, 20);
  harness.feed_group(0x123, /*first_ci=*/4, /*blocks=*/{},
                     make_group_header(0, 0, /*final_block_bytes=*/28),
                     payload);

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(group.channel, 0x123u);
  EXPECT_EQ(group.outcome, NabtsGroupOutcome::kComplete);
  EXPECT_EQ(group.packets, 1u);
  EXPECT_EQ(group.packets_lost, 0u);
  // Whole, but sent without a suffix, so nothing vouched for its bytes.
  EXPECT_EQ(group.blocks_unchecked, 1u);
  EXPECT_FALSE(group.intact());
  // The eight header bytes are part of the block they arrived in but not part
  // of the group's data (§4.2.1, §5.1).
  EXPECT_EQ(group.data, payload);
}

// §4.1: a group is the packets of one packet address, running from its
// synchronizing packet to its last block.
TEST(NabtsGroupAssembler, ReassemblesAMultiPacketGroupInOrder) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);  // 28 - 8 header bytes
  const auto block1 = data_run(0x20, 28);
  const auto block2 = data_run(0x40, 28);
  harness.feed_group(0x000, 0, {block1, block2},
                     make_group_header(0, /*further_blocks=*/2,
                                       /*final_block_bytes=*/28),
                     sync_data);

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(group.outcome, NabtsGroupOutcome::kComplete);
  EXPECT_EQ(group.packets, 3u);

  std::vector<uint8_t> expected = sync_data;
  expected.insert(expected.end(), block1.begin(), block1.end());
  expected.insert(expected.end(), block2.begin(), block2.end());
  EXPECT_EQ(group.data, expected);
}

// Every suffix length leaves a different data-block length (§3.3), and the
// assembler has to concatenate whatever each packet actually carried.
TEST(NabtsGroupAssembler, ReassemblesAcrossEverySuffixLength) {
  struct Case {
    NabtsSuffixKind suffix;
    size_t block_bytes;
  };
  const Case cases[] = {
      {NabtsSuffixKind::kNone, 28},
      {NabtsSuffixKind::kLongitudinal, 27},
      {NabtsSuffixKind::kLongitudinalPlusReserved, 26},
  };

  for (const Case& test_case : cases) {
    Harness harness;
    const auto sync_data = data_run(0x00, test_case.block_bytes - 8);
    const auto block = data_run(0x30, test_case.block_bytes);
    harness.feed_group(
        0x001, 0, {block},
        make_group_header(0, /*further_blocks=*/1,
                          /*final_block_bytes=*/
                          static_cast<uint16_t>(test_case.block_bytes)),
        sync_data, test_case.suffix);

    ASSERT_EQ(harness.groups().size(), 1u)
        << "suffix " << static_cast<int>(test_case.suffix);
    const auto& group = harness.groups()[0];
    EXPECT_EQ(group.outcome, NabtsGroupOutcome::kComplete);
    EXPECT_EQ(group.data.size(), sync_data.size() + block.size());
    // Only a suffix vouches for a block (§3.4), so a group sent without one
    // is whole but not intact.
    EXPECT_EQ(group.intact(), test_case.suffix != NabtsSuffixKind::kNone)
        << "suffix " << static_cast<int>(test_case.suffix);
  }
}

// CEA-516 §3.4 makes the suffix the receiver's means of detecting errors in a
// data block, so a block sent without one is neither known damaged nor known
// undamaged. A group of them is never intact, which is what keeps one copy of
// a record from a service that sends no suffix from being believed over the
// vote across its other copies.
TEST(NabtsGroupAssembler, CountsBlocksSentWithoutASuffixAsUnchecked) {
  Harness harness;
  harness.feed_group(0x001, 0, {data_run(0x30, 28), data_run(0x40, 28)},
                     make_group_header(0, /*further_blocks=*/2,
                                       /*final_block_bytes=*/28),
                     data_run(0x00, 20));

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(group.outcome, NabtsGroupOutcome::kComplete);
  EXPECT_EQ(group.packets_lost, 0u);
  EXPECT_EQ(group.blocks_damaged, 0u);
  EXPECT_EQ(group.blocks_unchecked, 3u);
  EXPECT_FALSE(group.intact());
}

/// A synchronizing block: |header| then |data|.
std::vector<uint8_t> sync_block(std::vector<uint8_t> header,
                                const std::vector<uint8_t>& data) {
  header.insert(header.end(), data.begin(), data.end());
  return header;
}

/// |packet| with |mask| exclusive-ored into data-block byte |index|.
std::vector<uint8_t> damage(std::vector<uint8_t> packet, size_t index,
                            uint8_t mask) {
  packet[orc::kNabtsPrefixBytes + index] =
      static_cast<uint8_t>(packet[orc::kNabtsPrefixBytes + index] ^ mask);
  return packet;
}

// What the suffix check made of each block goes with the block's bytes: vouched
// for where it passed as received or the product code put its one wrong bit
// right, merely arrived where it failed (CEA-516 §3.4). The vote reads it.
TEST(NabtsGroupAssembler, MarksEachByteWithWhatTheSuffixCheckMadeOfItsBlock) {
  Harness harness;
  constexpr auto kSuffix = NabtsSuffixKind::kLongitudinal;
  harness.feed(
      make_packet(0x001, 0, /*synchronizing=*/true, kSuffix,
                  sync_block(make_group_header(0, /*further_blocks=*/2,
                                               /*final_block_bytes=*/27),
                             data_run(0x00, 19))));
  // One wrong bit: the product code finds it and puts it right.
  harness.feed(damage(make_packet(0x001, 1, /*synchronizing=*/false, kSuffix,
                                  data_run(0x30, 27)),
                      3, 0x10));
  // Two in one byte: parity passes them, and the check can only say so.
  harness.feed(damage(make_packet(0x001, 2, /*synchronizing=*/false, kSuffix,
                                  data_run(0x50, 27)),
                      3, 0x03));

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(group.blocks_corrected, 1u);
  EXPECT_EQ(group.blocks_damaged, 1u);
  ASSERT_EQ(group.present.size(), 19u + 27u + 27u);
  for (size_t i = 0; i < group.present.size(); ++i) {
    EXPECT_EQ(group.present[i],
              i < 46 ? orc::kNabtsByteVouched : orc::kNabtsByteArrived)
        << "byte " << i;
  }
}

// A block the detector read again under its own check passes that check
// because it was read to (NabtsPacket::read_under_check). Its bytes stay
// vouched for, so the vote still believes them over copies the check condemns,
// but the copy holding it is not one that arrived whole: the record catalogue
// reads an intact copy's length as its record's, and a copy that lost a packet
// without a gap in the continuity index shows nothing else.
TEST(NabtsGroupAssembler, ABlockReadUnderItsCheckIsVouchedButNotIntact) {
  constexpr auto kSuffix = NabtsSuffixKind::kLongitudinal;
  const auto sync =
      make_packet(0x001, 0, /*synchronizing=*/true, kSuffix,
                  sync_block(make_group_header(0, /*further_blocks=*/1,
                                               /*final_block_bytes=*/27),
                             data_run(0x00, 19)));
  const auto block = make_packet(0x001, 1, /*synchronizing=*/false, kSuffix,
                                 data_run(0x30, 27));
  for (const bool reread : {false, true}) {
    Harness harness;
    harness.feed(sync);
    auto packet = orc::nabts_decode_packet(block.data(), block.size());
    packet.read_under_check = reread;
    harness.feed_decoded(packet);

    ASSERT_EQ(harness.groups().size(), 1u) << "read again " << reread;
    const auto& group = harness.groups()[0];
    EXPECT_EQ(group.blocks_damaged, 0u);
    EXPECT_EQ(group.blocks_read_under_check, reread ? 1u : 0u);
    EXPECT_EQ(group.intact(), !reread);
    ASSERT_EQ(group.present.size(), 19u + 27u);
    for (size_t i = 0; i < group.present.size(); ++i) {
      EXPECT_EQ(group.present[i], orc::kNabtsByteVouched)
          << "byte " << i << ", read again " << reread;
    }
  }
}

// The bytes the detector's wide reading changed (NabtsPacket::reread) pass
// their parity, and their block its check, because the reading was taken for
// passing more of them: the mask marks them kNabtsByteReread, which the record
// layer reads as no evidence either way, and the rest of the block keeps what
// its check made of it. A block the wide reading changed is one read under its
// check, so the group is not intact.
TEST(NabtsGroupAssembler, BytesTheWideReadingChangedAreMarkedRereadNotVouched) {
  constexpr auto kSuffix = NabtsSuffixKind::kLongitudinal;
  const auto sync =
      make_packet(0x001, 0, /*synchronizing=*/true, kSuffix,
                  sync_block(make_group_header(0, /*further_blocks=*/1,
                                               /*final_block_bytes=*/27),
                             data_run(0x00, 19)));
  const auto block = make_packet(0x001, 1, /*synchronizing=*/false, kSuffix,
                                 data_run(0x30, 27));
  Harness harness;
  harness.feed(sync);
  auto packet = orc::nabts_decode_packet(block.data(), block.size());
  packet.reread = (1u << 3) | (1u << 20);
  harness.feed_decoded(packet);

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(group.blocks_damaged, 0u);
  EXPECT_EQ(group.blocks_read_under_check, 1u);
  EXPECT_FALSE(group.intact());
  ASSERT_EQ(group.present.size(), 19u + 27u);
  for (size_t i = 0; i < group.present.size(); ++i) {
    const bool changed = i == 19 + 3 || i == 19 + 20;
    EXPECT_EQ(group.present[i],
              changed ? orc::kNabtsByteReread : orc::kNabtsByteVouched)
        << "byte " << i;
  }
}

// A longitudinal parity that passes says only that each bit position went wrong
// an even number of times, which two wrong bits down one column do. §3.3's odd
// parity, which a type-zero group's bytes carry, still sees each byte they fell
// in — so the block is damage, and no copy holding it is intact. A private-use
// group's bytes carry no parity to see it by.
TEST(NabtsGroupAssembler,
     AClearLongitudinalCheckWithBytesFailingParityIsDamage) {
  for (const uint8_t type :
       {orc::kNabtsBroadcastGroupType, orc::kNabtsPrivateGroupType}) {
    Harness harness;
    constexpr auto kSuffix = NabtsSuffixKind::kLongitudinal;
    harness.feed(
        make_packet(0x001, 0, /*synchronizing=*/true, kSuffix,
                    sync_block(make_group_header(type, /*further_blocks=*/1,
                                                 /*final_block_bytes=*/27),
                               data_run(0x00, 19))));
    harness.feed(damage(damage(make_packet(0x001, 1, /*synchronizing=*/false,
                                           kSuffix, data_run(0x30, 27)),
                               3, 0x01),
                        4, 0x01));

    ASSERT_EQ(harness.groups().size(), 1u);
    const auto& group = harness.groups()[0];
    const bool teletext = type == orc::kNabtsBroadcastGroupType;
    EXPECT_EQ(group.blocks_damaged, teletext ? 1u : 0u) << "type " << +type;
    EXPECT_EQ(group.intact(), !teletext) << "type " << +type;
    ASSERT_EQ(group.present.size(), 19u + 27u);
    EXPECT_EQ(group.present.back(),
              teletext ? orc::kNabtsByteArrived : orc::kNabtsByteVouched)
        << "type " << +type;
  }
}

// §4.2.6: F1,F2 is the useful length of the final non-zero data block, so a
// group whose last packet was padded out yields only the bytes it promised.
TEST(NabtsGroupAssembler, TrimsTheFinalBlockToItsStatedLength) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  const auto block = data_run(0x30, 28);
  harness.feed_group(0x000, 0, {block},
                     make_group_header(0, /*further_blocks=*/1,
                                       /*final_block_bytes=*/10),
                     sync_data);

  ASSERT_EQ(harness.groups().size(), 1u);
  // 20 bytes from the synchronizing packet plus the 10 useful bytes of the
  // final block; the 18 padding bytes after them are dropped.
  EXPECT_EQ(harness.groups()[0].data.size(), sync_data.size() + 10);
}

// §8.4.2.6: a final-non-zero-block size greater than a block is read as full.
TEST(NabtsGroupAssembler, ReadsAnOversizedFinalBlockSizeAsFull) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  const auto block = data_run(0x30, 28);
  harness.feed_group(0x000, 0, {block},
                     make_group_header(0, 1, /*final_block_bytes=*/200),
                     sync_data);

  ASSERT_EQ(harness.groups().size(), 1u);
  EXPECT_EQ(harness.groups()[0].data.size(), sync_data.size() + block.size());
}

// §8.4.2.6: a final-non-zero-block size of zero has the receiver ignore that
// block entirely.
TEST(NabtsGroupAssembler, DiscardsTheFinalBlockWhenItsStatedSizeIsZero) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  const auto block = data_run(0x30, 28);
  harness.feed_group(0x000, 0, {block},
                     make_group_header(0, 1, /*final_block_bytes=*/0),
                     sync_data);

  ASSERT_EQ(harness.groups().size(), 1u);
  EXPECT_EQ(harness.groups()[0].data, sync_data);
}

// §4.2.5 counts blocks of zero length towards the group size, and §3.4 has the
// bundle packet's continuity index maintained like any other's — so a bundle
// packet advances the group without contributing bytes, and the final non-zero
// block may sit before it (§4.2.6).
TEST(NabtsGroupAssembler, ABundlePacketAdvancesTheGroupWithoutAddingBytes) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  const auto block = data_run(0x30, 28);

  std::vector<uint8_t> sync_block =
      make_group_header(0, /*further_blocks=*/2, /*final_block_bytes=*/28);
  sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());
  harness.feed(make_packet(0x000, 0, /*synchronizing=*/true,
                           NabtsSuffixKind::kNone, sync_block));
  harness.feed(make_packet(0x000, 1, /*synchronizing=*/false,
                           NabtsSuffixKind::kNone, block));
  harness.feed(make_packet(0x000, 2, /*synchronizing=*/false,
                           NabtsSuffixKind::kBundle, {}));

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(group.outcome, NabtsGroupOutcome::kComplete);
  EXPECT_EQ(group.packets, 3u);
  EXPECT_EQ(group.data.size(), sync_data.size() + block.size());
  EXPECT_EQ(harness.stats().bundle_packets, 1u);
  // The bundle packet carries no block for a suffix to vouch for, so only the
  // two blocks sent without one are unchecked.
  EXPECT_EQ(group.blocks_unchecked, 2u);
}

// §4.1: a group is per packet address, so two channels sending at once do not
// contaminate each other.
TEST(NabtsGroupAssembler, KeepsInterleavedChannelsApart) {
  Harness harness;
  const auto a_sync = data_run(0x00, 20);
  const auto a_block = data_run(0x20, 28);
  const auto b_sync = data_run(0x50, 20);
  const auto b_block = data_run(0x60, 28);

  std::vector<uint8_t> a_head = make_group_header(0, 1, 28);
  a_head.insert(a_head.end(), a_sync.begin(), a_sync.end());
  std::vector<uint8_t> b_head = make_group_header(0, 1, 28);
  b_head.insert(b_head.end(), b_sync.begin(), b_sync.end());

  // Interleaved: A's header, B's header, A's block, B's block.
  harness.feed(make_packet(0x100, 0, true, NabtsSuffixKind::kNone, a_head));
  harness.feed(make_packet(0x200, 5, true, NabtsSuffixKind::kNone, b_head));
  harness.feed(make_packet(0x100, 1, false, NabtsSuffixKind::kNone, a_block));
  harness.feed(make_packet(0x200, 6, false, NabtsSuffixKind::kNone, b_block));

  ASSERT_EQ(harness.groups().size(), 2u);
  EXPECT_EQ(harness.groups()[0].channel, 0x100u);
  EXPECT_EQ(harness.groups()[1].channel, 0x200u);

  std::vector<uint8_t> expected_a = a_sync;
  expected_a.insert(expected_a.end(), a_block.begin(), a_block.end());
  EXPECT_EQ(harness.groups()[0].data, expected_a);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Loss
////////////////////////////////////////////////////////////////////////////////////////////

// §3.2.4: the continuity index increments once per packet of a channel, so a
// gap in it is how many packets never arrived.
TEST(NabtsGroupAssembler, DetectsADroppedPacketThroughTheContinuityIndex) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  const auto block = data_run(0x30, 28);

  std::vector<uint8_t> sync_block = make_group_header(0, /*further_blocks=*/3,
                                                      /*final_block_bytes=*/28);
  sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());
  harness.feed(make_packet(0x000, 0, true, NabtsSuffixKind::kNone, sync_block));
  harness.feed(make_packet(0x000, 1, false, NabtsSuffixKind::kNone, block));
  // Continuity index 2 never arrived: the line that carried it yielded nothing.
  harness.empty_lines(1);
  harness.feed(make_packet(0x000, 3, false, NabtsSuffixKind::kNone, block));

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(group.packets_lost, 1u);
  EXPECT_EQ(harness.stats().continuity_misreads, 0u);
  EXPECT_FALSE(group.intact());
  // The lost packet still counted towards the group's size, so the group
  // completed on the packet after it rather than hanging for one that will
  // never come.
  EXPECT_EQ(group.outcome, NabtsGroupOutcome::kComplete);
  EXPECT_EQ(group.packets, 3u);
}

// A packet cannot be lost on a line that never went by. Hamming 8/4 resolves a
// three-bit burst silently to a neighbouring codeword, so a continuity index
// can read as the previous packet's — fifteen packets lost, by the arithmetic
// — on the very next line. Believing it would open a fifteen-packet hole and
// move every byte after it; the packet is read as the one that could have
// arrived there instead.
TEST(NabtsGroupAssembler, ReadsAGapNoLinesCouldHaveCarriedAsAMisreadIndex) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  const auto first = data_run(0x30, 28);
  const auto second = data_run(0x50, 28);
  const auto third = data_run(0x70, 28);

  std::vector<uint8_t> sync_block = make_group_header(0, /*further_blocks=*/3,
                                                      /*final_block_bytes=*/28);
  sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());
  harness.feed(make_packet(0x000, 0, true, NabtsSuffixKind::kNone, sync_block));
  harness.feed(make_packet(0x000, 1, false, NabtsSuffixKind::kNone, first));
  // Index 2 arrived misread as 1, on the next line.
  harness.feed(make_packet(0x000, 1, false, NabtsSuffixKind::kNone, second));
  harness.feed(make_packet(0x000, 3, false, NabtsSuffixKind::kNone, third));

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(group.packets_lost, 0u);
  EXPECT_EQ(group.outcome, NabtsGroupOutcome::kComplete);
  EXPECT_EQ(harness.stats().continuity_misreads, 1u);

  // Every block where it was sent, and none of them a hole.
  std::vector<uint8_t> expected(sync_data.begin(), sync_data.end());
  expected.insert(expected.end(), first.begin(), first.end());
  expected.insert(expected.end(), second.begin(), second.end());
  expected.insert(expected.end(), third.begin(), third.end());
  EXPECT_EQ(group.data, expected);
  EXPECT_TRUE(group.present.empty() ||
              std::all_of(group.present.begin(), group.present.end(),
                          [](uint8_t arrived) { return arrived != 0; }));
}

// Where some loss was possible but not as much as the index claims, how many
// packets went missing cannot be told. 0xE7 corrects to index 9, seven packets
// on from the one expected, with two lines gone by: two could have been lost,
// or one, or none. Rather than guess — a wrong guess puts every byte after it
// in the wrong place — the rest of the group is left unplaced, its bytes
// marked lost, for the record's other copies and other recordings to decide.
TEST(NabtsGroupAssembler, AMisreadIndexOverAPossibleLossLeavesTheRestUnplaced) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  const auto block = data_run(0x30, 28);

  std::vector<uint8_t> sync_block = make_group_header(0, /*further_blocks=*/5,
                                                      /*final_block_bytes=*/28);
  sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());
  harness.feed(make_packet(0x000, 0, true, NabtsSuffixKind::kNone, sync_block));
  harness.feed(make_packet(0x000, 1, false, NabtsSuffixKind::kNone, block));
  harness.empty_lines(2);
  auto misread = make_packet(0x000, 4, false, NabtsSuffixKind::kNone, block);
  misread[3] = 0xE7;
  ASSERT_EQ(orc::nabts_decode_packet(misread.data(), misread.size()).continuity,
            9u);
  harness.feed(misread);
  harness.feed(make_packet(0x000, 5, false, NabtsSuffixKind::kNone, block));
  harness.flush();

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(harness.stats().continuity_misreads, 1u);
  const size_t placed = sync_data.size() + block.size();
  ASSERT_GT(group.present.size(), placed);
  for (size_t i = 0; i < placed; ++i) {
    EXPECT_NE(group.present[i], 0) << "byte " << i << " before the misread";
  }
  for (size_t i = placed; i < group.present.size(); ++i) {
    EXPECT_EQ(group.present[i], 0) << "byte " << i << " after the misread";
  }
}

// Lines that yielded nothing are what make a real loss believable: with enough
// of them gone by, the index is taken at its word however large the gap.
TEST(NabtsGroupAssembler, BelievesAGapTheLinesCouldHaveCarried) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  const auto block = data_run(0x30, 28);

  std::vector<uint8_t> sync_block = make_group_header(0, /*further_blocks=*/20,
                                                      /*final_block_bytes=*/28);
  sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());
  harness.feed(make_packet(0x000, 0, true, NabtsSuffixKind::kNone, sync_block));
  harness.feed(make_packet(0x000, 1, false, NabtsSuffixKind::kNone, block));
  harness.empty_lines(15);
  // Index 1 again: fifteen packets lost, and fifteen lines to lose them on.
  harness.feed(make_packet(0x000, 1, false, NabtsSuffixKind::kNone, block));
  harness.flush();

  ASSERT_EQ(harness.groups().size(), 1u);
  EXPECT_EQ(harness.groups()[0].packets_lost, 15u);
  EXPECT_EQ(harness.stats().continuity_misreads, 0u);
}

// The index wraps at 16 (§3.2.4), which bounds what can be detected: a loss of
// exactly one modulus reads as none. Stated as a test because it is a limit of
// the standard rather than of this code.
TEST(NabtsGroupAssembler, CannotSeeALossOfExactlyOneContinuityModulus) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  const auto block = data_run(0x30, 28);

  std::vector<uint8_t> sync_block = make_group_header(0, 1, 28);
  sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());
  harness.feed(make_packet(0x000, 0, true, NabtsSuffixKind::kNone, sync_block));
  // 16 packets later the index is back to 1, which is exactly what the next
  // packet's would have been.
  harness.feed(make_packet(0x000, 1, false, NabtsSuffixKind::kNone, block));

  ASSERT_EQ(harness.groups().size(), 1u);
  EXPECT_EQ(harness.groups()[0].packets_lost, 0u);
}

/// A group of |further_blocks| blocks after its synchronizing packet on channel
/// 000, its synchronizing packet and first block fed with continuity indices 0
/// and 1; the caller feeds what follows.
void open_group(Harness& harness, const std::vector<uint8_t>& sync_data,
                const std::vector<uint8_t>& block, uint8_t further_blocks) {
  std::vector<uint8_t> sync_block =
      make_group_header(0, further_blocks, /*final_block_bytes=*/28);
  sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());
  harness.feed(make_packet(0x000, 0, true, NabtsSuffixKind::kNone, sync_block));
  harness.feed(make_packet(0x000, 1, false, NabtsSuffixKind::kNone, block));
}

// The index wraps at 16 (§3.2.4), so it reads eighteen packets lost as two.
// Where sixteen more lines that carry data went by empty than it reads, which
// it was cannot be told, and taken at the smaller reading the next record's
// packets are placed as this one's: a card dump that lost a field of every
// frame and whole runs of frames as its tape started had seven of its pages
// were each listed a second time as a copy that ran on into the page sent after
// it. The rest of the group is left unplaced instead, its bytes marked lost, as
// after a misread index.
TEST(NabtsGroupAssembler, AGapTheIndexCouldHaveComeRoundOverIsNotPlaced) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  const auto block = data_run(0x30, 28);
  open_group(harness, sync_data, block, /*further_blocks=*/30);
  harness.empty_lines(18);
  // Index 4: two packets lost as read, eighteen as the lines allow.
  harness.feed(make_packet(0x000, 4, false, NabtsSuffixKind::kNone, block));
  harness.feed(make_packet(0x000, 5, false, NabtsSuffixKind::kNone, block));
  harness.flush();

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(harness.stats().continuity_wraps, 1u);
  EXPECT_EQ(harness.stats().continuity_misreads, 0u);
  EXPECT_FALSE(group.placed);
  const size_t placed = sync_data.size() + block.size();
  ASSERT_GT(group.present.size(), placed);
  for (size_t i = 0; i < placed; ++i) {
    EXPECT_NE(group.present[i], 0) << "byte " << i << " before the gap";
  }
  for (size_t i = placed; i < group.present.size(); ++i) {
    EXPECT_EQ(group.present[i], 0) << "byte " << i << " after the gap";
  }
}

// With fewer lines than that gone by, sixteen more could not have been lost,
// and the gap is the one the index reads.
TEST(NabtsGroupAssembler, BelievesAGapTooFewLinesWentByToComeRoundOver) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  const auto block = data_run(0x30, 28);
  open_group(harness, sync_data, block, /*further_blocks=*/30);
  harness.empty_lines(17);
  harness.feed(make_packet(0x000, 4, false, NabtsSuffixKind::kNone, block));
  harness.flush();

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(harness.stats().continuity_wraps, 0u);
  EXPECT_EQ(group.packets_lost, 2u);
  EXPECT_TRUE(group.placed);
}

// A line never seen to carry data, read only on a pass's periodic look at the
// whole window, lost no group's packet: it bounds a gap like any line, but
// does not make one the index could have come round over.
TEST(NabtsGroupAssembler, LinesThatNeverCarriedDataDoNotMakeAGapComeRound) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  const auto block = data_run(0x30, 28);
  open_group(harness, sync_data, block, /*further_blocks=*/30);
  harness.empty_lines(2);
  harness.probed_lines(20);
  harness.feed(make_packet(0x000, 4, false, NabtsSuffixKind::kNone, block));
  harness.flush();

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(harness.stats().continuity_wraps, 0u);
  EXPECT_EQ(group.packets_lost, 2u);
  EXPECT_TRUE(group.placed);
}

// A channel whose packets come seldom — an encoder's idle fill between other
// services' packets, ten lines apart — has taken a tenth of the lines that
// carry data, so twenty empty lines account for two of its packets, not
// eighteen, and a gap the index reads as none is believed.
TEST(NabtsGroupAssembler, ASeldomSentChannelsEmptyLinesAreNotAllItsLosses) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  const auto block = data_run(0x30, 28);
  std::vector<uint8_t> sync_block =
      make_group_header(0, /*further_blocks=*/10, /*final_block_bytes=*/28);
  sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());
  harness.feed(make_packet(0x000, 0, true, NabtsSuffixKind::kNone, sync_block));
  for (uint8_t ci = 1; ci <= 3; ++ci) {
    harness.empty_lines(9);
    harness.feed(make_packet(0x000, ci, false, NabtsSuffixKind::kNone, block));
  }
  harness.empty_lines(20);
  harness.feed(make_packet(0x000, 4, false, NabtsSuffixKind::kNone, block));
  harness.flush();

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(harness.stats().continuity_wraps, 0u);
  EXPECT_EQ(group.packets_lost, 0u);
  EXPECT_TRUE(group.placed);
}

// A group whose every block passed its check, but which could not place the
// bytes after a gap, did not arrive whole: the record catalogue reads an
// intact copy's length as its record's and believes it over the vote.
TEST(NabtsGroupAssembler, AGroupThatCouldNotPlaceItsBytesIsNotIntact) {
  constexpr auto kSuffix = NabtsSuffixKind::kLongitudinal;
  for (const size_t silent : {size_t{0}, size_t{16}}) {
    Harness harness;
    harness.feed(
        make_packet(0x001, 0, /*synchronizing=*/true, kSuffix,
                    sync_block(make_group_header(0, /*further_blocks=*/2,
                                                 /*final_block_bytes=*/27),
                               data_run(0x00, 19))));
    harness.feed(make_packet(0x001, 1, /*synchronizing=*/false, kSuffix,
                             data_run(0x30, 27)));
    harness.empty_lines(silent);
    harness.feed(make_packet(0x001, 2, /*synchronizing=*/false, kSuffix,
                             data_run(0x50, 27)));

    ASSERT_EQ(harness.groups().size(), 1u) << silent << " lines";
    const auto& group = harness.groups()[0];
    EXPECT_EQ(group.outcome, NabtsGroupOutcome::kComplete);
    EXPECT_EQ(group.packets_lost, 0u);
    EXPECT_EQ(group.placed, silent == 0) << silent << " lines";
    EXPECT_EQ(group.intact(), silent == 0) << silent << " lines";
  }
}

// A group's suffix failures are its own, and a group carrying a damaged block
// is still delivered — the bytes that did arrive are worth having.
TEST(NabtsGroupAssembler, CountsCorrectedAndDamagedBlocksPerGroup) {
  Harness harness;
  const auto sync_data = data_run(0x00, 19);  // 27 - 8 header bytes
  auto corrected = make_packet(0x000, 1, false, NabtsSuffixKind::kLongitudinal,
                               data_run(0x30, 27));
  corrected[orc::kNabtsPrefixBytes] ^= 0x01;  // one bit: repairable
  auto damaged = make_packet(0x000, 2, false, NabtsSuffixKind::kLongitudinal,
                             data_run(0x50, 27));
  damaged[orc::kNabtsPrefixBytes + 0] ^= 0x01;  // two bits, two bytes: not
  damaged[orc::kNabtsPrefixBytes + 1] ^= 0x02;

  std::vector<uint8_t> sync_block = make_group_header(0, 2, 27);
  sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());
  harness.feed(
      make_packet(0x000, 0, true, NabtsSuffixKind::kLongitudinal, sync_block));
  harness.feed(corrected);
  harness.feed(damaged);

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(group.blocks_corrected, 1u);
  EXPECT_EQ(group.blocks_damaged, 1u);
  EXPECT_FALSE(group.intact());
  EXPECT_EQ(group.outcome, NabtsGroupOutcome::kComplete);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Ends and bounds
////////////////////////////////////////////////////////////////////////////////////////////

// §4.1: the start of a group is the end of the one before it on that channel.
// The abandoned group is still delivered — a truncated record header identifies
// its record — at the length its header declared, the five blocks it never
// received held open as a hole. (It came out as the synchronizing packet's
// bytes alone before: see AGroupEndedShortOfItsBlockCountKeepsItsLength.)
TEST(NabtsGroupAssembler, ASecondSynchronizingPacketSupersedesTheOpenGroup) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  std::vector<uint8_t> sync_block = make_group_header(0, /*further_blocks=*/5,
                                                      /*final_block_bytes=*/28);
  sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());

  harness.feed(make_packet(0x000, 0, true, NabtsSuffixKind::kNone, sync_block));
  harness.feed(make_packet(0x000, 1, true, NabtsSuffixKind::kNone, sync_block));

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(group.outcome, NabtsGroupOutcome::kSuperseded);
  ASSERT_EQ(group.data.size(), sync_data.size() + 5 * 28);
  EXPECT_TRUE(
      std::equal(sync_data.begin(), sync_data.end(), group.data.begin()));
  ASSERT_EQ(group.present.size(), group.data.size());
  for (size_t i = 0; i < group.present.size(); ++i) {
    EXPECT_EQ(group.present[i] != 0, i < sync_data.size()) << "byte " << i;
  }
  EXPECT_EQ(harness.stats().groups_superseded, 1u);
}

// The packets a group loses last are the commonest loss of all, and no later
// packet of the group is there to carry a continuity index past them. The
// header says how many blocks there were (§4.2.5), so the group is held at that
// length, and F1,F2 still trims its final block (§4.2.6): the copy lines up
// with every other copy of its record, and a link after it in a linked series
// starts where it should.
TEST(NabtsGroupAssembler, AGroupEndedShortOfItsBlockCountKeepsItsLength) {
  for (const bool flushed : {false, true}) {
    Harness harness;
    const auto sync_data = data_run(0x00, 20);
    const auto block = data_run(0x30, 28);
    std::vector<uint8_t> sync_block =
        make_group_header(0, /*further_blocks=*/3, /*final_block_bytes=*/10);
    sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());
    harness.feed(
        make_packet(0x000, 0, true, NabtsSuffixKind::kNone, sync_block));
    harness.feed(make_packet(0x000, 1, false, NabtsSuffixKind::kNone, block));
    if (flushed) {
      harness.flush();
    } else {
      harness.feed(
          make_packet(0x000, 2, true, NabtsSuffixKind::kNone, sync_block));
    }

    ASSERT_EQ(harness.groups().size(), 1u);
    const auto& group = harness.groups()[0];
    EXPECT_EQ(group.outcome, flushed ? NabtsGroupOutcome::kUnfinished
                                     : NabtsGroupOutcome::kSuperseded);
    // 20 bytes after the header, a block of 28, a lost block of 28, and the
    // lost final block's 10 useful bytes.
    ASSERT_EQ(group.data.size(), 20u + 28u + 28u + 10u);
    ASSERT_EQ(group.present.size(), group.data.size());
    for (size_t i = 0; i < group.present.size(); ++i) {
      EXPECT_EQ(group.present[i] != 0, i < 48) << "byte " << i;
    }
    EXPECT_FALSE(group.intact());
  }
}

// A packet structure byte is Hamming 8/4 like the rest of the prefix (§3.2.5),
// and a burst can mis-correct its suffix bits. A block read at the width the
// misread byte claims moves every byte after it in the group, so it is read at
// the width the group's synchronizing packet has instead — and the suffix check
// made over the misnamed layout vouches for nothing.
TEST(NabtsGroupAssembler, ReadsABlockWhosePsByteWasMisreadAtTheGroupsWidth) {
  Harness harness;
  constexpr auto kSuffix = NabtsSuffixKind::kLongitudinal;
  const auto sync_data = data_run(0x00, 19);
  const auto first = data_run(0x30, 27);
  const auto second = data_run(0x50, 27);
  harness.feed(
      make_packet(0x001, 0, /*synchronizing=*/true, kSuffix,
                  sync_block(make_group_header(0, /*further_blocks=*/2,
                                               /*final_block_bytes=*/27),
                             sync_data)));
  // Sent with a one-byte suffix, received with a PS byte saying none.
  auto misread = make_packet(0x001, 1, /*synchronizing=*/false, kSuffix, first);
  misread[4] = nabts::hamming(nabts::packet_structure(
      /*synchronizing=*/false, /*not_full=*/false, NabtsSuffixKind::kNone));
  ASSERT_EQ(
      orc::nabts_decode_packet(misread.data(), misread.size()).data_length,
      28u);
  harness.feed(misread);
  harness.feed(make_packet(0x001, 2, /*synchronizing=*/false, kSuffix, second));

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  std::vector<uint8_t> expected = sync_data;
  expected.insert(expected.end(), first.begin(), first.end());
  expected.insert(expected.end(), second.begin(), second.end());
  EXPECT_EQ(group.data, expected);
  ASSERT_EQ(group.present.size(), expected.size());
  for (size_t i = 0; i < group.present.size(); ++i) {
    EXPECT_EQ(group.present[i], i >= 19 && i < 46 ? orc::kNabtsByteArrived
                                                  : orc::kNabtsByteVouched)
        << "byte " << i;
  }
  EXPECT_EQ(group.blocks_damaged, 1u);
  EXPECT_FALSE(group.intact());
}

TEST(NabtsGroupAssembler, FlushReportsWhateverWasStillOpen) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  std::vector<uint8_t> sync_block = make_group_header(0, 5, 28);
  sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());
  harness.feed(make_packet(0x000, 0, true, NabtsSuffixKind::kNone, sync_block));

  EXPECT_TRUE(harness.groups().empty());
  harness.flush();
  ASSERT_EQ(harness.groups().size(), 1u);
  EXPECT_EQ(harness.groups()[0].outcome, NabtsGroupOutcome::kUnfinished);

  // Idempotent: a second flush has nothing left to report.
  harness.flush();
  EXPECT_EQ(harness.groups().size(), 1u);
}

// §8.4.2.5 caps a group at 67 further blocks. A larger claim is a misread
// header or a service this cannot follow, and either way it is refused before
// it can reserve the memory it asked for.
TEST(NabtsGroupAssembler,
     RefusesAGroupClaimingMoreBlocksThanTheStandardAllows) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  std::vector<uint8_t> sync_block =
      make_group_header(0, /*further_blocks=*/orc::kNabtsMaxFurtherBlocks + 1,
                        /*final_block_bytes=*/28);
  sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());
  harness.feed(make_packet(0x000, 0, true, NabtsSuffixKind::kNone, sync_block));
  harness.flush();

  EXPECT_TRUE(harness.groups().empty());
  EXPECT_EQ(harness.stats().oversized_groups, 1u);

  // The largest group the standard does allow is accepted.
  Harness allowed;
  std::vector<uint8_t> at_limit =
      make_group_header(0, orc::kNabtsMaxFurtherBlocks, 28);
  at_limit.insert(at_limit.end(), sync_data.begin(), sync_data.end());
  allowed.feed(make_packet(0x000, 0, true, NabtsSuffixKind::kNone, at_limit));
  EXPECT_EQ(allowed.stats().oversized_groups, 0u);
  allowed.flush();
  ASSERT_EQ(allowed.groups().size(), 1u);
  EXPECT_EQ(allowed.groups()[0].outcome, NabtsGroupOutcome::kUnfinished);
}

// §3.2.3 permits 4096 channels, and a misread prefix invents them. The
// open-group bound is what stops a noisy recording from opening a buffer for
// each.
TEST(NabtsGroupAssembler, RefusesToOpenMoreGroupsThanTheBoundAllows) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  std::vector<uint8_t> sync_block = make_group_header(0, /*further_blocks=*/9,
                                                      /*final_block_bytes=*/28);
  sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());

  // One more channel than may be open at once, each starting a group that never
  // finishes.
  for (size_t i = 0; i <= orc::kNabtsMaxOpenGroups; ++i) {
    harness.feed(make_packet(static_cast<uint16_t>(0x100 + i), 0, true,
                             NabtsSuffixKind::kNone, sync_block));
  }

  EXPECT_EQ(harness.stats().refused_groups, 1u);
  harness.flush();
  EXPECT_EQ(harness.groups().size(), orc::kNabtsMaxOpenGroups);
}

// A standard packet on a channel with no group open is the normal state of
// affairs at the head of a recording, which starts part way through whatever
// was being transmitted.
TEST(NabtsGroupAssembler, CountsOrphanPacketsWithoutInventingAGroupForThem) {
  Harness harness;
  harness.feed(make_packet(0x000, 3, /*synchronizing=*/false,
                           NabtsSuffixKind::kNone, data_run(0x30, 28)));

  EXPECT_TRUE(harness.groups().empty());
  EXPECT_EQ(harness.stats().orphan_packets, 1u);
}

TEST(NabtsGroupAssembler, CountsAPacketWhoseHeaderDidNotDecode) {
  Harness harness;
  auto sync_block = make_group_header(0, 1, 28);
  sync_block[3] = static_cast<uint8_t>(sync_block[3] ^ 0x03);  // two bits
  sync_block.resize(20, parity(0));
  harness.feed(make_packet(0x000, 0, true, NabtsSuffixKind::kNone, sync_block));

  EXPECT_TRUE(harness.groups().empty());
  EXPECT_EQ(harness.stats().header_failures, 1u);
}

TEST(NabtsGroupAssembler, CountsAPacketWhosePrefixDidNotDecode) {
  Harness harness;
  auto packet = make_packet(0x000, 0, false, NabtsSuffixKind::kNone, {});
  packet[0] = static_cast<uint8_t>(packet[0] ^ 0x03);  // two bits
  harness.feed(packet);

  EXPECT_TRUE(harness.groups().empty());
  EXPECT_EQ(harness.stats().prefix_failures, 1u);
  EXPECT_EQ(harness.stats().packets_seen, 1u);
}

// §4.2.2 reserves group types other than zero rather than forbidding them, so
// one is delivered and counted rather than dropped — what to do with it is the
// caller's decision.
TEST(NabtsGroupAssembler, DeliversAGroupOfANonTeletextType) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  harness.feed_group(0x000, 0, /*blocks=*/{},
                     make_group_header(orc::kNabtsPrivateGroupType, 0, 28),
                     sync_data);

  ASSERT_EQ(harness.groups().size(), 1u);
  EXPECT_EQ(harness.groups()[0].header.type, orc::kNabtsPrivateGroupType);
  EXPECT_EQ(harness.stats().groups_completed, 1u);
}

////////////////////////////////////////////////////////////////////////////////////////////
// An encoder's idle fill
////////////////////////////////////////////////////////////////////////////////////////////

/// |count| bytes of a bit-error-rate test pattern of the Norpak kind: each
/// byte's seven information bits a window on a shift register's output, moved
/// on one bit from the byte before, with odd parity.
std::vector<uint8_t> serial_pattern(size_t count, uint8_t seed) {
  // x^8 + x^6 + x^5 + x^4 + 1, a primitive register, from a non-zero seed.
  std::vector<uint8_t> bits;
  uint8_t state = seed == 0 ? 1 : seed;
  for (size_t i = 0; i < count + 7; ++i) {
    bits.push_back(static_cast<uint8_t>(state & 1U));
    const uint8_t feedback = static_cast<uint8_t>(
        ((state >> 0) ^ (state >> 2) ^ (state >> 3) ^ (state >> 4)) & 1U);
    state = static_cast<uint8_t>((state >> 1) | (feedback << 7));
  }
  std::vector<uint8_t> out;
  for (size_t i = 0; i < count; ++i) {
    uint8_t window = 0;
    for (size_t bit = 0; bit < 7; ++bit) {
      window = static_cast<uint8_t>(window | (bits[i + bit] << bit));
    }
    out.push_back(parity(window));
  }
  return out;
}

/// The blocks of a group of BERT fill: |count| blocks of 27 bytes.
std::vector<std::vector<uint8_t>> fill_blocks(size_t count) {
  std::vector<std::vector<uint8_t>> blocks;
  for (size_t i = 0; i < count; ++i) {
    blocks.push_back(serial_pattern(27, static_cast<uint8_t>(i * 7 + 1)));
  }
  return blocks;
}

// An encoder's BERT fill can arrive on F00 as groups of type zero, 66 packets
// each, which the record layer would read hundreds of records out of a tape.
// Each is withheld, and counted.
TEST(NabtsGroupAssembler, WithholdsAnEncodersIdleFill) {
  Harness harness;
  harness.feed_group(0xF00, 0, fill_blocks(65), make_group_header(0, 65, 16),
                     parity_bytes({0x0F, 0x0F, 0x0F, 0x0F}),
                     NabtsSuffixKind::kLongitudinal);

  EXPECT_TRUE(harness.groups().empty());
  EXPECT_EQ(harness.stats().groups_completed, 1u);
  EXPECT_EQ(harness.stats().idle_fill_groups, 1u);
  EXPECT_NE(harness.stats().summary().find("Idle fill:     1 groups"),
            std::string::npos)
      << harness.stats().summary();
}

TEST(NabtsGroupAssembler, DeliversTeletextBesideIdleFill) {
  Harness harness;
  harness.feed_group(0xF00, 0, fill_blocks(65), make_group_header(0, 65, 16),
                     parity_bytes({0x0F}), NabtsSuffixKind::kLongitudinal);
  harness.feed_group(0x000, 3, {data_run(0x20, 27), data_run(0x41, 27)},
                     make_group_header(0, 2, 27), data_run(0x10, 19),
                     NabtsSuffixKind::kLongitudinal);

  ASSERT_EQ(harness.groups().size(), 1u);
  EXPECT_EQ(harness.groups()[0].channel, 0x000u);
  EXPECT_EQ(harness.stats().idle_fill_groups, 1u);
}

// A fill group that lost most of its packets has too few bytes to be judged
// on. On the channel fill has been found on, it is taken for more of it.
TEST(NabtsGroupAssembler, WithholdsAShortGroupOnAChannelCarryingFill) {
  Harness harness;
  harness.feed_group(0xF00, 0, fill_blocks(65), make_group_header(0, 65, 16),
                     parity_bytes({0x0F}), NabtsSuffixKind::kLongitudinal);
  // A synchronizing packet and one block, then the next group's
  // synchronizing packet ends it.
  harness.feed_group(0xF00, 2, fill_blocks(1), make_group_header(0, 65, 16),
                     parity_bytes({0x0F}), NabtsSuffixKind::kLongitudinal);
  harness.feed_group(0xF00, 4, fill_blocks(65), make_group_header(0, 65, 16),
                     parity_bytes({0x0F}), NabtsSuffixKind::kLongitudinal);

  EXPECT_TRUE(harness.groups().empty());
  EXPECT_EQ(harness.stats().groups_superseded, 1u);
  EXPECT_EQ(harness.stats().idle_fill_groups, 3u);
}

// Too short to judge, on a channel no fill has been seen on: delivered.
TEST(NabtsGroupAssembler, DeliversAShortGroupWhereNoFillHasBeenSeen) {
  Harness harness;
  harness.feed_group(0xF00, 0, fill_blocks(1), make_group_header(0, 1, 27),
                     parity_bytes({0x0F}), NabtsSuffixKind::kLongitudinal);

  ASSERT_EQ(harness.groups().size(), 1u);
  EXPECT_EQ(harness.stats().idle_fill_groups, 0u);
}

// Bytes a lost packet stands in for are zeros, and judge nothing: a group of
// teletext with a hole in it is not fill, and a fill group is fill over the
// bytes that did arrive.
TEST(NabtsGroupAssembler, JudgesIdleFillOnTheBytesThatArrived) {
  orc::NabtsDataGroup fill;
  fill.data = serial_pattern(200, 9);
  fill.present.assign(fill.data.size(), orc::kNabtsByteVouched);
  for (size_t i = 100; i < 150; ++i) {
    fill.data[i] = 0;
    fill.present[i] = orc::kNabtsByteLost;
  }
  EXPECT_TRUE(orc::nabts_group_is_idle_fill(fill));

  orc::NabtsDataGroup teletext;
  teletext.data = data_run(0x20, 90);
  EXPECT_FALSE(orc::nabts_group_is_idle_fill(teletext));
  EXPECT_GE(orc::nabts_group_serial_evidence(teletext).pairs, 64u);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Misreads a damaged recording makes of the packet and group headers
////////////////////////////////////////////////////////////////////////////////////////////

// A data packet whose PS byte was misread as synchronizing (§3.2.5 b2) offers
// its first eight data bytes as a group header, which does not decode. It
// carries the continuity index the open group expects next, so it is that
// group's block rather than a refused group.
TEST(NabtsGroupAssembler, TakesADataPacketMisreadAsSynchronizingIntoItsGroup) {
  Harness harness;
  ASSERT_FALSE(
      orc::nabts_decode_group_header(data_run(0x41, 28).data(), 28).valid);
  harness.feed(
      make_packet(0x000, 5, /*synchronizing=*/true, NabtsSuffixKind::kNone, [] {
        std::vector<uint8_t> block = make_group_header(0, 2, 28);
        const auto data = data_run(0x20, 20);
        block.insert(block.end(), data.begin(), data.end());
        return block;
      }()));
  harness.feed(make_packet(0x000, 6, /*synchronizing=*/true,
                           NabtsSuffixKind::kNone, data_run(0x41, 28)));
  harness.feed(make_packet(0x000, 7, /*synchronizing=*/false,
                           NabtsSuffixKind::kNone, data_run(0x61, 28)));

  ASSERT_EQ(harness.groups().size(), 1u);
  const orc::NabtsDataGroup& group = harness.groups()[0];
  EXPECT_EQ(group.outcome, NabtsGroupOutcome::kComplete);
  EXPECT_EQ(group.packets_lost, 0u);
  EXPECT_EQ(harness.stats().synchronizing_misreads, 1u);
  EXPECT_EQ(harness.stats().header_failures, 0u);
  ASSERT_EQ(group.data.size(), 20u + 28u + 28u);
  const auto misread = data_run(0x41, 28);
  EXPECT_TRUE(
      std::equal(misread.begin(), misread.end(), group.data.begin() + 20));
}

// With no group open to take it, the packet is refused as it always was.
TEST(NabtsGroupAssembler, RefusesAnUndecodableHeaderNoOpenGroupExpects) {
  Harness harness;
  harness.feed(make_packet(0x000, 6, /*synchronizing=*/true,
                           NabtsSuffixKind::kNone, data_run(0x41, 28)));
  harness.flush();
  EXPECT_TRUE(harness.groups().empty());
  EXPECT_EQ(harness.stats().header_failures, 1u);
  EXPECT_EQ(harness.stats().synchronizing_misreads, 0u);
}

// §4.2.2: a teletext channel carries type zero. A type byte that had to be
// corrected into another value there is a codeword a burst moved, and is
// read as zero; one that arrived as a codeword is believed.
TEST(NabtsGroupAssembler, ReadsACorrectedTypeOnATeletextChannelAsZero) {
  Harness harness;
  harness.feed_group(0x000, 0, {data_run(0x30, 28)},
                     make_group_header(0, 1, 28), data_run(0x20, 20));
  std::vector<uint8_t> corrected = make_group_header(0xB, 1, 28);
  corrected[0] ^= 0x01;  // one bit wrong: corrected back to 0xB
  harness.feed_group(0x000, 2, {data_run(0x50, 28)}, corrected,
                     data_run(0x40, 20));
  harness.feed_group(0x000, 4, {data_run(0x50, 28)},
                     make_group_header(0xB, 1, 28), data_run(0x40, 20));

  ASSERT_EQ(harness.groups().size(), 3u);
  EXPECT_EQ(harness.groups()[1].header.type, 0u);
  EXPECT_EQ(harness.groups()[2].header.type, 0xBu)
      << "a type that arrived as a codeword is the service's";
  EXPECT_EQ(harness.stats().type_misreads, 1u);
}

// Until the channel has carried teletext, a corrected type is left as read.
TEST(NabtsGroupAssembler, LeavesACorrectedTypeOnAChannelNotKnownForTeletext) {
  Harness harness;
  std::vector<uint8_t> corrected = make_group_header(0xB, 1, 28);
  corrected[0] ^= 0x01;
  harness.feed_group(0x000, 2, {data_run(0x50, 28)}, corrected,
                     data_run(0x40, 20));

  ASSERT_EQ(harness.groups().size(), 1u);
  EXPECT_EQ(harness.groups()[0].header.type, 0xBu);
  EXPECT_EQ(harness.stats().type_misreads, 0u);
}

////////////////////////////////////////////////////////////////////////////////////////////
// Loose runs: a teletext channel's packets with no group open
////////////////////////////////////////////////////////////////////////////////////////////

// A group whose synchronizing packet was lost arrives as packets with no group
// open. On a channel known to carry teletext they are handed on as one run,
// with a hole held open where the continuity index says a packet was lost, and
// the run ends where the next group begins.
TEST(NabtsGroupAssembler, HandsOnAGroupWhoseStartWasLostAsALooseRun) {
  Harness harness;
  harness.feed_group(0x000, 0, {data_run(0x30, 28)},
                     make_group_header(0, 1, 28), data_run(0x20, 20));
  harness.empty_lines(1);  // the lost synchronizing packet's line
  harness.feed(make_packet(0x000, 3, /*synchronizing=*/false,
                           NabtsSuffixKind::kNone, data_run(0x41, 28)));
  harness.feed(make_packet(0x000, 4, /*synchronizing=*/false,
                           NabtsSuffixKind::kNone, data_run(0x51, 28)));
  harness.empty_lines(1);  // a packet lost inside the run
  harness.feed(make_packet(0x000, 6, /*synchronizing=*/false,
                           NabtsSuffixKind::kNone, data_run(0x61, 28)));
  harness.feed_group(0x000, 7, {data_run(0x30, 28)},
                     make_group_header(0, 1, 28), data_run(0x20, 20));

  ASSERT_EQ(harness.loose().size(), 1u);
  const orc::NabtsLooseRun& run = harness.loose()[0];
  EXPECT_EQ(run.channel, 0x000u);
  EXPECT_EQ(run.packets, 3u);
  EXPECT_EQ(run.first_continuity, 3u);
  EXPECT_FALSE(run.starts_synchronizing);
  ASSERT_EQ(run.data.size(), 4u * 28u);
  const auto first = data_run(0x41, 28);
  EXPECT_TRUE(std::equal(first.begin(), first.end(), run.data.begin()));
  for (size_t i = 56; i < 84; ++i) {
    EXPECT_EQ(run.present[i], orc::kNabtsByteLost) << "byte " << i;
  }
  const auto last = data_run(0x61, 28);
  EXPECT_TRUE(std::equal(last.begin(), last.end(), run.data.begin() + 84));
  EXPECT_EQ(harness.stats().loose_runs, 1u);
  EXPECT_EQ(harness.stats().loose_packets, 3u);
}

// Only a channel that has carried teletext is collected: packets on a channel
// no group of type zero arrived on are a misread address or another service.
TEST(NabtsGroupAssembler, CollectsNoLooseRunOnAChannelNotKnownForTeletext) {
  Harness harness;
  harness.feed(make_packet(0x123, 3, /*synchronizing=*/false,
                           NabtsSuffixKind::kNone, data_run(0x41, 28)));
  harness.feed(make_packet(0x123, 4, /*synchronizing=*/false,
                           NabtsSuffixKind::kNone, data_run(0x51, 28)));
  harness.flush();
  EXPECT_TRUE(harness.loose().empty());
  EXPECT_EQ(harness.stats().orphan_packets, 2u);
}

// A group start whose header did not decode heads the run it starts.
TEST(NabtsGroupAssembler, StartsALooseRunAtAnUndecodableGroupStart) {
  Harness harness;
  harness.feed_group(0x000, 0, {data_run(0x30, 28)},
                     make_group_header(0, 1, 28), data_run(0x20, 20));
  harness.feed(make_packet(0x000, 9, /*synchronizing=*/true,
                           NabtsSuffixKind::kNone, data_run(0x41, 28)));
  harness.feed(make_packet(0x000, 10, /*synchronizing=*/false,
                           NabtsSuffixKind::kNone, data_run(0x51, 28)));
  harness.flush();

  ASSERT_EQ(harness.loose().size(), 1u);
  EXPECT_TRUE(harness.loose()[0].starts_synchronizing);
  EXPECT_EQ(harness.loose()[0].packets, 2u);
  EXPECT_EQ(harness.stats().header_failures, 1u);
}

}  // namespace
}  // namespace orc_unit_test
