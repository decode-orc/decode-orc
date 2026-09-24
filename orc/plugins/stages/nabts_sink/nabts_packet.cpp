/*
 * File:        nabts_packet.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     NABTS data packet decoding implementation (CEA-516 §3)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_packet.h"

#include <cmath>

#include "vbi-services/teletext_page_decoder.h"

namespace orc {

namespace {

// CEA-516 numbers the bits of a byte b1 to b8 with b8 transmitted last, and
// §5.2.8.3 states plainly that b8 is the most significant of the four
// information bits. The Hamming 8/4 code is the one ETSI EN 300 706 §8.2
// specifies (§3.2.2 says as much), and teletext_hamming84_decode() returns that
// nibble with the bit at transmission position 8 as its most significant. So
// the decoded nibble's bits, most significant first, are b8 b6 b4 b2 — which is
// what these accessors say, once rather than at every use.
constexpr uint8_t bit_b2(int nibble) {
  return static_cast<uint8_t>(nibble & 0x1);
}
constexpr uint8_t bit_b4(int nibble) {
  return static_cast<uint8_t>((nibble >> 1) & 0x1);
}
constexpr uint8_t bit_b6(int nibble) {
  return static_cast<uint8_t>((nibble >> 2) & 0x1);
}
constexpr uint8_t bit_b8(int nibble) {
  return static_cast<uint8_t>((nibble >> 3) & 0x1);
}

// PS b8 and b6 as the two-bit suffix code of §3.4, b8 the more significant.
NabtsSuffixKind suffix_from_ps(int ps) {
  switch ((bit_b8(ps) << 1) | bit_b6(ps)) {
    case 0:
      return NabtsSuffixKind::kNone;
    case 1:
      return NabtsSuffixKind::kLongitudinal;
    case 2:
      return NabtsSuffixKind::kLongitudinalPlusReserved;
    default:
      return NabtsSuffixKind::kBundle;
  }
}

/**
 * @brief Check, and where possible repair, one data block against its suffix
 *
 * @param bytes  Data block followed by suffix, contiguous as transmitted
 * @param count  Data block plus suffix length, i.e. 28 for every packet that
 *               has a suffix at all
 * @param data   Receives the data block, corrected if it could be
 * @param data_length Data-block bytes, so the correction can be reported only
 *               when it landed in the block rather than in the suffix
 *
 * CEA-516 §3.4: the longitudinal byte is chosen so that the exclusive-or of the
 * data block and the suffix is 0xFF. The syndrome — that exclusive-or against
 * 0xFF — is therefore zero on a clean block, and on a block with one wrong bit
 * it has exactly that bit set. Which byte the bit is in comes from the per-byte
 * odd parity §3.3 requires: exactly one byte will have failed it.
 *
 * Two wrong bits in one byte leave the byte's parity intact and put two bits in
 * the syndrome; two in different bytes fail two parities. Neither is localised,
 * and both are reported rather than guessed at.
 */
NabtsBlockIntegrity check_and_repair(
    const uint8_t* bytes, size_t count,
    std::array<uint8_t, kNabtsMaxDataBlockBytes>& data, size_t data_length) {
  uint8_t longitudinal = 0;
  for (size_t i = 0; i < count; ++i) {
    longitudinal ^= bytes[i];
  }
  const uint8_t syndrome = static_cast<uint8_t>(longitudinal ^ 0xFF);
  if (syndrome == 0) {
    return NabtsBlockIntegrity::kClean;
  }

  // One bit set means one bit position is wrong across the whole block. Any
  // other syndrome is multiple bits, which this code detects but cannot place.
  const bool one_bit = (syndrome & static_cast<uint8_t>(syndrome - 1)) == 0;
  if (!one_bit) {
    return NabtsBlockIntegrity::kUncorrectable;
  }

  size_t failing = count;
  size_t failures = 0;
  for (size_t i = 0; i < count; ++i) {
    if (!teletext_odd_parity_valid(bytes[i])) {
      failing = i;
      ++failures;
    }
  }
  if (failures != 1) {
    return NabtsBlockIntegrity::kUncorrectable;
  }

  // Located. A correction that lands in the suffix leaves the data block as
  // transmitted and is still a correction: the block is now known good.
  if (failing < data_length) {
    data[failing] = static_cast<uint8_t>(data[failing] ^ syndrome);
  }
  return NabtsBlockIntegrity::kCorrected;
}

/// One byte of TeletextLineResult::byte_confidence on the 0-255 scale
/// NabtsPacket::confidence uses. A null measurement is full confidence: see
/// NabtsPacket::confidence for why the threshold detector reads that way.
uint8_t quantise_confidence(const TeletextPacketConfidence* confidence,
                            size_t index) {
  if (confidence == nullptr || index >= confidence->size()) {
    return 255;
  }
  const float value = (*confidence)[index];
  if (!(value > 0.0F)) {
    return 0;  // also catches a NaN, which is not a measurement of anything
  }
  if (value >= 1.0F) {
    return 255;
  }
  return static_cast<uint8_t>(std::lround(value * 255.0F));
}

}  // namespace

NabtsPacket nabts_decode_packet(const uint8_t* packet, size_t length,
                                const TeletextPacketConfidence* confidence) {
  NabtsPacket out;
  if (packet == nullptr || length < kNabtsPacketBytes) {
    return out;
  }

  // §3.2.2: every prefix byte is Hamming 8/4, so one uncorrectable byte makes
  // the whole prefix unusable — a packet whose channel did not decode cannot be
  // filed under a channel, and one whose PS did not decode has no known data
  // extent to read.
  std::array<int, kNabtsPrefixBytes> prefix{};
  for (size_t i = 0; i < kNabtsPrefixBytes; ++i) {
    prefix[i] = teletext_hamming84_decode(packet[i]);
    if (prefix[i] < 0) {
      return out;
    }
  }

  out.valid = true;
  // §3.2.3: P1 is the most significant of the three hexadecimal digits.
  out.channel =
      static_cast<uint16_t>((prefix[0] << 8) | (prefix[1] << 4) | prefix[2]);
  out.address_attested = teletext_hamming84_clean(packet[0]) &&
                         teletext_hamming84_clean(packet[1]) &&
                         teletext_hamming84_clean(packet[2]);
  out.continuity = static_cast<uint8_t>(prefix[3]);
  out.continuity_byte = packet[3];

  const int ps = prefix[4];
  out.synchronizing = bit_b2(ps) != 0;
  out.not_full = bit_b4(ps) != 0;
  out.suffix = suffix_from_ps(ps);
  out.data_length = nabts_data_block_bytes(out.suffix);

  for (size_t i = 0; i < out.data_length; ++i) {
    out.data[i] = packet[kNabtsPrefixBytes + i];
    out.confidence[i] = quantise_confidence(confidence, kNabtsPrefixBytes + i);
  }

  // A bundle packet is all suffix (§3.4) and its protection method is reserved,
  // so there is nothing to check it against and no block to check. Its
  // continuity index still counts, which is the caller's business and is why
  // the packet is returned valid rather than dropped.
  if (out.suffix == NabtsSuffixKind::kNone ||
      out.suffix == NabtsSuffixKind::kBundle) {
    out.integrity = NabtsBlockIntegrity::kUnchecked;
    return out;
  }

  out.integrity =
      check_and_repair(packet + kNabtsPrefixBytes, kNabtsMaxDataBlockBytes,
                       out.data, out.data_length);
  return out;
}

}  // namespace orc
