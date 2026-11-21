/*
 * File:        audio_import_stage_deps_interface.h
 * Module:      orc-stage-plugin-audio_import
 * Purpose:     Dependency interface for AudioImportStage (mockable WAV
 *              and FLAC file probing and sample access seam)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace orc {

class ISynchronousAudioStore;

// Result of probing an external audio file for import. The file is valid
// only when it is either
//   - a WAV (RIFF/WAVE, RF64 or BW64): PCM, stereo, 16- or 24-bit, or
//   - a FLAC stream: stereo, 24 bits per sample or fewer,
// with a non-zero sample rate. The stage converts the material to the
// synchronous 48 kHz 24-bit channel-pair pipeline form on ingest, so any
// SoXR-supported rate is acceptable.
struct AudioImportProbeResult {
  bool valid{false};
  std::string error;            // human-readable reason when !valid
  uint32_t sample_rate{0};      // header sample rate in Hz
  uint16_t bits_per_sample{0};  // as stored: 16 or 24 (WAV), 1-24 (FLAC)
  // Stereo pairs in the file; 0 for a FLAC stream that does not record its
  // length, which is then read to the end of its data.
  uint64_t pair_count{0};
};

// Dependency seam for AudioImportStage: probes the audio file and serves its
// samples as interleaved stereo pairs. Split out so unit tests can mock the
// file access (no filesystem in unit tests).
//
// Samples are read a range at a time: the ingest conversion is sequential
// and never holds the whole stream.
//
// Thread safety: open() is called once before the wrapped representation is
// published; the other methods may then be called concurrently from any
// thread.
class IAudioImportDeps {
 public:
  virtual ~IAudioImportDeps() = default;

  // Opens and validates |path| (WAV or FLAC, told apart by content, not by
  // file name). On success the instance is bound to the file and the read
  // methods serve its samples.
  virtual AudioImportProbeResult open(const std::string& path) = 0;

  // Up to |pair_count| stereo pairs starting at |first_pair|, interleaved,
  // in the 24-bit-in-int32 carrier: 16-bit samples are widened (<< 8),
  // 24-bit samples sign-extended (narrower FLAC samples are widened the same
  // way). Fewer pairs (whole pairs only) at the end of the data.
  virtual std::vector<int32_t> read_pairs(uint64_t first_pair,
                                          size_t pair_count) const = 0;

  // Store for the converted (48 kHz) audio of a file at another rate,
  // created on the first audio access. Production: a scratch file in the
  // audio cache directory. nullptr means none could be created; the audio is
  // then served as silence.
  virtual std::unique_ptr<ISynchronousAudioStore> make_audio_store() const = 0;
};

}  // namespace orc
