/*
 * File:        command_tools.cpp
 * Module:      orc-cli
 * Purpose:     Stage tool subcommand (list / run / refresh-config / refresh)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "command_tools.h"

#include <stage_ux_strings.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "analysis_presenter.h"
#include "analysis_report_json.h"
#include "analysis_sink_runner.h"
#include "analysis_tool_presenter.h"
#include "analysis_tool_presenter_factory.h"
#include "cli_exit_codes.h"
#include "command_process.h"
#include "detail_fields.h"
#include "json_writer.h"
#include "node_editing.h"
#include "project_args.h"
#include "project_presenter.h"
#include "tool_refresh.h"
#include "tool_running.h"

namespace orc {
namespace cli {

namespace {

using orc::presenters::AnalysisSinkKind;
using orc::presenters::AnalysisSinkRun;
using orc::presenters::NodeInfo;
using orc::presenters::ProjectPresenter;
using orc::presenters::ToolRunMode;

/// One label/value line of a text listing.
struct Field {
  std::string label;
  std::string value;
};

/// StatisticValue's wide integer alternative.
using StatisticInt = long long;  // NOLINT(google-runtime-int)

/// Items listed in a text report before the rest are left to --json.
constexpr size_t kTextItemLimit = 20;

// Set by the first Ctrl-C; the running tool polls it through its cancel
// check. A second Ctrl-C ends the process at once, so a stretch the tool
// cannot cancel (executing the upstream stages) never traps the user.
std::atomic<bool> g_cancel_requested{false};

extern "C" void on_interrupt(int /*signal*/) {
  if (g_cancel_requested.exchange(true)) {
    std::_Exit(kExitCancelled);
  }
  std::signal(SIGINT, on_interrupt);
}

void print_tools_usage(const char* program_name) {
  std::cerr << "Usage: " << program_name
            << " tools <subcommand> <project> [options]\n";
  std::cerr << "\n";
  std::cerr << "A <node> is a stage's id or its name, as for 'nodes'.\n";
  std::cerr << "\n";
  std::cerr << "Subcommands:\n";
  std::cerr << "  list <project> [<node>] [--json]  List the tools each "
               "stage offers, with their parameters\n";
  std::cerr << "  run <project> <tool> --node <node> [options]  Run a tool "
               "against a stage (dry run unless --apply)\n";
  std::cerr << "  refresh-config <project> <node> <tool> [--param key=value "
               "...]  Choose the tool 'refresh' runs for a stage\n";
  std::cerr << "  refresh-config <project> <node> --clear  Stop refreshing "
               "a stage\n";
  std::cerr << "  refresh <project> [options]    Run every stage's refresh "
               "tool in pipeline order, then save\n";
  std::cerr << "\n";
  std::cerr << "Options for 'run':\n";
  std::cerr << "  --node NODE                    The stage to run the tool "
               "against (required)\n";
  std::cerr << "  --param key=value              A tool parameter; repeat "
               "for each one\n";
  std::cerr << "  --apply                        Write the result to the "
               "stage and save the project\n";
  std::cerr << "  --output FILE                  With --apply, save to FILE "
               "instead of overwriting <project>\n";
  std::cerr << "  --csv FILE                     Analysis sinks only: also "
               "write the per-frame results as CSV\n";
  std::cerr << "  --quiet                        No progress lines, and only "
               "warnings and errors in the log\n";
  std::cerr << "\n";
  std::cerr << "Without --apply nothing is saved: the report shows what the "
               "tool found and the\n";
  std::cerr << "values it would write. A tool that should produce values and "
               "produces none counts\n";
  std::cerr << "as failed (exit 6), so a disc that cannot be mapped is never "
               "stacked silently.\n";
  std::cerr << "Interactive tools (the Dropout Editor, the catalogue "
               "browsers, the preset pickers)\n";
  std::cerr << "are refused with the GUI tool or command to use instead.\n";
  std::cerr << "\n";
  std::cerr << "Options for 'refresh-config':\n";
  std::cerr << "  --param key=value              A tool parameter the "
               "refresh runs with; others take defaults\n";
  std::cerr << "  --clear                        Remove the stage's refresh "
               "tool\n";
  std::cerr << "  --output FILE                  Save to FILE instead of "
               "overwriting <project>\n";
  std::cerr << "\n";
  std::cerr << "Options for 'refresh':\n";
  std::cerr << "  --only TOOL[,TOOL...]          Run only the stages "
               "refreshed by these tools\n";
  std::cerr << "  --dry-run                      Run every tool but save "
               "nothing\n";
  std::cerr << "  --output FILE                  Save to FILE instead of "
               "overwriting <project>\n";
  std::cerr << "  --process                      After saving, process the "
               "project as --process does\n";
  std::cerr << "  --quiet                        No progress lines, and only "
               "warnings and errors in the log\n";
  std::cerr << "\n";
  std::cerr << "'refresh' stops at the first tool that fails and then saves "
               "nothing, so the project\n";
  std::cerr << "is either fully refreshed or unchanged.\n";
  std::cerr << "\n";
  std::cerr << "Ctrl-C asks the tool to stop (exit 130); press it again to "
               "stop at once.\n";
  std::cerr << "\n";
  std::cerr << "Scripting:\n";
  std::cerr << "  --json                         Machine-readable output for "
               "'list', 'run' and 'refresh'\n";
}

std::string statistic_text(const orc::StatisticValue& value) {
  if (const auto* v = std::get_if<bool>(&value)) {
    return *v ? "true" : "false";
  }
  if (const auto* v = std::get_if<int>(&value)) {
    return std::to_string(*v);
  }
  if (const auto* v = std::get_if<StatisticInt>(&value)) {
    return std::to_string(*v);
  }
  if (const auto* v = std::get_if<double>(&value)) {
    const std::string token = number_token(*v);
    return token.empty() ? "n/a" : token;
  }
  return std::get<std::string>(value);
}

/// Prints throttled progress lines to stderr. Callbacks can arrive from the
/// tool's worker threads, hence the lock.
class ProgressPrinter {
 public:
  ProgressPrinter(std::string prefix, bool enabled)
      : prefix_(std::move(prefix)), enabled_(enabled) {}

