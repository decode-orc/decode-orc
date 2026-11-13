/*
 * File:        dag_sink_runner.cpp
 * Module:      orc-core
 * Purpose:     Trigger several sinks of one DAG, sharing everything upstream
 *              of them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "dag_sink_runner.h"

#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_cache.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <memory>
#include <set>
#include <stdexcept>
#include <thread>

#include "sink_read_pacer.h"

namespace orc {

namespace {

// How far past the slowest of several sinks reading one input a sink may
// read. Small against the caches (frame_cache_capacity_for()), which must hold
// one sink's frames plus this.
constexpr size_t kSinkPaceFrames = 16;

// Every node @p node_id reads from, directly or through others.
std::set<NodeID> upstream_of(const std::map<NodeID, const DAGNode*>& by_id,
                             const NodeID& node_id) {
  std::set<NodeID> seen;
  std::vector<NodeID> pending{node_id};
  while (!pending.empty()) {
    const NodeID current = pending.back();
    pending.pop_back();
    const auto it = by_id.find(current);
    if (it == by_id.end()) continue;
    for (const auto& input : it->second->input_node_ids) {
      if (seen.insert(input).second) pending.push_back(input);
    }
  }
  return seen;
}

struct PreparedSink {
  size_t index = 0;  // into the requests
  const DAGNode* node = nullptr;
  TriggerableStage* stage = nullptr;
  std::vector<ArtifactPtr> inputs;
  std::set<NodeID> upstream;
  std::shared_ptr<PacedFrameRepresentation> paced;
};

// Gives the sinks of one group that read the same node output each a paced
// view of it (see SinkReadPacer).
void pace_shared_inputs(const std::vector<PreparedSink*>& group) {
  std::map<const Artifact*, std::vector<PreparedSink*>> by_input;
  for (auto* sink : group) {
    if (sink->inputs.size() == 1) {
      by_input[sink->inputs[0].get()].push_back(sink);
    }
  }
  for (auto& [artifact, readers] : by_input) {
    if (readers.size() < 2) continue;
    auto source = std::dynamic_pointer_cast<const VideoFrameRepresentation>(
        readers.front()->inputs[0]);
    if (!source) continue;
    // A read further back than the caches hold starts a new pass.
    const size_t rewind =
        std::min(source->max_concurrent_frame_requests(),
                 frame_cache_capacity_for(kMaxAutoFrameWorkers));
    auto pacer = std::make_shared<SinkReadPacer>(source->frame_range().first,
                                                 kSinkPaceFrames, rewind);
    for (auto* sink : readers) {
      sink->paced =
          std::make_shared<PacedFrameRepresentation>(source, *artifact, pacer);
      sink->inputs[0] = sink->paced;
    }
  }
}

// The sinks in groups that share no upstream node, each group in request
// order, the groups in the order of their first request.
std::vector<std::vector<PreparedSink*>> group_by_upstream(
    std::vector<PreparedSink>& sinks) {
  std::vector<std::vector<PreparedSink*>> groups;
  for (auto& sink : sinks) {
    std::vector<PreparedSink*> merged{&sink};
    std::vector<std::vector<PreparedSink*>> kept;
    for (auto& group : groups) {
      const bool shares = std::any_of(
          group.begin(), group.end(), [&sink](const PreparedSink* other) {
            return std::any_of(sink.upstream.begin(), sink.upstream.end(),
                               [other](const NodeID& id) {
                                 return other->upstream.count(id) > 0;
                               });
          });
      if (shares) {
        merged.insert(merged.end(), group.begin(), group.end());
      } else {
        kept.push_back(std::move(group));
      }
    }
    kept.push_back(std::move(merged));
    groups = std::move(kept);
  }
  const auto by_index = [](const PreparedSink* a, const PreparedSink* b) {
    return a->index < b->index;
  };
  for (auto& group : groups) {
    std::sort(group.begin(), group.end(), by_index);
  }
  std::sort(groups.begin(), groups.end(),
            [&by_index](const std::vector<PreparedSink*>& a,
                        const std::vector<PreparedSink*>& b) {
              return by_index(a.front(), b.front());
            });
  return groups;
}

}  // namespace

std::vector<SinkRunResult> run_sinks(
    const DAG& dag, DAGExecutor& executor,
    const std::vector<SinkRunRequest>& requests) {
  std::vector<SinkRunResult> results;
  for (const auto& request : requests) {
    results.push_back({request.node_id, false, ""});
  }

  std::map<NodeID, const DAGNode*> by_id;
  for (const auto& node : dag.nodes()) by_id.emplace(node.node_id, &node);

  // Executed one sink at a time on this thread, since a stage instance must
  // not execute on two threads at once.
  std::vector<PreparedSink> sinks;
  for (size_t i = 0; i < requests.size(); ++i) {
    auto& result = results[i];
    const auto it = by_id.find(requests[i].node_id);
    if (it == by_id.end()) {
      result.status = "Node not found";
      continue;
    }
    auto* stage = dynamic_cast<TriggerableStage*>(it->second->stage.get());
    if (!stage) {
      result.status = "Stage is not triggerable";
      continue;
    }
    PreparedSink sink;
    sink.index = i;
    sink.node = it->second;
    sink.stage = stage;
    try {
      const auto& node = *it->second;
      for (size_t k = 0; k < node.input_node_ids.size(); ++k) {
        const auto& input_id = node.input_node_ids[k];
        const size_t output =
            k < node.input_indices.size() ? node.input_indices[k] : 0;
        auto outputs = executor.execute_to_node(dag, input_id);
        const auto out = outputs.find(input_id);
        if (out != outputs.end() && output < out->second.size()) {
          sink.inputs.push_back(out->second[output]);
        }
      }
    } catch (const std::exception& e) {
      result.status = e.what();
      ORC_LOG_ERROR("Preparing the inputs of node {} failed: {}",
                    requests[i].node_id, e.what());
      continue;
    }
    if (sink.inputs.empty()) {
      result.status = "No inputs available";
      continue;
    }
    sink.upstream = upstream_of(by_id, sink.node->node_id);
    sinks.push_back(std::move(sink));
  }

  const auto run = [&](PreparedSink* sink) {
    const auto& request = requests[sink->index];
    auto& result = results[sink->index];
    try {
      if (request.progress) {
        sink->stage->set_progress_callback(request.progress);
      }
      auto parameters = sink->node->parameters;
      for (const auto& [key, value] : request.parameter_overrides) {
        parameters[key] = value;
      }
      ObservationContext observation_context =
          request.with_executor_observations
              ? executor.get_observation_context()
              : ObservationContext{};
      result.success =
          sink->stage->trigger(sink->inputs, parameters, observation_context);
      result.status = sink->stage->get_trigger_status();
    } catch (const std::exception& e) {
      result.success = false;
      result.status = e.what();
    } catch (...) {
      result.success = false;
      result.status = "Unknown error";
    }
    if (request.progress) {
      sink->stage->set_progress_callback(nullptr);
    }
    // Finished, failed or cancelled: it no longer holds back the others.
    if (sink->paced) sink->paced->detach();
  };

  for (const auto& group : group_by_upstream(sinks)) {
    if (group.size() == 1) {
      run(group.front());
      continue;
    }
    pace_shared_inputs(group);
    ORC_LOG_INFO("Running {} sinks side by side on shared inputs",
                 group.size());
    std::vector<std::thread> threads;
    threads.reserve(group.size());
    for (auto* sink : group) {
      threads.emplace_back(run, sink);
    }
    for (auto& thread : threads) thread.join();
  }
  return results;
}

}  // namespace orc
