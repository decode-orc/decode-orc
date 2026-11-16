/*
 * File:        audio_align_stage.cpp
 * Module:      orc-stage-plugin-audio_align
 * Purpose:     Per-channel-pair audio sync adjustment transform stage
 *              (VFrameR)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "audio_align_stage.h"

#include <orc/stage/error_types.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <variant>

namespace orc {

namespace {

// Parses a channel-pair index string in the range [0, kMaxAudioChannelPairs).
// Returns nullopt on any non-numeric or out-of-range value. Mirrors the
// audio_channel_map stage so both share the same string channel-pair contract.
std::optional<int32_t> parse_pair_index(const std::string& text) {
  int parsed = 0;
  try {
    size_t consumed = 0;
    parsed = std::stoi(text, &consumed);
    if (consumed != text.size()) return std::nullopt;
  } catch (const std::exception&) {
    return std::nullopt;
  }
  if (parsed < 0 || parsed >= static_cast<int>(kMaxAudioChannelPairs)) {
    return std::nullopt;
  }
  return static_cast<int32_t>(parsed);
}

// Frame index n whose audio window contains absolute stereo-pair position
// |pos|: audio_pair_offset(n) <= pos < audio_pair_offset(n + 1). The cadence
// is exactly periodic (PAL/SECAM 1920 pairs/frame; NTSC/PAL-M 8008 pairs per
// 5-frame sequence, SMPTE 272M-1994 §14.3), so the rational estimate is at
// most one frame off and each adjustment loop runs at most once.
uint64_t frame_containing_pair(uint64_t pos, VideoSystem system) {
  switch (system) {
    case VideoSystem::PAL:
    case VideoSystem::SECAM:
      return pos / 1920u;
    case VideoSystem::NTSC:
    case VideoSystem::PAL_M: {
      uint64_t n = pos * 5u / 8008u;
      while (audio_pair_offset(n + 1, system) <= pos) ++n;
      while (n > 0 && audio_pair_offset(n, system) > pos) --n;
      return n;
    }
    case VideoSystem::Unknown:
      break;
  }
  return 0;
}

}  // namespace

// ============================================================================
// AlignedAudioChannelPairRepresentation
// ============================================================================

std::vector<int32_t> AlignedAudioChannelPairRepresentation::get_audio_samples(
    size_t pair, FrameID id) const {
  if (!source_) return {};
  if (pair >= source_->audio_channel_pair_count()) return {};
  if (pair != target_pair_) return source_->get_audio_samples(pair, id);
  return assemble_shifted_window(id);
}

std::vector<int32_t>
AlignedAudioChannelPairRepresentation::assemble_shifted_window(
    FrameID id) const {
  // The output window for frame |id| covers absolute stream pair positions
  // [audio_pair_offset(id) - offset, audio_pair_offset(id + 1) - offset).
  // Cumulative audio_pair_offset() arithmetic — never a constant per-frame
  // stride — keeps the NTSC/PAL-M 1602/1601 cadence exact when the window
  // crosses frame boundaries.
  const auto params = source_->get_video_parameters();
  const VideoSystem system = params ? params->system : VideoSystem::Unknown;
  const int64_t pairs = static_cast<int64_t>(audio_pairs_in_frame(id, system));
  if (pairs <= 0) return {};

  const auto range = source_->frame_range();
  if (range.empty() || id < range.first || id > range.last) return {};

  // Stream extent in absolute pair coordinates.
  const int64_t stream_begin =
      static_cast<int64_t>(audio_pair_offset(range.first, system));
  const int64_t stream_end =
      static_cast<int64_t>(audio_pair_offset(range.last + 1, system));

  // A positive offset delays the audio relative to the video, so output
  // position q reads source position q - offset.
  std::vector<int32_t> out(static_cast<size_t>(pairs) * 2, 0);
  int64_t src_pos =
      static_cast<int64_t>(audio_pair_offset(id, system)) - offset_pairs_;
  int64_t out_pair = 0;
  while (out_pair < pairs) {
    if (src_pos < stream_begin) {
      // Before the start of the stream: leave silence.
      const int64_t skip = std::min(stream_begin - src_pos, pairs - out_pair);
      src_pos += skip;
      out_pair += skip;
      continue;
    }
    if (src_pos >= stream_end) break;  // past the end: silence tail

    const uint64_t src_frame =
        frame_containing_pair(static_cast<uint64_t>(src_pos), system);
    const int64_t frame_start =
        static_cast<int64_t>(audio_pair_offset(src_frame, system));
    const int64_t frame_pairs =
        static_cast<int64_t>(audio_pairs_in_frame(src_frame, system));
    const int64_t within = src_pos - frame_start;
    const int64_t take = std::min(frame_pairs - within, pairs - out_pair);
    const auto src = source_->get_audio_samples(target_pair_, src_frame);
    for (int64_t p = 0; p < take; ++p) {
      const size_t sp = static_cast<size_t>(within + p) * 2;
      if (sp + 1 < src.size()) {
        out[static_cast<size_t>(out_pair + p) * 2] = src[sp];
        out[static_cast<size_t>(out_pair + p) * 2 + 1] = src[sp + 1];
      }
    }
    src_pos += take;
    out_pair += take;
  }
  return out;
}

// ============================================================================
// AudioAlignStage
// ============================================================================

AudioAlignStage::AudioAlignStage() {
  // Both parameters have complete defaults (zero offset = pass-through).
  set_configuration_status(orc::ConfigurationStatus::Green);
}

std::vector<ArtifactPtr> AudioAlignStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  (void)observation_context;
  if (inputs.empty()) {
    throw DAGExecutionError("AudioAlignStage requires one input");
  }
  auto vfr =
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  if (!vfr) {
    throw DAGExecutionError(
        "AudioAlignStage input must be VideoFrameRepresentation");
  }

  if (!parameters.empty()) set_parameters(parameters);

  // A channel pair the input does not carry (most commonly a source with no
  // audio at all) is a configuration mismatch, not a pipeline failure. The
  // stage passes its input through instead of throwing, because a DAG
  // execution error here blanks the preview of every downstream node —
  // including the video sink — for a fault that only concerns audio.
  const size_t pair = static_cast<size_t>(channel_pair_);
  const size_t pair_count = vfr->audio_channel_pair_count();
  if (pair >= pair_count) {
    const std::string reason = "channel pair " + std::to_string(channel_pair_) +
                               " is out of range: the input carries " +
                               std::to_string(pair_count) +
                               " audio channel pair(s)";
    // Executed once per render request, so only log when the reason changes.
    if (reason != last_inactive_reason_) {
      ORC_LOG_WARN("AudioAlignStage: {}; passing the input through unchanged",
                   reason);
      last_inactive_reason_ = reason;
    }
    cached_output_ = vfr;
    return {inputs[0]};
  }
  last_inactive_reason_.clear();

  auto output = process(vfr);
  cached_output_ = output;

  if (output == vfr) {
    // Zero offset: pass the input artifact through unchanged.
    return {inputs[0]};
  }

  std::vector<ArtifactPtr> outputs;
  outputs.push_back(
      std::const_pointer_cast<AlignedAudioChannelPairRepresentation>(
          std::dynamic_pointer_cast<
              const AlignedAudioChannelPairRepresentation>(output)));
  return outputs;
}

std::shared_ptr<const VideoFrameRepresentation> AudioAlignStage::process(
    std::shared_ptr<const VideoFrameRepresentation> source) const {
  if (!source) return nullptr;

  // SMPTE 272M-1994 §1.2: every channel pair is 48000 Hz synchronous, so a
  // millisecond offset converts at exactly 48 stereo pairs per millisecond.
  const int64_t offset_pairs = static_cast<int64_t>(std::llround(
      offset_ms_ * static_cast<double>(kAudioSampleRateHz) / 1000.0));

  if (offset_pairs == 0) return source;
  return std::make_shared<AlignedAudioChannelPairRepresentation>(
      std::move(source), static_cast<size_t>(channel_pair_), offset_pairs);
}

std::vector<ParameterDescriptor> AudioAlignStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)project_format;
  (void)source_type;
  std::vector<ParameterDescriptor> descriptors;

  {
    ParameterDescriptor desc;
    desc.name = "channel_pair";
    desc.display_name = "Channel pair";
    desc.description =
        "Audio channel pair to shift (0-based, matching the CVBS container "
        "channel-pair numbering). The GUI restricts the choices to the channel "
        "pairs the input actually carries.";
    desc.type = ParameterType::STRING;
    for (size_t p = 0; p < kMaxAudioChannelPairs; ++p) {
      desc.constraints.allowed_strings.push_back(std::to_string(p));
    }
    desc.constraints.default_value = std::string("0");
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "offset_ms";
    desc.display_name = "Offset (ms)";
    desc.description =
        "Time offset in milliseconds. Positive values delay the audio "
        "relative to the video (insert lead-in); negative values advance it "
        "(trim from the start).";
    desc.type = ParameterType::DOUBLE;
    // A finite range keeps the GUI spin box a sensible width; ±1 hour is far
    // beyond any real audio/video sync correction.
    desc.constraints.min_value = -3'600'000.0;
    desc.constraints.max_value = 3'600'000.0;
    desc.constraints.default_value = 0.0;
    descriptors.push_back(desc);
  }

  return descriptors;
}

std::map<std::string, ParameterValue> AudioAlignStage::get_parameters() const {
  return {{"channel_pair", std::to_string(channel_pair_)},
          {"offset_ms", offset_ms_}};
}

bool AudioAlignStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    if (key == "channel_pair") {
      const auto* v = std::get_if<std::string>(&value);
      if (!v) {
        ORC_LOG_ERROR("AudioAlignStage: channel_pair must be a string");
        return false;
      }
      const auto parsed = parse_pair_index(*v);
      if (!parsed) {
        ORC_LOG_ERROR("AudioAlignStage: invalid channel_pair '{}'", *v);
        return false;
      }
      channel_pair_ = *parsed;
    } else if (key == "offset_ms") {
      const auto* v = std::get_if<double>(&value);
      if (!v) {
        ORC_LOG_ERROR("AudioAlignStage: offset_ms must be a number");
        return false;
      }
      offset_ms_ = *v;
    } else {
      ORC_LOG_WARN("AudioAlignStage: unknown parameter '{}'", key);
      return false;
    }
  }
  return true;
}

StagePreviewCapability AudioAlignStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_output_);
}

}  // namespace orc