  void operator()(int percent, const std::string& status) {
    if (!enabled_) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now = std::chrono::steady_clock::now();
    const int bucket = std::clamp(percent, 0, 100) / 5;
    const bool new_bucket = bucket != last_bucket_;
    const bool new_status =
        status != last_status_ && now - last_print_ >= std::chrono::seconds(1);
    if (!new_bucket && !new_status) {
      return;
    }
    last_bucket_ = bucket;
    last_status_ = status;
    last_print_ = now;
    std::cerr << prefix_ << ": " << std::setw(3) << std::clamp(percent, 0, 100)
              << "% " << status << "\n";
  }

 private:
  std::string prefix_;
  bool enabled_;
  std::mutex mutex_;
  int last_bucket_ = -1;
  std::string last_status_;
  std::chrono::steady_clock::time_point last_print_{};
};

/// The `.tbc` / `.cvbs` sources a node reads from, for the run report: a
/// script needs a CVBS capture's continuity to judge the Disc Mapper's result.
struct SourceReport {
  NodeInfo node;
  std::optional<ProjectPresenter::CVBSSignalState> signal_state;
  bool is_cvbs = false;
};

std::vector<SourceReport> sources_feeding(const ProjectPresenter& presenter,
                                          orc::NodeID node_id) {
  std::vector<SourceReport> sources;
  const auto edges = presenter.getEdges();
  for (const auto& id : orc::presenters::upstreamNodes(edges, node_id)) {
    const auto node = presenter.getNodeInfo(id);
    SourceReport report;
    report.node = node;
    if (ProjectPresenter::isCVBSSourceStage(node.stage_name)) {
      report.is_cvbs = true;
      report.signal_state = presenter.getSourceSignalState(id);
    } else if (node.stage_name != "tbc_source") {
      continue;
    }
    sources.push_back(std::move(report));
  }
  return sources;
}

void write_sources_json(JsonWriter* json,
                        const std::vector<SourceReport>& sources) {
  json->key("sources");
  json->begin_array();
  for (const auto& source : sources) {
    json->begin_object();
    json->member_int("id", source.node.node_id.value());
    json->member("label", source.node.label);
    json->member("stage", source.node.stage_name);
    if (source.is_cvbs) {
      if (source.signal_state) {
        json->member("signal_state", source.signal_state->signal_state_preset);
      } else {
        json->member_null("signal_state");
      }
      if (source.signal_state && source.signal_state->sequence_continuous) {
        json->member_bool("sequence_continuous",
                          *source.signal_state->sequence_continuous);
      } else {
        json->member_null("sequence_continuous");
      }
    }
    json->end_object();
  }
  json->end_array();
}

void print_sources_text(const std::vector<SourceReport>& sources) {
  for (const auto& source : sources) {
    if (!source.is_cvbs) {
      continue;
    }
    std::string continuous = "unknown";
    if (source.signal_state && source.signal_state->sequence_continuous) {
      continuous = *source.signal_state->sequence_continuous ? "true" : "false";
    }
    std::cout << "Source '" << source.node.label << "': sequence continuous "
              << continuous << "\n";
  }
}

std::string parameter_type_text(const orc::ParameterDescriptor& descriptor) {
  std::string text = orc::parameter_util::type_name(descriptor.type);
  if (!descriptor.constraints.allowed_strings.empty()) {
    text += ", one of:";
    for (const auto& allowed : descriptor.constraints.allowed_strings) {
      // Labelled entries pack "value<sep>label"; a script sets the value.
      text +=
          " " + allowed.substr(
                    0, allowed.find(orc::stage_ux::kComboValueLabelSeparator));
    }
  }
  return text;
}

// =============================================================================
// tools list
// =============================================================================

