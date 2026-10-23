/*
 * File:        analysis_tool_presenter.cpp
 * Module:      orc-presenters
 * Purpose:     Base class for analysis tool presenters implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "analysis_tool_presenter.h"

#include <orc/stage/artifact.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <map>
#include <stdexcept>

#include "../core/analysis/analysis_registry.h"
#include "../core/analysis/analysis_tool.h"
#include "../core/include/dag_executor.h"
#include "../core/include/project.h"
#include "../core/include/project_to_dag.h"

namespace orc::presenters {

// Forward declaration of DAG builder utility
namespace {
/**
 * @brief Build DAG from project nodes and edges
 * @param project Project to build DAG from
 * @return Shared pointer to built DAG
 * @throws std::runtime_error if DAG construction fails
 */
std::shared_ptr<orc::DAG> buildDAGFromProject(const orc::Project* project);
}  // namespace

// =============================================================================
// Impl Class
// =============================================================================

class AnalysisToolPresenter::Impl {
 public:
  explicit Impl(orc::Project* project) : project_(project) {
    if (!project_) {
      throw std::invalid_argument(
          "AnalysisToolPresenter: project cannot be null");
    }
  }

  orc::Project* project_;
  std::shared_ptr<orc::DAG> cached_dag_;
  AnalysisToolPresenter::ProgressCallback progress_callback_;
};

// =============================================================================
// Constructor/Destructor
// =============================================================================

AnalysisToolPresenter::AnalysisToolPresenter(void* project_handle)
    : impl_(
          std::make_unique<Impl>(static_cast<orc::Project*>(project_handle))) {}

AnalysisToolPresenter::~AnalysisToolPresenter() = default;

AnalysisToolPresenter::AnalysisToolPresenter(AnalysisToolPresenter&&) noexcept =
    default;
AnalysisToolPresenter& AnalysisToolPresenter::operator=(
    AnalysisToolPresenter&&) noexcept = default;

// =============================================================================
// DAG Management
// =============================================================================

std::shared_ptr<void> AnalysisToolPresenter::getOrBuildDAG() {
  if (!impl_->cached_dag_) {
    ORC_LOG_DEBUG("AnalysisToolPresenter: Building DAG from project");
    impl_->cached_dag_ = buildDAGFromProject(impl_->project_);
  }
  return impl_->cached_dag_;
}

void AnalysisToolPresenter::invalidateDAG() {
  ORC_LOG_DEBUG("AnalysisToolPresenter: Invalidating cached DAG");
  impl_->cached_dag_.reset();
}

// =============================================================================
// Node Query Utilities (internal helpers using concrete DAG)
// =============================================================================

static const std::vector<orc::DAGNode>& getProjectNodesInternal(
    const std::shared_ptr<orc::DAG>& dag) {
  return dag->nodes();
}

bool AnalysisToolPresenter::hasNodeInput(orc::NodeID node_id) const {
  auto dag = std::static_pointer_cast<orc::DAG>(
      const_cast<AnalysisToolPresenter*>(this)->getOrBuildDAG());
  const auto& nodes = getProjectNodesInternal(dag);
  auto it = std::find_if(
      nodes.begin(), nodes.end(),
      [node_id](const orc::DAGNode& node) { return node.node_id == node_id; });

  return it != nodes.end() && !it->input_node_ids.empty();
}

orc::NodeID AnalysisToolPresenter::getFirstInputNodeId(
    orc::NodeID node_id) const {
  auto dag = std::static_pointer_cast<orc::DAG>(
      const_cast<AnalysisToolPresenter*>(this)->getOrBuildDAG());
  const auto& nodes = getProjectNodesInternal(dag);
  auto it = std::find_if(
      nodes.begin(), nodes.end(),
      [node_id](const orc::DAGNode& node) { return node.node_id == node_id; });

  if (it != nodes.end() && !it->input_node_ids.empty()) {
    return it->input_node_ids[0];
  }

  return orc::NodeID();  // Invalid ID
}

// =============================================================================
// DAG Execution
// =============================================================================

