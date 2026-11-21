/*
 * File:        stage_analysis_tools.cpp
 * Module:      orc-presenters
 * Purpose:     Running a stage's analysis tools (StageToolKind::Analysis)
 *              for orc-cli and orc-gui, and their view-tier forms
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../include/stage_analysis_tools.h"

#include <orc/plugin/orc_stage_tooling.h>
#include <orc/stage/common_types.h>
#include <orc/stage/tooling/stage_analysis_tool.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>
#include <variant>

#include "../../core/include/dag_executor.h"
#include "../../core/include/project.h"
#include "../../core/include/stage_registry.h"
#include "../include/stage_tool_parameters.h"
#include "../include/tool_refresh.h"
#include "../include/tool_running.h"
#include "tool_job_context.h"

namespace orc::presenters {

namespace {

/// StatisticValue's wide integer alternative.
using StatisticInt = long long;  // NOLINT(google-runtime-int)

orc::StatisticValue statisticValue(const orc::StageAnalysisValue& value) {
  if (const auto* v = std::get_if<bool>(&value)) {
    return *v;
  }
  if (const auto* v = std::get_if<int64_t>(&value)) {
    return static_cast<StatisticInt>(*v);
  }
  if (const auto* v = std::get_if<double>(&value)) {
    return *v;
  }
  return std::get<std::string>(value);
}

int frameNumber(int64_t frame) {
  return frame < 0 ? -1 : static_cast<int>(frame);
}

orc::AnalysisResult hostResult(orc::StageAnalysisReport report) {
  orc::AnalysisResult result;
  switch (report.status) {
    case orc::StageAnalysisReport::Status::Success:
      result.status = orc::AnalysisResult::Success;
      break;
    case orc::StageAnalysisReport::Status::Cancelled:
      result.status = orc::AnalysisResult::Cancelled;
      break;
    case orc::StageAnalysisReport::Status::Failed:
      result.status = orc::AnalysisResult::Failed;
      break;
  }
  result.summary = std::move(report.summary);
  for (const auto& [name, value] : report.statistics) {
    result.statistics[name] = statisticValue(value);
  }
  for (auto& item : report.items) {
    orc::AnalysisResultItem host_item;
    host_item.type = std::move(item.type);
    host_item.message = std::move(item.message);
    host_item.startFrame = frameNumber(item.start_frame);
    host_item.endFrame = frameNumber(item.end_frame);
    for (const auto& [name, value] : item.data) {
      host_item.metadata[name] = statisticValue(value);
    }
    result.items.push_back(std::move(host_item));
  }
  result.parameterChanges = std::move(report.parameter_changes);
  return result;
}

orc::AnalysisResult failed(std::string why) {
  orc::AnalysisResult result;
  result.status = orc::AnalysisResult::Failed;
  result.summary = std::move(why);
  return result;
}

orc::AnalysisResult cancelledResult() {
  orc::AnalysisResult result;
  result.status = orc::AnalysisResult::Cancelled;
  result.summary = "Analysis was cancelled.";
  return result;
}

std::string valueText(const std::optional<orc::ParameterValue>& value) {
  if (!value) {
    return {};
  }
  return orc::parameter_util::value_to_string(*value);
}

std::string realText(const std::optional<orc::ParameterValue>& value) {
  if (!value) {
    return {};
  }
  if (std::holds_alternative<double>(*value)) {
    const auto text = toolParameterText({{"v", *value}});
    return text.at("v");
  }
  return valueText(value);
}

}  // namespace

std::vector<orc::ParameterDescriptor> stageAnalysisToolParameters(
    const std::string& stage_name, const std::string& tool_id) {
  try {
    auto stage = orc::StageRegistry::instance().create_stage(stage_name);
    const auto* tools = dynamic_cast<orc::IStageAnalysisTool*>(stage.get());
    if (!tools) {
      return {};
    }
    return tools->analysis_tool_parameters(tool_id);
  } catch (const std::exception& e) {
    ORC_LOG_WARN("Could not read the parameters of '{}' from stage '{}': {}",
                 tool_id, stage_name, e.what());
    return {};
  }
}

bool stageAnalysisToolRunsAlongside(const std::string& stage_name,
                                    const std::string& tool_id) {
  try {
    auto stage = orc::StageRegistry::instance().create_stage(stage_name);
    const auto* tools = dynamic_cast<orc::IStageAnalysisTool*>(stage.get());
    return tools && tools->analysis_tool_runs_alongside(tool_id);
  } catch (const std::exception& e) {
    ORC_LOG_WARN("Could not ask stage '{}' how '{}' runs: {}", stage_name,
                 tool_id, e.what());
    return false;
  }
}

std::optional<StageAnalysisToolRef> findStageAnalysisTool(
    void* project_handle, NodeID node, const std::string& contract_id) {
  if (!project_handle || contract_id.empty()) {
    return std::nullopt;
  }
  const auto& nodes = static_cast<orc::Project*>(project_handle)->get_nodes();
  const auto node_it =
      std::find_if(nodes.begin(), nodes.end(),
                   [node](const auto& n) { return n.node_id == node; });
  if (node_it == nodes.end()) {
    return std::nullopt;
  }
  try {
    auto stage =
        orc::StageRegistry::instance().create_stage(node_it->stage_name);
    const auto* provider = dynamic_cast<orc::StageToolProvider*>(stage.get());
    if (!provider || !dynamic_cast<orc::IStageAnalysisTool*>(stage.get())) {
      return std::nullopt;
    }
    for (const auto& tool : provider->get_stage_tools()) {
      if (tool.kind == orc::StageToolKind::Analysis &&
          tool.contract_id == contract_id) {
        return StageAnalysisToolRef{node_it->stage_name, tool.tool_id,
                                    tool.display_name, tool.description};
      }
    }
  } catch (const std::exception& e) {
    ORC_LOG_WARN("Could not read the tools of stage '{}': {}",
                 node_it->stage_name, e.what());
  }
  return std::nullopt;
}

StageAnalysisRun::StageAnalysisRun(
    const void* project_handle, NodeID node, std::string tool_id,
    std::map<std::string, orc::ParameterValue> parameters)
    : project_(project_handle
                   ? std::make_unique<orc::Project>(
                         *static_cast<const orc::Project*>(project_handle))
                   : nullptr),
      node_(node),
      tool_id_(std::move(tool_id)),
      parameters_(std::move(parameters)) {}

StageAnalysisRun::~StageAnalysisRun() = default;

orc::AnalysisResult StageAnalysisRun::run(const ProgressFn& on_progress,
                                          const CancelledFn& is_cancelled) {
  const auto cancelled = [&is_cancelled] {
    return is_cancelled && is_cancelled();
  };
  if (!project_) {
    return failed("No project is open");
  }
  int percent = 0;
  const auto report = [&on_progress, &percent](double fraction,
                                               const std::string& text) {
    if (fraction >= 0.0) {
      percent =
          static_cast<int>(std::lround(std::clamp(fraction, 0.0, 1.0) * 100.0));
    }
    if (on_progress) {
      on_progress(percent, text);
    }
  };
  report(0.0, "Preparing the node's inputs...");

  try {
    const auto cannot_run = [this](const std::string& why) {
      return failed("Analysis tool '" + tool_id_ + "' cannot run: " + why);
    };
    auto prepared = prepareToolJob(*project_, node_);
    if (!prepared.stage) {
      return cannot_run(prepared.error);
    }
    auto* tools = dynamic_cast<orc::IStageAnalysisTool*>(prepared.stage);
    const auto* provider =
        dynamic_cast<const orc::StageToolProvider*>(prepared.stage);
    const auto offered = [this, provider] {
      if (!provider) {
        return false;
      }
      const auto descriptors = provider->get_stage_tools();
      return std::any_of(descriptors.begin(), descriptors.end(),
                         [this](const orc::StageToolDescriptor& tool) {
                           return tool.kind == orc::StageToolKind::Analysis &&
                                  tool.tool_id == tool_id_;
                         });
    };
    if (!tools || !offered()) {
      return cannot_run("the node's stage does not offer it");
    }
    if (cancelled()) {
      return cancelledResult();
    }
    HostToolJobContext context(
        std::move(prepared.inputs), report, is_cancelled,
        [this] { return openPrivateToolInputs(*project_, node_); });
    auto result =
        hostResult(tools->run_analysis_tool(tool_id_, parameters_, context));
    if (cancelled()) {
      return cancelledResult();
    }
    return result;
  } catch (const std::exception& e) {
    ORC_LOG_WARN("Analysis tool '{}' on node {} failed: {}", tool_id_,
                 node_.to_string(), e.what());
    if (cancelled()) {
      return cancelledResult();
    }
    return failed(std::string("The analysis failed: ") + e.what());
  } catch (...) {
    if (cancelled()) {
      return cancelledResult();
    }
    return failed("The analysis failed");
  }
}

std::vector<orc::view::ToolAnalysisParameter> toolAnalysisParameters(
    const std::vector<orc::ParameterDescriptor>& descriptors) {
  using Type = orc::view::ToolAnalysisParameter::Type;
  std::vector<orc::view::ToolAnalysisParameter> out;
  for (const auto& descriptor : descriptors) {
    orc::view::ToolAnalysisParameter parameter;
    parameter.name = descriptor.name;
    parameter.display_name = descriptor.display_name.empty()
                                 ? descriptor.name
                                 : descriptor.display_name;
    parameter.description = descriptor.description;
    const auto& limits = descriptor.constraints;
    switch (descriptor.type) {
      case orc::ParameterType::BOOL:
        parameter.type = Type::Bool;
        break;
      case orc::ParameterType::INT32:
      case orc::ParameterType::UINT32:
        parameter.type = Type::Integer;
        parameter.min_value = valueText(limits.min_value);
        parameter.max_value = valueText(limits.max_value);
        if (descriptor.type == orc::ParameterType::UINT32 &&
            parameter.min_value.empty()) {
          parameter.min_value = "0";
        }
        break;
      case orc::ParameterType::DOUBLE:
        parameter.type = Type::Real;
        parameter.min_value = realText(limits.min_value);
        parameter.max_value = realText(limits.max_value);
        break;
      case orc::ParameterType::STRING:
      case orc::ParameterType::FILE_PATH:
        parameter.type = Type::Text;
        parameter.allowed_values = limits.allowed_strings;
        break;
    }
    parameter.default_value = descriptor.type == orc::ParameterType::DOUBLE
                                  ? realText(limits.default_value)
                                  : valueText(limits.default_value);
    if (limits.depends_on) {
      parameter.depends_on = limits.depends_on->parameter_name;
      parameter.depends_on_values = limits.depends_on->required_values;
    }
    out.push_back(std::move(parameter));
  }
  return out;
}

orc::view::ToolAnalysisReport toolAnalysisReport(
    const orc::AnalysisResult& result) {
  using Status = orc::view::ToolAnalysisReport::Status;
  orc::view::ToolAnalysisReport report;
  switch (result.status) {
    case orc::AnalysisResult::Success:
      report.status = Status::Success;
      break;
    case orc::AnalysisResult::Cancelled:
      report.status = Status::Cancelled;
      break;
    case orc::AnalysisResult::Failed:
      report.status = Status::Failed;
      break;
  }
  report.summary = result.summary;
  if (result.status == orc::AnalysisResult::Success) {
    report.parameter_changes = toolParameterText(result.parameterChanges);
  }
  return report;
}

bool toolAnalysisParameterValues(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const orc::view::ToolParameterMap& text,
    std::map<std::string, orc::ParameterValue>* values, std::string* error) {
  std::map<std::string, orc::ParameterValue> parsed;
  if (!mergeToolParameters(descriptors, {}, text, &parsed, error)) {
    return false;
  }
  if (values) {
    *values = withParameterDefaults(descriptors, std::move(parsed));
  }
  return true;
}

orc::view::ToolAnalysisInfo stageAnalysisToolInfo(
    void* project_handle, NodeID node, const std::string& contract_id) {
  const auto tool = findStageAnalysisTool(project_handle, node, contract_id);
  if (!tool) {
    return {};
  }
  orc::view::ToolAnalysisInfo info;
  info.description = tool->description;
  info.parameters = toolAnalysisParameters(
      stageAnalysisToolParameters(tool->stage_name, tool->tool_id));
  const auto system =
      static_cast<const orc::Project*>(project_handle)->get_video_format();
  if (system != orc::VideoSystem::Unknown) {
    info.video_system = orc::video_system_to_string(system);
  }
  return info;
}

std::shared_ptr<StageAnalysisRun> prepareStageAnalysisRun(
    void* project_handle, NodeID node, const std::string& contract_id,
    const orc::view::ToolParameterMap& parameters, std::string* why) {
  const auto tool = findStageAnalysisTool(project_handle, node, contract_id);
  if (!tool) {
    if (why) {
      *why = "The node's stage has no such analysis tool";
    }
    return nullptr;
  }
  std::map<std::string, orc::ParameterValue> values;
  if (!toolAnalysisParameterValues(
          stageAnalysisToolParameters(tool->stage_name, tool->tool_id),
          parameters, &values, why)) {
    return nullptr;
  }
  return std::make_shared<StageAnalysisRun>(project_handle, node, tool->tool_id,
                                            std::move(values));
}

orc::view::ToolRefreshState stageAnalysisRefreshState(
    void* project_handle, NodeID node, const std::string& contract_id) {
  orc::view::ToolRefreshState state;
  const auto tool = findStageAnalysisTool(project_handle, node, contract_id);
  if (!tool) {
    return state;
  }
  state.available = true;
  const auto stored = getNodeToolRefresh(project_handle, node);
  if (stored && stored->tool_id == tool->tool_id) {
    state.this_tool = true;
    state.parameters = toolParameterText(stored->parameters);
  } else if (stored) {
    state.other_tool = stored->tool_id;
  }
  return state;
}

orc::view::ToolReply setStageAnalysisRefresh(
    void* project_handle, NodeID node, const std::string& contract_id,
    bool use_this_tool, const orc::view::ToolParameterMap& parameters) {
  orc::view::ToolReply reply;
  const auto tool = findStageAnalysisTool(project_handle, node, contract_id);
  if (!tool) {
    reply.error = "The node's stage has no such analysis tool";
    return reply;
  }
  const auto stored = getNodeToolRefresh(project_handle, node);
  std::optional<ToolRefreshConfig> wanted = stored;
  if (use_this_tool) {
    std::map<std::string, orc::ParameterValue> values;
    if (!toolAnalysisParameterValues(
            stageAnalysisToolParameters(tool->stage_name, tool->tool_id),
            parameters, &values, &reply.error)) {
      return reply;
    }
    wanted = ToolRefreshConfig{tool->tool_id, std::move(values)};
  } else if (stored && stored->tool_id == tool->tool_id) {
    wanted.reset();
  }
  reply.ok = true;
  const bool changed = !(wanted == stored);
  if (changed) {
    setNodeToolRefresh(project_handle, node, wanted);
  }
  reply.data.push_back(changed ? 1 : 0);
  return reply;
}

}  // namespace orc::presenters
