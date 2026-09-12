/*
 * File:        vbi_bcd.h
 * Module:      orc-core
 * Purpose:     Binary Coded Decimal decoding for IEC 60856/60857 VBI words
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>

namespace orc {

/**
 * @brief Decode a Binary Coded Decimal field from a VBI word
 *
 * Digits are read from the least significant nibble upwards. A nibble outside
 * 0-9 is not a decimal digit, so the whole field is rejected: this is the only
 * error detection the encoding itself offers.
 *
 * @param bcd    The (already masked) BCD field
 * @param output Receives the decoded value on success
 * @return true when every nibble was a valid decimal digit
 */
inline bool decode_vbi_bcd(uint32_t bcd, int32_t& output) {
  output = 0;
  int32_t multiplier = 1;

  while (bcd > 0) {
    uint32_t digit = bcd & 0x0F;
    if (digit > 9) {
      return false;  // Invalid BCD digit
    }
    output += static_cast<int32_t>(digit) * multiplier;
    multiplier *= 10;
    bcd >>= 4;
  }

  return true;
}

}  // namespace orc
