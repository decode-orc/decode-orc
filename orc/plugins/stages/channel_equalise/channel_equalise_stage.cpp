/*
 * File:        channel_equalise_stage.cpp
 * Module:      orc-stage-plugin-channel-equalise
 * Purpose:     Removes the PAL LaserVision CAV edge smear from each frame and
 *              restores the chroma level (composite stays composite)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "channel_equalise_stage.h"

#include <orc/stage/error_types.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include "channel_calibration_tool.h"

namespace orc {

namespace {

// The Domesday CAV sides' tail time constant: the default, so a fraction
// typed in by hand has a plausible tail to go with it.
constexpr double kDefaultTauSamples = 6.1;

std::optional<double> as_double(const ParameterValue& value) {
  if (const auto* d = std::get_if<double>(&value)) return *d;
  if (const auto* i = std::get_if<int32_t>(&value)) {
    return static_cast<double>(*i);
  }
  if (const auto* u = std::get_if<uint32_t>(&value)) {
    return static_cast<double>(*u);
  }
  return std::nullopt;
}

}  // namespace

// ============================================================================
// EqualisedVideoFrameRepresentation
// ============================================================================

EqualisedVideoFrameRepresentation::EqualisedVideoFrameRepresentation(
    std::shared_ptr<const VideoFrameRepresentation> source,
    channel_equalise::EdgeSmear smear, SmearTrack track,
    SmearTrack track_field2)
    : VideoFrameRepresentationWrapper(std::move(source)),
      Artifact(ArtifactID("equalised_frame"), Provenance{}),
      smear_(smear),
      track_(std::move(track)),
      track_field2_(std::move(track_field2)) {}

channel_equalise::EdgeSmear EqualisedVideoFrameRepresentation::smear_for(
    FrameID id, int field) const {
  if (track_.empty() && track_field2_.empty()) return smear_;
  const auto& track =
      (field == 1 && !track_field2_.empty()) ? track_field2_ : track_;
  return track.at(static_cast<int64_t>(id));
}

EqualisedVideoFrameRepresentation::Frame
EqualisedVideoFrameRepresentation::equalised(FrameID id) const {
  return frames_.get(id, [&] { return build_equalised(id); });
}

EqualisedVideoFrameRepresentation::Frame
EqualisedVideoFrameRepresentation::build_equalised(FrameID id) const {
  const auto desc = source_->get_frame_descriptor(id);
  const auto params = source_->get_video_parameters();
  auto frame = std::make_shared<std::vector<int16_t>>();
  const auto smear = smear_for(id, 0);
  const auto smear_field2 = smear_for(id, 1);
  if ((smear.active() || smear_field2.active()) && desc && params &&
      desc->system == VideoSystem::PAL && !source_->has_separate_channels()) {
    // A copy: a source's frame pointer is not held while the frame is
    // filtered.
    const std::vector<int16_t> copy = source_->get_frame_copy(id);
    if (copy.size() >= desc->samples_total) {
      auto result = channel_equalise::equalise_frame(
          copy.data(), desc->samples_total, desc->samples_per_line_nominal,
          params->blanking_level, params->white_level, smear, smear_field2);
      ORC_LOG_TRACE(
          "ChannelEqualise: frame {} smear {:.4f} / {:.3f}, chroma gain {:.3f}",
          id, smear.fraction, smear.tau_samples, result.gain);
      *frame = std::move(result.frame);
    }
  }
  return frame;
}

const int16_t* EqualisedVideoFrameRepresentation::get_frame(FrameID id) const {
  auto frame = equalised(id);
  if (frame->empty()) return source_->get_frame(id);
  const int16_t* data = frame->data();
  return frames_.pin(std::move(frame), data);
}

const int16_t* EqualisedVideoFrameRepresentation::get_line(FrameID id,
                                                           size_t line) const {
  auto frame = equalised(id);
  if (frame->empty()) return source_->get_line(id, line);
  const auto desc = source_->get_frame_descriptor(id);
  if (!desc || line >= desc->height) return nullptr;
  const int16_t* data =
      frame->data() + frame_line_sample_offset(
                          desc->system, desc->samples_per_line_nominal, line);
  return frames_.pin(std::move(frame), data);
}

std::vector<int16_t> EqualisedVideoFrameRepresentation::get_frame_copy(
    FrameID id) const {
  const auto frame = equalised(id);
  if (frame->empty()) return source_->get_frame_copy(id);
  return *frame;
}

// ============================================================================
// ChannelEqualiseStage
// ============================================================================

ChannelEqualiseStage::ChannelEqualiseStage() {
  set_configuration_status(ConfigurationStatus::Yellow);
}

std::vector<ArtifactPtr> ChannelEqualiseStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& /*observation_context*/) {
  if (inputs.empty()) {
    throw DAGExecutionError("ChannelEqualiseStage requires one input");
  }
  auto source =
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  if (!source) {
    throw DAGExecutionError(
        "ChannelEqualiseStage input must be VideoFrameRepresentation");
  }
  if (!parameters.empty() && !set_parameters(parameters)) {
    throw DAGExecutionError("ChannelEqualiseStage: invalid parameters");
  }
  const auto params = source->get_video_parameters();
  if (!params || params->system != VideoSystem::PAL ||
      source->has_separate_channels()) {
    ORC_LOG_WARN(
        "ChannelEqualise: the edge smear is a PAL LaserDisc composite effect; "
        "this source passes through unchanged");
  }
  if (!track_.empty() || !track_field2_.empty()) {
    ORC_LOG_DEBUG(
        "ChannelEqualise: smear tracked along the source, {} + {} knots",
        track_.knots().size(), track_field2_.knots().size());
  } else if (smear_.active()) {
    ORC_LOG_DEBUG("ChannelEqualise: smear fraction {:.3f}, tau {:.2f} samples",
                  smear_.fraction, smear_.tau_samples);
  } else {
    ORC_LOG_DEBUG(
        "ChannelEqualise: smear fraction 0; frames pass through unchanged");
  }
  auto output = std::make_shared<EqualisedVideoFrameRepresentation>(
      source, smear_, track_, track_field2_);
  cached_output_ = output;
  return {output};
}

