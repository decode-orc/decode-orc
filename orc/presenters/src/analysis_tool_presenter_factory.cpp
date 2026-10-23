/*
 * File:        analysis_tool_presenter_factory.cpp
 * Module:      orc-presenters
 * Purpose:     Tool id to analysis tool presenter selection
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../include/analysis_tool_presenter_factory.h"

#include <orc/support/logging.h>

#include <memory>
#include <utility>

#include "../../core/analysis/analysis_progress.h"
#include "../../core/analysis/analysis_registry.h"
#include "../../core/analysis/analysis_tool.h"
#include "../../core/include/dag_executor.h"
#include "../../core/include/project.h"
#include "../include/disc_mapper_presenter.h"
#include "../include/dropout_editor_presenter.h"
#include "../include/ffmpeg_preset_presenter.h"
#include "../include/frame_corruption_presenter.h"
#include "../include/frame_map_range_presenter.h"
#include "../include/mask_line_presenter.h"
#include "../include/source_alignment_presenter.h"

namespace orc::presenters {

namespace {

/**
 * @brief Runs a registered tool with no presenter-side preparation
 *
 * The context is the one every dedicated presenter builds: the node, the
 * parameters, the project's DAG and a snapshot of the project.
 */
class GenericAnalysisToolPresenter : public AnalysisToolPresenter {
 public:
  GenericAnalysisToolPresenter(std::string tool_id, void* project_handle)
      : AnalysisToolPresenter(project_handle), tool_id_(std::move(tool_id)) {}

  orc::AnalysisResult runAnalysis(
      NodeID node_id,
      const std::map<std::string, orc::ParameterValue>& parameters,
      std::function<void(int, const std::string&)> progress_callback,
      std::function<bool()> cancel_check) override {
    orc::AnalysisResult result;
    result.status = orc::AnalysisResult::Status::Failed;

    auto* tool = orc::AnalysisRegistry::instance().findById(tool_id_);
    if (!tool) {
      result.summary = "Analysis tool '" + tool_id_ + "' is not registered";
      ORC_LOG_ERROR("{}", result.summary);
      return result;
    }

    std::shared_ptr<orc::DAG> dag;
    try {
      dag = std::static_pointer_cast<orc::DAG>(getOrBuildDAG());
    } catch (const std::exception& e) {
      result.summary = e.what();
      ORC_LOG_ERROR("{}", result.summary);
      return result;
    }

    orc::AnalysisContext ctx;
    ctx.source_type = orc::AnalysisSourceType::LaserDisc;
    ctx.node_id = node_id;
    ctx.parameters = parameters;
    ctx.dag = dag;
    ctx.project = std::make_shared<orc::Project>(
        *static_cast<orc::Project*>(getProjectPointer()));

    Progress progress(std::move(progress_callback), std::move(cancel_check));
    return tool->analyze(ctx, &progress);
  }

 protected:
  std::string toolId() const override { return tool_id_; }
  std::string toolName() const override {
    const auto* tool = orc::AnalysisRegistry::instance().findById(tool_id_);
    return tool ? tool->name() : tool_id_;
  }

 private:
  /// Forwards core progress to the (percentage, status) callback.
  class Progress : public orc::AnalysisProgress {
   public:
    Progress(std::function<void(int, const std::string&)> callback,
             std::function<bool()> cancel_check)
        : callback_(std::move(callback)),
          cancel_check_(std::move(cancel_check)) {}

    void setProgress(int percentage) override {
      last_progress_ = percentage;
      report(status_);
    }
    void setStatus(const std::string& status) override {
      status_ = status;
      report(status_);
    }
    void setSubStatus(const std::string& sub_status) override {
      if (status_.empty() || sub_status.empty()) {
        report(status_ + sub_status);
      } else {
        report(status_ + " - " + sub_status);
      }
    }
    bool isCancelled() const override {
      return cancel_check_ && cancel_check_();
    }
    void reportPartialResult(const orc::AnalysisResult::ResultItem&) override {}

   private:
    void report(const std::string& status) const {
      if (callback_) {
        callback_(last_progress_, status);
      }
    }

    std::function<void(int, const std::string&)> callback_;
    std::function<bool()> cancel_check_;
    int last_progress_ = 0;
    std::string status_;
  };

  std::string tool_id_;
};

}  // namespace

bool hasDedicatedAnalysisToolPresenter(const std::string& tool_id) {
  return tool_id == "frame_corruption" || tool_id == "field_mapping" ||
         tool_id == "disc_mapper" || tool_id == "frame_map_range" ||
         tool_id == "source_alignment" || tool_id == "mask_line_config" ||
         tool_id == "ffmpeg_preset_config" || tool_id == "dropout_editor";
}

std::unique_ptr<AnalysisToolPresenter> makeAnalysisToolPresenter(
    const std::string& tool_id, void* project_handle) {
  if (tool_id == "frame_corruption") {
    return std::make_unique<FrameCorruptionPresenter>(project_handle);
  }
  // "disc_mapper" is an older spelling of the Disc Mapper's id
  if (tool_id == "field_mapping" || tool_id == "disc_mapper") {
    return std::make_unique<DiscMapperPresenter>(project_handle);
  }
  if (tool_id == "frame_map_range") {
    return std::make_unique<FrameMapRangePresenter>(project_handle);
  }
  if (tool_id == "source_alignment") {
    return std::make_unique<SourceAlignmentPresenter>(project_handle);
  }
  if (tool_id == "mask_line_config") {
    return std::make_unique<MaskLinePresenter>(project_handle);
  }
  if (tool_id == "ffmpeg_preset_config") {
    return std::make_unique<FFmpegPresetPresenter>(project_handle);
  }
  if (tool_id == "dropout_editor") {
    return std::make_unique<DropoutEditorPresenter>(project_handle);
  }
  return std::make_unique<GenericAnalysisToolPresenter>(tool_id,
                                                        project_handle);
}

}  // namespace orc::presenters
