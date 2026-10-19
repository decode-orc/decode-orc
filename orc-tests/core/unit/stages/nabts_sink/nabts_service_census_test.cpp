/*
 * File:        nabts_service_census_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the census of NABTS services that are not
 *              teletext
 *
 * Covers: RFC 2728's row code, Berlekamp-Massey, the clock's dates, and the
 * four services the census names — an FEC datacast and its bundle layout, an
 * encoder's idle fill, a test sequence, a clock — against packets built here
 * and packets as the corpus recorded them, and the teletext it must not name.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_service_census.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "nabts_test_builders.h"

namespace orc_unit_test {
namespace {

using nabts::hamming;
using nabts::make_packet;
using nabts::parity;
using orc::NabtsOtherService;
using orc::NabtsSuffixKind;

////////////////////////////////////////////////////////////////////////////////
// Builders
////////////////////////////////////////////////////////////////////////////////

// RFC 2728 Appendix A's field, multiplied out longhand so the test shares no
// table with the census.
uint8_t gf_multiply(uint8_t a, uint8_t b) {
  unsigned product = 0;
  unsigned shifted = a;
  for (unsigned bit = 0; bit < 8; ++bit) {
    if (((b >> bit) & 1U) != 0) {
      product ^= shifted;
    }
    shifted <<= 1U;
    if ((shifted & 0x100U) != 0) {
      shifted ^= 0x11DU;
    }
  }
  return static_cast<uint8_t>(product);
}

uint8_t gf_inverse(uint8_t value) {
  for (unsigned candidate = 1; candidate < 256; ++candidate) {
    if (gf_multiply(value, static_cast<uint8_t>(candidate)) == 1) {
      return static_cast<uint8_t>(candidate);
    }
  }
  return 0;
}

uint8_t gf_power(uint8_t value, unsigned exponent) {
  uint8_t out = 1;
  for (unsigned i = 0; i < exponent; ++i) {
    out = gf_multiply(out, value);
  }
  return out;
}

/// The 28 bytes after the prefix of a row of RFC 2728's code: |data| (26
/// bytes) then the two check bytes that make alpha and alpha^3 roots.
std::vector<uint8_t> row_codeword(const std::vector<uint8_t>& data) {
  const uint8_t alpha = 0x1D;
  // Codeword positions 2 to 27 are the data bytes; positions 0 and 1 the
  // checks, solved from S0 = S1 = 0.
  uint8_t d0 = 0;
  uint8_t d1 = 0;
  for (unsigned i = 0; i < 26; ++i) {
    d0 ^= gf_multiply(data[i], gf_power(alpha, i + 2));
    d1 ^= gf_multiply(data[i], gf_power(alpha, 3 * (i + 2)));
  }
  const uint8_t alpha3 = gf_power(alpha, 3);
  const uint8_t c1 =
      gf_multiply(static_cast<uint8_t>(d0 ^ d1),
                  gf_inverse(static_cast<uint8_t>(alpha ^ alpha3)));
  const uint8_t c0 = static_cast<uint8_t>(d0 ^ gf_multiply(c1, alpha));
  std::vector<uint8_t> out = data;
  out.push_back(c0);
  out.push_back(c1);
  return out;
}

/// A whole packet: the prefix for |channel|, |continuity| and |suffix|, then
/// |block| as the 28 bytes after it.
std::vector<uint8_t> packet_with_block(uint16_t channel, uint8_t continuity,
                                       NabtsSuffixKind suffix,
                                       const std::vector<uint8_t>& block,
                                       bool synchronizing = false) {
  std::vector<uint8_t> packet =
      make_packet(channel, continuity, synchronizing, suffix, {});
  for (size_t i = 0; i < block.size() && i < orc::kNabtsMaxDataBlockBytes;
       ++i) {
    packet[orc::kNabtsPrefixBytes + i] = block[i];
  }
  return packet;
}

/// Deterministic bytes for the tests: the top byte of a linear congruential
/// generator. Not a shift register — its carries make it non-linear over
/// GF(2), which a stand-in for real data has to be here, since the census
/// recognises shift-register sequences.
class Bytes {
 public:
  explicit Bytes(uint32_t seed) : state_(seed) {}
  uint8_t next() {
    state_ = state_ * 1664525U + 1013904223U;
    return static_cast<uint8_t>(state_ >> 24U);
  }

 private:
  uint32_t state_;
};

/// Feeds packets to a census as the sink does: decoded, with the raw bytes.
class Census {
 public:
  void feed(const std::vector<uint8_t>& packet) {
    census_.add_packet(orc::nabts_decode_packet(packet.data(), packet.size()),
                       packet.data());
  }
  std::vector<NabtsOtherService> services() const { return census_.services(); }

 private:
  orc::NabtsServiceCensus census_;
};

// ITU-T O.150's 2^11 - 1 sequence, s[n] = s[n-9] + s[n-11]: a datacast's
// test stream.
std::vector<uint8_t> prbs11_bits(size_t count) {
  std::vector<uint8_t> bits{1, 0, 0, 1, 0, 1, 1, 0, 1, 1, 1};
  while (bits.size() < count) {
    const size_t n = bits.size();
    bits.push_back(static_cast<uint8_t>(bits[n - 9] ^ bits[n - 11]));
  }
  return bits;
}

/// |bits| packed into bytes, least significant bit first, as a NABTS line
/// sends them.
std::vector<uint8_t> pack_lsb_first(const std::vector<uint8_t>& bits,
                                    size_t first, size_t bytes) {
  std::vector<uint8_t> out(bytes, 0);
  for (size_t i = 0; i < bytes * 8; ++i) {
    out[i / 8] =
        static_cast<uint8_t>(out[i / 8] | (bits[first + i] << (i % 8)));
  }
  return out;
}

/// The 28 bytes of a PBS datacast clock packet: fourteen bytes sent as
/// Hamming nibble pairs, low nibble first.
std::vector<uint8_t> clock_block(const std::array<uint8_t, 14>& values) {
  std::vector<uint8_t> out;
  for (const uint8_t value : values) {
    out.push_back(hamming(static_cast<uint8_t>(value & 0xF)));
    out.push_back(hamming(static_cast<uint8_t>(value >> 4)));
  }
  return out;
}

std::vector<uint8_t> clock_time_block(uint32_t unix_seconds) {
  return clock_block({2, 2, 0, 0, 0, static_cast<uint8_t>(unix_seconds),
                      static_cast<uint8_t>(unix_seconds >> 8),
                      static_cast<uint8_t>(unix_seconds >> 16),
                      static_cast<uint8_t>(unix_seconds >> 24), 0, 0, 0, 0, 0});
}

std::vector<uint8_t> clock_name_block() {
  return clock_block({1, 2, 0, 0, 0, 'P', 'B', 'S', 0, 0, 0, 0, 0, 0});
}

// An idle-fill address as recorded: the data block of
// each continuity index of the encoder's BERT fill (one-byte suffix, PS 4),
// and the synchronizing packet's (PS 5) group header, record header and SI
// padding. Each byte is the one before it shifted down a place with a new bit
// at the top and odd parity.
const std::array<std::array<uint8_t, 27>, 16> kBertFillBlocks = {{
    {0x57, 0xAB, 0xD5, 0x2A, 0xD5, 0xEA, 0x75, 0x7A, 0xFD,
     0xFE, 0xBF, 0xDF, 0x2F, 0x97, 0x0B, 0x45, 0xA2, 0x51,
     0xA8, 0x54, 0xEA, 0xB5, 0xDA, 0x6D, 0x76, 0x3B, 0x9D},
    {0x57, 0x6B, 0x75, 0xBA, 0x5D, 0x6E, 0x37, 0x5B, 0x6D,
     0xB6, 0x9B, 0x0D, 0x86, 0x83, 0x01, 0x40, 0xE0, 0xB0,
     0x98, 0x8C, 0x46, 0xE3, 0x31, 0x58, 0xEC, 0x76, 0xFB},
    {0x54, 0x2A, 0x15, 0x4A, 0xE5, 0xF2, 0x79, 0xBC, 0x9E,
     0x4F, 0x67, 0xB3, 0xD9, 0xEC, 0xB6, 0x5B, 0xAD, 0x16,
     0x0B, 0x85, 0x02, 0xC1, 0x20, 0xD0, 0xA8, 0x94, 0x4A},
    {0x54, 0xEA, 0xB5, 0xDA, 0x6D, 0x76, 0x3B, 0x9D, 0x0E,
     0x07, 0x43, 0x61, 0x70, 0xF8, 0xBC, 0x5E, 0xEF, 0xF7,
     0x3B, 0x5D, 0xAE, 0x97, 0xCB, 0xE5, 0x32, 0xD9, 0x2C},
    {0xD5, 0x2A, 0xD5, 0xEA, 0x75, 0x7A, 0xFD, 0xFE, 0xBF,
     0xDF, 0x2F, 0x97, 0x0B, 0x45, 0xA2, 0x51, 0xA8, 0x54,
     0xEA, 0xB5, 0xDA, 0x6D, 0x76, 0x3B, 0x9D, 0x0E, 0x07},
    {0xD5, 0xEA, 0x75, 0x7A, 0xFD, 0xFE, 0xBF, 0xDF, 0x2F,
     0x97, 0x0B, 0x45, 0xA2, 0x51, 0xA8, 0x54, 0xEA, 0xB5,
     0xDA, 0x6D, 0x76, 0x3B, 0x9D, 0x0E, 0x07, 0x43, 0x61},
    {0xDA, 0x6D, 0x76, 0x3B, 0x9D, 0x0E, 0x07, 0x43, 0x61,
     0x70, 0xF8, 0xBC, 0x5E, 0xEF, 0xF7, 0x3B, 0x5D, 0xAE,
     0x97, 0xCB, 0xE5, 0x32, 0xD9, 0x2C, 0x16, 0xCB, 0x25},
    {0xDA, 0xAD, 0xD6, 0xAB, 0x15, 0x8A, 0x45, 0x62, 0xF1,
     0x38, 0xDC, 0x6E, 0xF7, 0xFB, 0xFD, 0x3E, 0x1F, 0x4F,
     0xA7, 0x13, 0x49, 0x64, 0x32, 0x19, 0x8C, 0x86, 0x43},
    {0x5B, 0x6D, 0xB6, 0x9B, 0x0D, 0x86, 0x83, 0x01, 0x40,
     0xE0, 0xB0, 0x98, 0x8C, 0x46, 0xE3, 0x31, 0x58, 0xEC,
     0x76, 0xFB, 0x3D, 0x9E, 0x8F, 0xC7, 0x23, 0x51, 0x68},
    {0x5B, 0xAD, 0x16, 0x0B, 0x85, 0x02, 0xC1, 0x20, 0xD0,
     0xA8, 0x94, 0x4A, 0x25, 0x52, 0xE9, 0x34, 0x1A, 0x0D,
     0x46, 0x23, 0x91, 0xC8, 0x64, 0xF2, 0xB9, 0x1C, 0x0E},
    {0x58, 0xEC, 0x76, 0xFB, 0x3D, 0x9E, 0x8F, 0xC7, 0x23,
     0x51, 0x68, 0xF4, 0x7A, 0x3D, 0x5E, 0x2F, 0x57, 0xAB,
     0xD5, 0x2A, 0xD5, 0xEA, 0x75, 0x7A, 0xFD, 0xFE, 0xBF},
    {0x58, 0x2C, 0xD6, 0x6B, 0xB5, 0x1A, 0xCD, 0xE6, 0xB3,
     0x19, 0x4C, 0x26, 0xD3, 0x29, 0x54, 0x2A, 0x15, 0x4A,
     0xE5, 0xF2, 0x79, 0xBC, 0x9E, 0x4F, 0x67, 0xB3, 0xD9},
    {0xD9, 0xEC, 0xB6, 0x5B, 0xAD, 0x16, 0x0B, 0x85, 0x02,
     0xC1, 0x20, 0xD0, 0xA8, 0x94, 0x4A, 0x25, 0x52, 0xE9,
     0x34, 0x1A, 0x0D, 0x46, 0x23, 0x91, 0xC8, 0x64, 0xF2},
    {0xD9, 0x2C, 0x16, 0xCB, 0x25, 0x92, 0x49, 0xA4, 0x92,
     0x89, 0x04, 0x02, 0x01, 0x01, 0x40, 0x20, 0x10, 0x08,
     0x04, 0xC2, 0xA1, 0x10, 0xC8, 0xA4, 0x52, 0x29, 0x94},
    {0x5E, 0x2F, 0x57, 0xAB, 0xD5, 0x2A, 0xD5, 0xEA, 0x75,
     0x7A, 0xFD, 0xFE, 0xBF, 0xDF, 0x2F, 0x97, 0x0B, 0x45,
     0xA2, 0x51, 0xA8, 0x54, 0xEA, 0xB5, 0xDA, 0x6D, 0x76},
    {0x5E, 0xEF, 0xF7, 0x3B, 0x5D, 0xAE, 0x97, 0xCB, 0xE5,
     0x32, 0xD9, 0x2C, 0x16, 0xCB, 0x25, 0x92, 0x49, 0xA4,
     0x92, 0x89, 0x04, 0x02, 0x01, 0x01, 0x40, 0x20, 0x10},
}};

const std::vector<uint8_t> kBertFillGroupStart = {
    0x15, 0x15, 0x02, 0x64, 0x02, 0x15, 0x02, 0x15, 0x15,
    0x15, 0x15, 0x15, 0x15, 0x8F, 0x8F, 0x8F, 0x8F, 0x8F,
    0x8F, 0x8F, 0x8F, 0x8F, 0x8F, 0x8F, 0x8F, 0x8F, 0x8F};

/// One packet of the BERT fill as an encoder would send it: continuity index
/// |ci|, the synchronizing packet every sixty-six (S1,S2 = 65).
std::vector<uint8_t> bert_packet(uint8_t ci, bool synchronizing) {
  const std::vector<uint8_t> block =
      synchronizing ? kBertFillGroupStart
                    : std::vector<uint8_t>(kBertFillBlocks[ci].begin(),
                                           kBertFillBlocks[ci].end());
  return make_packet(0xF00, ci, synchronizing, NabtsSuffixKind::kLongitudinal,
                     block);
}

/// A run of teletext: odd-parity data packets of a small carousel, each
/// different, repeated |cycles| times.
void feed_carousel(Census& census, uint16_t channel, size_t pages,
                   size_t cycles) {
  Bytes bytes(7);
  std::vector<std::vector<uint8_t>> blocks;
  for (size_t p = 0; p < pages; ++p) {
    std::vector<uint8_t> block;
    for (size_t i = 0; i < 27; ++i) {
      block.push_back(parity(bytes.next()));
    }
    blocks.push_back(block);
  }
  uint8_t ci = 0;
  for (size_t c = 0; c < cycles; ++c) {
    for (const auto& block : blocks) {
      census.feed(make_packet(channel, ci, false,
                              NabtsSuffixKind::kLongitudinal, block));
      ci = static_cast<uint8_t>((ci + 1) % 16);
    }
  }
}

////////////////////////////////////////////////////////////////////////////////
// RFC 2728's row code
////////////////////////////////////////////////////////////////////////////////

// A data row of a datacast channel (PS 0xA, the two-byte suffix), exactly as
// recovered: the row code of RFC 2728 Appendix A
// holds over it.
TEST(NabtsServiceCensus, RowSyndromes_PassARowOfADatacastAsRecorded) {
  // The 28 bytes after the prefix: 26 data bytes, then the two check bytes.
  const std::array<uint8_t, 28> block = {
      0xE9, 0x93, 0x3A, 0x04, 0xCC, 0x41, 0x55, 0xCF, 0x83, 0x95,
      0xA1, 0x74, 0x91, 0xAB, 0x5E, 0x8E, 0x60, 0x62, 0xB8, 0x4C,
      0xB5, 0xD7, 0x42, 0x91, 0x32, 0xD6, 0x86, 0x08};
  const auto syndromes = orc::nabts_rfc2728_row_syndromes(block.data());
  EXPECT_EQ(syndromes[0], 0u);
  EXPECT_EQ(syndromes[1], 0u);
}

TEST(NabtsServiceCensus, RowSyndromes_PassEveryRowTheCodeMakesAndNoDamagedOne) {
  Bytes bytes(1998);
  for (int row = 0; row < 32; ++row) {
    std::vector<uint8_t> data;
    for (int i = 0; i < 26; ++i) {
      data.push_back(bytes.next());
    }
    std::vector<uint8_t> codeword = row_codeword(data);
    auto syndromes = orc::nabts_rfc2728_row_syndromes(codeword.data());
    EXPECT_EQ(syndromes[0], 0u) << "row " << row;
    EXPECT_EQ(syndromes[1], 0u) << "row " << row;

    // One wrong byte anywhere leaves both syndromes non-zero: the code's
    // distance is three.
    codeword[static_cast<size_t>(row % 28)] ^= 0x5A;
    syndromes = orc::nabts_rfc2728_row_syndromes(codeword.data());
    EXPECT_NE(syndromes[0], 0u) << "row " << row;
    EXPECT_NE(syndromes[1], 0u) << "row " << row;
  }
}

////////////////////////////////////////////////////////////////////////////////
// Linear complexity and dates
////////////////////////////////////////////////////////////////////////////////

// The 224 bits after the prefix of a datacast test packet, least significant
// bit first, are eleven stages' worth of the O.150 sequence x^11 + x^9 + 1.
TEST(NabtsServiceCensus, LinearComplexity_FindsTheRegisterOfATestStream) {
  const std::array<uint8_t, 28> block = {
      0xA1, 0x4A, 0xC0, 0x82, 0x13, 0xBB, 0xAE, 0x28, 0x14, 0x89,
      0x5A, 0x61, 0xC8, 0xD3, 0x39, 0xBD, 0x93, 0xBA, 0xA1, 0x4E,
      0xE8, 0x92, 0xB2, 0xF1, 0x6E, 0xAA, 0x07, 0x32};
  std::vector<uint8_t> bits;
  for (const uint8_t byte : block) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      bits.push_back(static_cast<uint8_t>((byte >> bit) & 1U));
    }
  }
  uint64_t polynomial = 0;
  EXPECT_EQ(orc::nabts_linear_complexity(bits, polynomial), 11u);
  EXPECT_EQ(polynomial, (1ULL << 0) | (1ULL << 9) | (1ULL << 11));
}

TEST(NabtsServiceCensus, LinearComplexity_OfRandomBitsIsAboutHalfTheirLength) {
  Bytes bytes(31);
  std::vector<uint8_t> bits;
  for (int i = 0; i < 224; ++i) {
    bits.push_back(static_cast<uint8_t>(bytes.next() & 1U));
  }
  uint64_t polynomial = 0;
  const uint32_t complexity = orc::nabts_linear_complexity(bits, polynomial);
  EXPECT_GT(complexity, 100u);
  EXPECT_LT(complexity, 124u);
}

// A datacast clock's time packet: 10.30 pm Eastern.
TEST(NabtsServiceCensus, UtcText_ReadsADatacastClock) {
  EXPECT_EQ(orc::nabts_utc_text(910495807), "1998-11-08 03:30:07");
  EXPECT_EQ(orc::nabts_utc_text(0), "1970-01-01 00:00:00");
  EXPECT_EQ(orc::nabts_utc_text(951782400), "2000-02-29 00:00:00");
}

////////////////////////////////////////////////////////////////////////////////
// What the census names
////////////////////////////////////////////////////////////////////////////////

// A datacast family: sixteen addresses, each sending one check row
// (the 28-byte suffix) ahead of every sixteen data rows of RFC 2728's code.
TEST(NabtsServiceCensus, NamesAFamilyOfFecAddressesAndTheirBundles) {
  Census census;
  Bytes bytes(5);
  for (int bundle = 0; bundle < 3; ++bundle) {
    for (uint16_t channel = 0x0F0; channel <= 0x0FF; ++channel) {
      uint8_t ci = static_cast<uint8_t>((bundle * 17) % 16);
      std::vector<uint8_t> check;
      for (int i = 0; i < 28; ++i) {
        check.push_back(bytes.next());
      }
      census.feed(
          packet_with_block(channel, ci, NabtsSuffixKind::kBundle, check));
      for (int row = 0; row < 16; ++row) {
        ci = static_cast<uint8_t>((ci + 1) % 16);
        std::vector<uint8_t> data;
        for (int i = 0; i < 26; ++i) {
          data.push_back(bytes.next());
        }
        census.feed(packet_with_block(
            channel, ci, NabtsSuffixKind::kLongitudinalPlusReserved,
            row_codeword(data)));
      }
    }
  }
  const auto services = census.services();
  ASSERT_EQ(services.size(), 1u);
  EXPECT_EQ(services[0].kind, NabtsOtherService::Kind::kFecDatacast);
  EXPECT_EQ(services[0].channels, "0F0-0FF");
  EXPECT_EQ(services[0].packets, 16u * 3u * 17u);
  EXPECT_NE(services[0].description.find("16 data rows to each check row"),
            std::string::npos)
      << services[0].description;
  EXPECT_NE(services[0].description.find("100.0 %"), std::string::npos)
      << services[0].description;
}

// Two families side by side — 0E0-0EF and 0F0-0FF — are two services, not one
// run of thirty-two addresses.
TEST(NabtsServiceCensus, ListsNeighbouringFamiliesApart) {
  Census census;
  Bytes bytes(14);
  for (uint16_t channel = 0x0E0; channel <= 0x0FF; ++channel) {
    for (uint8_t row = 0; row < 32; ++row) {
      const auto ci = static_cast<uint8_t>(row % 16);
      std::vector<uint8_t> data;
      for (int i = 0; i < 26; ++i) {
        data.push_back(bytes.next());
      }
      census.feed(packet_with_block(channel, ci,
                                    NabtsSuffixKind::kLongitudinalPlusReserved,
                                    row_codeword(data)));
    }
  }
  const auto services = census.services();
  ASSERT_EQ(services.size(), 2u);
  EXPECT_EQ(services[0].channels, "0E0-0EF");
  EXPECT_EQ(services[1].channels, "0F0-0FF");
  EXPECT_NE(services[0].description.find("no check rows"), std::string::npos)
      << services[0].description;
}

// RFC 2728 §3.3's own bundle, as a datacast address can send it: data at
// continuity indices 0 to 13, the two check rows at 14 and 15.
TEST(NabtsServiceCensus, NamesRfc2728sOwnBundleLayout) {
  Census census;
  Bytes bytes(242);
  for (int bundle = 0; bundle < 4; ++bundle) {
    for (uint8_t ci = 0; ci < 16; ++ci) {
      if (ci < 14) {
        std::vector<uint8_t> data;
        for (int i = 0; i < 26; ++i) {
          data.push_back(bytes.next());
        }
        census.feed(packet_with_block(
            0x242, ci, NabtsSuffixKind::kLongitudinalPlusReserved,
            row_codeword(data)));
      } else {
        std::vector<uint8_t> check;
        for (int i = 0; i < 28; ++i) {
          check.push_back(bytes.next());
        }
        census.feed(
            packet_with_block(0x242, ci, NabtsSuffixKind::kBundle, check));
      }
    }
  }
  const auto services = census.services();
  ASSERT_EQ(services.size(), 1u);
  EXPECT_EQ(services[0].channels, "242");
  EXPECT_NE(
      services[0].description.find(
          "RFC 2728 bundles, 14 data rows and check rows at CI 14 and 15"),
      std::string::npos)
      << services[0].description;
}

TEST(NabtsServiceCensus, NamesTheEncodersBertFillOnF00) {
  Census census;
  for (int group = 0; group < 20; ++group) {
    for (int packet = 0; packet < 66; ++packet) {
      const auto ci = static_cast<uint8_t>((group * 66 + packet) % 16);
      census.feed(bert_packet(ci, /*synchronizing=*/packet == 0));
    }
  }
  const auto services = census.services();
  ASSERT_EQ(services.size(), 1u);
  EXPECT_EQ(services[0].kind, NabtsOtherService::Kind::kIdleFill);
  EXPECT_EQ(services[0].channels, "F00");
  EXPECT_EQ(services[0].packets, 20u * 66u);
  EXPECT_NE(services[0].description.find("17 distinct contents"),
            std::string::npos)
      << services[0].description;
  EXPECT_NE(services[0].description.find("BERT"), std::string::npos);
}

