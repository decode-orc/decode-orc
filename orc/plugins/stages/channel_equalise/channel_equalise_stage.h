/*
 * File:        channel_equalise_stage.h
 * Module:      orc-stage-plugin-channel-equalise
 * Purpose:     Removes the PAL LaserVision CAV edge smear from each frame and
 *              restores the chroma level (composite stays composite)
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
#include <orc/support/channel_equalise.h>
#include <orc/support/lru_cache.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace orc {

// ============================================================================
// EqualisedVideoFrameRepresentation
// ============================================================================
class EqualisedVideoFrameRepresentation
    : public VideoFrameRepresentationWrapper,
      public Artifact {
 public:
  EqualisedVideoFrameRepresentation(
      std::shared_ptr<const VideoFrameRepresentation> source,
      channel_equalise::EdgeSmear smear);

  std::string type_name() const override {
    return "equalised_video_frame_representation";
  }

  const int16_t* get_frame(FrameID id) const override;
  const int16_t* get_line(FrameID id, size_t line) const override;
  std::vector<int16_t> get_frame_copy(FrameID id) const override;

 private:
  // A frame costs a few milliseconds; the video sink's 3D decoders read a
  // few frames either side of the one they decode.
  static constexpr size_t kMaxCachedFrames = 16;

  channel_equalise::EdgeSmear smear_;

  // Equalised frames; an empty entry is a frame read from the source as it
  // is (not PAL composite, or short).
  mutable LRUCache<FrameID, std::vector<int16_t>> frames_;

  const std::vector<int16_t>* equalised(FrameID id) const;
};

// ============================================================================
// ChannelEqualiseStage
// ============================================================================
class ChannelEqualiseStage : public DAGStage,
                             public ParameterizedStage,
                             public IStagePreviewCapability {
 public:
  ChannelEqualiseStage();

  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::TRANSFORM,
                        "channel_equalise",
                        "Channel Equalise",
                        "Remove the edge smear PAL LaserDisc CAV masters "
                        "carry and restore the chroma level",
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
  channel_equalise::EdgeSmear smear_{0.0, 6.1};

  mutable std::shared_ptr<const VideoFrameRepresentation> cached_output_;
};

}  // namespace orc
