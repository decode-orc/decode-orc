/*
 * File:        cvbs_sink_stage.h
 * Module:      orc-core
 * Purpose:     CVBS Sink Stage - writes CVBS file-format family output
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_CVBS_SINK_STAGE_H
#define ORC_CORE_CVBS_SINK_STAGE_H

#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/node_type.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace orc {

class ICVBSSinkStageDeps;

class CVBSSinkStage : public DAGStage,
                      public ParameterizedStage,
                      public TriggerableStage {
 public:
  CVBSSinkStage();

  void set_deps_override(std::shared_ptr<ICVBSSinkStageDeps> deps) {
    deps_override_ = std::move(deps);
  }

  ~CVBSSinkStage() override = default;

  // DAGStage interface
  std::string version() const override { return "1.1"; }
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

  std::string get_trigger_status() const override;

  void set_progress_callback(TriggerProgressCallback callback) override {
    progress_callback_ = callback;
  }

  bool is_trigger_in_progress() const override { return is_processing_.load(); }

  void cancel_trigger() override { cancel_requested_.store(true); }

 private:
  std::map<std::string, ParameterValue> parameters_;

  TriggerProgressCallback progress_callback_;
  std::atomic<bool> is_processing_{false};
  std::shared_ptr<ICVBSSinkStageDeps> deps_override_;
  std::atomic<bool> cancel_requested_{false};
  std::string last_status_;
};

}  // namespace orc

#endif  // ORC_CORE_CVBS_SINK_STAGE_H
