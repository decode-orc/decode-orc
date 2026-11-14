/*
 * File:        analysis_tool_presenter.cpp
 * Module:      orc-presenters
 * Purpose:     Running a stage's analysis tool and applying its result
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "analysis_tool_presenter.h"

#include <orc/support/logging.h>

#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

#include "../core/include/project.h"
#include "../include/stage_analysis_tools.h"

namespace orc::presenters {

class AnalysisToolPresenter::Impl {
 public:
  Impl(std::string tool_id, orc::Project* project)
      : tool_id_(std::move(tool_id)), project_(project) {
    if (!project_) {
      throw std::invalid_argument(
          "AnalysisToolPresenter: project cannot be null");
    }
  }

  std::string tool_id_;
  orc::Project* project_;
};

AnalysisToolPresenter::AnalysisToolPresenter(std::string tool_id,
                                             void* project_handle)
    : impl_(std::make_unique<Impl>(
          std::move(tool_id), static_cast<orc::Project*>(project_handle))) {}

AnalysisToolPresenter::~AnalysisToolPresenter() = default;

orc::AnalysisResult AnalysisToolPresenter::runAnalysis(
    NodeID node_id,
    const std::map<std::string, orc::ParameterValue>& parameters,
    std::function<void(int, const std::string&)> progress_callback,
    std::function<bool()> cancel_check) {
  StageAnalysisRun run(impl_->project_, node_id, impl_->tool_id_, parameters);
  return run.run(progress_callback, cancel_check);
}

bool AnalysisToolPresenter::prepareResultChanges(
    const orc::AnalysisResult& result, orc::NodeID /*node_id*/,
    std::map<std::string, orc::ParameterValue>* changes) const {
  // The stage's report holds the changes themselves.
  if (result.status != orc::AnalysisResult::Success ||
      result.parameterChanges.empty()) {
    return false;
  }
  if (changes) {
    *changes = result.parameterChanges;
  }
  return true;
}

bool AnalysisToolPresenter::applyResultToGraph(
    const orc::AnalysisResult& result, orc::NodeID node_id) {
  std::map<std::string, orc::ParameterValue> changes;
  if (!prepareResultChanges(result, node_id, &changes)) {
    return false;
  }

  try {
    // set_node_parameters replaces the node's whole parameter set, so the
    // tool's changes join the stored values: a Disc Mapper apply writes
    // `ranges` and must leave the Frame Map's padding settings alone.
    const auto& nodes = impl_->project_->get_nodes();
    const auto node_it = std::find_if(
        nodes.begin(), nodes.end(),
        [&node_id](const auto& n) { return n.node_id == node_id; });
    if (node_it == nodes.end()) {
      ORC_LOG_ERROR("AnalysisToolPresenter: Node {} not found",
                    node_id.value());
      return false;
    }
    auto merged = node_it->parameters;
    for (const auto& [key, value] : changes) {
      merged[key] = value;
    }
    orc::project_io::set_node_parameters(*impl_->project_, node_id, merged);
    ORC_LOG_INFO(
        "AnalysisToolPresenter: Applied {} parameter changes from {} "
        "to node {}",
        changes.size(), impl_->tool_id_, node_id.value());
    return true;
  } catch (const std::exception& e) {
    ORC_LOG_ERROR("AnalysisToolPresenter: Exception applying result: {}",
                  e.what());
    return false;
  }
}

}  // namespace orc::presenters