// A carousel of a few pages repeats its packets as often as fill does; its
// bytes are not a shift register's, so it is left to be read as teletext.
TEST(NabtsServiceCensus, DoesNotCallASmallCarouselIdleFill) {
  Census census;
  feed_carousel(census, 0x0A0, /*pages=*/20, /*cycles=*/40);
  EXPECT_TRUE(census.services().empty());
}

TEST(NabtsServiceCensus, NamesATestSequenceAndItsPolynomial) {
  Census census;
  const std::vector<uint8_t> bits = prbs11_bits(28 * 8 * 120);
  for (size_t packet = 0; packet < 100; ++packet) {
    census.feed(packet_with_block(0x300, static_cast<uint8_t>(packet % 16),
                                  NabtsSuffixKind::kNone,
                                  pack_lsb_first(bits, packet * 28 * 8, 28)));
  }
  const auto services = census.services();
  ASSERT_EQ(services.size(), 1u);
  EXPECT_EQ(services[0].kind, NabtsOtherService::Kind::kTestSequence);
  EXPECT_EQ(services[0].channels, "300");
  EXPECT_NE(services[0].description.find("x^11 + x^9 + 1"), std::string::npos)
      << services[0].description;
}

// A run of one byte has a complexity of a few stages too, and is not a test
// sequence.
TEST(NabtsServiceCensus, DoesNotCallARunOfOneByteATestSequence) {
  Census census;
  for (uint8_t packet = 0; packet < 100; ++packet) {
    census.feed(packet_with_block(0x300, packet % 16, NabtsSuffixKind::kNone,
                                  std::vector<uint8_t>(28, 0x80)));
  }
  EXPECT_TRUE(census.services().empty());
}

