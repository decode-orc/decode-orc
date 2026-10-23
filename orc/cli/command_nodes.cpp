/*
 * File:        command_nodes.cpp
 * Module:      orc-cli
 * Purpose:     Project node subcommand (list / get / set / reset)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "command_nodes.h"

#include <algorithm>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "analysis_presenter.h"
#include "cli_exit_codes.h"
#include "detail_fields.h"
#include "json_writer.h"
#include "node_editing.h"
#include "node_reset.h"
#include "project_args.h"
#include "project_presenter.h"
#include "stage_details.h"
#include "tool_refresh.h"

namespace orc {
namespace cli {

namespace {

using orc::presenters::NodeInfo;
using orc::presenters::ProjectPresenter;

/// One label/value line of a node's text listing.
struct Field {
  std::string label;
  std::string value;
};

void print_nodes_usage(const char* program_name) {
  std::cerr << "Usage: " << program_name
            << " nodes <subcommand> <project> [options]\n";
  std::cerr << "\n";
  std::cerr << "A <node> is a stage's id or its name (the label shown on the "
               "stage). Names are\n";
  std::cerr << "unique within a project and never made only of digits, so "
               "either form names\n";
  std::cerr << "exactly one stage.\n";
  std::cerr << "\n";
  std::cerr << "Subcommands:\n";
  std::cerr << "  list <project> [--json]        List the project's stages "
               "with their status and tools\n";
  std::cerr << "  get <project> <node> [--json]  Show a stage's parameters\n";
  std::cerr
      << "  set <project> <node> key=value [...]  Set parameters and save "
         "the project\n";
  std::cerr << "  reset <project> <node>         Reset a stage's parameters "
               "and save the project\n";
  std::cerr << "\n";
  std::cerr << "Options for 'set' and 'reset':\n";
  std::cerr << "  --output FILE                  Save to FILE instead of "
               "overwriting <project>\n";
  std::cerr << "\n";
  std::cerr << "Values are converted to the type each parameter declares and "
               "checked against\n";
  std::cerr << "its range and allowed values; an unknown key is refused. "
               "Values are stored as\n";
  std::cerr << "given, so relative and ${PROJECT_ROOT} paths stay relative to "
               "the saved file.\n";
  std::cerr << "\n";
  std::cerr << "'reset' does what the stage's Reset button does: each "
               "parameter takes its\n";
  std::cerr << "default, and one with no default keeps its value. Video "
               "Parameters takes the\n";
  std::cerr << "values its input reports instead, and is refused (exit 5) "
               "when its input\n";
  std::cerr << "cannot be read.\n";
  std::cerr << "\n";
  std::cerr << "Scripting:\n";
  std::cerr << "  --json                         Machine-readable output for "
               "'list' and 'get'\n";
}

std::string source_format_name(orc::presenters::SourceType type) {
  switch (type) {
    case orc::presenters::SourceType::Composite:
      return "composite";
    case orc::presenters::SourceType::YC:
      return "Y/C";
    case orc::presenters::SourceType::Unknown:
      break;
  }
  return "source format unknown";
}

std::vector<std::string> tool_ids_for(ProjectPresenter* presenter,
                                      const std::string& stage_name) {
  orc::presenters::AnalysisPresenter analysis(
      presenter->getCoreProjectHandle());
  std::vector<std::string> ids;
  for (const auto& tool : analysis.getToolsForStage(stage_name)) {
    ids.push_back(tool.id);
  }
  return ids;
}

std::string join(const std::vector<std::string>& items) {
  std::string joined;
  for (const auto& item : items) {
    if (!joined.empty()) {
      joined += ", ";
    }
    joined += item;
  }
  return joined;
}

/// Fields every node description starts with, shared by `list` and `get`.
void write_node_identity_json(JsonWriter* json, const NodeInfo& node,
                              const ProjectPresenter& presenter) {
  json->member_int("id", node.node_id.value());
  json->member("label", node.label);
  json->member("stage", node.stage_name);
  json->member("status",
               orc::presenters::configurationStatusName(
                   presenter.getNodeConfigurationStatus(node.node_id)));
}

/// The node's `tool_refresh` block as JSON, or null when it has none.
void write_tool_refresh_json(JsonWriter* json, const NodeInfo& node,
                             ProjectPresenter* presenter) {
  const auto refresh = orc::presenters::getNodeToolRefresh(
      presenter->getCoreProjectHandle(), node.node_id);
  if (!refresh) {
    json->member_null("tool_refresh");
    return;
  }
  json->key("tool_refresh");
  json->begin_object();
  json->member("tool", refresh->tool_id);
  json->key("parameters");
  json->begin_object();
  for (const auto& [name, value] : refresh->parameters) {
    write_parameter_value_json(json, name, value);
  }
  json->end_object();
  json->end_object();
}

/// The node's refresh tool as one line of text, or empty when it has none.
std::string tool_refresh_text(const NodeInfo& node,
                              ProjectPresenter* presenter) {
  const auto refresh = orc::presenters::getNodeToolRefresh(
      presenter->getCoreProjectHandle(), node.node_id);
  if (!refresh) {
    return {};
  }
  std::string text = refresh->tool_id;
  std::vector<std::string> parameters;
  for (const auto& [name, value] : refresh->parameters) {
    parameters.push_back(name + "=" +
                         orc::parameter_util::value_to_string(value));
  }
  if (!parameters.empty()) {
    text += " (" + join(parameters) + ")";
  }
  return text;
}

/// Parse the options shared by `list` and `get`: positional arguments and
/// --json. Returns false (having reported why) on an unknown option.
bool parse_query_args(int argc, char* argv[], std::vector<std::string>* args,
                      bool* as_json) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      *as_json = true;
    } else if (arg.rfind("--", 0) == 0) {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return false;
    } else {
      args->push_back(arg);
    }
  }
  return true;
}

int cmd_nodes_list(int argc, char* argv[]) {
  std::vector<std::string> args;
  bool as_json = false;
  if (!parse_query_args(argc, argv, &args, &as_json)) {
    return kExitUsage;
  }
  if (args.size() != 1) {
    std::cerr << "Usage: orc-cli nodes list <project> [--json]\n";
    return kExitUsage;
  }

  ProjectPresenter presenter;
  if (!load_project_or_report(args[0], &presenter)) {
    return kExitUsage;
  }
  const auto nodes = presenter.getNodes();

  if (as_json) {
    JsonWriter json(std::cout);
    json.begin_array();
    for (const auto& node : nodes) {
      json.begin_object();
      write_node_identity_json(&json, node, presenter);
      json.member_strings("tools", tool_ids_for(&presenter, node.stage_name));
      write_tool_refresh_json(&json, node, &presenter);
      // Only a CVBS source carries a signal state; other stages leave the
      // members out rather than reporting nulls that mean nothing for them.
      if (presenter.isCVBSSourceStage(node.stage_name)) {
        const auto state = presenter.getSourceSignalState(node.node_id);
        if (state) {
          json.member("signal_state", state->signal_state_preset);
        } else {
          json.member_null("signal_state");
        }
        if (state && state->sequence_continuous) {
          json.member_bool("sequence_continuous", *state->sequence_continuous);
        } else {
          json.member_null("sequence_continuous");
        }
      }
      json.end_object();
    }
    json.end_array();
    json.finish();
    return kExitSuccess;
  }

  std::cout << "Stages (" << nodes.size() << "):\n";
  for (const auto& node : nodes) {
    std::vector<Field> fields;
    fields.push_back({"Id", node.node_id.to_string()});
    fields.push_back({"Name", node.label});
    fields.push_back({"Stage", node.stage_name});
    fields.push_back(
        {"Status", orc::presenters::configurationStatusName(
                       presenter.getNodeConfigurationStatus(node.node_id))});
    const auto tools = tool_ids_for(&presenter, node.stage_name);
    if (!tools.empty()) {
      fields.push_back({"Tools", join(tools)});
    }
    const auto refresh = tool_refresh_text(node, &presenter);
    if (!refresh.empty()) {
      fields.push_back({"Refreshed by", refresh});
    }
    if (presenter.isCVBSSourceStage(node.stage_name)) {
      const auto state = presenter.getSourceSignalState(node.node_id);
      fields.push_back(
          {"Signal state", state ? state->signal_state_preset : "unknown"});
      std::string continuous = "unknown";
      if (state && state->sequence_continuous) {
        continuous = *state->sequence_continuous ? "true" : "false";
      }
      fields.push_back({"Sequence continuous", continuous});
    }
    print_detail_fields(fields, "  ");
    std::cout << "\n";
  }
  return kExitSuccess;
}

int cmd_nodes_get(int argc, char* argv[]) {
  std::vector<std::string> args;
  bool as_json = false;
  if (!parse_query_args(argc, argv, &args, &as_json)) {
    return kExitUsage;
  }
  if (args.size() != 2) {
    std::cerr << "Usage: orc-cli nodes get <project> <node> [--json]\n";
    return kExitUsage;
  }

  ProjectPresenter presenter;
  if (!load_project_or_report(args[0], &presenter)) {
    return kExitUsage;
  }
  const auto node = find_node_or_report(presenter, args[1]);
  if (!node) {
    return kExitNotFound;
  }

  const auto descriptors = presenter.getStageParameters(node->stage_name);
  const auto stored = presenter.getNodeParameters(node->node_id);

  // Every declared parameter, with its stored value or the default the stage
  // starts from, then any stored value the stage no longer declares.
  struct Row {
    std::string name;
    std::string type;
    std::optional<orc::ParameterValue> value;
    bool set = false;
    bool declared = true;
  };
  std::vector<Row> rows;
  for (const auto& descriptor : descriptors) {
    Row row;
    row.name = descriptor.name;
    row.type = orc::parameter_util::type_name(descriptor.type);
    const auto it = stored.find(descriptor.name);
    if (it != stored.end()) {
      row.value = it->second;
      row.set = true;
    } else {
      std::string ignored;
      row.value = orc::presenters::parseParameterText(
          descriptor, orc::presenters::stageParameterStoredDefault(descriptor),
          &ignored);
    }
    rows.push_back(std::move(row));
  }
  for (const auto& [name, value] : stored) {
    const bool declared =
        std::any_of(descriptors.begin(), descriptors.end(),
                    [&name](const auto& d) { return d.name == name; });
    if (!declared) {
      rows.push_back({name, "", value, true, false});
    }
  }

  if (as_json) {
    JsonWriter json(std::cout);
    json.begin_object();
    write_node_identity_json(&json, *node, presenter);
    write_tool_refresh_json(&json, *node, &presenter);
    json.key("parameters");
    json.begin_array();
    for (const auto& row : rows) {
      json.begin_object();
      json.member("name", row.name);
      if (row.declared) {
        json.member("type", row.type);
      } else {
        json.member_null("type");
      }
      if (row.value) {
        write_parameter_value_json(&json, "value", *row.value);
      } else {
        json.member_null("value");
      }
      json.member_bool("set", row.set);
      json.member_bool("declared", row.declared);
      json.end_object();
    }
    json.end_array();
    json.end_object();
    json.finish();
    return kExitSuccess;
  }

  std::vector<Field> identity{
      {"Id", node->node_id.to_string()},
      {"Name", node->label},
      {"Stage", node->stage_name},
      {"Status", orc::presenters::configurationStatusName(
                     presenter.getNodeConfigurationStatus(node->node_id))}};
  const auto refresh = tool_refresh_text(*node, &presenter);
  if (!refresh.empty()) {
    identity.push_back({"Refreshed by", refresh});
  }
  print_detail_fields(identity, "");
  std::cout << "\n";
  if (rows.empty()) {
    std::cout << "This stage has no parameters.\n";
    return kExitSuccess;
  }

  std::vector<Field> parameters;
  for (const auto& row : rows) {
    std::string text =
        row.value ? orc::parameter_util::value_to_string(*row.value) : "";
    if (!row.set) {
      text += "  (default)";
    } else if (!row.declared) {
      text += "  (not declared by the stage)";
    }
    parameters.push_back({row.name, text});
  }
  std::cout << "Parameters (" << rows.size() << "):\n";
  print_detail_fields(parameters, "  ");
  return kExitSuccess;
}

int cmd_nodes_set(int argc, char* argv[]) {
  std::vector<std::string> args;
  std::string output_path;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--output") {
      if (i + 1 >= argc) {
        std::cerr << "Error: --output requires a value\n";
        return kExitUsage;
      }
      output_path = argv[++i];
    } else if (arg.rfind("--", 0) == 0) {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return kExitUsage;
    } else {
      args.push_back(arg);
    }
  }
  if (args.size() < 3) {
    std::cerr << "Error: 'set' requires a project, a node and at least one "
                 "key=value\n";
    std::cerr << "Usage: orc-cli nodes set <project> <node> key=value [...] "
                 "[--output FILE]\n";
    return kExitUsage;
  }

  const std::string project_path = args[0];
  ProjectPresenter presenter;
  if (!load_project_or_report(project_path, &presenter)) {
    return kExitUsage;
  }
  const auto node = find_node_or_report(presenter, args[1]);
  if (!node) {
    return kExitNotFound;
  }

  // Keys are checked against what the stage offers in this project. The
  // format-free descriptor set only explains a refusal: `input_path` on a Y/C
  // project is a key the stage has, just not here.
  const auto descriptors = presenter.getStageParameters(node->stage_name);
  ProjectPresenter any_format;
  any_format.setVideoFormat(presenter.getVideoFormat());
  const auto any_format_descriptors =
      any_format.getStageParameters(node->stage_name);

  const std::vector<std::string> assignments(args.begin() + 2, args.end());
  const auto parsed = orc::presenters::parseParameterAssignments(
      descriptors, any_format_descriptors,
      source_format_name(presenter.getSourceType()), assignments);
  for (const auto& message : parsed.unknown_keys) {
    std::cerr << "Error: " << node->label << ": " << message << "\n";
  }
  for (const auto& message : parsed.errors) {
    std::cerr << "Error: " << node->label << ": " << message << "\n";
  }
  if (!parsed.unknown_keys.empty()) {
    std::cerr << "Run 'orc-cli nodes get " << project_path << " "
              << node->node_id.to_string()
              << "' to see the parameters this stage takes.\n";
    return kExitNotFound;
  }
  if (!parsed.errors.empty()) {
    return kExitUsage;
  }

  // The new values join the stored ones; setNodeParameters replaces the set.
  auto merged = presenter.getNodeParameters(node->node_id);
  for (const auto& [key, value] : parsed.values) {
    merged[key] = value;
  }
  try {
    presenter.setNodeParameters(node->node_id, merged);
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return kExitProjectRejected;
  }

  const std::string save_path =
      output_path.empty() ? project_path : output_path;
  if (!presenter.saveProject(save_path)) {
    std::cerr << "Error: Failed to save project: " << save_path << "\n";
    return kExitUsage;
  }
  std::cout << "Set " << parsed.values.size() << " parameter(s) on '"
            << node->label << "' (id " << node->node_id.to_string()
            << ") and saved " << save_path << "\n";
  return kExitSuccess;
}

int cmd_nodes_reset(int argc, char* argv[]) {
  std::vector<std::string> args;
  std::string output_path;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--output") {
      if (i + 1 >= argc) {
        std::cerr << "Error: --output requires a value\n";
        return kExitUsage;
      }
      output_path = argv[++i];
    } else if (arg.rfind("--", 0) == 0) {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return kExitUsage;
    } else {
      args.push_back(arg);
    }
  }
  if (args.size() != 2) {
    std::cerr << "Usage: orc-cli nodes reset <project> <node> [--output "
                 "FILE]\n";
    return kExitUsage;
  }

  const std::string project_path = args[0];
  ProjectPresenter presenter;
  if (!load_project_or_report(project_path, &presenter)) {
    return kExitUsage;
  }
  const auto node = find_node_or_report(presenter, args[1]);
  if (!node) {
    return kExitNotFound;
  }

  const auto descriptors = presenter.getStageParameters(node->stage_name);
  if (descriptors.empty()) {
    std::cout << "'" << node->label << "' (id " << node->node_id.to_string()
              << ") has no parameters to reset; " << project_path
              << " was not changed.\n";
    return kExitSuccess;
  }

  const auto plan = orc::presenters::planNodeReset(
      node->stage_name, descriptors, presenter.getNodeParameters(node->node_id),
      [&]() -> std::optional<orc::SourceParameters> {
        // A graph that cannot be built has no input to read, which the plan
        // reports as its problem.
        std::shared_ptr<void> dag;
        try {
          dag = presenter.getDAG();
        } catch (const std::exception& e) {
          std::cerr << "Warning: " << e.what() << "\n";
          return std::nullopt;
        }
        return orc::presenters::readInputVideoParameters(
            presenter.getCoreProjectHandle(), dag, presenter.getEdges(),
            node->node_id);
      });
  if (!plan.problem.empty()) {
    std::cerr << "Error: Cannot reset '" << node->label << "': " << plan.problem
              << "\n";
    std::cerr << project_path << " was not changed.\n";
    return kExitProjectRejected;
  }

  try {
    presenter.setNodeParameters(node->node_id, plan.values);
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << "\n";
    return kExitProjectRejected;
  }

  const std::string save_path =
      output_path.empty() ? project_path : output_path;
  if (!presenter.saveProject(save_path)) {
    std::cerr << "Error: Failed to save project: " << save_path << "\n";
    return kExitUsage;
  }

  std::cout << "Reset " << plan.reset_count << " parameter(s) on '"
            << node->label << "' (id " << node->node_id.to_string() << ") "
            << (plan.from_source_metadata ? "to the values its input reports"
                                          : "to their defaults")
            << " and saved " << save_path << "\n";
  std::vector<Field> values;
  for (const auto& descriptor : descriptors) {
    const auto it = plan.values.find(descriptor.name);
    if (it != plan.values.end()) {
      values.push_back(
          {descriptor.name, orc::parameter_util::value_to_string(it->second)});
    }
  }
  print_detail_fields(values, "  ");
  return kExitSuccess;
}

}  // namespace

int nodes_command(int argc, char* argv[]) {
  // argv[0] = program name, argv[1] = subcommand
  if (argc < 2) {
    print_nodes_usage(argv[0]);
    return kExitUsage;
  }

  const std::string subcommand = argv[1];

  if (subcommand == "--help" || subcommand == "-h") {
    print_nodes_usage(argv[0]);
    return kExitSuccess;
  }

  // Project loading logs (including any stage renaming) go to stderr, so
  // stdout carries only the command's answer.
  reserve_stdout_for_json();

  if (subcommand == "list") {
    return cmd_nodes_list(argc - 1, argv + 1);
  }
  if (subcommand == "get") {
    return cmd_nodes_get(argc - 1, argv + 1);
  }
  if (subcommand == "set") {
    return cmd_nodes_set(argc - 1, argv + 1);
  }
  if (subcommand == "reset") {
    return cmd_nodes_reset(argc - 1, argv + 1);
  }

  std::cerr << "Error: Unknown nodes subcommand: " << subcommand << "\n\n";
  print_nodes_usage(argv[0]);
  return kExitUsage;
}

}  // namespace cli
}  // namespace orc