std::vector<std::shared_ptr<void>> AnalysisToolPresenter::executeToNode(
    orc::NodeID node_id) {
  auto dag_void = getOrBuildDAG();
  auto dag = std::static_pointer_cast<orc::DAG>(dag_void);
  if (!dag) {
    throw std::runtime_error(
        "AnalysisToolPresenter: Cannot execute - DAG not available");
  }

  ORC_LOG_DEBUG("AnalysisToolPresenter: Executing DAG to node {}",
                node_id.value());

  try {
    orc::DAGExecutor executor;
    auto all_outputs = executor.execute_to_node(*dag, node_id);

    auto output_it = all_outputs.find(node_id);
    if (output_it != all_outputs.end()) {
      ORC_LOG_DEBUG("AnalysisToolPresenter: Node {} produced {} artifacts",
                    node_id.value(), output_it->second.size());
      // Convert concrete artifacts to opaque void* handles
      std::vector<std::shared_ptr<void>> result;
      result.reserve(output_it->second.size());
      for (const auto& artifact : output_it->second) {
        result.push_back(std::static_pointer_cast<void>(
            std::const_pointer_cast<orc::Artifact>(artifact)));
      }
      return result;
    }

    ORC_LOG_WARN(
        "AnalysisToolPresenter: Node {} executed but produced no output",
        node_id.value());
    return {};

  } catch (const std::exception& e) {
    ORC_LOG_ERROR("AnalysisToolPresenter: DAG execution failed: {}", e.what());
    throw std::runtime_error(std::string("Failed to execute DAG: ") + e.what());
  }
}

void* AnalysisToolPresenter::getProjectPointer() const {
  return impl_->project_;
}

// =============================================================================
// Result Application
// =============================================================================

bool AnalysisToolPresenter::prepareResultChanges(
    const orc::AnalysisResult& result, orc::NodeID node_id,
    std::map<std::string, orc::ParameterValue>* changes) {
  auto* tool = orc::AnalysisRegistry::instance().findById(toolId());
  if (!tool) {
    ORC_LOG_ERROR("AnalysisToolPresenter: Tool {} not found in registry",
                  toolId());
    return false;
  }

  if (!tool->canApplyToGraph()) {
    ORC_LOG_WARN(
        "AnalysisToolPresenter: Tool {} does not support applying to graph",
        toolName());
    return false;
  }

  // applyToGraph() reads only the status, summary and graphData, and fills
  // parameterChanges from them; it never touches the project.
  orc::AnalysisResult core_result;
  core_result.status = result.status;
  core_result.summary = result.summary;
  core_result.graphData = result.graphData;

  try {
    if (!tool->applyToGraph(core_result, *impl_->project_, node_id)) {
      ORC_LOG_WARN(
          "AnalysisToolPresenter: Failed to prepare {} result for node {}",
          toolName(), node_id.value());
      return false;
    }
  } catch (const std::exception& e) {
    ORC_LOG_ERROR("AnalysisToolPresenter: Exception preparing result: {}",
                  e.what());
    return false;
  }

  if (changes) {
    *changes = std::move(core_result.parameterChanges);
  }
  return true;
}

bool AnalysisToolPresenter::applyResultToGraph(
    const orc::AnalysisResult& result, orc::NodeID node_id) {
  std::map<std::string, orc::ParameterValue> changes;
  if (!prepareResultChanges(result, node_id, &changes)) {
    return false;
  }

  try {
    if (!changes.empty()) {
      // set_node_parameters replaces the node's whole parameter set, so the
      // tool's changes join the stored values: a Disc Mapper apply writes
      // `ranges` and must leave the Frame Map's padding settings alone.
      const auto& nodes = impl_->project_->get_nodes();
      const auto node_it = std::find_if(
          nodes.begin(), nodes.end(),
          [&node_id](const auto& n) { return n.node_id == node_id; });
      if (node_it == nodes.end()) {
        ORC_LOG_ERROR("AnalysisToolPresenter: Node {} not found",
                      node_id.value());
        return false;
      }
      auto merged = node_it->parameters;
      for (const auto& [key, value] : changes) {
        merged[key] = value;
      }
      orc::project_io::set_node_parameters(*impl_->project_, node_id, merged);
      ORC_LOG_DEBUG(
          "AnalysisToolPresenter: Applied {} parameter changes to node {}",
          changes.size(), node_id.value());
    }

    ORC_LOG_INFO(
        "AnalysisToolPresenter: Successfully applied {} result to node {}",
        toolName(), node_id.value());
    invalidateDAG();  // DAG structure may have changed
    return true;
  } catch (const std::exception& e) {
    ORC_LOG_ERROR("AnalysisToolPresenter: Exception applying result: {}",
                  e.what());
    return false;
  }
}

// =============================================================================
// DAG Building Utility
// =============================================================================

namespace {

std::shared_ptr<orc::DAG> buildDAGFromProject(const orc::Project* project) {
  if (!project) {
    throw std::runtime_error("Cannot build DAG from null project");
  }

  // Use the core project_to_dag utility which handles stage instantiation
  try {
    auto dag = orc::project_to_dag(*project);
    ORC_LOG_DEBUG("AnalysisToolPresenter: Built DAG with {} nodes",
                  project->get_nodes().size());
    return dag;
  } catch (const orc::ProjectConversionError& e) {
    throw std::runtime_error(std::string("Failed to convert project to DAG: ") +
                             e.what());
  }
}

}  // anonymous namespace

}  // namespace orc::presenters
