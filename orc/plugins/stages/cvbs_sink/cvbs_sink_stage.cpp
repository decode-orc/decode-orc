/*
 * File:        cvbs_sink_stage.cpp
 * Module:      orc-core
 * Purpose:     CVBS Sink Stage - writes CVBS file-format family output
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "cvbs_sink_stage.h"

#include <orc/stage/common_types.h>
#include <orc/support/logging.h>

#include <stdexcept>

#include "cvbs_sink_encode.h"
#include "cvbs_sink_stage_deps.h"
#include "cvbs_sink_stage_deps_interface.h"

namespace orc {

namespace {

// Output sample encodings offered by the sample_encoding parameter, in the
// order presented to the user. CVBS_U10_4FSC is the normative default.
constexpr const char* kOutputEncodings[] = {"CVBS_U10_4FSC", "CVBS_U16_4FSC",
                                            "CVBS_TPG21_4FSC", "CVBS_S16_4FSC"};

constexpr const char* kSignalTypeComposite = "composite";
constexpr const char* kSignalTypeYC = "yc";

std::string get_string_param(
    const std::map<std::string, ParameterValue>& parameters,
    const std::string& name, const std::string& fallback) {
  const auto it = parameters.find(name);
  if (it == parameters.end()) return fallback;
  const auto* s = std::get_if<std::string>(&it->second);
  return (s && !s->empty()) ? *s : fallback;
}

}  // namespace

CVBSSinkStage::CVBSSinkStage() {
  set_configuration_status(orc::ConfigurationStatus::Red);
}

NodeTypeInfo CVBSSinkStage::get_node_type_info() const {
  return NodeTypeInfo{
      NodeType::SINK,
      "CVBSSink",
      "CVBS Sink",
      "Writes frames to the CVBS file format (.cvbs or .cvbsy/.cvbsc payload, "
      ".meta sidecar, and dropout/audio/EFM/AC3 sidecars when present)",
      1,
      1,
      0,
      0,
      VideoFormatCompatibility::ALL};
}

std::vector<ArtifactPtr> CVBSSinkStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  (void)inputs;
  (void)parameters;
  (void)observation_context;
  return {};
}

std::vector<ParameterDescriptor> CVBSSinkStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)project_format;
  std::vector<ParameterDescriptor> descriptors;

  // The output signal type is not a choice: it follows the project type.
  // A composite project writes <base>.cvbs; a Y/C project writes
  // <base>.cvbsy + <base>.cvbsc (CVBS file format spec, File Naming
  // Convention).
  const bool yc_project = (source_type == SourceType::YC);

  {
    ParameterDescriptor desc;
    desc.name = "output_path";
    desc.display_name = "Output CVBS File";
    desc.description =
        std::string(
            "Base path for the output files. The stage appends the payload "
            "extension and .meta automatically based on the project type (") +
        (yc_project ? ".cvbsy/.cvbsc for this Y/C project"
                    : ".cvbs for this composite project") +
        "); a trailing .cvbs/.cvbsy/.cvbsc extension is stripped when "
        "present. \"-\" writes the primary payload to the CLI process's "
        "standard output instead of a file — runs only via the CLI; "
        "settable here for a project you'll execute there. Composite only, "
        "and no .meta/dropout/audio/EFM/AC3 "
        "sidecars, since a pipe can only carry one stream.";
    desc.type = ParameterType::FILE_PATH;
    desc.constraints.required = true;
    desc.constraints.default_value = std::string("");
    desc.file_extension_hint = yc_project ? ".cvbsy" : ".cvbs";
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "sample_encoding";
    desc.display_name = "Sample Encoding";
    desc.description =
        "Sample encoding of the output CVBS data, recorded as "
        "sample_encoding_preset in the .meta sidecar. CVBS_U10_4FSC (the "
        "default) preserves the internal 10-bit domain losslessly, including "
        "headroom; the other encodings clamp to their representable domain "
        "before scaling.";
    desc.type = ParameterType::STRING;
    desc.constraints.required = false;
    desc.constraints.default_value = std::string(kOutputEncodings[0]);
    for (const char* encoding : kOutputEncodings) {
      desc.constraints.allowed_strings.push_back(encoding);
    }
    descriptors.push_back(desc);
  }

  {
    ParameterDescriptor desc;
    desc.name = "capture_notes";
    desc.display_name = "Capture Notes";
    desc.description =
        "Optional free-text notes written to the capture_notes field of the "
        ".meta sidecar. Left out of the metadata when empty.";
    desc.type = ParameterType::STRING;
    desc.constraints.required = false;
    desc.constraints.default_value = std::string("");
    descriptors.push_back(desc);
  }

  return descriptors;
}

std::map<std::string, ParameterValue> CVBSSinkStage::get_parameters() const {
  return parameters_;
}

bool CVBSSinkStage::set_parameters(
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

bool CVBSSinkStage::trigger(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    IObservationContext& observation_context) {
  (void)observation_context;
  is_processing_.store(true);
  cancel_requested_.store(false);

  try {
    if (inputs.empty()) {
      throw std::runtime_error(
          "CVBS sink requires one input (VideoFrameRepresentation)");
    }

    auto vfr = std::dynamic_pointer_cast<VideoFrameRepresentation>(inputs[0]);
    if (!vfr) {
      throw std::runtime_error("Input must be a VideoFrameRepresentation");
    }

    auto output_path_it = parameters.find("output_path");
    if (output_path_it == parameters.end()) {
      throw std::runtime_error("output_path parameter is required");
    }

    CVBSSinkWriteConfig config;
    config.output_base_path = std::get<std::string>(output_path_it->second);

    const std::string encoding_name =
        get_string_param(parameters, "sample_encoding", kOutputEncodings[0]);
    const auto encoding = parse_cvbs_sample_encoding(encoding_name);
    if (!encoding.has_value()) {
      throw std::runtime_error("Unsupported sample encoding '" + encoding_name +
                               "'");
    }
    config.sample_encoding = *encoding;

    // The signal type is not user-selectable: it follows the input. A Y/C
    // project produces representations with separate channels and is written
    // as .cvbsy/.cvbsc; a composite project is written as .cvbs.
    config.signal_type =
        vfr->has_separate_channels() ? kSignalTypeYC : kSignalTypeComposite;

    config.capture_notes = get_string_param(parameters, "capture_notes", "");

    ORC_LOG_INFO("CVBSSink: Writing CVBS data to {} ({}, {})",
                 config.output_base_path, config.signal_type, encoding_name);

    std::shared_ptr<ICVBSSinkStageDeps> deps = deps_override_;
    if (!deps) {
      auto deps_impl = std::make_shared<CVBSSinkStageDeps>();
      deps_impl->init(progress_callback_, &cancel_requested_);
      deps = deps_impl;
    }

    const auto result = deps->write_cvbs(vfr.get(), config);
    last_status_ = result.status_message;

    if (!result.success) {
      ORC_LOG_ERROR("CVBSSink: {}", result.status_message);
      is_processing_.store(false);
      return false;
    }

    ORC_LOG_INFO("CVBSSink: {}", result.status_message);
    is_processing_.store(false);
    return true;

  } catch (const std::exception& e) {
    last_status_ = std::string("Error: ") + e.what();
    ORC_LOG_ERROR("CVBSSink: {}", e.what());
    is_processing_.store(false);
    return false;
  }
}

std::string CVBSSinkStage::get_trigger_status() const { return last_status_; }

}  // namespace orc
