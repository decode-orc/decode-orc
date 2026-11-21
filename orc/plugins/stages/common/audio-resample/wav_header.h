/*
 * File:        wav_header.h
 * Module:      orc-audio-resample (shared stage-plugin library, header-only)
 * Purpose:     RIFF/WAVE and RF64 header parsing with 64-bit sizes,
 *              detection of 32-bit sizes that have wrapped, and building
 *              PCM headers that switch to RF64 past the RIFF limit
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace orc {
namespace wav {

// Reads up to |length| bytes at |offset| into |out|; returns the count read.
using ByteReader =
    std::function<size_t(uint64_t offset, uint8_t* out, size_t length)>;

// Where the samples are and how they are laid out. data_bytes is the full
// 64-bit payload size: from the ds64 chunk for RF64 (EBU Tech 3306) and
// BW64 (ITU-R BS.2088), from the data chunk header for RIFF.
struct WavLayout {
  bool valid = false;
  std::string error;  // human-readable reason when !valid
  bool rf64 = false;  // RF64/BW64 container (sizes from ds64)
  // RIFF only: the file holds at least 4 GiB more after the declared data
  // than the data chunk header says, so the 32-bit size has wrapped and the
  // header cannot describe the file. valid is false.
  bool size_wrapped = false;
  uint16_t format_tag = 0;
  uint16_t channels = 0;
  uint32_t sample_rate_hz = 0;
  uint16_t bits_per_sample = 0;
  uint64_t fmt_offset = 0;   // file offset of the "fmt " chunk header
  uint64_t data_offset = 0;  // file offset of the first sample byte
  uint64_t data_bytes = 0;
};

namespace detail {

inline uint16_t le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t le64(const uint8_t* p) {
  return static_cast<uint64_t>(le32(p)) |
         (static_cast<uint64_t>(le32(p + 4)) << 32);
}

}  // namespace detail

// The 32-bit size value RF64 writes where a size does not fit.
constexpr uint32_t kRf64SizePlaceholder = 0xFFFFFFFFu;

// Parse the header of a WAV file of |file_size| bytes. Walks the chunk list
// for "fmt " and "data" (and "ds64" in RF64/BW64). Does not judge the sample
// format: the caller checks format_tag, channels, rate and bit depth.
inline WavLayout parse_header(const ByteReader& read_at, uint64_t file_size) {
  using detail::le16;
  using detail::le32;
  using detail::le64;
  WavLayout layout;

  std::array<uint8_t, 12> riff{};
  if (read_at(0, riff.data(), riff.size()) != riff.size() ||
      std::memcmp(riff.data() + 8, "WAVE", 4) != 0) {
    layout.error = "not a RIFF/WAVE file";
    return layout;
  }
  if (std::memcmp(riff.data(), "RF64", 4) == 0 ||
      std::memcmp(riff.data(), "BW64", 4) == 0) {
    layout.rf64 = true;
  } else if (std::memcmp(riff.data(), "RIFF", 4) != 0) {
    layout.error = "not a RIFF/WAVE file";
    return layout;
  }

  bool have_fmt = false;
  bool have_data = false;
  bool have_ds64 = false;
  uint64_t ds64_data_bytes = 0;
  uint32_t data_chunk_size = 0;
  uint64_t pos = 12;

  while (!(have_fmt && have_data) && pos + 8 <= file_size) {
    std::array<uint8_t, 8> header{};
    if (read_at(pos, header.data(), header.size()) != header.size()) break;
    const uint32_t chunk_size = le32(header.data() + 4);
    const uint64_t payload = pos + 8;

    if (std::memcmp(header.data(), "ds64", 4) == 0) {
      // ds64: riffSize, dataSize, sampleCount (64-bit each), then a table.
      std::array<uint8_t, 24> ds64{};
      if (chunk_size < ds64.size() ||
          read_at(payload, ds64.data(), ds64.size()) != ds64.size()) {
        layout.error = "malformed ds64 chunk";
        return layout;
      }
      ds64_data_bytes = le64(ds64.data() + 8);
      have_ds64 = true;
    } else if (std::memcmp(header.data(), "fmt ", 4) == 0) {
      std::array<uint8_t, 16> fmt{};
      if (chunk_size < fmt.size() ||
          read_at(payload, fmt.data(), fmt.size()) != fmt.size()) {
        layout.error = "malformed fmt chunk";
        return layout;
      }
      layout.fmt_offset = pos;
      layout.format_tag = le16(fmt.data());
      layout.channels = le16(fmt.data() + 2);
      layout.sample_rate_hz = le32(fmt.data() + 4);
      layout.bits_per_sample = le16(fmt.data() + 14);
      have_fmt = true;
    } else if (std::memcmp(header.data(), "data", 4) == 0) {
      layout.data_offset = payload;
      data_chunk_size = chunk_size;
      have_data = true;
      if (layout.rf64 && chunk_size == kRf64SizePlaceholder) {
        // The real size is in ds64, and the walk cannot step over the data.
        if (have_fmt) break;
        if (!have_ds64) break;
        pos = payload + ds64_data_bytes + (ds64_data_bytes & 1u);
        continue;
      }
    }
    // Chunks are word-aligned: odd sizes carry one pad byte.
    pos = payload + chunk_size + (chunk_size & 1u);
  }

  if (!have_fmt || !have_data) {
    layout.error = "missing fmt or data chunk";
    return layout;
  }

  if (layout.rf64) {
    if (data_chunk_size == kRf64SizePlaceholder) {
      if (!have_ds64) {
        layout.error = "RF64 file without a ds64 chunk";
        return layout;
      }
      layout.data_bytes = ds64_data_bytes;
    } else {
      layout.data_bytes = data_chunk_size;
    }
  } else {
    layout.data_bytes = data_chunk_size;
    // A RIFF writer that counts in 32 bits stores the size modulo 2^32. The
    // samples are all there, but the header describes only the remainder.
    const uint64_t available =
        file_size > layout.data_offset ? file_size - layout.data_offset : 0;
    if (available > layout.data_bytes &&
        available - layout.data_bytes >= (uint64_t{1} << 32)) {
      layout.size_wrapped = true;
      layout.error =
          "the file is " + std::to_string(file_size) +
          " bytes but its 32-bit RIFF header declares " +
          std::to_string(layout.data_bytes) +
          " bytes of samples: the size has wrapped past 4 GiB, so the header "
          "cannot describe the file (a WAV this large needs RF64)";
      return layout;
    }
  }

  layout.valid = true;
  return layout;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

// The largest data chunk a RIFF file can describe: the RIFF size field
// (36 + data bytes for the canonical 44-byte header) is 32 bits.
constexpr uint64_t kRiffMaxDataBytes = uint64_t{0xFFFFFFFFu} - 36;

// True when a canonical 44-byte RIFF header can describe |data_bytes|.
constexpr bool riff_can_describe(uint64_t data_bytes) {
  return data_bytes <= kRiffMaxDataBytes;
}

// Size of the header build_pcm_header() writes for |data_bytes|.
constexpr size_t pcm_header_bytes(uint64_t data_bytes) {
  return riff_can_describe(data_bytes) ? 44 : 80;
}

namespace detail {

inline void put_le16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

inline void put_le32(std::vector<uint8_t>& out, uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<uint8_t>((v >> shift) & 0xFF));
  }
}

inline void put_le64(std::vector<uint8_t>& out, uint64_t v) {
  put_le32(out, static_cast<uint32_t>(v & 0xFFFFFFFFu));
  put_le32(out, static_cast<uint32_t>(v >> 32));
}

inline void put_tag(std::vector<uint8_t>& out, const char (&tag)[5]) {
  out.insert(out.end(), tag, tag + 4);
}

}  // namespace detail

// Header for integer PCM (format tag 1) with |data_bytes| of samples.
//
// Up to kRiffMaxDataBytes this is the canonical 44-byte RIFF header. Past it,
// an RF64 header (EBU Tech 3306): "RF64", a ds64 chunk carrying the 64-bit
// RIFF size, data size and sample-frame count, the fmt chunk, and a data
// chunk whose 32-bit size is the 0xFFFFFFFF placeholder. 80 bytes.
inline std::vector<uint8_t> build_pcm_header(uint16_t channels,
                                             uint32_t sample_rate_hz,
                                             uint16_t bits_per_sample,
                                             uint64_t data_bytes) {
  using detail::put_le16;
  using detail::put_le32;
  using detail::put_le64;
  using detail::put_tag;
  const uint16_t block_align =
      static_cast<uint16_t>(channels * ((bits_per_sample + 7) / 8));
  const bool rf64 = !riff_can_describe(data_bytes);

  std::vector<uint8_t> header;
  header.reserve(pcm_header_bytes(data_bytes));
  if (!rf64) {
    put_tag(header, "RIFF");
    put_le32(header, static_cast<uint32_t>(36 + data_bytes));
    put_tag(header, "WAVE");
  } else {
    // RIFF size: everything after the 8-byte RIFF chunk header, plus the
    // data chunk's pad byte when the payload is odd.
    const uint64_t riff_size =
        4 + (8 + 28) + (8 + 16) + 8 + data_bytes + (data_bytes & 1u);
    put_tag(header, "RF64");
    put_le32(header, kRf64SizePlaceholder);
    put_tag(header, "WAVE");
    put_tag(header, "ds64");
    put_le32(header, 28);
    put_le64(header, riff_size);
    put_le64(header, data_bytes);
    put_le64(header, block_align ? data_bytes / block_align : 0);
    put_le32(header, 0);  // no table entries
  }
  put_tag(header, "fmt ");
  put_le32(header, 16);
  put_le16(header, 1);  // PCM
  put_le16(header, channels);
  put_le32(header, sample_rate_hz);
  put_le32(header, sample_rate_hz * block_align);
  put_le16(header, block_align);
  put_le16(header, bits_per_sample);
  put_tag(header, "data");
  put_le32(header,
           rf64 ? kRf64SizePlaceholder : static_cast<uint32_t>(data_bytes));
  return header;
}

}  // namespace wav
}  // namespace orc
