/*
 * File:        project_to_dag.h
 * Module:      orc-core
 * Purpose:     Project to DAG conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

// =============================================================================
// MVP Architecture Enforcement
// =============================================================================
// This header is part of the CORE internal implementation.
// GUI code must NOT include this header directly.
// DAG operations are exposed through ProjectPresenter and RenderPresenter.
// =============================================================================
#if defined(ORC_GUI_BUILD)
#error \
    "GUI code cannot include core/include/project_to_dag.h. Use ProjectPresenter/RenderPresenter instead."
#endif
#if defined(ORC_CLI_BUILD)
#error \
    "CLI code cannot include core/include/project_to_dag.h. DAG is managed by presenters."
#endif

#include <map>
#include <memory>
#include <stdexcept>

#include "dag_executor.h"
#include "project.h"

namespace orc {

/**
 * @brief Exception thrown during Project-to-DAG conversion
 */
class ProjectConversionError : public std::runtime_error {
 public:
  explicit ProjectConversionError(const std::string& msg)
      : std::runtime_error(msg) {}
};

/**
 * @brief Convert a Project to an executable DAG
 *
 * This function bridges the gap between serializable Projects (strings, data)
 * and executable DAGs (C++ objects, stages).
 *
 * Conversion process:
 * 1. Create DAG nodes by instantiating stages from the stage registry
 * 2. SOURCE nodes use the configured source stage which loads source files from
 * parameters
 * 3. Set up edges and dependencies
 * 4. Validate the resulting DAG
 *
 * A node whose stage, parameters and input wiring are all unchanged from
 * @p previous keeps that build's stage instance rather than getting a fresh
 * one. Stages are stateful, and what execute() accumulates behind them can be
 * expensive: a source caches the representation it loaded, which for a long
 * capture with a large dropout sidecar is seconds of work that an unrelated
 * edit — deleting a link elsewhere in the graph — would otherwise throw away
 * and pay again. Everything a node actually changed is still built fresh.
 *
 * Pass nullptr (the default) for a DAG that must own its stages outright: a
 * throwaway built only to check the project converts, or the first build of a
 * project. Note that this is not the thread-isolation primitive — a DAG to be
 * executed on another thread wants clone_dag_with_fresh_stages().
 *
 * @param project The project to convert
 * @param previous Previous build of this project to carry unchanged stage
 *                 instances over from, or nullptr to build every stage fresh
 * @return Executable DAG ready for rendering or execution
 * @throws ProjectConversionError if conversion fails
 *
 * Example:
 * ```cpp
 * Project project = load_project("example.orc-project");
 *
 * // Convert to executable DAG
 * auto dag = project_to_dag(project);
 *
 * // Now can render frames
 * DAGFrameRenderer renderer(dag);
 * auto result = renderer.render_frame_at_node("transform_1", FrameID(42));
 * ```
 */
std::shared_ptr<DAG> project_to_dag(const Project& project,
                                    const DAG* previous = nullptr);

/**
 * @brief Resolve the file-path parameters of a node against the project root
 *
 * Project files store paths as the user gave them: absolute, relative to the
 * project directory, or written with ${PROJECT_ROOT}. A stage only ever sees
 * resolved paths, so every consumer of a node's stored parameters has to run
 * them through this before handing them to a stage — otherwise a stage that
 * checks its input file (every source stage does, for its status dot) is told
 * a path that does not resolve from the process working directory and reports
 * itself unconfigured.
 *
 * Parameters whose name does not look like a path are left untouched.
 *
 * @param parameters  Parameter map, modified in place
 * @param project_root Directory the project file lives in; no-op when empty
 */
void resolve_path_parameters(std::map<std::string, ParameterValue>& parameters,
                             const std::string& project_root);

/**
 * @brief Fill in the reserved input-identity parameter for a node
 *
 * A stage that has to know which upstream node feeds each entry of execute()'s
 * inputs vector declares a STRING parameter named orc::kInputNodeIdsParameter;
 * the host is the only thing that can answer that, because artifacts carry no
 * node identity. This writes the source node IDs of the node's incoming
 * connections into that parameter, comma-separated, in input order.
 *
 * Stages that do not declare the parameter are left untouched, so callers can
 * apply this unconditionally to every node.
 *
 * @param stage           Stage instance for the node (its descriptors are read)
 * @param project_format  Project video format, for descriptor lookup
 * @param source_type     Project source type, for descriptor lookup
 * @param input_node_ids  Source node IDs of the incoming connections, in input
 *                        order
 * @param parameters      Parameter map, modified in place
 */
void apply_input_node_ids_parameter(
    const DAGStage& stage, VideoSystem project_format, SourceType source_type,
    const std::vector<NodeID>& input_node_ids,
    std::map<std::string, ParameterValue>& parameters);

/**
 * @brief Validate that all source nodes in a DAG can be accessed
 *
 * This function attempts to execute each source node in the DAG to verify
 * that they can produce output. This is useful for validating that source
 * files exist and can be loaded.
 *
 * @param dag The DAG to validate
 * @throws ProjectConversionError if any source node fails validation
 */
void validate_source_nodes(const std::shared_ptr<DAG>& dag);

}  // namespace orc
