/*
 * File:        dag_executor.h
 * Module:      orc-core
 * Purpose:     DAG execution engine
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

// Stage and plugin implementations may use this header for DAG execution.
// GUI/CLI code must NOT include this directly; use presenters instead.

#include <orc/stage/artifact.h>
#include <orc/stage/node_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/stage.h>
#include <orc/support/lru_cache.h>

#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "frame_provenance.h"

namespace orc {

class ObservationStore;   // observation_store.h
class SharedNodeOutputs;  // shared_node_outputs.h

/**
 * @brief Exception thrown during DAG execution
 */
/**
 * @brief Represents a node in the processing DAG
 */
struct DAGNode {
  NodeID node_id;     // Unique within DAG
  DAGStagePtr stage;  // Processing stage
  std::map<std::string, ParameterValue>
      parameters;                      // Stage parameters (strong types)
  std::vector<NodeID> input_node_ids;  // Dependencies
  std::vector<size_t> input_indices;   // Which output from each input node
};

/**
 * @brief A complete processing DAG
 *
 * The DAG is static and declarative. Execution order is derived from
 * dependencies. No dynamic mutation during execution.
 */
class DAG {
 public:
  DAG() = default;

  // Prevent copying - DAG instances should be owned and referenced via
  // shared_ptr
  DAG(const DAG&) = delete;
  DAG& operator=(const DAG&) = delete;

  // Allow moving for construction/initialization
  DAG(DAG&&) = default;
  DAG& operator=(DAG&&) = default;

  // DAG construction
  void add_node(DAGNode node);
  void set_output_nodes(const std::vector<NodeID>& node_ids);

  // DAG validation
  bool validate() const;
  std::vector<std::string> get_validation_errors() const;

  // Accessors
  const std::vector<DAGNode>& nodes() const { return nodes_; }
  const std::vector<ArtifactPtr>& root_inputs() const { return root_inputs_; }
  const std::vector<NodeID>& output_nodes() const { return output_node_ids_; }

  // Helper for validation (needs to be public for DAGExecutor)
  std::map<NodeID, size_t> build_node_index() const;

 private:
  std::vector<DAGNode> nodes_;
  std::vector<ArtifactPtr> root_inputs_;
  std::vector<NodeID> output_node_ids_;

  // Helper for validation
  bool has_cycle() const;
};

/**
 * @brief Copy @p dag with a fresh stage instance behind every node.
 *
 * Stages are stateful: most keep their configuration in members and several
 * re-apply the parameter map from execute(), so one instance cannot be
 * executed from two threads at once. A DAG is otherwise shared freely — the
 * background observation pool hands the same one to every worker — so a
 * consumer that executes on its own thread takes a clone and owns the stages
 * behind it outright.
 *
 * Each node is rebuilt from the registry by its stage name and configured
 * from the node's parameter map, which is the whole of a stage's configured
 * state (project_to_dag builds a node the same way). State a stage acquired
 * after that — a trigger's cached results, a source's loaded representation —
 * is not carried over: the clone is for executing, not for reading results
 * back out of.
 *
 * Returns nullptr if any stage cannot be recreated, which leaves the caller
 * to fall back to the shared DAG rather than silently losing nodes.
 */
std::shared_ptr<DAG> clone_dag_with_fresh_stages(const DAG& dag);

/**
 * @brief Executes a DAG, producing output artifacts
 *
 * Handles:
 * - Topological sorting
 * - Caching (by artifact ID)
 * - Partial re-execution
 */
class DAGExecutor {
 public:
  DAGExecutor();

  // Prevent copying - executors have state (cache) that shouldn't be duplicated
  DAGExecutor(const DAGExecutor&) = delete;
  DAGExecutor& operator=(const DAGExecutor&) = delete;

  // Move operations deleted - cache contains mutex which is not movable
  DAGExecutor(DAGExecutor&&) = delete;
  DAGExecutor& operator=(DAGExecutor&&) = delete;