std::vector<ParameterDescriptor>
ChannelEqualiseStage::get_parameter_descriptors(
    VideoSystem /*project_format*/, SourceType /*source_type*/) const {
  return {
      ParameterDescriptor{
          "smear_fraction", "Smear fraction",
          "The share of each step that the disc's edge smear moves into its "
          "tail. Set it with the Channel Calibration tool, which measures it "
          "on the VITS. 0 (the default) passes every frame through unchanged.",
          ParameterType::DOUBLE,
          ParameterConstraints{ParameterValue(0.0),
                               ParameterValue(channel_equalise::kMaxFraction),
                               ParameterValue(0.0),
                               {},
                               false,
                               std::nullopt}},
      ParameterDescriptor{
          "smear_tau_samples", "Smear time constant (samples)",
          "The time constant of the smear's tail, in 4fsc samples. Set it "
          "with the Channel Calibration tool. Default 6.1, the Domesday CAV "
          "sides'.",
          ParameterType::DOUBLE,
          ParameterConstraints{ParameterValue(channel_equalise::kMinTauSamples),
                               ParameterValue(channel_equalise::kMaxTauSamples),
                               ParameterValue(kDefaultTauSamples),
                               {},
                               false,
                               std::nullopt}},
      ParameterDescriptor{
          "smear_track", "Smear track",
          "The smear measured along the source, frame by frame, as knots "
          "separated by \";\". A knot is \"frame:fraction:tau\" (the slow "
          "component alone, as the two values above) or "
          "\"frame:fraction:tau:fast_fraction:fast_tau\" with the fast "
          "component as well: a signed fraction (-0.45 to 0.3) with a time "
          "constant of 0.5 to 4.5 samples, for the tail within a few samples "
          "of an edge. A frame between two knots takes the straight line "
          "between them. Set it with the Channel Tracking tool, which ties "
          "the knots to the frame numbers of this input. When set, each "
          "frame is corrected with its own smear and the two values above "
          "are not used; a frame where the track has no smear passes through "
          "unchanged. Empty (the default): every frame takes the two values "
          "above.",
          ParameterType::STRING,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               ParameterValue(std::string()),
                               {},
                               false,
                               std::nullopt}},
      ParameterDescriptor{
          "smear_track_field2", "Smear track (second field)",
          "The smear track for the second field of each frame, in the same "
          "form as the smear track. Set it with the Channel Tracking tool, "
          "which measures each field parity on its own VITS. Empty (the "
          "default): the second field takes the smear track above.",
          ParameterType::STRING,
          ParameterConstraints{std::nullopt,
                               std::nullopt,
                               ParameterValue(std::string()),
                               {},
                               false,
                               std::nullopt}},
  };
}

