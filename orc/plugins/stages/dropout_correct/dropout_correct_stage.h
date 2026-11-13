/*
 * File:        dropout_correct_stage.h
 * Module:      orc-core
 * Purpose:     Dropout correction stage (VFrameR)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <orc/plugin/orc_stage_preview.h>
#include <orc/plugin/orc_stage_runtime.h>
#include <orc/stage/artifact.h>
#include <orc/stage/dropout/dropout_decision.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/params/stage_parameter.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_cache.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace orc {

// How a dropout in the picture is filled: copied from one nearby line, or
// interpolated from the nearest clean lines above and below once luma and
// chroma are separated.
enum class DropoutCorrectionMethod { LineCopy, Interpolate };

inline constexpr const char kMethodLineCopy[] = "line_copy";
inline constexpr const char kMethodInterpolate[] = "interpolate";

std::optional<DropoutCorrectionMethod> parse_dropout_correction_method(
    const std::string& value);

struct DropoutCorrectionConfig {
  uint32_t overcorrect_extension = 0;
  bool match_chroma_phase = true;
  bool highlight_corrections = false;
  DropoutCorrectionMethod method = DropoutCorrectionMethod::Interpolate;
};

// Forward declaration
class DropoutCorrectStage;

// ============================================================================
// Internal per-line dropout descriptor (frame-flat addressing)
// ============================================================================
struct LineDropout {
  uint32_t line;
  uint32_t start_sample;
  uint32_t end_sample;
};

// A corrected frame: the composite frame, or a Y/C source's luma, and a Y/C
// source's chroma. Empty planes are a frame with no dropout hints, read from
// the source as it is.
struct CorrectedFrame {
  std::vector<int16_t> samples;
  std::vector<int16_t> chroma;
};

// ============================================================================
// CorrectedVideoFrameRepresentation
// ============================================================================
class CorrectedVideoFrameRepresentation
    : public VideoFrameRepresentationWrapper,
      public Artifact {
 public:
  CorrectedVideoFrameRepresentation(
      std::shared_ptr<const VideoFrameRepresentation> source,
      DropoutCorrectStage* stage, bool highlight_corrections);

  std::string type_name() const override {
    return "corrected_video_frame_representation";
  }

  const int16_t* get_frame(FrameID id) const override;
  const int16_t* get_line(FrameID id, size_t line) const override;
  std::vector<int16_t> get_frame_copy(FrameID id) const override;

  const int16_t* get_frame_luma(FrameID id) const override;
  const int16_t* get_line_luma(FrameID id, size_t line) const override;
  const int16_t* get_frame_chroma(FrameID id) const override;
  const int16_t* get_line_chroma(FrameID id, size_t line) const override;

  // After correction there are no dropouts in the output.
  std::vector<DropoutRun> get_dropout_hints(FrameID /*id*/) const override {
    return {};
  }

  // A frame with no source dropout hints is passed through byte-identical
  // (correct_single_frame returns empty planes and every accessor falls back
  // to the source buffer), so the host may share its content-derived analysis
  // with the upstream node. Answered from dropout metadata alone — no sample
  // decode.
  std::shared_ptr<const VideoFrameRepresentation> video_passthrough_source(
      FrameID id) const override {
    if (!source_ || !source_->get_dropout_hints(id).empty()) {
      return nullptr;
    }
    return source_;
  }

  // The readers this stage's frame cache is sized for, at most.
  size_t max_concurrent_frame_requests() const override {
    return std::min(source_->max_concurrent_frame_requests(),
                    nominal_frame_cache_capacity());
  }

  // Thread safety: every method may be called from many threads at once. A
  // pointer from a frame or line accessor stays valid until the calling
  // thread's next call on this object (luma and chroma pointers are kept
  // apart, so both of one frame may be held).

  friend class DropoutCorrectStage;

 private:
  DropoutCorrectStage* stage_;
  bool highlight_corrections_;

  using Frame = std::shared_ptr<const CorrectedFrame>;
  static constexpr unsigned kSamplesSlot = 0;
  static constexpr unsigned kChromaSlot = 1;

  // Corrected frames, each corrected once however many threads want it.
  mutable FrameCache<CorrectedFrame> frames_;

  Frame corrected(FrameID frame_id) const;
  // |plane| of |frame| from |line| (or the whole frame when |line| is
  // npos), pinned in |slot|; nullptr when the line is out of range.
  const int16_t* pinned(Frame frame, const std::vector<int16_t>& plane,
                        FrameID id, size_t line, unsigned slot) const;
};

// ============================================================================
// DropoutCorrectStage
// ============================================================================
class DropoutCorrectStage : public DAGStage,
                            public ParameterizedStage,
                            public IStagePreviewCapability {
 public:
  explicit DropoutCorrectStage(
      const DropoutCorrectionConfig& config = DropoutCorrectionConfig{})
      : config_(config) {}

  std::string version() const override { return "1.0"; }
  ORC_STAGE_INSTRUCTIONS_MD

  NodeTypeInfo get_node_type_info() const override {
    return NodeTypeInfo{NodeType::TRANSFORM,
                        "dropout_correct",
                        "Dropout Correction",
                        "Correct dropouts by replacing corrupted samples with "
                        "data from other lines",
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

  // Public for lazy correction from CorrectedVideoFrameRepresentation
  CorrectedFrame correct_single_frame(
      const CorrectedVideoFrameRepresentation& corrected,
      std::shared_ptr<const VideoFrameRepresentation> source,
      FrameID frame_id) const;

  // Public for unit testing
  static std::vector<LineDropout> runs_to_line_dropouts(
      const std::vector<DropoutRun>& runs, VideoSystem system,
      size_t nominal_spl);

 private:
  DropoutCorrectionConfig config_;

  enum class DropoutLocation { COLOUR_BURST, VISIBLE_LINE, UNKNOWN };
  enum class Channel { COMPOSITE, LUMA, CHROMA };

  DropoutLocation classify_dropout(
      const LineDropout& dropout, const FrameDescriptor& desc,
      const std::optional<SourceParameters>& params) const;

  std::vector<LineDropout> split_dropout_regions(
      const std::vector<LineDropout>& dropouts, const FrameDescriptor& desc,
      const std::optional<SourceParameters>& params) const;

  struct ReplacementLine {
    bool found = false;
    FrameID source_frame = 0;
    uint32_t source_line = 0;
    double quality = 0.0;
    uint32_t distance = 0;
    const int16_t* cached_data = nullptr;
  };

  ReplacementLine find_replacement_line(
      const VideoFrameRepresentation& source, FrameID frame_id, uint32_t line,
      const LineDropout& dropout, bool intrafield,
      bool match_chroma_phase_override, size_t field1_lines,
      const std::vector<LineDropout>& frame_dropouts,
      Channel channel = Channel::COMPOSITE) const;

  double calculate_line_quality(const int16_t* line_data, size_t width,
                                const LineDropout& dropout) const;

  // The interpolate method: fills |dropout| in |out_line| (its line of the
  // channel's corrected buffer) from the nearest clean lines of its field
  // above and below. False, with |out_line| untouched, where neither side
  // has a clean line within reach or the line's subcarrier phase is not
  // known; the caller then copies a line instead.
  bool interpolate_dropout(const VideoFrameRepresentation& source,
                           FrameID frame_id, const FrameDescriptor& desc,
                           const LineDropout& dropout,
                           const std::vector<LineDropout>& frame_dropouts,
                           size_t field1_lines, Channel channel,
                           std::optional<int16_t> highlight,
                           int16_t* out_line) const;

  mutable std::shared_ptr<const VideoFrameRepresentation> cached_output_;
};

}  // namespace orc
