/*
 * File:        efm_sink_stage.cpp
 * Module:      orc-core
 * Purpose:     Raw EFM Data Sink Stage - writes EFM t-values to raw file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "efm_sink_stage.h"

#include <orc/abi/orc_plugin_services.h>
#include <orc/stage/common_types.h>
#include <orc/support/logging.h>

#include <stdexcept>

#include "efm_sink_stage_deps.h"
#include "efm_sink_stage_deps_interface.h"

namespace orc {

RawEFMSinkStage::RawEFMSinkStage() {
  set_configuration_status(orc::ConfigurationStatus::Red);
}

NodeTypeInfo RawEFMSinkStage::get_node_type_info() const {
  return NodeTypeInfo{
      NodeType::SINK,
      "RawEFMSink",
      "Raw EFM Data Sink",
      "Extracts raw EFM t-values from the VFR and writes them to a binary file",
      1,
      1,  // One input
      0,
      0,  // No outputs (sink)
      VideoFormatCompatibility::ALL};
}

std::vector<ArtifactPtr> RawEFMSinkStage::execute(
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

std::vector<ParameterDescriptor> RawEFMSinkStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)project_format;
  (void)source_type;
  std::vector<ParameterDescriptor> descriptors;

  // output_path parameter
  {
    ParameterDescriptor desc;
    desc.name = "output_path";
    desc.display_name = "Output EFM File";
    desc.description = "Path to output EFM data file (raw t-values)";
    desc.type = ParameterType::FILE_PATH;
    desc.constraints.required = true;
    desc.constraints.default_value = std::string("");
    desc.file_extension_hint = ".efm";
    descriptors.push_back(desc);
  }

  // include_confidence parameter
  {
    ParameterDescriptor desc;
    desc.name = "include_confidence";
    desc.display_name = "Include Confidence Nibble";
    desc.description =
        "Write each t-value as the packed byte the pipeline carries: the "
        "t-value in the low nibble and the producer's confidence (as a doubt "
        "value) in the high nibble. Disable to write bare t-values instead, "
        "for tools that pre-date the confidence field. A capture with nothing "
        "doubted produces the same bytes either way.";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = true;
    descriptors.push_back(desc);
  }

  return descriptors;
}

std::map<std::string, ParameterValue> RawEFMSinkStage::get_parameters() const {
  return parameters_;
}

bool RawEFMSinkStage::set_parameters(
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

bool RawEFMSinkStage::trigger(
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
          "RawEFM sink requires one input (VideoFrameRepresentation)");
    }

    auto vfr = std::dynamic_pointer_cast<VideoFrameRepresentation>(inputs[0]);
    if (!vfr) {
      throw std::runtime_error("Input must be a VideoFrameRepresentation");
    }

    // Check if VFrameR has EFM
    if (!vfr->has_efm()) {
      throw std::runtime_error(
          "Input VFrameR does not have EFM data (no EFM file specified in "
          "source?)");
    }

    // Get parameters
    auto output_path_it = parameters.find("output_path");
    if (output_path_it == parameters.end()) {
      throw std::runtime_error("output_path parameter is required");
    }
    std::string output_path = std::get<std::string>(output_path_it->second);

    // Defaults to the lossless export: keep the byte exactly as the pipeline
    // carries it.
    bool include_confidence = true;
    const auto include_confidence_it = parameters.find("include_confidence");
    if (include_confidence_it != parameters.end() &&
        std::holds_alternative<bool>(include_confidence_it->second)) {
      include_confidence = std::get<bool>(include_confidence_it->second);
    }

    ORC_LOG_INFO("RawEFMSink: Writing EFM data to {} ({} confidence nibble)",
                 output_path, include_confidence ? "with" : "without");

    std::shared_ptr<IRawEFMSinkStageDeps> deps = deps_override_;
    if (!deps) {
      auto deps_impl = std::make_shared<RawEFMSinkStageDeps>(
          orc::plugin::get_stage_services());
      deps_impl->init(progress_callback_, &cancel_requested_);
      deps = deps_impl;
    }

    const auto write_result =
        deps->write_raw_efm(vfr.get(), output_path, include_confidence);
    if (!write_result.success) {
      last_status_ = write_result.status_message;
      ORC_LOG_ERROR("RawEFMSink: {}", write_result.status_message);
      is_processing_.store(false);
      return false;
    }

    last_status_ = write_result.status_message;
    is_processing_.store(false);
    return true;

  } catch (const std::exception& e) {
    last_status_ = std::string("Error: ") + e.what();
    ORC_LOG_ERROR("RawEFMSink: {}", e.what());
    is_processing_.store(false);
    return false;
  }
}

std::string RawEFMSinkStage::get_trigger_status() const { return last_status_; }

}  // namespace orc
