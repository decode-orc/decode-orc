/*
 * File:        tbc_sink_stage.cpp
 * Module:      orc-core
 * Purpose:     TBC Sink Stage implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "tbc_sink_stage.h"

#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <memory>
#include <optional>
#include <string>
#include <variant>

#include "tbc_metadata_writer.h"
#include "tbc_sink_stage_deps.h"
#include "tbc_sink_stage_deps_interface.h"

namespace orc {

namespace {

// Reads the audio_channel_pair parameter, which the GUI presents as a string
// of allowed indices. Returns nullopt when the value is not a plain integer
// naming a channel pair the model permits.
std::optional<size_t> parse_audio_channel_pair(const ParameterValue& value) {
  if (!std::holds_alternative<std::string>(value)) return std::nullopt;
  const std::string& text = std::get<std::string>(value);
  try {
    size_t consumed = 0;
    const int parsed = std::stoi(text, &consumed);
    if (consumed != text.size() || parsed < 0 ||
        static_cast<size_t>(parsed) >= kMaxAudioChannelPairs) {
      return std::nullopt;
    }
    return static_cast<size_t>(parsed);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

}  // namespace

TBCSinkStage::TBCSinkStage(IStageServices* stage_services)
    : stage_services_(stage_services) {
  set_configuration_status(orc::ConfigurationStatus::Red);
}

NodeTypeInfo TBCSinkStage::get_node_type_info() const {
  return NodeTypeInfo{NodeType::SINK,  // type
                      "tbc_sink",      // stage_name
                      "TBC Sink",      // display_name
                      "Writes TBC fields and metadata to disk. Trigger to "
                      "export all fields.",  // description
                      1,                     // min_inputs
                      1,                     // max_inputs
                      0,                     // min_outputs
                      0,                     // max_outputs
                      VideoFormatCompatibility::ALL};
}

std::vector<ArtifactPtr> TBCSinkStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters [[maybe_unused]],
    ObservationContext& observation_context) {
  // Observations are not consumed here yet; wiring is tracked in the
  // Observation Service Implementation Plan (docs-tech/
  // plugin-observation-service-plan.md) as a pending consumer.
  (void)observation_context;
  // Cache input for preview rendering
  if (!inputs.empty()) {
    cached_input_ =
        std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  }

  // Sink stages don't produce outputs during normal execution
  // They are triggered manually to write data
  ORC_LOG_DEBUG("TBCSink execute called (cached input for preview)");
  return {};  // No outputs
}

std::vector<ParameterDescriptor> TBCSinkStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)project_format;
  (void)source_type;  // Unused - the TBC sink works with all formats

  std::vector<ParameterDescriptor> descriptors;
  descriptors.push_back(ParameterDescriptor{
      "output_path", "TBC Output Path",
      "Path to output TBC file (metadata will be written to .db). \"-\" "
      "writes the .tbc payload to the CLI process's standard output instead "
      "of a file — runs only via the CLI; settable here for a project "
      "you'll execute there. No .db metadata "
      "and no .pcm/.efm sidecars, since a pipe can only carry one stream.",
      ParameterType::FILE_PATH,
      ParameterConstraints{
          std::nullopt, std::nullopt, std::string(""), {}, false, std::nullopt},
      ".tbc"  // file_extension_hint
  });

  // The ld-decode layout has room for exactly one analogue audio sidecar, so
  // a pipeline carrying several channel pairs has to nominate one. The lowest
  // pair is the default because that is where a TBC or CVBS source puts the
  // analogue audio it read.
  {
    ParameterDescriptor desc;
    desc.name = "audio_channel_pair";
    desc.display_name = "Audio Channel Pair";
    desc.description =
        "Audio channel pair written to the .pcm sidecar (0-based, matching "
        "the CVBS container channel pair numbering). Defaults to the lowest "
        "pair, which is the analogue audio on a TBC or CVBS source. Ignored "
        "when the input carries no audio; a pair the input does not have "
        "falls back to the lowest one. The GUI lists only the channel pairs "
        "the input actually carries, labelled with their names (for example "
        "\"0: Analogue\").";
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

std::map<std::string, ParameterValue> TBCSinkStage::get_parameters() const {
  std::map<std::string, ParameterValue> params;
  params["output_path"] = output_path_;
  params["audio_channel_pair"] = std::to_string(audio_channel_pair_);
  return params;
}

bool TBCSinkStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  auto it = params.find("output_path");
  if (it != params.end()) {
    if (std::holds_alternative<std::string>(it->second)) {
      output_path_ = std::get<std::string>(it->second);
      ORC_LOG_DEBUG("TBCSink: output_path set to '{}'", output_path_);
    } else {
      ORC_LOG_ERROR("TBCSink: output_path parameter must be string");
      return false;
    }
  }

  auto pair_it = params.find("audio_channel_pair");
  if (pair_it != params.end()) {
    const auto pair = parse_audio_channel_pair(pair_it->second);
    if (!pair) {
      ORC_LOG_ERROR(
          "TBCSink: audio_channel_pair must be a channel pair index in 0..{}",
          kMaxAudioChannelPairs - 1);
      return false;
    }
    audio_channel_pair_ = *pair;
    ORC_LOG_DEBUG("TBCSink: audio_channel_pair set to {}", audio_channel_pair_);
  }

  set_configuration_status(output_path_.empty()
                               ? orc::ConfigurationStatus::Red
                               : orc::ConfigurationStatus::Green);
  return true;
}

bool TBCSinkStage::trigger(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    IObservationContext& observation_context) {
  ORC_LOG_DEBUG("TBCSink: Trigger started");
  trigger_status_ = "Starting export...";
  is_processing_.store(true);
  cancel_requested_.store(false);

  const auto fail_trigger = [this](const std::string& status) {
    trigger_status_ = status;
    is_processing_.store(false);
    return false;
  };

  // Validate parameters
  auto it = parameters.find("output_path");
  if (it == parameters.end() ||
      !std::holds_alternative<std::string>(it->second)) {
    ORC_LOG_ERROR("TBCSink: No output_path parameter");
    return fail_trigger("Error: No output path specified");
  }

  std::string output_path = std::get<std::string>(it->second);
  if (output_path.empty()) {
    ORC_LOG_ERROR("TBCSink: output_path is empty");
    return fail_trigger("Error: Output path is empty");
  }

  // Validate inputs
  if (inputs.empty()) {
    ORC_LOG_ERROR("TBCSink: No input provided");
    return fail_trigger("Error: No input connected");
  }

  // Get input representation
  auto representation =
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  if (!representation) {
    ORC_LOG_ERROR("TBCSink: Input is not VideoFrameRepresentation");
    return fail_trigger("Error: Input is not a video frame representation");
  }

  // Write TBC and metadata
  ORC_LOG_INFO("TBCSink: Writing to '{}'", output_path);
  // Clear previous observations to avoid mixing runs
  observation_context.clear();

  // Use injected deps override (test seam) if set; otherwise build from SDK
  // services.
  std::shared_ptr<ITBCSinkStageDeps> deps = deps_override_;
  if (!deps) {
    auto metadata_writer = std::make_shared<TBCMetadataWriter>();
    auto deps_impl =
        std::make_shared<TBCSinkStageDeps>(stage_services_, metadata_writer);
    deps_impl->init(progress_callback_, &is_processing_, &cancel_requested_);
    deps = deps_impl;
  }
  // The trigger-time parameter map is authoritative; fall back to the value
  // set_parameters() cached when the map omits it.
  size_t audio_channel_pair = audio_channel_pair_;
  auto pair_it = parameters.find("audio_channel_pair");
  if (pair_it != parameters.end()) {
    const auto parsed = parse_audio_channel_pair(pair_it->second);
    if (!parsed) {
      ORC_LOG_ERROR("TBCSink: Invalid audio_channel_pair parameter");
      return fail_trigger("Error: Invalid audio channel pair");
    }
    audio_channel_pair = *parsed;
  }

  bool success =
      deps->write_tbc_and_metadata(representation.get(), output_path,
                                   audio_channel_pair, observation_context);

  if (success) {
    auto frame_rng = representation->frame_range();
    trigger_status_ = "Exported " + std::to_string(frame_rng.count() * 2) +
                      " fields to " + output_path;
    ORC_LOG_DEBUG("TBCSink: Trigger completed successfully");
  } else {
    trigger_status_ = "Error: Failed to write output files";
    ORC_LOG_ERROR("TBCSink: Trigger failed");
  }

  is_processing_.store(false);
  return success;
}

std::string TBCSinkStage::get_trigger_status() const { return trigger_status_; }

StagePreviewCapability TBCSinkStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_input_);
}

}  // namespace orc
