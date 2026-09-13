/*
 * File:        command_process.cpp
 * Module:      orc-cli
 * Purpose:     Process DAG command
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "command_process.h"

#include <filesystem>
#include <iostream>

#include "logging.h"
#include "project_presenter.h"

namespace fs = std::filesystem;

namespace orc {
namespace cli {

namespace {

std::string format_plugin_diagnostic_message(
    const orc::presenters::PluginDiagnosticInfo& diagnostic) {
  if (diagnostic.path.empty()) {
    return diagnostic.message;
  }

  return diagnostic.message + " [" + diagnostic.path + "]";
}

void log_plugin_runtime_state(
    const orc::presenters::ProjectPresenter& presenter) {
  const auto search_paths = presenter.listPluginSearchPaths();
  const auto registry = presenter.getPluginRegistry();

  if (!registry.registry_path.empty()) {
    ORC_LOG_DEBUG("Plugin registry path: {}", registry.registry_path);
  }

  if (!registry.entries.empty()) {
    ORC_LOG_DEBUG("Configured {} plugin registry entr{}",
                  registry.entries.size(),
                  registry.entries.size() == 1 ? "y" : "ies");
    for (const auto& entry : registry.entries) {
      ORC_LOG_DEBUG(
          "  registry entry '{}' enabled={} loaded={} exists={} path='{}'",
          entry.selector, entry.enabled ? "true" : "false",
          entry.is_loaded ? "true" : "false",
          entry.path_exists ? "true" : "false", entry.path);
    }
  }

  if (!search_paths.empty()) {
    ORC_LOG_DEBUG("Configured runtime stage plugin search paths: {}",
                  search_paths.size());
    for (const auto& path : search_paths) {
      ORC_LOG_DEBUG("  plugin search path: {}", path);
    }
  }

  const auto loaded_plugins = presenter.listLoadedPlugins();
  if (!loaded_plugins.empty()) {
    ORC_LOG_DEBUG("Loaded {} runtime stage plugin(s)", loaded_plugins.size());
    for (const auto& plugin : loaded_plugins) {
      ORC_LOG_DEBUG("  plugin '{}' version '{}' registered {} stage(s)",
                    plugin.plugin_id, plugin.plugin_version,
                    plugin.registered_stage_names.size());
    }
  }

  for (const auto& diagnostic : presenter.listPluginDiagnostics()) {
    const std::string message = format_plugin_diagnostic_message(diagnostic);
    switch (diagnostic.severity) {
      case orc::presenters::PluginDiagnosticSeverity::Info:
        ORC_LOG_DEBUG("Plugin runtime: {}", message);
        break;
      case orc::presenters::PluginDiagnosticSeverity::Warning:
        ORC_LOG_WARN("Plugin runtime: {}", message);
        break;
      case orc::presenters::PluginDiagnosticSeverity::Error:
        ORC_LOG_ERROR("Plugin runtime: {}", message);
        break;
    }
  }
}

}  // namespace

int process_command(const ProcessOptions& options) {
  // Check if file exists
  if (!fs::exists(options.project_path)) {
    ORC_LOG_ERROR("Project file not found: {}", options.project_path);
    return 1;
  }

  ORC_LOG_INFO("Loading project: {}", options.project_path);

  // Create presenter and load project
  orc::presenters::ProjectPresenter presenter;
  try {
    if (!presenter.loadProject(options.project_path)) {
      ORC_LOG_ERROR("Failed to load project: {}", options.project_path);
      return 1;
    }
  } catch (const std::exception& e) {
    ORC_LOG_ERROR("Failed to load project: {}", e.what());
    return 1;
  }

  log_plugin_runtime_state(presenter);

  ORC_LOG_INFO("Project loaded: {}", presenter.getProjectName());
  if (!presenter.getProjectDescription().empty()) {
    ORC_LOG_DEBUG("Project description: {}", presenter.getProjectDescription());
  }
  auto nodes = presenter.getNodes();
  auto edges = presenter.getEdges();
  ORC_LOG_DEBUG("Project contains {} nodes and {} edges", nodes.size(),
                edges.size());

  // CLI-only pre-flight check for the "-" stdio piping convention (see
  // docs/technical/plugin-architecture.md, "Stdio Piping Convention"). A
  // no-op for the overwhelmingly common case of a project that never uses
  // "-" at all.
  const auto pipe_errors = presenter.validatePipeExecution();
  if (!pipe_errors.empty()) {
    for (const auto& error : pipe_errors) {
      ORC_LOG_ERROR("{}", error);
    }
    return 1;
  }

  // Set up progress callback for console output
  size_t last_percent = 0;
  auto progress_callback = [&last_percent](size_t current, size_t total,
                                           const std::string& message) {
    if (total > 0) {
      size_t percent = (current * 100) / total;
      // Only log on significant progress change (every 5%)
      if (percent >= last_percent + 5 || current == total) {
        ORC_LOG_INFO("[Progress: {}%] {}", percent, message);
        last_percent = percent;
      }
    }
  };

  // Trigger all sink nodes using presenter
  bool all_success = presenter.triggerAllSinks(progress_callback);

  if (all_success) {
    return 0;
  } else {
    return 1;
  }
}

}  // namespace cli
}  // namespace orc
