/*
 * File:        project_to_dag.cpp
 * Module:      orc-core
 * Purpose:     Project to DAG conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns *
 * ARCHITECTURE NOTE:
 * This file uses READ-ONLY access to Project via const getters.
 * It NEVER modifies Project state - use project_io:: functions for that. */

#include "project_to_dag.h"

#include <orc/stage/observation/observation_context.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <sstream>

#include "stage_registry.h"

namespace orc {

namespace {

std::string build_missing_stage_message(const Project& project,
                                        const ProjectDAGNode& node) {
  for (const auto& requirement : project.get_required_plugins()) {
    if (std::find(requirement.stage_names.begin(),
                  requirement.stage_names.end(),
                  node.stage_name) == requirement.stage_names.end()) {
      continue;
    }

    std::ostringstream oss;
    oss << "Unknown stage type: " << node.stage_name << " in node "
        << node.node_id.value();

    if (!requirement.plugin_id.empty()) {
      oss << ". Required plugin: " << requirement.plugin_id;
    }

    if (!requirement.source_repo_url.empty()) {
      oss << ". Source: " << requirement.source_repo_url;
    } else if (!requirement.release_asset_url.empty()) {
      oss << ". Release asset: " << requirement.release_asset_url;
    }

    oss << ". Install or re-enable the plugin, then reload the project.";
    return oss.str();
  }

  return "Unknown stage type: " + node.stage_name + " in node " +
         std::to_string(node.node_id.value());
}

// True for the parameter names that carry a filesystem path.
bool is_file_path_parameter(const std::string& parameter_name) {
  return parameter_name.find("_path") != std::string::npos ||
         parameter_name == "output_path" || parameter_name == "input_path";
}

// Source node IDs of a node's incoming connections, in project edge order —
// the same order the executor gathers the artifacts into execute()'s inputs
// vector.
std::vector<NodeID> input_node_ids_of(const Project& project,
                                      const NodeID& node_id) {
  std::vector<NodeID> ids;
  for (const auto& edge : project.get_edges()) {
    if (edge.target_node_id == node_id) {
      ids.push_back(edge.source_node_id);
    }
  }
  return ids;
}

}  // namespace

// Helper function to resolve paths relative to project root
// Matches the resolve_path function in project.cpp
static std::string resolve_path_for_execution(const std::string& path,
                                              const std::string& project_root) {
  if (path.empty() || project_root.empty()) {
    return path;
  }

  // First expand any ${PROJECT_ROOT} variables
  const std::string variable = "${PROJECT_ROOT}";
  std::string expanded = path;
  size_t pos = 0;
  while ((pos = expanded.find(variable, pos)) != std::string::npos) {
    expanded.replace(pos, variable.length(), project_root);
    pos += project_root.length();
  }

  // Create a filesystem path
  std::filesystem::path p(expanded);

  // If already absolute, return normalized version
  if (p.is_absolute()) {
    try {
      return std::filesystem::weakly_canonical(p).string();
    } catch (const std::filesystem::filesystem_error&) {
      return p.string();
    }
  }

  // Resolve relative to project root
  std::filesystem::path resolved = std::filesystem::path(project_root) / p;
  try {
    return std::filesystem::weakly_canonical(resolved).string();
  } catch (const std::filesystem::filesystem_error&) {
    return resolved.string();
  }
}

void resolve_path_parameters(std::map<std::string, ParameterValue>& parameters,
                             const std::string& project_root) {
  for (auto& [param_name, param_value] : parameters) {
    if (!std::holds_alternative<std::string>(param_value)) {
      continue;
    }
    if (!is_file_path_parameter(param_name)) {
      continue;
    }
    const std::string path = std::get<std::string>(param_value);
    if (path.empty()) {
      continue;
    }
    param_value = resolve_path_for_execution(path, project_root);
  }
}

void apply_input_node_ids_parameter(
    const DAGStage& stage, VideoSystem project_format, SourceType source_type,
    const std::vector<NodeID>& input_node_ids,
    std::map<std::string, ParameterValue>& parameters) {
  const auto* param_stage = dynamic_cast<const ParameterizedStage*>(&stage);
  if (!param_stage) return;

  const auto descriptors =
      param_stage->get_parameter_descriptors(project_format, source_type);
  const bool declared =
      std::any_of(descriptors.begin(), descriptors.end(),
                  [](const ParameterDescriptor& descriptor) {
                    return descriptor.name == kInputNodeIdsParameter;
                  });
  if (!declared) return;

  std::string value;
  for (const auto& id : input_node_ids) {
    if (!value.empty()) value += ",";
    value += std::to_string(id.value());
  }
  parameters[kInputNodeIdsParameter] = value;
}

// Stage instances from |previous| that |nodes| can carry over, keyed by node
// id.
//
// Rebuilding a DAG used to discard every stage object, and with it everything
// execute() had accumulated behind them. That is invisible for most stages,
// which recompute from their inputs anyway, but a source stage caches the
// representation it loaded — for a long capture with a large dropout sidecar,
// seconds of work — and would only load exactly the same thing again. Editing
// a link somewhere else in the graph therefore cost a full source reload.
//
// A node is a candidate only when nothing about it changed:
//
//  - Same stage. A node id can be re-used by a different stage type.
//  - Same effective parameters. This is the whole of a stage's configured
//    state, so an unchanged map means an unchanged stage; it also covers the
//    wiring of any stage that declares the reserved input-node-ids parameter,
//    which is written into the map before this runs.
//  - Same input wiring. Checked separately because a stage that does not
//    declare that parameter has no other way to notice its inputs changed.
//
// Being unchanged in itself is not enough, though: a stage caches what it
// computed, and what it computed came from its inputs. So the disqualification
// propagates downstream — a node whose ancestry changed anywhere is rebuilt
// too, even when its own configuration and immediate wiring are untouched.
// That buys the invariant worth having: a carried-over instance holds state
// produced by a subgraph identical to the one it now sits in. It costs nothing
// in practice, because the expensive instances are sources, and a source is
// upstream of everything — never downstream of an edit that spared it.
std::map<NodeID, DAGStagePtr> reusable_stages(
    const DAG* previous, const std::vector<DAGNode>& nodes) {
  std::map<NodeID, DAGStagePtr> reusable;
  if (!previous) return reusable;

  std::map<NodeID, const DAGNode*> prior_index;
  for (const auto& node : previous->nodes()) {
    prior_index[node.node_id] = &node;
  }

  // Pass 1: the nodes that are unchanged in themselves.
  for (const auto& node : nodes) {
    const auto prior = prior_index.find(node.node_id);
    if (prior == prior_index.end()) continue;

    const DAGNode& before = *prior->second;
    if (!before.stage || !node.stage) continue;
    if (before.stage->get_node_type_info().stage_name !=
        node.stage->get_node_type_info().stage_name) {
      continue;
    }
    if (before.parameters != node.parameters) continue;
    if (before.input_node_ids != node.input_node_ids) continue;

    reusable[node.node_id] = before.stage;
  }

  // Pass 2: withdraw every node a rebuilt node feeds, transitively. Iterated to
  // a fixpoint rather than walked in dependency order, because the project's
  // node list carries no guarantee of being topologically sorted.
  bool settled = false;
  while (!settled) {
    settled = true;
    for (const auto& node : nodes) {
      if (reusable.find(node.node_id) == reusable.end()) continue;
      for (const auto& input_id : node.input_node_ids) {
        if (reusable.find(input_id) != reusable.end()) continue;
        reusable.erase(node.node_id);
        settled = false;
        break;
      }
    }
  }

  return reusable;
}

std::shared_ptr<DAG> project_to_dag(const Project& project,
                                    const DAG* previous) {
  auto dag = std::make_shared<DAG>();
  auto& registry = StageRegistry::instance();

  // Get project root for path resolution
  const std::string& project_root = project.get_project_root();

  // Convert each ProjectDAGNode to a DAGNode
  // All nodes are uniform now - SOURCE nodes use the configured source stage
  std::vector<DAGNode> dag_nodes;

  for (const auto& proj_node : project.get_nodes()) {
    DAGNode dag_node;
    dag_node.node_id = proj_node.node_id;

    // Instantiate stage from registry
    if (!registry.has_stage(proj_node.stage_name)) {
      throw ProjectConversionError(
          build_missing_stage_message(project, proj_node));
    }

    dag_node.stage = registry.create_stage(proj_node.stage_name);

    ORC_LOG_DEBUG(
        "Node '{}': Converting from project (stage: {}, {} parameters)",
        proj_node.node_id, proj_node.stage_name, proj_node.parameters.size());

    // Build the effective parameter map:
    // 1. Start from the stage's declared descriptor defaults for this project's
    //    video format and source type.  This ensures format-specific defaults
    //    (e.g. decoder_type="pal2d" for PAL) are applied even when a project
    //    file was saved without an explicit value for a parameter.
    // 2. Then overlay any values that are actually stored in the project file,
    //    so user-saved choices always win.
    auto* param_stage_for_defaults =
        dynamic_cast<ParameterizedStage*>(dag_node.stage.get());
    if (param_stage_for_defaults) {
      const auto descriptors =
          param_stage_for_defaults->get_parameter_descriptors(
              project.get_video_format(), project.get_source_format());
      for (const auto& desc : descriptors) {
        if (desc.constraints.default_value.has_value()) {
          dag_node.parameters.emplace(desc.name,
                                      desc.constraints.default_value.value());
        }
      }
    }

    // Overlay the stored project parameters (they take precedence over
    // defaults)
    for (const auto& [key, value] : proj_node.parameters) {
      dag_node.parameters[key] = value;
    }

    // Resolve file paths relative to project root
    resolve_path_parameters(dag_node.parameters, project_root);

    // A stage that identifies its inputs by node ID can only be told by the
    // host: artifacts carry no node identity. Fill the reserved parameter in
    // before the stage is configured, so set_parameters() sees the inputs the
    // node actually has.
    const std::vector<NodeID> input_ids =
        input_node_ids_of(project, proj_node.node_id);
    apply_input_node_ids_parameter(*dag_node.stage, project.get_video_format(),
                                   project.get_source_format(), input_ids,
                                   dag_node.parameters);

    for (const auto& [key, value] : dag_node.parameters) {
      std::visit(
          [&proj_node,
           key_ref = std::cref(key)]([[maybe_unused]] const auto& v) {
            ORC_LOG_DEBUG("Node '{}':   param '{}' = {}", proj_node.node_id,
                          key_ref.get(), v);
          },
          value);
    }

    // Apply parameters to the stage instance if it's parameterized
    auto* param_stage = dynamic_cast<ParameterizedStage*>(dag_node.stage.get());
    if (param_stage && !dag_node.parameters.empty()) {
      param_stage->set_parameters(dag_node.parameters);
      ORC_LOG_DEBUG("Node '{}': Applied {} parameters to stage instance",
                    proj_node.node_id, dag_node.parameters.size());
    }

    // Input edges for this node, in the order the executor gathers them
    dag_node.input_node_ids = input_ids;
    dag_node.input_indices.assign(input_ids.size(), 0);  // Output index 0

    dag_nodes.push_back(dag_node);
  }

  // Carry unchanged stage instances over from the previous build, so whatever
  // they had loaded survives the rebuild. The stages created above for those
  // nodes are dropped: they were needed to read the descriptors that seed the
  // parameter map, and building one costs nothing (a source's expense is in
  // execute(), not in construction).
  //
  // Deliberately without re-applying the parameters to a carried-over
  // instance. They are identical to the ones it was configured with, so
  // set_parameters() could only be a no-op — or, for a stage that treats it as
  // a reconfiguration and drops its caches, destroy the very state being
  // preserved.
  const auto reusable = reusable_stages(previous, dag_nodes);
  for (auto& node : dag_nodes) {
    const auto it = reusable.find(node.node_id);
    if (it == reusable.end()) continue;
    node.stage = it->second;
    ORC_LOG_DEBUG("Node '{}': Reusing stage instance (unchanged)",
                  node.node_id);
  }

  // Add all nodes to DAG
  for (const auto& node : dag_nodes) {
    dag->add_node(node);
  }

  // Find SINK nodes for output
  std::vector<NodeID> output_node_ids;
  for (const auto& proj_node : project.get_nodes()) {
    if (proj_node.node_type == NodeType::SINK) {
      output_node_ids.push_back(proj_node.node_id);
    }
  }
  if (!output_node_ids.empty()) {
    dag->set_output_nodes(output_node_ids);
  }

  // Validate the DAG
  if (!dag->validate()) {
    std::ostringstream oss;
    oss << "DAG validation failed:";
    for (const auto& error : dag->get_validation_errors()) {
      oss << "\n  - " << error;
    }
    throw ProjectConversionError(oss.str());
  }

  return dag;
}

void validate_source_nodes(const std::shared_ptr<DAG>& dag) {
  if (!dag) {
    throw ProjectConversionError("Cannot validate null DAG");
  }

  // Try to execute each source node to validate they can be accessed
  // Source nodes may produce empty output if no file path is configured (valid
  // placeholder state)
  ORC_LOG_DEBUG("Validating {} DAG nodes", dag->nodes().size());

  for (const auto& node : dag->nodes()) {
    // Check if this is a source node by checking if it has no inputs
    if (node.input_node_ids.empty()) {
      ORC_LOG_DEBUG("Validating source node: {}", node.node_id);
      try {
        // Execute the stage with empty inputs to validate
        // This will trigger source loading and validation
        ObservationContext observation_context;
        auto outputs =
            node.stage->execute({}, node.parameters, observation_context);
        if (outputs.empty()) {
          // Empty output is valid - source may have no file configured
          // (placeholder node)
          ORC_LOG_WARN(
              "Source node '{}' produced no output (no file configured)",
              node.node_id);
        } else {
          ORC_LOG_DEBUG("Source node validation passed: {}", node.node_id);
        }
      } catch (const std::exception& e) {
        // Source validation failed - re-throw with more context
        throw ProjectConversionError("Source validation failed for node '" +
                                     node.node_id.to_string() +
                                     "': " + e.what());
      }
    }
  }
}

}  // namespace orc
