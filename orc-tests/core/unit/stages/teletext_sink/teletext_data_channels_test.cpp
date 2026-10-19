/*
 * File:        teletext_data_channels_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for the decoder of the teletext packets that carry
 *              data rather than pages
 *
 * Covers: packet 8/30 Format 1 (network, initial page, time and status) and
 * Format 2 (the programme label); IDL Format A with an explicit and an
 * implicit continuity indicator, repeats, losses, dummy bytes and a failed
 * check word; IDL Format B's forward error correction, horizontal and
 * vertical; Datavideo, masked and not; low bit-rate audio; and the channels
 * no format is defined for. Every packet is built here as EN 300 706 and
 * EN 300 708 code it. Deterministic, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "teletext_data_channels.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "vbi-services/teletext_page_decoder.h"

namespace orc_unit_test {
namespace {

using orc::kTeletextPacketBytes;
using orc::TeletextDataChannels;
using orc::TeletextDataFormat;
using orc::TeletextDataStream;
using Packet = std::array<uint8_t, kTeletextPacketBytes>;

uint8_t ham(int value) {
  return orc::teletext_hamming84_encode(static_cast<uint8_t>(value & 0xF));
}

uint8_t reversed(uint8_t byte) {
  uint8_t out = 0;
  for (int bit = 0; bit < 8; ++bit) {
    out = static_cast<uint8_t>((out << 1) | ((byte >> bit) & 1));
  }
  return out;
}

/// A packet of data channel |channel| (EN 300 708 §6.4.2): its first address
/// byte the channel's four bits, its second the rest of packet number 30 or
/// 31.
Packet packet_on(int channel) {
  Packet packet{};
  packet[0] = ham(channel);
  packet[1] = ham(15);
  return packet;
}

/// The two check word bytes that leave the register at zero after |count|
/// bytes from |bytes|: the remainder, highest term first (§6.5.8.1).
std::array<uint8_t, 2> check_word(const uint8_t* bytes, size_t count,
                                  uint16_t polynomial) {
  const uint16_t remainder =
      orc::teletext_data_line_crc_register(bytes, count, polynomial);
  return {reversed(static_cast<uint8_t>(remainder >> 8)),
          reversed(static_cast<uint8_t>(remainder & 0xFF))};
}

/// Packet 8/30 Format 1 (EN 300 706 §9.8.1 Table 18) of initial page 100,
/// network |network|, local time UTC+1 and |utc| as "hhmmss", status "NEWS
/// 24".
Packet service_packet(uint16_t network, const char* utc, int mjd) {
  Packet packet = packet_on(0);
  packet[1] = ham(15);
  packet[2] = ham(0);  // Format 1, within a video signal
  // Page 100, sub-code 3F7F: units 0, tens 0, S1 F, S2 7 with M1 (magazine
  // 1), S3 F, S4 3.
  const int page[6] = {0, 0, 0xF, 0x7 | 0x8, 0xF, 0x3};
  for (size_t i = 0; i < 6; ++i) {
    packet[3 + i] = ham(page[i]);
  }
  packet[9] = reversed(static_cast<uint8_t>(network >> 8));
  packet[10] = reversed(static_cast<uint8_t>(network & 0xFF));
  packet[11] = 0x81 | (2 << 1);  // +1 hour: two half hours
  const auto up = [](int d) { return static_cast<uint8_t>(d + 1); };
  const int digits[5] = {mjd / 10000 % 10, mjd / 1000 % 10, mjd / 100 % 10,
                         mjd / 10 % 10, mjd % 10};
  packet[12] = static_cast<uint8_t>(0xF0 | up(digits[0]));
  packet[13] = static_cast<uint8_t>((up(digits[1]) << 4) | up(digits[2]));
  packet[14] = static_cast<uint8_t>((up(digits[3]) << 4) | up(digits[4]));
  for (size_t i = 0; i < 3; ++i) {
    packet[15 + i] = static_cast<uint8_t>((up(utc[2 * i] - '0') << 4) |
                                          up(utc[2 * i + 1] - '0'));
  }
  const std::string status = "NEWS 24";
  for (size_t i = 0; i < 20; ++i) {
    const char c = i < status.size() ? status[i] : ' ';
    packet[22 + i] = orc::teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return packet;
}

TEST(TeletextDataChannels, ReadsBroadcastServiceDataFormat1) {
  TeletextDataChannels channels;
  // MJD 59000 is 2020-05-31.
  channels.add_packet(service_packet(0x1234, "123456", 59000),
                      kTeletextPacketBytes);
  channels.add_packet(service_packet(0x1234, "123457", 59000),
                      kTeletextPacketBytes);
  channels.add_packet(service_packet(0x1234, "123458", 59000),
                      kTeletextPacketBytes);

  const auto& service = channels.service_data();
  EXPECT_EQ(service.format1_packets, 3u);
  EXPECT_TRUE(service.multiplexed);
  EXPECT_EQ(service.initial_page, "100");
  ASSERT_EQ(service.network_ids.size(), 1u);
  EXPECT_EQ(service.network_ids.begin()->first, 0x1234);
  EXPECT_EQ(service.status_display, "NEWS 24");
  EXPECT_EQ(service.offset_minutes, 60);
  ASSERT_TRUE(service.first_utc.has_value());
  EXPECT_EQ(*service.first_utc,
            (59000 - 40587) * 86400LL + 12 * 3600 + 34 * 60 + 56);
  EXPECT_EQ(*service.last_utc, *service.first_utc + 2);
  const auto lines = service.summary();
  ASSERT_GE(lines.size(), 4u);
  EXPECT_NE(lines[3].find("2020-05-31 12:34:56 UTC"), std::string::npos);
}

TEST(TeletextDataChannels, ATimeNoOtherPacketAgreesWithIsNotBelieved) {
  // A damaged packet's time agrees with neither neighbour.
  TeletextDataChannels channels;
  channels.add_packet(service_packet(0x1234, "123456", 59000),
                      kTeletextPacketBytes);
  channels.add_packet(service_packet(0x1234, "223456", 59000),
                      kTeletextPacketBytes);
  channels.add_packet(service_packet(0x1234, "123458", 59000),
                      kTeletextPacketBytes);
  EXPECT_FALSE(channels.service_data().first_utc.has_value());
}

TEST(TeletextDataChannels, NamesTheNetworkByItsCommonestReading) {
  // The code has no Hamming protection, so a damaged packet reads another.
  TeletextDataChannels channels;
  for (const char* utc : {"123456", "123457", "123458"}) {
    channels.add_packet(service_packet(0x1234, utc, 59000),
                        kTeletextPacketBytes);
  }
  channels.add_packet(service_packet(0x9234, "123459", 59000),
                      kTeletextPacketBytes);
  const auto lines = channels.service_data().summary();
  ASSERT_GE(lines.size(), 3u);
  EXPECT_EQ(lines[2],
            "network identification 0x1234 (3 packets); 1 packet read "
            "otherwise");
}

TEST(TeletextDataChannels, ReadsTheProgrammeLabelOfFormat2) {
  // EN 300 231 §8.2.1 Tables 7 and 8: label channel 1, prepare to record,
  // stereo, mode identifier, network 0x1DC1, 18 October 20:15, type 0x41.
  Packet packet = packet_on(0);
  packet[2] = ham(2);  // Format 2, within a video signal
  for (size_t i = 3; i < 9; ++i) {
    packet[i] = ham(0xF);
  }
  int nibble[13] = {};
  const auto set = [&nibble](int byte, int bit, int value) {
    nibble[byte] |= (value & 1) << bit;
  };
  // Parameter bits b1 first, b1 the most significant (§6.2).
  const auto put = [&set](std::initializer_list<std::pair<int, int>> places,
                          int value, int width) {
    int bit = width - 1;
    for (const auto& [byte, at] : places) {
      set(byte, at, value >> bit);
      --bit;
    }
  };
  put({{0, 0}, {0, 1}}, 1, 2);  // LCI
  set(0, 3, 1);                 // PRF
  put({{1, 0}, {1, 1}}, 2, 2);  // PCS: stereo
  set(1, 2, 1);                 // MI
  put({{2, 0},
       {2, 1},
       {2, 2},
       {2, 3},
       {8, 2},
       {8, 3},
       {9, 0},
       {9, 1},
       {3, 0},
       {3, 1},
       {9, 2},
       {9, 3},
       {10, 0},
       {10, 1},
       {10, 2},
       {10, 3}},
      0x1DC1, 16);
  put({{3, 2}, {3, 3}, {4, 0}, {4, 1}, {4, 2}}, 18, 5);
  put({{4, 3}, {5, 0}, {5, 1}, {5, 2}}, 10, 4);
  put({{5, 3}, {6, 0}, {6, 1}, {6, 2}, {6, 3}}, 20, 5);
  put({{7, 0}, {7, 1}, {7, 2}, {7, 3}, {8, 0}, {8, 1}}, 15, 6);
  put({{11, 0}, {11, 1}, {11, 2}, {11, 3}, {12, 0}, {12, 1}, {12, 2}, {12, 3}},
      0x41, 8);
  for (size_t i = 0; i < 13; ++i) {
    packet[9 + i] = ham(nibble[i]);
  }
  for (size_t i = 22; i < 42; ++i) {
    packet[i] = orc::teletext_odd_parity_encode(' ');
  }

  TeletextDataChannels channels;
  channels.add_packet(packet, kTeletextPacketBytes);
  const auto& labels = channels.service_data().programme_labels;
  ASSERT_EQ(labels.size(), 1u);
  EXPECT_EQ(labels.begin()->first,
            "label channel 1: network 0x1DC1, programme 18-10 20:15, stereo, "
            "programme type 0x41, prepare to record, mode identifier set");
  EXPECT_EQ(channels.service_data().format2_packets, 1u);
}

TEST(TeletextDataChannels, IgnoresAPacketWhoseAddressWasCorrected) {
  // The address must arrive as codewords: a corrected one is as likely
  // another packet's.
  Packet packet = service_packet(0x1234, "123456", 59000);
  packet[1] ^= 0x01;
  TeletextDataChannels channels;
  channels.add_packet(packet, kTeletextPacketBytes);
  EXPECT_EQ(channels.service_data().format1_packets, 0u);
  EXPECT_EQ(channels.packets_refused(), 0u);
}

/// An IDL Format A packet on channel 9 (EN 300 708 §6.5) with service packet
/// address 2A, an explicit continuity indicator and a data length byte.
Packet format_a_packet(uint8_t continuity, const std::vector<uint8_t>& data) {
  Packet packet = packet_on(9);
  packet[2] = ham(0x4 | 0x8);  // Format A, CI and DL
  packet[3] = ham(2);          // two address digits
  packet[4] = ham(0xA);        // least significant first
  packet[5] = ham(0x2);
  packet[6] = continuity;
  packet[7] = static_cast<uint8_t>(data.size());
  for (size_t i = 0; i < data.size(); ++i) {
    packet[8 + i] = data[i];
  }
  const auto crc =
      check_word(&packet[6], 40 - 6, orc::kTeletextIdlFormatAPolynomial);
  packet[40] = crc[0];
  packet[41] = crc[1];
  return packet;
}

const TeletextDataStream* only_stream(
    const std::vector<TeletextDataStream>& streams) {
  EXPECT_EQ(streams.size(), 1u);
  return streams.size() == 1 ? &streams.front() : nullptr;
}

TEST(TeletextDataChannels, ReadsAnIdlFormatADataLine) {
  TeletextDataChannels channels;
  channels.add_packet(format_a_packet(5, {'H', 'E', 'L', 'L', 'O'}),
                      kTeletextPacketBytes);
  // Sent again under the same indicator: a repeat.
  channels.add_packet(format_a_packet(5, {'H', 'E', 'L', 'L', 'O'}),
                      kTeletextPacketBytes);
  // Indicator 7: the packet with 6 went missing.
  channels.add_packet(format_a_packet(7, {'!'}), kTeletextPacketBytes);

  const auto streams = channels.streams();
  const TeletextDataStream* stream = only_stream(streams);
  ASSERT_NE(stream, nullptr);
  EXPECT_EQ(stream->format, TeletextDataFormat::kIdlFormatA);
  EXPECT_EQ(stream->channel, 9);
  EXPECT_EQ(stream->address, 0x2Au);
  EXPECT_EQ(stream->tag(), "9-2A");
  EXPECT_EQ(stream->packets, 3u);
  EXPECT_EQ(stream->repeats, 1u);
  EXPECT_EQ(stream->gaps, 1u);
  EXPECT_EQ(std::string(stream->payload.begin(), stream->payload.end()),
            "HELLO!");
}

TEST(TeletextDataChannels, RefusesAnIdlPacketWhoseCheckWordFails) {
  Packet packet = format_a_packet(5, {'H', 'E', 'L', 'L', 'O'});
  packet[9] ^= 0x10;
  TeletextDataChannels channels;
  channels.add_packet(packet, kTeletextPacketBytes);
  EXPECT_TRUE(channels.streams().empty());
  EXPECT_EQ(channels.packets_refused(), 1u);
}

TEST(TeletextDataChannels, DropsTheDummyByteAfterEightIdenticalBytes) {
  // §6.5.7.1: the continuity indicator 00 and seven zero bytes are eight, so
  // the byte after them is a dummy.
  std::vector<uint8_t> data(7, 0x00);
  data.push_back(0x55);  // the dummy
  data.push_back('A');
  TeletextDataChannels channels;
  channels.add_packet(format_a_packet(0, data), kTeletextPacketBytes);
  const auto streams = channels.streams();
  const TeletextDataStream* stream = only_stream(streams);
  ASSERT_NE(stream, nullptr);
  std::vector<uint8_t> expected(7, 0x00);
  expected.push_back('A');
  EXPECT_EQ(stream->payload, expected);
}

/// An IDL Format A packet with neither address nor explicit indicator: its
/// check word set so that the register ends holding |continuity| twice,
/// least significant bit at the output end (§6.5.8.2).
Packet implicit_packet(uint8_t continuity, uint8_t data) {
  Packet packet = packet_on(8);
  packet[2] = ham(0);  // Format A, nothing optional
  packet[3] = ham(0);  // no address
  for (size_t i = 4; i < 40; ++i) {
    packet[i] = data;
  }
  const uint16_t target =
      static_cast<uint16_t>((reversed(continuity) << 8) | reversed(continuity));
  for (uint32_t word = 0; word <= 0xFFFF; ++word) {
    packet[40] = static_cast<uint8_t>(word >> 8);
    packet[41] = static_cast<uint8_t>(word & 0xFF);
    if (orc::teletext_data_line_crc_register(
            &packet[4], 38, orc::kTeletextIdlFormatAPolynomial) == target) {
      break;
    }
  }
  return packet;
}

TEST(TeletextDataChannels, ReadsTheContinuityAnImplicitIndicatorLeaves) {
  // Consecutive indicators 1 and 2 must read as consecutive: read the other
  // way round the register's bits would make them 0x80 and 0x40.
  TeletextDataChannels channels;
  channels.add_packet(implicit_packet(1, 'x'), kTeletextPacketBytes);
  channels.add_packet(implicit_packet(2, 'y'), kTeletextPacketBytes);
  const auto streams = channels.streams();
  const TeletextDataStream* stream = only_stream(streams);
  ASSERT_NE(stream, nullptr);
  EXPECT_EQ(stream->packets, 2u);
  EXPECT_EQ(stream->gaps, 0u);
  EXPECT_EQ(stream->repeats, 0u);
  EXPECT_EQ(stream->payload.size(), 72u);
}

// GF(2^8) of the Format B code, x^8 + x^4 + x^3 + x^2 + 1, alpha = 2.
uint8_t gf_mul(uint8_t a, uint8_t b) {
  unsigned product = 0;
  unsigned x = a;
  for (int bit = 0; bit < 8; ++bit) {
    if (((b >> bit) & 1) != 0) {
      product ^= x;
    }
    x <<= 1;
    if ((x & 0x100) != 0) {
      x ^= 0x11D;
    }
  }
  return static_cast<uint8_t>(product);
}

uint8_t gf_div(uint8_t a, uint8_t b) {
  for (unsigned q = 0; q < 256; ++q) {
    if (gf_mul(static_cast<uint8_t>(q), b) == a) {
      return static_cast<uint8_t>(q);
    }
  }
  return 0;
}

/// The two suffix values of §6.8.2.3 for |values|: S0 = (X + Y) / (1 + a)
/// and S1 = S0 + X, the i-th of n values weighted a^(n+1-i).
std::array<uint8_t, 2> suffix(const std::vector<uint8_t>& values) {
  uint8_t x = 0;
  uint8_t y = 0;
  for (const uint8_t value : values) {
    x ^= value;
    y = static_cast<uint8_t>(gf_mul(y, 2) ^ value);
  }
  // Two places on for the suffix bytes' own weights a and 1.
  y = gf_mul(gf_mul(y, 2), 2);
  const uint8_t s0 = gf_div(static_cast<uint8_t>(x ^ y), 3);
  return {s0, static_cast<uint8_t>(s0 ^ x)};
}

/// The sixteen packets of one Format B bundle on channel 15, application 3
/// of application number 1, carrying |data| (14 rows of 35 bytes).
std::vector<Packet> format_b_bundle(const std::vector<uint8_t>& data) {
  std::array<std::array<uint8_t, 37>, 16> rows{};
  for (size_t r = 0; r < 14; ++r) {
    for (size_t c = 0; c < 35; ++c) {
      rows[r][c] = data[r * 35 + c];
    }
  }
  // Packets 14 and 15 protect each column (§6.8.2.2).
  for (size_t c = 0; c < 35; ++c) {
    std::vector<uint8_t> column;
    for (size_t r = 0; r < 14; ++r) {
      column.push_back(rows[r][c]);
    }
    const auto check = suffix(column);
    rows[14][c] = check[0];
    rows[15][c] = check[1];
  }
  std::vector<Packet> packets;
  for (size_t r = 0; r < 16; ++r) {
    const auto check =
        suffix(std::vector<uint8_t>(rows[r].begin(), rows[r].begin() + 35));
    rows[r][35] = check[0];
    rows[r][36] = check[1];
    Packet packet = packet_on(15);
    packet[2] = ham(1 | (1 << 2));  // Format B, application number 1
    packet[3] = ham(3);
    packet[4] = ham(static_cast<int>(r));
    for (size_t c = 0; c < 37; ++c) {
      packet[5 + c] = rows[r][c];
    }
    packets.push_back(packet);
  }
  return packets;
}

TEST(TeletextDataChannels, CorrectsAndRestoresAnIdlFormatBBundle) {
  std::vector<uint8_t> data(14 * 35);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>(i * 7 + 3);
  }
  auto packets = format_b_bundle(data);
  packets[2][5 + 10] ^= 0x5A;          // one byte wrong: the packet's own code
  packets.erase(packets.begin() + 5);  // one packet lost: the columns' code

  TeletextDataChannels channels;
  for (const Packet& packet : packets) {
    channels.add_packet(packet, kTeletextPacketBytes);
  }
  const auto streams = channels.streams();
  const TeletextDataStream* stream = only_stream(streams);
  ASSERT_NE(stream, nullptr);
  EXPECT_EQ(stream->format, TeletextDataFormat::kIdlFormatB);
  EXPECT_EQ(stream->tag(), "15-b1-3");
  EXPECT_EQ(stream->payload, data);
  EXPECT_EQ(stream->gaps, 1u);
  EXPECT_EQ(stream->rows_restored, 1u);
  EXPECT_EQ(stream->rows_lost, 0u);
  EXPECT_GE(stream->bytes_corrected, 1u);
}

/// A Datavideo packet on channel 5 (EN 300 708 §6.6): packet address 123,
/// continuity index |continuity|, masked where asked, its check word sent
/// complemented (§6.6.6.1).
Packet datavideo_packet(int continuity, bool masked,
                        const std::vector<uint8_t>& data) {
  static const uint8_t kMask[35] = {
      0xAF, 0xAA, 0x81, 0x4A, 0xF2, 0xEE, 0x07, 0x3A, 0x4F, 0x5D, 0x44, 0x86,
      0x70, 0x9D, 0x83, 0x43, 0xBC, 0x3F, 0xE0, 0xF7, 0xC5, 0xCC, 0x82, 0x53,
      0xB4, 0x79, 0xF3, 0x62, 0xA4, 0x71, 0xB5, 0x71, 0x31, 0x10, 0x08};
  Packet packet = packet_on(5);
  packet[2] = ham(0x3);
  packet[3] = ham(0x2);
  packet[4] = ham(0x1);
  packet[5] = ham(continuity & 0xF);
  packet[6] = ham(((continuity >> 4) & 0x3) | (masked ? 0x4 : 0));
  std::array<uint8_t, 35> body{};
  for (size_t i = 0; i < 33 && i < data.size(); ++i) {
    body[i] = data[i];
  }
  const auto crc =
      check_word(body.data(), 33, orc::kTeletextDatavideoPolynomial);
  body[33] = static_cast<uint8_t>(crc[0] ^ 0xFF);
  body[34] = static_cast<uint8_t>(crc[1] ^ 0xFF);
  for (size_t i = 0; i < 35; ++i) {
    packet[7 + i] = static_cast<uint8_t>(body[i] ^ (masked ? kMask[i] : 0));
  }
  return packet;
}

TEST(TeletextDataChannels, ReadsADatavideoPacketMaskedOrNot) {
  const std::vector<uint8_t> data = {'D', 'A', 'T', 'A'};
  TeletextDataChannels channels;
  channels.add_packet(datavideo_packet(10, false, data), kTeletextPacketBytes);
  // §6.6.3: the same packet sent masked carries the same index.
  channels.add_packet(datavideo_packet(10, true, data), kTeletextPacketBytes);
  channels.add_packet(datavideo_packet(11, true, data), kTeletextPacketBytes);
  const auto streams = channels.streams();
  const TeletextDataStream* stream = only_stream(streams);
  ASSERT_NE(stream, nullptr);
  EXPECT_EQ(stream->format, TeletextDataFormat::kDatavideo);
  EXPECT_EQ(stream->tag(), "5-123");
  EXPECT_EQ(stream->packets, 3u);
  EXPECT_EQ(stream->repeats, 1u);
  EXPECT_EQ(stream->masked_packets, 2u);
  ASSERT_EQ(stream->payload.size(), 66u);
  EXPECT_EQ(stream->payload[33], 'D');
}

TEST(TeletextDataChannels, KeepsLowBitRateAudioWithItsFadeByte) {
  Packet packet = packet_on(4);
  packet[2] = ham(0);      // AUDETEL
  packet[3] = ham(1 | 8);  // 4 dB, speech muted (Tables 19 and 20)
  for (size_t i = 4; i < 42; ++i) {
    packet[i] = static_cast<uint8_t>(i);
  }
  TeletextDataChannels channels;
  // A service sends audio continuously; one packet with no check word is no
  // evidence of one.
  channels.add_packet(packet, kTeletextPacketBytes);
  EXPECT_TRUE(channels.streams().empty());
  EXPECT_EQ(channels.packets_refused(), 1u);
  for (uint64_t i = 1; i < orc::kTeletextAudioMinPackets; ++i) {
    channels.add_packet(packet, kTeletextPacketBytes);
  }
  const auto streams = channels.streams();
  const TeletextDataStream* stream = only_stream(streams);
  ASSERT_NE(stream, nullptr);
  EXPECT_EQ(stream->format, TeletextDataFormat::kAudio);
  EXPECT_EQ(stream->tag(), "4-s0");
  EXPECT_EQ(stream->payload.size(), 38u * orc::kTeletextAudioMinPackets);
  EXPECT_NE(stream->summary().find("AUDETEL"), std::string::npos);
  EXPECT_NE(
      stream->summary().find("fade 4 dB, speech muted in " +
                             std::to_string(orc::kTeletextAudioMinPackets)),
      std::string::npos);
  EXPECT_EQ(channels.packets_refused(), 0u);
}

TEST(TeletextDataChannels, TooFewServiceDataPacketsAreNotBelieved) {
  // Their fields are mostly unprotected: one or two could be a misread row.
  TeletextDataChannels channels;
  channels.add_packet(service_packet(0x1234, "123456", 59000),
                      kTeletextPacketBytes);
  channels.add_packet(service_packet(0x1234, "123457", 59000),
                      kTeletextPacketBytes);
  EXPECT_FALSE(channels.service_data().any());
  EXPECT_TRUE(channels.service_data().summary().empty());
  EXPECT_EQ(channels.packets_refused(), 2u);
}

TEST(TeletextDataChannels, CountsPacketsOnChannelsWithNoFormat) {
  TeletextDataChannels channels;
  channels.add_packet(packet_on(1), kTeletextPacketBytes);
  channels.add_packet(packet_on(7), kTeletextPacketBytes);
  EXPECT_EQ(channels.packets_unassigned(), 2u);
  EXPECT_TRUE(channels.streams().empty());
}

TEST(TeletextDataChannels, PageRowsAreNotData) {
  // Packets numbered below 30 are a page's, whatever they hold.
  Packet row{};
  row[0] = ham(1);
  row[1] = ham(1);  // packet 1/2
  TeletextDataChannels channels;
  channels.add_packet(row, kTeletextPacketBytes);
  EXPECT_EQ(channels.packets_refused(), 0u);
  EXPECT_EQ(channels.packets_unassigned(), 0u);
  EXPECT_FALSE(channels.service_data().any());
}

}  // namespace
}  // namespace orc_unit_test
