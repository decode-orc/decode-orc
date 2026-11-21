/*
 * File:        tbc_sink_sidecars.h
 * Module:      orc-stage-plugin-tbc-sink
 * Purpose:     Layout arithmetic for the .pcm and .efm sidecars a TBC export
 *              writes alongside the .tbc and .tbc.db
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/common_types.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace orc {

// ---------------------------------------------------------------------------
// Sidecar paths
// ---------------------------------------------------------------------------
// ld-decode names the sidecars off the base, with the .tbc extension replaced
// rather than extended — "foo.tbc" pairs with "foo.pcm" and "foo.efm" (the
// metadata database is the exception: "foo.tbc.db").  This is the layout
// tbc_source auto-detects, so an export drops straight back in as a source.

// Strip a trailing ".tbc" from |tbc_path| to get the sidecar base.
inline std::string tbc_sidecar_base(const std::string& tbc_path) {
  constexpr std::string_view kExt = ".tbc";
  if (tbc_path.size() >= kExt.size() &&
      tbc_path.compare(tbc_path.size() - kExt.size(), kExt.size(), kExt) == 0) {
    return tbc_path.substr(0, tbc_path.size() - kExt.size());
  }
  return tbc_path;
}

// ---------------------------------------------------------------------------
// Analogue audio (.pcm)
// ---------------------------------------------------------------------------
// The ld-decode analogue audio sidecar is headerless signed 16-bit
// little-endian stereo at 44100 Hz.  Pipeline audio is 48000 Hz synchronous
// 24-bit-in-int32 (SMPTE 272M-1994; audio_channel_pair.h), so an export
// resamples and narrows.

inline constexpr int32_t kTbcPcmSampleRateHz = 44100;
inline constexpr int32_t kTbcPcmBits = 16;

// 48000 → 44100 is exactly 160:147, so the whole layout is integer arithmetic
// and no rounding drift accumulates across a long export.
inline constexpr uint64_t kTbcPcmRateNum = 147;
inline constexpr uint64_t kTbcPcmRateDen = 160;

// The .pcm stereo-pair offset corresponding to a pipeline (48 kHz) offset,
// rounded half up.
inline constexpr uint64_t tbc_pcm_pair_offset(uint64_t pipeline_pair_offset) {
  return (pipeline_pair_offset * kTbcPcmRateNum + kTbcPcmRateDen / 2) /
         kTbcPcmRateDen;
}

// Total .pcm stereo pairs for |frame_count| frames of |system|.
inline uint64_t tbc_pcm_total_pairs(VideoSystem system, size_t frame_count) {
  return tbc_pcm_pair_offset(audio_pair_offset(frame_count, system));
}

// The .pcm stereo-pair counts of frame |frame|'s two fields (exported field
// order: frame n covers fields 2n and 2n+1), for the `audio_samples` column
// of each field record.
//
// The counts are derived from the cumulative offsets rather than from a
// per-frame rate, so over any run of frames they sum to exactly
// tbc_pcm_total_pairs() and the metadata always describes the file that was
// written.  A frame's odd pair goes to its first field.
struct TbcPcmFieldPairs {
  int32_t first_field = 0;
  int32_t second_field = 0;
};

inline TbcPcmFieldPairs tbc_pcm_frame_field_pairs(VideoSystem system,
                                                  size_t frame) {
  const uint64_t in_frame =
      tbc_pcm_pair_offset(audio_pair_offset(frame + 1, system)) -
      tbc_pcm_pair_offset(audio_pair_offset(frame, system));
  return {static_cast<int32_t>((in_frame + 1) / 2),
          static_cast<int32_t>(in_frame / 2)};
}

// Narrow one 24-bit-in-int32 pipeline sample to the sidecar's 16-bit form.
// Exactly inverts AudioResampler::widen_16_to_24 (<< 8); out-of-range values
// saturate rather than wrap.
inline int16_t tbc_pcm_narrow_24_to_16(int32_t sample) {
  return static_cast<int16_t>(std::clamp(sample >> 8, -32768, 32767));
}

// Pack |pair_count| interleaved stereo pairs of 24-bit-in-int32 audio into
// signed 16-bit little-endian bytes.  Short input is silence-padded so the
// written run always matches the count the metadata declares.
inline std::vector<uint8_t> tbc_pcm_pack_s16le(
    const std::vector<int32_t>& interleaved, size_t pair_count) {
  std::vector<uint8_t> bytes(pair_count * 4, 0);
  const size_t values = std::min(interleaved.size(), pair_count * 2);
  for (size_t i = 0; i < values; ++i) {
    const uint16_t v =
        static_cast<uint16_t>(tbc_pcm_narrow_24_to_16(interleaved[i]));
    bytes[i * 2] = static_cast<uint8_t>(v & 0xFF);
    bytes[i * 2 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  }
  return bytes;
}

// ---------------------------------------------------------------------------
// EFM (.efm)
// ---------------------------------------------------------------------------
// The raw EFM sidecar holds one byte per T-value in field order, with no
// index — tbc_source locates a frame's payload by running-summing the
// per-field `efm_t_values` counts, so those counts are what makes the file
// readable.
//
// The pipeline only exposes EFM per frame (VideoFrameRepresentation has no
// per-field EFM accessor), so a frame's byte run is split evenly between its
// two field records, odd byte to the first.  The concatenated stream — and
// therefore every frame's payload on re-import — is unaffected by where that
// internal boundary falls.
struct TbcEfmFieldSplit {
  int32_t first_field = 0;
  int32_t second_field = 0;
};

inline TbcEfmFieldSplit tbc_efm_split_frame_bytes(size_t frame_bytes) {
  TbcEfmFieldSplit split;
  split.first_field = static_cast<int32_t>((frame_bytes + 1) / 2);
  split.second_field = static_cast<int32_t>(frame_bytes / 2);
  return split;
}

}  // namespace orc
