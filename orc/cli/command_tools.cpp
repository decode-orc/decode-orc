/*
 * File:        command_tools.cpp
 * Module:      orc-cli
 * Purpose:     Stage tool subcommand (list / run)
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
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "analysis_presenter.h"
#include "analysis_sink_runner.h"
#include "analysis_tool_presenter.h"
#include "analysis_tool_presenter_factory.h"
#include "cli_exit_codes.h"
#include "detail_fields.h"
#include "json_writer.h"
#include "node_editing.h"
#include "project_args.h"
#include "project_presenter.h"
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
  std::cerr << "Ctrl-C asks the tool to stop (exit 130); press it again to "
               "stop at once.\n";
  std::cerr << "\n";
  std::cerr << "Scripting:\n";
  std::cerr << "  --json                         Machine-readable output for "
               "'list' and 'run'\n";
}

std::string number_token(double value) {
  if (!std::isfinite(value)) {
    return {};
  }
  std::ostringstream out;
  out << std::setprecision(10) << value;
  return out.str();
}

void write_double_json(JsonWriter* json, const std::string& name,
                       double value) {
  const std::string token = number_token(value);
  if (token.empty()) {
    json->member_null(name);
  } else {
    json->member_number(name, token);
  }
}

void write_statistic_json(JsonWriter* json, const std::string& name,
                          const orc::StatisticValue& value) {
  if (const auto* v = std::get_if<bool>(&value)) {
    json->member_bool(name, *v);
  } else if (const auto* v = std::get_if<int>(&value)) {
    json->member_int(name, *v);
  } else if (const auto* v = std::get_if<StatisticInt>(&value)) {
    json->member_int(name, *v);
  } else if (const auto* v = std::get_if<double>(&value)) {
    write_double_json(json, name, *v);
  } else {
    json->member(name, std::get<std::string>(value));
  }
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

void write_report_json(const RunOptions& options, const RunReport& report) {
  JsonWriter json(std::cout);
  json.begin_object();
  json.member("project", options.project_path);
  json.key("nodes");
  json.begin_array();
  json.begin_object();
  json.member_int("id", report.node.node_id.value());
  json.member("node", report.node.label);
  json.member("stage", report.node.stage_name);
  json.member("tool", report.tool_id);
  json.member("status", report.status);
  json.member("summary", report.summary);
  json.key("statistics");
  json.begin_object();
  for (const auto& [name, value] : report.statistics) {
    write_statistic_json(&json, name, value);
  }
  json.end_object();
  json.key("items");
  json.begin_array();
  for (const auto& item : report.items) {
    json.begin_object();
    json.member("type", item.type);
    json.member("message", item.message);
    if (item.startFrame >= 0) {
      json.member_int("start_frame", item.startFrame);
    } else {
      json.member_null("start_frame");
    }
    if (item.endFrame >= 0) {
      json.member_int("end_frame", item.endFrame);
    } else {
      json.member_null("end_frame");
    }
    json.end_object();
  }
  json.end_array();
  if (report.has_changes) {
    json.key("parameter_changes");
    json.begin_object();
    for (const auto& [name, value] : report.changes) {
      write_parameter_value_json(&json, name, value);
    }
    json.end_object();
  } else {
    json.member_null("parameter_changes");
  }
  if (report.sink) {
    write_sink_frames_json(&json, *report.sink);
  }
  write_sources_json(&json, report.sources);
  json.end_object();
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

void print_report_text(const RunOptions& options, const RunReport& report) {
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

  std::cerr << "Error: Unknown tools subcommand: " << subcommand << "\n\n";
  print_tools_usage(argv[0]);
  return kExitUsage;
}

}  // namespace cli
}  // namespace orc
