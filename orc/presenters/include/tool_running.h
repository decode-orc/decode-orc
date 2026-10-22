/*
 * File:        tool_running.h
 * Module:      orc-presenters
 * Purpose:     Checks and helpers for running stage tools without the GUI
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/node_id.h>
#include <orc/stage/params/parameter_types.h>
#include <orc_analysis.h>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "project_presenter_types.h"

namespace orc {
namespace presenters {

/// How a stage tool runs when there is no dialog to drive it.
enum class ToolRunMode {
  /// Runs an analysis whose result can be reported and, if the tool supports
  /// it, applied to the node (Disc Mapper, Source Alignment, ...).
  Analysis,
  /// Triggers the analysis sink the tool belongs to and reports its results
  /// (dropout, SNR and burst level analysis).
  BatchAnalysis,
  /// Needs a person at the GUI: an editor, a browser or a preset picker.
  GuiOnly
};

struct ToolRunSupport {
  ToolRunMode mode = ToolRunMode::Analysis;
  /// For GuiOnly: why, and what to use instead.
  std::string reason;
};

/// How @p tool runs without the GUI.
ToolRunSupport toolRunSupport(const orc::AnalysisToolInfo& tool);

/// True when the registered tool @p tool_id can write its result to a node.
bool toolCanApplyToGraph(const std::string& tool_id);

/// Short name of a tool's kind for listings: "analysis", "batch_analysis",
/// "config_dialog", "non_modal_editor", "preview_utility" or
/// "catalogue_browser".
std::string toolKindName(const orc::AnalysisToolInfo& tool);

/**
 * @brief Fill in the defaults a tool dialog would start from
 *
 * Every descriptor with a default and no value in @p values gets its default,
 * so a tool run with no parameters behaves as the dialog does when Run is
 * pressed straight away.
 */
std::map<std::string, ParameterValue> withParameterDefaults(
    const std::vector<ParameterDescriptor>& descriptors,
    std::map<std::string, ParameterValue> values);

/// True for the stages that read a pipe or standard input once, in order.
bool isStreamSourceStage(const std::string& stage_name);

/// Every node @p node_id reads from, directly or indirectly, nearest first.
/// The node itself is not included.
std::vector<NodeID> upstreamNodes(const std::vector<EdgeInfo>& edges,
                                  NodeID node_id);

/**
 * @brief Why a tool cannot run on @p node_id, or nothing if it can
 *
 * Refuses, before any analysis starts:
 * - a stream source upstream, which can be read only once;
 * - an upstream source that is not configured (@p source_configured false);
 * - `.tbc` and `.cvbs` sources feeding the same node, which no stack may mix.
 *
 * Sources are the upstream nodes nothing feeds.
 */
std::optional<std::string> checkToolUpstream(
    const std::vector<NodeInfo>& nodes, const std::vector<EdgeInfo>& edges,
    NodeID node_id, const std::function<bool(NodeID)>& source_configured);

}  // namespace presenters
}  // namespace orc
