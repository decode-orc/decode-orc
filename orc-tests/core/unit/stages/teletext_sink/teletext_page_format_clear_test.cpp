/*
 * File:        teletext_page_format_clear_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the decoder of the data a teletext service
 *              sent on pages (EN 300 708 Page Format - Clear and - CA)
 *
 * Covers: a stream of Bundle Information and two applications read from its
 * pages, found by packet 28 and by the packets' own structure; a page sent in
 * fragments; a lost packet costing only the block it held; a page of text not
 * taken for data; a Bundle Information whose checksum fails; and scrambled
 * pages counted. The pages are built here as §4.2-4.3 lay them out.
 * Deterministic, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "teletext_page_format_clear.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "vbi-services/teletext_page_decoder.h"

namespace orc_unit_test {
namespace {

using orc::kTeletextPacketBytes;
using orc::TeletextPageDataStream;
using orc::TeletextPageFormatClear;
using Packet = std::array<uint8_t, kTeletextPacketBytes>;

constexpr size_t kDataBytes = 39;  // a packet's bytes after its Block Pointer

uint8_t ham(int value) {
  return orc::teletext_hamming84_encode(static_cast<uint8_t>(value & 0xF));
}

/// The header of page |page| of magazine 1 (EN 300 708 §4.3.1): sub-code S1
/// the continuity index, S2 and S4 the Last Transmitted Packet, S3 the data
/// stream.
Packet header(int page, int continuity, int last_packet, int stream = 1) {
  Packet packet{};
  packet[0] = ham(1);
  packet[1] = ham(0);
  packet[2] = ham(page & 0xF);
  packet[3] = ham(page >> 4);
  packet[4] = ham(continuity);
  packet[5] = ham(last_packet & 0x7);
  packet[6] = ham(stream);
  packet[7] = ham((last_packet >> 3) & 0x3);
  packet[8] = ham(0);
  packet[9] = ham(0);
  return packet;
}

Packet row_packet(int row, int pointer, const std::array<uint8_t, 39>& data) {
  Packet packet{};
  packet[0] = ham(1 | ((row & 1) << 3));
  packet[1] = ham(row >> 1);
  packet[2] = ham(pointer);
  for (size_t i = 0; i < kDataBytes; ++i) {
    packet[3 + i] = data[i];
  }
  return packet;
}

/// Packet X/28/0 for a Page Format - Clear page (Table 4): page function
/// 0001, page coding 001, bits 8 to 18 set as the table gives them.
Packet packet28() {
  Packet packet{};
  packet[0] = ham(1);
  packet[1] = ham(14);  // packet 28
  packet[2] = ham(0);
  const uint32_t triplet = 0x1 | (0x1 << 4) | (0x1FF << 7);
  orc::teletext_hamming2418_encode(triplet, &packet[3]);
  for (size_t i = 1; i < 13; ++i) {
    orc::teletext_hamming2418_encode(0, &packet[3 + 3 * i]);
  }
  return packet;
}

struct Block {
  int application = 0;
  std::vector<uint8_t> data;
};

struct EncodedPacket {
  int pointer = 0xD;
  std::array<uint8_t, 39> data{};
};

/// A stream of |blocks| laid into packets as §4.3.2.6 maps one: each block a
/// Block Separator and its Structure Header before it, the first separator
/// of a packet at a place its Block Pointer can name, any further one
/// straight after the block before, filler between.
std::vector<EncodedPacket> encode(const std::vector<Block>& blocks) {
  std::vector<EncodedPacket> packets;
  size_t at = kDataBytes;
  bool started_here = false;
  const auto next_packet = [&] {
    EncodedPacket packet;
    packet.data.fill(0x5E);
    packets.push_back(packet);
    at = 0;
    started_here = false;
  };
  const auto put = [&](uint8_t byte) {
    if (at == kDataBytes) {
      next_packet();
    }
    packets.back().data[at++] = byte;
  };
  for (const Block& block : blocks) {
    if (at == kDataBytes) {
      next_packet();
    }
    if (!started_here) {
      size_t aligned = (at + 2) / 3 * 3;
      if (aligned > 36) {
        next_packet();
        aligned = 0;
      }
      at = aligned;
      packets.back().pointer = static_cast<int>(at / 3);
    }
    put(0xA1);
    started_here = true;
    const size_t length = block.data.size();
    put(ham(block.application & 0xF));
    put(ham(((block.application >> 4) & 1) |
            static_cast<int>((length & 7) << 1)));
    put(ham(static_cast<int>(length >> 3)));
    put(ham(static_cast<int>(length >> 7)));
    for (const uint8_t byte : block.data) {
      put(byte);
    }
  }
  return packets;
}

/// A Bundle Information block (§4.2.3) naming |types|.
Block bundle(const std::vector<uint16_t>& types, bool damage_checksum = false) {
  const size_t length = 4 + 4 * types.size();
  std::vector<int> fields = {static_cast<int>(types.size() & 0xF),
                             static_cast<int>(types.size() >> 4)};
  for (const uint16_t type : types) {
    for (int shift = 0; shift < 16; shift += 4) {
      fields.push_back((type >> shift) & 0xF);
    }
  }
  int sum = 0 + static_cast<int>((length & 7) << 1) +
            static_cast<int>((length >> 3) & 0xF) +
            static_cast<int>((length >> 7) & 0xF);
  for (const int field : fields) {
    sum += field;
  }
  int checksum = (0x100 - (sum & 0xFF)) & 0xFF;
  if (damage_checksum) {
    checksum ^= 0x10;
  }
  Block block;
  block.application = 0;
  block.data.push_back(ham(checksum & 0xF));
  block.data.push_back(ham(checksum >> 4));
  for (const int field : fields) {
    block.data.push_back(ham(field));
  }
  return block;
}

std::vector<uint8_t> text_bytes(const std::string& text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

/// Send |packets| as pages of |per_page| packets, continuity from 0, with a
/// packet 28 on each where asked, then a time-filling header to end the
/// last.
void send(TeletextPageFormatClear& decoder,
          const std::vector<EncodedPacket>& packets, size_t per_page,
          bool with_packet28, const std::vector<size_t>& lost = {}) {
  size_t index = 0;
  int continuity = 0;
  while (index < packets.size()) {
    decoder.add_packet(header(0xF0, continuity, static_cast<int>(per_page)),
                       kTeletextPacketBytes);
    if (with_packet28) {
      decoder.add_packet(packet28(), kTeletextPacketBytes);
    }
    for (size_t row = 1; row <= per_page; ++row, ++index) {
      if (index >= packets.size()) {
        EncodedPacket filler;
        filler.data.fill(0x5E);
        decoder.add_packet(row_packet(static_cast<int>(row), 0xD, filler.data),
                           kTeletextPacketBytes);
        continue;
      }
      if (std::find(lost.begin(), lost.end(), index) != lost.end()) {
        continue;
      }
      decoder.add_packet(
          row_packet(static_cast<int>(row), packets[index].pointer,
                     packets[index].data),
          kTeletextPacketBytes);
    }
    continuity = (continuity + 1) % 16;
  }
  decoder.add_packet(header(0xFF, 0, 0), kTeletextPacketBytes);
}

std::vector<Block> sample_blocks() {
  std::vector<Block> blocks;
  blocks.push_back(bundle({0x1234, 0x0042}));
  std::string text;
  for (int i = 0; i < 5; ++i) {
    text += "HELLO WORLD ";
  }
  blocks.push_back(Block{1, text_bytes(text)});
  std::vector<uint8_t> binary(100);
  for (size_t i = 0; i < binary.size(); ++i) {
    binary[i] = static_cast<uint8_t>(i);
  }
  blocks.push_back(Block{2, binary});
  blocks.push_back(Block{1, text_bytes("AGAIN")});
  return blocks;
}

const TeletextPageDataStream* application(
    const std::vector<TeletextPageDataStream>& streams, int number) {
  for (const TeletextPageDataStream& stream : streams) {
    if (stream.application == number) {
      return &stream;
    }
  }
  return nullptr;
}

TEST(TeletextPageFormatClear, ReadsTheApplicationsOfADataPageStream) {
  TeletextPageFormatClear decoder;
  send(decoder, encode(sample_blocks()), 4, true);
  const auto streams = decoder.streams();
  ASSERT_EQ(streams.size(), 2u);
  const TeletextPageDataStream* first = application(streams, 1);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->tag(), "1F0-s2-a1");
  ASSERT_TRUE(first->application_type.has_value());
  EXPECT_EQ(*first->application_type, 0x1234);
  EXPECT_EQ(first->blocks, 2u);
  EXPECT_EQ(first->blocks_damaged, 0u);
  std::string expected;
  for (int i = 0; i < 5; ++i) {
    expected += "HELLO WORLD ";
  }
  expected += "AGAIN";
  EXPECT_EQ(std::string(first->payload.begin(), first->payload.end()),
            expected);
  const TeletextPageDataStream* second = application(streams, 2);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(*second->application_type, 0x0042);
  EXPECT_EQ(second->payload.size(), 100u);
  EXPECT_EQ(second->payload[99], 99);
  EXPECT_EQ(second->pages_lost, 0u);
}

TEST(TeletextPageFormatClear, KnowsADataPageByItsPacketsWithoutPacket28) {
  TeletextPageFormatClear decoder;
  send(decoder, encode(sample_blocks()), 6, false);
  const auto streams = decoder.streams();
  ASSERT_EQ(streams.size(), 2u);
  EXPECT_EQ(application(streams, 1)->blocks, 2u);
}

TEST(TeletextPageFormatClear, CombinesTheFragmentsOfAPage) {
  // §4.3a: a page sent in fragments repeats its header, with its continuity
  // index, before each.
  const auto packets = encode(sample_blocks());
  ASSERT_GE(packets.size(), 6u);
  TeletextPageFormatClear decoder;
  const int last = static_cast<int>(packets.size());
  for (size_t part = 0; part < 2; ++part) {
    decoder.add_packet(header(0xF0, 0, last), kTeletextPacketBytes);
    decoder.add_packet(packet28(), kTeletextPacketBytes);
    for (size_t i = part; i < packets.size(); i += 2) {
      decoder.add_packet(row_packet(static_cast<int>(i + 1), packets[i].pointer,
                                    packets[i].data),
                         kTeletextPacketBytes);
    }
  }
  decoder.add_packet(header(0xFF, 0, 0), kTeletextPacketBytes);
  const auto streams = decoder.streams();
  const TeletextPageDataStream* first = application(streams, 1);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->pages, 1u);
  EXPECT_EQ(first->packets_lost, 0u);
  EXPECT_EQ(first->blocks, 2u);
}

TEST(TeletextPageFormatClear, ALostPacketCostsOnlyTheBlockItHeld) {
  // The binary block runs from the third packet to the fifth; losing the
  // fourth loses it, and the stream picks up at the separator after it.
  const auto packets = encode(sample_blocks());
  TeletextPageFormatClear decoder;
  send(decoder, packets, 4, true, {3});
  const auto streams = decoder.streams();
  const TeletextPageDataStream* second = application(streams, 2);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(second->blocks, 0u);
  EXPECT_EQ(second->blocks_damaged, 1u);
  const TeletextPageDataStream* first = application(streams, 1);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first->blocks, 2u);
  EXPECT_EQ(first->packets_lost, 1u);
}

TEST(TeletextPageFormatClear, APageOfTextIsNotTakenForData) {
  TeletextPageFormatClear decoder;
  decoder.add_packet(header(0x00, 0, 23), kTeletextPacketBytes);
  for (int row = 1; row <= 6; ++row) {
    std::array<uint8_t, 39> text{};
    text.fill(orc::teletext_odd_parity_encode('A'));
    Packet packet = row_packet(row, 0, text);
    packet[2] = orc::teletext_odd_parity_encode(0x02);  // alpha green
    decoder.add_packet(packet, kTeletextPacketBytes);
  }
  decoder.add_packet(header(0xFF, 0, 0), kTeletextPacketBytes);
  EXPECT_TRUE(decoder.streams().empty());
}

TEST(TeletextPageFormatClear, ABundleWhoseChecksumFailsNamesNoTypes) {
  std::vector<Block> blocks = sample_blocks();
  blocks.front() = bundle({0x1234, 0x0042}, true);
  TeletextPageFormatClear decoder;
  send(decoder, encode(blocks), 4, true);
  const auto streams = decoder.streams();
  const TeletextPageDataStream* first = application(streams, 1);
  ASSERT_NE(first, nullptr);
  EXPECT_FALSE(first->application_type.has_value());
  EXPECT_EQ(first->blocks, 2u);
}

TEST(TeletextPageFormatClear, CountsScrambledPages) {
  // §5.4.2: a page key packet, X/28/2, marks a scrambled page.
  TeletextPageFormatClear decoder;
  decoder.add_packet(header(0x50, 0, 23), kTeletextPacketBytes);
  Packet key{};
  key[0] = ham(1);
  key[1] = ham(14);
  key[2] = ham(2);
  for (size_t i = 0; i < 13; ++i) {
    orc::teletext_hamming2418_encode(static_cast<uint32_t>(i * 77),
                                     &key[3 + 3 * i]);
  }
  decoder.add_packet(key, kTeletextPacketBytes);
  decoder.add_packet(header(0xFF, 0, 0), kTeletextPacketBytes);
  EXPECT_EQ(decoder.scrambled_transmissions(), 1u);
  ASSERT_EQ(decoder.scrambled_pages().size(), 1u);
  EXPECT_EQ(decoder.scrambled_pages().begin()->second, 0x50);
  EXPECT_TRUE(decoder.streams().empty());
}

}  // namespace
}  // namespace orc_unit_test
