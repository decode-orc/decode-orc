/*
 * File:        audio_sink_stage.cpp
 * Module:      orc-core
 * Purpose:     Audio Sink Stage - writes PCM audio to WAV file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "audio_sink_stage.h"

#include <orc/abi/orc_plugin_services.h>
#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/common_types.h>
#include <orc/support/logging.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <variant>

#include "audio_sink_stage_deps.h"
#include "audio_sink_stage_deps_interface.h"

namespace orc {

namespace {

// Parses a fully-numeric channel-pair index string to an integer. Returns
// nullopt when the text is not a plain integer; the caller applies the range
// check so it can report a specific out-of-range message.
std::optional<int32_t> parse_channel_pair_string(const std::string& text) {
  try {
    size_t consumed = 0;
    const int parsed = std::stoi(text, &consumed);
    if (consumed != text.size()) return std::nullopt;
    return static_cast<int32_t>(parsed);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace

AudioSinkStage::AudioSinkStage() {
  set_configuration_status(orc::ConfigurationStatus::Red);
}

NodeTypeInfo AudioSinkStage::get_node_type_info() const {
  return NodeTypeInfo{NodeType::SINK,
                      "AudioSink",
                      "Audio Sink",
                      "Extracts audio data and writes it to a WAV file",
                      1,
                      1,  // One input
                      0,
                      0,  // No outputs (sink)
                      VideoFormatCompatibility::ALL};
}

std::vector<ArtifactPtr> AudioSinkStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  // Sink stages don't produce outputs in execute()
  // Actual work happens in trigger()
  (void)inputs;
  (void)parameters;
  (void)observation_context;
  return {};
}

std::vector<ParameterDescriptor> AudioSinkStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)project_format;
  (void)source_type;
  std::vector<ParameterDescriptor> descriptors;

  // output_path parameter
  {
    ParameterDescriptor desc;
    desc.name = "output_path";
    desc.display_name = "Output WAV File";
    desc.description =
        "Path to output WAV audio file. \"-\" writes the WAV stream to the "
        "CLI process's standard output instead of a file — runs only via "
        "the CLI; settable here for a project you'll execute there. The "
        "header's data size is computed up "
        "front from the frame range, so no seek-back is needed and the "
        "whole file streams in one forward pass.";
    desc.type = ParameterType::FILE_PATH;
    desc.constraints.required = true;
    desc.constraints.default_value = std::string("");
    desc.file_extension_hint = ".wav";
    descriptors.push_back(desc);
  }

  // channel_pair parameter — which audio channel pair to write. Channel pair
  // indices are 0-based, matching the CVBS container's _audio_<p>.wav
  // numbering.
  {
    ParameterDescriptor desc;
    desc.name = "channel_pair";
    desc.display_name = "Audio Channel Pair";
    desc.description =
        "Audio channel pair to write (0-based, matching the CVBS container "
        "channel pair numbering). The input may carry up to " +
        std::to_string(kMaxAudioChannelPairs) +
        " stereo channel pairs; triggering fails if the selected channel "
        "pair does not exist. The GUI restricts the choices to the channel "
        "pairs the input actually carries.";
    desc.type = ParameterType::STRING;
    desc.constraints.required = false;
    for (size_t p = 0; p < kMaxAudioChannelPairs; ++p) {
      desc.constraints.allowed_strings.push_back(std::to_string(p));
    }
    desc.constraints.default_value = std::string("0");
    descriptors.push_back(desc);
  }

  return descriptors;
}

std::map<std::string, ParameterValue> AudioSinkStage::get_parameters() const {
  return parameters_;
}

bool AudioSinkStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  parameters_ = params;

  const auto it = params.find("output_path");
  const bool has_path =
      (it != params.end() && std::holds_alternative<std::string>(it->second) &&
       !std::get<std::string>(it->second).empty());

  set_configuration_status(has_path ? orc::ConfigurationStatus::Green
                                    : orc::ConfigurationStatus::Red);
  return true;
}

bool AudioSinkStage::trigger(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    IObservationContext& observation_context) {
  (void)observation_context;
  is_processing_.store(true);
  cancel_requested_.store(false);

  try {
    // Validate inputs
    if (inputs.empty()) {
      throw std::runtime_error(
          "Audio sink requires one input (VideoFrameRepresentation)");
    }

    auto vfr = std::dynamic_pointer_cast<VideoFrameRepresentation>(inputs[0]);
    if (!vfr) {
      throw std::runtime_error("Input must be a VideoFrameRepresentation");
    }

    // Check if VFrameR has audio
    if (!vfr->has_audio()) {
      throw std::runtime_error(
          "Input VFrameR does not have audio data (no PCM file specified in "
          "source?)");
    }

    // Get parameters
    auto output_path_it = parameters.find("output_path");
    if (output_path_it == parameters.end()) {
      throw std::runtime_error("output_path parameter is required");
    }
    if (!std::holds_alternative<std::string>(output_path_it->second)) {
      throw std::runtime_error("output_path parameter must be a string");
    }
    std::string output_path = std::get<std::string>(output_path_it->second);
    if (output_path.empty()) {
      throw std::runtime_error("output_path parameter is empty");
    }

    // Optional channel pair selection; defaults to channel pair 0. The
    // parameter is a channel-pair index string (the GUI drop-down); a legacy
    // integer value is also accepted for backwards compatibility.
    size_t pair = 0;
    const auto pair_it = parameters.find("channel_pair");
    if (pair_it != parameters.end()) {
      std::optional<int32_t> pair_value;
      if (const auto* s = std::get_if<std::string>(&pair_it->second)) {
        pair_value = parse_channel_pair_string(*s);
      } else if (const auto* i = std::get_if<int32_t>(&pair_it->second)) {
        pair_value = *i;
      }
      if (!pair_value) {
        throw std::runtime_error(
            "channel_pair parameter must be a channel-pair index");
      }
      if (*pair_value < 0 ||
          *pair_value >= static_cast<int32_t>(kMaxAudioChannelPairs)) {
        throw std::runtime_error(
            "channel_pair parameter must be between 0 and " +
            std::to_string(kMaxAudioChannelPairs - 1));
      }
      pair = static_cast<size_t>(*pair_value);
    }

    ORC_LOG_INFO("AudioSink: Writing audio to {}", output_path);

    std::shared_ptr<IAudioSinkStageDeps> deps = deps_override_;
    if (!deps) {
      auto deps_impl = std::make_shared<AudioSinkStageDeps>(
          orc::plugin::get_stage_services());
      deps_impl->init(progress_callback_, &is_processing_, &cancel_requested_);
      deps = deps_impl;
    }

    const auto write_result =
        deps->write_audio_wav(vfr.get(), output_path, pair);
    if (!write_result.success) {
      last_status_ = "Error: " + write_result.error_message;
      ORC_LOG_ERROR("AudioSink: {}", write_result.error_message);
      is_processing_.store(false);
      return false;
    }

    ORC_LOG_INFO("AudioSink: Successfully wrote {} frames to {}",
                 write_result.frames_written, output_path);
    last_status_ = "Success: " + std::to_string(write_result.frames_written) +
                   " samples written";
    is_processing_.store(false);
    return true;

  } catch (const std::exception& e) {
    last_status_ = std::string("Error: ") + e.what();
    ORC_LOG_ERROR("AudioSink: {}", e.what());
    is_processing_.store(false);
    return false;
  }
}

std::string AudioSinkStage::get_trigger_status() const { return last_status_; }

}  // namespace orc
