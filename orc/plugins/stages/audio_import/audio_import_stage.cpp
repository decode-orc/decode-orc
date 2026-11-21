/*
 * File:        audio_import_stage.cpp
 * Module:      orc-stage-plugin-audio_import
 * Purpose:     External WAV import transform stage (VFrameR)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "audio_import_stage.h"

#include <orc/stage/error_types.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <variant>

#include "audio-resample/synchronous_audio_cache.h"
#include "audio_import_stage_deps.h"

namespace orc {

namespace {

// "path/to/My Pair.wav" → "My Pair"; empty input yields the fallback.
std::string file_stem(const std::string& path) {
  const size_t sep = path.find_last_of("/\\");
  std::string name = (sep == std::string::npos) ? path : path.substr(sep + 1);
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) name = name.substr(0, dot);
  return name.empty() ? "Imported audio" : name;
}

}  // namespace

// ============================================================================
// ImportedAudioChannelPairRepresentation
// ============================================================================

std::optional<AudioChannelPairDescriptor>
ImportedAudioChannelPairRepresentation::get_audio_channel_pair_descriptor(
    size_t pair) const {
  if (pair == imported_pair_index()) return descriptor_;
  return source_ ? source_->get_audio_channel_pair_descriptor(pair)
                 : std::nullopt;
}

std::vector<int32_t> ImportedAudioChannelPairRepresentation::get_audio_samples(
    size_t pair, FrameID id) const {
  if (pair != imported_pair_index()) {
    // Out-of-range indices forward too: the wrapped source returns empty for
    // any pair beyond its own count.
    return source_ ? source_->get_audio_samples(pair, id)
                   : std::vector<int32_t>{};
  }
  if (!source_ || !source_->has_frame(id)) return {};
  const auto range = source_->frame_range();
  if (id < range.first) return {};
  const uint64_t index = id - range.first;

  return audio_cache().frame_block(index);
}

const SynchronousAudioCache&
ImportedAudioChannelPairRepresentation::audio_cache() const {
  std::call_once(convert_once_, [this] {
    // Convert to the synchronous pipeline form: 48000 Hz, segmented into
    // cadence-sized per-frame blocks (SMPTE 272M-1994 §14.3), zero-padded or
    // truncated to exactly audio_pair_offset(frame_count_) total pairs. A
    // 48000 Hz WAV is read straight from the file per frame; any other rate
    // is resampled sequentially into a scratch file, as far as the frames
    // asked for.
    auto deps = deps_;
    audio_cache_ = std::make_unique<SynchronousAudioCache>(
        [deps](uint64_t first, size_t count) {
          return deps->read_pairs(first, count);
        },
        static_cast<double>(wav_sample_rate_hz_), system_,
        static_cast<uint64_t>(frame_count_),
        [deps] { return deps->make_audio_store(); });
  });
  return *audio_cache_;
}

// ============================================================================
// AudioImportStage
// ============================================================================

AudioImportStage::AudioImportStage() {
  // wav_path is required.
  set_configuration_status(orc::ConfigurationStatus::Red);
}

std::vector<ArtifactPtr> AudioImportStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  (void)observation_context;
  if (inputs.empty()) {
    throw DAGExecutionError("AudioImportStage requires one input");
  }
  auto vfr =
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  if (!vfr) {
    throw DAGExecutionError(
        "AudioImportStage input must be VideoFrameRepresentation");
  }

  if (!parameters.empty()) set_parameters(parameters);

  if (wav_path_.empty()) {
    throw DAGExecutionError("AudioImportStage: wav_path is not set");
  }

  // The pair cap is enforced at every pair-adding stage, not just at the
  // container sink, so a DAG that validates also exports.
  if (vfr->audio_channel_pair_count() >= kMaxAudioChannelPairs) {
    throw DAGExecutionError(
        "AudioImportStage: cannot append a channel pair: the input already "
        "carries the maximum of " +
        std::to_string(kMaxAudioChannelPairs) + " audio channel pairs");
  }

  auto deps = deps_override_ ? deps_override_
                             : std::static_pointer_cast<IAudioImportDeps>(
                                   std::make_shared<AudioImportDeps>());
  const WavProbeResult probe = deps->open(wav_path_);
  if (!probe.valid) {
    throw DAGExecutionError("AudioImportStage: '" + wav_path_ +
                            "' is not importable: " + probe.error);
  }

  // The synchronous audio layout (cadence) is defined by the project's video
  // standard; without one the imported material has no frame mapping.
  const auto params = vfr->get_video_parameters();
  const VideoSystem system = params ? params->system : VideoSystem::Unknown;
  if (audio_pairs_in_frame(0, system) == 0) {
    throw DAGExecutionError(
        "AudioImportStage: a known video standard is required to define the "
        "synchronous 48 kHz audio layout");
  }

  AudioChannelPairDescriptor descriptor;
  descriptor.name = pair_name_.empty() ? file_stem(wav_path_) : pair_name_;
  descriptor.origin = AudioOrigin::IMPORTED;

  auto output = std::make_shared<ImportedAudioChannelPairRepresentation>(
      vfr, std::move(deps), std::move(descriptor), probe.sample_rate,
      probe.bits_per_sample, system, vfr->frame_count());
  cached_output_ = output;

  return {output};
}

std::vector<ParameterDescriptor> AudioImportStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)project_format;
  (void)source_type;
  std::vector<ParameterDescriptor> descriptors;

  {
    ParameterDescriptor desc;
    desc.name = "wav_path";
    desc.display_name = "WAV File";
    desc.description =
        "External WAV file to attach as a new audio channel pair. Must be "
        "RIFF/WAVE, PCM, stereo, 16- or 24-bit; any sample rate (converted "
        "to synchronous 48 kHz 24-bit on import).";
    desc.type = ParameterType::FILE_PATH;
    desc.file_extension_hint = ".wav";
    desc.constraints.default_value = std::string("");
    desc.constraints.required = true;
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "pair_name";
    desc.display_name = "Channel Pair Name";
    desc.description =
        "Human-readable name for the imported channel pair. Empty uses the "
        "WAV file name.";
    desc.type = ParameterType::STRING;
    desc.constraints.default_value = std::string("");
    descriptors.push_back(desc);
  }

  return descriptors;
}

std::map<std::string, ParameterValue> AudioImportStage::get_parameters() const {
  return {{"wav_path", wav_path_}, {"pair_name", pair_name_}};
}

bool AudioImportStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    if (key == "wav_path") {
      const auto* v = std::get_if<std::string>(&value);
      if (!v) return false;
      wav_path_ = *v;
    } else if (key == "pair_name") {
      const auto* v = std::get_if<std::string>(&value);
      if (!v) return false;
      pair_name_ = *v;
    } else {
      ORC_LOG_WARN("AudioImportStage: unknown parameter '{}'", key);
      return false;
    }
  }
  set_configuration_status(wav_path_.empty() ? orc::ConfigurationStatus::Red
                                             : orc::ConfigurationStatus::Green);
  return true;
}

StagePreviewCapability AudioImportStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_output_);
}

}  // namespace orc
