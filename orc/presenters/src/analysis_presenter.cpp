/*
 * File:        analysis_presenter.cpp
 * Module:      orc-presenters
 * Purpose:     Analysis presenter implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "analysis_presenter.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <unordered_set>

#include "../../sdk/include/orc/plugin/orc_stage_tooling.h"
#include "../core/analysis/analysis_registry.h"
#include "../core/analysis/analysis_tool.h"
#include "../core/include/dag_executor.h"
#include "../core/include/project.h"
#include "../core/include/stage_registry.h"

namespace orc::presenters {

namespace {

void applyLegacyToolMetadata(orc::AnalysisToolInfo& info) {
  if (info.id == "mask_line_config") {
    info.stage_tool_kind = "config_dialog";
    info.stage_tool_contract = "decode-orc.stage-tools.mask-line-config.v1";
    info.stage_tool_non_modal = false;
  } else if (info.id == "ffmpeg_preset_config") {
    info.stage_tool_kind = "config_dialog";
    info.stage_tool_contract = "decode-orc.stage-tools.ffmpeg-preset.v1";
    info.stage_tool_non_modal = false;
  } else if (info.id == "dropout_editor") {
    info.stage_tool_kind = "non_modal_editor";
    info.stage_tool_contract = "decode-orc.stage-tools.dropout-editor.v1";
    info.stage_tool_non_modal = true;
  } else if (info.id == "vectorscope") {
    info.stage_tool_kind = "preview_utility";
    info.stage_tool_contract = "decode-orc.stage-tools.vectorscope.v1";
    info.stage_tool_non_modal = true;
  } else if (info.id == "dropout_analysis" || info.id == "snr_analysis" ||
             info.id == "burst_level_analysis") {
    info.stage_tool_kind = "batch_analysis";
    if (info.id == "dropout_analysis") {
      info.stage_tool_contract = "decode-orc.stage-tools.dropout-analysis.v1";
    } else if (info.id == "snr_analysis") {
      info.stage_tool_contract = "decode-orc.stage-tools.snr-analysis.v1";
    } else {
      info.stage_tool_contract =
          "decode-orc.stage-tools.burst-level-analysis.v1";
    }
    info.stage_tool_non_modal = false;
  }
}

const char* stageToolKindToString(orc::StageToolKind kind) {
  switch (kind) {
    case orc::StageToolKind::ConfigDialog:
      return "config_dialog";
    case orc::StageToolKind::NonModalEditor:
      return "non_modal_editor";
    case orc::StageToolKind::BatchAnalysis:
      return "batch_analysis";
    case orc::StageToolKind::PreviewUtility:
      return "preview_utility";
    case orc::StageToolKind::PluginView:
      return "plugin_view";
    default:
      return "";
  }
}

}  // namespace

class AnalysisPresenter::Impl {
 public:
  explicit Impl(orc::Project* project) : project_(project), is_running_(false) {
    if (!project_) {
      throw std::invalid_argument("Project cannot be null");
    }
  }

  // Helper method to get tool by ID
  orc::AnalysisTool* getToolById(const std::string& tool_id) const {
    auto& registry = orc::AnalysisRegistry::instance();
    return registry.findById(tool_id);
  }

  orc::Project* project_;
  std::shared_ptr<orc::Project> project;  // Shared pointer for context
  std::shared_ptr<orc::DAG> dag;          // DAG for analysis context
  bool is_running_;
};

AnalysisPresenter::AnalysisPresenter(void* project_handle)
    : impl_(
          std::make_unique<Impl>(static_cast<orc::Project*>(project_handle))) {}

AnalysisPresenter::~AnalysisPresenter() = default;

AnalysisPresenter::AnalysisPresenter(AnalysisPresenter&&) noexcept = default;
AnalysisPresenter& AnalysisPresenter::operator=(AnalysisPresenter&&) noexcept =
    default;

bool AnalysisPresenter::runSNRAnalysis(
    orc::NodeID node_id, AnalysisProgressCallback progress_callback) {
  return false;
}

bool AnalysisPresenter::runDropoutAnalysis(
    orc::NodeID node_id, AnalysisProgressCallback progress_callback) {
  return false;
}

bool AnalysisPresenter::runBurstAnalysis(
    orc::NodeID node_id, AnalysisProgressCallback progress_callback) {
  return false;
}

bool AnalysisPresenter::runQualityAnalysis(
    orc::NodeID node_id, AnalysisProgressCallback progress_callback) {
  return false;
}

void AnalysisPresenter::cancelAnalysis() { impl_->is_running_ = false; }

SNRAnalysisData AnalysisPresenter::getSNRAnalysis(orc::NodeID node_id) const {
  return SNRAnalysisData{};
}

DropoutAnalysisData AnalysisPresenter::getDropoutAnalysis(
    orc::NodeID node_id) const {
  return DropoutAnalysisData{};
}

BurstAnalysisData AnalysisPresenter::getBurstAnalysis(
    orc::NodeID node_id) const {
  return BurstAnalysisData{};
}

QualityAnalysisData AnalysisPresenter::getQualityAnalysis(
    orc::NodeID node_id) const {
  return QualityAnalysisData{};
}

bool AnalysisPresenter::hasAnalysisData(orc::NodeID node_id,
                                        AnalysisType type) const {
  return false;
}

void AnalysisPresenter::setAnalysisParameters(
    orc::NodeID node_id, AnalysisType type,
    const std::map<std::string, std::string>& parameters) {}

std::map<std::string, std::string> AnalysisPresenter::getAnalysisParameters(
    orc::NodeID node_id, AnalysisType type) const {
  return {};
}

bool AnalysisPresenter::exportToCSV(orc::NodeID node_id, AnalysisType type,
                                    const std::string& output_path) const {
  return false;
}

// === Analysis Tool Registry (Phase 2.4) ===

std::vector<orc::AnalysisToolInfo> AnalysisPresenter::getToolsForStage(
    const std::string& stage_name) const {
  std::vector<orc::AnalysisToolInfo> result;
  std::unordered_set<std::string> existing_ids;

  auto& registry = orc::AnalysisRegistry::instance();
  auto all_tools = registry.tools();

  // Filter tools applicable to this stage
  for (const auto* tool : all_tools) {
    if (!tool || !tool->isApplicableToStage(stage_name)) {
      continue;
    }

    orc::AnalysisToolInfo info;
    info.id = tool->id();
    info.name = tool->name();
    info.description = tool->description();
    info.category = tool->category();
    info.priority = tool->priority();
    info.applicable_stages.push_back(stage_name);
    applyLegacyToolMetadata(info);

    existing_ids.insert(info.id);
    result.push_back(std::move(info));
  }

  // Phase B: allow stages to advertise helper/tool contracts directly via SDK.
  try {
    auto stage = orc::StageRegistry::instance().create_stage(stage_name);
    if (stage) {
      auto* provider = dynamic_cast<orc::StageToolProvider*>(stage.get());
      if (provider) {
        const auto descriptors = provider->get_stage_tools();
        for (const auto& descriptor : descriptors) {
          if (descriptor.tool_id.empty() ||
              existing_ids.find(descriptor.tool_id) != existing_ids.end()) {
            continue;
          }

          orc::AnalysisToolInfo info;
          info.id = descriptor.tool_id;
          info.name = descriptor.display_name;
          info.description = descriptor.description;
          switch (descriptor.kind) {
            case orc::StageToolKind::ConfigDialog:
              info.category = "Stage Config";
              break;
            case orc::StageToolKind::NonModalEditor:
              info.category = "Stage Editor";
              break;
            case orc::StageToolKind::BatchAnalysis:
            case orc::StageToolKind::PluginView:
              // A plugin's own view of what a run left behind is a batch
              // analysis with a browsable result, so it belongs in the same
              // menu.
              info.category = "Stage Analysis";
              break;
            case orc::StageToolKind::PreviewUtility:
              info.category = "Preview";
              break;
            default:
              info.category = "Stage Tools";
              break;
          }
          info.priority = 25;
          info.applicable_stages.push_back(stage_name);
          info.stage_tool_kind = stageToolKindToString(descriptor.kind);
          info.stage_tool_contract = descriptor.contract_id;
          info.stage_tool_non_modal = descriptor.non_modal;

          existing_ids.insert(info.id);
          result.push_back(std::move(info));
        }
      }
    }
  } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
    // Missing/invalid stage registration should not block standard analysis
    // tool discovery.
  }

  // Sort by priority (lower = first), then alphabetically
  std::sort(result.begin(), result.end(),
            [](const orc::AnalysisToolInfo& a, const orc::AnalysisToolInfo& b) {
              if (a.priority != b.priority) {
                return a.priority < b.priority;
              }
              return a.name < b.name;
            });

  return result;
}

orc::AnalysisToolInfo AnalysisPresenter::getToolInfo(
    const std::string& tool_id) const {
  auto& registry = orc::AnalysisRegistry::instance();
  auto* tool = registry.findById(tool_id);

  if (!tool) {
    return orc::AnalysisToolInfo{};  // Empty info
  }

  orc::AnalysisToolInfo info;
  info.id = tool->id();
  info.name = tool->name();
  info.description = tool->description();
  info.category = tool->category();
  info.priority = tool->priority();
  applyLegacyToolMetadata(info);

  return info;
}

// === Generic Analysis Execution (Phase 2.8) ===

std::vector<orc::ParameterDescriptor> AnalysisPresenter::getToolParameters(
    const std::string& tool_id, orc::AnalysisSourceType source_type) const {
  auto* tool = impl_->getToolById(tool_id);
  if (!tool) {
    return {};
  }

  // Create minimal context for parameter query
  orc::AnalysisContext context;
  context.source_type = static_cast<orc::AnalysisSourceType>(source_type);
  context.project = impl_->project;
  context.dag = impl_->dag;

  return tool->parametersForContext(context);
}

}  // namespace orc::presenters
