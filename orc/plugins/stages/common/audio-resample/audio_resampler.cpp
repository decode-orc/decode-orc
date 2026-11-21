/*
 * File:        audio_resampler.cpp
 * Module:      orc-audio-resample (shared stage-plugin library)
 * Purpose:     SoXR-based stereo conversion of any-rate audio to the
 *              synchronous 48 kHz 24-bit channel-pair pipeline form
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "audio_resampler.h"

#include <orc/support/logging.h>
#include <soxr.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace orc {

namespace {

constexpr unsigned kChannels = 2;

}  // namespace

// ---------------------------------------------------------------------------
// AudioResampleStream
// ---------------------------------------------------------------------------

AudioResampleStream::AudioResampleStream(double in_rate, double out_rate) {
  // SoXR HQ quality, int32 interleaved I/O.
  // SOXR_INT32_I = signed 32-bit interleaved (all channels in one array).
  // The 24-bit-in-int32 carrier passes through unchanged in scale: SoXR is
  // linear, so 24-bit-ranged input yields 24-bit-ranged output.
  const soxr_io_spec_t io_spec = soxr_io_spec(SOXR_INT32_I, SOXR_INT32_I);
  const soxr_quality_spec_t quality = soxr_quality_spec(SOXR_HQ, 0);

  soxr_error_t err = nullptr;
  soxr_ = soxr_create(in_rate, out_rate, kChannels, &err, &io_spec, &quality,
                      nullptr);
  if (!soxr_ || err) {
    ORC_LOG_ERROR("AudioResampler: soxr_create failed: {}",
                  err ? err : "null handle");
    if (soxr_) soxr_delete(soxr_);
    soxr_ = nullptr;
    return;
  }

  // The stream is fed to SoXR in fixed-size chunks rather than in a single
  // whole-stream call. SoXR buffers everything handed to it in an internal
  // FIFO that grows by exactly one block per realloc (soxr fifo.h:
  // `allocation += n`, never doubling), so a one-shot call makes the resample
  // quadratic on any allocator that cannot extend a large block in place. On
  // glibc that is hidden by mremap, but the Windows heap copies every time,
  // and an hour-long capture (~150 M frames) spent nearly an hour here
  // shifting tens of terabytes (issue #230). Chunking bounds the FIFO to a
  // few hundred KB and makes the cost linear on every platform; the output is
  // byte-identical to the one-shot call.
  //
  // Per-chunk output capacity. A short capacity is not lossy: SoXR simply
  // consumes less input and the loop comes back for the rest. Sizing it to
  // the rate ratio plus slack keeps the common case to one pass per chunk.
  chunk_capacity_ =
      static_cast<size_t>(
          std::ceil(static_cast<double>(kChunkFrames) * out_rate / in_rate)) +
      256;
  chunk_.resize(chunk_capacity_ * kChannels);
}

AudioResampleStream::~AudioResampleStream() {
  if (soxr_) soxr_delete(soxr_);
}

void AudioResampleStream::drain_into(size_t frames,
                                     std::vector<int32_t>& out) const {
  out.insert(out.end(), chunk_.begin(),
             chunk_.begin() + static_cast<std::ptrdiff_t>(frames * kChannels));
}

bool AudioResampleStream::process(const int32_t* in_stereo, size_t in_frames,
                                  std::vector<int32_t>& out) {
  if (!soxr_) return false;
  size_t done = 0;
  while (done < in_frames) {
    const size_t want = std::min(kChunkFrames, in_frames - done);
    size_t idone = 0;
    size_t odone = 0;
    const soxr_error_t err =
        soxr_process(soxr_, in_stereo + done * kChannels, want, &idone,
                     chunk_.data(), chunk_capacity_, &odone);
    if (err) {
      ORC_LOG_WARN("AudioResampler: soxr_process error: {}", err);
      drain_into(odone, out);
      soxr_delete(soxr_);
      soxr_ = nullptr;
      return false;
    }
    drain_into(odone, out);
    // No input accepted and no output produced means SoXR cannot make
    // progress; stop rather than spin. Output alone still counts as progress
    // (the FIFO drained, so the next call can take input).
    if (idone == 0 && odone == 0) {
      ORC_LOG_WARN(
          "AudioResampler: SoXR stalled after {} input frames; the remainder "
          "is dropped",
          consumed_ + done);
      consumed_ += done;
      finish(out);
      return false;
    }
    done += idone;
  }
  consumed_ += done;
  return true;
}

void AudioResampleStream::finish(std::vector<int32_t>& out) {
  if (!soxr_) return;
  // Flush residual samples (nullptr input signals end-of-stream to SoXR),
  // draining until it stops producing.
  for (;;) {
    size_t odone = 0;
    const soxr_error_t err = soxr_process(
        soxr_, nullptr, 0, nullptr, chunk_.data(), chunk_capacity_, &odone);
    if (err) {
      ORC_LOG_WARN("AudioResampler: soxr_process flush error: {}", err);
      break;
    }
    if (odone == 0) break;
    drain_into(odone, out);
  }
  soxr_delete(soxr_);
  soxr_ = nullptr;
}

// ---------------------------------------------------------------------------
// widen_16_to_24
// ---------------------------------------------------------------------------

std::vector<int32_t> AudioResampler::widen_16_to_24(
    const std::vector<int16_t>& input) {
  std::vector<int32_t> output(input.size());
  std::transform(
      input.begin(), input.end(), output.begin(),
      // * 256, not << 8: shifting a negative value is undefined in C++17.
      [](int16_t s) { return static_cast<int32_t>(s) * 256; });
  return output;
}

// ---------------------------------------------------------------------------
// resample
// ---------------------------------------------------------------------------

std::vector<int32_t> AudioResampler::resample(
    const std::vector<int32_t>& input_stereo, double in_rate, double out_rate) {
  if (input_stereo.empty()) return {};
  if (in_rate == out_rate) return input_stereo;

  const size_t in_frames = input_stereo.size() / kChannels;
  if (in_frames == 0) return {};

  AudioResampleStream stream(in_rate, out_rate);
  if (!stream.ok()) return {};

  std::vector<int32_t> output;
  // Estimate the output frame count with a small safety margin so the whole
  // result lands in one allocation.
  output.reserve((static_cast<size_t>(std::lround(
                      static_cast<double>(in_frames) * out_rate / in_rate)) +
                  64) *
                 kChannels);

  if (stream.process(input_stereo.data(), in_frames, output)) {
    stream.finish(output);
  }
  return output;
}

// ---------------------------------------------------------------------------
// resample_to_synchronous
// ---------------------------------------------------------------------------

std::vector<std::vector<int32_t>> AudioResampler::resample_to_synchronous(
    const std::vector<int32_t>& raw_stereo, double in_rate_hz,
    VideoSystem system, size_t frame_count) {
  std::vector<std::vector<int32_t>> frames(frame_count);
  if (system == VideoSystem::Unknown) return frames;

  if (raw_stereo.empty() || frame_count == 0) {
    for (size_t i = 0; i < frame_count; ++i) {
      frames[i].assign(static_cast<size_t>(audio_pairs_in_frame(i, system)) * 2,
                       0);
    }
    return frames;
  }

  // Convert to the synchronous 48 kHz rate (no-op for 48 kHz input).
  std::vector<int32_t> resampled;
  const std::vector<int32_t>* stream = &raw_stereo;
  if (in_rate_hz != static_cast<double>(kAudioSampleRateHz)) {
    resampled = resample(raw_stereo, in_rate_hz,
                         static_cast<double>(kAudioSampleRateHz));
    stream = &resampled;
  }

  // Segment into cadence-sized blocks; zero-pad short material and truncate
  // excess so the blocks total exactly audio_pair_offset(frame_count) pairs.
  for (size_t i = 0; i < frame_count; ++i) {
    const size_t block_pairs =
        static_cast<size_t>(audio_pairs_in_frame(i, system));
    const size_t src_start =
        static_cast<size_t>(audio_pair_offset(i, system)) * 2;
    const size_t src_end = src_start + block_pairs * 2;

    frames[i].assign(block_pairs * 2, 0);

    if (src_start < stream->size()) {
      const size_t available = std::min(src_end, stream->size()) - src_start;
      std::memcpy(frames[i].data(), stream->data() + src_start,
                  available * sizeof(int32_t));
      // Remaining values are already zero from assign().
    }
    // When src_start >= stream->size() the frame is silent (all zeros).
  }

  return frames;
}

}  // namespace orc
