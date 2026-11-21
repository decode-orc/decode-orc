/*
 * File:        burst_level_analysis_sink_stage.h
 * Module:      orc-core
 * Purpose:     Burst Level Analysis Sink Stage - computes burst statistics and
 * optionally writes CSV
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_CORE_BURST_LEVEL_ANALYSIS_SINK_STAGE_H
#define ORC_CORE_BURST_LEVEL_ANALYSIS_SINK_STAGE_H

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/plugin/orc_stage_tooling.h>
#include <orc/stage/node_type.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/tooling/stage_results_report.h>
#include <orc/stage/tooling/stage_tool_requests.h>
#include <orc/stage/triggerable_stage.h>

#include <atomic>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "burst_level_analysis_sink_deps_interface.h"
#include "burst_level_analysis_types.h"

namespace orc {

/**
 * @brief Burst Level Analysis Sink Stage
 *
 * Trigger to compute burst level stats across input fields. Optionally writes
 * CSV. Dataset is cached for GUI retrieval after trigger.
 */
class BurstLevelAnalysisSinkStage : public DAGStage,
                                    public ParameterizedStage,
                                    public TriggerableStage,
                                    public StageToolProvider,
                                    public IStagePreviewCapability,
                                    public IStageToolRequestHandler,
                                    public IStageResultsReportProvider {
 public:
  BurstLevelAnalysisSinkStage();
  ~BurstLevelAnalysisSinkStage() override = default;

  void set_deps_override(
      std::shared_ptr<IBurstLevelAnalysisSinkStageDeps> deps) {
    deps_override_ = deps;
  }

  // DAGStage interface
  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD
  NodeTypeInfo get_node_type_info() const override;

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      ObservationContext& observation_context) override;

  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 0; }

  // ParameterizedStage interface
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;
  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // TriggerableStage interface
  bool trigger(const std::vector<ArtifactPtr>& inputs,
               const std::map<std::string, ParameterValue>& parameters,
               IObservationContext& observation_context) override;

  std::string get_trigger_status() const override { return last_status_; }

  void set_progress_callback(TriggerProgressCallback callback) override {
    progress_callback_ = callback;
  }
  bool is_trigger_in_progress() const override { return is_processing_.load(); }
  void cancel_trigger() override { cancel_requested_.store(true); }

  // Results of the last trigger
  const std::vector<FrameBurstLevelStats>& frame_stats() const {
    return frame_stats_;
  }
  int32_t total_frames() const { return total_frames_; }
  bool has_results() const { return has_results_; }

  // IStageToolRequestHandler: the "results" verb of the tool's view
  StageToolReply handle_tool_request(
      const std::string& contract_id, const std::string& verb,
      const std::vector<uint8_t>& payload) override;

  // IStageResultsReportProvider: what orc-cli tools run prints
  bool results_report(StageResultsReport* report) const override;

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

  std::vector<StageToolDescriptor> get_stage_tools() const override {
    return {StageToolDescriptor{
        "burst_level_analysis", "Burst Level Analysis",
        "Compute and visualize per-frame colour-burst levels.",
        StageToolKind::BatchAnalysis, false,
        "decode-orc.stage-tools.burst-level-analysis.v1"}};
  }

 private:
  struct ParsedConfig {
    std::string output_path;
    bool write_csv = false;
  };

  ParsedConfig parse_config(
      const std::map<std::string, ParameterValue>& parameters) const;

  std::map<std::string, ParameterValue> parameters_;
  TriggerProgressCallback progress_callback_;
  std::atomic<bool> is_processing_{false};
  std::atomic<bool> cancel_requested_{false};
  std::string last_status_;

  std::vector<FrameBurstLevelStats> frame_stats_;
  int32_t total_frames_ = 0;
  bool has_results_ = false;
  // The analysed source's levels, for the view's amplitude units.
  VideoSystem video_system_ = VideoSystem::Unknown;
  int32_t blanking_level_ = -1;
  int32_t white_level_ = -1;
  std::shared_ptr<IBurstLevelAnalysisSinkStageDeps> deps_override_;
  mutable std::shared_ptr<const VideoFrameRepresentation> cached_input_;
};

}  // namespace orc

#endif  // ORC_CORE_BURST_LEVEL_ANALYSIS_SINK_STAGE_H