// A clock sent on EF0 and FC0 alike is listed once for both.
TEST(NabtsServiceCensus, ListsOneClockOnTheAddressesThatCarryIt) {
  Census census;
  uint32_t time = 910495807;
  for (int packet = 0; packet < 40; ++packet) {
    const bool name = packet % 5 == 0;
    const auto block = name ? clock_name_block() : clock_time_block(time);
    for (const uint16_t channel : {uint16_t{0xEF0}, uint16_t{0xFC0}}) {
      census.feed(packet_with_block(channel, static_cast<uint8_t>(packet % 16),
                                    NabtsSuffixKind::kNone, block));
    }
    if (!name) {
      time += 1;
    }
  }
  const auto services = census.services();
  ASSERT_EQ(services.size(), 1u);
  EXPECT_EQ(services[0].kind, NabtsOtherService::Kind::kClock);
  EXPECT_EQ(services[0].channels, "EF0, FC0");
  EXPECT_EQ(services[0].packets, 80u);
  EXPECT_NE(services[0].description.find(
                "1998-11-08 03:30:07 to 1998-11-08 03:30:38 UTC"),
            std::string::npos)
      << services[0].description;
  EXPECT_NE(services[0].description.find("\"PBS\""), std::string::npos);
}

// A clock's first readings on EF0 and FC0 can be a second apart; it is still
// one clock, listed over both spans.
TEST(NabtsServiceCensus, ListsAClockWhoseAddressesBeganASecondApartOnce) {
  Census census;
  for (int packet = 0; packet < 40; ++packet) {
    census.feed(packet_with_block(
        0xEF0, static_cast<uint8_t>(packet % 16), NabtsSuffixKind::kNone,
        clock_time_block(910495806U + static_cast<uint32_t>(packet))));
    census.feed(packet_with_block(
        0xFC0, static_cast<uint8_t>(packet % 16), NabtsSuffixKind::kNone,
        clock_time_block(910495807U + static_cast<uint32_t>(packet))));
  }
  const auto services = census.services();
  ASSERT_EQ(services.size(), 1u);
  EXPECT_EQ(services[0].channels, "EF0, FC0");
  EXPECT_NE(services[0].description.find(
                "1998-11-08 03:30:06 to 1998-11-08 03:30:46 UTC"),
            std::string::npos)
      << services[0].description;
}

