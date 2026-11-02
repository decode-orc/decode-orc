/*
 * File:        cc_sink_stage.cpp
 * Module:      orc-core
 * Purpose:     Closed Caption Sink Stage - exports one EIA-608 service, or
 *              every one in a pass
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "cc_sink_stage.h"

#include <orc/abi/orc_plugin_services.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/support/logging.h>

#include <memory>
#include <stdexcept>

#include "cc_sink_stage_deps.h"

namespace orc {

CCSinkStage::CCSinkStage() {
  set_configuration_status(orc::ConfigurationStatus::Red);
}

NodeTypeInfo CCSinkStage::get_node_type_info() const {
  return NodeTypeInfo{
      NodeType::SINK,
      "CCSink",
      "Closed Caption Sink",
      "Exports one EIA-608 caption or text service to SCC, SRT, "
      "plain text or HTML, or every service in one pass",
      1,
      1,  // One input
      0,
      0,  // No outputs (sink)
      VideoFormatCompatibility::ALL};
}

std::vector<ArtifactPtr> CCSinkStage::execute(
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

std::vector<ParameterDescriptor> CCSinkStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)project_format;
  (void)source_type;
  std::vector<ParameterDescriptor> descriptors;

  // output_path parameter
  {
    ParameterDescriptor desc;
    desc.name = "output_path";
    desc.display_name = "Output File";
    desc.description = "Path to output closed caption file";
    desc.type = ParameterType::FILE_PATH;
    desc.constraints.required = true;
    desc.constraints.default_value = std::string("");
    descriptors.push_back(desc);
  }

  // service parameter
  {
    ParameterDescriptor desc;
    desc.name = "service";
    desc.display_name = "Caption Service";
    desc.description =
        "Which of the services multiplexed onto line 21 to export. CC1 is the "
        "primary caption service; CC2 is a second caption service (often a "
        "translation); TEXT1 and TEXT2 are text services carrying pages of "
        "information rather than captions. CC3, CC4, TEXT3 and TEXT4 are "
        "their counterparts on the second field, and XDS is the second "
        "field's Extended Data Service (station, programme, time of day), "
        "exported as plain text or HTML. Exporting without choosing one "
        "interleaves them all. All exports every service in one pass into "
        "the folder the output path names: CC1-CC4 as SubRip, TEXT1-TEXT4 "
        "and XDS as plain text, a file for each service that carried "
        "anything (the format does not apply).";
    desc.type = ParameterType::STRING;
    desc.constraints.required = true;
    desc.constraints.allowed_strings = {"CC1",   "CC2",   "CC3",   "CC4",
                                        "TEXT1", "TEXT2", "TEXT3", "TEXT4",
                                        "XDS",   "All"};
    desc.constraints.default_value = std::string("CC1");
    descriptors.push_back(desc);
  }

  // format parameter
  {
    ParameterDescriptor desc;
    desc.name = "format";
    desc.display_name = "Export Format";
    desc.description =
        "Output format: Scenarist SCC V1.0 (raw byte pairs with timecodes), "
        "SubRip subtitles, plain text, or a monospaced HTML transcript";
    desc.type = ParameterType::STRING;
    desc.constraints.required = true;
    desc.constraints.allowed_strings = {"Scenarist SCC", "SubRip SRT",
                                        "Plain Text", "HTML"};
    desc.constraints.default_value = std::string("Scenarist SCC");
    descriptors.push_back(desc);
  }

  // write_report parameter
  {
    ParameterDescriptor desc;
    desc.name = "write_report";
    desc.display_name = "Write Report File";
    desc.description =
        "Write a report of everything line 21 carried beside the output, "
        "named after it (captions.scc gives captions.scc.report.txt): the "
        "byte pairs each field yielded, the characters of every service, the "
        "Extended Data Service's station, programme and time of day, and the "
        "ATVEF triggers on TEXT2. The same report is always written to the "
        "log";
    desc.type = ParameterType::BOOL;
    desc.constraints.default_value = false;
    descriptors.push_back(desc);
  }

  return descriptors;
}

std::map<std::string, ParameterValue> CCSinkStage::get_parameters() const {
  return parameters_;
}

bool CCSinkStage::set_parameters(
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

CCExportOptions CCSinkStage::parse_config(
    const std::map<std::string, ParameterValue>& parameters) const {
  CCExportOptions cfg;

  auto output_path_it = parameters.find("output_path");
  if (output_path_it == parameters.end() ||
      !std::holds_alternative<std::string>(output_path_it->second)) {
    throw std::runtime_error("output_path parameter is required");
  }
  cfg.output_path = std::get<std::string>(output_path_it->second);

  auto format_it = parameters.find("format");
  if (format_it != parameters.end() &&
      std::holds_alternative<std::string>(format_it->second)) {
    const std::string format_str = std::get<std::string>(format_it->second);
    if (format_str == "Plain Text") {
      cfg.export_format = CCExportFormat::PLAIN_TEXT;
    } else if (format_str == "SubRip SRT") {
      cfg.export_format = CCExportFormat::SRT;
    } else if (format_str == "HTML") {
      cfg.export_format = CCExportFormat::HTML;
    }
  }

  auto service_it = parameters.find("service");
  if (service_it != parameters.end() &&
      std::holds_alternative<std::string>(service_it->second)) {
    const std::string service_str = std::get<std::string>(service_it->second);
    if (service_str == "All") {
      // Every service in one pass, each in the format the report tool reads:
      // the captions as SubRip, the text services and XDS as plain text.
      cfg.all_services = true;
    } else if (service_str == "XDS") {
      // The Extended Data Service is packets of values, not a caption
      // stream: there is nothing to time as subtitles or to write as SCC
      // caption pairs.
      if (cfg.export_format != CCExportFormat::PLAIN_TEXT &&
          cfg.export_format != CCExportFormat::HTML) {
        throw std::runtime_error(
            "XDS is exported as Plain Text or HTML, not as captions");
      }
      cfg.export_xds = true;
    } else {
      const auto parsed = eia608_service_from_name(service_str);
      if (!parsed.has_value()) {
        throw std::runtime_error("Unknown caption service: " + service_str);
      }
      cfg.service = *parsed;
    }
  }

  const auto report_it = parameters.find("write_report");
  if (report_it != parameters.end() &&
      std::holds_alternative<bool>(report_it->second)) {
    cfg.write_report = std::get<bool>(report_it->second);
  }

  return cfg;
}

bool CCSinkStage::trigger(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    IObservationContext& observation_context) {
  is_processing_.store(true);
  cancel_requested_.store(false);

  try {
    // Validate inputs
    if (inputs.empty()) {
      throw std::runtime_error(
          "CC sink requires one input (VideoFrameRepresentation)");
    }

    auto vfr = std::dynamic_pointer_cast<VideoFrameRepresentation>(inputs[0]);
    if (!vfr) {
      throw std::runtime_error("Input must be a VideoFrameRepresentation");
    }

    const CCExportOptions options = parse_config(parameters);

    std::shared_ptr<ICCSinkStageDeps> deps = deps_override_;
    if (!deps) {
      deps = std::make_shared<CCSinkStageDeps>(
          orc::plugin::get_observation_service());
    }

    deps->init(progress_callback_, &cancel_requested_);

    const CCExportResult export_result =
        deps->export_cc(vfr.get(), observation_context, options);

    is_processing_.store(false);

    if (!export_result.success) {
      ORC_LOG_ERROR("CC sink error: {}",
                    export_result.message.empty()
                        ? "Failed to export closed captions"
                        : export_result.message);
      return false;
    }

    ORC_LOG_INFO("Closed caption export completed successfully: {} CC frames",
                 export_result.cc_frames_exported);
    return true;

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("CC sink error: {}", e.what());
    is_processing_.store(false);
    return false;
  }
}

std::string CCSinkStage::get_trigger_status() const {
  if (is_processing_.load()) {
    return "Exporting closed captions...";
  }
  return "Idle";
}

}  // namespace orc
