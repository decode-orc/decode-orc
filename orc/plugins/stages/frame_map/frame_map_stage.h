/*
 * File:        frame_map_stage.h
 * Module:      orc-stage-plugin-frame-map
 * Purpose:     Frame mapping/reordering and sequence-correction stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/video_frame_representation.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace orc {

// ============================================================================
// FrameMappedRepresentation
// ============================================================================
// VideoFrameRepresentationWrapper that presents frames in a user-specified
// order without copying sample data.  Each output FrameID maps to a source
// FrameID through a lookup table built at stage execute() time.
//
// Padding frames (is_padding_frame == true) are synthesised with a descriptor
// derived from the source's video parameters; their sample data comes from a
// single black frame whose layout matches the source geometry.
class FrameMappedRepresentation : public VideoFrameRepresentationWrapper,
                                  public Artifact {
 public:
  struct PaddingDescriptor {
    FrameID output_id;
    VideoSystem system;
    size_t height;
    size_t samples_total;
    size_t samples_per_line_nominal;
  };

  FrameMappedRepresentation(
      std::shared_ptr<const VideoFrameRepresentation> source,
      std::vector<FrameID> frame_mapping,
      std::vector<PaddingDescriptor> padding_descriptors,
      const std::string& tag);

  ~FrameMappedRepresentation() override = default;

  std::string type_name() const override {
    return "frame_mapped_representation";
  }

  // Navigation
  FrameIDRange frame_range() const override;
  size_t frame_count() const override { return frame_mapping_.size(); }
  bool has_frame(FrameID id) const override;
  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override;

  // Flat access — delegate to source via mapped ID; return black for padding
  const sample_type* get_frame(FrameID id) const override;
  const sample_type* get_line(FrameID id, size_t line) const override;
  std::vector<sample_type> get_frame_copy(FrameID id) const override;

  // YC
  bool has_separate_channels() const override {
    return source_ ? source_->has_separate_channels() : false;
  }
  const sample_type* get_frame_luma(FrameID id) const override;
  const sample_type* get_frame_chroma(FrameID id) const override;
  const sample_type* get_line_luma(FrameID id, size_t line) const override;
  const sample_type* get_line_chroma(FrameID id, size_t line) const override;

  // Hints
  std::vector<DropoutRun> get_dropout_hints(FrameID id) const override;
  std::optional<SourceParameters> get_video_parameters() const override {
    return source_ ? source_->get_video_parameters() : std::nullopt;
  }

  // Audio — channel pairs remap in lockstep with the frame mapping; pair
  // count and descriptors forward from the source via the wrapper base.
  // Every output frame p serves exactly audio_pairs_in_frame(p) stereo
  // pairs: padding frames carry cadence-sized silence, and a mapping that
  // breaks the NTSC/PAL-M five-frame audio sequence phase
  // (SMPTE 272M-1994 §14.3) truncates one trailing pair or appends one
  // trailing silence pair. Phase-preserving mappings and all PAL mappings
  // are sample-exact.
  std::vector<int32_t> get_audio_samples(size_t pair,
                                         FrameID id) const override;

  // EFM / AC3
  bool has_efm() const override { return source_ ? source_->has_efm() : false; }
  uint32_t get_efm_sample_count(FrameID id) const override;
  std::vector<uint8_t> get_efm_samples(FrameID id) const override;
  bool has_ac3_rf() const override {
    return source_ ? source_->has_ac3_rf() : false;
  }
  uint32_t get_ac3_symbol_count(FrameID id) const override;
  std::vector<uint8_t> get_ac3_symbols(FrameID id) const override;

 private:
  // Maps output index (0-based) → source FrameID.
  // An invalid FrameID (UINT64_MAX) indicates a synthetic padding frame.
  std::vector<FrameID> frame_mapping_;

  // Descriptors for padding frames (keyed by output FrameID value)
  std::vector<PaddingDescriptor> padding_descriptors_;

  // Resolve output FrameID → entry in frame_mapping_
  std::optional<size_t> resolve_index(FrameID id) const;
  bool is_padding(size_t index) const;
  const PaddingDescriptor* find_padding(FrameID output_id) const;

  // Black sample buffer for padding frames
  mutable std::vector<sample_type> black_frame_;
  void ensure_black_frame() const;
};

// ============================================================================
// FrameMapStage
// ============================================================================
// Pipeline orchestration stage that reorders frames using a range string and
// optionally removes duplicate colour-frame-index frames and fills sequence
// gaps with synthetic padding frames.
//
// Parameters:
//   ranges             — comma-separated FrameID ranges, e.g. "0-10,20-30"
//                        (empty = pass-through)
//   remove_duplicates  — when true, consecutive frames with identical
//                        colour_frame_index are deduplicated
//   pad_gaps           — when true, detected sequence gaps are filled with
//                        synthetic padding frames
//   pad_strategy       — "black" (insert blank padding frames). The legacy
//                        "nearest" value is accepted and treated as "black".
//
// Observations emitted:
//   frame_map.frames_removed  — count of frames removed by deduplication
//   frame_map.frames_padded   — count of padding frames inserted
//   frame_map.gap_positions   — string listing output positions of gap starts
class FrameMapStage : public DAGStage,
                      public ParameterizedStage,
                      public IStagePreviewCapability {
 public:
  FrameMapStage();

  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::TRANSFORM,
                        "frame_map",
                        "Frame Map",
                        "Reorder frames by range specification; optionally "
                        "deduplicate and fill sequence gaps",
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

 private:
  // Everything one execute() needs, resolved from the parameter map it was
  // handed over the configuration the stage holds.
  //
  // Held on the stack rather than read back off the members: a DAG's stages
  // are shared, and re-applying the parameter map to the object on every
  // execute() let one thread reparse the ranges while another was walking
  // them — a use-after-free whose garbage bounds turned the mapping loop into
  // an unbounded one.
  struct RunConfig {
    std::string range_spec;
    std::vector<std::pair<uint64_t, uint64_t>> ranges;
    bool remove_duplicates = false;
    bool pad_gaps = false;
  };

  // Resolve @p parameters against the stage's configured values. Returns
  // nullopt for a malformed range spec (already logged).
  std::optional<RunConfig> config_for(
      const std::map<std::string, ParameterValue>& parameters) const;

  // Parse "0-10,20-30,11-19" → vector of [start, end] inclusive pairs.
  // PAD_N tokens produce a pair with first == UINT64_MAX, second == N.
  static std::vector<std::pair<uint64_t, uint64_t>> parse_ranges(
      const std::string& spec);

  // Build the output frame_mapping vector from a parsed range list.
  static std::vector<FrameID> build_frame_mapping(
      const std::vector<std::pair<uint64_t, uint64_t>>& ranges,
      const VideoFrameRepresentation& source);

  // Apply duplicate-frame removal: when two consecutive frames share the same
  // colour_frame_index, drop the second.  Returns removed-count.
  static size_t apply_remove_duplicates(std::vector<FrameID>& mapping,
                                        const VideoFrameRepresentation& source);

  // Detect gaps in the colour_frame_index sequence and insert padding frames.
  // Returns number of padding frames inserted; fills padding_descriptors_out.
  // Padding frames always render black.
  static size_t apply_pad_gaps(
      std::vector<FrameID>& mapping,
      std::vector<FrameMappedRepresentation::PaddingDescriptor>& pads,
      const VideoFrameRepresentation& source, std::string& gap_positions_out);

  // Expected next colour_frame_index after 'current' for the given system.
  static int next_colour_index(int current, VideoSystem sys);

  // Current parameters
  std::string range_spec_;
  bool remove_duplicates_ = false;
  bool pad_gaps_ = false;
  std::string pad_strategy_ = "black";

  // Cached parsed ranges
  std::vector<std::pair<uint64_t, uint64_t>> cached_ranges_;

  // Cached output for preview rendering
  mutable std::shared_ptr<const VideoFrameRepresentation> cached_output_;

  // The last mapped run, handed back by execute() while its input and
  // configuration (the representation's tag) are unchanged, with the counts it
  // reported.
  struct LastRun {
    std::shared_ptr<const VideoFrameRepresentation> input;
    std::string tag;
    std::shared_ptr<FrameMappedRepresentation> output;
    size_t removed_count = 0;
    size_t padded_count = 0;
    std::string gap_positions;
  };
  std::mutex last_run_mutex_;
  std::optional<LastRun> last_run_;

  static void emit_counts(ObservationContext& observation_context,
                          size_t removed_count, size_t padded_count,
                          const std::string& gap_positions);
};

}  // namespace orc
