/*
 * File:        analysis_sink_runner.cpp
 * Module:      orc-presenters
 * Purpose:     Trigger an analysis sink and keep its per-frame results
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "analysis_sink_runner.h"

#include <orc/stage/tooling/stage_results_report.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

#include "../core/include/dag_executor.h"
#include "../core/include/dag_sink_runner.h"
#include "../core/include/project.h"
#include "../core/include/project_to_dag.h"

namespace orc {
namespace presenters {

namespace {

// Reads the report an analysis sink's stage kept from its last trigger.
void read_results(orc::DAGStage* stage, AnalysisSinkRun& run) {
  const auto* provider =
      dynamic_cast<const orc::IStageResultsReportProvider*>(stage);
  orc::StageResultsReport report;
  if (provider && provider->results_report(&report)) {
    run.report = std::move(report);
  }
}

}  // namespace

AnalysisSinkRun runAnalysisSink(
    void* project_handle, NodeID node_id,
    const std::map<std::string, ParameterValue>& parameter_overrides,
    const std::function<void(int, const std::string&)>& progress_callback,
    const std::function<bool()>& cancel_check) {
  std::function<void(size_t, int, const std::string&)> progress;
  if (progress_callback) {
    progress = [&progress_callback](size_t, int percent,
                                    const std::string& message) {
      progress_callback(percent, message);
    };
  }
  auto runs = runAnalysisSinks(project_handle, {{node_id, parameter_overrides}},
                               progress, cancel_check);
  return runs.front();
}

std::vector<AnalysisSinkRun> runAnalysisSinks(
    void* project_handle, const std::vector<AnalysisSinkRequest>& requests,
    const std::function<void(size_t, int, const std::string&)>&
        progress_callback,
    const std::function<bool()>& cancel_check) {
  std::vector<AnalysisSinkRun> runs(requests.size());
  auto* project = static_cast<orc::Project*>(project_handle);
  if (!project) {
    for (auto& run : runs) run.status = "No project";
    return runs;
  }

  try {
    // The DAG's nodes carry the stage instances the results are read from
    // afterwards, and parameters with project-relative paths resolved.
    auto dag = orc::project_to_dag(*project);
    std::map<NodeID, orc::DAGStage*> stages;
    for (const auto& node : dag->nodes()) {
      stages.emplace(node.node_id, node.stage.get());
    }

    std::mutex progress_mutex;
    std::vector<orc::SinkRunRequest> sink_requests;
    std::vector<std::atomic<bool>> cancelled(requests.size());
    for (size_t i = 0; i < requests.size(); ++i) {
      orc::SinkRunRequest request;
      request.node_id = requests[i].node_id;
      request.parameter_overrides = requests[i].parameter_overrides;
      request.with_executor_observations = true;
      const auto stage = stages.find(request.node_id);
      auto* trigger_stage =
          stage == stages.end()
              ? nullptr
              : dynamic_cast<orc::TriggerableStage*>(stage->second);
      request.progress = [&, i, trigger_stage](size_t current, size_t total,
                                               const std::string& message) {
        if (cancel_check && cancel_check() && trigger_stage) {
          cancelled[i] = true;
          trigger_stage->cancel_trigger();
        }
        if (progress_callback) {
          const int percent =
              total > 0 ? static_cast<int>((current * 100) / total) : 0;
          std::lock_guard<std::mutex> lock(progress_mutex);
          progress_callback(i, percent, message);
        }
      };
      sink_requests.push_back(std::move(request));
    }

    // Kept alive until the triggers finish: the input artifacts hold raw
    // pointers to stages the executor owns.
    orc::DAGExecutor executor;
    const auto results = orc::run_sinks(*dag, executor, sink_requests);
    for (size_t i = 0; i < requests.size(); ++i) {
      auto& run = runs[i];
      run.success = results[i].success;
      run.status = results[i].status;
      run.cancelled = cancelled[i];
      if (run.cancelled) {
        run.success = false;
      }
      if (run.success) {
        read_results(stages[requests[i].node_id], run);
      }
    }
  } catch (const std::exception& e) {
    for (auto& run : runs) {
      run.success = false;
      run.status = e.what();
    }
    ORC_LOG_ERROR("Analysis sink run failed: {}", e.what());
  }
  return runs;
}

}  // namespace presenters
}  // namespace orc
