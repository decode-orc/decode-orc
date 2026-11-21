/*
 * File:        audio_import_stage_deps.cpp
 * Module:      orc-stage-plugin-audio_import
 * Purpose:     Production dependencies for AudioImportStage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "audio_import_stage_deps.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

#include "audio-resample/synchronous_audio_cache.h"
#include "audio-resample/wav_header.h"

namespace orc {

namespace {

// 3-byte little-endian two's-complement sample, sign-extended into the
// 24-bit-in-int32 carrier (−8388608 … 8388607).
int32_t read_s24_le(const uint8_t* p) {
  uint32_t u = static_cast<uint32_t>(p[0]) |
               (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16);
  if (u & 0x00800000u) u |= 0xFF000000u;
  return static_cast<int32_t>(u);
}

}  // namespace

WavProbeResult AudioImportDeps::open(const std::string& wav_path) {
  WavProbeResult result;

  std::error_code ec;
  const uint64_t file_size = std::filesystem::file_size(wav_path, ec);
  std::ifstream file(wav_path, std::ios::binary);
  if (ec || !file) {
    result.error = "cannot open file";
    return result;
  }

  const wav::WavLayout layout = wav::parse_header(
      [&file](uint64_t offset, uint8_t* out, size_t length) -> size_t {
        file.clear();
        file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!file) return 0;
        file.read(reinterpret_cast<char*>(out),
                  static_cast<std::streamsize>(length));
        return static_cast<size_t>(file.gcount());
      },
      file_size);
  if (!layout.valid) {
    result.error = layout.error;
    return result;
  }
  if (layout.format_tag != 1) {  // WAVE_FORMAT_PCM
    result.error = "not PCM encoded";
    return result;
  }
  if (layout.channels != 2) {
    result.error =
        "not stereo (" + std::to_string(layout.channels) + " channel(s))";
    return result;
  }
  if (layout.bits_per_sample != 16 && layout.bits_per_sample != 24) {
    result.error = "not 16- or 24-bit (" +
                   std::to_string(layout.bits_per_sample) + " bits/sample)";
    return result;
  }
  if (layout.sample_rate_hz == 0) {
    result.error = "invalid sample rate (0 Hz)";
    return result;
  }

  // Bytes per stereo pair: 16-bit = 2 × 2 bytes; 24-bit = 2 × 3 bytes.
  const uint64_t bytes_per_pair = (layout.bits_per_sample == 16) ? 4 : 6;

  wav_path_ = wav_path;
  bits_per_sample_ = layout.bits_per_sample;
  data_offset_ = layout.data_offset;
  data_pairs_ = layout.data_bytes / bytes_per_pair;

  result.valid = true;
  result.sample_rate = layout.sample_rate_hz;
  result.bits_per_sample = layout.bits_per_sample;
  result.pair_count = data_pairs_;
  return result;
}

std::vector<int32_t> AudioImportDeps::read_pairs(uint64_t first_pair,
                                                 size_t pair_count) const {
  if (wav_path_.empty() || first_pair >= data_pairs_ || pair_count == 0) {
    return {};
  }
  const auto pairs = static_cast<size_t>(
      std::min<uint64_t>(pair_count, data_pairs_ - first_pair));
  const size_t bytes_per_pair = (bits_per_sample_ == 16) ? 4 : 6;

  std::ifstream file(wav_path_, std::ios::binary);
  if (!file) return {};
  file.seekg(
      static_cast<std::streamoff>(data_offset_ + first_pair * bytes_per_pair),
      std::ios::beg);
  std::vector<uint8_t> bytes(pairs * bytes_per_pair);
  file.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  const size_t pairs_read = static_cast<size_t>(file.gcount()) / bytes_per_pair;

  std::vector<int32_t> samples(pairs_read * 2);
  if (bits_per_sample_ == 16) {
    for (size_t i = 0; i < samples.size(); ++i) {
      const auto v =
          static_cast<int16_t>(static_cast<uint16_t>(bytes[i * 2]) |
                               static_cast<uint16_t>(bytes[i * 2 + 1] << 8));
      samples[i] = static_cast<int32_t>(v) << 8;  // 16 → 24-bit carrier
    }
  } else {
    for (size_t i = 0; i < samples.size(); ++i) {
      samples[i] = read_s24_le(bytes.data() + i * 3);
    }
  }
  return samples;
}

std::unique_ptr<ISynchronousAudioStore> AudioImportDeps::make_audio_store()
    const {
  std::string error;
  auto store = make_scratch_file_audio_store(error);
  if (!store) ORC_LOG_ERROR("AudioImportStage: {}", error);
  return store;
}

}  // namespace orc
