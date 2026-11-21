/*
 * File:        synchronous_audio_cache.h
 * Module:      orc-audio-resample (shared stage-plugin library)
 * Purpose:     Sequential conversion of a source audio stream to the
 *              synchronous 48 kHz pipeline form, kept in an on-disk cache
 *              and served per frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/common_types.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace orc {

class AudioResampleStream;

// ---------------------------------------------------------------------------
// ISynchronousAudioStore
// ---------------------------------------------------------------------------
// Append-only store of interleaved stereo int32 pairs. append() is called by
// one thread at a time; read() may run concurrently with append() and with
// other reads, for pairs that an earlier append() has returned.
class ISynchronousAudioStore {
 public:
  virtual ~ISynchronousAudioStore() = default;

  // Append |pair_count| pairs (2 × pair_count values). False on I/O failure.
  virtual bool append(const int32_t* samples, size_t pair_count) = 0;

  // Copy |pair_count| pairs starting at |first_pair| into |out| (2 ×
  // pair_count values). False on I/O failure or when the range was never
  // appended.
  virtual bool read(uint64_t first_pair, size_t pair_count,
                    int32_t* out) const = 0;
};

// In-memory store, for unit tests (which touch no files).
std::unique_ptr<ISynchronousAudioStore> make_memory_audio_store();

// Store in a scratch file under audio_cache_directory(). The file is removed
// when the store is destroyed; on POSIX it is unlinked as soon as it is open,
// so a crash leaves nothing behind. Returns nullptr and sets |error| when the
// file cannot be created.
std::unique_ptr<ISynchronousAudioStore> make_scratch_file_audio_store(
    std::string& error);

// Directory for converted-audio cache files: ORC_AUDIO_CACHE_DIR when set,
// otherwise scratch_directory("audio") (<orc/support/scratch_file.h>), which
// is ORC_CACHE_DIR/audio or the per-user cache directory (XDG_CACHE_HOME or
// ~/.cache on Linux, ~/Library/Caches on macOS, %LOCALAPPDATA% on Windows)
// plus "decode-orc/audio". Not the system temp directory, which may be
// RAM-backed: six hours of converted audio is about 8.3 GB. Created on demand.
std::filesystem::path audio_cache_directory();

// ---------------------------------------------------------------------------
// SynchronousAudioCache
// ---------------------------------------------------------------------------
// Serves a source audio stream in the synchronous pipeline form: 48000 Hz,
// 24-bit-in-int32 stereo, segmented into cadence-sized per-frame blocks
// (SMPTE 272M-1994 §14.3), zero-padded or truncated so the blocks total
// exactly audio_pair_offset(frame_count) pairs. The samples are identical to
// AudioResampler::resample_to_synchronous() on the whole stream.
//
// No whole-stream buffer exists. A source already at 48000 Hz is read
// straight from the source for each frame. Any other rate is resampled
// sequentially, one AudioResampleStream::kChunkFrames read at a time, into a
// store; a request past the converted point converts up to it, and earlier
// frames are read back from the store. So the first frame is served after one
// chunk, not after the whole stream.
//
// Thread-safety: frame_block() may be called concurrently. Frames already
// converted are served without waiting on a conversion in progress.
class SynchronousAudioCache {
 public:
  // Reads up to |pair_count| stereo pairs of source audio starting at
  // |first_pair|, in the 24-bit-in-int32 carrier, interleaved. Fewer pairs
  // (whole pairs only) mean the source ends there.
  using SourceReader =
      std::function<std::vector<int32_t>(uint64_t first_pair, size_t)>;
  // Creates the store on the first conversion; nullptr means the converted
  // audio cannot be kept, and the frames past the source's 48 kHz pass-
  // through are served as silence.
  using StoreFactory = std::function<std::unique_ptr<ISynchronousAudioStore>()>;

  SynchronousAudioCache(SourceReader reader, double source_rate_hz,
                        VideoSystem system, uint64_t frame_count,
                        StoreFactory make_store);
  ~SynchronousAudioCache();

  SynchronousAudioCache(const SynchronousAudioCache&) = delete;
  SynchronousAudioCache& operator=(const SynchronousAudioCache&) = delete;

  // Block for frame |frame_index| (0-based from the first frame of the
  // stream): audio_pairs_in_frame(frame_index) pairs, interleaved. Empty for
  // an unknown video system or an index at or past frame_count.
  std::vector<int32_t> frame_block(uint64_t frame_index) const;

  // Convert the whole stream now (for a caller that reports progress on one
  // pass). |progress| receives (converted pairs, total pairs).
  void convert_all(
      const std::function<void(uint64_t, uint64_t)>& progress = {}) const;

  // True when the source needs no rate conversion and is read per frame.
  bool is_pass_through() const { return pass_through_; }

  // Synchronous pairs held in the store so far (0 for pass-through).
  uint64_t converted_pairs() const {
    return stored_pairs_.load(std::memory_order_acquire);
  }

 private:
  // Convert until at least |pair_end| pairs are stored or the stream is
  // exhausted. Caller holds convert_mutex_.
  void convert_until(
      uint64_t pair_end,
      const std::function<void(uint64_t, uint64_t)>& progress) const;
  void finish_conversion() const;  // caller holds convert_mutex_
  void store_output(std::vector<int32_t>& output) const;

  SourceReader reader_;
  double source_rate_hz_;
  VideoSystem system_;
  uint64_t frame_count_;
  uint64_t total_pairs_;  // audio_pair_offset(frame_count)
  bool pass_through_;
  StoreFactory make_store_;

  mutable std::mutex convert_mutex_;
  mutable std::unique_ptr<ISynchronousAudioStore> store_;
  mutable std::unique_ptr<AudioResampleStream> stream_;
  mutable uint64_t source_pos_ = 0;  // source pairs read so far
  mutable bool conversion_done_ = false;
  // Pairs readable from store_. Written under convert_mutex_, read without.
  mutable std::atomic<uint64_t> stored_pairs_{0};
};

}  // namespace orc
