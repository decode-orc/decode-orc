/*
 * File:        analysis_presenter.cpp
 * Module:      orc-presenters
 * Purpose:     The tools a stage offers, and their parameters
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "analysis_presenter.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

#include "../../sdk/include/orc/plugin/orc_stage_tooling.h"
#include "../core/include/stage_registry.h"
#include "../include/stage_analysis_tools.h"

namespace orc::presenters {

namespace {

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
    case orc::StageToolKind::Analysis:
      return "analysis";
    default:
      return "";
  }
}

}  // namespace

class AnalysisPresenter::Impl {
 public:
  explicit Impl(orc::Project* project) : project_(project) {
    if (!project_) {
      throw std::invalid_argument("Project cannot be null");
    }
  }

  orc::Project* project_;
};

AnalysisPresenter::AnalysisPresenter(void* project_handle)
    : impl_(
          std::make_unique<Impl>(static_cast<orc::Project*>(project_handle))) {}

AnalysisPresenter::~AnalysisPresenter() = default;

AnalysisPresenter::AnalysisPresenter(AnalysisPresenter&&) noexcept = default;
AnalysisPresenter& AnalysisPresenter::operator=(AnalysisPresenter&&) noexcept =
    default;

std::vector<orc::AnalysisToolInfo> AnalysisPresenter::getToolsForStage(
    const std::string& stage_name) const {
  std::vector<orc::AnalysisToolInfo> result;
  std::unordered_set<std::string> existing_ids;

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
          info.applicable_stages.push_back(stage_name);
          info.stage_tool_kind = stageToolKindToString(descriptor.kind);
          info.stage_tool_contract = descriptor.contract_id;

          existing_ids.insert(info.id);
          result.push_back(std::move(info));
        }
      }
    }
  } catch (const std::exception&) {  // NOLINT(bugprone-empty-catch)
    // A stage that is not registered, or fails to construct, offers no
    // tools.
  }

  std::sort(result.begin(), result.end(),
            [](const orc::AnalysisToolInfo& a, const orc::AnalysisToolInfo& b) {
              return a.name < b.name;
            });

  return result;
}

std::vector<orc::ParameterDescriptor> AnalysisPresenter::getToolParameters(
    const std::string& tool_id, const std::string& stage_name) const {
  return stageAnalysisToolParameters(stage_name, tool_id);
}

std::vector<orc::ParameterDescriptor> AnalysisPresenter::getToolParameters(
    const orc::AnalysisToolInfo& tool) const {
  return getToolParameters(tool.id, tool.applicable_stages.empty()
                                        ? std::string()
                                        : tool.applicable_stages.front());
}

}  // namespace orc::presenters
