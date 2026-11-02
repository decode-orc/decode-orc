/*
 * File:        nabts_bytes.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Little-endian values in the bytes a datacast carried
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_BYTES_H
#define ORC_NABTS_BYTES_H

#include <cstdint>

namespace orc {

/// The 16-bit value at |p|, its low byte first, as the framed services'
/// frames, tables and archives store their numbers.
constexpr uint16_t nabts_le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8U));
}

/// The 32-bit value at |p|, its low byte first.
constexpr uint32_t nabts_le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
         (static_cast<uint32_t>(p[2]) << 16U) |
         (static_cast<uint32_t>(p[3]) << 24U);
}

}  // namespace orc

#endif  // ORC_NABTS_BYTES_H
