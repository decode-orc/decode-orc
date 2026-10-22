/*
 * File:        tool_refresh.h
 * Module:      orc-presenters
 * Purpose:     Per-node refresh tools and the `tools refresh` run order
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/node_id.h>
#include <orc/stage/params/parameter_types.h>
#include <orc_analysis.h>

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "project_presenter_types.h"

namespace orc {
namespace presenters {

class AnalysisToolPresenter;

/// The tool a node's `tool_refresh` block names, with the tool parameters to
/// run it with. Parameters left out take the tool's defaults.
struct ToolRefreshConfig {
  std::string tool_id;
  std::map<std::string, ParameterValue> parameters;

  bool operator==(const ToolRefreshConfig& other) const {
    return tool_id == other.tool_id && parameters == other.parameters;
  }
};

/// The refresh tool stored on @p node_id, if any.
std::optional<ToolRefreshConfig> getNodeToolRefresh(void* project_handle,
                                                    NodeID node_id);

/**
 * @brief Store or clear the refresh tool of @p node_id
 *
 * The project is marked modified only when the stored block changes. Check
 * the tool with refreshToolProblem() first; this stores what it is given.
 *
 * @throws std::runtime_error if the node does not exist
 */
void setNodeToolRefresh(void* project_handle, NodeID node_id,
                        const std::optional<ToolRefreshConfig>& config);

/// True when @p tool can be a refresh tool: it runs without the GUI and
/// writes its result to the node.
bool toolCanRefresh(const orc::AnalysisToolInfo& tool);

/**
 * @brief Why @p tool_id cannot be the refresh tool of a stage offering
 * @p stage_tools, or nothing if it can
 */
std::optional<std::string> refreshToolProblem(
    const std::vector<orc::AnalysisToolInfo>& stage_tools,
    const std::string& tool_id);

/// One node `tools refresh` runs a tool for.
struct RefreshTarget {
  NodeInfo node;
  ToolRefreshConfig config;
};

/**
 * @brief The nodes to refresh, in the order their tools must run
 *
 * Pipeline order: a node runs after every node upstream of it, so each Frame
 * Map is mapped before the Source Align that reads through it. Nodes the
 * order leaves free run lowest id first.
 *
 * @param refresh_of The refresh tool of a node, if it has one
 * @param only Tool ids to keep; empty keeps every node
 */
std::vector<RefreshTarget> planToolRefresh(
    const std::vector<NodeInfo>& nodes, const std::vector<EdgeInfo>& edges,
    const std::function<std::optional<ToolRefreshConfig>(NodeID)>& refresh_of,
    const std::vector<std::string>& only);

/// What became of one node in a refresh.
enum class RefreshStepStatus {
  /// The tool ran and its result was applied.
  Success,
  /// The analysis failed.
  Failed,
  /// The analysis succeeded but gave nothing to write to the node.
  NoChanges,
  /// The project refused the tool's changes.
  Rejected,
  /// Stopped by the cancel check.
  Cancelled,
  /// Not run, because an earlier node's tool did not succeed.
  Skipped
};

struct RefreshStepReport {
  RefreshTarget target;
  RefreshStepStatus status = RefreshStepStatus::Skipped;
  orc::AnalysisResult result;
  /// The values the tool wrote (or, failing that, would have written).
  std::map<std::string, ParameterValue> changes;
};

/**
 * @brief The project operations a refresh is built from
 *
 * The seam lets the run order and the all-or-nothing save be tested without
 * real captures.
 */
class ToolRefreshBackend {
 public:
  virtual ~ToolRefreshBackend() = default;

  /// Run the node's tool with its stored parameters over the tool defaults.
  virtual orc::AnalysisResult runTool(
      const RefreshTarget& target,
      const std::function<void(int, const std::string&)>& progress,
      const std::function<bool()>& cancel_check) = 0;

  /// The values applying @p result would write to the node; false if none.
  virtual bool prepareChanges(
      const RefreshTarget& target, const orc::AnalysisResult& result,
      std::map<std::string, ParameterValue>* changes) = 0;

  /// Write @p result to the in-memory project, so the tools after it read
  /// the node's new parameters.
  virtual bool applyChanges(const RefreshTarget& target,
                            const orc::AnalysisResult& result) = 0;

  /// Save the project. Called once, after every tool has succeeded.
  virtual bool save() = 0;
};

struct ToolRefreshOutcome {
  /// One report per target, in run order.
  std::vector<RefreshStepReport> steps;
  /// True when every tool ran and its result was applied.
  bool completed = false;
  bool saved = false;
  /// True when every tool succeeded and saving then failed.
  bool save_failed = false;
};

/**
 * @brief Run each target's tool in order, applying each result before the
 * next tool runs, and save once at the end
 *
 * Stops at the first node whose tool fails, produces no changes, is refused
 * or is cancelled; the nodes after it are reported as skipped and nothing is
 * saved. A dry run applies to the in-memory project only and never saves.
 *
 * @param progress Called with the step index, a percentage and a status
 */
ToolRefreshOutcome runToolRefresh(
    const std::vector<RefreshTarget>& targets, ToolRefreshBackend* backend,
    bool dry_run,
    const std::function<void(size_t, int, const std::string&)>& progress,
    const std::function<bool()>& cancel_check);

/**
 * @brief The backend over a loaded project
 *
 * Tools run through the shared tool presenter factory, with the same
 * parameter defaults as `tools run`. @p save writes the project wherever
 * the caller chooses.
 */
class ProjectToolRefreshBackend : public ToolRefreshBackend {
 public:
  ProjectToolRefreshBackend(void* project_handle, std::function<bool()> save);
  ~ProjectToolRefreshBackend() override;

  orc::AnalysisResult runTool(
      const RefreshTarget& target,
      const std::function<void(int, const std::string&)>& progress,
      const std::function<bool()>& cancel_check) override;
  bool prepareChanges(const RefreshTarget& target,
                      const orc::AnalysisResult& result,
                      std::map<std::string, ParameterValue>* changes) override;
  bool applyChanges(const RefreshTarget& target,
                    const orc::AnalysisResult& result) override;
  bool save() override;

 private:
  void* project_handle_;
  std::function<bool()> save_;
  /// The presenter of the tool last run, which prepares and applies its
  /// result.
  std::unique_ptr<AnalysisToolPresenter> tool_presenter_;
};

}  // namespace presenters
}  // namespace orc
