/*
 * File:        audio_resampler.h
 * Module:      orc-audio-resample (shared stage-plugin library)
 * Purpose:     SoXR-based stereo conversion of any-rate audio to the
 *              synchronous 48 kHz 24-bit channel-pair pipeline form
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/common_types.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Opaque SoXR handle (soxr.h), so consumers need no SoXR include path.
struct soxr;

namespace orc {

// ---------------------------------------------------------------------------
// AudioResampleStream
// ---------------------------------------------------------------------------
// Sequential SoXR HQ resampler for interleaved stereo int32 (24-bit-in-int32
// carrier) that is fed the stream piece by piece, so no whole-stream buffer
// exists. Feeding a stream through process() in pieces of any size and then
// calling finish() yields exactly the samples AudioResampler::resample()
// returns for the whole stream: that function is built on this class.
//
// Thread-safety: none; one instance belongs to one sequential pass.
class AudioResampleStream {
 public:
  AudioResampleStream(double in_rate, double out_rate);
  ~AudioResampleStream();

  AudioResampleStream(const AudioResampleStream&) = delete;
  AudioResampleStream& operator=(const AudioResampleStream&) = delete;

  // False when SoXR could not be created; process() and finish() then
  // produce nothing.
  bool ok() const { return soxr_ != nullptr; }

  // Resample |in_frames| stereo pairs and append the output to |out|.
  // Returns false (after appending whatever SoXR produced) on a SoXR error or
  // when SoXR stops accepting input; the stream is then finished.
  bool process(const int32_t* in_stereo, size_t in_frames,
               std::vector<int32_t>& out);

  // Signal end of stream and append the remaining output to |out|. Further
  // calls do nothing.
  void finish(std::vector<int32_t>& out);

  // Input pairs per SoXR call. Kept at the size the whole-stream path has
  // always used (issue #230); readers that feed this class use it as their
  // read size.
  static constexpr size_t kChunkFrames = 65536;

 private:
  void drain_into(size_t frames, std::vector<int32_t>& out) const;

  soxr* soxr_ = nullptr;
  size_t chunk_capacity_ = 0;
  std::vector<int32_t> chunk_;
  size_t consumed_ = 0;  // input pairs accepted so far (diagnostics)
};

// ---------------------------------------------------------------------------
// AudioResampler
// ---------------------------------------------------------------------------
// The single ingest-conversion authority for pipeline audio: converts stereo
// PCM of any source rate into the only permitted pipeline form — 48000 Hz
// synchronous (frame-locked) 24-bit-in-int32 stereo channel pairs
// (SMPTE 272M-1994). Used by tbc_source, cvbs_source, efm_audio_decode, and
// audio_import at the point of production.
//
// Resampling uses SoXR HQ quality and is duration- and sync-preserving.
//
// Thread-safety: stateless static methods — safe to call from any thread.
class AudioResampler {
 public:
  // Widen 16-bit samples to the 24-bit-in-int32 carrier (<< 8). Exactly
  // reversible; no scaling or dithering.
  static std::vector<int32_t> widen_16_to_24(const std::vector<int16_t>& input);

  // Resample interleaved stereo int32 PCM (24-bit-in-int32 carrier) from
  // in_rate to out_rate Hz using SoXR HQ resampling. Returns interleaved
  // stereo int32 at out_rate; a same-rate call returns the input unchanged.
  //
  // Returns empty vector on error (e.g. SoXR allocation failure).
  static std::vector<int32_t> resample(const std::vector<int32_t>& input_stereo,
                                       double in_rate, double out_rate);

  // Convert an interleaved stereo int32 stream of any rate into frame_count
  // synchronous per-frame blocks for the given video system:
  //   1. Resample in_rate_hz → 48000 Hz (SoXR HQ; skipped when the input is
  //      already 48000 Hz).
  //   2. Segment into cadence-sized blocks of audio_pairs_in_frame(i) stereo
  //      pairs (PAL 1920 constant; NTSC/PAL-M 1602/1601 per the SMPTE
  //      272M-1994 §14.3 audio frame sequence).
  // Short material is zero-padded and excess truncated, so the blocks total
  // exactly audio_pair_offset(frame_count) pairs. Always returns frame_count
  // blocks; an unknown video system has no audio layout and yields empty
  // blocks.
  static std::vector<std::vector<int32_t>> resample_to_synchronous(
      const std::vector<int32_t>& raw_stereo, double in_rate_hz,
      VideoSystem system, size_t frame_count);
};

}  // namespace orc
