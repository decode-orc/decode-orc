/*
 * File:        channel_calibration_analysis.cpp
 * Module:      analysis
 * Purpose:     Channel Calibration: sets Channel Equalise's edge smear from
 *              the VITS on the stage's input
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "channel_calibration_analysis.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <iomanip>
#include <sstream>

#include "../../include/dag_executor.h"
#include "../../include/project.h"
#include "../analysis_registry.h"
#include "channel_calibration.h"

namespace orc {

REGISTER_ANALYSIS_TOOL(ChannelCalibrationAnalysisTool)

void force_link_ChannelCalibrationAnalysisTool() {}

namespace {

constexpr const char* kFractionKey = "smearFraction";
constexpr const char* kTauKey = "smearTauSamples";

std::string fixed(double value, int digits) {
  std::ostringstream s;
  s << std::fixed << std::setprecision(digits) << value;
  return s.str();
}

std::string verdict_text(SmearVerdict verdict) {
  switch (verdict) {
    case SmearVerdict::Smear:
      return "smear";
    case SmearVerdict::NoSmear:
      return "no smear";
    case SmearVerdict::CannotTell:
      break;
  }
  return "cannot tell";
}

}  // namespace

std::string ChannelCalibrationAnalysisTool::id() const {
  return "channel_calibration";
}

std::string ChannelCalibrationAnalysisTool::name() const {
  return "Channel Calibration";
}

std::string ChannelCalibrationAnalysisTool::description() const {
  return "Measures the disc's edge smear from the test signals on field 1 "
         "line 19, for Channel Equalise";
}

std::string ChannelCalibrationAnalysisTool::category() const {
  return "Source Processing";
}

std::vector<ParameterDescriptor> ChannelCalibrationAnalysisTool::parameters()
    const {
  return {};
}

bool ChannelCalibrationAnalysisTool::canAnalyze(
    AnalysisSourceType source_type) const {
  return source_type == AnalysisSourceType::LaserDisc;
}

bool ChannelCalibrationAnalysisTool::isApplicableToStage(
    const std::string& stage_name) const {
  return stage_name == "channel_equalise";
}

AnalysisResult ChannelCalibrationAnalysisTool::analyze(
    const AnalysisContext& ctx, AnalysisProgress* progress) {
  AnalysisResult result;
  auto fail = [&](const std::string& why) {
    result.status = AnalysisResult::Failed;
    result.summary = why;
    return result;
  };
  if (!ctx.dag || !ctx.node_id.is_valid()) {
    return fail(
        "Channel Calibration needs a project and a channel_equalise node.");
  }
  const auto& nodes = ctx.dag->nodes();
  const auto node_it = std::find_if(
      nodes.begin(), nodes.end(),
      [&](const DAGNode& node) { return node.node_id == ctx.node_id; });
  if (node_it == nodes.end() || node_it->input_node_ids.empty()) {
    return fail("Connect a source to the Channel Equalise node first.");
  }

  if (progress) progress->setStatus("Reading the source");
  std::shared_ptr<const VideoFrameRepresentation> source;
  try {
    const NodeID input = node_it->input_node_ids[0];
    DAGExecutor executor;
    const auto outputs = executor.execute_to_node(*ctx.dag, input);
    const auto it = outputs.find(input);
    if (it != outputs.end()) {
      for (const auto& artifact : it->second) {
        source =
            std::dynamic_pointer_cast<const VideoFrameRepresentation>(artifact);
        if (source) break;
      }
    }
  } catch (const std::exception& e) {
    return fail(std::string("The input could not be run: ") + e.what());
  }
  if (!source) return fail("The input produces no video.");
  const auto params = source->get_video_parameters();
  if (!params || params->system != VideoSystem::PAL ||
      source->has_separate_channels()) {
    return fail(
        "The edge smear is a PAL LaserDisc effect: the input must be a PAL "
        "composite source.");
  }

  const auto calibration =
      calibrate_channel(*source, ChannelCalibrationOptions{},
                        [&](int percent, const std::string& status) {
                          if (progress) {
                            progress->setProgress(percent);
                            progress->setSubStatus(status);
                            return !progress->isCancelled();
                          }
                          return true;
                        });
  if (progress && progress->isCancelled()) {
    result.status = AnalysisResult::Cancelled;
    return result;
  }

  result.status = AnalysisResult::Success;
  size_t frames_used = 0;
  for (const auto& window : calibration.windows) {
    frames_used += window.frames_used;
    AnalysisResultItem item;
    item.type = "window";
    item.startFrame = static_cast<int>(window.first);
    item.endFrame = static_cast<int>(window.first + window.frames - 1);
    item.message = verdict_text(window.verdict);
    item.metadata["framesUsed"] = static_cast<int>(window.frames_used);
    if (window.fit) {
      item.message += ": fraction " + fixed(window.fit->fraction, 3) +
                      ", tau " + fixed(window.fit->tau_samples, 2) + " samples";
      item.metadata["fraction"] = window.fit->fraction;
      item.metadata["tauSamples"] = window.fit->tau_samples;
      item.metadata["residualIre"] = window.fit->residual_ire;
      item.metadata["residualNoSmearIre"] = window.fit->residual_no_smear_ire;
    }
    result.items.push_back(item);
  }
  result.statistics["windows"] = static_cast<int>(calibration.windows.size());
  result.statistics["framesUsed"] = static_cast<int>(frames_used);

  switch (calibration.verdict) {
    case SmearVerdict::CannotTell:
      result.summary =
          "No smear could be measured: none of the frames read has a usable "
          "white bar and 2T pulse (CCIR 17) on field 1 line 19. The stage's "
          "values are left as they are.";
      return result;
    case SmearVerdict::NoSmear:
      result.summary =
          "No smear: the test signals on field 1 line 19 are explained "
          "without one. Leave Channel Equalise out of this project; Apply to "
          "Stage sets smear_fraction to 0, which passes every frame through "
          "unchanged.";
      result.statistics["smearFraction"] = 0.0;
      result.graphData[kFractionKey] = "0";
      return result;
    case SmearVerdict::Smear:
      break;
  }
  result.statistics["smearFraction"] = calibration.fraction;
  result.statistics["smearTauSamples"] = calibration.tau_samples;
  result.summary = "Edge smear: fraction " + fixed(calibration.fraction, 3) +
                   ", time constant " + fixed(calibration.tau_samples, 2) +
                   " samples.\nMeasured on field 1 line 19 of " +
                   std::to_string(frames_used) +
                   " frames.\nApply to Stage "
                   "sets smear_fraction and smear_tau_samples.";
  result.graphData[kFractionKey] = fixed(calibration.fraction, 4);
  result.graphData[kTauKey] = fixed(calibration.tau_samples, 3);
  return result;
}

bool ChannelCalibrationAnalysisTool::canApplyToGraph() const { return true; }

bool ChannelCalibrationAnalysisTool::applyToGraph(AnalysisResult& result,
                                                  const Project& /*project*/,
                                                  NodeID node_id) {
  const auto fraction = result.graphData.find(kFractionKey);
  if (result.status != AnalysisResult::Success ||
      fraction == result.graphData.end()) {
    return false;
  }
  try {
    result.parameterChanges["smear_fraction"] = std::stod(fraction->second);
    const auto tau = result.graphData.find(kTauKey);
    if (tau != result.graphData.end()) {
      result.parameterChanges["smear_tau_samples"] = std::stod(tau->second);
    }
  } catch (const std::exception&) {
    return false;
  }
  ORC_LOG_DEBUG("Channel calibration: smear fraction {} for node '{}'",
                fraction->second, node_id);
  return true;
}

}  // namespace orc
