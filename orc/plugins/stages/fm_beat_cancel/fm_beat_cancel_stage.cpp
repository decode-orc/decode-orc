/*
 * File:        fm_beat_cancel_stage.cpp
 * Module:      orc-stage-plugin-fm-beat-cancel
 * Purpose:     Removes the PAL LaserDisc FM-beat herringbone from each field
 *              (composite stays composite)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "fm_beat_cancel_stage.h"

#include <orc/stage/error_types.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

namespace orc {

namespace {

constexpr double kHzPerMhz = 1.0e6;

// The carrier's range, in MHz: the PAL LaserDisc carrier at blanking is
// 7.1 MHz, and a master's own is some tens of kHz from it.
constexpr double kMinCarrierMhz = 6.9;
constexpr double kMaxCarrierMhz = 7.3;

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
// BeatCancelledVideoFrameRepresentation
// ============================================================================

BeatCancelledVideoFrameRepresentation::BeatCancelledVideoFrameRepresentation(
    std::shared_ptr<const VideoFrameRepresentation> source,
    double carrier_black_hz)
    : VideoFrameRepresentationWrapper(std::move(source)),
      Artifact(ArtifactID("beat_cancelled_frame"), Provenance{}),
      carrier_black_hz_(carrier_black_hz),
      frames_(kMaxCachedFrames) {}

const std::vector<int16_t>* BeatCancelledVideoFrameRepresentation::cancelled(
    FrameID id) const {
  if (const auto* cached = frames_.get_ptr(id)) return cached;
  const auto desc = source_->get_frame_descriptor(id);
  const auto params = source_->get_video_parameters();
  // A copy: a source's frame pointer is not held while the fields are fitted.
  const std::vector<int16_t> copy = source_->get_frame_copy(id);
  const int16_t* input = copy.data();
  std::vector<int16_t> frame;
  if (desc && params && copy.size() >= desc->samples_total &&
      desc->system == VideoSystem::PAL && !source_->has_separate_channels()) {
    // A field without flat saturated colour is not touched; a frame neither
    // of whose fields is corrected stays the source's.
    for (const auto& field : fm_beat::frame_fields(*desc, *params)) {
      if (fm_beat::flat_colour_samples(input, field) <
          fm_beat::kMinFlatColourSamples) {
        continue;
      }
      const fm_beat::FieldBeat beat(input, field, carrier_black_hz_);
      const auto fit = beat.fit();
      if (fit.lines_corrected == 0) continue;
      if (frame.empty()) frame = copy;
      beat.subtract(fit, frame.data());
    }
  }
  // put_if_absent: concurrent readers of one frame may both cancel it, and
  // replacing a cached entry would free a buffer another thread still reads.
  frames_.put_if_absent(id, std::move(frame));
  return frames_.get_ptr(id);
}

const int16_t* BeatCancelledVideoFrameRepresentation::get_frame(
    FrameID id) const {
  const auto* frame = cancelled(id);
  if (!frame || frame->empty()) return source_->get_frame(id);
  return frame->data();
}

const int16_t* BeatCancelledVideoFrameRepresentation::get_line(
    FrameID id, size_t line) const {
  const auto* frame = cancelled(id);
  if (!frame || frame->empty()) return source_->get_line(id, line);
  const auto desc = source_->get_frame_descriptor(id);
  if (!desc || line >= desc->height) return nullptr;
  return frame->data() +
         frame_line_sample_offset(desc->system, desc->samples_per_line_nominal,
                                  line);
}

std::vector<int16_t> BeatCancelledVideoFrameRepresentation::get_frame_copy(
    FrameID id) const {
  const auto* frame = cancelled(id);
  if (!frame || frame->empty()) return source_->get_frame_copy(id);
  return *frame;
}

// ============================================================================
// FmBeatCancelStage
// ============================================================================

std::vector<ArtifactPtr> FmBeatCancelStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& /*observation_context*/) {
  if (inputs.empty()) {
    throw DAGExecutionError("FmBeatCancelStage requires one input");
  }
  auto source =
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  if (!source) {
    throw DAGExecutionError(
        "FmBeatCancelStage input must be VideoFrameRepresentation");
  }
  if (!parameters.empty() && !set_parameters(parameters)) {
    throw DAGExecutionError("FmBeatCancelStage: invalid parameters");
  }
  const auto params = source->get_video_parameters();
  if (!params || params->system != VideoSystem::PAL ||
      source->has_separate_channels()) {
    ORC_LOG_WARN(
        "FmBeatCancel: the beat is a PAL LaserDisc composite effect; this "
        "source passes through unchanged");
  }
  ORC_LOG_DEBUG("FmBeatCancel: carrier at black {:.4f} MHz",
                carrier_black_mhz_);
  auto output = std::make_shared<BeatCancelledVideoFrameRepresentation>(
      source, carrier_black_mhz_ * kHzPerMhz);
  cached_output_ = output;
  return {output};
}

std::vector<ParameterDescriptor> FmBeatCancelStage::get_parameter_descriptors(
    VideoSystem /*project_format*/, SourceType /*source_type*/) const {
  return {
      ParameterDescriptor{
          "carrier_black_mhz", "Carrier at black (MHz)",
          "The master's FM video carrier at black. The beat's phase along a "
          "line follows it closely, so set it with the FM Beat Calibration "
          "tool, which measures it on the source. Default 7.1 MHz, the PAL "
          "LaserDisc standard's.",
          ParameterType::DOUBLE,
          ParameterConstraints{
              ParameterValue(kMinCarrierMhz),
              ParameterValue(kMaxCarrierMhz),
              ParameterValue(fm_beat::kNominalCarrierBlackHz / kHzPerMhz),
              {},
              false,
              std::nullopt}},
  };
}

std::map<std::string, ParameterValue> FmBeatCancelStage::get_parameters()
    const {
  return {{"carrier_black_mhz", ParameterValue{carrier_black_mhz_}}};
}

bool FmBeatCancelStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    const auto number = as_double(value);
    if (key != "carrier_black_mhz" || !number || *number < kMinCarrierMhz ||
        *number > kMaxCarrierMhz) {
      return false;
    }
    carrier_black_mhz_ = *number;
  }
  return true;
}

StagePreviewCapability FmBeatCancelStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_output_);
}

}  // namespace orc