  // Execution
  std::vector<ArtifactPtr> execute(const DAG& dag);

  /**
   * @brief Execute DAG up to and including a specific node
   *
   * Executes only the portion of the DAG needed to produce the output
   * of the specified node. Returns a map of all node outputs produced
   * during execution.
   *
   * @param dag The DAG to execute
   * @param target_node_id The node to execute up to (inclusive)
   * @return Map of node_id -> list of output artifacts
   * @throws DAGExecutionError if target_node_id doesn't exist
   */
  std::map<NodeID, std::vector<ArtifactPtr>> execute_to_node(
      const DAG& dag, const NodeID& target_node_id);

  // Cache management
  void set_cache_enabled(bool enabled) { cache_enabled_ = enabled; }
  bool is_cache_enabled() const { return cache_enabled_; }

  void clear_cache();

  // Observation context access
  ObservationContext& get_observation_context() { return observation_context_; }
  const ObservationContext& get_observation_context() const {
    return observation_context_;
  }

  /**
   * @brief Share node outputs with other executors running copies of the DAG.
   *
   * With @p shared set, a node that is not a sink takes its outputs from
   * @p shared, executing its stage only when no copy has produced them yet
   * (see SharedNodeOutputs). nullptr (the default) executes every node here.
   * Set before executing; not thread-safe with execute().
   */
  void set_shared_node_outputs(std::shared_ptr<SharedNodeOutputs> shared) {
    shared_node_outputs_ = std::move(shared);
  }

  /**
   * @brief Give stages that keep measurements the host's store.
   *
   * Before executing a node whose stage implements IStageMeasurementConsumer,
   * the executor hands it a store bound to the node's fingerprint in
   * @p fingerprints (nullptr when either argument is null or the node has no
   * fingerprint). Set before executing; not thread-safe with execute().
   */
  void set_measurement_store(
      std::shared_ptr<ObservationStore> store,
      std::shared_ptr<const NodeFingerprintMap> fingerprints) {
    measurement_store_ = std::move(store);
    measurement_fingerprints_ = std::move(fingerprints);
  }

  // Progress callback (optional)
  using ProgressCallback =
      std::function<void(NodeID node_id, size_t current, size_t total)>;
  void set_progress_callback(ProgressCallback callback) {
    progress_callback_ = callback;
  }

 private:
  bool cache_enabled_ = true;

  /// Observation context for this pipeline execution
  ObservationContext observation_context_;

  std::shared_ptr<SharedNodeOutputs> shared_node_outputs_;
  std::shared_ptr<ObservationStore> measurement_store_;
  std::shared_ptr<const NodeFingerprintMap> measurement_fingerprints_;

  // Hands @p node's stage its measurement store, when it keeps measurements.
  void provide_measurement_store(const DAGNode& node) const;

  // Executes @p node's stage on @p inputs and checks it produced outputs.
  std::vector<ArtifactPtr> execute_stage(
      const DAGNode& node, const std::vector<ArtifactPtr>& inputs);

  /// LRU cache for artifact results
  /// Limit cache size to prevent unbounded memory growth during batch
  /// processing Each cached entry can be ~1-2MB (VideoFrameRepresentation), so
  /// 500 entries ≈ 500-1000MB max
  static constexpr size_t MAX_CACHED_ARTIFACTS = 500;
  LRUCache<ArtifactID, std::vector<ArtifactPtr>> artifact_cache_;

  ProgressCallback progress_callback_;

  // Execution helpers
  std::vector<NodeID> topological_sort(const DAG& dag) const;
  std::vector<NodeID> topological_sort_to_node(
      const DAG& dag, const NodeID& target_node_id) const;
  std::vector<ArtifactPtr> get_cached_or_execute(
      const DAGNode& node, const std::vector<ArtifactPtr>& inputs);
  ArtifactID compute_expected_artifact_id(
      const DAGNode& node, const std::vector<ArtifactPtr>& inputs) const;
};

}  // namespace orc