int cmd_tools_list(int argc, char* argv[]) {
  std::vector<std::string> args;
  bool as_json = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--json") {
      as_json = true;
    } else if (arg.rfind("--", 0) == 0) {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return kExitUsage;
    } else {
      args.push_back(arg);
    }
  }
  if (args.empty() || args.size() > 2) {
    std::cerr << "Usage: orc-cli tools list <project> [<node>] [--json]\n";
    return kExitUsage;
  }

  ProjectPresenter presenter;
  if (!load_project_or_report(args[0], &presenter)) {
    return kExitUsage;
  }
  std::vector<NodeInfo> nodes;
  if (args.size() == 2) {
    const auto node = find_node_or_report(presenter, args[1]);
    if (!node) {
      return kExitNotFound;
    }
    nodes.push_back(*node);
  } else {
    nodes = presenter.getNodes();
  }

  orc::presenters::AnalysisPresenter analysis(presenter.getCoreProjectHandle());

  if (as_json) {
    JsonWriter json(std::cout);
    json.begin_array();
    for (const auto& node : nodes) {
      json.begin_object();
      json.member_int("id", node.node_id.value());
      json.member("label", node.label);
      json.member("stage", node.stage_name);
      json.key("tools");
      json.begin_array();
      for (const auto& tool : analysis.getToolsForStage(node.stage_name)) {
        const auto support = orc::presenters::toolRunSupport(tool);
        json.begin_object();
        json.member("id", tool.id);
        json.member("name", tool.name);
        json.member("kind", orc::presenters::toolKindName(tool));
        json.member_bool("runnable", support.mode != ToolRunMode::GuiOnly);
        if (support.mode == ToolRunMode::GuiOnly) {
          json.member("not_runnable_reason", support.reason);
        }
        json.member_bool("can_apply",
                         support.mode == ToolRunMode::Analysis &&
                             orc::presenters::toolCanApplyToGraph(tool.id));
        json.key("parameters");
        json.begin_array();
        if (support.mode == ToolRunMode::Analysis) {
          for (const auto& descriptor : analysis.getToolParameters(
                   tool.id, orc::AnalysisSourceType::LaserDisc)) {
            json.begin_object();
            json.member("name", descriptor.name);
            json.member("type",
                        orc::parameter_util::type_name(descriptor.type));
            if (descriptor.constraints.default_value) {
              write_parameter_value_json(&json, "default",
                                         *descriptor.constraints.default_value);
            } else {
              json.member_null("default");
            }
            std::vector<std::string> allowed;
            for (const auto& entry : descriptor.constraints.allowed_strings) {
              allowed.push_back(entry.substr(
                  0, entry.find(orc::stage_ux::kComboValueLabelSeparator)));
            }
            json.member_strings("allowed", allowed);
            json.member("description", descriptor.description);
            json.end_object();
          }
        }
        json.end_array();
        json.end_object();
      }
      json.end_array();
      json.end_object();
    }
    json.end_array();
    json.finish();
    return kExitSuccess;
  }

  bool any = false;
  for (const auto& node : nodes) {
    const auto tools = analysis.getToolsForStage(node.stage_name);
    if (tools.empty()) {
      if (args.size() == 2) {
        std::cout << "'" << node.label << "' (" << node.stage_name
                  << ") offers no tools.\n";
      }
      continue;
    }
    any = true;
    std::cout << node.label << " (id " << node.node_id.to_string() << ", "
              << node.stage_name << "):\n";
    for (const auto& tool : tools) {
      const auto support = orc::presenters::toolRunSupport(tool);
      std::vector<Field> fields;
      fields.push_back({"Name", tool.name});
      fields.push_back({"Kind", orc::presenters::toolKindName(tool)});
      if (support.mode == ToolRunMode::GuiOnly) {
        fields.push_back({"Runs from CLI", "no: " + support.reason});
      } else if (support.mode == ToolRunMode::Analysis &&
                 orc::presenters::toolCanApplyToGraph(tool.id)) {
        fields.push_back({"Runs from CLI", "yes (--apply writes the result)"});
      } else {
        fields.push_back({"Runs from CLI", "yes (reports results)"});
      }
      std::cout << "  " << tool.id << "\n";
      print_detail_fields(fields, "    ");
      if (support.mode == ToolRunMode::Analysis) {
        const auto descriptors = analysis.getToolParameters(
            tool.id, orc::AnalysisSourceType::LaserDisc);
        for (const auto& descriptor : descriptors) {
          std::string text = parameter_type_text(descriptor);
          if (descriptor.constraints.default_value) {
            text += ", default " + orc::parameter_util::value_to_string(
                                       *descriptor.constraints.default_value);
          }
          std::cout << "    --param " << descriptor.name << "  (" << text
                    << ")\n";
        }
      }
    }
    std::cout << "\n";
  }
  if (!any && args.size() == 1) {
    std::cout << "No stage in this project offers a tool.\n";
  }
  return kExitSuccess;
}

// =============================================================================
// tools run
// =============================================================================

struct RunOptions {
  std::string project_path;
  std::string tool_id;
  std::string node_address;
  std::vector<std::string> params;
  bool apply = false;
  std::string output_path;
  std::string csv_path;
  bool as_json = false;
  bool quiet = false;
};

/// What `run` found, ready to print as text or JSON.
struct RunReport {
  NodeInfo node;
  std::string tool_id;
  std::string tool_name;
  /// "success", "failed" or "cancelled".
  std::string status = "failed";
  std::string summary;
  std::map<std::string, orc::StatisticValue> statistics;
  std::vector<orc::AnalysisResultItem> items;
  bool has_changes = false;
  std::map<std::string, orc::ParameterValue> changes;
  std::vector<SourceReport> sources;
  std::optional<AnalysisSinkRun> sink;
  bool applied = false;
  bool saved = false;
  std::string saved_path;
};

bool parse_run_args(int argc, char* argv[], RunOptions* options) {
  std::vector<std::string> positional;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    auto take_value = [&](std::string* out) {
      if (i + 1 >= argc) {
        std::cerr << "Error: " << arg << " requires a value\n";
        return false;
      }
      *out = argv[++i];
      return true;
    };
    if (arg == "--node") {
      if (!take_value(&options->node_address)) return false;
    } else if (arg == "--param") {
      std::string value;
      if (!take_value(&value)) return false;
      options->params.push_back(value);
    } else if (arg == "--output") {
      if (!take_value(&options->output_path)) return false;
    } else if (arg == "--csv") {
      if (!take_value(&options->csv_path)) return false;
    } else if (arg == "--apply") {
      options->apply = true;
    } else if (arg == "--json") {
      options->as_json = true;
    } else if (arg == "--quiet") {
      options->quiet = true;
    } else if (arg.rfind("--", 0) == 0) {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return false;
    } else {
      positional.push_back(arg);
    }
  }
  if (positional.size() != 2 || options->node_address.empty()) {
    std::cerr << "Usage: orc-cli tools run <project> <tool> --node <node> "
                 "[--param key=value ...] [--apply] [--output FILE] "
                 "[--csv FILE] [--json] [--quiet]\n";
    return false;
  }
  if (!options->output_path.empty() && !options->apply) {
    std::cerr << "Error: --output names where --apply saves; add --apply or "
                 "drop --output\n";
    return false;
  }
  options->project_path = positional[0];
  options->tool_id = positional[1];
  return true;
}

