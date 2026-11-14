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
#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

#include "../core/include/project.h"
#include "analysis_presenter.h"
#include "analysis_tool_presenter.h"
#include "stage_analysis_tools.h"
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
         toolCanApplyToGraph(tool);
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

  // Every node upstream of each node, filled in run order: a node's inputs
  // are visited before it.
  std::map<NodeID, std::set<NodeID>> upstream;
  std::map<NodeID, size_t> target_index;

  std::vector<RefreshTarget> targets;
  while (!ready.empty()) {
    const NodeID id = *ready.begin();
    ready.erase(ready.begin());
    auto& above = upstream[id];
    for (const auto& edge : edges) {
      if (edge.target_node == id && pending_inputs.count(edge.source_node)) {
        above.insert(edge.source_node);
        const auto& further = upstream[edge.source_node];
        above.insert(further.begin(), further.end());
      }
    }
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
    RefreshTarget target{*node_it, *config, {}};
    for (const NodeID above_id : above) {
      const auto it = target_index.find(above_id);
      if (it != target_index.end()) {
        target.waits_for.push_back(it->second);
      }
    }
    std::sort(target.waits_for.begin(), target.waits_for.end());
    target_index[id] = targets.size();
    targets.push_back(std::move(target));
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
  const size_t count = targets.size();

  // Progress passes one step at a time, in plan order. A step running ahead
  // keeps only its latest report, passed on when the steps before it are
  // done.
  std::mutex progress_mutex;
  size_t progress_front = 0;
  std::vector<std::optional<std::pair<int, std::string>>> held(count);
  const auto report = [&](size_t step, int percent, const std::string& status) {
    if (!progress) return;
    std::lock_guard<std::mutex> lock(progress_mutex);
    if (step == progress_front) {
      progress(step, percent, status);
    } else {
      held[step] = std::make_pair(percent, status);
    }
  };
  const auto move_progress_to = [&](size_t step) {
    if (!progress) return;
    std::lock_guard<std::mutex> lock(progress_mutex);
    progress_front = step;
    if (step < count && held[step]) {
      progress(step, held[step]->first, held[step]->second);
    }
  };
  const auto step_progress = [&report](size_t step) {
    return [&report, step](int percent, const std::string& status) {
      report(step, percent, status);
    };
  };

  // Tools running on their own threads are stopped once the refresh stops.
  std::atomic<bool> stopping{false};
  const std::function<bool()> tool_cancel = [&stopping, &cancel_check] {
    return stopping.load() || (cancel_check && cancel_check());
  };

  // A step's tool may start once the steps it waits for are applied.
  const auto can_start = [&targets](size_t step, size_t applied) {
    const auto& waits = targets[step].waits_for;
    return std::all_of(waits.begin(), waits.end(),
                       [applied](size_t w) { return w < applied; });
  };

  // Declared last, so leaving early joins the tools before what they use
  // goes away.
  std::vector<std::future<orc::AnalysisResult>> running(count);
  size_t next = 0;
  size_t alongside = 0;
  const auto stop_running = [&stopping] { stopping.store(true); };

  for (size_t i = 0; i < count; ++i) {
    // Start, in plan order, the tools that may run alongside others.
    while (next < count && alongside < kRefreshToolsAtOnce &&
           backend->canRunAlongside(targets[next]) && can_start(next, i)) {
      backend->prepareTool(targets[next]);
      const size_t step = next;
      running[step] =
          std::async(std::launch::async,
                     [backend, &targets, step, &step_progress, &tool_cancel] {
                       return backend->runTool(
                           targets[step], step_progress(step), tool_cancel);
                     });
      ++alongside;
      ++next;
    }

    auto& step = outcome.steps[i];
    if (running[i].valid()) {
      try {
        step.result = running[i].get();
      } catch (...) {
        stop_running();
        throw;
      }
      --alongside;
    } else {
      // Nothing is running: every step before this one is done, and none
      // after it has started.
      backend->prepareTool(targets[i]);
      step.result =
          backend->runTool(step.target, step_progress(i), cancel_check);
      ++next;
    }

    if (step.result.status == orc::AnalysisResult::Status::Cancelled ||
        (cancel_check && cancel_check())) {
      step.status = RefreshStepStatus::Cancelled;
      stop_running();
      return outcome;
    }
    if (step.result.status != orc::AnalysisResult::Status::Success) {
      step.status = RefreshStepStatus::Failed;
      stop_running();
      return outcome;
    }
    // A tool that writes parameters and produced none must stop the batch:
    // a disc that could not be mapped is never stacked silently.
    if (!backend->prepareChanges(step.target, step.result, &step.changes) ||
        step.changes.empty()) {
      step.status = RefreshStepStatus::NoChanges;
      stop_running();
      return outcome;
    }
    if (!backend->applyChanges(step.target, step.result)) {
      step.status = RefreshStepStatus::Rejected;
      stop_running();
      return outcome;
    }
    step.status = RefreshStepStatus::Success;
    move_progress_to(i + 1);
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

bool ProjectToolRefreshBackend::canRunAlongside(
    const RefreshTarget& target) const {
  return stageAnalysisToolRunsAlongside(target.node.stage_name,
                                        target.config.tool_id);
}

void ProjectToolRefreshBackend::prepareTool(const RefreshTarget& target) {
  AnalysisPresenter analysis(project_handle_);
  const auto descriptors =
      analysis.getToolParameters(target.config.tool_id, target.node.stage_name);

  PreparedTool tool;
  tool.parameters =
      withParameterDefaults(descriptors, target.config.parameters);
  void* handle = project_handle_;
  if (canRunAlongside(target)) {
    auto snapshot = std::make_shared<orc::Project>(
        *static_cast<const orc::Project*>(project_handle_));
    handle = snapshot.get();
    tool.snapshot = std::move(snapshot);
  }
  tool.presenter =
      std::make_unique<AnalysisToolPresenter>(target.config.tool_id, handle);

  std::lock_guard<std::mutex> lock(prepared_mutex_);
  prepared_[target.node.node_id] = std::move(tool);
}

orc::AnalysisResult ProjectToolRefreshBackend::runTool(
    const RefreshTarget& target,
    const std::function<void(int, const std::string&)>& progress,
    const std::function<bool()>& cancel_check) {
  PreparedTool tool;
  {
    std::unique_lock<std::mutex> lock(prepared_mutex_);
    auto it = prepared_.find(target.node.node_id);
    if (it == prepared_.end()) {
      lock.unlock();
      prepareTool(target);
      lock.lock();
      it = prepared_.find(target.node.node_id);
    }
    tool = std::move(it->second);
    prepared_.erase(it);
  }
  // The tool, its DAG and the snapshot go when the run ends; the result is
  // prepared and applied through the live project.
  return tool.presenter->runAnalysis(target.node.node_id, tool.parameters,
                                     progress, cancel_check);
}

std::unique_ptr<AnalysisToolPresenter> ProjectToolRefreshBackend::livePresenter(
    const std::string& tool_id) const {
  return std::make_unique<AnalysisToolPresenter>(tool_id, project_handle_);
}

bool ProjectToolRefreshBackend::prepareChanges(
    const RefreshTarget& target, const orc::AnalysisResult& result,
    std::map<std::string, ParameterValue>* changes) {
  const auto presenter = livePresenter(target.config.tool_id);
  return presenter &&
         presenter->prepareResultChanges(result, target.node.node_id, changes);
}

bool ProjectToolRefreshBackend::applyChanges(
    const RefreshTarget& target, const orc::AnalysisResult& result) {
  const auto presenter = livePresenter(target.config.tool_id);
  return presenter &&
         presenter->applyResultToGraph(result, target.node.node_id);
}

bool ProjectToolRefreshBackend::save() { return save_ && save_(); }

}  // namespace presenters
}  // namespace orc
