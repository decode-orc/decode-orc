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

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace orc {

// One row of the .meta audio_channel_pair table (CVBS file format spec
// v1.4.0): channel pair number and its human-readable description.
struct CVBSAudioChannelPairMetaRow {
  int32_t channel_pair = 0;  // 0–7, matches the _audio_<p>.wav suffix
  std::string description;   // empty = NULL
};

// Core <base>.meta metadata schema.
// CVBS file format spec v1.6.0: Metadata Schema (PRAGMA user_version = 11);
// v1.6.0 renamed the TBC preset-name token to STABLE and moved sequence
// continuity out of the preset into the sequence_continuous column.
inline constexpr const char* kCVBSCoreMetaSchemaSql =
    "PRAGMA user_version = 11;"
    "CREATE TABLE cvbs_file ("
    "    cvbs_file_id                INTEGER PRIMARY KEY,"
    "    preset                      TEXT    NOT NULL"
    "        CHECK (preset IN ('NTSC', 'PAL', 'PAL_M')),"
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

// <base>_audio_<p>.wav path for a channel pair number (single digit, CVBS
// file format spec v1.4.0).
inline std::string cvbs_audio_pair_path(const std::string& base, size_t pair) {
  return base + "_audio_" + std::to_string(pair) + ".wav";
}

inline void cvbs_wav_append_le16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

inline void cvbs_wav_append_le32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(static_cast<uint8_t>(v & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
  out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

// 44-byte canonical RIFF/WAVE header for the container audio payload.
// CVBS file format spec v1.4.0 (after SMPTE 272M-1994): PCM, 2 channels,
// 48000 Hz (exact for all systems), 24-bit signed LE — the only permitted
// audio format.
inline std::vector<uint8_t> make_cvbs_audio_wav_header(uint32_t data_bytes) {
  constexpr uint16_t kChannels = 2;
  constexpr uint16_t kBitsPerSample = kAudioBitDepth;               // 24
  constexpr uint16_t kBlockAlign = kChannels * kBitsPerSample / 8;  // 6

  std::vector<uint8_t> header;
  header.reserve(44);
  header.insert(header.end(), {'R', 'I', 'F', 'F'});
  cvbs_wav_append_le32(header, 36 + data_bytes);
  header.insert(header.end(), {'W', 'A', 'V', 'E'});
  header.insert(header.end(), {'f', 'm', 't', ' '});
  cvbs_wav_append_le32(header, 16);  // fmt chunk size
  cvbs_wav_append_le16(header, 1);   // PCM
  cvbs_wav_append_le16(header, kChannels);
  cvbs_wav_append_le32(header, kAudioSampleRateHz);
  cvbs_wav_append_le32(header, kAudioSampleRateHz * kBlockAlign);  // byte rate
  cvbs_wav_append_le16(header, kBlockAlign);
  cvbs_wav_append_le16(header, kBitsPerSample);
  header.insert(header.end(), {'d', 'a', 't', 'a'});
  cvbs_wav_append_le32(header, data_bytes);
  return header;
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