/// Summary figures of an analysis sink run, keyed as the JSON report names
/// them.
std::map<std::string, orc::StatisticValue> sink_statistics(
    const AnalysisSinkRun& run) {
  std::map<std::string, orc::StatisticValue> stats;
  stats["total_frames"] = static_cast<StatisticInt>(run.total_frames);
  auto add_series = [&stats](const std::string& name,
                             const std::vector<double>& values) {
    if (values.empty()) {
      return;
    }
    double sum = 0.0;
    for (const double v : values) {
      sum += v;
    }
    stats["mean_" + name] = sum / static_cast<double>(values.size());
    stats["min_" + name] = *std::min_element(values.begin(), values.end());
    stats["max_" + name] = *std::max_element(values.begin(), values.end());
  };
  switch (run.kind) {
    case AnalysisSinkKind::Dropout: {
      StatisticInt dropouts = 0;
      StatisticInt samples = 0;
      StatisticInt frames_with = 0;
      for (const auto& f : run.dropout) {
        dropouts += f.dropout_count;
        samples += f.dropout_length_samples;
        frames_with += f.has_data ? 1 : 0;
      }
      stats["frames_analysed"] = static_cast<StatisticInt>(run.dropout.size());
      stats["frames_with_dropouts"] = frames_with;
      stats["total_dropouts"] = dropouts;
      stats["total_dropout_samples"] = samples;
      break;
    }
    case AnalysisSinkKind::SNR: {
      std::vector<double> white;
      std::vector<double> black;
      for (const auto& f : run.snr) {
        if (f.has_white_snr) white.push_back(f.white_snr);
        if (f.has_black_psnr) black.push_back(f.black_psnr);
      }
      stats["frames_analysed"] = static_cast<StatisticInt>(run.snr.size());
      add_series("white_snr", white);
      add_series("black_psnr", black);
      break;
    }
    case AnalysisSinkKind::BurstLevel: {
      std::vector<double> burst;
      for (const auto& f : run.burst_level) {
        if (f.has_data) burst.push_back(f.median_burst_10bit);
      }
      stats["frames_analysed"] =
          static_cast<StatisticInt>(run.burst_level.size());
      add_series("median_burst_10bit", burst);
      break;
    }
    case AnalysisSinkKind::None:
      break;
  }
  return stats;
}

std::string sink_kind_name(AnalysisSinkKind kind) {
  switch (kind) {
    case AnalysisSinkKind::Dropout:
      return "dropout";
    case AnalysisSinkKind::SNR:
      return "snr";
    case AnalysisSinkKind::BurstLevel:
      return "burst_level";
    case AnalysisSinkKind::None:
      break;
  }
  return "none";
}

void write_sink_frames_json(JsonWriter* json, const AnalysisSinkRun& run) {
  json->key("results");
  json->begin_object();
  json->member("kind", sink_kind_name(run.kind));
  json->member_int("total_frames", run.total_frames);
  json->key("frames");
  json->begin_array();
  switch (run.kind) {
    case AnalysisSinkKind::Dropout:
      for (const auto& f : run.dropout) {
        json->begin_object();
        json->member_int("frame", f.frame_number);
        json->member_int("dropout_count", f.dropout_count);
        json->member_int("dropout_length_samples", f.dropout_length_samples);
        json->end_object();
      }
      break;
    case AnalysisSinkKind::SNR:
      for (const auto& f : run.snr) {
        json->begin_object();
        json->member_int("frame", f.frame_number);
        if (f.has_white_snr) {
          write_double_json(json, "white_snr", f.white_snr);
        } else {
          json->member_null("white_snr");
        }
        if (f.has_black_psnr) {
          write_double_json(json, "black_psnr", f.black_psnr);
        } else {
          json->member_null("black_psnr");
        }
        json->end_object();
      }
      break;
    case AnalysisSinkKind::BurstLevel:
      for (const auto& f : run.burst_level) {
        json->begin_object();
        json->member_int("frame", f.frame_number);
        if (f.has_data) {
          write_double_json(json, "median_burst_10bit", f.median_burst_10bit);
        } else {
          json->member_null("median_burst_10bit");
        }
        json->end_object();
      }
      break;
    case AnalysisSinkKind::None:
      break;
  }
  json->end_array();
  json->end_object();
}

/// One element of a report's `nodes` array; `run` and `refresh` share it.
void write_node_report_json(JsonWriter* json, const RunReport& report) {
  json->begin_object();
  json->member_int("id", report.node.node_id.value());
  json->member("node", report.node.label);
  json->member("stage", report.node.stage_name);
  json->member("tool", report.tool_id);
  json->member("status", report.status);
  json->member("summary", report.summary);
  json->key("statistics");
  json->begin_object();
  for (const auto& [name, value] : report.statistics) {
    write_statistic_json(json, name, value);
  }
  json->end_object();
  json->key("items");
  json->begin_array();
  for (const auto& item : report.items) {
    write_analysis_item_json(json, item);
  }
  json->end_array();
  if (report.has_changes) {
    json->key("parameter_changes");
    json->begin_object();
    for (const auto& [name, value] : report.changes) {
      write_parameter_value_json(json, name, value);
    }
    json->end_object();
  } else {
    json->member_null("parameter_changes");
  }
  if (report.sink) {
    write_sink_frames_json(json, *report.sink);
  }
  write_sources_json(json, report.sources);
  json->end_object();
}

void write_report_json(const RunOptions& options, const RunReport& report) {
  JsonWriter json(std::cout);
  json.begin_object();
  json.member("project", options.project_path);
  json.key("nodes");
  json.begin_array();
  write_node_report_json(&json, report);
  json.end_array();
  json.member_bool("applied", report.applied);
  json.member_bool("saved", report.saved);
  if (report.saved) {
    json.member("saved_to", report.saved_path);
  } else {
    json.member_null("saved_to");
  }
  json.end_object();
  json.finish();
}

/// What a tool found on one node, and the values it wrote or would write;
/// `run` and `refresh` share it.
void print_node_report_text(const RunReport& report) {
  std::cout << report.tool_name << " on '" << report.node.label << "' (id "
            << report.node.node_id.to_string() << "): " << report.status
            << "\n";
  if (!report.summary.empty()) {
    std::cout << "\n" << report.summary << "\n";
  }
  if (!report.statistics.empty()) {
    std::vector<Field> fields;
    for (const auto& [name, value] : report.statistics) {
      fields.push_back({name, statistic_text(value)});
    }
    std::cout << "\nStatistics:\n";
    print_detail_fields(fields, "  ");
  }
  if (!report.items.empty()) {
    std::cout << "\nItems (" << report.items.size() << "):\n";
    for (size_t i = 0; i < report.items.size() && i < kTextItemLimit; ++i) {
      const auto& item = report.items[i];
      std::cout << "  [" << item.type << "]";
      if (item.startFrame >= 0) {
        std::cout << " frames " << item.startFrame << "-" << item.endFrame;
      }
      std::cout << " " << item.message << "\n";
    }
    if (report.items.size() > kTextItemLimit) {
      std::cout << "  ... and " << report.items.size() - kTextItemLimit
                << " more (use --json to see them all)\n";
    }
  }
  if (!report.sources.empty()) {
    std::cout << "\n";
    print_sources_text(report.sources);
  }
  if (report.has_changes) {
    std::vector<Field> fields;
    for (const auto& [name, value] : report.changes) {
      fields.push_back({name, orc::parameter_util::value_to_string(value)});
    }
    std::cout << "\n" << (report.applied ? "Applied" : "Would set") << ":\n";
    print_detail_fields(fields, "  ");
  }
}

