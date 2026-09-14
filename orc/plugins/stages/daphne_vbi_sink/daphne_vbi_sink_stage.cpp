/*
 * File:        daphne_vbi_sink_stage.cpp
 * Module:      orc-core
 * Purpose:     Generate .VBI binary files
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Matt Ownby
 */

#include "daphne_vbi_sink_stage.h"

#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <filesystem>
#include <memory>

#include "daphne_vbi_sink_stage_deps.h"
#include "daphne_vbi_writer_util.h"

namespace orc {

DaphneVBISinkStage::DaphneVBISinkStage(IStageServices* stage_services)
    : stage_services_(stage_services) {
  set_configuration_status(orc::ConfigurationStatus::Red);
}

NodeTypeInfo DaphneVBISinkStage::get_node_type_info() const {
  return NodeTypeInfo{NodeType::SINK,     // type
                      "daphne_vbi_sink",  // stage_name
                      "Daphne VBI Sink",  // display_name
                      "Generates Daphne VBI file ( "
                      "https://www.daphne-emu.com:9443/mediawiki/index.php/"
                      "VBIInfo )",  // description
                      1,            // min_inputs
                      1,            // max_inputs
                      0,            // min_outputs
                      0,            // max_outputs
                      VideoFormatCompatibility::ALL};
}

std::vector<ArtifactPtr> DaphneVBISinkStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters [[maybe_unused]],
    ObservationContext& observation_context) {
  (void)observation_context;

  cached_input_ = nullptr;
  if (!inputs.empty()) {
    cached_input_ =
        std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  }

  return {};  // No outputs
}

StagePreviewCapability DaphneVBISinkStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_input_);
}

std::vector<ParameterDescriptor> DaphneVBISinkStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)project_format;
  (void)source_type;
  return {ParameterDescriptor{
      "output_path", "VBI Output Path",
      "Path to output VBI file. \"-\" writes the stream to the CLI "
      "process's standard output instead of a file (CLI only; the GUI "
      "rejects this value).",
      ParameterType::FILE_PATH,
      ParameterConstraints{
          std::nullopt, std::nullopt, std::string(""), {}, false, std::nullopt},
      ".vbi"  // file_extension_hint
  }};
}

std::map<std::string, ParameterValue> DaphneVBISinkStage::get_parameters()
    const {
  std::map<std::string, ParameterValue> params;
  params["output_path"] = output_path_;
  return params;
}

bool DaphneVBISinkStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  auto it = params.find("output_path");
  if (it != params.end()) {
    if (std::holds_alternative<std::string>(it->second)) {
      output_path_ = std::get<std::string>(it->second);
      ORC_LOG_DEBUG("DaphneVBISink: output_path set to '{}'", output_path_);
    } else {
      ORC_LOG_ERROR("DaphneVBISink: output_path parameter must be string");
      return false;
    }
  }

  set_configuration_status(output_path_.empty()
                               ? orc::ConfigurationStatus::Red
                               : orc::ConfigurationStatus::Green);
  return true;
}

bool DaphneVBISinkStage::trigger(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    IObservationContext& observation_context) {
  ORC_LOG_DEBUG("DaphneVBISink: Trigger started");
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
    ORC_LOG_ERROR("DaphneVBISink: No output_path parameter");
    return fail_trigger("Error: No output path specified");
  }

  std::string output_path = std::get<std::string>(it->second);
  if (output_path.empty()) {
    ORC_LOG_ERROR("DaphneVBISink: output_path is empty");
    return fail_trigger("Error: Output path is empty");
  }

  // Validate inputs
  if (inputs.empty()) {
    ORC_LOG_ERROR("DaphneVBISink: No input provided");
    return fail_trigger("Error: No input connected");
  }

  // Get input representation
  auto representation =
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  if (!representation) {
    ORC_LOG_ERROR("DaphneVBISink: Input is not VideoFrameRepresentation");
    return fail_trigger("Error: Input is not a video frame representation");
  }

  // Write .VBI binary file
  ORC_LOG_INFO("DaphneVBISink: Writing to '{}'", output_path);
  // Clear previous observations to avoid mixing runs
  observation_context.clear();

  // Use injected deps override (test seam) if set; otherwise build from SDK
  // services.
  std::shared_ptr<IDaphneVBISinkStageDeps> deps = deps_override_;
  if (!deps) {
    auto deps_impl = std::make_shared<DaphneVBISinkStageDeps>(stage_services_);
    deps_impl->init(progress_callback_, &is_processing_, &cancel_requested_);
    deps = deps_impl;
  }
  bool success =
      deps->write_vbi(representation.get(), output_path, observation_context);

  if (success) {
    auto frame_rng = representation->frame_range();
    trigger_status_ = "Exported " + std::to_string(frame_rng.count() * 2) +
                      " fields to " + output_path;
    ORC_LOG_DEBUG("DaphneVBISink: Trigger completed successfully");
  } else {
    trigger_status_ = "Error: Failed to write output files";
    ORC_LOG_ERROR("DaphneVBISink: Trigger failed");
  }

  is_processing_.store(false);
  return success;
}

std::string DaphneVBISinkStage::get_trigger_status() const {
  return trigger_status_;
}

}  // namespace orc