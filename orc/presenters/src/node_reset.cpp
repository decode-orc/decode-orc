/*
 * File:        node_reset.cpp
 * Module:      orc-presenters
 * Purpose:     What resetting a stage's parameters gives each of them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "node_reset.h"

#include <algorithm>

#include "render_presenter.h"

namespace orc {
namespace presenters {

bool stageResetsToSourceMetadata(const std::string& stage_name) {
  return stage_name == "video_params";
}

std::map<std::string, ParameterValue> sourceParametersToVideoParamsStageValues(
    const orc::SourceParameters& params) {
  // Line numbers are frame-flat on both sides, and the level the stage calls
  // black is the source's black — which sits 7.5 IRE above blanking on NTSC
  // (SMPTE 170M-2004 Table 1), so blanking is not a stand-in for it.
  return {{"activeVideoStart", params.active_video_start},
          {"activeVideoEnd", params.active_video_end},
          {"firstActiveFrameLine", params.first_active_frame_line},
          {"lastActiveFrameLine", params.last_active_frame_line},
          {"whiteLevel", params.white_level},
          {"blackLevel", params.black_level}};
}

NodeID resetInputNode(const std::vector<EdgeInfo>& edges, NodeID node_id) {
  const auto input_edge = std::find_if(
      edges.begin(), edges.end(),
      [&node_id](const EdgeInfo& e) { return e.target_node == node_id; });
  return input_edge != edges.end() ? input_edge->source_node : node_id;
}

std::optional<orc::SourceParameters> readInputVideoParameters(
    void* project_handle, std::shared_ptr<void> dag_handle,
    const std::vector<EdgeInfo>& edges, NodeID node_id) {
  if (!project_handle || !dag_handle) {
    return std::nullopt;
  }
  // The input, not the node: the node's own output already carries whatever
  // this stage does to it.
  RenderPresenter render_presenter(project_handle);
  // Throwaway helper presenter: parameter reads only — no sidecar, scheduler
  // or sweeps, so construction stays cheap on the GUI thread.
  render_presenter.setBackgroundObservationEnabled(false);
  render_presenter.setDAG(std::move(dag_handle));
  return render_presenter.getVideoParameters(resetInputNode(edges, node_id));
}

std::map<std::string, ParameterValue> resetParameterValues(
    const std::vector<ParameterDescriptor>& descriptors,
    const std::optional<std::map<std::string, ParameterValue>>& reset_values) {
  std::map<std::string, ParameterValue> values;
  for (const auto& descriptor : descriptors) {
    if (reset_values) {
      const auto it = reset_values->find(descriptor.name);
      if (it != reset_values->end()) {
        values[descriptor.name] = it->second;
        continue;
      }
    }
    if (descriptor.constraints.default_value) {
      values[descriptor.name] = *descriptor.constraints.default_value;
    }
  }
  return values;
}

NodeResetPlan planNodeReset(
    const std::string& stage_name,
    const std::vector<ParameterDescriptor>& descriptors,
    const std::map<std::string, ParameterValue>& stored,
    const std::function<std::optional<orc::SourceParameters>()>&
        read_input_video_parameters) {
  NodeResetPlan plan;

  std::optional<std::map<std::string, ParameterValue>> reset_values;
  if (stageResetsToSourceMetadata(stage_name)) {
    const auto input = read_input_video_parameters
                           ? read_input_video_parameters()
                           : std::nullopt;
    if (!input) {
      plan.problem =
          "the video parameters arriving at its input could not be read, so "
          "there are no source values to reset to. Check that the source "
          "upstream of it is configured and its files exist.";
      return plan;
    }
    reset_values = sourceParametersToVideoParamsStageValues(*input);
    plan.from_source_metadata = true;
  }

  const auto reset = resetParameterValues(descriptors, reset_values);
  plan.reset_count = reset.size();
  plan.values = stored;
  for (const auto& [name, value] : reset) {
    plan.values[name] = value;
  }
  return plan;
}

}  // namespace presenters
}  // namespace orc