void print_report_text(const RunOptions& options, const RunReport& report) {
  print_node_report_text(report);
  if (report.saved) {
    std::cout << "\nSaved " << report.saved_path << "\n";
  } else if (report.has_changes && report.status == "success") {
    std::cout << "\nDry run: " << options.project_path
              << " was not changed. Add --apply to write these values.\n";
  }
  if (!options.csv_path.empty() && report.status == "success") {
    std::cout << "\nWrote per-frame results to " << options.csv_path << "\n";
  }
}

int finish(const RunOptions& options, const RunReport& report, int code) {
  if (options.as_json) {
    write_report_json(options, report);
  } else {
    print_report_text(options, report);
  }
  return code;
}

int cmd_tools_run(int argc, char* argv[]) {
  RunOptions options;
  if (!parse_run_args(argc, argv, &options)) {
    return kExitUsage;
  }
  if (options.quiet) {
    quiet_runtime_logging();
  }

  ProjectPresenter presenter;
  if (!load_project_or_report(options.project_path, &presenter)) {
    return kExitUsage;
  }
  const auto node = find_node_or_report(presenter, options.node_address);
  if (!node) {
    return kExitNotFound;
  }

  orc::presenters::AnalysisPresenter analysis(presenter.getCoreProjectHandle());
  const auto tools = analysis.getToolsForStage(node->stage_name);
  const auto tool_it = std::find_if(
      tools.begin(), tools.end(),
      [&options](const auto& t) { return t.id == options.tool_id; });
  if (tool_it == tools.end()) {
    std::cerr << "Error: '" << options.tool_id << "' is not a tool of '"
              << node->label << "' (" << node->stage_name << ").";
    if (tools.empty()) {
      std::cerr << " This stage offers no tools.\n";
    } else {
      std::cerr << " Its tools:";
      for (const auto& t : tools) {
        std::cerr << " " << t.id;
      }
      std::cerr << "\n";
    }
    return kExitNotFound;
  }
  const auto& tool = *tool_it;

  const auto support = orc::presenters::toolRunSupport(tool);
  if (support.mode == ToolRunMode::GuiOnly) {
    std::cerr << "Error: " << support.reason << ".\n";
    return kExitUsage;
  }
  const bool can_apply = support.mode == ToolRunMode::Analysis &&
                         orc::presenters::toolCanApplyToGraph(tool.id);
  if (options.apply && !can_apply) {
    std::cerr << "Error: '" << tool.id
              << "' reports results but has nothing to apply; drop --apply\n";
    return kExitUsage;
  }
  if (!options.csv_path.empty() && support.mode != ToolRunMode::BatchAnalysis) {
    std::cerr << "Error: --csv is for the analysis sinks' tools; use --json "
                 "for this tool's report\n";
    return kExitUsage;
  }

  std::vector<orc::ParameterDescriptor> descriptors;
  if (support.mode == ToolRunMode::Analysis) {
    descriptors =
        analysis.getToolParameters(tool.id, orc::AnalysisSourceType::LaserDisc);
  }
  const auto parsed = orc::presenters::parseParameterAssignments(
      descriptors, descriptors, "", options.params);
  for (const auto& message : parsed.unknown_keys) {
    std::cerr << "Error: " << tool.id << ": " << message << "\n";
  }
  for (const auto& message : parsed.errors) {
    std::cerr << "Error: " << tool.id << ": " << message << "\n";
  }
  if (!parsed.unknown_keys.empty()) {
    std::cerr << "Run 'orc-cli tools list " << options.project_path << " "
              << node->node_id.to_string()
              << "' to see the parameters this tool takes.\n";
    return kExitNotFound;
  }
  if (!parsed.errors.empty()) {
    return kExitUsage;
  }
  const auto parameters =
      orc::presenters::withParameterDefaults(descriptors, parsed.values);

  const auto refusal = orc::presenters::checkToolUpstream(
      presenter.getNodes(), presenter.getEdges(), node->node_id,
      [&presenter](orc::NodeID id) {
        return presenter.getNodeConfigurationStatus(id) !=
               orc::ConfigurationStatus::Red;
      });
  if (refusal) {
    std::cerr << "Error: " << *refusal << "\n";
    return kExitProjectRejected;
  }

  RunReport report;
  report.node = *node;
  report.tool_id = tool.id;
  report.tool_name = tool.name;
  report.sources = sources_feeding(presenter, node->node_id);

  g_cancel_requested.store(false);
  std::signal(SIGINT, on_interrupt);
  const auto cancel_check = [] { return g_cancel_requested.load(); };
  ProgressPrinter progress(tool.id, !options.quiet);
  const auto progress_callback = [&progress](int percent,
                                             const std::string& status) {
    progress(percent, status);
  };

  if (support.mode == ToolRunMode::BatchAnalysis) {
    std::map<std::string, orc::ParameterValue> overrides;
    if (!options.csv_path.empty()) {
      overrides["write_csv"] = true;
      overrides["output_path"] = options.csv_path;
    }
    auto run = orc::presenters::runAnalysisSink(
        presenter.getCoreProjectHandle(), node->node_id, overrides,
        progress_callback, cancel_check);
    std::signal(SIGINT, SIG_DFL);
    report.summary = run.status;
    if (run.cancelled || g_cancel_requested.load()) {
      report.status = "cancelled";
      return finish(options, report, kExitCancelled);
    }
    if (!run.success || run.kind == AnalysisSinkKind::None) {
      if (run.success) {
        report.summary = "The analysis produced no results";
      }
      return finish(options, report, kExitAnalysisFailed);
    }
    report.status = "success";
    report.statistics = sink_statistics(run);
    report.sink = std::move(run);
    return finish(options, report, kExitSuccess);
  }

  auto tool_presenter = orc::presenters::makeAnalysisToolPresenter(
      tool.id, presenter.getCoreProjectHandle());
  const auto result = tool_presenter->runAnalysis(
      node->node_id, parameters, progress_callback, cancel_check);
  std::signal(SIGINT, SIG_DFL);
  report.summary = result.summary;
  report.statistics = result.statistics;
  report.items = result.items;

  if (result.status == orc::AnalysisResult::Status::Cancelled ||
      g_cancel_requested.load()) {
    report.status = "cancelled";
    return finish(options, report, kExitCancelled);
  }
  if (result.status != orc::AnalysisResult::Status::Success) {
    return finish(options, report, kExitAnalysisFailed);
  }

  if (can_apply) {
    // A tool that writes parameters and produced none must not let the
    // script carry on as if the node were ready (a disc with no VBI, say).
    report.has_changes = tool_presenter->prepareResultChanges(
        result, node->node_id, &report.changes);
    if (!report.has_changes || report.changes.empty()) {
      report.has_changes = false;
      if (report.summary.empty()) {
        report.summary = "The analysis produced no parameter changes";
      } else {
        report.summary += "\nThe analysis produced no parameter changes.";
      }
      return finish(options, report, kExitAnalysisFailed);
    }
  }
  report.status = "success";

  if (options.apply) {
    if (!tool_presenter->applyResultToGraph(result, node->node_id)) {
      std::cerr << "Error: The project refused the tool's changes to '"
                << node->label << "'\n";
      report.status = "failed";
      return finish(options, report, kExitProjectRejected);
    }
    report.applied = true;
    const std::string save_path = options.output_path.empty()
                                      ? options.project_path
                                      : options.output_path;
    if (!presenter.saveProject(save_path)) {
      std::cerr << "Error: Failed to save project: " << save_path << "\n";
      return finish(options, report, kExitUsage);
    }
    report.saved = true;
    report.saved_path = save_path;
  }
  return finish(options, report, kExitSuccess);
}

