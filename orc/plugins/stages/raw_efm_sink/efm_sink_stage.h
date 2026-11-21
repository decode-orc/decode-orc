/*
 * File:        efm_sink_stage.h
 * Module:      orc-core
 * Purpose:     Raw EFM Data Sink Stage - writes EFM t-values to raw file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef ORC_CORE_EFM_SINK_STAGE_H
#define ORC_CORE_EFM_SINK_STAGE_H

#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/node_type.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/triggerable_stage.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace orc {

class IRawEFMSinkStageDeps;

/**
 * @brief Raw EFM Data Sink Stage
 *
 * Extracts EFM (Eight to Fourteen Modulation) t-values from TBC metadata
 * and writes them to a raw binary file.
 * This is a SINK stage - it has inputs but no outputs.
 *
 * The EFM data flows through the VideoFrameRepresentation from the source
 * stage, which reads the .efm file (if specified in the source stage
 * parameters).
 *
 * The EFM data format is:
 * - Raw 8-bit unsigned integers
 * - Valid t-values range from 3 to 11 (inclusive)
 * - Sequential field-by-field storage
 *
 * This stage extracts the EFM data from the VFR and writes it to a binary file
 * with no headers or formatting - just raw t-values.
 *
 * Parameters:
 * - output_path: Output EFM file path
 */
class RawEFMSinkStage : public DAGStage,
                        public ParameterizedStage,
                        public TriggerableStage {
 public:
  RawEFMSinkStage();
  /// Testing seam: inject a pre-built deps instance to substitute concrete dep
  /// creation in trigger().
  void set_deps_override(std::shared_ptr<IRawEFMSinkStageDeps> deps) {
    deps_override_ = std::move(deps);
  }
  ~RawEFMSinkStage() override = default;

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
  // Store parameters for inspection
  std::map<std::string, ParameterValue> parameters_;

  // Progress tracking
  TriggerProgressCallback progress_callback_;
  std::atomic<bool> is_processing_{false};
  std::shared_ptr<IRawEFMSinkStageDeps> deps_override_;
  std::atomic<bool> cancel_requested_{false};
  std::string last_status_;
};

}  // namespace orc

#endif  // ORC_CORE_EFM_SINK_STAGE_H
