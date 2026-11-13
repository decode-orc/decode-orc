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
#include <orc/support/frame_cache.h>

#include <algorithm>
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

  // The readers this stage's frame cache is sized for, at most.
  size_t max_concurrent_frame_requests() const override {
    return std::min(source_->max_concurrent_frame_requests(),
                    nominal_frame_cache_capacity());
  }

  // Thread safety: every method may be called from many threads at once. A
  // pointer from get_frame() or get_line() stays valid until the calling
  // thread's next call on this object.

 private:
  // A frame is fitted in about 150 ms where its fields carry flat colour, so
  // each must be fitted once: the cache holds a stacker's reach (its
  // still-frame search either side and its signature lookahead) beside the
  // sink's span (frame_cache_capacity()). A search wider than the default
  // refits the frames past that reach.
  double carrier_black_hz_;

  // Frames with the beat removed; an empty entry is a frame neither of whose
  // fields was corrected, read from the source as it is.
  using Frame = std::shared_ptr<const std::vector<int16_t>>;
  mutable FrameCache<std::vector<int16_t>> frames_;

  Frame cancelled(FrameID id) const;
  Frame build_cancelled(FrameID id) const;
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
