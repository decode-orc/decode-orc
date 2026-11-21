/*
 * File:        synchronous_audio_cache.cpp
 * Module:      orc-audio-resample (shared stage-plugin library)
 * Purpose:     Sequential conversion of a source audio stream to the
 *              synchronous 48 kHz pipeline form, kept in an on-disk cache
 *              and served per frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "synchronous_audio_cache.h"

#include <orc/support/logging.h>
#include <orc/support/scratch_file.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <system_error>

#include "audio_resampler.h"

namespace orc {

namespace {

constexpr size_t kBytesPerPair = 2 * sizeof(int32_t);

// ---------------------------------------------------------------------------
// MemoryAudioStore
// ---------------------------------------------------------------------------
class MemoryAudioStore final : public ISynchronousAudioStore {
 public:
  bool append(const int32_t* samples, size_t pair_count) override {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.insert(samples_.end(), samples, samples + pair_count * 2);
    return true;
  }

  bool read(uint64_t first_pair, size_t pair_count,
            int32_t* out) const override {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t begin = first_pair * 2;
    const uint64_t count = static_cast<uint64_t>(pair_count) * 2;
    if (begin + count > samples_.size()) return false;
    std::copy_n(samples_.begin() + static_cast<std::ptrdiff_t>(begin),
                static_cast<std::ptrdiff_t>(count), out);
    return true;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<int32_t> samples_;
};

// ---------------------------------------------------------------------------
// ScratchFileAudioStore
// ---------------------------------------------------------------------------
// Positioned reads and writes, so concurrent readers need no lock and never
// share a file position.
class ScratchFileAudioStore final : public ISynchronousAudioStore {
 public:
  explicit ScratchFileAudioStore(std::unique_ptr<ScratchFile> file)
      : file_(std::move(file)) {}

  bool append(const int32_t* samples, size_t pair_count) override {
    const uint64_t length = static_cast<uint64_t>(pair_count) * kBytesPerPair;
    if (!file_->write_at(size_bytes_, samples, length)) return false;
    size_bytes_ += length;
    return true;
  }

  bool read(uint64_t first_pair, size_t pair_count,
            int32_t* out) const override {
    return file_->read_at(first_pair * kBytesPerPair, out,
                          static_cast<uint64_t>(pair_count) * kBytesPerPair);
  }

 private:
  std::unique_ptr<ScratchFile> file_;
  uint64_t size_bytes_ = 0;  // only the appending thread touches it
};

}  // namespace

std::unique_ptr<ISynchronousAudioStore> make_memory_audio_store() {
  return std::make_unique<MemoryAudioStore>();
}

std::filesystem::path audio_cache_directory() {
  const char* override_dir = std::getenv("ORC_AUDIO_CACHE_DIR");
  if (override_dir != nullptr && *override_dir != '\0') {
    return std::filesystem::path(override_dir);
  }
  return scratch_directory("audio");
}

std::unique_ptr<ISynchronousAudioStore> make_scratch_file_audio_store(
    std::string& error) {
  std::string file_error;
  auto file = ScratchFile::create(audio_cache_directory(), "orc-audio-", ".s32",
                                  file_error);
  if (!file) {
    error = "audio cache: " + file_error;
    return nullptr;
  }
  return std::make_unique<ScratchFileAudioStore>(std::move(file));
}

// ---------------------------------------------------------------------------
// SynchronousAudioCache
// ---------------------------------------------------------------------------

SynchronousAudioCache::SynchronousAudioCache(SourceReader reader,
                                             double source_rate_hz,
                                             VideoSystem system,
                                             uint64_t frame_count,
                                             StoreFactory make_store)
    : reader_(std::move(reader)),
      source_rate_hz_(source_rate_hz),
      system_(system),
      frame_count_(frame_count),
      total_pairs_(audio_pair_offset(frame_count, system)),
      pass_through_(source_rate_hz == static_cast<double>(kAudioSampleRateHz)),
      make_store_(std::move(make_store)) {}

SynchronousAudioCache::~SynchronousAudioCache() = default;

std::vector<int32_t> SynchronousAudioCache::frame_block(
    uint64_t frame_index) const {
  if (frame_index >= frame_count_) return {};
  const uint32_t block_pairs = audio_pairs_in_frame(frame_index, system_);
  if (block_pairs == 0) return {};
  const uint64_t first = audio_pair_offset(frame_index, system_);
  std::vector<int32_t> block(static_cast<size_t>(block_pairs) * 2, 0);

  if (pass_through_) {
    // Already synchronous: the block is the source's pairs at the cadence
    // offset, zero-padded where the source ends.
    if (reader_) {
      const std::vector<int32_t> source = reader_(first, block_pairs);
      std::copy_n(source.begin(), std::min(source.size(), block.size()),
                  block.begin());
    }
    return block;
  }

  const uint64_t end = first + block_pairs;
  uint64_t stored = stored_pairs_.load(std::memory_order_acquire);
  if (stored < end) {
    std::lock_guard<std::mutex> lock(convert_mutex_);
    convert_until(end, {});
    stored = stored_pairs_.load(std::memory_order_acquire);
  }
  // Pairs past the end of the converted stream stay silent.
  if (stored > first && store_) {
    const auto available =
        static_cast<size_t>(std::min<uint64_t>(stored, end) - first);
    if (!store_->read(first, available, block.data())) {
      ORC_LOG_ERROR(
          "SynchronousAudioCache: failed to read frame {} from the audio "
          "cache; serving silence",
          frame_index);
      std::fill(block.begin(), block.end(), 0);
    }
  }
  return block;
}

void SynchronousAudioCache::convert_all(
    const std::function<void(uint64_t, uint64_t)>& progress) const {
  if (pass_through_) return;
  std::lock_guard<std::mutex> lock(convert_mutex_);
  convert_until(total_pairs_, progress);
}

void SynchronousAudioCache::convert_until(
    uint64_t pair_end,
    const std::function<void(uint64_t, uint64_t)>& progress) const {
  if (conversion_done_) return;
  if (!stream_) {
    if (total_pairs_ == 0 || !reader_) {
      conversion_done_ = true;
      return;
    }
    store_ = make_store_ ? make_store_() : nullptr;
    if (!store_) {
      ORC_LOG_ERROR(
          "SynchronousAudioCache: no audio cache could be created; the "
          "converted audio is served as silence");
      conversion_done_ = true;
      return;
    }
    stream_ = std::make_unique<AudioResampleStream>(
        source_rate_hz_, static_cast<double>(kAudioSampleRateHz));
    if (!stream_->ok()) {
      conversion_done_ = true;
      return;
    }
  }

  std::vector<int32_t> output;
  while (!conversion_done_ &&
         stored_pairs_.load(std::memory_order_relaxed) < pair_end) {
    const std::vector<int32_t> input =
        reader_(source_pos_, AudioResampleStream::kChunkFrames);
    const size_t pairs = input.size() / 2;
    output.clear();
    bool more = pairs == AudioResampleStream::kChunkFrames;
    if (pairs > 0 && !stream_->process(input.data(), pairs, output)) {
      more = false;  // SoXR error or stall; process() has finished it
    }
    source_pos_ += pairs;
    if (!more) stream_->finish(output);
    store_output(output);
    if (!more) conversion_done_ = true;
    if (progress) {
      progress(stored_pairs_.load(std::memory_order_relaxed), total_pairs_);
    }
  }
  if (stored_pairs_.load(std::memory_order_relaxed) >= total_pairs_) {
    finish_conversion();
  }
}

void SynchronousAudioCache::store_output(std::vector<int32_t>& output) const {
  const uint64_t stored = stored_pairs_.load(std::memory_order_relaxed);
  // Truncate the excess past audio_pair_offset(frame_count).
  const uint64_t room = total_pairs_ > stored ? total_pairs_ - stored : 0;
  const auto pairs =
      static_cast<size_t>(std::min<uint64_t>(output.size() / 2, room));
  if (pairs == 0) return;
  if (!store_->append(output.data(), pairs)) {
    ORC_LOG_ERROR(
        "SynchronousAudioCache: failed writing the audio cache (disk full?) "
        "after {} pairs; the rest of the audio is served as silence",
        stored);
    conversion_done_ = true;
    stream_.reset();
    return;
  }
  stored_pairs_.store(stored + pairs, std::memory_order_release);
}

void SynchronousAudioCache::finish_conversion() const {
  // Enough synchronous pairs are stored; later SoXR output would be
  // truncated, so the source need not be read further.
  conversion_done_ = true;
  stream_.reset();
}

}  // namespace orc