// =============================================================================
// tools refresh-config
// =============================================================================

int cmd_tools_refresh_config(int argc, char* argv[]) {
  std::vector<std::string> positional;
  std::vector<std::string> params;
  std::string output_path;
  bool clear = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--param" || arg == "--output") {
      if (i + 1 >= argc) {
        std::cerr << "Error: " << arg << " requires a value\n";
        return kExitUsage;
      }
      if (arg == "--param") {
        params.emplace_back(argv[++i]);
      } else {
        output_path = argv[++i];
      }
    } else if (arg == "--clear") {
      clear = true;
    } else if (arg.rfind("--", 0) == 0) {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return kExitUsage;
    } else {
      positional.push_back(arg);
    }
  }
  const size_t expected = clear ? 2 : 3;
  if (positional.size() != expected || (clear && !params.empty())) {
    std::cerr << "Usage: orc-cli tools refresh-config <project> <node> <tool> "
                 "[--param key=value ...] [--output FILE]\n"
                 "       orc-cli tools refresh-config <project> <node> "
                 "--clear [--output FILE]\n";
    return kExitUsage;
  }
  const std::string& project_path = positional[0];

  ProjectPresenter presenter;
  if (!load_project_or_report(project_path, &presenter)) {
    return kExitUsage;
  }
  const auto node = find_node_or_report(presenter, positional[1]);
  if (!node) {
    return kExitNotFound;
  }
  void* handle = presenter.getCoreProjectHandle();

  std::optional<orc::presenters::ToolRefreshConfig> config;
  if (!clear) {
    const std::string& tool_id = positional[2];
    orc::presenters::AnalysisPresenter analysis(handle);
    const auto tools = analysis.getToolsForStage(node->stage_name);
    const bool offered =
        std::any_of(tools.begin(), tools.end(),
                    [&tool_id](const auto& t) { return t.id == tool_id; });
    if (const auto problem =
            orc::presenters::refreshToolProblem(tools, tool_id)) {
      std::cerr << "Error: " << *problem << " ('" << node->label << "', "
                << node->stage_name << ").";
      std::string refreshable;
      for (const auto& t : tools) {
        if (orc::presenters::toolCanRefresh(t)) {
          refreshable += " " + t.id;
        }
      }
      if (refreshable.empty()) {
        std::cerr << " This stage has no tool a refresh can run.\n";
      } else {
        std::cerr << " Tools a refresh can run here:" << refreshable << "\n";
      }
      return offered ? kExitUsage : kExitNotFound;
    }

    const auto descriptors =
        analysis.getToolParameters(tool_id, orc::AnalysisSourceType::LaserDisc);
    const auto parsed = orc::presenters::parseParameterAssignments(
        descriptors, descriptors, "", params);
    for (const auto& message : parsed.unknown_keys) {
      std::cerr << "Error: " << tool_id << ": " << message << "\n";
    }
    for (const auto& message : parsed.errors) {
      std::cerr << "Error: " << tool_id << ": " << message << "\n";
    }
    if (!parsed.unknown_keys.empty()) {
      std::cerr << "Run 'orc-cli tools list " << project_path << " "
                << node->node_id.to_string()
                << "' to see the parameters this tool takes.\n";
      return kExitNotFound;
    }
    if (!parsed.errors.empty()) {
      return kExitUsage;
    }
    config = orc::presenters::ToolRefreshConfig{tool_id, parsed.values};
  }

  try {
    orc::presenters::setNodeToolRefresh(handle, node->node_id, config);
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

  if (config) {
    std::cout << "'" << node->label << "' is refreshed by " << config->tool_id;
    if (!config->parameters.empty()) {
      std::cout << " with";
      for (const auto& [name, value] : config->parameters) {
        std::cout << " " << name << "="
                  << orc::parameter_util::value_to_string(value);
      }
    }
    std::cout << "\n";
  } else {
    std::cout << "'" << node->label << "' has no refresh tool\n";
  }
  std::cout << "Saved " << save_path << "\n";
  return kExitSuccess;
}

