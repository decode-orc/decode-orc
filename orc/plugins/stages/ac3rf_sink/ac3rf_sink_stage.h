/*
 * File:        ac3rf_sink_stage.h
 * Module:      orc-core
 * Purpose:     AC3 RF Sink Stage - decodes AC3 RF samples and writes AC3 frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_AC3RF_SINK_STAGE_H
#define ORC_CORE_AC3RF_SINK_STAGE_H

#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/node_type.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/triggerable_stage.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace orc {

class IAC3RFSinkStageDeps;

/**
 * @brief AC3 RF Sink Stage
 *
 * Decodes AC3 RF samples from the VideoFrameRepresentation and writes the
 * resulting AC3 frames to an output file.
 *
 * This is a SINK stage — it takes one VFR input and produces no outputs.
 *
 * The AC3 RF data must be present in the source stage's VFR (i.e. the source
 * stage must have been given an AC3 RF input file so that has_ac3_rf() returns
 * true).
 *
 * Parameters:
 * - output_path: Output .ac3 file path
 */
class AC3RFSinkStage : public DAGStage,
                       public ParameterizedStage,
                       public TriggerableStage {
 public:
  AC3RFSinkStage();
  /// Testing seam: inject a pre-built deps instance to substitute concrete dep
  /// creation in trigger().
  void set_deps_override(std::shared_ptr<IAC3RFSinkStageDeps> deps) {
    deps_override_ = std::move(deps);
  }
  ~AC3RFSinkStage() override = default;

  // DAGStage interface
  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD
  NodeTypeInfo get_node_type_info() const override;

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      ObservationContext& observation_context) override;

  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 0; }  // Sink has no outputs

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
  std::shared_ptr<IAC3RFSinkStageDeps> deps_override_;
  std::atomic<bool> cancel_requested_{false};
  std::string last_status_;
};

}  // namespace orc

#endif  // ORC_CORE_AC3RF_SINK_STAGE_H
