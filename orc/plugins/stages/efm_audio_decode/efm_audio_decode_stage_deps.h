/*
 * File:        efm_audio_decode_stage_deps.h
 * Module:      orc-stage-plugin-efm_audio_decode
 * Purpose:     Default EFMAudioDecodeStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <filesystem>

#include "efm_audio_decode_stage_deps_interface.h"

namespace orc {

// Production implementation: gathers the EFM t-value stream, runs the shared
// efm-decode pipeline (headerless PCM output) into a scratch cache file in
// the audio cache directory (audio_cache_directory(): disk, not the system
// temp directory, which may be RAM-backed), and serves stereo-pair reads by
// seeking into that file. The converted synchronous pipeline stream (48 kHz
// 24-bit-in-int32) goes to a second scratch file, after which the raw cache
// is discarded. Decoded audio is hundreds of MB/hour, hence disk rather than
// RAM. Cache files are removed when this object is destroyed.
class EFMAudioDecodeDeps : public IEFMAudioDecodeDeps {
 public:
  EFMAudioDecodeDeps() = default;
  ~EFMAudioDecodeDeps() override;

  EFMAudioDecodeDeps(const EFMAudioDecodeDeps&) = delete;
  EFMAudioDecodeDeps& operator=(const EFMAudioDecodeDeps&) = delete;

  EFMAudioDecodeResult decode_to_cache(
      const VideoFrameRepresentation& representation,
      const EFMAudioDecodeOptions& options,
      const ProgressFn& progress) override;

  std::vector<int16_t> read_cache_pairs(uint64_t first_pair,
                                        uint32_t pair_count) const override;

  std::unique_ptr<ISynchronousAudioStore> make_audio_store() const override;

  void discard_cache() override;

 private:
  std::filesystem::path cache_path_;  // empty until decode succeeds
};

}  // namespace orc
