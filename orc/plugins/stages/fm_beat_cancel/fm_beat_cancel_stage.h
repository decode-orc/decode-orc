/*
 * File:        fm_beat_cancel_stage.h
 * Module:      orc-stage-plugin-fm-beat-cancel
 * Purpose:     Removes the PAL LaserDisc FM-beat herringbone from each field
 *              (composite stays composite)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/artifact.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/fm_beat.h>
#include <orc/support/lru_cache.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace orc {

// ============================================================================
// BeatCancelledVideoFrameRepresentation
// ============================================================================
class BeatCancelledVideoFrameRepresentation
    : public VideoFrameRepresentationWrapper,
      public Artifact {
 public:
  BeatCancelledVideoFrameRepresentation(
      std::shared_ptr<const VideoFrameRepresentation> source,
      double carrier_black_hz);

  std::string type_name() const override {
    return "beat_cancelled_video_frame_representation";
  }

  const int16_t* get_frame(FrameID id) const override;
  const int16_t* get_line(FrameID id, size_t line) const override;
  std::vector<int16_t> get_frame_copy(FrameID id) const override;

 private:
  static constexpr size_t kMaxCachedFrames = 64;

  double carrier_black_hz_;

  // Frames with the beat removed; an empty entry is a frame neither of whose
  // fields was corrected, read from the source as it is.
  mutable LRUCache<FrameID, std::vector<int16_t>> frames_;

  const std::vector<int16_t>* cancelled(FrameID id) const;
};

// ============================================================================
// FmBeatCancelStage
// ============================================================================
class FmBeatCancelStage : public DAGStage,
                          public ParameterizedStage,
                          public IStagePreviewCapability {
 public:
  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::TRANSFORM,
                        "fm_beat_cancel",
                        "FM Beat Cancel",
                        "Remove the FM-beat herringbone some PAL LaserDisc "
                        "masters carry in flat saturated colour",
                        1,
                        1,
                        1,
                        UINT32_MAX,
                        VideoFormatCompatibility::PAL_ONLY};
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      ObservationContext& observation_context) override;

  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 1; }

  // ParameterizedStage
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;
  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

 private:
  double carrier_black_mhz_ = fm_beat::kNominalCarrierBlackHz / 1.0e6;

  mutable std::shared_ptr<const VideoFrameRepresentation> cached_output_;
};

}  // namespace orc
