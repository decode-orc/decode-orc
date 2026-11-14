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
#include <mutex>
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
  /// The earlier targets (indices into the plan) upstream of this node. Its
  /// tool must not run until their results are applied, because it reads
  /// the parameters they write.
  std::vector<size_t> waits_for;
};

/**
 * @brief The nodes to refresh, in the order their tools must run
 *
 * Pipeline order: a node runs after every node upstream of it, so each Frame
 * Map is mapped before the Source Align that reads through it. Nodes the
 * order leaves free run lowest id first. Each target lists the earlier
 * targets upstream of it in RefreshTarget::waits_for.
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

  /// True when @p target's tool may run on its own thread while other such
  /// tools run. The default keeps every tool on the refresh's thread, one
  /// after another.
  virtual bool canRunAlongside(const RefreshTarget& /*target*/) const {
    return false;
  }

  /// Called on the refresh's thread just before runTool(), once the results
  /// the target waits for are applied. Whatever the tool needs from the
  /// project is taken here, so a tool running alongside others never reads
  /// the project while a result is applied to it.
  virtual void prepareTool(const RefreshTarget& /*target*/) {}

  /// Run the node's tool with its stored parameters over the tool defaults.
  /// For a target canRunAlongside() accepts, this runs on its own thread.
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

/// Tools a refresh runs at once, where the backend allows it. A Disc Mapper
/// sweep takes every reader the link can use (the Frame Map plugin's
/// ObservationReaderBudget), so two at once is enough for one capture's
/// mapping to overlap the next capture's reading.
constexpr size_t kRefreshToolsAtOnce = 2;

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
 * @brief Run each target's tool, apply the results in order, and save once
 * at the end
 *
 * A tool runs only once the results of the targets it waits for are
 * applied. Up to kRefreshToolsAtOnce tools that the backend lets run
 * alongside others run at once; the rest run one after another. Results are
 * checked and applied in plan order either way, on the calling thread.
 *
 * Stops at the first node whose tool fails, produces no changes, is refused
 * or is cancelled; the nodes after it are reported as skipped (tools already
 * running are cancelled) and nothing is saved. A dry run applies to the
 * in-memory project only and never saves.
 *
 * @param progress Called with the step index, a percentage and a status,
 *        one step at a time in plan order: a step's reports start once the
 *        steps before it are done. Calls may come from any thread, never two
 *        at once.
 * @param cancel_check May be called from several threads at once.
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

  /// The Disc Mapper: its sweeps share the reader budget, and it reads only
  /// the snapshot prepareTool() takes.
  bool canRunAlongside(const RefreshTarget& target) const override;
  /// Takes a snapshot of the project and builds the tool's presenter on it.
  void prepareTool(const RefreshTarget& target) override;
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
  /// A tool ready to run: its presenter works on its own copy of the
  /// project, taken when the results it waits for were applied.
  struct PreparedTool {
    /// The copied project (opaque, like the handle).
    std::shared_ptr<void> snapshot;
    std::unique_ptr<AnalysisToolPresenter> presenter;
    std::map<std::string, ParameterValue> parameters;
  };

  /// A presenter of @p tool_id on the live project, which prepares and
  /// applies results.
  std::unique_ptr<AnalysisToolPresenter> livePresenter(
      const std::string& tool_id) const;

  void* project_handle_;
  std::function<bool()> save_;
  std::mutex prepared_mutex_;
  std::map<NodeID, PreparedTool> prepared_;
};

}  // namespace presenters
}  // namespace orc
