/*
 * File:        source_align_stage.h
 * Module:      orc-core
 * Purpose:     Source alignment stage for synchronizing multiple sources
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/preview/stage_custom_preview_renderer.h>
#include <orc/stage/video_frame_representation.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace orc {

/**
 * @brief Source alignment stage that synchronizes multiple sources.
 *
 * Many-to-Many stage. Aligns multiple VFrameR inputs so that output
 * frame_id 0 represents the same content across all sources.
 * Alignment comes from a manual alignment map ("1+2, 2+2, 3+1, 4+1" =
 * input_id+frame_offset), typically filled in by the source alignment
 * analysis tool; with no map the stage is not configured.
 */
class SourceAlignStage : public DAGStage,
                         public ParameterizedStage,
                         public IStagePreviewCapability,
                         public IStageCustomPreviewRenderer {
 public:
  SourceAlignStage();

  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::COMPLEX,
                        "source_align",
                        "Source Align",
                        "Synchronize multiple sources by VBI frame number",
                        1,
                        16,
                        2,
                        UINT32_MAX,
                        VideoFormatCompatibility::ALL};
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      ObservationContext& observation_context) override;

  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return UINT32_MAX; }

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

  // IStageCustomPreviewRenderer
  std::vector<PreviewOption> get_preview_options() const override;
  PreviewImage render_preview(const std::string& option_id, uint64_t index,
                              PreviewNavigationHint hint) const override;

  // ParameterizedStage
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;
  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

 private:
  static std::vector<std::pair<size_t, size_t>> parse_alignment_map(
      const std::string& alignment_spec);

  mutable std::mutex execute_mutex_;

  // Reporting state
  mutable std::vector<FrameID> alignment_offsets_;
  mutable std::vector<std::shared_ptr<const VideoFrameRepresentation>>
      input_sources_;

  // Preview state
  mutable std::vector<std::shared_ptr<const VideoFrameRepresentation>>
      cached_outputs_;

  // What the last aligned run returned and the map and mode it was built
  // from, handed back by execute() while those and input_sources_ are
  // unchanged.
  std::vector<ArtifactPtr> last_outputs_;
  std::string last_alignment_map_;
  std::string last_alignment_mode_;

  // Parameters
  std::string alignment_map_;
  std::string alignment_mode_{"pad_for_alignment"};
};

}  // namespace orc