// A time read wrong falls into line with neither neighbour, and stays out of
// the span, as a time misread a year on does.
TEST(NabtsServiceCensus, LeavesAMisreadTimeOutOfTheClocksSpan) {
  Census census;
  uint32_t time = 910495807;
  for (int packet = 0; packet < 40; ++packet) {
    const uint32_t sent = packet == 20 ? 920373260U : time;
    census.feed(packet_with_block(0xEF0, static_cast<uint8_t>(packet % 16),
                                  NabtsSuffixKind::kNone,
                                  clock_time_block(sent)));
    time += 1;
  }
  const auto services = census.services();
  ASSERT_EQ(services.size(), 1u);
  EXPECT_NE(services[0].description.find(
                "1998-11-08 03:30:07 to 1998-11-08 03:30:46 UTC"),
            std::string::npos)
      << services[0].description;
}

// An address corrected into the one it reads as may be a neighbour's, so only
// addresses that arrived as transmitted are counted.
TEST(NabtsServiceCensus, CountsOnlyAddressesThatArrivedAsTransmitted) {
  Census census;
  for (int group = 0; group < 20; ++group) {
    for (int packet = 0; packet < 66; ++packet) {
      const auto ci = static_cast<uint8_t>((group * 66 + packet) % 16);
      std::vector<uint8_t> bytes = bert_packet(ci, packet == 0);
      bytes[0] = static_cast<uint8_t>(bytes[0] ^ 0x01);  // one bit off
      census.feed(bytes);
    }
  }
  EXPECT_TRUE(census.services().empty());
}

