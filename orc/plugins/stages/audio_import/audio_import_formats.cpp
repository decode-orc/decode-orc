/*
 * File:        audio_import_formats.cpp
 * Module:      orc-stage-plugin-audio_import
 * Purpose:     Container detection and FLAC stream rules for the Audio
 *              Import stage, and the decoded-sample conversion to the 24-bit
 *              carrier
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "audio_import_formats.h"

#include <cstring>

namespace orc {

namespace {

bool starts_with(const uint8_t* head, size_t length, const char* marker,
                 size_t marker_length) {
  return length >= marker_length &&
         std::memcmp(head, marker, marker_length) == 0;
}

// The carrier is 24 bits wide: a 16-bit word moves up by 8 bits to fill it
// and a left-justified 32-bit word moves down by 8.
constexpr int kWidenS16Shift = 8;
constexpr int kNarrowS32Shift = 8;

int32_t carrier_from_s16(const uint8_t* p) {
  int16_t v;
  std::memcpy(&v, p, sizeof(v));
  return static_cast<int32_t>(v) * (1 << kWidenS16Shift);
}

int32_t carrier_from_s32(const uint8_t* p) {
  int32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v >> kNarrowS32Shift;  // arithmetic: sign is kept
}

}  // namespace

AudioImportContainer detect_audio_import_container(const uint8_t* head,
                                                   size_t length) {
  if (head == nullptr) return AudioImportContainer::kUnknown;
  if (starts_with(head, length, "RIFF", 4) ||
      starts_with(head, length, "RF64", 4) ||
      starts_with(head, length, "BW64", 4)) {
    return AudioImportContainer::kWav;
  }
  if (starts_with(head, length, "fLaC", 4) ||
      starts_with(head, length, "ID3", 3)) {
    return AudioImportContainer::kFlac;
  }
  return AudioImportContainer::kUnknown;
}

std::string flac_stream_problem(int channels, int bits_per_sample,
                                int sample_rate_hz) {
  if (channels != 2) {
    return "not stereo (" + std::to_string(channels) + " channel(s))";
  }
  if (bits_per_sample < 1 || bits_per_sample > kMaxFlacBitsPerSample) {
    return "not 24-bit or narrower (" + std::to_string(bits_per_sample) +
           " bits/sample)";
  }
  if (sample_rate_hz <= 0) {
    return "invalid sample rate (" + std::to_string(sample_rate_hz) + " Hz)";
  }
  return {};
}

void append_decoded_pairs(DecodedSampleLayout layout,
                          const uint8_t* const* planes, size_t pair_count,
                          std::vector<int32_t>& out) {
  if (planes == nullptr || pair_count == 0) return;
  out.reserve(out.size() + pair_count * 2);
  switch (layout) {
    case DecodedSampleLayout::kS16Interleaved:
      for (size_t i = 0; i < pair_count * 2; ++i) {
        out.push_back(carrier_from_s16(planes[0] + i * sizeof(int16_t)));
      }
      break;
    case DecodedSampleLayout::kS16Planar:
      for (size_t i = 0; i < pair_count; ++i) {
        out.push_back(carrier_from_s16(planes[0] + i * sizeof(int16_t)));
        out.push_back(carrier_from_s16(planes[1] + i * sizeof(int16_t)));
      }
      break;
    case DecodedSampleLayout::kS32Interleaved:
      for (size_t i = 0; i < pair_count * 2; ++i) {
        out.push_back(carrier_from_s32(planes[0] + i * sizeof(int32_t)));
      }
      break;
    case DecodedSampleLayout::kS32Planar:
      for (size_t i = 0; i < pair_count; ++i) {
        out.push_back(carrier_from_s32(planes[0] + i * sizeof(int32_t)));
        out.push_back(carrier_from_s32(planes[1] + i * sizeof(int32_t)));
      }
      break;
  }
}

}  // namespace orc
