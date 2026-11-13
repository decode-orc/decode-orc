/*
 * File:        command_stages.cpp
 * Module:      orc-cli
 * Purpose:     Stage introspection subcommand (list / info / help)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "command_stages.h"

#include <stage_ux_strings.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "cli_exit_codes.h"
#include "detail_fields.h"
#include "json_writer.h"
#include "plugin_selector.h"
#include "project_presenter.h"
#include "stage_details.h"
#include "stage_json.h"

namespace orc {
namespace cli {

namespace {

using orc::presenters::ProjectPresenter;
using orc::presenters::StageDetailField;
using orc::presenters::StageInfo;
using orc::presenters::VideoFormat;

/// How many alternatives an unknown stage name is worth offering.
constexpr std::size_t kMaxNearMatches = 5;

/// Parse a video format name, accepting "NTSC", "PAL" or "PAL-M"
/// (case-insensitive; "PAL_M"/"PALM" too), so `list --format` and
/// `info --format` accept exactly the same spellings. Returns false, leaving
/// @p out untouched, for anything else.
bool parse_video_format_name(const std::string& text, VideoFormat* out) {
  std::string upper;
  upper.reserve(text.size());
  for (char c : text) {
    upper += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  if (upper == "NTSC") {
    *out = VideoFormat::NTSC;
    return true;
  }
  if (upper == "PAL") {
    *out = VideoFormat::PAL;
    return true;
  }
  if (upper == "PAL-M" || upper == "PAL_M" || upper == "PALM") {
    *out = VideoFormat::PAL_M;
    return true;
  }
  return false;
}

void print_stages_usage(const char* program_name) {
  std::cerr << "Usage: " << program_name << " stages <subcommand> [options]\n";
  std::cerr << "\n";
  std::cerr << "A <stage> is the stage name 'stages list' prints — the "
               "token a project's\n";
  std::cerr << "'stage:' key holds. Display names are never accepted.\n";
  std::cerr << "\n";
  std::cerr << "Subcommands:\n";
  std::cerr << "  list [options]                 List the stages this build "
               "can run (core stages are hidden)\n";
  std::cerr << "  info <stage> [options]         Describe a stage and every "
               "parameter it takes\n";
  std::cerr << "  help <stage>                   Show the instructions shipped "
               "with a stage\n";
  std::cerr << "\n";
  std::cerr << "Options for 'list':\n";
  std::cerr << "  --kind KIND                    Only source, transform, "
               "analysis or sink stages\n";
  std::cerr << "                                 ('sink' includes the "
               "analysis sinks)\n";
  std::cerr << "  --plugin ID                    Only stages from one plugin "
               "(takes a plugin selector)\n";
  std::cerr << "  --format NTSC|PAL|PAL-M        Only stages usable with that "
               "video format\n";
  std::cerr << "  --core, --all                  Include the core stages that "
               "ship with Decode-Orc\n";
  std::cerr << "\n";
  std::cerr << "Options for 'info':\n";
  std::cerr << "  --format NTSC|PAL|PAL-M        Report the defaults that "
               "format selects\n";
  std::cerr << "\n";
  std::cerr << "Scripting:\n";
  std::cerr << "  --json                         Machine-readable output for "
               "'list' and 'info'; every\n";
  std::cerr << "                                 object carries the stage "
               "name to pass back\n";
}

/// Read every registered stage, optionally restricted to one video format.
std::vector<StageInfo> read_stages(VideoFormat format) {
  return format == VideoFormat::Unknown
             ? ProjectPresenter::getAllStages()
             : ProjectPresenter::getAvailableStages(format);
}

/// Resolve a stage name, reporting the near matches when it does not exist.
/// Returns false when the caller should stop.
bool find_stage_or_report(const std::string& stage_name,
                          const std::vector<StageInfo>& stages,
                          const StageInfo** found) {
  const auto it = std::find_if(
      stages.begin(), stages.end(),
      [&stage_name](const StageInfo& s) { return s.name == stage_name; });
  if (it != stages.end()) {
    *found = &*it;
    return true;
  }

  std::cerr << "Error: " << stage_ux::unknownStageMessage(stage_name) << "\n";
  const auto near = orc::presenters::findNearbyStageNames(stage_name, stages,
                                                          kMaxNearMatches);
  if (!near.empty()) {
    std::cerr << stage_ux::kNearMatchesIntro << "\n";
    for (const auto& name : near) {
      std::cerr << "  " << name << "\n";
    }
  }
  return false;
}

int cmd_stages_list(int argc, char* argv[]) {
  bool show_core = false;
  bool filter_by_kind = false;
  orc::StageCategory kind = orc::StageCategory::SOURCE;
  std::string plugin_id;
  VideoFormat format = VideoFormat::Unknown;
  bool as_json = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    // An option missing its value is reported as such rather than as an
    // unknown option, which is what it would look like from the chain below.
    if ((arg == "--kind" || arg == "--plugin" || arg == "--format") &&
        i + 1 >= argc) {
      std::cerr << "Error: " << arg << " requires a value\n";
      return kExitUsage;
    }

    if (arg == "--core" || arg == "--all") {
      show_core = true;
    } else if (arg == "--json") {
      as_json = true;
    } else if (arg == "--kind") {
      const std::string value = argv[++i];
      if (!orc::presenters::parseStageKind(value, &kind)) {
        std::cerr << "Error: Unknown stage kind: " << value << "\n";
        std::cerr << "Usage: orc-cli stages list --kind "
                     "source|transform|analysis|sink\n";
        return kExitUsage;
      }
      filter_by_kind = true;
    } else if (arg == "--plugin") {
      plugin_id = argv[++i];
    } else if (arg == "--format") {
      const std::string value = argv[++i];
      if (!parse_video_format_name(value, &format)) {
        std::cerr << "Error: Unknown video format: " << value << "\n";
        std::cerr << "Usage: orc-cli stages list --format NTSC|PAL|PAL-M\n";
        return kExitUsage;
      }
    } else {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      std::cerr << "Usage: orc-cli stages list [--kind KIND] [--plugin ID] "
                   "[--format FMT] [--core|--all] [--json]\n";
      return kExitUsage;
    }
  }

  if (as_json) {
    reserve_stdout_for_json();
  }

  const auto stages = read_stages(format);

  // --plugin takes any plugin selector, exactly as `plugins info` does. An
  // owning id printed by this listing matches directly; a path/url/bare form
  // resolves through the presenter to the id it names. A selector that names
  // no plugin at all is an error, not an empty listing.
  if (!plugin_id.empty()) {
    const bool owns_a_stage = std::any_of(
        stages.begin(), stages.end(), [&plugin_id](const StageInfo& stage) {
          return stage.owning_plugin_id == plugin_id;
        });
    if (!owns_a_stage) {
      const auto resolution =
          ProjectPresenter::resolvePluginRegistrySelector(plugin_id);
      if (resolution.status ==
          orc::presenters::PluginSelectorStatus::Ambiguous) {
        std::cerr << "Error: "
                  << orc::presenters::describeAmbiguousPluginSelector(
                         plugin_id, resolution.candidates)
                  << "\n";
        return kExitNotFound;
      }
      if (resolution.status ==
              orc::presenters::PluginSelectorStatus::Resolved &&
          !resolution.entry.plugin_id.empty()) {
        plugin_id = resolution.entry.plugin_id;
      } else {
        std::cerr << "Error: No plugin matching '" << plugin_id
                  << "' owns any registered stage\n";
        return kExitNotFound;
      }
    }
  }

  // Core stages ship with the application, so they are hidden unless asked
  // for — the same default as `plugins list` applies to their plugins.
  std::size_t hidden_core = 0;
  std::vector<const StageInfo*> visible;
  for (const auto& stage : stages) {
    if (filter_by_kind && !orc::presenters::stageMatchesKind(stage, kind)) {
      continue;
    }
    if (!plugin_id.empty() && stage.owning_plugin_id != plugin_id) {
      continue;
    }
    if (stage.is_core_plugin && !show_core) {
      ++hidden_core;
      continue;
    }
    visible.push_back(&stage);
  }

  if (as_json) {
    // A bare array: the command answers one question, and every element is a
    // stage whose `name` the other stage commands accept unchanged.
    JsonWriter json(std::cout);
    json.begin_array();
    for (const auto* stage : visible) {
      json.begin_object();
      write_stage_fields_json(&json, *stage);
      json.end_object();
    }
    json.end_array();
    json.finish();
    return kExitSuccess;
  }

  if (visible.empty()) {
    std::cout << stage_ux::kNoStages << "\n";
  } else {
    std::cout << "Stages (" << visible.size() << "):\n";
    for (const auto* stage : visible) {
      print_detail_fields(orc::presenters::makeStageDetails(*stage), "  ");
      std::cout << "\n";
    }
  }

  if (hidden_core > 0) {
    std::cout << "Note: " << hidden_core
              << " core stage(s) hidden; pass --core to include them.\n";
  }
  return kExitSuccess;
}

int cmd_stages_info(int argc, char* argv[]) {
  std::string stage_name;
  VideoFormat format = VideoFormat::Unknown;
  bool as_json = false;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--format" && i + 1 >= argc) {
      std::cerr << "Error: --format requires a value\n";
      return kExitUsage;
    }

    if (arg == "--json") {
      as_json = true;
    } else if (arg == "--format") {
      const std::string value = argv[++i];
      if (!parse_video_format_name(value, &format)) {
        std::cerr << "Error: Unknown video format: " << value << "\n";
        return kExitUsage;
      }
    } else if (arg.rfind("--", 0) == 0) {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return kExitUsage;
    } else if (stage_name.empty()) {
      stage_name = arg;
    } else {
      std::cerr << "Error: Unexpected argument: " << arg << "\n";
      return kExitUsage;
    }
  }

  if (stage_name.empty()) {
    std::cerr << "Error: 'info' requires a stage name\n";
    std::cerr << "Usage: orc-cli stages info <stage> [--format FMT] "
                 "[--json]\n";
    return kExitUsage;
  }

  if (as_json) {
    reserve_stdout_for_json();
  }

  // Resolve against every stage, not just those the format allows, so a stage
  // that exists but is wrong for the format is reported as such rather than as
  // a spelling mistake.
  const auto stages = ProjectPresenter::getAllStages();
  const StageInfo* stage = nullptr;
  if (!find_stage_or_report(stage_name, stages, &stage)) {
    return kExitNotFound;
  }

  // Parameter descriptors depend on the project's video format, exactly as
  // they do for the GUI's parameter dialog; an empty project reports the
  // format-agnostic defaults unless --format says otherwise.
  ProjectPresenter presenter;
  if (format != VideoFormat::Unknown) {
    presenter.setVideoFormat(format);
  }
  const auto descriptors = presenter.getStageParameters(stage_name);

  if (as_json) {
    // Stage and parameters in one object. The defaults are the stored 0-based
    // values a project file carries, not the 1-based numbers the table
    // presents, because this side is read by a machine.
    JsonWriter json(std::cout);
    json.begin_object();
    write_stage_fields_json(&json, *stage);
    json.key("parameters");
    json.begin_array();
    for (const auto& descriptor : descriptors) {
      write_stage_parameter_json(&json, descriptor);
    }
    json.end_array();
    json.end_object();
    json.finish();
    return kExitSuccess;
  }

  print_detail_fields(orc::presenters::makeStageDetails(*stage));
  std::cout << "\n";

  if (descriptors.empty()) {
    std::cout << stage_ux::kNoParameters << "\n";
    return kExitSuccess;
  }

  // One column for every parameter, so the whole section reads as one table
  // rather than as blocks that each found their own alignment.
  std::vector<std::vector<StageDetailField>> parameter_fields;
  std::size_t width = 0;
  parameter_fields.reserve(descriptors.size());
  for (const auto& descriptor : descriptors) {
    parameter_fields.push_back(
        orc::presenters::makeStageParameterDetails(stage_name, descriptor));
    width = std::max(width, widest_label(parameter_fields.back()));
  }

  std::cout << "Parameters (" << descriptors.size() << "):\n";
  for (std::size_t i = 0; i < descriptors.size(); ++i) {
    std::cout << "  " << descriptors[i].name << "\n";
    print_detail_fields(parameter_fields[i], "    ", width);
    std::cout << "\n";
  }

  if (orc::presenters::stageHasIndexedSpecParameter(stage_name, descriptors)) {
    std::cout << "Note: " << stage_ux::kIndexedSpecNote << "\n";
  }
  return kExitSuccess;
}

int cmd_stages_help(int argc, char* argv[]) {
  std::string stage_name;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--", 0) == 0) {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return kExitUsage;
    }
    if (stage_name.empty()) {
      stage_name = arg;
    } else {
      std::cerr << "Error: Unexpected argument: " << arg << "\n";
      return kExitUsage;
    }
  }

  if (stage_name.empty()) {
    std::cerr << "Error: 'help' requires a stage name\n";
    std::cerr << "Usage: orc-cli stages help <stage>\n";
    return kExitUsage;
  }

  const auto stages = ProjectPresenter::getAllStages();
  const StageInfo* stage = nullptr;
  if (!find_stage_or_report(stage_name, stages, &stage)) {
    return kExitNotFound;
  }

  // The same Markdown the GUI's Help... dialog renders, read at runtime from
  // beside the plugin binary.
  ProjectPresenter presenter;
  const std::string instructions = presenter.getStageInstructions(stage_name);
  if (instructions.empty()) {
    // A stage the user asked about does exist, so this is a documentation gap
    // rather than a lookup failure; report it instead of succeeding silently.
    std::cerr << "Error: " << stage_ux::kNoInstructions << "\n";
    return kExitUsage;
  }

  std::cout << instructions;
  if (instructions.back() != '\n') {
    std::cout << "\n";
  }
  return kExitSuccess;
}

}  // namespace

int stages_command(int argc, char* argv[]) {
  // argv[0] = program name, argv[1] = subcommand
  if (argc < 2) {
    print_stages_usage(argv[0]);
    return kExitUsage;
  }

  const std::string subcommand = argv[1];

  if (subcommand == "--help" || subcommand == "-h") {
    print_stages_usage(argv[0]);
    return kExitSuccess;
  }

  if (subcommand == "list") {
    return cmd_stages_list(argc - 1, argv + 1);
  }

  if (subcommand == "info") {
    return cmd_stages_info(argc - 1, argv + 1);
  }

  if (subcommand == "help") {
    return cmd_stages_help(argc - 1, argv + 1);
  }

  std::cerr << "Error: Unknown stages subcommand: " << subcommand << "\n\n";
  print_stages_usage(argv[0]);
  return kExitUsage;
}

}  // namespace cli
}  // namespace orc
