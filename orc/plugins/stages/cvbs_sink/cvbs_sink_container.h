/*
 * File:        cvbs_sink_container.h
 * Module:      orc-core
 * Purpose:     Pure CVBS container-format helpers for the CVBS sink stage
 *              (audio WAV header, channel-pair naming, .meta schema)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_CVBS_SINK_CONTAINER_H
#define ORC_CORE_CVBS_SINK_CONTAINER_H

#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/common_types.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "../common/audio-resample/wav_header.h"

namespace orc {

// One row of the .meta audio_channel_pair table (CVBS file format spec
// v1.4.0): channel pair number and its human-readable description.
struct CVBSAudioChannelPairMetaRow {
  int32_t channel_pair = 0;  // 0–7, matches the _audio_<p>.wav suffix
  std::string description;   // empty = NULL
};

// Core <base>.meta metadata schema for a file of |system|.
// CVBS file format spec v1.6.0: Metadata Schema (PRAGMA user_version = 11);
// v1.6.0 renamed the TBC preset-name token to STABLE and moved sequence
// continuity out of the preset into the sequence_continuous column.
// v1.8.0 (user_version = 12) only adds 'SECAM' to the preset CHECK, and a
// producer writing a SECAM file must write 12 (spec index.md, "Schema version
// 12"). A file that declares PAL, NTSC or PAL_M stays valid at 11 under
// v1.8.1, and 11 is still readable by consumers that predate v1.8.0, so those
// keep 11.
inline std::string cvbs_core_meta_schema_sql(VideoSystem system) {
  const bool secam = (system == VideoSystem::SECAM);
  return std::string(secam ? "PRAGMA user_version = 12;"
                           : "PRAGMA user_version = 11;") +
         "CREATE TABLE cvbs_file ("
         "    cvbs_file_id                INTEGER PRIMARY KEY,"
         "    preset                      TEXT    NOT NULL"
         "        CHECK (preset IN ('NTSC', 'PAL', 'PAL_M'" +
         std::string(secam ? ", 'SECAM'" : "") +
         ")),"
         "    sample_encoding_preset      TEXT    NOT NULL"
         "        CHECK (sample_encoding_preset IN ('CVBS_U10_4FSC', "
         "'CVBS_U16_4FSC', 'RAW_S16_28M', 'RAW_S16_40M', 'CVBS_TPG21_4FSC', "
         "'CVBS_S16_4FSC')),"
         "    signal_state_preset         TEXT    NOT NULL"
         "        CHECK (signal_state_preset IN ("
         "            'STANDARD_STABLE_LOCKED',"
         "            'STANDARD_STABLE_UNLOCKED',"
         "            'STANDARD_RAW',"
         "            'NONSTANDARD_STABLE_LOCKED',"
         "            'NONSTANDARD_STABLE_UNLOCKED',"
         "            'NONSTANDARD_RAW'"
         "        )),"
         "    sequence_continuous         BOOLEAN,"
         "    signal_type                 TEXT    NOT NULL"
         "        CHECK (signal_type IN ('composite', 'yc')),"
         "    decoder                     TEXT    NOT NULL,"
         "    git_branch                  TEXT,"
         "    git_commit                  TEXT,"
         "    number_of_sequential_frames INTEGER"
         "        CHECK (number_of_sequential_frames IS NULL OR "
         "number_of_sequential_frames >= 1),"
         "    black_level                 INTEGER,"
         "    has_nonstandard_values      BOOLEAN,"
         "    capture_notes               TEXT"
         ");"
         "CREATE TABLE audio_channel_pair ("
         "    channel_pair                INTEGER PRIMARY KEY"
         "        CHECK (channel_pair BETWEEN 0 AND 7),"
         "    description                 TEXT"
         ");";
}

// <base>_audio_<p>.wav path for a channel pair number (single digit, CVBS
// file format spec v1.4.0).
inline std::string cvbs_audio_pair_path(const std::string& base, size_t pair) {
  return base + "_audio_" + std::to_string(pair) + ".wav";
}

// Bytes of one channel pair's audio payload for |pairs| stereo pairs.
inline constexpr uint64_t cvbs_audio_data_bytes(uint64_t pairs) {
  return pairs * 2 * (kAudioBitDepth / 8);
}

// The largest channel pair payload a RIFF sidecar may carry. CVBS file
// format spec v1.8.0 onwards: a file whose RIFF chunk size (36 + data bytes
// for the 44-byte header) would exceed 0xFFFFFFFE is written as RF64. That
// is about 4 hours 8 minutes of 48 kHz 24-bit stereo.
inline constexpr uint64_t kCvbsAudioRiffMaxDataBytes = wav::kRiffMaxDataBytes;

// True when a channel pair payload of |data_bytes| must be RF64 under
// |riff_max_data_bytes|. Every pair of one export carries the same number of
// stereo pairs, so the choice is the same for all of them, as the
// specification requires.
inline constexpr bool cvbs_audio_needs_rf64(
    uint64_t data_bytes,
    uint64_t riff_max_data_bytes = kCvbsAudioRiffMaxDataBytes) {
  return data_bytes > riff_max_data_bytes;
}

// Header for a channel pair file: the canonical 44-byte RIFF header, or the
// 80-byte RF64 header (RF64, ds64 with the sample count and a table length
// of 0, fmt, data, 32-bit sizes 0xFFFFFFFF) when |rf64|. CVBS file format
// spec (after SMPTE 272M-1994): PCM, 2 channels, 48000 Hz (exact for all
// systems), 24-bit signed LE — the only permitted audio format.
inline std::vector<uint8_t> make_cvbs_audio_wav_header(uint64_t data_bytes,
                                                       bool rf64) {
  return wav::build_pcm_header(2, kAudioSampleRateHz, kAudioBitDepth,
                               data_bytes, rf64);
}

// Pack pipeline audio (24-bit-in-int32 carrier) into the container's 24-bit
// signed LE payload: exactly |expected_values| samples (2 × stereo pairs),
// zero-padded when the input is short, truncated when long, saturated to the
// 24-bit range (−8388608 … 8388607) defensively.
inline std::vector<uint8_t> pack_audio_s24le(const std::vector<int32_t>& audio,
                                             size_t expected_values) {
  std::vector<uint8_t> bytes(expected_values * 3, 0);
  const size_t copy_values = std::min(audio.size(), expected_values);
  for (size_t i = 0; i < copy_values; ++i) {
    const int32_t v = std::clamp(audio[i], -8388608, 8388607);
    bytes[i * 3] = static_cast<uint8_t>(v & 0xFF);
    bytes[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    bytes[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  }
  return bytes;
}

}  // namespace orc

#endif  // ORC_CORE_CVBS_SINK_CONTAINER_H
