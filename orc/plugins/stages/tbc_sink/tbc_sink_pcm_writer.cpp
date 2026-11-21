/*
 * File:        tbc_sink_pcm_writer.cpp
 * Module:      orc-stage-plugin-tbc-sink
 * Purpose:     Streams the 48 kHz pipeline channel pair into the 44.1 kHz
 *              16-bit .pcm sidecar as the export walks its frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "tbc_sink_pcm_writer.h"

#include <orc/plugin/orc_stage_services.h>
#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/support/logging.h>

#include <algorithm>

#include "tbc_sink_sidecars.h"

namespace orc {

TbcPcmStreamWriter::TbcPcmStreamWriter(std::shared_ptr<IFileWriterUint8> writer,
                                       uint64_t declared_pairs)
    : writer_(std::move(writer)),
      declared_pairs_(declared_pairs),
      stream_(static_cast<double>(kAudioSampleRateHz),
              static_cast<double>(kTbcPcmSampleRateHz)),
      ok_(stream_.ok()) {
  if (!ok_) {
    ORC_LOG_ERROR("TBCSink: Audio resampler to {} Hz could not be created",
                  kTbcPcmSampleRateHz);
  }
  pending_.reserve(AudioResampleStream::kChunkFrames * 2);
}

void TbcPcmStreamWriter::append(const int32_t* interleaved, size_t pairs) {
  if (!ok_ || closed_) return;
  // Batched to the resampler's own chunk so each SoXR call is a full one.
  while (pairs > 0) {
    const size_t room = AudioResampleStream::kChunkFrames - pending_.size() / 2;
    const size_t take = std::min(room, pairs);
    pending_.insert(pending_.end(), interleaved, interleaved + take * 2);
    interleaved += take * 2;
    pairs -= take;
    if (pending_.size() / 2 == AudioResampleStream::kChunkFrames) {
      feed_pending();
    }
  }
}

void TbcPcmStreamWriter::feed_pending() {
  if (pending_.empty()) return;
  resampled_.clear();
  if (!stream_.process(pending_.data(), pending_.size() / 2, resampled_)) {
    ORC_LOG_ERROR("TBCSink: Audio resample to {} Hz failed",
                  kTbcPcmSampleRateHz);
    ok_ = false;
  }
  pending_.clear();
  emit(resampled_);
}

void TbcPcmStreamWriter::emit(const std::vector<int32_t>& resampled) {
  if (written_ >= declared_pairs_) return;
  const size_t pairs = static_cast<size_t>(
      std::min<uint64_t>(resampled.size() / 2, declared_pairs_ - written_));
  if (pairs == 0) return;
  bytes_.resize(pairs * 4);
  for (size_t i = 0; i < pairs * 2; ++i) {
    const uint16_t v =
        static_cast<uint16_t>(tbc_pcm_narrow_24_to_16(resampled[i]));
    bytes_[i * 2] = static_cast<uint8_t>(v & 0xFF);
    bytes_[i * 2 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
  }
  writer_->write(bytes_);
  written_ += pairs;
}

void TbcPcmStreamWriter::write_silence(uint64_t pairs) {
  constexpr uint64_t kSilenceChunkPairs = 1u << 16;
  while (pairs > 0) {
    const size_t chunk =
        static_cast<size_t>(std::min(kSilenceChunkPairs, pairs));
    bytes_.assign(chunk * 4, 0);
    writer_->write(bytes_);
    written_ += chunk;
    pairs -= chunk;
  }
}

bool TbcPcmStreamWriter::finish(uint64_t final_pairs) {
  if (closed_) return ok_;
  declared_pairs_ = std::min(declared_pairs_, final_pairs);
  if (ok_) {
    feed_pending();
    resampled_.clear();
    if (ok_) stream_.finish(resampled_);
    emit(resampled_);
  }
  if (ok_ && written_ < declared_pairs_) {
    write_silence(declared_pairs_ - written_);
  }
  writer_->close();
  closed_ = true;
  return ok_;
}

void TbcPcmStreamWriter::abandon() {
  if (closed_) return;
  writer_->close();
  closed_ = true;
}

}  // namespace orc
