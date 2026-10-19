/*
 * File:        nabts_decompress.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     The two compressions a framed datacast service's containers
 *              use: zlib's (RFC 1950/1951) and the implode of PKWARE's Data
 *              Compression Library
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_DECOMPRESS_H
#define ORC_NABTS_DECOMPRESS_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace orc {

/// What decompressing a stream came to.
struct NabtsDecompressed {
  /// The bytes the stream gave, as far as it could be read.
  std::vector<uint8_t> bytes;
  /// The stream's bytes it took.
  size_t used = 0;
  /// The stream reached its end as its format ends one.
  bool ended = false;
  /// It did, and its own check — zlib's Adler-32 — held where it has one:
  /// nothing of it lost or read wrong that the format can tell.
  bool complete = false;
};

/**
 * @brief A zlib stream (RFC 1950) inflated (RFC 1951)
 *
 * Its two-byte header, its deflate blocks — stored, and coded by the fixed
 * codes or by codes of their own — and the Adler-32 of what they give. A
 * stream that breaks off, or reads as none, gives what it had given before it
 * did; at most |limit| bytes are given. Written after the method of zlib's
 * puff, by Mark Adler.
 *
 * Pure: no I/O.
 */
NabtsDecompressed nabts_inflate(const uint8_t* data, size_t size, size_t limit);

/**
 * @brief A stream imploded by PKWARE's Data Compression Library, exploded
 *
 * Its two-byte header — whether its literals are coded, and its dictionary's
 * size, 1, 2 or 4 KiB — then literals and matches, a bit telling which, to its
 * end code. The format carries no check of its own. A stream that breaks off
 * gives what it had given before it did; at most |limit| bytes are given.
 * Written after the method of zlib's blast, by Mark Adler.
 *
 * Pure: no I/O.
 */
NabtsDecompressed nabts_explode(const uint8_t* data, size_t size, size_t limit);

/// The CRC-32 of |data|, as ISO 3309, zlib and PKZIP give it.
uint32_t nabts_crc32(const uint8_t* data, size_t size);

}  // namespace orc

#endif  // ORC_NABTS_DECOMPRESS_H
