/*
 * File:        efm_audio_decode_stage_deps_interface.h
 * Module:      orc-stage-plugin-efm_audio_decode
 * Purpose:     Dependency interface for EFMAudioDecodeStage (mockable decode
 *              and cache-file access seam)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/video_frame_representation.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace orc {

// Decode options forwarded to the shared EFM decode pipeline (a subset of
// efm_sink's audio-mode options).
struct EFMAudioDecodeOptions {
  bool no_timecodes{false};
  bool no_audio_concealment{false};
  // Q-8: when true, ignore the 50/15 us pre-emphasis flag and skip de-emphasis.
  bool ignore_preemphasis{false};
  // When non-empty, a detailed decode statistics report is written to this
  // path. Empty (the default) means no report is written. Unlike efm_sink,
  // this stage has no user-facing audio output file to append ".txt" to, so
  // the report location is specified explicitly.
  std::string report_path;
  // Human-readable name for the appended EFM audio channel pair. Surfaces as
  // the pair descriptor name (the CVBS container description column and the
  // video sink's per-stream title). Empty falls back to "EFM digital audio".
  std::string pair_name{"EFM digital audio"};
  // User sync slip in milliseconds applied on top of the automatic
  // input-timeline (video) alignment; positive delays the audio relative to
  // the video, negative advances it (issue #231).
  double offset_ms{0.0};
  // Issue #307: smallest producer doubt (0 trusted - 15 distrusted) that makes
  // an otherwise-clean EFM symbol a C1/C2 erasure candidate. 0 disables
  // doubt-derived erasures (bit-exact legacy output).
  uint8_t doubt_erasure_threshold{0};
};

struct EFMAudioDecodeResult {
  bool success{false};
  std::string error_message;      // human-readable reason when !success
  uint64_t stream_pair_count{0};  // decoded 44.1 kHz stereo pairs available
  // Net head adjustment the decoder applied to align the stream with the
  // input (video) timeline, in 44.1 kHz stereo pairs (positive = silence
  // prepended, negative = pairs dropped). Informational.
  int64_t applied_sync_offset_pairs{0};
};

class ISynchronousAudioStore;

// Dependency seam for EFMAudioDecodeStage: performs the whole-stream EFM
// decode into a scratch cache, serves stereo-pair reads from it, and makes
// the store for the converted synchronous (48 kHz 24-bit-in-int32,
// cadence-aligned) pipeline stream. Split out so unit tests can mock the
// decode and file access (no filesystem in unit tests).
//
// Thread safety: decode_to_cache(), read_cache_pairs(), make_audio_store()
// and discard_cache() are called from the one-time decode and conversion
// (the caller serialises via std::call_once).
class IEFMAudioDecodeDeps {
 public:
  virtual ~IEFMAudioDecodeDeps() = default;

  // Progress callback for the whole-stream decode: (done_frames, total_frames,
  // message). Empty/absent means no reporting. Reported at frame granularity
  // so a sink can meter the decode on its progress dialog.
  using ProgressFn =
      std::function<void(uint64_t done, uint64_t total, const std::string&)>;

  // Accumulates the representation's EFM t-value stream across its full frame
  // range, decodes it to headerless 44.1 kHz interleaved stereo int16_t PCM
  // in a scratch cache file, and reports the decoded pair count. When
  // |progress| is set it is invoked periodically as the frame range is
  // streamed into the decoder.
  virtual EFMAudioDecodeResult decode_to_cache(
      const VideoFrameRepresentation& representation,
      const EFMAudioDecodeOptions& options, const ProgressFn& progress) = 0;

  // Reads up to |pair_count| stereo pairs starting at |first_pair| from the
  // raw 44.1 kHz cache written by decode_to_cache(). Returns interleaved L,R
  // samples; short reads are truncated to whole pairs. Empty when no cache
  // exists.
  virtual std::vector<int16_t> read_cache_pairs(uint64_t first_pair,
                                                uint32_t pair_count) const = 0;

  // Store for the converted pipeline stream (48 kHz synchronous,
  // 24-bit-in-int32, interleaved stereo). Production: a scratch file in the
  // audio cache directory. nullptr when none can be created.
  virtual std::unique_ptr<ISynchronousAudioStore> make_audio_store() const = 0;

  // Removes the raw 44.1 kHz cache once the converted stream holds all of it.
  virtual void discard_cache() = 0;
};

}  // namespace orc
