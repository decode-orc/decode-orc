/*
 * File:        dag_executor.cpp
 * Module:      orc-core
 * Purpose:     DAG execution engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "dag_executor.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <queue>
#include <set>
#include <sstream>

#include "include/frame_provenance.h"
#include "include/node_measurement_store.h"
#include "include/pipeline_validator.h"
#include "include/shared_node_outputs.h"
#include "include/stage_registry.h"

namespace orc {

// ============================================================================
// DAG cloning
// ============================================================================

std::shared_ptr<DAG> clone_dag_with_fresh_stages(const DAG& dag) {
  auto& registry = StageRegistry::instance();
  auto clone = std::make_shared<DAG>();

  for (const auto& node : dag.nodes()) {
    if (!node.stage) {
      ORC_LOG_ERROR("clone_dag: node '{}' has no stage", node.node_id);
      return nullptr;
    }
    const std::string stage_name = node.stage->get_node_type_info().stage_name;
    if (!registry.has_stage(stage_name)) {
      ORC_LOG_ERROR("clone_dag: stage '{}' of node '{}' is not registered",
                    stage_name, node.node_id);
      return nullptr;
    }

    DAGNode copy;
    copy.node_id = node.node_id;
    copy.parameters = node.parameters;
    copy.input_node_ids = node.input_node_ids;
    copy.input_indices = node.input_indices;
    copy.stage = registry.create_stage(stage_name);
    if (!copy.stage) {
      ORC_LOG_ERROR("clone_dag: could not create stage '{}' for node '{}'",
                    stage_name, node.node_id);
      return nullptr;
    }

    // The parameter map is already the effective one — descriptor defaults
    // overlaid with the project's stored values, paths resolved — so the fresh
    // stage is configured exactly as the original was.
    auto* parameterized = dynamic_cast<ParameterizedStage*>(copy.stage.get());
    if (parameterized && !copy.parameters.empty() &&
        !parameterized->set_parameters(copy.parameters)) {
      ORC_LOG_ERROR(
          "clone_dag: stage '{}' of node '{}' rejected its own "
          "parameters",
          stage_name, node.node_id);
      return nullptr;
    }

    clone->add_node(std::move(copy));
  }

  // root_inputs_ is never populated by any DAG builder, so there is nothing to
  // carry across (and no setter to carry it with).
  clone->set_output_nodes(dag.output_nodes());
  return clone;
}

// ============================================================================
// DAGExecutor Implementation
// ============================================================================

DAGExecutor::DAGExecutor()
    : cache_enabled_(true),
      artifact_cache_(MAX_CACHED_ARTIFACTS),
      progress_callback_(nullptr) {}

// ============================================================================
// DAG Implementation
// ============================================================================

void DAG::add_node(DAGNode node) { nodes_.push_back(std::move(node)); }

void DAG::set_output_nodes(const std::vector<NodeID>& node_ids) {
  output_node_ids_ = node_ids;
}

std::map<NodeID, size_t> DAG::build_node_index() const {
  std::map<NodeID, size_t> index;
  for (size_t i = 0; i < nodes_.size(); ++i) {
    index[nodes_[i].node_id] = i;
  }
  return index;
}

bool DAG::validate() const { return get_validation_errors().empty(); }

std::vector<std::string> DAG::get_validation_errors() const {
  std::vector<std::string> errors;

  // Build node index
  auto node_index = build_node_index();

  // Check for duplicate node IDs
  if (node_index.size() != nodes_.size()) {
    errors.push_back("Duplicate node IDs detected");
  }

  // Check that all input dependencies exist
  for (const auto& node : nodes_) {
    for (const auto& input_id : node.input_node_ids) {
      if (node_index.find(input_id) == node_index.end()) {
        errors.push_back("Node '" + node.node_id.to_string() +
                         "' depends on non-existent node '" +
                         input_id.to_string() + "'");
      }
    }
  }

  // Check for cycles
  if (has_cycle()) {
    errors.push_back("DAG contains a cycle");
  }

  // Check output nodes exist
  for (const auto& output_id : output_node_ids_) {
    if (node_index.find(output_id) == node_index.end()) {
      errors.push_back("Output node '" + output_id.to_string() +
                       "' does not exist");
    }
  }

  return errors;
}

bool DAG::has_cycle() const {
  auto node_index = build_node_index();
  std::map<NodeID, int> state;  // 0=unvisited, 1=visiting, 2=visited

  std::function<bool(const NodeID&)> visit =
      [&](const NodeID& node_id) -> bool {
    if (state[node_id] == 1) return true;   // Cycle detected
    if (state[node_id] == 2) return false;  // Already visited

    state[node_id] = 1;  // Mark as visiting

    auto it = node_index.find(node_id);
    if (it != node_index.end()) {
      const auto& node = nodes_[it->second];
      for (const auto& input_id : node.input_node_ids) {
        if (visit(input_id)) return true;
      }
    }

    state[node_id] = 2;  // Mark as visited
    return false;
  };

  for (const auto& node : nodes_) {
    if (state[node.node_id] == 0) {
      if (visit(node.node_id)) return true;
    }
  }

  return false;
}

// ============================================================================
// DAGExecutor Implementation
// ============================================================================

std::vector<ArtifactPtr> DAGExecutor::execute(const DAG& dag) {
  if (!dag.validate()) {
    auto errors = dag.get_validation_errors();
    std::ostringstream oss;
    oss << "DAG validation failed:\n";
    for (const auto& error : errors) {
      oss << "  - " << error << "\n";
    }
    throw DAGExecutionError(oss.str());
  }

  // Topological sort
  auto execution_order = topological_sort(dag);

  // Validate observation dependencies across stages in execution order
  {
    std::vector<DAGStagePtr> stages_in_order;
    stages_in_order.reserve(execution_order.size());
    auto node_index = dag.build_node_index();
    for (const auto& node_id : execution_order) {
      const auto& node = dag.nodes()[node_index[node_id]];
      stages_in_order.push_back(node.stage);
    }
    auto validation =
        PipelineValidator::validate_observation_dependencies(stages_in_order);
    if (!validation.valid) {
      std::ostringstream oss;
      oss << "Observation dependency validation failed:\n";
      for (const auto& err : validation.errors) {
        oss << "  - " << err << "\n";
      }
      throw DAGExecutionError(oss.str());
    }
    // Register provided observation schema into the context for type checking
    observation_context_.clear_schema();
    for (const auto& stage : stages_in_order) {
      if (stage) {
        observation_context_.register_schema(
            stage->get_provided_observations());
      }
    }
  }

  // Execute nodes in order
  std::map<NodeID, std::vector<ArtifactPtr>> node_outputs;

  // Initialize with root inputs (virtual node)
  node_outputs[NodeID::root()] = dag.root_inputs();

  size_t total_nodes = execution_order.size();
  size_t current_node = 0;

  auto node_index = dag.build_node_index();

  for (const auto& node_id : execution_order) {
    ++current_node;

    ORC_LOG_DEBUG("Node '{}': Executing ({}/{} in order)", node_id,
                  current_node, total_nodes);

    if (progress_callback_) {
      progress_callback_(node_id, current_node, total_nodes);
    }

    const auto& node = dag.nodes()[node_index[node_id]];

    // Gather inputs
    std::vector<ArtifactPtr> inputs;

    if (node.input_node_ids.empty()) {
      // Root node with no dependencies (e.g., SOURCE nodes)
      // No inputs needed - stage generates output
      inputs = {};
    } else {
      // Special handling for MERGER nodes: collect ALL outputs from source
      // nodes
      bool is_merger =
          (node.stage->get_node_type_info().type == NodeType::MERGER);

      if (is_merger && node.input_node_ids.size() == 1) {
        // MERGER with single input node - collect all outputs from that node
        const auto& input_node_id = node.input_node_ids[0];
        auto it = node_outputs.find(input_node_id);
        if (it == node_outputs.end()) {
          throw DAGExecutionError("Missing input for node '" +
                                  node_id.to_string() + "' from '" +
                                  input_node_id.to_string() + "'");
        }
        // Add ALL outputs from the source node
        inputs = it->second;
        ORC_LOG_DEBUG("Node '{}': MERGER collecting {} outputs from node '{}'",
                      node_id, inputs.size(), input_node_id);
      } else {
        // Normal input gathering - one input per edge
        for (size_t i = 0; i < node.input_node_ids.size(); ++i) {
          const auto& input_node_id = node.input_node_ids[i];
          size_t output_index =
              i < node.input_indices.size() ? node.input_indices[i] : 0;

          auto it = node_outputs.find(input_node_id);
          if (it == node_outputs.end() || output_index >= it->second.size()) {
            throw DAGExecutionError("Missing input for node '" +
                                    node_id.to_string() + "' from '" +
                                    input_node_id.to_string() + "'");
          }

          inputs.push_back(it->second[output_index]);
        }
      }
    }

    // Execute or retrieve from cache
    auto outputs = get_cached_or_execute(node, inputs);
    node_outputs[node_id] = outputs;
  }

  // Gather output artifacts
  std::vector<ArtifactPtr> results;
  for (const auto& output_id : dag.output_nodes()) {
    auto it = node_outputs.find(output_id);
    if (it != node_outputs.end() && !it->second.empty()) {
      results.push_back(it->second[0]);
    }
  }

  return results;
}

std::vector<NodeID> DAGExecutor::topological_sort(const DAG& dag) const {
  auto node_index = dag.build_node_index();

  // Calculate in-degrees
  std::map<NodeID, size_t> in_degree;
  for (const auto& node : dag.nodes()) {
    in_degree[node.node_id] = 0;
  }
  for (const auto& node : dag.nodes()) {
    for (const auto& input_id : node.input_node_ids) {
      in_degree[input_id]++;
    }
  }

  // Kahn's algorithm
  std::queue<NodeID> queue;
  for (const auto& [node_id, degree] : in_degree) {
    if (degree == 0) {
      queue.push(node_id);
    }
  }

  std::vector<NodeID> result;
  while (!queue.empty()) {
    NodeID node_id = queue.front();
    queue.pop();
    result.push_back(node_id);

    const auto& node = dag.nodes()[node_index[node_id]];
    for (const auto& input_id : node.input_node_ids) {
      in_degree[input_id]--;
      if (in_degree[input_id] == 0) {
        queue.push(input_id);
      }
    }
  }

  // Reverse for execution order (dependencies first)
  std::reverse(result.begin(), result.end());

  return result;
}

std::vector<ArtifactPtr> DAGExecutor::get_cached_or_execute(
    const DAGNode& node, const std::vector<ArtifactPtr>& inputs) {
  // Compute expected artifact ID
  auto expected_id = compute_expected_artifact_id(node, inputs);

  // Check cache
  if (cache_enabled_) {
    auto cached = artifact_cache_.get(expected_id);
    if (cached.has_value()) {
      ORC_LOG_TRACE(
          "Node '{}': Using cached result ({} outputs, cache size: {})",
          node.node_id.to_string(), cached->size(), artifact_cache_.size());
      return *cached;
    } else {
      ORC_LOG_TRACE("Node '{}': Cache miss - expected_id='{}' (cache size: {})",
                    node.node_id.to_string(), expected_id.value(),
                    artifact_cache_.size());
    }
  }

  // Sink stages are allowed to return empty outputs (they consume inputs
  // without producing outputs), and keep per-copy state, so they are never
  // shared.
  const auto node_type = node.stage->get_node_type_info().type;
  const bool is_sink =
      (node_type == NodeType::SINK || node_type == NodeType::ANALYSIS_SINK);

  std::vector<ArtifactPtr> outputs;
  if (shared_node_outputs_ && !is_sink) {
    outputs =
        shared_node_outputs_
            ->get_or_produce(node.node_id,
                             [&] {
                               return SharedNodeOutputs::Entry{
                                   execute_stage(node, inputs), node.stage};
                             })
            .outputs;
  } else {
    outputs = execute_stage(node, inputs);
  }

  if (outputs.empty() && is_sink) {
    ORC_LOG_DEBUG("Node '{}': Sink stage executed (no outputs expected)",
                  node.node_id.to_string());
    return {};  // Sink stages don't produce artifacts
  }

  // Log number of outputs for debugging
  if (outputs.size() > 1) {
    ORC_LOG_DEBUG("Node '{}': Stage produced {} outputs",
                  node.node_id.to_string(), outputs.size());
  }

  // Cache ALL outputs from the stage
  if (cache_enabled_ && !outputs.empty()) {
    ORC_LOG_TRACE(
        "Node '{}': Caching {} output(s) with expected_id='{}' (cache will be "
        "size: {})",
        node.node_id.to_string(), outputs.size(), expected_id.value(),
        artifact_cache_.size() + 1);
    artifact_cache_.put(expected_id, outputs);
  }

  return outputs;
}

std::vector<ArtifactPtr> DAGExecutor::execute_stage(
    const DAGNode& node, const std::vector<ArtifactPtr>& inputs) {
  provide_measurement_store(node);
  ORC_LOG_DEBUG("Node '{}': Executing stage '{}'", node.node_id.to_string(),
                node.stage->get_node_type_info().stage_name);
  auto outputs =
      node.stage->execute(inputs, node.parameters, observation_context_);

  const auto node_type = node.stage->get_node_type_info().type;
  const bool is_sink =
      (node_type == NodeType::SINK || node_type == NodeType::ANALYSIS_SINK);
  if (outputs.empty() && !is_sink) {
    ORC_LOG_ERROR("Node '{}': Stage '{}' produced no outputs",
                  node.node_id.to_string(),
                  node.stage->get_node_type_info().stage_name);
    throw DAGExecutionError("Stage '" +
                            node.stage->get_node_type_info().stage_name +
                            "' produced no outputs");
  }
  return outputs;
}

void DAGExecutor::provide_measurement_store(const DAGNode& node) const {
  auto* consumer = dynamic_cast<IStageMeasurementConsumer*>(node.stage.get());
  if (!consumer) {
    return;
  }
  std::shared_ptr<IStageMeasurementStore> store;
  if (measurement_store_ && measurement_fingerprints_) {
    auto it = measurement_fingerprints_->find(node.node_id);
    if (it != measurement_fingerprints_->end()) {
      store = std::make_shared<NodeMeasurementStore>(measurement_store_,
                                                     it->second);
    }
  }
  consumer->set_measurement_store(std::move(store));
}

ArtifactID DAGExecutor::compute_expected_artifact_id(
    const DAGNode& node, const std::vector<ArtifactPtr>& inputs) const {
  // Content-addressed ID derived from the single, shared provenance
  // serialisation (see frame_provenance.h). The executor keys on the runtime
  // input artifact IDs; the static NodeFingerprintMap keys on recursively
  // composed node fingerprints. Both use serialize_node_provenance() so there
  // is one definition of provenance composition.
  std::vector<std::string> input_tokens;
  input_tokens.reserve(inputs.size());
  for (const auto& input : inputs) {
    if (!input) {
      throw DAGExecutionError(
          "Null input artifact in compute_expected_artifact_id");
    }
    input_tokens.push_back(input->id().value());
  }

  return ArtifactID(serialize_node_provenance(
      node.stage->get_node_type_info().stage_name, node.stage->version(),
      input_tokens, node.parameters));
}

void DAGExecutor::clear_cache() { artifact_cache_.clear(); }

std::map<NodeID, std::vector<ArtifactPtr>> DAGExecutor::execute_to_node(
    const DAG& dag, const NodeID& target_node_id) {
  ORC_LOG_DEBUG("Node '{}': Executing DAG to this node",
                target_node_id.to_string());

  if (!dag.validate()) {
    auto errors = dag.get_validation_errors();
    ORC_LOG_ERROR("DAG validation failed with {} errors", errors.size());
    std::ostringstream oss;
    oss << "DAG validation failed:\n";
    for (const auto& error : errors) {
      oss << "  - " << error << "\n";
    }
    throw DAGExecutionError(oss.str());
  }

  // Check that target node exists
  auto node_index = dag.build_node_index();
  if (node_index.find(target_node_id) == node_index.end()) {
    ORC_LOG_ERROR("Node '{}': Does not exist in DAG",
                  target_node_id.to_string());
    throw DAGExecutionError("Target node '" + target_node_id.to_string() +
                            "' does not exist in DAG");
  }

  // Topological sort up to target node
  auto execution_order = topological_sort_to_node(dag, target_node_id);
  ORC_LOG_DEBUG("Node '{}': Execution order includes {} nodes",
                target_node_id.to_string(), execution_order.size());

  // Execute nodes in order
  std::map<NodeID, std::vector<ArtifactPtr>> node_outputs;

  // Initialize with root inputs (virtual node)
  node_outputs[NodeID::root()] = dag.root_inputs();

  size_t total_nodes = execution_order.size();
  size_t current_node = 0;

  for (const auto& node_id : execution_order) {
    ++current_node;

    ORC_LOG_DEBUG("Node '{}': Executing ({}/{} in order)", node_id,
                  current_node, total_nodes);

    if (progress_callback_) {
      progress_callback_(node_id, current_node, total_nodes);
    }

    const auto& node = dag.nodes()[node_index[node_id]];

    // Gather inputs
    std::vector<ArtifactPtr> inputs;

    if (node.input_node_ids.empty()) {
      // Root node with no dependencies (e.g., SOURCE nodes)
      // No inputs needed - stage generates output
      inputs = {};
    } else {
      // Special handling for MERGER nodes: collect ALL outputs from source
      // nodes
      bool is_merger =
          (node.stage->get_node_type_info().type == NodeType::MERGER);

      if (is_merger && node.input_node_ids.size() == 1) {
        // MERGER with single input node - collect all outputs from that node
        const auto& input_node_id = node.input_node_ids[0];
        auto it = node_outputs.find(input_node_id);
        if (it == node_outputs.end()) {
          throw DAGExecutionError("Missing input for node '" +
                                  node_id.to_string() + "' from '" +
                                  input_node_id.to_string() + "'");
        }
        // Add ALL outputs from the source node
        inputs = it->second;
        ORC_LOG_DEBUG("Node '{}': MERGER collecting {} outputs from node '{}'",
                      node_id, inputs.size(), input_node_id);
      } else {
        // Normal input gathering - one input per edge
        for (size_t i = 0; i < node.input_node_ids.size(); ++i) {
          const auto& input_node_id = node.input_node_ids[i];
          size_t output_index =
              i < node.input_indices.size() ? node.input_indices[i] : 0;

          auto it = node_outputs.find(input_node_id);
          if (it == node_outputs.end() || output_index >= it->second.size()) {
            throw DAGExecutionError("Missing input for node '" +
                                    node_id.to_string() + "' from '" +
                                    input_node_id.to_string() + "'");
          }

          inputs.push_back(it->second[output_index]);
        }
      }
    }

    // Execute or retrieve from cache
    auto outputs = get_cached_or_execute(node, inputs);
    node_outputs[node_id] = outputs;
  }

  return node_outputs;
}

std::vector<NodeID> DAGExecutor::topological_sort_to_node(
    const DAG& dag, const NodeID& target_node_id) const {
  // Build dependency graph and find all nodes needed to compute target
  auto node_index = dag.build_node_index();
  std::set<NodeID> required_nodes;

  // Recursive DFS to find all dependencies
  std::function<void(const NodeID&)> collect_dependencies =
      [&](const NodeID& node_id) {
        if (required_nodes.find(node_id) != required_nodes.end()) {
          return;  // Already visited
        }

        required_nodes.insert(node_id);

        auto it = node_index.find(node_id);
        if (it != node_index.end()) {
          const auto& node = dag.nodes()[it->second];
          for (const auto& input_id : node.input_node_ids) {
            collect_dependencies(input_id);
          }
        }
      };

  collect_dependencies(target_node_id);

  // Now do topological sort on only the required nodes
  std::map<NodeID, size_t> in_degree;
  for (const auto& node_id : required_nodes) {
    in_degree[node_id] = 0;
  }

  for (const auto& node_id : required_nodes) {
    auto it = node_index.find(node_id);
    if (it != node_index.end()) {
      const auto& node = dag.nodes()[it->second];
      for (const auto& input_id : node.input_node_ids) {
        if (required_nodes.find(input_id) != required_nodes.end()) {
          in_degree[input_id]++;
        }
      }
    }
  }

  // Kahn's algorithm on required nodes only
  std::queue<NodeID> queue;
  for (const auto& [node_id, degree] : in_degree) {
    if (degree == 0) {
      queue.push(node_id);
    }
  }

  std::vector<NodeID> result;
  while (!queue.empty()) {
    NodeID node_id = queue.front();
    queue.pop();
    result.push_back(node_id);

    auto it = node_index.find(node_id);
    if (it != node_index.end()) {
      const auto& node = dag.nodes()[it->second];
      for (const auto& input_id : node.input_node_ids) {
        if (required_nodes.find(input_id) != required_nodes.end()) {
          in_degree[input_id]--;
          if (in_degree[input_id] == 0) {
            queue.push(input_id);
          }
        }
      }
    }
  }

  // Reverse for execution order (dependencies first)
  std::reverse(result.begin(), result.end());

  return result;
}

}  // namespace orc