// =============================================================================
// tools refresh
// =============================================================================

struct RefreshOptions {
  std::string project_path;
  std::vector<std::string> only;
  bool dry_run = false;
  std::string output_path;
  bool as_json = false;
  bool process = false;
  bool quiet = false;
};

bool parse_refresh_args(int argc, char* argv[], RefreshOptions* options) {
  std::vector<std::string> positional;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--only" || arg == "--output") {
      if (i + 1 >= argc) {
        std::cerr << "Error: " << arg << " requires a value\n";
        return false;
      }
      const std::string value = argv[++i];
      if (arg == "--output") {
        options->output_path = value;
        continue;
      }
      std::stringstream list(value);
      std::string tool_id;
      while (std::getline(list, tool_id, ',')) {
        if (!tool_id.empty()) {
          options->only.push_back(tool_id);
        }
      }
    } else if (arg == "--dry-run") {
      options->dry_run = true;
    } else if (arg == "--json") {
      options->as_json = true;
    } else if (arg == "--process") {
      options->process = true;
    } else if (arg == "--quiet") {
      options->quiet = true;
    } else if (arg.rfind("--", 0) == 0) {
      std::cerr << "Error: Unknown option: " << arg << "\n";
      return false;
    } else {
      positional.push_back(arg);
    }
  }
  if (positional.size() != 1) {
    std::cerr << "Usage: orc-cli tools refresh <project> [--only TOOL[,...]] "
                 "[--dry-run] [--output FILE] [--process] [--json] "
                 "[--quiet]\n";
    return false;
  }
  if (options->dry_run && (!options->output_path.empty() || options->process)) {
    std::cerr << "Error: --dry-run saves nothing, so it cannot be combined "
                 "with --output or --process\n";
    return false;
  }
  options->project_path = positional[0];
  return true;
}

std::string refresh_status_name(orc::presenters::RefreshStepStatus status) {
  using orc::presenters::RefreshStepStatus;
  switch (status) {
    case RefreshStepStatus::Success:
      return "success";
    case RefreshStepStatus::Cancelled:
      return "cancelled";
    case RefreshStepStatus::Skipped:
      return "skipped";
    case RefreshStepStatus::Failed:
    case RefreshStepStatus::NoChanges:
    case RefreshStepStatus::Rejected:
      break;
  }
  return "failed";
}

