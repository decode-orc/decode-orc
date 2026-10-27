/*
 * File:        source_alignment_presenter.cpp
 * Module:      orc-presenters
 * Purpose:     Presenter for Source Alignment analysis tool
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../include/source_alignment_presenter.h"

#include <orc/stage/stage.h>
#include <orc/support/logging.h>

#include <algorithm>

#include "../../core/analysis/analysis_progress.h"
#include "../../core/analysis/analysis_registry.h"
#include "../../core/include/dag_executor.h"
#include "../../core/include/project.h"

namespace orc::presenters {

SourceAlignmentPresenter::SourceAlignmentPresenter(void* project_handle)
    : AnalysisToolPresenter(project_handle) {}

std::string SourceAlignmentPresenter::toolId() const {
  return "source_alignment";
}

std::string SourceAlignmentPresenter::toolName() const {
  return "Source Alignment Analysis";
}

bool SourceAlignmentPresenter::validateNode(NodeID node_id,
                                            std::string& error_message) {
  auto dag_void = getOrBuildDAG();
  if (!dag_void) {
    error_message = "Failed to build DAG from project";
    return false;
  }
  auto dag = std::static_pointer_cast<orc::DAG>(dag_void);

  const auto& dag_nodes = dag->nodes();
  auto node_it = std::find_if(
      dag_nodes.begin(), dag_nodes.end(),
      [&node_id](const DAGNode& node) { return node.node_id == node_id; });

  if (node_it == dag_nodes.end()) {
    error_message = "Node not found in DAG";
    return false;
  }

  if (!node_it->stage ||
      node_it->stage->get_node_type_info().stage_name != "source_align") {
    error_message =
        "Source Alignment Analysis only applies to source_align stages";
    return false;
  }

  if (node_it->input_node_ids.empty()) {
    error_message =
        "Source Alignment Analysis requires at least one input source.\n\n"
        "The source_align stage needs input connections from field_map stages\n"
        "that are processing different TBC captures of the same disc.";
    return false;
  }

  return true;
}

orc::AnalysisResult SourceAlignmentPresenter::runAnalysis(
    NodeID node_id,
    const std::map<std::string, orc::ParameterValue>& parameters,
    std::function<void(int, const std::string&)> progress_callback,
    std::function<bool()> cancel_check) {
  orc::AnalysisResult result;
  result.status = orc::AnalysisResult::Status::Failed;

  // Report progress
  if (progress_callback) {
    progress_callback(0, "Initializing source alignment analysis...");
  }

  // Get the analysis tool from registry
  auto tool = orc::AnalysisRegistry::instance().findById(toolId());
  if (!tool) {
    result.summary = "Source Alignment Analysis tool not found in registry";
    ORC_LOG_ERROR("{}", result.summary);
    return result;
  }

  // Build DAG
  auto dag_void = getOrBuildDAG();
  if (!dag_void) {
    result.summary = "Failed to build DAG from project";
    ORC_LOG_ERROR("{}", result.summary);
    return result;
  }
  auto dag = std::static_pointer_cast<orc::DAG>(dag_void);

  // Validate node
  std::string error_message;
  if (!validateNode(node_id, error_message)) {
    result.summary = error_message;
    ORC_LOG_ERROR("{}", result.summary);
    return result;
  }

  if (progress_callback) {
    progress_callback(5, "Building analysis context...");
  }

  // Create analysis context
  // The Source Alignment tool needs:
  // - DAG context (to execute input nodes and find VBI data)
  // - Project context (for node access)
  // - Node ID being analyzed
  orc::AnalysisContext ctx;
  ctx.source_type = orc::AnalysisSourceType::LaserDisc;
  ctx.node_id = node_id;
  ctx.parameters = parameters;
  ctx.dag = dag;
  // Create snapshot of project for analysis context
  // Create snapshot of project for analysis context (cast opaque handle)
  ctx.project = std::make_shared<orc::Project>(
      *static_cast<orc::Project*>(getProjectPointer()));

  // Create progress adapter to convert AnalysisProgress to our callback
  class ProgressAdapter : public orc::AnalysisProgress {
   public:
    ProgressAdapter(std::function<void(int, const std::string&)> callback,
                    std::function<bool()> cancel_check)
        : callback_(callback), cancel_check_(std::move(cancel_check)) {}

    void setProgress(int percentage) override {
      current_percentage_ = percentage;
      if (callback_) {
        callback_(percentage, current_status_);
      }
    }

    void setStatus(const std::string& status) override {
      current_status_ = status;
      if (callback_) {
        callback_(current_percentage_, status);
      }
    }

    void setSubStatus(const std::string& message) override {
      // Not mapped separately; reuse status channel
      setStatus(message);
    }

    bool isCancelled() const override {
      return cancel_check_ && cancel_check_();
    }

    void reportPartialResult(const orc::AnalysisResult::ResultItem&) override {
      // Not used for this presenter
    }

   private:
    std::function<void(int, const std::string&)> callback_;
    std::function<bool()> cancel_check_;
    std::string current_status_;
    int current_percentage_ = 0;
  };

  ProgressAdapter progress_adapter(progress_callback, cancel_check);

  if (progress_callback) {
    progress_callback(10, "Running source alignment analysis...");
  }

  // Run the analysis
  orc::AnalysisResult core_result = tool->analyze(ctx, &progress_adapter);

  // Convert core result to public API result
  result.status = core_result.status == orc::AnalysisResult::Success
                      ? orc::AnalysisResult::Status::Success
                      : (core_result.status == orc::AnalysisResult::Cancelled
                             ? orc::AnalysisResult::Status::Cancelled
                             : orc::AnalysisResult::Status::Failed);
  result.summary = core_result.summary;
  result.statistics = core_result.statistics;

  // Convert result items
  result.items.clear();
  for (const auto& core_item : core_result.items) {
    orc::AnalysisResultItem item;
    item.type = core_item.type;
    item.message = core_item.message;
    item.startFrame = core_item.startFrame;
    item.endFrame = core_item.endFrame;
    item.metadata = core_item.metadata;
    result.items.push_back(item);
  }

  // Store graph data if present (for Apply button)
  result.graphData = core_result.graphData;

  if (progress_callback) {
    if (result.status == orc::AnalysisResult::Status::Success) {
      progress_callback(100, "Analysis complete");
    } else if (result.status == orc::AnalysisResult::Status::Cancelled) {
      progress_callback(0, "Analysis cancelled");
    } else {
      progress_callback(0, "Analysis failed");
    }
  }

  ORC_LOG_DEBUG("Source Alignment Analysis complete: status={}, summary={}",
                static_cast<int>(result.status), result.summary);

  return result;
}

}  // namespace orc::presenters
