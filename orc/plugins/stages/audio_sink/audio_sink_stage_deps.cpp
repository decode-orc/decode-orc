/*
 * File:        audio_sink_stage_deps.cpp
 * Module:      orc-core
 * Purpose:     AudioSinkStage dependency implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "audio_sink_stage_deps.h"

#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cstddef>
#include <limits>
#include <utility>

namespace orc {
namespace {

uint32_t clamp_to_uint32(uint64_t value) {
  return value > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())
             ? std::numeric_limits<uint32_t>::max()
             : static_cast<uint32_t>(value);
}

// Pack the 24-bit-in-int32 pipeline carrier into 24-bit signed LE PCM:
// exactly |expected_values| samples (2 × stereo pairs), zero-padded when the
// input is short, truncated when long, saturated to the 24-bit range
// defensively (producers already guarantee cadence-sized, in-range frames).
std::vector<uint8_t> pack_s24le(const std::vector<int32_t>& samples,
                                size_t expected_values) {
  std::vector<uint8_t> bytes(expected_values * 3, 0);
  const size_t copy_values = std::min(samples.size(), expected_values);
  for (size_t i = 0; i < copy_values; ++i) {
    const int32_t v = std::clamp(samples[i], -8388608, 8388607);
    bytes[i * 3] = static_cast<uint8_t>(v & 0xFF);
    bytes[i * 3 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    bytes[i * 3 + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
  }
  return bytes;
}

}  // namespace

void AudioSinkStageDeps::init(TriggerProgressCallback progress_callback,
                              std::atomic<bool>* is_processing,
                              std::atomic<bool>* cancel_requested) {
  progress_callback_ = std::move(progress_callback);
  is_processing_ = is_processing;
  cancel_requested_ = cancel_requested;
}

std::vector<uint8_t> AudioSinkStageDeps::build_wav_header(
    uint32_t num_pairs) const {
  constexpr uint16_t kNumChannels = 2;
  constexpr uint16_t kBitsPerSample = kAudioBitDepth;                  // 24
  constexpr uint16_t kBlockAlign = kNumChannels * kBitsPerSample / 8;  // 6
  const uint32_t byte_rate = kAudioSampleRateHz * kBlockAlign;
  const uint32_t data_size = num_pairs * kBlockAlign;
  const uint32_t file_size = 36 + data_size;

  std::vector<uint8_t> header;
  header.reserve(44);
  auto append_le16 = [&header](uint16_t v) {
    header.push_back(static_cast<uint8_t>(v & 0xFF));
    header.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  };
  auto append_le32 = [&header](uint32_t v) {
    header.push_back(static_cast<uint8_t>(v & 0xFF));
    header.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    header.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    header.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  };

  header.insert(header.end(), {'R', 'I', 'F', 'F'});
  append_le32(file_size);
  header.insert(header.end(), {'W', 'A', 'V', 'E'});
  header.insert(header.end(), {'f', 'm', 't', ' '});
  append_le32(16);  // fmt chunk size
  append_le16(1);   // PCM
  append_le16(kNumChannels);
  append_le32(kAudioSampleRateHz);
  append_le32(byte_rate);
  append_le16(kBlockAlign);
  append_le16(kBitsPerSample);
  header.insert(header.end(), {'d', 'a', 't', 'a'});
  append_le32(data_size);

  return header;
}

AudioSinkWriteResult AudioSinkStageDeps::write_audio_wav(
    const VideoFrameRepresentation* representation,
    const std::string& output_path, size_t pair) {
  const auto descriptor =
      representation->get_audio_channel_pair_descriptor(pair);
  if (!descriptor) {
    return {false, 0,
            "Audio channel pair " + std::to_string(pair) +
                " does not exist in the input (" +
                std::to_string(representation->audio_channel_pair_count()) +
                " channel pair(s) available)"};
  }

  // WAV's header declares the total payload size up front, computed here
  // from frame_range() — which a piped/unbounded source (frame_count left
  // at 0) reports as a huge placeholder rather than its real length, not
  // known until it ends. Unlike the sink stages with a real streaming path,
  // there is no way to patch a WAV header after the fact once it has gone
  // to a pipe, so refuse cleanly rather than declare a fictitious size.
  if (representation->has_unbounded_frame_range()) {
    return {false, 0,
            "Input source has an unbounded frame range (a piped/live "
            "source left frame_count at 0) — WAV declares its total size "
            "in the header up front, which an unbounded source cannot "
            "provide; set an explicit frame_count on it first."};
  }

  const auto params = representation->get_video_parameters();
  const VideoSystem system = params ? params->system : VideoSystem::Unknown;

  // Per-frame pair counts are a model invariant of the video system
  // (SMPTE 272M-1994 §14.3 cadence), so the total payload — which the
  // stream-only writer must declare in the header up front — is known before
  // any samples are read.
  const auto frame_rng = representation->frame_range();
  const uint64_t total_pairs = audio_pair_offset(frame_rng.last + 1, system) -
                               audio_pair_offset(frame_rng.first, system);
  if (total_pairs == 0) {
    return {false, 0, "No audio samples found in frame range"};
  }

  std::shared_ptr<IFileWriterUint8> writer;
  if (stage_services_) {
    writer = stage_services_->create_buffered_file_writer_uint8(
        static_cast<size_t>(4 * 1024 * 1024));
  }
  if (!writer) {
    return {false, 0, "File writer service unavailable"};
  }
  if (!writer->open(output_path)) {
    return {false, 0, "Failed to open output file: " + output_path};
  }

  writer->write(build_wav_header(clamp_to_uint32(total_pairs)));

  const uint64_t total_frames = frame_rng.count();
  uint64_t pairs_written = 0;
  uint64_t current_frame = 0;
  for (FrameID fid = frame_rng.first; fid <= frame_rng.last; ++fid) {
    if (cancel_requested_ && cancel_requested_->load()) {
      writer->close();
      if (is_processing_) {
        is_processing_->store(false);
      }
      return {false, 0, "Cancelled by user"};
    }

    // Cadence-sized payload per frame: frames that yield no audio become
    // silence of the same size, keeping the payload aligned with the header
    // and preserving A/V sync.
    const uint32_t frame_pairs = audio_pairs_in_frame(fid, system);
    const auto samples = representation->get_audio_samples(pair, fid);
    writer->write(pack_s24le(samples, static_cast<size_t>(frame_pairs) * 2));
    pairs_written += frame_pairs;

    ++current_frame;
    if (progress_callback_ && current_frame % 10 == 0) {
      progress_callback_(current_frame, total_frames,
                         "Writing audio frame " +
                             std::to_string(current_frame) + "/" +
                             std::to_string(total_frames));
    }
  }

  writer->close();
  ORC_LOG_DEBUG("AudioSinkDeps: Wrote {} stereo pairs at {} Hz", pairs_written,
                kAudioSampleRateHz);
  return {true, pairs_written, ""};
}
}  // namespace orc
