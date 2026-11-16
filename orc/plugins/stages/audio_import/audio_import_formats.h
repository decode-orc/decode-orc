/*
 * File:        audio_import_formats.h
 * Module:      orc-stage-plugin-audio_import
 * Purpose:     Container detection and FLAC stream rules for the Audio
 *              Import stage, and the decoded-sample conversion to the 24-bit
 *              carrier
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace orc {

// The audio containers the stage can import.
enum class AudioImportContainer {
  kWav,      // RIFF/WAVE, RF64 or BW64
  kFlac,     // native FLAC, optionally behind an ID3v2 tag
  kUnknown,  // anything else
};

// Bytes of the file head detect_audio_import_container() needs.
constexpr size_t kAudioImportContainerProbeBytes = 4;

// The container of a file from its first bytes. A file opening on an ID3v2
// tag is taken for FLAC (the tag sits in front of the "fLaC" marker); the
// FLAC reader then refuses it if the stream behind the tag is not FLAC.
AudioImportContainer detect_audio_import_container(const uint8_t* head,
                                                   size_t length);

// The largest FLAC sample width imported. Every width up to this converts
// exactly to the 24-bit carrier; a 32-bit stream would have to be truncated.
constexpr int kMaxFlacBitsPerSample = 24;

// Why a FLAC stream with these STREAMINFO properties cannot be imported, or
// empty when it can: stereo, 1 to 24 bits per sample, a non-zero rate.
std::string flac_stream_problem(int channels, int bits_per_sample,
                                int sample_rate_hz);

// How the FLAC decoder hands over a block of samples. The decoder left-
// justifies every width: widths up to 16 bits arrive as 16-bit words, wider
// ones as 32-bit words, with the sample in the most significant bits.
enum class DecodedSampleLayout {
  kS16Interleaved,
  kS16Planar,
  kS32Interleaved,
  kS32Planar,
};

// Append |pair_count| decoded stereo pairs to |out| as interleaved values in
// the 24-bit-in-int32 carrier. |planes| holds one plane for an interleaved
// layout and two (left, right) for a planar one. A 16-bit word is widened
// (<< 8) and a 32-bit word narrowed (>> 8, arithmetic): both are exact for
// the widths flac_stream_problem() accepts, since the decoder leaves the
// bits below the sample's width zero.
void append_decoded_pairs(DecodedSampleLayout layout,
                          const uint8_t* const* planes, size_t pair_count,
                          std::vector<int32_t>& out);

}  // namespace orc
