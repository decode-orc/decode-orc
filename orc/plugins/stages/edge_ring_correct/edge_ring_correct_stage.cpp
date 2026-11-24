/*
 * File:        edge_ring_correct_stage.cpp
 * Module:      orc-stage-plugin-edge-ring-correct
 * Purpose:     Removes the ring after sharp picture edges and the halo after
 *              thin dark lines of PAL LaserDisc pictures
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "edge_ring_correct_stage.h"

#include <orc/stage/error_types.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include "edge_ring.h"
#include "edge_ring_calibration_tool.h"

namespace orc {

// ============================================================================
// RingCorrectedVideoFrameRepresentation
// ============================================================================

RingCorrectedVideoFrameRepresentation::RingCorrectedVideoFrameRepresentation(
    std::shared_ptr<const VideoFrameRepresentation> source,
    edge_ring::RingTrack track)
    : VideoFrameRepresentationWrapper(std::move(source)),
      Artifact(ArtifactID("ring_corrected_frame"), Provenance{}),
      track_(std::move(track)) {}

RingCorrectedVideoFrameRepresentation::Frame
RingCorrectedVideoFrameRepresentation::corrected(FrameID id) const {
  return frames_.get(id, [&] { return build_corrected(id); });
}

RingCorrectedVideoFrameRepresentation::Frame
RingCorrectedVideoFrameRepresentation::build_corrected(FrameID id) const {
  std::vector<int16_t> frame;
  const auto* window = track_.at(static_cast<int64_t>(id));
  const auto desc = source_->get_frame_descriptor(id);
  const auto params = source_->get_video_parameters();
  if (window == nullptr || !desc || !params ||
      desc->system != VideoSystem::PAL || source_->has_separate_channels() ||
      params->white_level <= params->black_level) {
    return std::make_shared<const std::vector<int16_t>>(std::move(frame));
  }
  const int16_t* input = source_->get_frame(id);
  if (input == nullptr) {
    return std::make_shared<const std::vector<int16_t>>(std::move(frame));
  }
  frame.assign(input, input + desc->samples_total);
  if (!edge_ring::correct_frame(
          input, frame.data(), desc->samples_per_line_nominal, desc->height,
          params->black_level, params->white_level, *window)) {
    frame.clear();
  }
  return std::make_shared<const std::vector<int16_t>>(std::move(frame));
}

const int16_t* RingCorrectedVideoFrameRepresentation::get_frame(
    FrameID id) const {
  auto frame = corrected(id);
  if (!frame || frame->empty()) return source_->get_frame(id);
  const int16_t* data = frame->data();
  return frames_.pin(std::move(frame), data);
}

const int16_t* RingCorrectedVideoFrameRepresentation::get_line(
    FrameID id, size_t line) const {
  auto frame = corrected(id);
  if (!frame || frame->empty()) return source_->get_line(id, line);
  const auto desc = source_->get_frame_descriptor(id);
  if (!desc || line >= desc->height) return nullptr;
  const int16_t* data =
      frame->data() + frame_line_sample_offset(
                          desc->system, desc->samples_per_line_nominal, line);
  return frames_.pin(std::move(frame), data);
}

std::vector<int16_t> RingCorrectedVideoFrameRepresentation::get_frame_copy(
    FrameID id) const {
  const auto frame = corrected(id);
  if (!frame || frame->empty()) return source_->get_frame_copy(id);
  return *frame;
}

// ============================================================================
// EdgeRingCorrectStage
// ============================================================================

std::vector<ArtifactPtr> EdgeRingCorrectStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& /*observation_context*/) {
  if (inputs.empty()) {
    throw DAGExecutionError("EdgeRingCorrectStage requires one input");
  }
  auto source =
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  if (!source) {
    throw DAGExecutionError(
        "EdgeRingCorrectStage input must be VideoFrameRepresentation");
  }
  if (!parameters.empty() && !set_parameters(parameters)) {
    throw DAGExecutionError("EdgeRingCorrectStage: invalid parameters");
  }
  const auto params = source->get_video_parameters();
  if (!params || params->system != VideoSystem::PAL ||
      source->has_separate_channels()) {
    ORC_LOG_WARN(
        "EdgeRingCorrect: the correction is for PAL composite sources; this "
        "source passes through unchanged");
  }
  ORC_LOG_DEBUG("EdgeRingCorrect: {} corrected windows",
                track_.windows().size());
  auto output =
      std::make_shared<RingCorrectedVideoFrameRepresentation>(source, track_);
  cached_output_ = output;
  return {output};
}

std::vector<ParameterDescriptor>
EdgeRingCorrectStage::get_parameter_descriptors(
    VideoSystem /*project_format*/, SourceType /*source_type*/) const {
  return {
      ParameterDescriptor{
          "ring_track", "Ring track",
          "The certified corrections along the source, one per window of "
          "frames, separated by \";\": "
          "\"first-end:f|w:rise_gain,fall_gain,line_gain:rise:fall:line\", "
          "the last three the templates (comma lists, 1e-5 of the step). "
          "Set it with the Edge Ring Calibration tool, which ties the windows "
          "to the frame numbers of this input and keeps only windows whose "
          "correction it certified. A frame in no window passes through "
          "unchanged. Empty (the default): every frame passes through.",
          ParameterType::STRING,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               ParameterValue(std::string()),
                               {},
                               false,
                               std::nullopt}},
  };
}

std::map<std::string, ParameterValue> EdgeRingCorrectStage::get_parameters()
    const {
  return {{"ring_track", ParameterValue{track_.to_string()}}};
}

bool EdgeRingCorrectStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  auto track = track_;
  for (const auto& [key, value] : params) {
    if (key != "ring_track") return false;
    const auto* text = std::get_if<std::string>(&value);
    if (!text) return false;
    const auto parsed = edge_ring::RingTrack::parse(*text);
    if (!parsed) return false;
    track = *parsed;
  }
  track_ = std::move(track);
  return true;
}

StagePreviewCapability EdgeRingCorrectStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_output_);
}

std::vector<StageToolDescriptor> EdgeRingCorrectStage::get_stage_tools() const {
  StageToolDescriptor tool;
  tool.tool_id = edge_ring_calibration_tool::kToolId;
  tool.display_name = "Edge Ring Calibration";
  tool.description =
      "Fits the edge-ring templates window by window on the source's own "
      "sharp edges and thin lines, and keeps each window's correction only "
      "where it is certified better on frames the fit did not see";
  tool.kind = StageToolKind::Analysis;
  tool.contract_id = edge_ring_calibration_tool::kContractId;
  return {tool};
}

std::vector<ParameterDescriptor> EdgeRingCorrectStage::analysis_tool_parameters(
    const std::string& /*tool_id*/) const {
  return {};
}

StageAnalysisReport EdgeRingCorrectStage::run_analysis_tool(
    const std::string& tool_id,
    const std::map<std::string, ParameterValue>& /*parameters*/,
    IToolJobContext& context) {
  if (tool_id != edge_ring_calibration_tool::kToolId) {
    StageAnalysisReport report;
    report.summary = "Edge Ring Correct has no tool '" + tool_id + "'";
    return report;
  }
  return edge_ring_calibration_tool::run(context);
}

}  // namespace orc