int cmd_tools_refresh(int argc, char* argv[]) {
  using orc::presenters::RefreshStepStatus;

  RefreshOptions options;
  if (!parse_refresh_args(argc, argv, &options)) {
    return kExitUsage;
  }
  if (options.quiet) {
    quiet_runtime_logging();
  }

  ProjectPresenter presenter;
  if (!load_project_or_report(options.project_path, &presenter)) {
    return kExitUsage;
  }
  void* handle = presenter.getCoreProjectHandle();
  const auto nodes = presenter.getNodes();
  const auto edges = presenter.getEdges();
  const auto refresh_of = [handle](orc::NodeID id) {
    return orc::presenters::getNodeToolRefresh(handle, id);
  };

  const auto all_targets =
      orc::presenters::planToolRefresh(nodes, edges, refresh_of, {});
  if (all_targets.empty()) {
    std::cerr << "Error: No stage in " << options.project_path
              << " has a refresh tool. Choose one with 'orc-cli tools "
                 "refresh-config', or tick \"Use this tool when refreshing "
                 "from the CLI\" in the tool's dialog in the GUI.\n";
    return kExitUsage;
  }
  for (const auto& tool_id : options.only) {
    const bool used = std::any_of(
        all_targets.begin(), all_targets.end(),
        [&tool_id](const auto& t) { return t.config.tool_id == tool_id; });
    if (!used) {
      std::cerr << "Error: --only " << tool_id
                << ": no stage is refreshed by that tool. The project's "
                   "refresh tools:";
      std::set<std::string> ids;
      for (const auto& t : all_targets) {
        ids.insert(t.config.tool_id);
      }
      for (const auto& id : ids) {
        std::cerr << " " << id;
      }
      std::cerr << "\n";
      return kExitNotFound;
    }
  }
  const auto targets = options.only.empty()
                           ? all_targets
                           : orc::presenters::planToolRefresh(
                                 nodes, edges, refresh_of, options.only);

  // Every check comes before the first tool runs, so a broken template
  // fails in seconds rather than after an hour of mapping.
  orc::presenters::AnalysisPresenter analysis(handle);
  std::map<std::string, std::string> tool_names;
  for (const auto& target : targets) {
    const auto& tool_id = target.config.tool_id;
    const auto tools = analysis.getToolsForStage(target.node.stage_name);
    if (const auto problem =
            orc::presenters::refreshToolProblem(tools, tool_id)) {
      std::cerr << "Error: '" << target.node.label << "' ("
                << target.node.stage_name << "): " << *problem
                << ". Choose another with 'orc-cli tools refresh-config'.\n";
      return kExitNotFound;
    }
    for (const auto& t : tools) {
      if (t.id == tool_id) {
        tool_names[tool_id] = t.name;
      }
    }
    const auto descriptors =
        analysis.getToolParameters(tool_id, orc::AnalysisSourceType::LaserDisc);
    for (const auto& [name, value] : target.config.parameters) {
      const bool known =
          std::any_of(descriptors.begin(), descriptors.end(),
                      [&name](const auto& d) { return d.name == name; });
      if (!known) {
        std::cerr << "Error: '" << target.node.label << "': " << tool_id
                  << " has no parameter '" << name
                  << "'. Fix it with 'orc-cli tools refresh-config'.\n";
        return kExitNotFound;
      }
    }
    const auto refusal = orc::presenters::checkToolUpstream(
        nodes, edges, target.node.node_id, [&presenter](orc::NodeID id) {
          return presenter.getNodeConfigurationStatus(id) !=
                 orc::ConfigurationStatus::Red;
        });
    if (refusal) {
      std::cerr << "Error: '" << target.node.label << "': " << *refusal << "\n";
      return kExitProjectRejected;
    }
  }

  const std::string save_path =
      options.output_path.empty() ? options.project_path : options.output_path;
  orc::presenters::ProjectToolRefreshBackend backend(
      handle,
      [&presenter, &save_path] { return presenter.saveProject(save_path); });

  g_cancel_requested.store(false);
  std::signal(SIGINT, on_interrupt);
  const auto cancel_check = [] { return g_cancel_requested.load(); };
  std::unique_ptr<ProgressPrinter> progress;
  size_t progress_step = targets.size();
  const auto progress_callback = [&](size_t step, int percent,
                                     const std::string& status) {
    if (step != progress_step) {
      progress_step = step;
      const auto& target = targets[step];
      progress = std::make_unique<ProgressPrinter>(
          "[" + std::to_string(step + 1) + "/" +
              std::to_string(targets.size()) + "] " + target.config.tool_id +
              " on '" + target.node.label + "'",
          !options.quiet);
    }
    (*progress)(percent, status);
  };
  const auto outcome = orc::presenters::runToolRefresh(
      targets, &backend, options.dry_run, progress_callback, cancel_check);
  std::signal(SIGINT, SIG_DFL);

  // One report per node, in the shape `tools run` uses
  int code = kExitSuccess;
  std::vector<RunReport> reports;
  for (const auto& step : outcome.steps) {
    RunReport report;
    report.node = step.target.node;
    report.tool_id = step.target.config.tool_id;
    report.tool_name = tool_names[report.tool_id];
    report.status = refresh_status_name(step.status);
    report.summary = step.result.summary;
    report.statistics = step.result.statistics;
    report.items = step.result.items;
    report.changes = step.changes;
    report.has_changes = !step.changes.empty();
    report.applied = outcome.saved && step.status == RefreshStepStatus::Success;
    report.sources = sources_feeding(presenter, step.target.node.node_id);
    switch (step.status) {
      case RefreshStepStatus::Success:
        break;
      case RefreshStepStatus::Skipped:
        report.summary = "Not run: an earlier tool did not succeed";
        break;
      case RefreshStepStatus::NoChanges:
        report.summary += report.summary.empty() ? "" : "\n";
        report.summary += "The analysis produced no parameter changes.";
        code = kExitAnalysisFailed;
        break;
      case RefreshStepStatus::Failed:
        code = kExitAnalysisFailed;
        break;
      case RefreshStepStatus::Rejected:
        report.summary += report.summary.empty() ? "" : "\n";
        report.summary += "The project refused the tool's changes.";
        code = kExitProjectRejected;
        break;
      case RefreshStepStatus::Cancelled:
        code = kExitCancelled;
        break;
    }
    reports.push_back(std::move(report));
  }
  if (outcome.save_failed) {
    std::cerr << "Error: Failed to save project: " << save_path << "\n";
    code = kExitUsage;
  }

  // --process reloads what was saved and triggers every sink, as
  // `orc-cli <project> --process` does.
  std::optional<bool> processed;
  if (options.process && outcome.saved) {
    ProcessOptions process_options;
    process_options.project_path = save_path;
    processed = process_command(process_options) == 0;
    if (!*processed) {
      std::cerr << "Error: Processing " << save_path << " failed\n";
      code = kExitUsage;
    }
  }

  if (options.as_json) {
    JsonWriter json(std::cout);
    json.begin_object();
    json.member("project", options.project_path);
    json.member_bool("dry_run", options.dry_run);
    json.key("nodes");
    json.begin_array();
    for (const auto& report : reports) {
      write_node_report_json(&json, report);
    }
    json.end_array();
    json.member_bool("completed", outcome.completed);
    json.member_bool("saved", outcome.saved);
    if (outcome.saved) {
      json.member("saved_to", save_path);
    } else {
      json.member_null("saved_to");
    }
    if (processed) {
      json.member_bool("processed", *processed);
    } else {
      json.member_null("processed");
    }
    json.end_object();
    json.finish();
    return code;
  }

  for (size_t i = 0; i < reports.size(); ++i) {
    std::cout << (i == 0 ? "" : "\n") << "[" << (i + 1) << "/" << reports.size()
              << "] ";
    if (outcome.steps[i].status == RefreshStepStatus::Skipped) {
      std::cout << reports[i].tool_name << " on '" << reports[i].node.label
                << "': skipped\n";
      continue;
    }
    print_node_report_text(reports[i]);
  }
  std::cout << "\n";
  if (outcome.saved) {
    std::cout << "Saved " << save_path << "\n";
  } else if (options.dry_run && outcome.completed) {
    std::cout << "Dry run: every tool succeeded; " << options.project_path
              << " was not changed.\n";
  } else if (!outcome.completed) {
    std::cout << "Stopped: " << options.project_path << " was not changed.\n";
  }
  if (processed && *processed) {
    std::cout << "Processed " << save_path << "\n";
  }
  return code;
}

}  // namespace

int tools_command(int argc, char* argv[]) {
  // argv[0] = program name, argv[1] = subcommand
  if (argc < 2) {
    print_tools_usage(argv[0]);
    return kExitUsage;
  }

  const std::string subcommand = argv[1];

  if (subcommand == "--help" || subcommand == "-h") {
    print_tools_usage(argv[0]);
    return kExitSuccess;
  }

  // Analysis and project loading log to stderr, so stdout carries only the
  // command's answer.
  reserve_stdout_for_json();

  if (subcommand == "list") {
    return cmd_tools_list(argc - 1, argv + 1);
  }
  if (subcommand == "run") {
    return cmd_tools_run(argc - 1, argv + 1);
  }
  if (subcommand == "refresh-config") {
    return cmd_tools_refresh_config(argc - 1, argv + 1);
  }
  if (subcommand == "refresh") {
    return cmd_tools_refresh(argc - 1, argv + 1);
  }

  std::cerr << "Error: Unknown tools subcommand: " << subcommand << "\n\n";
  print_tools_usage(argv[0]);
  return kExitUsage;
}

}  // namespace cli
}  // namespace orc
