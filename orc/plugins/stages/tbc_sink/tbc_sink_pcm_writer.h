/*
 * File:        tbc_sink_pcm_writer.h
 * Module:      orc-stage-plugin-tbc-sink
 * Purpose:     Streams the 48 kHz pipeline channel pair into the 44.1 kHz
 *              16-bit .pcm sidecar as the export walks its frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio-resample/audio_resampler.h"

namespace orc {

class IFileWriterUint8;

// Resamples pipeline audio (48 kHz, 24-bit-in-int32 interleaved stereo) to
// the sidecar's 44100 Hz, narrows it to signed 16-bit little-endian, and
// writes it as it arrives, so no part of the stream is held beyond one
// resampler chunk.
//
// The output is exactly the first |declared_pairs| pairs of a single SoXR
// pass over the whole stream (AudioResampleStream gives the same samples
// however the input is split), silence-padded if the stream comes out short:
// the metadata has already declared that length.
//
// Thread-safety: none; one instance belongs to one export.
class TbcPcmStreamWriter {
 public:
  // |writer| must already be open.
  TbcPcmStreamWriter(std::shared_ptr<IFileWriterUint8> writer,
                     uint64_t declared_pairs);

  // False when the resampler could not be created.
  bool ok() const { return ok_; }

  // Append |pairs| interleaved stereo pairs of pipeline audio.
  void append(const int32_t* interleaved, size_t pairs);

  // Flush the resampler, pad to |final_pairs| (the length the frames
  // actually exported declare; never more than the constructor's) and close
  // the file. Returns false if the resampler failed at any point.
  bool finish(uint64_t final_pairs);

  // Close the file without completing it (a cancelled or failed export).
  void abandon();

  uint64_t pairs_written() const { return written_; }

 private:
  void feed_pending();
  void emit(const std::vector<int32_t>& resampled);
  void write_silence(uint64_t pairs);

  std::shared_ptr<IFileWriterUint8> writer_;
  uint64_t declared_pairs_;
  AudioResampleStream stream_;
  bool ok_;
  bool closed_ = false;
  std::vector<int32_t> pending_;  // input not yet handed to the resampler
  std::vector<int32_t> resampled_;
  std::vector<uint8_t> bytes_;
  uint64_t written_ = 0;
};

}  // namespace orc
