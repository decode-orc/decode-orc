/*
 * File:        snr_analysis_sink_stage.cpp
 * Module:      orc-core
 * Purpose:     SNR Analysis Sink Stage implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "snr_analysis_sink_stage.h"

#include <orc/abi/orc_plugin_services.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "../common/analysis-results/analysis_report_statistics.h"
#include "../common/analysis-results/analysis_series_wire.h"
#include "snr_analysis_sink_deps.h"
#include "snr_analysis_sink_deps_interface.h"

namespace orc {

SNRAnalysisSinkStage::SNRAnalysisSinkStage() {
  set_configuration_status(orc::ConfigurationStatus::Yellow);
}

NodeTypeInfo SNRAnalysisSinkStage::get_node_type_info() const {
  return NodeTypeInfo{NodeType::ANALYSIS_SINK,
                      "snr_analysis_sink",
                      "SNR Analysis Sink",
                      "Computes SNR/PSNR statistics and optionally writes CSV. "
                      "Trigger to update dataset.",
                      1,  // min_inputs
                      1,  // max_inputs
                      0,  // min_outputs
                      0,  // max_outputs
                      VideoFormatCompatibility::ALL};
}

std::vector<ArtifactPtr> SNRAnalysisSinkStage::execute(
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

StagePreviewCapability SNRAnalysisSinkStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_input_);
}

std::vector<ParameterDescriptor>
SNRAnalysisSinkStage::get_parameter_descriptors(VideoSystem project_format,
                                                SourceType source_type) const {
  (void)project_format;
  (void)source_type;

  std::vector<ParameterDescriptor> descriptors;

  descriptors.push_back(ParameterDescriptor{
      "output_path", "CSV Output Path",
      "Destination CSV file for SNR metrics. Leave empty to skip file output.",
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

  descriptors.push_back(ParameterDescriptor{
      "mode", "Analysis Mode", "Select white, black, or both SNR metrics.",
      ParameterType::STRING,
      ParameterConstraints{std::nullopt,
                           std::nullopt,
                           std::string("both"),
                           {"white", "black", "both"},
                           true,
                           std::nullopt}});

  return descriptors;
}

std::map<std::string, ParameterValue> SNRAnalysisSinkStage::get_parameters()
    const {
  return parameters_;
}

bool SNRAnalysisSinkStage::set_parameters(
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

SNRAnalysisSinkStage::ParsedConfig SNRAnalysisSinkStage::parse_config(
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

  auto mode_it = parameters.find("mode");
  if (mode_it != parameters.end() &&
      std::holds_alternative<std::string>(mode_it->second)) {
    auto m = std::get<std::string>(mode_it->second);
    if (m == "white") {
      cfg.mode = SNRAnalysisMode::WHITE;
    } else if (m == "black") {
      cfg.mode = SNRAnalysisMode::BLACK;
    } else {
      cfg.mode = SNRAnalysisMode::BOTH;
    }
  }

  return cfg;
}

bool SNRAnalysisSinkStage::trigger(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    IObservationContext& observation_context) {
  ORC_LOG_DEBUG("SNRAnalysisSink: Trigger started");
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
    last_mode_ = cfg.mode;

    std::shared_ptr<ISNRAnalysisSinkStageDeps> deps = deps_override_;
    if (!deps) {
      deps = std::make_shared<SNRAnalysisSinkStageDeps>(
          orc::plugin::get_observation_service());
    }

    deps->init(progress_callback_, &cancel_requested_);

    SNRAnalysisComputeOptions compute_options;
    compute_options.output_path = cfg.output_path;
    compute_options.write_csv = cfg.write_csv;
    compute_options.snr_mode = cfg.mode;

    const SNRAnalysisComputeResult compute_result = deps->compute_and_analyze(
        vfr.get(), observation_context, compute_options);

    if (!compute_result.success) {
      has_results_ = false;
      frame_stats_.clear();
      total_frames_ = 0;
      last_status_ = compute_result.message.empty()
                         ? "Error: SNR analysis failed"
                         : (compute_result.message == "Cancelled by user"
                                ? compute_result.message
                                : "Error: " + compute_result.message);
      is_processing_.store(false);
      return false;
    }

    frame_stats_ = compute_result.frame_stats;
    total_frames_ = compute_result.total_frames;

    if (cfg.write_csv && !cfg.output_path.empty()) {
      if (!deps->write_csv(cfg.output_path, frame_stats_)) {
        ORC_LOG_WARN("SNRAnalysisSink: Failed to write CSV to {}",
                     cfg.output_path);
      }
    }

    last_status_ = "SNR analysis complete";
    has_results_ = true;
    is_processing_.store(false);
    return true;

  } catch (const std::exception& e) {
    last_status_ = std::string("Error: ") + e.what();
    ORC_LOG_ERROR("SNRAnalysisSink: Trigger failed: {}", e.what());
    is_processing_.store(false);
    return false;
  }
}

StageToolReply SNRAnalysisSinkStage::handle_tool_request(
    const std::string& contract_id, const std::string& verb,
    const std::vector<uint8_t>& payload) {
  (void)payload;
  StageToolReply reply;
  if (contract_id != analysis_wire::kSnrContractId ||
      verb != analysis_wire::kResultsVerb) {
    reply.error = "The SNR analysis sink does not answer '" + verb + "' for " +
                  contract_id;
    return reply;
  }
  if (!has_results_) {
    reply.error = analysis_wire::kNotTriggeredMessage;
    return reply;
  }
  analysis_wire::SnrResults results;
  results.total_frames = total_frames_;
  results.frames.reserve(frame_stats_.size());
  for (const auto& f : frame_stats_) {
    results.frames.push_back({f.frame_number, f.white_snr, f.black_psnr,
                              f.has_white_snr, f.has_black_psnr, f.has_data});
  }
  reply.data = analysis_wire::encode(results);
  reply.ok = true;
  return reply;
}

bool SNRAnalysisSinkStage::results_report(StageResultsReport* report) const {
  if (!has_results_ || !report) {
    return false;
  }
  *report = StageResultsReport{};
  report->kind = "snr";
  report->total_frames = total_frames_;
  std::vector<double> white;
  std::vector<double> black;
  report->columns = {"frame", "white_snr", "black_psnr"};
  report->rows.reserve(frame_stats_.size());
  for (const auto& f : frame_stats_) {
    if (f.has_white_snr) white.push_back(f.white_snr);
    if (f.has_black_psnr) black.push_back(f.black_psnr);
    report->rows.push_back(
        {StageReportValue::of_integer(f.frame_number),
         f.has_white_snr ? StageReportValue::of_real(f.white_snr)
                         : StageReportValue::absent(),
         f.has_black_psnr ? StageReportValue::of_real(f.black_psnr)
                          : StageReportValue::absent()});
  }
  auto& stats = report->statistics;
  stats["total_frames"] = StageReportValue::of_integer(total_frames_);
  stats["frames_analysed"] =
      StageReportValue::of_integer(static_cast<int64_t>(frame_stats_.size()));
  analysis_report::add_series_statistics(&stats, "white_snr", white);
  analysis_report::add_series_statistics(&stats, "black_psnr", black);
  return true;
}

}  // namespace orc
