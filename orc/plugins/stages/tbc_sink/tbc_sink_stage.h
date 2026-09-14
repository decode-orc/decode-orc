/*
 * File:        tbc_sink_stage.h
 * Module:      orc-core
 * Purpose:     TBC Sink Stage - writes TBC and metadata to disk
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef ORC_CORE_TBC_SINK_STAGE_H
#define ORC_CORE_TBC_SINK_STAGE_H

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/node_type.h>
#include <orc/stage/observation/observation_schema.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/streaming_capability.h>
#include <orc/stage/triggerable_stage.h>
#include <orc/stage/video_frame_representation.h>

#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace orc {

class IStageServices;
class ITBCSinkStageDeps;

/**
 * @brief TBC Sink Stage
 *
 * Writes TBC fields and metadata to disk in format compatible with legacy
 * tools. This is a SINK stage - it has inputs but no outputs.
 *
 * When triggered, it reads all fields from its input and writes them to:
 * - TBC file: Raw field data
 * - .db file: Metadata including all observations and hints
 *
 * This sink supports preview - it shows what will be written to disk.
 *
 * Parameters:
 * - output_path: Output file path (metadata will be output_path + ".db")
 */
class TBCSinkStage : public DAGStage,
                     public ParameterizedStage,
                     public TriggerableStage,
                     public IStagePreviewCapability,
                     public IStreamingCompatibility {
 public:
  explicit TBCSinkStage(IStageServices* stage_services);

  /// Testing seam: inject a pre-built deps instance to substitute concrete dep
  /// creation in trigger().
  void set_deps_override(std::shared_ptr<ITBCSinkStageDeps> deps) {
    deps_override_ = std::move(deps);
  }
  ~TBCSinkStage() override = default;

  // DAGStage interface
  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD
  NodeTypeInfo get_node_type_info() const override;
  std::vector<ObservationKey> get_provided_observations() const override {
    return {ObservationKey{"export", "seq_no", ObservationType::INT64,
                           "1-based sequence number for field", false},
            ObservationKey{"export", "is_first_field", ObservationType::BOOL,
                           "Field parity: first field (true) or second (false)",
                           true}};
  }

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

  // IStreamingCompatibility interface. The .tbc payload is always a single
  // stream (unlike CVBS Sink's composite-vs-Y/C split), so there is no
  // upstream-representation mismatch to catch here; piping simply drops the
  // .db/.pcm/.efm sidecars (see write_tbc_and_metadata()).
  bool supports_streaming_execution() const override { return true; }

 private:
  std::string output_path_;
  // Which pipeline audio channel pair becomes the .pcm sidecar; the lowest
  // pair by default (see get_parameter_descriptors).
  size_t audio_channel_pair_{0};
  std::string trigger_status_;
  mutable std::shared_ptr<const VideoFrameRepresentation>
      cached_input_;  // For preview
  TriggerProgressCallback
      progress_callback_;  // Progress callback for trigger operations
  std::atomic<bool> is_processing_{false};
  std::atomic<bool> cancel_requested_{false};
  IStageServices* stage_services_{nullptr};
  std::shared_ptr<ITBCSinkStageDeps> deps_override_;
};

}  // namespace orc

#endif  // ORC_CORE_TBC_SINK_STAGE_H
