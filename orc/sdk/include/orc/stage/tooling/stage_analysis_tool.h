/*
 * File:        stage_analysis_tool.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     A stage's analysis tools: measure the node's inputs and
 *              propose parameter values, run by orc-cli and orc-gui alike
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_TOOLING_STAGE_ANALYSIS_TOOL_H
#define ORC_STAGE_TOOLING_STAGE_ANALYSIS_TOOL_H

// SDK TIER: stage/tooling — stage contract type crossing the plugin boundary.
// A layout change here bumps the host ABI version.

#include <orc/stage/params/parameter_types.h>
#include <orc/stage/tooling/stage_tool_jobs.h>

#include <cstdint>
#include <map>
#include <string>
#include <variant>
#include <vector>

namespace orc {

/// One figure of a report: a flag, a count, a measurement or a name.
using StageAnalysisValue = std::variant<bool, int64_t, double, std::string>;

/// One finding of a report (a skipped frame, a warning, a point of a sweep).
struct StageAnalysisItem {
  /// What kind of finding it is, named by the tool ("skip", "warning", ...).
  std::string type;
  /// The finding for the user.
  std::string message;
  /// The frames it covers, 0-based; -1 when it covers none.
  int64_t start_frame = -1;
  int64_t end_frame = -1;
  /// Figures of the finding, named by the tool.
  std::map<std::string, StageAnalysisValue> data;
};

/// What one run of an analysis tool found.
struct StageAnalysisReport {
  enum class Status { Success, Failed, Cancelled };

  Status status = Status::Failed;
  /// What was found, for the user; on failure, why. Plain text, lines
  /// separated by '\n'.
  std::string summary;
  /// Figures of the run as a whole, named by the tool. orc-cli prints them
  /// (`tools run --json` under "statistics").
  std::map<std::string, StageAnalysisValue> statistics;
  /// Findings, in the order the tool wants them listed.
  std::vector<StageAnalysisItem> items;
  /// The node parameters the result sets, by the names the stage declares
  /// them under, with their declared types. Applying the result writes
  /// exactly these over the node's stored values and leaves the others.
  /// Empty when a successful run found nothing to set (a disc with no VBI,
  /// say); the host then tells the user there is nothing to apply.
  std::map<std::string, ParameterValue> parameter_changes;
};

/**
 * @brief Run the stage's analysis tools
 *
 * An analysis tool reads the node's inputs, reports what it found and
 * proposes values for the node's own parameters (a carrier frequency, a frame
 * map). The stage lists each one in get_stage_tools() as
 * StageToolKind::Analysis and implements this beside it. The host shows the
 * tool's parameters, runs it, shows the report and, if the user (or
 * `orc-cli tools run --apply`, or `tools refresh`) applies it, writes
 * parameter_changes to the node. orc-gui shows the host's analysis window,
 * or a window a view binary registered for the tool's contract id.
 *
 * The host reaches this by dynamic_cast, as it does IStageToolJobHandler.
 *
 * Threading: analysis_tool_parameters() is called on any thread, on any
 * object of the stage (one not configured as a node, say), and must not
 * read the stage's members. run_analysis_tool() is called as a tool job is
 * (see IStageToolJobHandler): on a worker thread, on an object of its own
 * built from the project as it stood when the run started.
 *
 * Exceptions thrown from run_analysis_tool() are caught and become a Failed
 * report saying what was thrown.
 */
class IStageAnalysisTool {
 public:
  virtual ~IStageAnalysisTool() = default;

  /// The parameters @p tool_id takes, with their defaults. Empty for a tool
  /// with none, or for a tool id this stage does not offer.
  virtual std::vector<ParameterDescriptor> analysis_tool_parameters(
      const std::string& tool_id) const = 0;

  /**
   * @brief Run @p tool_id on the node's inputs
   *
   * @param parameters Every parameter analysis_tool_parameters() declares,
   *        defaults filled in for those the user left out
   * @param context The node's inputs, progress and cancel; when
   *        is_cancelled() turns true, return soon with Status::Cancelled
   */
  virtual StageAnalysisReport run_analysis_tool(
      const std::string& tool_id,
      const std::map<std::string, ParameterValue>& parameters,
      IToolJobContext& context) = 0;

  /**
   * @brief Whether a project refresh may run @p tool_id alongside others
   *
   * True lets a refresh run this tool on its own thread while other such
   * tools run, each on its own copy of the project: worth it for a tool
   * that spends its time reading a capture, such as the Disc Mapper, which
   * one refresh runs once per disc of a stack. False, the default, runs
   * the tool on the refresh's thread, one tool after another.
   */
  virtual bool analysis_tool_runs_alongside(const std::string& tool_id) const {
    (void)tool_id;
    return false;
  }
};

}  // namespace orc

#endif  // ORC_STAGE_TOOLING_STAGE_ANALYSIS_TOOL_H
