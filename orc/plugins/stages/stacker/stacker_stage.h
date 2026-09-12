/*
 * File:        stacker_stage.h
 * Module:      orc-core
 * Purpose:     Multi-source CVBS_U10_4FSC frame stacking stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/dropout/dropout_run.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/frame_id.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/lru_cache.h>

#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace orc {

// Forward declaration
class StackerStage;

// ============================================================================
// StackedVideoFrameRepresentation
// ============================================================================
// Wraps multiple source VFrameRs and stacks corresponding frames on demand.
//
// Frame alignment: inputs arrive already frame-aligned — frame_map normalises
// each source onto the disc's frame numbering and source_align applies any
// residual offset, both driven by VBI frame numbers — so the same frame id is
// read from every source.  See collect_source_frame_ids() for why this stage
// deliberately does not re-align on colour_frame_index.
//
// Sample-grid alignment: independent decodes of the same disc need not place
// a line start on the same 4FSC sample.  sample_offsets() measures each
// source's grid offset against the first source so the samples can be brought
// onto a common grid before they are combined.
//
// Padding frames (FrameDescriptor::is_padding_frame == true) are silently
// skipped and contribute no sample data to the stack.
//
// Thread safety: all const methods are safe for concurrent access; internal
// caches are protected by cache_mutex_.
class StackedVideoFrameRepresentation : public VideoFrameRepresentationWrapper,
                                        public Artifact {
 public:
  StackedVideoFrameRepresentation(
      const std::vector<std::shared_ptr<const VideoFrameRepresentation>>&
          sources,
      StackerStage* stage);

  ~StackedVideoFrameRepresentation() override = default;

  std::string type_name() const override {
    return "stacked_video_frame_representation";
  }

  // Navigation — report the range of the primary (first) source
  FrameIDRange frame_range() const override;
  size_t frame_count() const override;
  bool has_frame(FrameID id) const override;
  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID id) const override;

  // Flat access — stacks on demand, caches result
  const sample_type* get_frame(FrameID id) const override;
  const sample_type* get_line(FrameID id, size_t line) const override;
  std::vector<sample_type> get_frame_copy(FrameID id) const override;

  // YC
  bool has_separate_channels() const override;
  const sample_type* get_frame_luma(FrameID id) const override;
  const sample_type* get_frame_chroma(FrameID id) const override;
  const sample_type* get_line_luma(FrameID id, size_t line) const override;
  const sample_type* get_line_chroma(FrameID id, size_t line) const override;

  // Hints — after stacking only residual dropouts remain
  std::vector<DropoutRun> get_dropout_hints(FrameID id) const override;

  // Audio — channel-pair count and descriptors come from the reference
  // source (the first source carrying audio). Inputs are stacked at the same
  // frame ID across sources, so per-frame pair counts always agree. Every
  // channel pair present in ALL inputs is stacked per the audio_stacking
  // parameter; pairs not common to all inputs pass through from the
  // per-frame best source.
  size_t audio_channel_pair_count() const override;
  std::optional<AudioChannelPairDescriptor> get_audio_channel_pair_descriptor(
      size_t pair) const override;
  std::vector<int32_t> get_audio_samples(size_t pair,
                                         FrameID id) const override;

  // EFM
  bool has_efm() const override;
  uint32_t get_efm_sample_count(FrameID id) const override;
  std::vector<uint8_t> get_efm_samples(FrameID id) const override;

  // Resolve which source frame ID corresponds to the reference (first) frame
  // id.  Returns UINT64_MAX when the source has no usable frame there.
  FrameID resolve_source_frame(size_t src_idx, FrameID ref_id) const;

  // Per-source sample-grid offset against sources_[0], in 4FSC samples:
  // reference sample n corresponds to source sample n + offset.  Measured
  // once on first use from a spread of frames and cached; all-zero when
  // alignment is disabled, when there is a single source, or when no reliable
  // measurement could be made.
  const std::vector<int32_t>& sample_offsets() const;

  // Count of sources that have a usable frame for ref_id
  size_t get_source_count(FrameID ref_id) const;

  friend class StackerStage;

 private:
  std::vector<std::shared_ptr<const VideoFrameRepresentation>> sources_;
  StackerStage* stage_;

  // LRU caches for stacked frames — composite and YC paths
  static constexpr size_t kMaxCachedFrames = 300;
  mutable LRUCache<FrameID, std::vector<sample_type>> stacked_frames_;
  mutable LRUCache<FrameID, std::vector<sample_type>> stacked_luma_;
  mutable LRUCache<FrameID, std::vector<sample_type>> stacked_chroma_;
  mutable LRUCache<FrameID, std::vector<DropoutRun>> stacked_dropouts_;
  mutable LRUCache<FrameID, std::vector<int32_t>> stacked_audio_;
  mutable LRUCache<FrameID, std::vector<uint8_t>> stacked_efm_;
  mutable LRUCache<FrameID, size_t> best_source_cache_;

  mutable std::mutex cache_mutex_;

  // Ensure the composite stacked frame is in cache (must hold cache_mutex_)
  void ensure_frame_stacked(FrameID id) const;

  // Ensure both YC stacked frames are in cache (must hold cache_mutex_)
  void ensure_frame_stacked_yc(FrameID id) const;

  // Find the source index with the fewest dropout samples for this frame
  size_t get_best_source_index(FrameID id) const;

  // Build the per-source frame ID vector for stacking
  std::vector<FrameID> collect_source_frame_ids(FrameID ref_id) const;

  // Measure sample_offsets_ (see sample_offsets()).
  void measure_sample_offsets() const;

  mutable std::vector<int32_t> sample_offsets_;
  mutable std::once_flag sample_offsets_once_;

  // The reference source for audio metadata: the first source carrying audio
  // (nullptr when none does).
  std::shared_ptr<const VideoFrameRepresentation> reference_audio_source()
      const;

  // True when channel pair |pair| exists in every source — the precondition
  // for combining it across inputs.
  bool pair_in_all_sources(size_t pair) const;

  // stacked_audio_ is keyed per (frame, pair); kMaxAudioChannelPairs = 8
  // leaves the low 3 bits for the pair index.
  static FrameID audio_cache_key(FrameID id, size_t pair) {
    return (id << 3) | static_cast<FrameID>(pair);
  }
};

// ============================================================================
// StackerStage
// ============================================================================
class StackerStage : public DAGStage,
                     public ParameterizedStage,
                     public IStagePreviewCapability {
 public:
  StackerStage();

  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD
  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::MERGER,
                        "stacker",
                        "Stacker",
                        "Combine multiple VFrameR sources by stacking frames "
                        "for superior output quality (1 input = passthrough)",
                        1,
                        16,
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

  std::shared_ptr<const VideoFrameRepresentation> process(
      const std::vector<std::shared_ptr<const VideoFrameRepresentation>>&
          sources) const;

  // ParameterizedStage
  std::vector<ParameterDescriptor> get_parameter_descriptors(
      VideoSystem project_format, SourceType source_type) const override;
  using ParameterizedStage::get_parameter_descriptors;
  std::map<std::string, ParameterValue> get_parameters() const override;
  bool set_parameters(
      const std::map<std::string, ParameterValue>& params) override;

  enum class AudioStackingMode { DISABLED, MEAN, MEDIAN };
  enum class EFMStackingMode { DISABLED, MEAN, MEDIAN, CONFIDENCE };

  using sample_type = VideoFrameRepresentation::sample_type;

  friend class StackedVideoFrameRepresentation;

 private:
  int32_t m_mode = -1;
  int32_t m_smart_threshold = 15;
  bool m_no_diff_dod = false;
  bool m_passthrough = false;
  bool m_sample_align = true;
  int32_t m_thread_count = 0;
  AudioStackingMode m_audio_stacking_mode = AudioStackingMode::MEAN;
  EFMStackingMode m_efm_stacking_mode = EFMStackingMode::DISABLED;

  std::map<std::string, ParameterValue> parameters_;

  mutable std::mutex execute_mutex_;
  mutable std::shared_ptr<const VideoFrameRepresentation> cached_output_;
  mutable std::vector<std::shared_ptr<const VideoFrameRepresentation>>
      cached_sources_;

  // Stack composite frame from per-source FrameIDs.  |sample_offsets| carries
  // each source's sample-grid offset (see
  // StackedVideoFrameRepresentation::sample_offsets()); pass an empty vector
  // to combine on the sources' own grids.
  void stack_frame(
      const std::vector<FrameID>& source_ids,
      const std::vector<std::shared_ptr<const VideoFrameRepresentation>>&
          sources,
      const std::vector<int32_t>& sample_offsets,
      std::vector<sample_type>& output_samples,
      std::vector<DropoutRun>& output_dropouts) const;

  // Stack YC frame from per-source FrameIDs
  void stack_frame_yc(
      const std::vector<FrameID>& source_ids,
      const std::vector<std::shared_ptr<const VideoFrameRepresentation>>&
          sources,
      const std::vector<int32_t>& sample_offsets,
      std::vector<sample_type>& output_luma,
      std::vector<sample_type>& output_chroma,
      std::vector<DropoutRun>& output_dropouts) const;

  // Apply chosen stacking mode to a set of sample values
  int16_t stack_mode(const std::vector<int16_t>& values,
                     bool all_dropout) const;

  int16_t compute_median(std::vector<int16_t> values) const;
  int32_t compute_mean(const std::vector<int16_t>& values) const;

  // Differential dropout detection: recover samples that are all marked as
  // dropout but are similar enough to be real signal.
  std::vector<int16_t> diff_dod(const std::vector<int16_t>& input,
                                int32_t black_level) const;

  // Line processing (parallel-friendly)
  void process_lines_range(
      size_t start_line, size_t end_line, size_t width, VideoSystem system,
      const std::vector<std::vector<sample_type>>& all_frames,
      const std::vector<bool>& frame_valid,
      const std::vector<std::vector<DropoutRun>>& all_dropouts,
      size_t num_sources, int32_t black_level, int32_t nominal_width,
      std::vector<sample_type>& output_samples,
      std::vector<DropoutRun>& output_dropouts, size_t& total_dropouts,
      size_t& total_stacked) const;

  void process_lines_range_yc(
      size_t start_line, size_t end_line, size_t width, VideoSystem system,
      const std::vector<std::vector<sample_type>>& all_luma,
      const std::vector<std::vector<sample_type>>& all_chroma,
      const std::vector<bool>& frame_valid,
      const std::vector<std::vector<DropoutRun>>& all_dropouts,
      size_t num_sources, int32_t black_level, int32_t nominal_width,
      std::vector<sample_type>& output_luma,
      std::vector<sample_type>& output_chroma,
      std::vector<DropoutRun>& output_dropouts, size_t& total_dropouts,
      size_t& total_stacked) const;

  // Audio and EFM stacking helpers. Audio samples are 24-bit values carried
  // in int32_t (see audio_channel_pair.h); mean/median results are saturated
  // to the 24-bit range.
  std::vector<int32_t> stack_audio(
      size_t pair, const std::vector<FrameID>& source_ids,
      const std::vector<std::shared_ptr<const VideoFrameRepresentation>>&
          sources,
      size_t best_src) const;

  std::vector<uint8_t> stack_efm(
      const std::vector<FrameID>& source_ids,
      const std::vector<std::shared_ptr<const VideoFrameRepresentation>>&
          sources,
      size_t best_src) const;

  int32_t audio_mean(const std::vector<int32_t>& v) const;
  int32_t audio_median(std::vector<int32_t> v) const;
  uint8_t efm_mean(const std::vector<uint8_t>& v) const;
  uint8_t efm_median(std::vector<uint8_t> v) const;
};

}  // namespace orc
