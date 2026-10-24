/*
 * File:        vbi_byte_source.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Random-access byte stream interface for raw VBI captures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_BYTE_SOURCE_H
#define ORC_VBI_BYTE_SOURCE_H

#include <cstdint>
#include <optional>
#include <string>

namespace orc {

// The raw byte sequence of a capture, however it is transported.
//
// A plain .vbi file and a FLAC-wrapped .vbi.flac present exactly the same
// bytes through this interface; FLAC is a lossless byte compressor here, not
// audio, so unwrapping is purely a transport concern (design §3.3).  Every
// consumer above this interface is therefore transport-agnostic, and unit
// tests substitute an in-memory implementation instead of touching disk.
//
// Implementations are not required to be thread-safe: a decoder holds a
// stream position, so callers must serialise access.
class IVBIByteSource {
 public:
  virtual ~IVBIByteSource() = default;

  // Total bytes the source can yield, or nullopt when the transport cannot
  // determine it without decoding the whole stream.
  virtual std::optional<uint64_t> size_bytes() const = 0;

  // Copy count bytes starting at byte_offset into out_buffer, which must have
  // room for them.  Returns the number of bytes actually produced; a short
  // result means end of stream, which is not in itself an error.  Returns 0
  // with a non-empty error_message when the transport failed.
  virtual size_t read_at(uint64_t byte_offset, size_t count,
                         uint8_t* out_buffer, std::string& error_message) = 0;

  // Bits per sample the transport declares, when it declares one at all.  For
  // FLAC this is the single header field that is trustworthy and seeds
  // sample-format detection; the declared sample rate never is (design §3.3).
  virtual std::optional<uint32_t> declared_bits_per_sample() const {
    return std::nullopt;
  }

  // Samples per line record the transport declares, when its container has
  // records of its own.  A capture carried in a video file stores one record
  // per picture row, so the picture width is the record length, and a format
  // whose records are another length cannot be read from it.  A flat stream —
  // a raw file or a FLAC wrapper — has no rows and declares nothing.
  virtual std::optional<uint32_t> declared_record_samples() const {
    return std::nullopt;
  }

  // Sample bytes this source has produced over its lifetime, for diagnostics.
  // It is how a caller distinguishes a random access that seeked from one
  // that re-decoded the stream from its head.
  virtual uint64_t bytes_decoded() const { return 0; }

  // How far past its current decode position a read may start before the
  // transport repositions instead of decoding forward through the gap.
  //
  // A reader that skips ahead over stretches another reader covers — one of
  // several decoders each taking its own chunks of a capture — sets this to
  // zero, because every byte it would decode in the gap is one another
  // decoder is producing anyway.  Transports that hold no decode position
  // ignore it, which is the default.
  virtual void set_forward_decode_limit(uint64_t bytes) { (void)bytes; }
};

}  // namespace orc

#endif  // ORC_VBI_BYTE_SOURCE_H
