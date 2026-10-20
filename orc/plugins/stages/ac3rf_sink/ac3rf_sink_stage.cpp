/*
 * File:        ac3rf_sink_stage.cpp
 * Module:      orc-core
 * Purpose:     AC3 RF Sink Stage - decodes AC3 RF samples and writes AC3 frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "ac3rf_sink_stage.h"

#include <orc/stage/common_types.h>
#include <orc/support/logging.h>

#include <stdexcept>

#include "ac3rf_sink_stage_deps.h"
#include "ac3rf_sink_stage_deps_interface.h"

namespace orc {

AC3RFSinkStage::AC3RFSinkStage() {
  set_configuration_status(orc::ConfigurationStatus::Red);
}

NodeTypeInfo AC3RFSinkStage::get_node_type_info() const {
  return NodeTypeInfo{NodeType::SINK,
                      "AC3RFSink",
                      "AC3 RF Sink",
                      "Decodes AC3 RF samples and writes AC3 frames to file",
                      1,
                      1,  // One input
                      0,
                      0,  // No outputs (sink)
                      VideoFormatCompatibility::ALL};
}

std::vector<ArtifactPtr> AC3RFSinkStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  // Sink stages produce no outputs in execute(); work happens in trigger()
  (void)inputs;
  (void)parameters;
  (void)observation_context;
  return {};
}

std::vector<ParameterDescriptor> AC3RFSinkStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)project_format;
  (void)source_type;
  std::vector<ParameterDescriptor> descriptors;

  {
    ParameterDescriptor desc;
    desc.name = "output_path";
    desc.display_name = "Output AC3 File";
    desc.description =
        "Path to the output AC3 file. \"-\" writes the stream to the CLI "
        "process's standard output instead of a file — runs only via the "
        "CLI; settable here for a project you'll execute there.";
    desc.type = ParameterType::FILE_PATH;
    desc.constraints.required = true;
    desc.constraints.default_value = std::string("");
    desc.file_extension_hint = ".ac3";
    descriptors.push_back(desc);
  }

  return descriptors;
}

std::map<std::string, ParameterValue> AC3RFSinkStage::get_parameters() const {
  return parameters_;
}

bool AC3RFSinkStage::set_parameters(
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

bool AC3RFSinkStage::trigger(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    IObservationContext& observation_context) {
  (void)observation_context;
  is_processing_.store(true);
  cancel_requested_.store(false);

  try {
    // Validate input
    if (inputs.empty()) {
      throw std::runtime_error(
          "AC3 RF sink requires one input (VideoFrameRepresentation)");
    }

    auto vfr = std::dynamic_pointer_cast<VideoFrameRepresentation>(inputs[0]);
    if (!vfr) {
      throw std::runtime_error("Input must be a VideoFrameRepresentation");
    }

    if (!vfr->has_ac3_rf()) {
      throw std::runtime_error(
          "Input VFrameR does not have AC3 RF symbols data "
          "(no AC3 RF symbols file specified in the source stage?)");
    }

    // Get output path
    auto path_it = parameters.find("output_path");
    if (path_it == parameters.end()) {
      throw std::runtime_error("output_path parameter is required");
    }
    const std::string output_path = std::get<std::string>(path_it->second);

    ORC_LOG_INFO("AC3RFSink: Decoding AC3 RF symbols, writing to {}",
                 output_path);

    std::shared_ptr<IAC3RFSinkStageDeps> deps = deps_override_;
    if (!deps) {
      auto deps_impl = std::make_shared<AC3RFSinkStageDeps>();
      deps_impl->init(progress_callback_, &cancel_requested_);
      deps = deps_impl;
    }

    const auto decode_result =
        deps->decode_and_write_ac3(vfr.get(), output_path);
    if (!decode_result.success) {
      last_status_ = decode_result.status_message;
      ORC_LOG_ERROR("AC3RFSink: {}", decode_result.status_message);
      is_processing_.store(false);
      return false;
    }

    ORC_LOG_INFO("AC3RFSink: Wrote {} AC3 frames to {}",
                 decode_result.frames_written, output_path);
    last_status_ = decode_result.status_message;
    is_processing_.store(false);
    return true;

  } catch (const std::exception& e) {
    last_status_ = std::string("Error: ") + e.what();
    ORC_LOG_ERROR("AC3RFSink: {}", e.what());
    is_processing_.store(false);
    return false;
  }
}

std::string AC3RFSinkStage::get_trigger_status() const { return last_status_; }

}  // namespace orc