TEST(NabtsServiceCensus, SaysNothingOfTeletext) {
  Census census;
  feed_carousel(census, 0x000, /*pages=*/600, /*cycles=*/2);
  EXPECT_TRUE(census.services().empty());
}

////////////////////////////////////////////////////////////////////////////////
// Text in the provider's own data groups
////////////////////////////////////////////////////////////////////////////////

/// A data group of |type| on |channel| whose data is |text| sent as seven-bit
/// characters with odd parity, after a short binary header of the provider's
/// own, as teletype groups are.
orc::NabtsDataGroup text_group(uint16_t channel, uint8_t type,
                               const std::string& text) {
  orc::NabtsDataGroup group;
  group.channel = channel;
  group.channel_attested = true;
  group.header.valid = true;
  group.header.attested = true;
  group.header.type = type;
  group.header.continuity = 3;
  group.packets = 4;
  group.data = {hamming(1), hamming(2), hamming(9)};
  for (const char c : text) {
    group.data.push_back(parity(static_cast<uint8_t>(c)));
  }
  return group;
}

TEST(NabtsPrivateTextLog, KeepsTheTextOfPrivateUseGroups) {
  orc::NabtsPrivateTextLog log;
  log.add_group(
      text_group(0xCD0, 15, "CALL CTV\r\nFM CND2 44\r\nNETWORK NEWS  7:30\r\n"),
      1234);
  const auto services = log.services();
  ASSERT_EQ(services.size(), 1u);
  EXPECT_EQ(services[0].kind, NabtsOtherService::Kind::kPrivateText);
  EXPECT_EQ(services[0].channels, "CD0");
  EXPECT_EQ(services[0].packets, 4u);
  EXPECT_NE(services[0].description.find("1 data groups of type 15"),
            std::string::npos)
      << services[0].description;

  const std::string document = log.document();
  EXPECT_NE(document.find("--- CD0  frame 1234  type 15  group 3\n"),
            std::string::npos)
      << document;
  EXPECT_NE(document.find("CALL CTV\nFM CND2 44\nNETWORK NEWS  7:30\n"),
            std::string::npos)
      << document;
}

