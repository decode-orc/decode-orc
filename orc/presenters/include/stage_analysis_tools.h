/*
 * File:        stage_analysis_tools.h
 * Module:      orc-presenters
 * Purpose:     Running a stage's analysis tools (StageToolKind::Analysis)
 *              for orc-cli and orc-gui, and their view-tier forms
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/node_id.h>
#include <orc/stage/params/parameter_types.h>
#include <orc/view/stage_tool_session.h>
#include <orc_analysis.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace orc {
class Project;
}  // namespace orc

namespace orc::presenters {

/// The tool_info.stage_tool_kind of an analysis tool a stage offers.
inline constexpr const char kStageAnalysisToolKind[] = "analysis";

/// True when @p tool is an analysis tool a stage offers (rather than a
/// configuration dialog, editor or other view).
inline bool isStageAnalysisTool(const orc::AnalysisToolInfo& tool) {
  return tool.stage_tool_kind == kStageAnalysisToolKind;
}

/// The parameters @p tool_id of @p stage_name takes; empty when the stage
/// is not registered or offers no such tool.
std::vector<orc::ParameterDescriptor> stageAnalysisToolParameters(
    const std::string& stage_name, const std::string& tool_id);

/// Whether a project refresh may run @p tool_id of @p stage_name alongside
/// other tools (IStageAnalysisTool::analysis_tool_runs_alongside()); false
/// when the stage is not registered or offers no such tool.
bool stageAnalysisToolRunsAlongside(const std::string& stage_name,
                                    const std::string& tool_id);

/// One analysis tool of a node, found from the contract id a view session
/// was opened with.
struct StageAnalysisToolRef {
  std::string stage_name;
  std::string tool_id;
  std::string tool_name;
  std::string description;
};

/// The analysis tool of @p node's stage with @p contract_id, if it has one.
std::optional<StageAnalysisToolRef> findStageAnalysisTool(
    void* project_handle, NodeID node, const std::string& contract_id);

/**
 * @brief One run of a stage's analysis tool
 *
 * Construct on the thread that owns the project (it copies it), run on any
 * other: the run builds a graph of its own from the copy, so the stage
 * object it calls is used by nothing else.
 */
class StageAnalysisRun {
 public:
  using ProgressFn = std::function<void(int percent, const std::string&)>;
  using CancelledFn = std::function<bool()>;

  /// @param parameters The tool's parameters, defaults already filled in
  StageAnalysisRun(const void* project_handle, NodeID node, std::string tool_id,
                   std::map<std::string, orc::ParameterValue> parameters);
  ~StageAnalysisRun();

  StageAnalysisRun(const StageAnalysisRun&) = delete;
  StageAnalysisRun& operator=(const StageAnalysisRun&) = delete;

  /**
   * @brief Run the tool to the end
   *
   * @return The stage's report as the host's analysis result:
   *         parameterChanges holds what applying it writes. Failed with the
   *         reason when the node, its stage or the tool is missing or the
   *         stage throws; Cancelled once @p is_cancelled turns true.
   */
  orc::AnalysisResult run(const ProgressFn& on_progress,
                          const CancelledFn& is_cancelled);

 private:
  std::unique_ptr<orc::Project> project_;
  NodeID node_;
  std::string tool_id_;
  std::map<std::string, orc::ParameterValue> parameters_;
};

/// @p descriptors as an analysis window offers them.
std::vector<orc::view::ToolAnalysisParameter> toolAnalysisParameters(
    const std::vector<orc::ParameterDescriptor>& descriptors);

/// @p result as a view's report; the changes as text, as
/// IStageToolSession::apply_parameters() takes them.
orc::view::ToolAnalysisReport toolAnalysisReport(
    const orc::AnalysisResult& result);

/**
 * @brief A window's parameter text as the tool's typed parameters
 *
 * Checked against @p descriptors, all or nothing; parameters left out take
 * their defaults.
 *
 * @return False with @p error set when a value is refused or unknown
 */
bool toolAnalysisParameterValues(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const orc::view::ToolParameterMap& text,
    std::map<std::string, orc::ParameterValue>* values, std::string* error);

// ---- what a view session of an analysis tool is answered with ----

/// The description and parameters of @p node's analysis tool with
/// @p contract_id; empty when it has none.
orc::view::ToolAnalysisInfo stageAnalysisToolInfo(
    void* project_handle, NodeID node, const std::string& contract_id);

/**
 * @brief Prepare a run of @p node's analysis tool with @p contract_id
 *
 * Call on the thread that owns the project (it is copied here).
 *
 * @return Null with @p why set when the node has no such tool or a
 *         parameter is refused
 */
std::shared_ptr<StageAnalysisRun> prepareStageAnalysisRun(
    void* project_handle, NodeID node, const std::string& contract_id,
    const orc::view::ToolParameterMap& parameters, std::string* why);

/// Whether that tool is @p node's refresh tool.
orc::view::ToolRefreshState stageAnalysisRefreshState(
    void* project_handle, NodeID node, const std::string& contract_id);

/**
 * @brief Make that tool @p node's refresh tool with @p parameters, or stop
 * it being one
 *
 * @return ok == false with the reason when nothing was stored; ok with
 *         data[0] == 1 when the stored block changed (the project then has
 *         unsaved changes)
 */
orc::view::ToolReply setStageAnalysisRefresh(
    void* project_handle, NodeID node, const std::string& contract_id,
    bool use_this_tool, const orc::view::ToolParameterMap& parameters);

}  // namespace orc::presenters
