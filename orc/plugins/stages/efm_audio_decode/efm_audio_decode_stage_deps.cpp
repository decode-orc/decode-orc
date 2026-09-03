/*
 * File:        efm_audio_decode_stage_deps.cpp
 * Module:      orc-stage-plugin-efm_audio_decode
 * Purpose:     Default EFMAudioDecodeStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "efm_audio_decode_stage_deps.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <system_error>

#include "efm-decode/efm-lib/efm_exception.h"
#include "efm-decode/efm_processor.h"

namespace orc {

namespace {

constexpr uint64_t kBytesPerStereoPair = 4;  // 2 channels x int16_t

// IEC 60908: CD audio is 44.1 kHz; used to convert the user's millisecond
// sync slip into decoder stream pairs.
constexpr double kCdSampleRateHz = 44100.0;

// 2 channels x int32_t in the converted synchronous cache.
constexpr uint64_t kBytesPerSynchronousStereoPair = 8;

// Unique scratch path for a decoded-audio cache file. Uniqueness comes from
// the steady-clock tick count plus the owning object's address, which is
// enough to keep concurrent pipelines in the same process apart.
std::filesystem::path make_cache_path(const void* owner,
                                      const std::string& extension) {
  const auto ticks =
      std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("orc-efm-audio-" + std::to_string(ticks) + "-" +
          std::to_string(reinterpret_cast<uintptr_t>(owner)) + extension);
}

}  // namespace

EFMAudioDecodeDeps::~EFMAudioDecodeDeps() {
  std::error_code ec;
  if (!cache_path_.empty()) {
    std::filesystem::remove(cache_path_, ec);
  }
  if (!sync_cache_path_.empty()) {
    std::filesystem::remove(sync_cache_path_, ec);
  }
}

EFMAudioDecodeResult EFMAudioDecodeDeps::decode_to_cache(
    const VideoFrameRepresentation& representation,
    const EFMAudioDecodeOptions& options, const ProgressFn& progress) {
  const auto frame_rng = representation.frame_range();
  const FrameID start_fid = frame_rng.first;
  const FrameID end_fid = frame_rng.last;
  const uint64_t total_frames = frame_rng.count();

  ORC_LOG_DEBUG("EFMAudioDecodeDeps: counting EFM t-values across {} frames",
                frame_rng.count());

  uint64_t total_tvalues = 0;
  for (FrameID fid = start_fid; fid <= end_fid; ++fid) {
    total_tvalues += representation.get_efm_sample_count(fid);
  }
  if (total_tvalues == 0) {
    return {false, "no EFM t-values found in frame range", 0};
  }

  const std::filesystem::path cache_path = make_cache_path(this, ".pcm");

  // Audio-mode decode to headerless PCM: the cache holds exactly the decoded
  // 44.1 kHz interleaved stereo int16_t stream, so pair offsets map directly
  // to byte offsets.
  EfmProcessor processor;
  processor.setAudioMode(true);
  processor.setNoWavHeader(true);
  processor.setNoTimecodes(options.no_timecodes);
  processor.setNoAudioConcealment(options.no_audio_concealment);
  processor.setIgnorePreemphasis(options.ignore_preemphasis);

  // Issue #231: the EFM t-values are demodulated from the same FM signal as
  // the video, so the decoded audio has a fixed, computable relationship to
  // the video timeline. Align the stream head so cache pair 0 really is
  // synchronous with the first frame of the decode range, plus any user slip.
  processor.setAudioSyncOffset(true);
  processor.setAudioSyncSlipPairs(static_cast<int64_t>(
      std::llround(options.offset_ms * kCdSampleRateHz / 1000.0)));

  // The decode targets a scratch cache, so point the report at the
  // user-chosen path rather than letting it default to "<cache>.txt".
  if (!options.report_path.empty()) {
    processor.setReportOutput(true);
    processor.setReportFilename(options.report_path);
  }

  // The EFM decoder throws efm::EfmDecodeError on unrecoverable conditions;
  // catch it so a bad decode fails this stage rather than killing the process.
  try {
    processor.beginStream(cache_path.string(),
                          static_cast<int64_t>(total_tvalues));

    // Stream each frame's t-values straight into the decoder, accumulating into
    // a bounded staging buffer only to preserve the 1024-byte chunk sizing of
    // the old buffered path (so decoded output stays byte-identical). Peak RSS
    // is now ~one chunk, independent of capture length.
    constexpr size_t kChunkSize = 1024;
    std::vector<uint8_t> staging;
    staging.reserve(kChunkSize);
    // Report every ~1% of the range (and always the first frame) so the sink's
    // progress dialog advances instead of freezing for the whole decode.
    const uint64_t report_stride = std::max<uint64_t>(1, total_frames / 100);
    for (FrameID fid = start_fid; fid <= end_fid; ++fid) {
      if (progress) {
        const uint64_t done = static_cast<uint64_t>(fid - start_fid);
        if (done % report_stride == 0) {
          progress(done, total_frames, "Decoding EFM audio...");
        }
      }
      const auto samples = representation.get_efm_samples(fid);
      size_t pos = 0;
      while (pos < samples.size()) {
        const size_t take =
            std::min(kChunkSize - staging.size(), samples.size() - pos);
        // The pipeline carries the producer's per-t-value doubt in the high
        // nibble of each byte. It travels the DAG intact, but the decoder
        // consumes t-values, so strip it here at the point of consumption.
        // (The doubt is not yet used; when the C1/C2 stages learn to take
        // erasure hints it should be forwarded rather than discarded.)
        for (size_t i = 0; i < take; ++i) {
          staging.push_back(efm_tvalue(samples[pos + i]));
        }
        pos += take;
        if (staging.size() == kChunkSize) {
          processor.pushChunk(staging);
          staging.clear();
        }
      }
    }
    // Flush the final partial chunk (fewer than kChunkSize t-values).
    if (!staging.empty()) {
      processor.pushChunk(staging);
    }

    if (!processor.finishStream()) {
      std::error_code ec;
      std::filesystem::remove(cache_path, ec);
      std::string reason = processor.lastError();
      if (reason.empty()) {
        reason = "EFM decoding did not complete successfully";
      }
      return {false, reason, 0};
    }
  } catch (const efm::EfmDecodeError& e) {
    std::error_code ec;
    std::filesystem::remove(cache_path, ec);
    ORC_LOG_ERROR("EFMAudioDecodeDeps: {}", e.what());
    return {false, e.what(), 0};
  } catch (const std::exception& e) {
    // Backstop: any other escape from the decoder must fail this stage (and
    // clean up the partial cache file) rather than kill the host process.
    std::error_code ec;
    std::filesystem::remove(cache_path, ec);
    ORC_LOG_ERROR("EFMAudioDecodeDeps: unexpected exception: {}", e.what());
    return {false, e.what(), 0};
  }

  std::error_code ec;
  const uint64_t cache_bytes = std::filesystem::file_size(cache_path, ec);
  if (ec) {
    std::filesystem::remove(cache_path, ec);
    return {false, "failed to stat decoded audio cache file", 0};
  }

  cache_path_ = cache_path;
  return {true, "", cache_bytes / kBytesPerStereoPair,
          processor.appliedAudioSyncOffsetPairs()};
}

std::vector<int16_t> EFMAudioDecodeDeps::read_cache_pairs(
    uint64_t first_pair, uint32_t pair_count) const {
  if (cache_path_.empty() || pair_count == 0) return {};

  std::ifstream file(cache_path_, std::ios::binary);
  if (!file) {
    ORC_LOG_ERROR("EFMAudioDecodeDeps: cannot open audio cache file {}",
                  cache_path_.string());
    return {};
  }

  file.seekg(static_cast<std::streamoff>(first_pair * kBytesPerStereoPair));
  std::vector<int16_t> samples(static_cast<size_t>(pair_count) * 2);
  file.read(reinterpret_cast<char*>(samples.data()),
            static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));

  // Truncate a short read to whole stereo pairs.
  const uint64_t pairs_read =
      static_cast<uint64_t>(file.gcount()) / kBytesPerStereoPair;
  samples.resize(static_cast<size_t>(pairs_read) * 2);
  return samples;
}

bool EFMAudioDecodeDeps::write_synchronous_cache(
    const std::vector<int32_t>& samples) {
  const std::filesystem::path sync_path = make_cache_path(this, ".s32");

  {
    std::ofstream file(sync_path, std::ios::binary | std::ios::trunc);
    if (!file) {
      ORC_LOG_ERROR(
          "EFMAudioDecodeDeps: cannot create synchronous audio cache file {}",
          sync_path.string());
      return false;
    }
    file.write(reinterpret_cast<const char*>(samples.data()),
               static_cast<std::streamsize>(samples.size() * sizeof(int32_t)));
    if (!file) {
      ORC_LOG_ERROR(
          "EFMAudioDecodeDeps: failed writing synchronous audio cache file {}",
          sync_path.string());
      std::error_code ec;
      std::filesystem::remove(sync_path, ec);
      return false;
    }
  }

  sync_cache_path_ = sync_path;

  // The raw 44.1 kHz cache is no longer needed once the converted stream is
  // on disk; drop it to halve the scratch footprint.
  if (!cache_path_.empty()) {
    std::error_code ec;
    std::filesystem::remove(cache_path_, ec);
    cache_path_.clear();
  }
  return true;
}

std::vector<int32_t> EFMAudioDecodeDeps::read_synchronous_pairs(
    uint64_t first_pair, uint32_t pair_count) const {
  if (sync_cache_path_.empty() || pair_count == 0) return {};

  std::ifstream file(sync_cache_path_, std::ios::binary);
  if (!file) {
    ORC_LOG_ERROR(
        "EFMAudioDecodeDeps: cannot open synchronous audio cache file {}",
        sync_cache_path_.string());
    return {};
  }

  file.seekg(
      static_cast<std::streamoff>(first_pair * kBytesPerSynchronousStereoPair));
  std::vector<int32_t> samples(static_cast<size_t>(pair_count) * 2);
  file.read(reinterpret_cast<char*>(samples.data()),
            static_cast<std::streamsize>(samples.size() * sizeof(int32_t)));

  // Truncate a short read to whole stereo pairs.
  const uint64_t pairs_read =
      static_cast<uint64_t>(file.gcount()) / kBytesPerSynchronousStereoPair;
  samples.resize(static_cast<size_t>(pairs_read) * 2);
  return samples;
}

}  // namespace orc
