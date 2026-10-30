/*
 * File:        picture_band_limit_stage.cpp
 * Module:      orc-stage-plugin-picture-band-limit
 * Purpose:     Cuts the band above the picture: a low-pass of the luma
 *              (composite stays composite)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "picture_band_limit_stage.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/error_types.h>
#include <orc/support/composite_split.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>

#include <algorithm>
#include <cmath>

namespace orc {

namespace {

constexpr double kHzPerMhz = 1.0e6;

// The parameters' ranges, in MHz.
constexpr double kMinCutoffMhz = 2.0;
constexpr double kMaxCutoffMhz = 8.0;
constexpr double kMinRolloffMhz = 0.1;
constexpr double kMaxRolloffMhz = 2.0;

// Stop-band attenuation of the low-pass, in dB.
constexpr double kStopBandDb = 50.0;

// The zeroth-order modified Bessel function of the first kind, for the
// Kaiser window.
double bessel_i0(double x) {
  double sum = 1.0;
  double term = 1.0;
  for (int k = 1; k < 50; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < sum * 1e-12) break;
  }
  return sum;
}

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

size_t first_field_lines(VideoSystem system) {
  return system == VideoSystem::PAL ? static_cast<size_t>(kPalField1Lines)
                                    : static_cast<size_t>(kNtscField1Lines);
}

}  // namespace

double default_picture_cutoff_mhz(VideoSystem system) {
  return system == VideoSystem::PAL ? 5.4 : 5.0;
}

std::vector<double> picture_band_taps(double cutoff_hz, double rolloff_hz,
                                      double sample_rate_hz) {
  // Kaiser's estimates for the window's shape and length.
  const double beta = 0.1102 * (kStopBandDb - 8.7);
  const double transition =
      2.0 * composite_split::kPi * rolloff_hz / sample_rate_hz;
  const auto radius = static_cast<int32_t>(
      std::ceil((kStopBandDb - 8.0) / (2.285 * transition) / 2.0));
  const double fc = cutoff_hz / sample_rate_hz;
  std::vector<double> taps(static_cast<size_t>(radius) + 1);
  double sum = 0.0;
  for (int32_t k = 0; k <= radius; ++k) {
    const double x = 2.0 * fc * k;
    const double sinc = k == 0 ? 1.0
                               : std::sin(composite_split::kPi * x) /
                                     (composite_split::kPi * x);
    const double r = static_cast<double>(k) / (radius + 1);
    const double window =
        bessel_i0(beta * std::sqrt(1.0 - (r * r))) / bessel_i0(beta);
    taps[static_cast<size_t>(k)] = 2.0 * fc * sinc * window;
    sum += (k == 0 ? 1.0 : 2.0) * taps[static_cast<size_t>(k)];
  }
  for (double& tap : taps) tap /= sum;
  return taps;
}

bool picture_band_limits_composite(VideoSystem system, double cutoff_hz,
                                   double rolloff_hz) {
  const double chroma_edge = (sample_rate_from_system(system) / 4.0) +
                             composite_split::kChromaBandwidthHz;
  return cutoff_hz - (rolloff_hz / 2.0) >= chroma_edge;
}

// ============================================================================
// BandLimitedVideoFrameRepresentation
// ============================================================================

BandLimitedVideoFrameRepresentation::BandLimitedVideoFrameRepresentation(
    std::shared_ptr<const VideoFrameRepresentation> source, double cutoff_mhz,
    double rolloff_mhz)
    : VideoFrameRepresentationWrapper(std::move(source)),
      Artifact(ArtifactID("band_limited_frame"), Provenance{}),
      frames_(kMaxCachedFrames) {
  const auto params = source_->get_video_parameters();
  const VideoSystem system = params ? params->system : VideoSystem::PAL;
  taps_ = picture_band_taps(cutoff_mhz * kHzPerMhz, rolloff_mhz * kHzPerMhz,
                            sample_rate_from_system(system));
  whole_composite_ =
      source_->has_separate_channels() ||
      picture_band_limits_composite(system, cutoff_mhz * kHzPerMhz,
                                    rolloff_mhz * kHzPerMhz);
}

const std::vector<int16_t>* BandLimitedVideoFrameRepresentation::filtered(
    FrameID id) const {
  if (const auto* cached = frames_.get_ptr(id)) return cached;
  const auto desc = source_->get_frame_descriptor(id);
  if (!desc) return nullptr;
  const bool yc = source_->has_separate_channels();
  // The Y/C luma plane carries no subcarrier; composite is split unless the
  // whole roll-off lies above its chroma band.
  std::vector<int16_t> frame;
  if (yc) {
    const int16_t* luma = source_->get_frame_luma(id);
    if (!luma) return nullptr;
    frame.assign(luma, luma + desc->samples_total);
  } else {
    frame = source_->get_frame_copy(id);
  }
  if (frame.empty()) return nullptr;

  const VideoSystem system = desc->system;
  const size_t spl = desc->samples_per_line_nominal;
  const auto params = source_->get_video_parameters();
  const size_t f1 = first_field_lines(system);

  // The active picture, in flat lines of each field (see Dropout
  // Correction's replacement search) and in samples of each line. Lines
  // outside it, VBI data and test signals among them, are left as they are.
  size_t first_line = 0;
  size_t field1_end = f1;
  size_t field2_end = desc->height - f1;
  int32_t active_start = 0;
  int32_t active_end = -1;
  if (params && params->first_active_frame_line >= 0 &&
      params->last_active_frame_line >= 0) {
    first_line = static_cast<size_t>(params->first_active_frame_line / 2);
    field1_end = std::min(
        f1, static_cast<size_t>((params->last_active_frame_line + 1) / 2));
    field2_end =
        std::min(desc->height - f1,
                 static_cast<size_t>(params->last_active_frame_line / 2));
  }
  if (params && params->active_video_start >= 0 &&
      params->active_video_end > params->active_video_start) {
    active_start = params->active_video_start;
    active_end = params->active_video_end;
  }

  const auto radius = static_cast<int32_t>(taps_.size()) - 1;
  const bool split = !whole_composite_;
  const composite_split::ChromaSplitter splitter(system);
  std::vector<std::complex<double>> chroma;
  std::vector<double> padded;
  std::vector<double> chroma_part;

  auto filter_line = [&](size_t line) {
    const size_t base = frame_line_sample_offset(system, spl, line);
    const auto length =
        static_cast<int32_t>(frame_line_sample_count(system, spl, line));
    const int16_t* x = frame.data() + base;
    const int32_t start = std::clamp(active_start, 0, length);
    const int32_t end = active_end < 0 ? length : std::min(active_end, length);
    if (end <= start) return;
    // The samples the low-pass reads, with the line's own samples beyond the
    // active picture and its end samples repeated past the line's ends.
    const int32_t lo = std::max(0, start - radius);
    const int32_t hi = std::min(length, end + radius);
    if (split) {
      splitter.demodulate(x, length, lo, hi, chroma);
    }
    padded.assign(
        static_cast<size_t>(end - start) + (2 * static_cast<size_t>(radius)),
        0.0);
    for (int32_t n = start - radius; n < end + radius; ++n) {
      const int32_t m = std::clamp(n, lo, hi - 1);
      double v = x[m];
      if (split) {
        v -=
            composite_split::remodulate(chroma[static_cast<size_t>(m - lo)], m);
      }
      padded[static_cast<size_t>(n - (start - radius))] = v;
    }
    chroma_part.assign(static_cast<size_t>(end - start), 0.0);
    if (split) {
      for (int32_t n = start; n < end; ++n) {
        chroma_part[static_cast<size_t>(n - start)] =
            composite_split::remodulate(chroma[static_cast<size_t>(n - lo)], n);
      }
    }
    int16_t* out = frame.data() + base;
    for (int32_t n = start; n < end; ++n) {
      const double* centre =
          padded.data() + static_cast<size_t>(n - start + radius);
      double sum = taps_[0] * centre[0];
      for (int32_t k = 1; k <= radius; ++k) {
        sum += taps_[static_cast<size_t>(k)] * (centre[-k] + centre[k]);
      }
      sum += chroma_part[static_cast<size_t>(n - start)];
      out[n] = static_cast<int16_t>(std::clamp(
          static_cast<int64_t>(std::llround(sum)),
          static_cast<int64_t>(INT16_MIN), static_cast<int64_t>(INT16_MAX)));
    }
  };

  for (size_t line = first_line; line < field1_end; ++line) {
    filter_line(line);
  }
  for (size_t line = f1 + first_line; line < f1 + field2_end; ++line) {
    filter_line(line);
  }

  // put_if_absent: concurrent readers of one frame may both filter it, and
  // replacing a cached entry would free a buffer another thread still reads.
  frames_.put_if_absent(id, std::move(frame));
  return frames_.get_ptr(id);
}

const int16_t* BandLimitedVideoFrameRepresentation::line_of(FrameID id,
                                                            size_t line) const {
  const auto* frame = filtered(id);
  const auto desc = source_->get_frame_descriptor(id);
  if (!frame || !desc || line >= desc->height) return nullptr;
  return frame->data() +
         frame_line_sample_offset(desc->system, desc->samples_per_line_nominal,
                                  line);
}

const int16_t* BandLimitedVideoFrameRepresentation::get_frame(
    FrameID id) const {
  if (source_->has_separate_channels()) return source_->get_frame(id);
  const auto* frame = filtered(id);
  return frame ? frame->data() : nullptr;
}

const int16_t* BandLimitedVideoFrameRepresentation::get_line(
    FrameID id, size_t line) const {
  if (source_->has_separate_channels()) return source_->get_line(id, line);
  return line_of(id, line);
}

std::vector<int16_t> BandLimitedVideoFrameRepresentation::get_frame_copy(
    FrameID id) const {
  if (source_->has_separate_channels()) return source_->get_frame_copy(id);
  const auto* frame = filtered(id);
  return frame ? *frame : std::vector<int16_t>{};
}

const int16_t* BandLimitedVideoFrameRepresentation::get_frame_luma(
    FrameID id) const {
  if (!source_->has_separate_channels()) {
    return VideoFrameRepresentationWrapper::get_frame_luma(id);
  }
  const auto* frame = filtered(id);
  return frame ? frame->data() : nullptr;
}

const int16_t* BandLimitedVideoFrameRepresentation::get_line_luma(
    FrameID id, size_t line) const {
  if (!source_->has_separate_channels()) {
    return VideoFrameRepresentationWrapper::get_line_luma(id, line);
  }
  return line_of(id, line);
}

// ============================================================================
// PictureBandLimitStage
// ============================================================================

std::vector<ArtifactPtr> PictureBandLimitStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& /*observation_context*/) {
  if (inputs.empty()) {
    throw DAGExecutionError("PictureBandLimitStage requires one input");
  }
  auto source =
      std::dynamic_pointer_cast<const VideoFrameRepresentation>(inputs[0]);
  if (!source) {
    throw DAGExecutionError(
        "PictureBandLimitStage input must be VideoFrameRepresentation");
  }
  if (!parameters.empty() && !set_parameters(parameters)) {
    throw DAGExecutionError("PictureBandLimitStage: invalid parameters");
  }
  const auto params = source->get_video_parameters();
  const VideoSystem system = params ? params->system : VideoSystem::PAL;
  const double cutoff =
      cutoff_mhz_.value_or(default_picture_cutoff_mhz(system));
  ORC_LOG_DEBUG("PictureBandLimit: cutoff {:.2f} MHz, roll-off {:.2f} MHz, {}",
                cutoff, rolloff_mhz_,
                source->has_separate_channels() ? "Y/C luma plane"
                : picture_band_limits_composite(system, cutoff * kHzPerMhz,
                                                rolloff_mhz_ * kHzPerMhz)
                    ? "whole composite"
                    : "luma of the composite");
  auto output = std::make_shared<BandLimitedVideoFrameRepresentation>(
      source, cutoff, rolloff_mhz_);
  cached_output_ = output;
  return {output};
}

