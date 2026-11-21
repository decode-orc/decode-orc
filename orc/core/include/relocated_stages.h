/*
 * File:        relocated_stages.h
 * Module:      orc-core
 * Purpose:     Stages that moved out of decode-orc into a plugin, and the
 *              message a project that uses one gets when it is missing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

// =============================================================================
// MVP Architecture Enforcement
// =============================================================================
// This header is part of the CORE internal implementation.
// =============================================================================
#if defined(ORC_GUI_BUILD)
#error "GUI code cannot include core/include/relocated_stages.h."
#endif
#if defined(ORC_CLI_BUILD)
#error "CLI code cannot include core/include/relocated_stages.h."
#endif

#include <orc/stage/node_id.h>

#include <optional>
#include <string>
#include <vector>

#include "project.h"

namespace orc {

// A stage that used to be built into decode-orc and now ships in a plugin.
struct RelocatedStage {
  const char* stage_name;
  const char* plugin_id;
  const char* source_repo_url;
};

// Every relocated stage. A project saved while one of these was built in
// carries no required_plugins entry for it, because built-in stages are never
// recorded (collect_required_plugins_for_project skips core plugins); this
// table is what names the plugin for such a project.
//
// Thread safety: returns a reference to immutable static data.
const std::vector<RelocatedStage>& relocated_stages();

// The plugin requirement for |stage_name| when it is a relocated stage, with
// that stage as its only stage name; std::nullopt otherwise.
std::optional<ProjectPluginRequirement> relocated_stage_requirement(
    const std::string& stage_name);

// The error for a project node whose stage is not registered.
//
// The requirement is the project's required_plugins entry that lists the
// stage; when there is none, it is taken from the relocated-stage table. With
// a requirement, the message names the plugin, where it comes from, and how
// to install it (Plugin Manager or orc-cli). With neither, it says only that
// the stage is unknown.
std::string missing_stage_message(
    const std::vector<ProjectPluginRequirement>& required_plugins,
    const std::string& stage_name, NodeID node_id);

}  // namespace orc
