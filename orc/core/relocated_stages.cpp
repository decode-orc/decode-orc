/*
 * File:        relocated_stages.cpp
 * Module:      orc-core
 * Purpose:     Stages that moved out of decode-orc into a plugin, and the
 *              message a project that uses one gets when it is missing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "relocated_stages.h"

#include <algorithm>
#include <sstream>

namespace orc {

namespace {

// Issue #377: the teletext and NABTS sinks, and the WST/NABTS decoding library
// behind them, moved to their own repository.
constexpr const char* kTeletextPluginId = "org.decodeorc.stage.teletext";
constexpr const char* kTeletextPluginRepo =
    "https://github.com/decode-orc/orc-plugin_teletext";

}  // namespace

const std::vector<RelocatedStage>& relocated_stages() {
  static const std::vector<RelocatedStage> stages = {
      {"teletext_sink", kTeletextPluginId, kTeletextPluginRepo},
      {"nabts_sink", kTeletextPluginId, kTeletextPluginRepo},
  };
  return stages;
}

std::optional<ProjectPluginRequirement> relocated_stage_requirement(
    const std::string& stage_name) {
  const auto& stages = relocated_stages();
  const auto it = std::find_if(stages.begin(), stages.end(),
                               [&stage_name](const RelocatedStage& stage) {
                                 return stage_name == stage.stage_name;
                               });
  if (it == stages.end()) {
    return std::nullopt;
  }

  ProjectPluginRequirement requirement;
  requirement.plugin_id = it->plugin_id;
  requirement.source_repo_url = it->source_repo_url;
  requirement.artifact_source = "release_asset";
  requirement.stage_names = {stage_name};
  return requirement;
}

std::string missing_stage_message(
    const std::vector<ProjectPluginRequirement>& required_plugins,
    const std::string& stage_name, NodeID node_id) {
  std::optional<ProjectPluginRequirement> requirement;
  for (const auto& candidate : required_plugins) {
    if (std::find(candidate.stage_names.begin(), candidate.stage_names.end(),
                  stage_name) != candidate.stage_names.end()) {
      requirement = candidate;
      break;
    }
  }
  if (!requirement) {
    requirement = relocated_stage_requirement(stage_name);
  }

  std::ostringstream oss;
  oss << "Unknown stage type: " << stage_name << " in node " << node_id.value();
  if (!requirement) {
    return oss.str();
  }

  if (!requirement->plugin_id.empty()) {
    oss << ". Required plugin: " << requirement->plugin_id;
  }

  if (!requirement->source_repo_url.empty()) {
    oss << ". Source: " << requirement->source_repo_url;
  } else if (!requirement->release_asset_url.empty()) {
    oss << ". Release asset: " << requirement->release_asset_url;
  }

  if (requirement->plugin_id.empty()) {
    oss << ". Install or re-enable the plugin, then reload the project.";
  } else {
    oss << ". Install it from Plugin Manager > Browse Plugins... or with "
           "'orc-cli plugins install "
        << requirement->plugin_id
        << "' (or re-enable it if it is installed), then reload the project.";
  }
  return oss.str();
}

}  // namespace orc
