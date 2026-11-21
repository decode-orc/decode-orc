/*
 * File:        orc_analysis.h
 * Module:      orc-view-types
 * Purpose:     Analysis tools and results view types
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/params/parameter_types.h>

#include <map>
#include <string>
#include <variant>
#include <vector>

namespace orc {

/**
 * @brief Information about an available analysis tool
 */
struct AnalysisToolInfo {
  std::string id;           ///< Unique tool identifier
  std::string name;         ///< Human-readable name
  std::string description;  ///< Description of what the tool does
  std::vector<std::string>
      applicable_stages;            ///< Stage types this tool can analyze
  std::string stage_tool_kind;      ///< SDK tool kind (config_dialog,
                                    ///< non_modal_editor, batch_analysis,
                                    ///< preview_utility, plugin_view,
                                    ///< analysis)
  std::string stage_tool_contract;  ///< SDK tool contract id: picks the
                                    ///< view's window
};

/**
 * @brief True when a tool exists only to show what a trigger produced
 *
 * Batch analyses and plugin views are both viewers over one
 * trigger run's results: triggering the stage is what fills them, so the host
 * opens them
 * automatically when a trigger succeeds. Picking one from the menu afterwards
 * re-opens those same results — a read, never a re-run, so it stays distinct
 * from the Trigger Stage action and cannot serve results the current
 * parameters did not produce.
 */
inline bool isTriggerResultViewer(const AnalysisToolInfo& tool) {
  return tool.stage_tool_kind == "batch_analysis" ||
         tool.stage_tool_kind == "plugin_view";
}

/**
 * @brief Statistic value that can be displayed
 */
using StatisticValue = std::variant<bool, int, long long, double, std::string>;

/**
 * @brief Individual result item from analysis
 */
struct AnalysisResultItem {
  std::string type;     ///< Type: "skip", "repeat", "gap", "warning", etc.
  std::string message;  ///< Human-readable description
  int startFrame = -1;  ///< Start frame (-1 if not applicable)
  int endFrame = -1;    ///< End frame (-1 if not applicable)
  std::map<std::string, StatisticValue> metadata;  ///< Tool-specific data
};

/**
 * @brief Complete analysis result
 */
struct AnalysisResult {
  enum Status { Success, Failed, Cancelled };

  Status status = Success;
  std::string summary;                               ///< Human-readable summary
  std::vector<AnalysisResultItem> items;             ///< Structured results
  std::map<std::string, StatisticValue> statistics;  ///< Statistics for display
  std::map<std::string, orc::ParameterValue>
      parameterChanges;  ///< Parameter modifications to apply
};

}  // namespace orc
