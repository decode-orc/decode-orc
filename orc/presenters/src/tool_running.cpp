/*
 * File:        tool_running.cpp
 * Module:      orc-presenters
 * Purpose:     Checks and helpers for running stage tools without the GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "tool_running.h"

#include <algorithm>
#include <deque>
#include <set>
#include <utility>

#include "../core/analysis/analysis_registry.h"
#include "../core/analysis/analysis_tool.h"
#include "project_presenter.h"

namespace orc {
namespace presenters {

namespace {

bool endsWith(const std::string& text, const std::string& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string describeNode(const NodeInfo& node) {
  return "'" + node.label + "' (" + node.stage_name + ")";
}

std::string describeNodes(const std::vector<const NodeInfo*>& nodes) {
  std::string text;
  for (const auto* node : nodes) {
    if (!text.empty()) {
      text += ", ";
    }
    text += describeNode(*node);
  }
  return text;
}

}  // namespace

ToolRunSupport toolRunSupport(const orc::AnalysisToolInfo& tool) {
  const std::string& kind = tool.stage_tool_kind;
  if (kind == "batch_analysis") {
    return {ToolRunMode::BatchAnalysis, ""};
  }
  if (kind == "config_dialog") {
    return {ToolRunMode::GuiOnly,
            "'" + tool.id +
                "' only picks a preset in the GUI; set the stage's "
                "parameters with 'orc-cli nodes set' instead"};
  }
  if (kind == "non_modal_editor") {
    return {ToolRunMode::GuiOnly, "'" + tool.id +
                                      "' is an interactive editor; use " +
                                      tool.name + " in the GUI"};
  }
  if (kind == "preview_utility") {
    return {ToolRunMode::GuiOnly, "'" + tool.id + "' is a preview view; use " +
                                      tool.name + " in the GUI"};
  }
  if (kind == "catalogue_browser") {
    return {ToolRunMode::GuiOnly,
            "'" + tool.id + "' browses results in the GUI (" + tool.name +
                "); the stage writes its files when the project is processed "
                "with --process"};
  }
  if (!kind.empty()) {
    return {ToolRunMode::GuiOnly, "'" + tool.id +
                                      "' has no command-line form; use " +
                                      tool.name + " in the GUI"};
  }
  return {ToolRunMode::Analysis, ""};
}

bool toolCanApplyToGraph(const std::string& tool_id) {
  const auto* tool = orc::AnalysisRegistry::instance().findById(tool_id);
  return tool && tool->canApplyToGraph();
}

std::string toolKindName(const orc::AnalysisToolInfo& tool) {
  return tool.stage_tool_kind.empty() ? "analysis" : tool.stage_tool_kind;
}

std::map<std::string, ParameterValue> withParameterDefaults(
    const std::vector<ParameterDescriptor>& descriptors,
    std::map<std::string, ParameterValue> values) {
  for (const auto& descriptor : descriptors) {
    if (descriptor.constraints.default_value &&
        values.find(descriptor.name) == values.end()) {
      values.emplace(descriptor.name, *descriptor.constraints.default_value);
    }
  }
  return values;
}

bool isStreamSourceStage(const std::string& stage_name) {
  return endsWith(stage_name, "_stream_source");
}

std::vector<NodeID> upstreamNodes(const std::vector<EdgeInfo>& edges,
                                  NodeID node_id) {
  std::vector<NodeID> found;
  std::set<NodeID> seen{node_id};
  std::deque<NodeID> pending{node_id};
  while (!pending.empty()) {
    const NodeID current = pending.front();
    pending.pop_front();
    for (const auto& edge : edges) {
      if (edge.target_node == current && seen.insert(edge.source_node).second) {
        found.push_back(edge.source_node);
        pending.push_back(edge.source_node);
      }
    }
  }
  return found;
}

std::optional<std::string> checkToolUpstream(
    const std::vector<NodeInfo>& nodes, const std::vector<EdgeInfo>& edges,
    NodeID node_id, const std::function<bool(NodeID)>& source_configured) {
  std::vector<const NodeInfo*> sources;
  for (const auto& id : upstreamNodes(edges, node_id)) {
    const bool fed =
        std::any_of(edges.begin(), edges.end(),
                    [&id](const EdgeInfo& e) { return e.target_node == id; });
    if (fed) {
      continue;
    }
    const auto it =
        std::find_if(nodes.begin(), nodes.end(),
                     [&id](const NodeInfo& n) { return n.node_id == id; });
    if (it != nodes.end()) {
      sources.push_back(&*it);
    }
  }

  std::vector<const NodeInfo*> streams;
  std::vector<const NodeInfo*> unconfigured;
  std::vector<const NodeInfo*> tbc;
  std::vector<const NodeInfo*> cvbs;
  for (const auto* source : sources) {
    if (isStreamSourceStage(source->stage_name)) {
      streams.push_back(source);
      continue;
    }
    if (source_configured && !source_configured(source->node_id)) {
      unconfigured.push_back(source);
    }
    if (source->stage_name == "tbc_source") {
      tbc.push_back(source);
    } else if (ProjectPresenter::isCVBSSourceStage(source->stage_name)) {
      cvbs.push_back(source);
    }
  }

  if (!streams.empty()) {
    return "Stage tools cannot read from a stream source (" +
           describeNodes(streams) +
           "): a stream can be read only once, and the tools read their "
           "input more than once. Decode to files first and use a file "
           "source.";
  }
  if (!unconfigured.empty()) {
    return "Source not configured: " + describeNodes(unconfigured) +
           ". Set its input paths with 'orc-cli nodes set' first.";
  }
  if (!tbc.empty() && !cvbs.empty()) {
    return "A stack cannot mix .tbc and .cvbs captures. TBC sources: " +
           describeNodes(tbc) + "; CVBS sources: " + describeNodes(cvbs) + ".";
  }
  return std::nullopt;
}

}  // namespace presenters
}  // namespace orc