std::map<std::string, ParameterValue> ChannelEqualiseStage::get_parameters()
    const {
  return {{"smear_fraction", ParameterValue{smear_.fraction}},
          {"smear_tau_samples", ParameterValue{smear_.tau_samples}},
          {"smear_track", ParameterValue{track_.to_string()}},
          {"smear_track_field2", ParameterValue{track_field2_.to_string()}}};
}

bool ChannelEqualiseStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  auto smear = smear_;
  auto track = track_;
  auto track_field2 = track_field2_;
  for (const auto& [key, value] : params) {
    if (key == "smear_track" || key == "smear_track_field2") {
      const auto* text = std::get_if<std::string>(&value);
      if (!text) return false;
      const auto parsed = SmearTrack::parse(*text);
      if (!parsed) return false;
      (key == "smear_track" ? track : track_field2) = *parsed;
      continue;
    }
    const auto number = as_double(value);
    if (!number) return false;
    if (key == "smear_fraction") {
      if (*number < 0.0 || *number > channel_equalise::kMaxFraction) {
        return false;
      }
      smear.fraction = *number;
    } else if (key == "smear_tau_samples") {
      if (*number < channel_equalise::kMinTauSamples ||
          *number > channel_equalise::kMaxTauSamples) {
        return false;
      }
      smear.tau_samples = *number;
    } else {
      return false;
    }
  }
  smear_ = smear;
  track_ = std::move(track);
  track_field2_ = std::move(track_field2);
  set_configuration_status(
      (smear_.active() || !track_.empty() || !track_field2_.empty())
          ? ConfigurationStatus::Green
          : ConfigurationStatus::Yellow);
  return true;
}

StagePreviewCapability ChannelEqualiseStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_output_);
}

std::vector<StageToolDescriptor> ChannelEqualiseStage::get_stage_tools() const {
  StageToolDescriptor tool;
  tool.tool_id = channel_calibration_tool::kToolId;
  tool.display_name = "Channel Calibration";
  tool.description =
      "Measures the disc's edge smear from the test signals on field 1 "
      "line 19, for Channel Equalise";
  tool.kind = StageToolKind::Analysis;
  tool.contract_id = channel_calibration_tool::kContractId;
  StageToolDescriptor tracking;
  tracking.tool_id = channel_calibration_tool::kTrackingToolId;
  tracking.display_name = "Channel Tracking";
  tracking.description =
      "Measures the edge smear on line 19 of every field and follows it "
      "along the source, so each frame is corrected with its own";
  tracking.kind = StageToolKind::Analysis;
  tracking.contract_id = channel_calibration_tool::kTrackingContractId;
  return {tool, tracking};
}

std::vector<ParameterDescriptor> ChannelEqualiseStage::analysis_tool_parameters(
    const std::string& tool_id) const {
  if (tool_id != channel_calibration_tool::kTrackingToolId) return {};
  return {ParameterDescriptor{
      channel_calibration_tool::kNearEdgeParameter, "Near-edge tails",
      "Also correct a tail confined to the first few samples after an edge "
      "(under 0.4 us), where both edges of the white bar carry one. Off (the "
      "default): only a source with the long smear is corrected.",
      ParameterType::BOOL,
      ParameterConstraints{std::nullopt,
                           std::nullopt,
                           ParameterValue(false),
                           {},
                           false,
                           std::nullopt}}};
}

StageAnalysisReport ChannelEqualiseStage::run_analysis_tool(
    const std::string& tool_id,
    const std::map<std::string, ParameterValue>& parameters,
    IToolJobContext& context) {
  if (tool_id == channel_calibration_tool::kTrackingToolId) {
    ChannelTrackOptions options;
    const auto it =
        parameters.find(channel_calibration_tool::kNearEdgeParameter);
    if (it != parameters.end()) {
      if (const auto* flag = std::get_if<bool>(&it->second)) {
        options.near_edge = *flag;
      }
    }
    return channel_calibration_tool::run_tracking(context, options);
  }
  if (tool_id != channel_calibration_tool::kToolId) {
    StageAnalysisReport report;
    report.summary = "Channel Equalise has no tool '" + tool_id + "'";
    return report;
  }
  return channel_calibration_tool::run(context);
}

}  // namespace orc
