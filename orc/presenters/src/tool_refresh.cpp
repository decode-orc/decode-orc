/*
 * File:        tool_refresh.cpp
 * Module:      orc-presenters
 * Purpose:     Per-node refresh tools and the `tools refresh` run order
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "tool_refresh.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <utility>

#include "../core/include/project.h"
#include "analysis_presenter.h"
#include "analysis_tool_presenter.h"
#include "analysis_tool_presenter_factory.h"
#include "tool_running.h"

namespace orc {
namespace presenters {

std::optional<ToolRefreshConfig> getNodeToolRefresh(void* project_handle,
                                                    NodeID node_id) {
  const auto* project = static_cast<const orc::Project*>(project_handle);
  if (!project) {
    return std::nullopt;
  }
  for (const auto& node : project->get_nodes()) {
    if (node.node_id == node_id && node.tool_refresh) {
      return ToolRefreshConfig{node.tool_refresh->tool_id,
                               node.tool_refresh->parameters};
    }
  }
  return std::nullopt;
}

void setNodeToolRefresh(void* project_handle, NodeID node_id,
                        const std::optional<ToolRefreshConfig>& config) {
  auto* project = static_cast<orc::Project*>(project_handle);
  if (!project) {
    throw std::runtime_error("No project");
  }
  std::optional<orc::NodeToolRefresh> refresh;
  if (config) {
    refresh = orc::NodeToolRefresh{config->tool_id, config->parameters};
  }
  orc::project_io::set_node_tool_refresh(*project, node_id, refresh);
}

bool toolCanRefresh(const orc::AnalysisToolInfo& tool) {
  return toolRunSupport(tool).mode == ToolRunMode::Analysis &&
         toolCanApplyToGraph(tool.id);
}

std::optional<std::string> refreshToolProblem(
    const std::vector<orc::AnalysisToolInfo>& stage_tools,
    const std::string& tool_id) {
  const auto it = std::find_if(
      stage_tools.begin(), stage_tools.end(),
      [&tool_id](const orc::AnalysisToolInfo& t) { return t.id == tool_id; });
  if (it == stage_tools.end()) {
    return "'" + tool_id + "' is not one of this stage's tools";
  }
  if (!toolCanRefresh(*it)) {
    return "'" + tool_id +
           "' does not write its result to the stage, so a refresh has "
           "nothing to apply";
  }
  return std::nullopt;
}

std::vector<RefreshTarget> planToolRefresh(
    const std::vector<NodeInfo>& nodes, const std::vector<EdgeInfo>& edges,
    const std::function<std::optional<ToolRefreshConfig>(NodeID)>& refresh_of,
    const std::vector<std::string>& only) {
  // Kahn's algorithm over the whole graph, taking the lowest ready id each
  // time, so the order is the same on every run.
  std::map<NodeID, size_t> pending_inputs;
  for (const auto& node : nodes) {
    pending_inputs[node.node_id] = 0;
  }
  for (const auto& edge : edges) {
    if (pending_inputs.count(edge.source_node) != 0 &&
        pending_inputs.count(edge.target_node) != 0) {
      ++pending_inputs[edge.target_node];
    }
  }
  std::set<NodeID> ready;
  for (const auto& [id, count] : pending_inputs) {
    if (count == 0) {
      ready.insert(id);
    }
  }

  std::vector<RefreshTarget> targets;
  while (!ready.empty()) {
    const NodeID id = *ready.begin();
    ready.erase(ready.begin());
    for (const auto& edge : edges) {
      if (edge.source_node == id && pending_inputs.count(edge.target_node) &&
          --pending_inputs[edge.target_node] == 0) {
        ready.insert(edge.target_node);
      }
    }

    const auto config = refresh_of(id);
    if (!config) {
      continue;
    }
    if (!only.empty() &&
        std::find(only.begin(), only.end(), config->tool_id) == only.end()) {
      continue;
    }
    const auto node_it =
        std::find_if(nodes.begin(), nodes.end(),
                     [&id](const NodeInfo& n) { return n.node_id == id; });
    targets.push_back({*node_it, *config});
  }
  return targets;
}

ToolRefreshOutcome runToolRefresh(
    const std::vector<RefreshTarget>& targets, ToolRefreshBackend* backend,
    bool dry_run,
    const std::function<void(size_t, int, const std::string&)>& progress,
    const std::function<bool()>& cancel_check) {
  ToolRefreshOutcome outcome;
  for (const auto& target : targets) {
    outcome.steps.push_back({target, RefreshStepStatus::Skipped, {}, {}});
  }

  for (size_t i = 0; i < outcome.steps.size(); ++i) {
    auto& step = outcome.steps[i];
    const auto step_progress = [&progress, i](int percent,
                                              const std::string& status) {
      if (progress) {
        progress(i, percent, status);
      }
    };

    step.result = backend->runTool(step.target, step_progress, cancel_check);
    if (step.result.status == orc::AnalysisResult::Status::Cancelled ||
        (cancel_check && cancel_check())) {
      step.status = RefreshStepStatus::Cancelled;
      return outcome;
    }
    if (step.result.status != orc::AnalysisResult::Status::Success) {
      step.status = RefreshStepStatus::Failed;
      return outcome;
    }
    // A tool that writes parameters and produced none must stop the batch:
    // a disc that could not be mapped is never stacked silently.
    if (!backend->prepareChanges(step.target, step.result, &step.changes) ||
        step.changes.empty()) {
      step.status = RefreshStepStatus::NoChanges;
      return outcome;
    }
    if (!backend->applyChanges(step.target, step.result)) {
      step.status = RefreshStepStatus::Rejected;
      return outcome;
    }
    step.status = RefreshStepStatus::Success;
  }

  outcome.completed = true;
  if (!dry_run) {
    outcome.saved = backend->save();
    outcome.save_failed = !outcome.saved;
  }
  return outcome;
}

ProjectToolRefreshBackend::ProjectToolRefreshBackend(void* project_handle,
                                                     std::function<bool()> save)
    : project_handle_(project_handle), save_(std::move(save)) {}

ProjectToolRefreshBackend::~ProjectToolRefreshBackend() = default;

orc::AnalysisResult ProjectToolRefreshBackend::runTool(
    const RefreshTarget& target,
    const std::function<void(int, const std::string&)>& progress,
    const std::function<bool()>& cancel_check) {
  AnalysisPresenter analysis(project_handle_);
  const auto descriptors = analysis.getToolParameters(
      target.config.tool_id, orc::AnalysisSourceType::LaserDisc);
  const auto parameters =
      withParameterDefaults(descriptors, target.config.parameters);

  tool_presenter_ =
      makeAnalysisToolPresenter(target.config.tool_id, project_handle_);
  return tool_presenter_->runAnalysis(target.node.node_id, parameters, progress,
                                      cancel_check);
}

bool ProjectToolRefreshBackend::prepareChanges(
    const RefreshTarget& target, const orc::AnalysisResult& result,
    std::map<std::string, ParameterValue>* changes) {
  return tool_presenter_ && tool_presenter_->prepareResultChanges(
                                result, target.node.node_id, changes);
}

bool ProjectToolRefreshBackend::applyChanges(
    const RefreshTarget& target, const orc::AnalysisResult& result) {
  return tool_presenter_ &&
         tool_presenter_->applyResultToGraph(result, target.node.node_id);
}

bool ProjectToolRefreshBackend::save() { return save_ && save_(); }

}  // namespace presenters
}  // namespace orc
