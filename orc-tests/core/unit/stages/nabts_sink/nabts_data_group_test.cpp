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
#include <vector>

#include "nabts_test_builders.h"

namespace orc_unit_test {
namespace {

using nabts::make_group_header;
using nabts::make_packet;
using nabts::parity;
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

  /// A line that could have carried a packet and yielded none.
  void empty_lines(size_t count) {
    for (size_t i = 0; i < count; ++i) {
      assembler_.add_empty_line();
    }
  }

  const std::vector<orc::NabtsDataGroup>& groups() const { return groups_; }
  const orc::NabtsGroupStats& stats() const { return assembler_.stats(); }
  void flush() { assembler_.flush(); }

 private:
  orc::NabtsGroupAssembler assembler_;
  std::vector<orc::NabtsDataGroup> groups_;
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
  EXPECT_TRUE(group.intact());
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
    EXPECT_TRUE(group.intact());
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

// Where some loss was possible but not as much as the index claims, the index
// is read as whichever possible one lies nearest the byte that was received.
// 0xE7 corrects to index 9, seven packets on from the one expected; with two
// lines gone by only indices 2 to 4 could have arrived, and 0xE7 is three bits
// from 4's codeword and five from the others'.
TEST(NabtsGroupAssembler, ReadsAnImpossibleIndexAsTheNearestPossibleOne) {
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
  // The next packet follows on from 4, not from 9.
  harness.feed(make_packet(0x000, 5, false, NabtsSuffixKind::kNone, block));

  ASSERT_EQ(harness.groups().size(), 1u);
  const auto& group = harness.groups()[0];
  EXPECT_EQ(group.packets_lost, 2u);
  EXPECT_EQ(group.outcome, NabtsGroupOutcome::kComplete);
  EXPECT_EQ(harness.stats().continuity_misreads, 1u);
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
// its record.
TEST(NabtsGroupAssembler, ASecondSynchronizingPacketSupersedesTheOpenGroup) {
  Harness harness;
  const auto sync_data = data_run(0x00, 20);
  std::vector<uint8_t> sync_block = make_group_header(0, /*further_blocks=*/5,
                                                      /*final_block_bytes=*/28);
  sync_block.insert(sync_block.end(), sync_data.begin(), sync_data.end());

  harness.feed(make_packet(0x000, 0, true, NabtsSuffixKind::kNone, sync_block));
  harness.feed(make_packet(0x000, 1, true, NabtsSuffixKind::kNone, sync_block));

  ASSERT_EQ(harness.groups().size(), 1u);
  EXPECT_EQ(harness.groups()[0].outcome, NabtsGroupOutcome::kSuperseded);
  EXPECT_EQ(harness.groups()[0].data, sync_data);
  EXPECT_EQ(harness.stats().groups_superseded, 1u);
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
  EXPECT_EQ(harness.stats().non_teletext_groups, 1u);
  EXPECT_EQ(harness.stats().groups_completed, 1u);
}

}  // namespace
}  // namespace orc_unit_test
