/*
 * File:        mask_line_stage.h
 * Module:      orc-core
 * Purpose:     Line masking stage (VFrameR, frame-flat line addressing)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/plugin/orc_stage_tooling.h>
#include <orc/stage/artifact.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/tooling/stage_tool_jobs.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_cache.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace orc {

// ============================================================================
// MaskedFrameRepresentation
// ============================================================================
// Wraps a VideoFrameRepresentation and blanks specified frame-flat lines.
//
// Line spec format: comma-separated line numbers or inclusive ranges.
//   "100"        – mask frame line 100
//   "100-115"    – mask frame lines 100 through 115 (inclusive)
//   "21,334-336" – mask line 21 and lines 334-336
//
// Mask value is a 10-bit sample level (0–1023, CVBS_U10_4FSC domain).
class MaskedFrameRepresentation : public VideoFrameRepresentationWrapper,
                                  public Artifact {
 public:
  MaskedFrameRepresentation(
      std::shared_ptr<const VideoFrameRepresentation> source,
      const std::string& line_spec, int32_t mask_sample_level);

  std::string type_name() const override {
    return "masked_frame_representation";
  }

  const sample_type* get_frame(FrameID id) const override;
  const sample_type* get_frame_luma(FrameID id) const override;
  const sample_type* get_frame_chroma(FrameID id) const override;
  const sample_type* get_line(FrameID id, size_t line) const override;
  std::vector<sample_type> get_frame_copy(FrameID id) const override;

  const sample_type* get_line_luma(FrameID id, size_t line) const override;
  const sample_type* get_line_chroma(FrameID id, size_t line) const override;

  // Masked frames held across the three planes' caches (each at most
  // cache_capacity()).
  size_t cached_frames() const {
    return masked_frames_.size() + masked_luma_frames_.size() +
           masked_chroma_frames_.size();
  }
  size_t cache_capacity() const { return masked_frames_.capacity(); }

  // The readers this stage's frame cache is sized for, at most.
  size_t max_concurrent_frame_requests() const override {
    return source_ ? std::min(source_->max_concurrent_frame_requests(),
                              nominal_frame_cache_capacity())
                   : nominal_frame_cache_capacity();
  }

 private:
  struct LineRange {
    size_t start;
    size_t end;
  };

  bool should_mask_line(size_t frame_line) const;
  void parse_line_spec(const std::string& line_spec);

  std::vector<LineRange> line_ranges_;
  int32_t mask_sample_level_;

  using Frame = std::shared_ptr<const std::vector<sample_type>>;

  // A masked frame plane, from |cache| or built from the source's plane and
  // cached. The returned pointer is pinned for the calling thread (each
  // plane's cache pins its own), so it stays valid under the
  // VideoFrameRepresentation contract even if another thread evicts the
  // entry.
  enum class Plane { kComposite, kLuma, kChroma };
  using PlaneCache = FrameCache<std::vector<sample_type>>;
  const sample_type* masked_plane(FrameID id, Plane plane,
                                  PlaneCache& cache) const;
  const sample_type* source_plane(FrameID id, Plane plane) const;

  // One line's worth of the mask level, as long as the longest line. Masked
  // lines of every plane point into it; it is written once, at construction,
  // and never again, so concurrent readers share it safely.
  std::vector<sample_type> mask_line_samples_;

  // Masked frames for get_frame() / get_frame_luma() / get_frame_chroma():
  // the chroma sink reads flat frame buffers through these pointer-returning
  // methods, so the masking must be applied here as well as in the line-level
  // accessors. Bounded, so a long export does not keep every frame it read.
  mutable PlaneCache masked_frames_;
  mutable PlaneCache masked_luma_frames_;
  mutable PlaneCache masked_chroma_frames_;
};

// ============================================================================
// MaskLineStage
// ============================================================================
class MaskLineStage : public DAGStage,
                      public ParameterizedStage,
                      public IStagePreviewCapability,
                      public StageToolProvider,
                      public IStageToolJobHandler {
 public:
  MaskLineStage();

  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD
  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::TRANSFORM,
                        "mask_line",
                        "Mask Line",
                        "Mask (blank) specified frame-flat lines",
                        1,
                        1,
                        1,
                        UINT32_MAX,
                        VideoFormatCompatibility::ALL};
  }

  std::vector<ArtifactPtr> execute(
      const std::vector<ArtifactPtr>& inputs,
      const std::map<std::string, ParameterValue>& parameters,
      ObservationContext& observation_context) override;

  size_t required_input_count() const override { return 1; }
  size_t output_count() const override { return 1; }

  std::shared_ptr<const VideoFrameRepresentation> process(
      std::shared_ptr<const VideoFrameRepresentation> source) const;

  // ParameterizedStage
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;
  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

  // The Mask Line Config dialog lives in orc-view_core.
  std::vector<StageToolDescriptor> get_stage_tools() const override;

  // IStageToolJobHandler: "signal" tells the dialog the input's video system
  // and levels, which it needs to number lines and offer mask levels
  // (orc/plugins/stages/common/config-tools/stage_config_wire.h).
  StageToolReply run_tool_job(const std::string& contract_id,
                              const std::string& verb,
                              const std::vector<uint8_t>& payload,
                              IToolJobContext& context) override;

 private:
  std::string line_spec_ = "";
  int32_t mask_sample_level_ = 0;

  mutable std::shared_ptr<const VideoFrameRepresentation> cached_output_;
};

}  // namespace orc
