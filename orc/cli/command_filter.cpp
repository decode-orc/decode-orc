/*
 * File:        command_filter.cpp
 * Module:      orc-cli
 * Purpose:     Build and process a DAG from a source/filters/sink triad.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "command_filter.h"

#include <orc/support/pipe_io.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "filtergraph_import.h"
#include "filtergraph_parser.h"
#include "logging.h"
#include "project_presenter.h"
#include "project_presenter_types.h"
#include "stage_category.h"

namespace orc {
namespace cli {

namespace {

using orc::presenters::FilterGraphParseResult;
using orc::presenters::parse_filtergraph;
using orc::presenters::ProjectPresenter;
using orc::presenters::SourceType;
using orc::presenters::StageInfo;
using orc::presenters::VideoFormat;

/**
 * Parse a source type name for --source-type (export-only override).
 * Accepts "composite" or "yc" (case-insensitive; "y/c" and "s-video" also
 * accepted for convenience). Returns false, leaving `out` untouched, for
 * anything else.
 */
bool parse_source_type_name(const std::string& text, SourceType& out) {
  std::string lower;
  lower.reserve(text.size());
  for (char c : text) {
    lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (lower == "composite" || lower == "cvbs") {
    out = SourceType::Composite;
    return true;
  }
  if (lower == "yc" || lower == "y/c" || lower == "svideo" ||
      lower == "s-video") {
    out = SourceType::YC;
    return true;
  }
  return false;
}

/**
 * Validate that every stage named in `segment` belongs to `category`
 * (input segments must be sources, filters segments must be neither
 * source nor sink, output segments must be sinks). A stage's category is
 * never a guess: it comes straight from the same StageInfo the GUI uses, so
 * this works identically for core stages and for any third-party plugin
 * stage. Returns true (and leaves `error` untouched) when `segment` is empty
 * or every stage matches; otherwise returns false with `error` set.
 */
bool validate_triad_segment(const std::string& segment, StageCategory category,
                            const std::map<std::string, StageInfo>& stage_index,
                            std::string& error) {
  if (segment.empty()) {
    return true;
  }
  FilterGraphParseResult parsed = parse_filtergraph(segment);
  if (!parsed.ok) {
    error = std::string(category_flag(category)) + ": " + parsed.error;
    return false;
  }
  for (const auto& stage : parsed.graph.stages) {
    auto it = stage_index.find(stage.stage_name);
    if (it == stage_index.end()) {
      error = std::string(category_flag(category)) + ": unknown stage '" +
              stage.stage_name +
              "'. Check the exact name in the GUI's stage palette or the "
              "stage's own documentation.";
      return false;
    }
    if (!stage_matches_category(it->second, category)) {
      error = std::string(category_flag(category)) + ": stage '" +
              stage.stage_name + "' (" + it->second.display_name + ") is " +
              actual_role_description(it->second) + " — it belongs under " +
              suggested_flag_for(it->second) + ", not " +
              category_flag(category) + ".";
      return false;
    }
  }
  return true;
}

/// Log runtime plugin diagnostics so stage-loading problems are visible, in the
/// same spirit as the process command.
void log_plugin_diagnostics(const ProjectPresenter& presenter) {
  const auto loaded = presenter.listLoadedPlugins();
  if (!loaded.empty()) {
    ORC_LOG_DEBUG("Loaded {} runtime stage plugin(s)", loaded.size());
  }
  for (const auto& diagnostic : presenter.listPluginDiagnostics()) {
    const std::string message =
        diagnostic.path.empty()
            ? diagnostic.message
            : diagnostic.message + " [" + diagnostic.path + "]";
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

int filter_command(const FilterOptions& options) {
  const auto stage_index = build_stage_index();

  std::string error;
  if (!validate_triad_segment(options.input_stages, StageCategory::kInput,
                              stage_index, error) ||
      !validate_triad_segment(options.filters_stages, StageCategory::kFilters,
                              stage_index, error) ||
      !validate_triad_segment(options.output_stages, StageCategory::kOutput,
                              stage_index, error)) {
    ORC_LOG_ERROR("{}", error);
    return 1;
  }

  std::vector<std::string> segments;
  for (const auto& segment :
       {options.input_stages, options.filters_stages, options.output_stages}) {
    if (!segment.empty()) {
      segments.push_back(segment);
    }
  }
  std::string combined_graph;
  for (size_t i = 0; i < segments.size(); ++i) {
    if (i > 0) {
      combined_graph += ",";
    }
    combined_graph += segments[i];
  }

  ProjectPresenter presenter;
  presenter.setProjectName("filtergraph");

  // Parsing, stage/parameter validation, video-format/source-type
  // auto-detection, and node/edge construction are all shared with the GUI
  // (any "paste a CLI command" feature would call the very same function).
  const orc::presenters::FiltergraphImportResult import_result =
      orc::presenters::import_filtergraph_into_project(presenter,
                                                       combined_graph);

  for (const auto& warning : import_result.warnings) {
    ORC_LOG_WARN("{}", warning);
  }
  if (!import_result.ok) {
    for (const auto& error : import_result.errors) {
      ORC_LOG_ERROR("{}", error);
    }
    return 1;
  }

  ORC_LOG_INFO("Parsed filtergraph successfully");
  log_plugin_diagnostics(presenter);

  // If the user supplied an explicit --video-format/--source-type and none
  // of the stages implied one, apply it now — regardless of whether we're
  // about to export or run directly. This is what makes a graph decoded
  // directly with --video-format behave identically to the same graph
  // exported then processed with --process: both see the same format-
  // specific parameter defaults (project_to_dag.cpp selects them from
  // video_format/source_type), rather than only the exported/reprocessed
  // path ever getting a concrete value.
  if (presenter.getVideoFormat() == orc::presenters::VideoFormat::Unknown &&
      !options.video_format_override.empty()) {
    orc::presenters::VideoFormat forced = orc::presenters::VideoFormat::Unknown;
    if (!parse_video_format_name(options.video_format_override, &forced)) {
      ORC_LOG_ERROR(
          "Unknown --video-format value '{}': expected NTSC, PAL, or PAL-M",
          options.video_format_override);
      return 1;
    }
    presenter.setVideoFormat(forced);
  }
  if (presenter.getSourceFormat() == SourceType::Unknown &&
      !options.source_type_override.empty()) {
    SourceType forced_source = SourceType::Unknown;
    if (!parse_source_type_name(options.source_type_override, forced_source)) {
      ORC_LOG_ERROR(
          "Unknown --source-type value '{}': expected composite or yc",
          options.source_type_override);
      return 1;
    }
    presenter.setSourceType(forced_source);
  }

  // --export-project: save the assembled project instead of running it.
  // This calls the presenter's existing saveProject() (the same YAML writer
  // used elsewhere), so it's a different entry point into the existing save
  // path, not a new one.
  if (!options.export_project_path.empty()) {
    // Running in memory tolerates an undetermined video format (the core's
    // own compatibility checks are simply skipped), but a saved .orcprj file
    // is not: the loader requires an explicit NTSC/PAL/PAL-M value and
    // rejects a project without one. The override above already applied
    // --video-format if one was given; this only fires when none of the
    // stages implied a format *and* no override was supplied either.
    if (presenter.getVideoFormat() == orc::presenters::VideoFormat::Unknown) {
      ORC_LOG_ERROR(
          "Cannot export: none of the stages used imply a video format "
          "(NTSC/PAL/PAL-M), so the saved project would fail to reload. "
          "Pass --video-format NTSC|PAL|PAL-M, or run the pipeline "
          "directly instead of exporting.");
      return 1;
    }

    // Mirror the same guard for source signal type: the loader also
    // hard-requires 'source_format' even though running in memory never
    // needs it, and infer_source_format() can only conclude anything when a
    // source stage's parameters actually reveal it (see
    // filtergraph_import.cpp) — some legitimate graphs give no such hint.
    if (presenter.getSourceFormat() == SourceType::Unknown) {
      ORC_LOG_ERROR(
          "Cannot export: none of the stages used imply a source signal "
          "type (composite/Y-C), so the saved project would fail to "
          "reload. Pass --source-type composite|yc, or run the pipeline "
          "directly instead of exporting.");
      return 1;
    }

    // A saved .orcprj resolves relative file-path parameters against the
    // *file's own* directory on reload (see Project::project_root_), not
    // the directory the export command was run from. Absolutise any
    // relative FILE_PATH parameter now, while it still means what the
    // person typed, so the path doesn't silently change meaning once saved
    // elsewhere.
    for (const auto& node : presenter.getNodes()) {
      const auto descriptors = presenter.getStageParameters(node.stage_name);
      auto params = presenter.getNodeParameters(node.node_id);
      bool changed = false;
      // Named rather than destructured: the descriptor lookup below captures
      // the key, and capturing a structured binding is only legal from C++20.
      for (auto& entry : params) {
        const auto& key = entry.first;
        auto& value = entry.second;
        const auto descriptor_it =
            std::find_if(descriptors.begin(), descriptors.end(),
                         [&](const auto& d) { return d.name == key; });
        if (descriptor_it == descriptors.end() ||
            descriptor_it->type != orc::ParameterType::FILE_PATH ||
            !std::holds_alternative<std::string>(value)) {
          continue;
        }
        const std::string& path_str = std::get<std::string>(value);
        // The "-" stdio convention and live network stream URLs are
        // sentinels, not relative filesystem paths — absolutising either
        // would silently turn it into a real (nonsense) path and break
        // every pipe-aware stage on reload, the same mistake already fixed
        // in project_to_dag's resolve_path_for_execution().
        if (path_str.empty() || path_str == orc::pipe_io::kStdioPathToken ||
            orc::pipe_io::is_network_stream_url(path_str)) {
          continue;
        }
        const std::filesystem::path path(path_str);
        if (path.is_relative()) {
          value = std::filesystem::absolute(path).lexically_normal().string();
          changed = true;
        }
      }
      if (changed) {
        presenter.setNodeParameters(node.node_id, params);
      }
    }

    if (!presenter.saveProject(options.export_project_path)) {
      ORC_LOG_ERROR("Failed to save project to '{}'",
                    options.export_project_path);
      return 1;
    }
    ORC_LOG_INFO("Project saved to '{}'", options.export_project_path);
    return 0;
  }

  // Validate the assembled project before running.
  if (!presenter.validateProject()) {
    for (const auto& error : presenter.getValidationErrors()) {
      ORC_LOG_ERROR("Validation: {}", error);
    }
    ORC_LOG_ERROR("Filtergraph did not produce a valid project");
    return 1;
  }

  // Trigger all sinks with console progress reporting.
  size_t last_percent = 0;
  auto progress_callback = [&last_percent](size_t current, size_t total,
                                           const std::string& message) {
    if (total > 0) {
      size_t percent = (current * 100) / total;
      if (percent >= last_percent + 5 || current == total) {
        ORC_LOG_INFO("[Progress: {}%] {}", percent, message);
        last_percent = percent;
      }
    }
  };

  const bool all_success = presenter.triggerAllSinks(progress_callback);
  return all_success ? 0 : 1;
}

}  // namespace cli
}  // namespace orc