TEST(NabtsPrivateTextLog, ShowsBytesLostWithTheirPacket) {
  orc::NabtsDataGroup group =
      text_group(0xCFD, 15, "STATIONS: PLEASE STAND BY FOR A NEWS SPECIAL");
  group.present.assign(group.data.size(), orc::kNabtsByteArrived);
  for (size_t i = 20; i < 26; ++i) {
    group.data[i] = 0;
    group.present[i] = orc::kNabtsByteLost;
  }
  orc::NabtsPrivateTextLog log;
  log.add_group(group, 1);
  // Data bytes 20-25 are text characters 17-22 after the three header bytes.
  EXPECT_NE(log.document().find("STATIONS: PLEASE [...]BY FOR A NEWS SPECIAL"),
            std::string::npos)
      << log.document();
}

// Other private-use groups, on A00, can be Hamming-coded control bytes,
// and a teletext group is the record layer's: neither is text here.
TEST(NabtsPrivateTextLog, LeavesControlGroupsAndTeletextAlone) {
  orc::NabtsPrivateTextLog log;
  orc::NabtsDataGroup control = text_group(0xA00, 15, "");
  for (uint8_t nibble = 0; nibble < 30; ++nibble) {
    control.data.push_back(hamming(static_cast<uint8_t>(nibble % 16)));
  }
  log.add_group(control, 1);
  log.add_group(text_group(0x000, 0, "A TELETEXT RECORD OF TEXT"), 2);
  orc::NabtsDataGroup misread = text_group(0xCD0, 15, "SOME TEXT TO READ HERE");
  misread.header.attested = false;
  log.add_group(misread, 3);
  EXPECT_TRUE(log.services().empty());
  EXPECT_TRUE(log.document().empty());
}

TEST(NabtsServiceCensus, Summary_ListsEachServiceUnderItsAddresses) {
  NabtsOtherService fill;
  fill.kind = NabtsOtherService::Kind::kIdleFill;
  fill.channels = "F00";
  fill.packets = 28962;
  fill.description = "17 distinct contents repeated";
  NabtsOtherService clock;
  clock.kind = NabtsOtherService::Kind::kClock;
  clock.channels = "EF0, FC0";
  clock.packets = 3874;
  clock.description = "Hamming-coded time of day";

  const std::string summary = orc::nabts_other_services_summary({fill, clock});
  EXPECT_EQ(summary,
            "Other services (not teletext)\n"
            "  F00       idle fill: 28962 packets, 17 distinct contents "
            "repeated\n"
            "  EF0, FC0  clock: 3874 packets, Hamming-coded time of day\n");
  EXPECT_TRUE(orc::nabts_other_services_summary({}).empty());
}

}  // namespace
}  // namespace orc_unit_test
