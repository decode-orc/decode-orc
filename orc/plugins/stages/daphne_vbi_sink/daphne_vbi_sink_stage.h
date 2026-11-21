/*
 * File:        daphne_vbi_sink_stage.h
 * Module:      orc-core
 * Purpose:     Generate .VBI binary files
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Matt Ownby
 */

#ifndef ORC_CORE_VBI_SINK_STAGE_H
#define ORC_CORE_VBI_SINK_STAGE_H

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/node_type.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace orc {

class IStageServices;
class IDaphneVBISinkStageDeps;

/**
 * @brief .VBI binary Sink Stage
 *
 * Writes VBI data from each field to a binary file according to the
 * specification at https://www.daphne-emu.com:9443/mediawiki/index.php/VBIInfo
 * . This is a SINK stage - it has inputs but no outputs.
 *
 * When triggered, it reads all fields from its input and writes them to:
 * - .vbi file: Binary data.
 *
 * Parameters:
 * - output_path: Output file path
 */
class DaphneVBISinkStage : public DAGStage,
                           public ParameterizedStage,
                           public TriggerableStage,
                           public IStagePreviewCapability {
 public:
  explicit DaphneVBISinkStage(IStageServices* stage_services);

  /// Testing seam: inject a pre-built deps instance to substitute concrete dep
  /// creation in trigger().
  void set_deps_override(std::shared_ptr<IDaphneVBISinkStageDeps> deps) {
    deps_override_ = std::move(deps);
  }
  ~DaphneVBISinkStage() override = default;

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

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

 private:
  std::string output_path_;
  std::string trigger_status_;
  TriggerProgressCallback
      progress_callback_;  // Progress callback for trigger operations
  std::atomic<bool> is_processing_{false};
  std::atomic<bool> cancel_requested_{false};
  IStageServices* stage_services_{nullptr};
  std::shared_ptr<IDaphneVBISinkStageDeps> deps_override_;
  mutable std::shared_ptr<const VideoFrameRepresentation> cached_input_;
};
}  // namespace orc

#endif  // ORC_CORE_VBI_SINK_STAGE_H