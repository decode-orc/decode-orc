/*
 * File:        dropout_map_stage.h
 * Module:      orc-core
 * Purpose:     Dropout map stage — override dropout hints per frame (VFrameR)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/plugin/orc_stage_tooling.h>
#include <orc/stage/artifact.h>
#include <orc/stage/dropout/dropout_run.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/dropout_util.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "../common/dropout-map/dropout_map_text.h"

namespace orc {

// Per-frame dropout additions and removals, as the "dropout_map" parameter
// stores them (frame-flat 0-based line, sample within the line), so that
// apply-time conversion to DropoutRun is straightforward. The text format is
// shared with the Dropout Editor in orc-view_core.
using DropoutEntrySpec = dropout_map_text::Span;
using FrameDropoutMapEntry = dropout_map_text::FrameEdits;

// ============================================================================
// DropoutMappedFrameRepresentation
// ============================================================================
class DropoutMappedFrameRepresentation : public VideoFrameRepresentationWrapper,
                                         public Artifact {
 public:
  DropoutMappedFrameRepresentation(
      std::shared_ptr<const VideoFrameRepresentation> source,
      const std::map<uint64_t, FrameDropoutMapEntry>& dropout_map);

  std::string type_name() const override {
    return "dropout_mapped_frame_representation";
  }

  std::vector<DropoutRun> get_dropout_hints(FrameID id) const override;

  // This stage modifies only dropout hints, never sample data, so line reads
  // may forward to the wrapped input to preserve its seek-one-line-from-disk
  // fast path (important for analysis sinks scanning whole recordings).
  std::vector<sample_type> get_line_samples(FrameID id,
                                            size_t line) const override {
    return source_ ? source_->get_line_samples(id, line)
                   : std::vector<sample_type>{};
  }
  const sample_type* get_line(FrameID id, size_t line) const override {
    return source_ ? source_->get_line(id, line) : nullptr;
  }
  const sample_type* get_line_luma(FrameID id, size_t line) const override {
    return source_ ? source_->get_line_luma(id, line) : nullptr;
  }
  const sample_type* get_line_chroma(FrameID id, size_t line) const override {
    return source_ ? source_->get_line_chroma(id, line) : nullptr;
  }

 private:
  std::map<uint64_t, FrameDropoutMapEntry> dropout_map_;

  // Convert (frame_id, line, sample_start, sample_end) to a DropoutRun.
  static std::optional<DropoutRun> entry_to_run(VideoSystem sys,
                                                int32_t nominal_spl,
                                                FrameID frame_id,
                                                const DropoutEntrySpec& entry);
};

// ============================================================================
// DropoutMapStage
// ============================================================================
class DropoutMapStage : public DAGStage,
                        public ParameterizedStage,
                        public IStagePreviewCapability,
                        public StageToolProvider {
 public:
  DropoutMapStage();

  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{
        NodeType::TRANSFORM,
        "dropout_map",
        "Dropout Map",
        "Override dropout hints on per-frame basis — add, remove, or modify "
        "dropout regions",
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

  // IStagePreviewCapability
  StagePreviewCapability get_preview_capability() const override;

  // ParameterizedStage
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;
  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  // The "dropout_map" parameter text (dropout_map_text.h).
  static std::map<uint64_t, FrameDropoutMapEntry> parse_dropout_map(
      const std::string& map_str);
  static std::string encode_dropout_map(
      const std::map<uint64_t, FrameDropoutMapEntry>& map);

  // The Dropout Editor, a window in orc-view_core. It needs nothing from the
  // stage at run time: it reads and writes the parameter through its session
  // and draws frames of the input the host renders for it.
  std::vector<StageToolDescriptor> get_stage_tools() const override;

 private:
  std::string dropout_map_str_ = "[]";
  mutable std::shared_ptr<const VideoFrameRepresentation> cached_output_;
};

}  // namespace orc
