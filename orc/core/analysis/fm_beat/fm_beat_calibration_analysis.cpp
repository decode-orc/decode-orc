/*
 * File:        fm_beat_calibration_analysis.cpp
 * Module:      analysis
 * Purpose:     FM Beat Calibration: sets FM Beat Cancel's carrier from the
 *              beat on the stage's input
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "fm_beat_calibration_analysis.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>

#include "../../include/dag_executor.h"
#include "../../include/project.h"
#include "../analysis_registry.h"
#include "fm_beat_calibration.h"

namespace orc {

REGISTER_ANALYSIS_TOOL(FmBeatCalibrationAnalysisTool)

void force_link_FmBeatCalibrationAnalysisTool() {}

namespace {

constexpr const char* kCarrierKey = "carrierBlackMHz";

std::string fixed(double value, int digits) {
  std::ostringstream s;
  s << std::fixed << std::setprecision(digits) << value;
  return s.str();
}

std::string mhz(double hz) { return fixed(hz / 1.0e6, 4); }

}  // namespace

std::string FmBeatCalibrationAnalysisTool::id() const {
  return "fm_beat_calibration";
}

std::string FmBeatCalibrationAnalysisTool::name() const {
  return "FM Beat Calibration";
}

std::string FmBeatCalibrationAnalysisTool::description() const {
  return "Measures the master's FM carrier at black from the FM-beat "
         "herringbone in the source's flat saturated colour, for FM Beat "
         "Cancel";
}

std::string FmBeatCalibrationAnalysisTool::category() const {
  return "Source Processing";
}

std::vector<ParameterDescriptor> FmBeatCalibrationAnalysisTool::parameters()
    const {
  return {};
}

bool FmBeatCalibrationAnalysisTool::canAnalyze(
    AnalysisSourceType source_type) const {
  return source_type == AnalysisSourceType::LaserDisc;
}

bool FmBeatCalibrationAnalysisTool::isApplicableToStage(
    const std::string& stage_name) const {
  return stage_name == "fm_beat_cancel";
}

AnalysisResult FmBeatCalibrationAnalysisTool::analyze(
    const AnalysisContext& ctx, AnalysisProgress* progress) {
  AnalysisResult result;
  auto fail = [&](const std::string& why) {
    result.status = AnalysisResult::Failed;
    result.summary = why;
    return result;
  };
  if (!ctx.dag || !ctx.node_id.is_valid()) {
    return fail(
        "FM Beat Calibration needs a project and an fm_beat_cancel node.");
  }
  const auto& nodes = ctx.dag->nodes();
  const auto node_it = std::find_if(
      nodes.begin(), nodes.end(),
      [&](const DAGNode& node) { return node.node_id == ctx.node_id; });
  if (node_it == nodes.end() || node_it->input_node_ids.empty()) {
    return fail("Connect a source to the FM Beat Cancel node first.");
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
        "The FM-beat herringbone is a PAL LaserDisc effect: the input must be "
        "a PAL composite source.");
  }

  const auto calibration =
      calibrate_fm_beat(*source, FmBeatCalibrationOptions{},
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
  result.statistics["probedFrames"] =
      static_cast<int>(calibration.probed_frames);
  result.statistics["fieldsSearched"] = static_cast<int>(calibration.fields);
  if (calibration.fields == 0) {
    result.summary =
        "No field of the " + std::to_string(calibration.probed_frames) +
        " frames read has enough flat saturated colour to measure a beat on. "
        "FM Beat Cancel leaves such pictures as they are, so it has nothing to "
        "do on this source.";
    return result;
  }
  result.statistics["carrierBlackMHz"] = calibration.carrier_black_hz / 1.0e6;
  result.statistics["linesCorrected"] =
      static_cast<int>(calibration.lines_corrected);
  result.statistics["pictureLines"] =
      static_cast<int>(calibration.picture_lines);
  result.statistics["removedRmsIre"] = calibration.removed_rms_ire;
  for (const auto& [carrier, energy] : calibration.sweep) {
    AnalysisResultItem item;
    item.type = "sweep";
    item.message = mhz(carrier) + " MHz";
    item.metadata["carrierMHz"] = carrier / 1.0e6;
    item.metadata["removedEnergy"] = energy;
    result.items.push_back(item);
  }
  if (!calibration.found) {
    result.summary =
        "No beat found: on the " + std::to_string(calibration.fields) +
        " fields with the most flat colour, no carrier corrects any line. "
        "This source carries no beat FM Beat Cancel can remove.";
    return result;
  }
  result.summary =
      "Carrier at black: " + mhz(calibration.carrier_black_hz) + " MHz.\n" +
      std::to_string(calibration.lines_corrected) + " of " +
      std::to_string(calibration.picture_lines) +
      " picture lines corrected on " + std::to_string(calibration.fields) +
      " fields with flat colour, " + fixed(calibration.removed_rms_ire, 2) +
      " IRE RMS removed on them.\nApply to Stage sets carrier_black_mhz.";
  result.graphData[kCarrierKey] = mhz(calibration.carrier_black_hz);
  return result;
}

bool FmBeatCalibrationAnalysisTool::canApplyToGraph() const { return true; }

bool FmBeatCalibrationAnalysisTool::applyToGraph(AnalysisResult& result,
                                                 const Project& /*project*/,
                                                 NodeID node_id) {
  const auto it = result.graphData.find(kCarrierKey);
  if (result.status != AnalysisResult::Success ||
      it == result.graphData.end()) {
    return false;
  }
  try {
    result.parameterChanges["carrier_black_mhz"] = std::stod(it->second);
  } catch (const std::exception&) {
    return false;
  }
  ORC_LOG_DEBUG("FM beat calibration: carrier {} MHz for node '{}'", it->second,
                node_id);
  return true;
}

}  // namespace orc