std::vector<ParameterDescriptor>
PictureBandLimitStage::get_parameter_descriptors(VideoSystem project_format,
                                                 SourceType) const {
  const double default_cutoff = default_picture_cutoff_mhz(
      project_format == VideoSystem::Unknown ? VideoSystem::PAL
                                             : project_format);
  return {
      ParameterDescriptor{
          "cutoff_mhz", "Cutoff (MHz)",
          "Where the picture ends: the frequency at which the low-pass "
          "halves the luma. Default 5.4 MHz for PAL, measured on the "
          "Domesday captures, and 5.0 MHz for NTSC and PAL-M, not yet "
          "measured on real discs. Source Alignment Analysis reports a "
          "set's own cutoff as lumaCutoffMHz.",
          ParameterType::DOUBLE,
          ParameterConstraints{ParameterValue(kMinCutoffMhz),
                               ParameterValue(kMaxCutoffMhz),
                               ParameterValue(default_cutoff),
                               {},
                               false,
                               std::nullopt}},
      ParameterDescriptor{
          "rolloff_mhz", "Roll-off (MHz)",
          "Width of the low-pass's transition, centred on the cutoff.",
          ParameterType::DOUBLE,
          ParameterConstraints{ParameterValue(kMinRolloffMhz),
                               ParameterValue(kMaxRolloffMhz),
                               ParameterValue(kDefaultRolloffMhz),
                               {},
                               false,
                               std::nullopt}},
  };
}

std::map<std::string, ParameterValue> PictureBandLimitStage::get_parameters()
    const {
  std::map<std::string, ParameterValue> params{
      {"rolloff_mhz", ParameterValue{rolloff_mhz_}}};
  if (cutoff_mhz_) params["cutoff_mhz"] = ParameterValue{*cutoff_mhz_};
  return params;
}

bool PictureBandLimitStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  for (const auto& [key, value] : params) {
    const auto number = as_double(value);
    if (key == "cutoff_mhz") {
      if (!number || *number < kMinCutoffMhz || *number > kMaxCutoffMhz) {
        return false;
      }
      cutoff_mhz_ = number;
    } else if (key == "rolloff_mhz") {
      if (!number || *number < kMinRolloffMhz || *number > kMaxRolloffMhz) {
        return false;
      }
      rolloff_mhz_ = *number;
    } else {
      return false;
    }
  }
  return true;
}

StagePreviewCapability PictureBandLimitStage::get_preview_capability() const {
  return PreviewHelpers::make_signal_preview_capability(cached_output_);
}

}  // namespace orc
