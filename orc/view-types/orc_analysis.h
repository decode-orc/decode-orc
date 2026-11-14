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

#include <cstdint>
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
  std::string category;     ///< Category for organization
  int priority;             ///< Menu ordering priority (lower = first)
  std::vector<std::string>
      applicable_stages;            ///< Stage types this tool can analyze
  std::string stage_tool_kind;      ///< Optional SDK tool kind (config_dialog,
                                    ///< non_modal_editor, batch_analysis,
                                    ///< preview_utility, plugin_view)
  std::string stage_tool_contract;  ///< Optional SDK tool contract id for
                                    ///< stable dispatch
  bool stage_tool_non_modal =
      false;  ///< True when the tool is intended to open non-modally
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
 * @brief Analysis operation status
 */
enum class AnalysisStatus {
  NotStarted,  ///< Analysis not yet started
  Running,     ///< Analysis in progress
  Complete,    ///< Analysis completed successfully
  Failed,      ///< Analysis failed with error
  Cancelled    ///< Analysis was cancelled
};

/**
 * @brief Progress information snapshot for displaying analysis progress
 */
struct AnalysisProgressInfo {
  int current;                 ///< Current progress value
  int total;                   ///< Total work units
  std::string status_message;  ///< Primary status message
  std::string sub_status;      ///< Secondary status message
  AnalysisStatus status;       ///< Current status
};

/**
 * @brief Type of source being analyzed
 */
enum class AnalysisSourceType { LaserDisc, CVBSVideo, Other };

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
  std::map<std::string, std::string>
      graphData;  ///< Data for graph application (opaque to GUI)
  std::map<std::string, orc::ParameterValue>
      parameterChanges;  ///< Parameter modifications to apply

  //  Nested type alias for compatibility
  using ResultItem = AnalysisResultItem;
};

}  // namespace orc
