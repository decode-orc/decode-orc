/*
 * File:        mask_line_analysis.cpp
 * Module:      orc-core
 * Purpose:     Line masking configuration analysis tool
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "mask_line_analysis.h"

#include <frame_numbering.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <sstream>

#include "../../include/dag_executor.h"
#include "../../include/project.h"
#include "../analysis_registry.h"

namespace orc {

// Force linker to include this object file (for static registration)
void force_link_MaskLineAnalysisTool() {}

std::string MaskLineAnalysisTool::id() const { return "mask_line_config"; }

std::string MaskLineAnalysisTool::name() const {
  return "Configure Line Masking";
}

std::string MaskLineAnalysisTool::description() const {
  return "Configure line masking with convenient presets for common scenarios "
         "like hiding NTSC closed captions. Configuration is applied "
         "immediately.";
}

std::string MaskLineAnalysisTool::category() const { return "Configuration"; }

std::vector<ParameterDescriptor> MaskLineAnalysisTool::parameters() const {
  std::vector<ParameterDescriptor> params;

  // Checkbox for masking NTSC CC line
  ParameterDescriptor mask_ntsc_cc;
  mask_ntsc_cc.name = "maskNTSC_CC";
  mask_ntsc_cc.display_name = "Mask NTSC Closed Captions";
  mask_ntsc_cc.description =
      "Mask field line 20 in first field (NTSC closed caption - traditional "
      "'line 21' is index 20)";
  mask_ntsc_cc.type = ParameterType::BOOL;
  mask_ntsc_cc.constraints.default_value = false;
  params.push_back(mask_ntsc_cc);

  // Checkbox for masking PAL Teletext/WSS
  ParameterDescriptor mask_pal_tt;
  mask_pal_tt.name = "maskPAL_TT";
  mask_pal_tt.display_name = "Mask PAL Teletext/WSS";
  mask_pal_tt.description =
      "Mask field lines 6-22 in both fields (PAL Teletext and WSS lines)";
  mask_pal_tt.type = ParameterType::BOOL;
  mask_pal_tt.constraints.default_value = false;
  params.push_back(mask_pal_tt);

  // Custom line specification (advanced)
  ParameterDescriptor custom_lines;
  custom_lines.name = "customLines";
  custom_lines.display_name = "Custom Line Spec";
  custom_lines.description =
      "Custom frame-flat line specification, 1-based as shown in the Mask "
      "Line stage parameters (e.g., '22' or '101-116', comma-separated). "
      "Leave empty to use only preset options above.";
  custom_lines.type = ParameterType::STRING;
  custom_lines.constraints.default_value = std::string("");
  params.push_back(custom_lines);

  // Mask sample level
  ParameterDescriptor mask_sample;
  mask_sample.name = "maskSampleLevel";
  mask_sample.display_name = "Mask Sample Level";
  mask_sample.description =
      "10-bit sample level to write to masked pixels (0=sync tip; PAL "
      "256=blanking, 844=white; NTSC/PAL-M 240=blanking, 282=black, "
      "800=white)";
  mask_sample.type = ParameterType::INT32;
  mask_sample.constraints.default_value = int32_t{0};
  mask_sample.constraints.min_value = ParameterValue{int32_t{0}};
  mask_sample.constraints.max_value = ParameterValue{int32_t{1023}};
  params.push_back(mask_sample);

  return params;
}

std::vector<ParameterDescriptor> MaskLineAnalysisTool::parametersForContext(
    const AnalysisContext& ctx) const {
  // Get base parameters
  auto params = parameters();

  // Show/hide PAL or NTSC specific options based on video system
  // For now, just return all parameters since we don't have direct access to
  // video system The user can see all options and choose what's relevant
  (void)ctx;  // Unused for now

  return params;
}

bool MaskLineAnalysisTool::canAnalyze(AnalysisSourceType source_type) const {
  // Can work with laserdisc sources
  return source_type == AnalysisSourceType::LaserDisc;
}

bool MaskLineAnalysisTool::isApplicableToStage(
    const std::string& stage_name) const {
  // This tool is applicable to mask_line stages
  return stage_name == "mask_line";
}

AnalysisResult MaskLineAnalysisTool::analyze(const AnalysisContext& ctx,
                                             AnalysisProgress* progress) {
  AnalysisResult result;

  // This is an instant configuration tool - no progress needed
  (void)progress;

  // Build the configuration based on checkboxes
  int32_t mask_sample_level = 0;

  // Extract parameters
  bool mask_ntsc_cc = false;
  bool mask_pal_tt = false;
  std::string custom_lines;

  auto it = ctx.parameters.find("maskNTSC_CC");
  if (it != ctx.parameters.end() && std::holds_alternative<bool>(it->second)) {
    mask_ntsc_cc = std::get<bool>(it->second);
  }

  it = ctx.parameters.find("maskPAL_TT");
  if (it != ctx.parameters.end() && std::holds_alternative<bool>(it->second)) {
    mask_pal_tt = std::get<bool>(it->second);
  }

  it = ctx.parameters.find("customLines");
  if (it != ctx.parameters.end() &&
      std::holds_alternative<std::string>(it->second)) {
    custom_lines = std::get<std::string>(it->second);
  }

  it = ctx.parameters.find("maskSampleLevel");
  if (it != ctx.parameters.end() &&
      std::holds_alternative<int32_t>(it->second)) {
    mask_sample_level =
        std::clamp(std::get<int32_t>(it->second), int32_t{0}, int32_t{1023});
  }

  // Build configuration
  std::string line_spec_result;

  if (mask_ntsc_cc) {
    // NTSC CC is on field line 20 (0-based index), first field only
    // Traditional "line 21" in 1-based video terminology = index 20
    line_spec_result = "F:20";
  } else if (mask_pal_tt) {
    // PAL Teletext/WSS is on field lines 6-22 (0-based), both fields
    line_spec_result = "A:6-22";
  } else if (!custom_lines.empty()) {
    // Custom lines are entered 1-based (matching the Mask Line stage
    // parameter dialog); store the 0-based form. Legacy parity-prefixed
    // specs (F:/S:/A:) that cannot be converted pass through unchanged.
    auto converted = range_spec_from_presentation(custom_lines);
    line_spec_result = converted ? *converted : custom_lines;
  } else {
    // No masking selected
    line_spec_result = "";
  }

  result.graphData["lineSpec"] = line_spec_result;
  result.graphData["maskSampleLevel"] = std::to_string(mask_sample_level);

  // Build summary showing what will be applied
  std::ostringstream summary;
  summary << "Configuration ready to apply:\n\n";
  // Line numbers are presented 1-based, matching the stage parameter dialog.
  summary << "Line Specification: ";
  if (line_spec_result.empty()) {
    summary << "(none - no lines will be masked)\n";
  } else {
    summary << range_spec_to_presentation(line_spec_result) << "\n";
    if (mask_ntsc_cc) {
      summary << "  → NTSC Closed Captions (field line 20, first field - "
                 "traditional 'line 21')\n";
    } else if (mask_pal_tt) {
      summary << "  → PAL Teletext/WSS (field lines 6-22, both fields)\n";
    }
  }
  summary << "\nMask Sample Level: " << mask_sample_level;
  if (mask_sample_level == 0) {
    summary << " (sync tip)";
  } else if (mask_sample_level == 256) {
    summary << " (blanking PAL)";
  } else if (mask_sample_level == 844) {
    summary << " (white PAL)";
  } else if (mask_sample_level == 240) {
    summary << " (blanking NTSC/PAL-M)";
  } else if (mask_sample_level == 282) {
    summary << " (black NTSC/PAL-M)";
  } else if (mask_sample_level == 800) {
    summary << " (white NTSC/PAL-M)";
  }
  summary << "\n\n";

  if (!line_spec_result.empty()) {
    summary << "Click 'Apply to Node' to configure the Mask Line stage.";
  }

  result.summary = summary.str();
  result.status = AnalysisResult::Success;

  return result;
}

bool MaskLineAnalysisTool::canApplyToGraph() const { return true; }

bool MaskLineAnalysisTool::applyToGraph(AnalysisResult& result,
                                        const Project& project,
                                        NodeID node_id) {
  // Apply the configuration to the mask_line stage
  try {
    // Get the node to modify parameters
    const auto& nodes = project.get_nodes();
    auto node_it = std::find_if(
        nodes.begin(), nodes.end(),
        [&node_id](const ProjectDAGNode& n) { return n.node_id == node_id; });

    if (node_it == nodes.end()) {
      ORC_LOG_ERROR("MaskLineAnalysisTool: node not found: {}", node_id);
      return false;
    }

    // Build updated parameters
    auto updated_params = node_it->parameters;

    auto data_it = result.graphData.find("lineSpec");
    if (data_it != result.graphData.end()) {
      updated_params["lineSpec"] = data_it->second;
    }

    data_it = result.graphData.find("maskSampleLevel");
    if (data_it != result.graphData.end()) {
      try {
        int32_t level = std::clamp(std::stoi(data_it->second), 0, 1023);
        updated_params["maskSampleLevel"] = level;
      } catch (const std::exception& e) {
        ORC_LOG_WARN("Failed to parse maskSampleLevel: {}", e.what());
      }
    }

    // Populate parameterChanges instead of modifying project directly
    result.parameterChanges = updated_params;

    ORC_LOG_INFO("Prepared line masking configuration for node '{}'", node_id);
    return true;

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("Failed to apply line masking configuration: {}", e.what());
    return false;
  }
}

// Register the tool
REGISTER_ANALYSIS_TOOL(MaskLineAnalysisTool);

}  // namespace orc
