/*
 * File:        node_reset.h
 * Module:      orc-presenters
 * Purpose:     What resetting a stage's parameters gives each of them, shared
 *              by the parameter dialogue's reset button and `orc-cli nodes
 *              reset`
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/node_id.h>
#include <orc/stage/orc_source_parameters.h>
#include <orc/stage/params/parameter_types.h>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "project_presenter_types.h"

namespace orc {
namespace presenters {

/**
 * @brief Stages whose reset restores values read from the node's input
 *
 * Video Parameters resets to what its input reports rather than to the
 * format's standard figures, so that per-capture values follow each capture.
 */
bool stageResetsToSourceMetadata(const std::string& stage_name);

/**
 * @brief The Video Parameters values a source's own figures reset to
 *
 * Keyed by the names that stage publishes. A key the stage does not have
 * resets to the descriptor default instead of the source's value and says
 * nothing about it, so these must stay in step with
 * VideoParamsStage::get_parameter_descriptors().
 */
std::map<std::string, ParameterValue> sourceParametersToVideoParamsStageValues(
    const orc::SourceParameters& params);

/**
 * @brief The node a stage reads its input from
 *
 * The source of the first edge into @p node_id, or the node itself when
 * nothing is connected to it.
 */
NodeID resetInputNode(const std::vector<EdgeInfo>& edges, NodeID node_id);

/**
 * @brief Read the video parameters arriving at a node's input
 *
 * Builds nothing: the DAG is the one the caller already holds for the
 * project. Returns nullopt when the input cannot be read (an unconfigured or
 * missing source, a graph that does not execute).
 *
 * @param project_handle ProjectPresenter::getCoreProjectHandle()
 * @param dag_handle     ProjectPresenter::getDAG() for that project
 */
std::optional<orc::SourceParameters> readInputVideoParameters(
    void* project_handle, std::shared_ptr<void> dag_handle,
    const std::vector<EdgeInfo>& edges, NodeID node_id);

/**
 * @brief The values a reset gives a stage's parameters
 *
 * A parameter takes its entry in @p reset_values when there is one, and its
 * descriptor default otherwise. A parameter with neither is left out, so the
 * caller keeps whatever it holds for it.
 */
std::map<std::string, ParameterValue> resetParameterValues(
    const std::vector<ParameterDescriptor>& descriptors,
    const std::optional<std::map<std::string, ParameterValue>>& reset_values);

/// What resetting one node would do.
struct NodeResetPlan {
  /// The node's parameters after the reset: its stored values with every
  /// value the reset gives laid over them.
  std::map<std::string, ParameterValue> values;
  /// How many parameters the reset gives a value.
  size_t reset_count = 0;
  /// Whether the values came from the node's input rather than the defaults.
  bool from_source_metadata = false;
  /// Set when the node cannot be reset as asked; values is then empty.
  std::string problem;
};

/**
 * @brief Work out what resetting one node gives its parameters
 *
 * A stage that resets to source metadata must be able to read its input:
 * falling back to the format's standard figures would leave a script that
 * expects per-capture values with figures that only look right, so that is a
 * problem rather than a quiet fallback.
 *
 * @param read_input_video_parameters Called only for a stage that resets to
 *        source metadata
 */
NodeResetPlan planNodeReset(
    const std::string& stage_name,
    const std::vector<ParameterDescriptor>& descriptors,
    const std::map<std::string, ParameterValue>& stored,
    const std::function<std::optional<orc::SourceParameters>()>&
        read_input_video_parameters);

}  // namespace presenters
}  // namespace orc
