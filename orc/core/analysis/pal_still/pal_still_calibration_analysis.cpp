/*
 * File:        pal_still_calibration_analysis.cpp
 * Module:      analysis
 * Purpose:     PAL Still Calibration: sets the video sink's line-alternating
 *              chroma matrix for the PAL Still decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "pal_still_calibration_analysis.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>

#include "../../include/dag_executor.h"
#include "../../include/project.h"
#include "../analysis_registry.h"
#include "pal_still_calibration.h"

namespace orc {

REGISTER_ANALYSIS_TOOL(PalStillCalibrationAnalysisTool)

void force_link_PalStillCalibrationAnalysisTool() {}

namespace {

// The video sink's parameters for M's elements, row-major.
constexpr std::array<const char*, 4> kParameters = {"hanover_uu", "hanover_uv",
                                                    "hanover_vu", "hanover_vv"};

std::string fixed(double value, int digits) {
  std::ostringstream s;
  s << std::fixed << std::setprecision(digits) << value;
  return s.str();
}

}  // namespace

std::string PalStillCalibrationAnalysisTool::id() const {
  return "pal_still_calibration";
}

std::string PalStillCalibrationAnalysisTool::name() const {
  return "PAL Still Calibration";
}

std::string PalStillCalibrationAnalysisTool::description() const {
  return "Measures the source's line-alternating chroma error on flat colour, "
         "which the PAL Still decoder corrects";
}

std::string PalStillCalibrationAnalysisTool::category() const {
  return "Source Processing";
}

std::vector<ParameterDescriptor> PalStillCalibrationAnalysisTool::parameters()
    const {
  return {};
}

bool PalStillCalibrationAnalysisTool::canAnalyze(
    AnalysisSourceType source_type) const {
  return source_type == AnalysisSourceType::LaserDisc;
}

bool PalStillCalibrationAnalysisTool::isApplicableToStage(
    const std::string& stage_name) const {
  return stage_name == "video_sink";
}

AnalysisResult PalStillCalibrationAnalysisTool::analyze(
    const AnalysisContext& ctx, AnalysisProgress* progress) {
  AnalysisResult result;
  auto fail = [&](const std::string& why) {
    result.status = AnalysisResult::Failed;
    result.summary = why;
    return result;
  };
  if (!ctx.dag || !ctx.node_id.is_valid()) {
    return fail("PAL Still Calibration needs a project and a video_sink node.");
  }
  const auto& nodes = ctx.dag->nodes();
  const auto node_it = std::find_if(
      nodes.begin(), nodes.end(),
      [&](const DAGNode& node) { return node.node_id == ctx.node_id; });
  if (node_it == nodes.end() || node_it->input_node_ids.empty()) {
    return fail("Connect a source to the video sink first.");
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
        "PAL Still decodes 625-line PAL composite sources: the input must be "
        "one.");
  }

  const auto calibration =
      calibrate_pal_still(*source, PalStillCalibrationOptions{},
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
  result.statistics["framesRead"] = static_cast<int>(calibration.frames_read);
  result.statistics["flatPairs"] = static_cast<int>(calibration.pairs);
  result.statistics["flatPairsUsed"] = static_cast<int>(calibration.pairs_used);
  if (!calibration.matrix) {
    result.summary =
        "Too little flat colour to measure: " +
        std::to_string(calibration.pairs_used) + " samples on the " +
        std::to_string(calibration.frames_read) +
        " frames read, where the fit needs 1000. The matrix is left as it "
        "is.";
    return result;
  }
  const auto& m = *calibration.matrix;
  for (size_t i = 0; i < kParameters.size(); ++i) {
    result.statistics[kParameters[i]] = m[i];
    result.graphData[kParameters[i]] = fixed(m[i], 4);
  }
  result.summary =
      "Line-alternating matrix: U from U " + fixed(m[0], 4) + ", U from V " +
      fixed(m[1], 4) + ", V from U " + fixed(m[2], 4) + ", V from V " +
      fixed(m[3], 4) + ".\nMeasured on " +
      std::to_string(calibration.pairs_used) + " samples of flat colour in " +
      std::to_string(calibration.frames_read) +
      " frames.\nApply to Stage sets hanover_uu, hanover_uv, hanover_vu and "
      "hanover_vv, which the palstill decoder uses.";
  return result;
}

bool PalStillCalibrationAnalysisTool::canApplyToGraph() const { return true; }

bool PalStillCalibrationAnalysisTool::applyToGraph(AnalysisResult& result,
                                                   const Project& /*project*/,
                                                   NodeID node_id) {
  if (result.status != AnalysisResult::Success) return false;
  for (const char* key : kParameters) {
    if (result.graphData.find(key) == result.graphData.end()) return false;
  }
  try {
    for (const char* key : kParameters) {
      result.parameterChanges[key] = std::stod(result.graphData.at(key));
    }
  } catch (const std::exception&) {
    return false;
  }
  ORC_LOG_DEBUG("PAL Still calibration: matrix applied to node '{}'", node_id);
  return true;
}

}  // namespace orc
