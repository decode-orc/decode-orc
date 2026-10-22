/*
 * File:        analysis_sink_runner.cpp
 * Module:      orc-presenters
 * Purpose:     Trigger an analysis sink and keep its per-frame results
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "analysis_sink_runner.h"

#include <orc/stage/analysis_sink_results.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

#include "../core/include/dag_executor.h"
#include "../core/include/project.h"
#include "../core/include/project_to_dag.h"

namespace orc {
namespace presenters {

AnalysisSinkRun runAnalysisSink(
    void* project_handle, NodeID node_id,
    const std::map<std::string, ParameterValue>& parameter_overrides,
    const std::function<void(int, const std::string&)>& progress_callback,
    const std::function<bool()>& cancel_check) {
  AnalysisSinkRun run;
  auto* project = static_cast<orc::Project*>(project_handle);
  if (!project) {
    run.status = "No project";
    return run;
  }

  try {
    // The DAG's node carries the stage instance the results are read from
    // afterwards, and parameters with project-relative paths resolved.
    // Core's trigger_node() makes and discards its own instance.
    auto dag = orc::project_to_dag(*project);
    const auto& nodes = dag->nodes();
    const auto node_it = std::find_if(
        nodes.begin(), nodes.end(),
        [&node_id](const orc::DAGNode& n) { return n.node_id == node_id; });
    if (node_it == nodes.end()) {
      run.status = "Node " + node_id.to_string() + " not found";
      return run;
    }
    auto* trigger_stage =
        dynamic_cast<orc::TriggerableStage*>(node_it->stage.get());
    if (!trigger_stage) {
      run.status = "Stage is not triggerable";
      return run;
    }

    // Kept alive until the trigger finishes: the input artifacts hold raw
    // pointers to stages the executor owns.
    auto executor = std::make_shared<orc::DAGExecutor>();
    std::vector<orc::ArtifactPtr> inputs;
    for (size_t i = 0; i < node_it->input_node_ids.size(); ++i) {
      const auto& input_id = node_it->input_node_ids[i];
      const size_t index =
          i < node_it->input_indices.size() ? node_it->input_indices[i] : 0;
      auto outputs = executor->execute_to_node(*dag, input_id);
      const auto it = outputs.find(input_id);
      if (it != outputs.end() && index < it->second.size()) {
        inputs.push_back(it->second[index]);
      }
    }
    if (inputs.empty()) {
      run.status = "No inputs available";
      return run;
    }

    trigger_stage->set_progress_callback(
        [&](size_t current, size_t total, const std::string& message) {
          if (cancel_check && cancel_check()) {
            run.cancelled = true;
            trigger_stage->cancel_trigger();
          }
          if (progress_callback) {
            const int percent =
                total > 0 ? static_cast<int>((current * 100) / total) : 0;
            progress_callback(percent, message);
          }
        });

    auto parameters = node_it->parameters;
    for (const auto& [key, value] : parameter_overrides) {
      parameters[key] = value;
    }
    run.success = trigger_stage->trigger(inputs, parameters,
                                         executor->get_observation_context());
    trigger_stage->set_progress_callback(nullptr);
    run.status = trigger_stage->get_trigger_status();
    if (run.cancelled) {
      run.success = false;
    }
    if (!run.success) {
      return run;
    }

    auto* stage = node_it->stage.get();
    if (auto* sink = dynamic_cast<orc::IDropoutAnalysisResults*>(stage);
        sink && sink->has_results()) {
      run.kind = AnalysisSinkKind::Dropout;
      run.dropout = sink->frame_stats();
      run.total_frames = sink->total_frames();
    } else if (auto* sink = dynamic_cast<orc::ISNRAnalysisResults*>(stage);
               sink && sink->has_results()) {
      run.kind = AnalysisSinkKind::SNR;
      run.snr = sink->frame_stats();
      run.total_frames = sink->total_frames();
    } else if (auto* sink =
                   dynamic_cast<orc::IBurstLevelAnalysisResults*>(stage);
               sink && sink->has_results()) {
      run.kind = AnalysisSinkKind::BurstLevel;
      run.burst_level = sink->frame_stats();
      run.total_frames = sink->total_frames();
    }
  } catch (const std::exception& e) {
    run.success = false;
    run.status = e.what();
    ORC_LOG_ERROR("Analysis sink run failed: {}", e.what());
  }
  return run;
}

}  // namespace presenters
}  // namespace orc
