/*
 * File:        burst_level_analysis_sink_stage.cpp
 * Module:      orc-core
 * Purpose:     Burst Level Analysis Sink Stage implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "burst_level_analysis_sink_stage.h"

#include <orc/abi/orc_plugin_services.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "../common/analysis-results/analysis_report_statistics.h"
#include "../common/analysis-results/analysis_series_wire.h"
#include "burst_level_analysis_sink_deps.h"

namespace orc {

BurstLevelAnalysisSinkStage::BurstLevelAnalysisSinkStage() {
  set_configuration_status(orc::ConfigurationStatus::Yellow);
}

NodeTypeInfo BurstLevelAnalysisSinkStage::get_node_type_info() const {
  return NodeTypeInfo{NodeType::ANALYSIS_SINK,
                      "burst_level_analysis_sink",
                      "Burst Level Analysis Sink",
                      "Computes burst level statistics and optionally writes "
                      "CSV. Trigger to update dataset.",
                      1,
                      1,
                      0,
                      0,
                      VideoFormatCompatibility::ALL};
}

std::vector<ArtifactPtr> BurstLevelAnalysisSinkStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& observation_context) {
  (void)parameters;
  (void)observation_context;
  cached_input_ = nullptr;
  if (!inputs.empty()) {
    cached_input_ =
        std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  }
  return {};
}

StagePreviewCapability BurstLevelAnalysisSinkStage::get_preview_capability()
    const {
  return PreviewHelpers::make_signal_preview_capability(cached_input_);
}

std::vector<ParameterDescriptor>
BurstLevelAnalysisSinkStage::get_parameter_descriptors(
    VideoSystem project_format, SourceType source_type) const {
  (void)project_format;
  (void)source_type;

  std::vector<ParameterDescriptor> descriptors;

  descriptors.push_back(ParameterDescriptor{
      "output_path", "CSV Output Path",
      "Destination CSV file for burst metrics. Leave empty to skip file "
      "output.",
      ParameterType::FILE_PATH,
      ParameterConstraints{
          std::nullopt, std::nullopt, std::string(""), {}, false, std::nullopt},
      ".csv"});

  descriptors.push_back(ParameterDescriptor{
      "write_csv", "Write CSV",
      "Enable writing results to CSV at trigger time.", ParameterType::BOOL,
      ParameterConstraints{std::nullopt,
                           std::nullopt,
                           ParameterValue(false),
                           {},
                           false,
                           std::nullopt}});

  return descriptors;
}

std::map<std::string, ParameterValue>
BurstLevelAnalysisSinkStage::get_parameters() const {
  return parameters_;
}

bool BurstLevelAnalysisSinkStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  parameters_ = params;

  const auto it = params.find("output_path");
  const bool has_path =
      (it != params.end() && std::holds_alternative<std::string>(it->second) &&
       !std::get<std::string>(it->second).empty());

  set_configuration_status(has_path ? orc::ConfigurationStatus::Green
                                    : orc::ConfigurationStatus::Yellow);
  return true;
}

BurstLevelAnalysisSinkStage::ParsedConfig
BurstLevelAnalysisSinkStage::parse_config(
    const std::map<std::string, ParameterValue>& parameters) const {
  ParsedConfig cfg;

  auto out_it = parameters.find("output_path");
  if (out_it != parameters.end() &&
      std::holds_alternative<std::string>(out_it->second)) {
    cfg.output_path = std::get<std::string>(out_it->second);
  }

  auto csv_it = parameters.find("write_csv");
  if (csv_it != parameters.end() &&
      std::holds_alternative<bool>(csv_it->second)) {
    cfg.write_csv = std::get<bool>(csv_it->second);
  }

  return cfg;
}

bool BurstLevelAnalysisSinkStage::trigger(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    IObservationContext& observation_context) {
  ORC_LOG_DEBUG("BurstLevelAnalysisSink: Trigger started");
  is_processing_.store(true);
  cancel_requested_.store(false);
  has_results_ = false;
  frame_stats_.clear();
  total_frames_ = 0;

  try {
    if (inputs.empty()) {
      throw std::runtime_error("No input connected");
    }

    auto vfr = std::dynamic_pointer_cast<VideoFrameRepresentation>(inputs[0]);
    if (!vfr) {
      throw std::runtime_error("Input is not a VideoFrameRepresentation");
    }

    ParsedConfig cfg = parse_config(parameters);
    std::shared_ptr<IBurstLevelAnalysisSinkStageDeps> deps = deps_override_;
    if (!deps) {
      deps = std::make_shared<BurstLevelAnalysisSinkStageDeps>(
          orc::plugin::get_observation_service());
    }

    deps->init(progress_callback_, &cancel_requested_);

    BurstAnalysisComputeOptions compute_options;
    compute_options.output_path = cfg.output_path;
    compute_options.write_csv = cfg.write_csv;

    const BurstAnalysisComputeResult compute_result = deps->compute_and_analyze(
        vfr.get(), observation_context, compute_options);

    if (!compute_result.success) {
      has_results_ = false;
      frame_stats_.clear();
      total_frames_ = 0;
      last_status_ = compute_result.message.empty()
                         ? "Error: Burst level analysis failed"
                         : (compute_result.message == "Cancelled by user"
                                ? compute_result.message
                                : "Error: " + compute_result.message);
      is_processing_.store(false);
      return false;
    }

    frame_stats_ = compute_result.frame_stats;
    total_frames_ = compute_result.total_frames;
    video_system_ = VideoSystem::Unknown;
    blanking_level_ = -1;
    white_level_ = -1;
    if (const auto params = vfr->get_video_parameters()) {
      video_system_ = params->system;
      blanking_level_ = params->blanking_level;
      white_level_ = params->white_level;
    }

    if (cfg.write_csv && !cfg.output_path.empty()) {
      if (!deps->write_csv(cfg.output_path, frame_stats_)) {
        ORC_LOG_WARN("BurstLevelAnalysisSink: Failed to write CSV to {}",
                     cfg.output_path);
      }
    }

    last_status_ = "Burst level analysis complete";
    has_results_ = true;
    is_processing_.store(false);
    return true;
  } catch (const std::exception& e) {
    last_status_ = std::string("Error: ") + e.what();
    ORC_LOG_ERROR("BurstLevelAnalysisSink: Trigger failed: {}", e.what());
    is_processing_.store(false);
    return false;
  }
}

namespace {

analysis_wire::System wire_system(VideoSystem system) {
  switch (system) {
    case VideoSystem::PAL:
      return analysis_wire::System::PAL;
    case VideoSystem::NTSC:
      return analysis_wire::System::NTSC;
    case VideoSystem::PAL_M:
      return analysis_wire::System::PAL_M;
    default:
      return analysis_wire::System::Unknown;
  }
}

}  // namespace

StageToolReply BurstLevelAnalysisSinkStage::handle_tool_request(
    const std::string& contract_id, const std::string& verb,
    const std::vector<uint8_t>& payload) {
  (void)payload;
  StageToolReply reply;
  if (contract_id != analysis_wire::kBurstLevelContractId ||
      verb != analysis_wire::kResultsVerb) {
    reply.error = "The burst level analysis sink does not answer '" + verb +
                  "' for " + contract_id;
    return reply;
  }
  if (!has_results_) {
    reply.error = analysis_wire::kNotTriggeredMessage;
    return reply;
  }
  analysis_wire::BurstLevelResults results;
  results.total_frames = total_frames_;
  results.levels = {wire_system(video_system_), blanking_level_, white_level_};
  results.frames.reserve(frame_stats_.size());
  for (const auto& f : frame_stats_) {
    results.frames.push_back(
        {f.frame_number, f.median_burst_10bit, f.has_data});
  }
  reply.data = analysis_wire::encode(results);
  reply.ok = true;
  return reply;
}

bool BurstLevelAnalysisSinkStage::results_report(
    StageResultsReport* report) const {
  if (!has_results_ || !report) {
    return false;
  }
  *report = StageResultsReport{};
  report->kind = "burst_level";
  report->total_frames = total_frames_;
  std::vector<double> burst;
  report->columns = {"frame", "median_burst_10bit"};
  report->rows.reserve(frame_stats_.size());
  for (const auto& f : frame_stats_) {
    if (f.has_data) burst.push_back(f.median_burst_10bit);
    report->rows.push_back(
        {StageReportValue::of_integer(f.frame_number),
         f.has_data ? StageReportValue::of_real(f.median_burst_10bit)
                    : StageReportValue::absent()});
  }
  auto& stats = report->statistics;
  stats["total_frames"] = StageReportValue::of_integer(total_frames_);
  stats["frames_analysed"] =
      StageReportValue::of_integer(static_cast<int64_t>(frame_stats_.size()));
  analysis_report::add_series_statistics(&stats, "median_burst_10bit", burst);
  return true;
}

}  // namespace orc
