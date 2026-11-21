/*
 * File:        black_psnr_observer.cpp
 * Module:      orc-sdk-support
 * Purpose:     Black PSNR observer implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>
#include <orc/support/observers/black_psnr_observer.h>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace orc {

void BlackPSNRObserver::process_frame(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    IObservationContext& context) {
  auto vp_opt = representation.get_video_parameters();
  if (!vp_opt.has_value()) {
    ORC_LOG_TRACE("BlackPSNRObserver: No video parameters for frame {}",
                  frame_id);
    return;
  }
  const auto& vp = vp_opt.value();

  size_t f1_lines = field1_lines(vp.system);

  // VITS black level locations (from ld-process-vits)
  // PAL: Line 22, 12μs start, 50μs length
  // NTSC: Line 1, 10μs start, 20μs length
  size_t line;
  double start_us;
  double length_us;

  if (vp.system == VideoSystem::PAL) {
    line = 22;
    start_us = 12.0;
    length_us = 50.0;
  } else {
    line = 1;
    start_us = 10.0;
    length_us = 20.0;
  }

  // Accumulate PSNR from both field halves, then store one frame-level result.
  double psnr_sum = 0.0;
  size_t psnr_count = 0;

  for (size_t field_idx = 0; field_idx < 2; ++field_idx) {
    size_t line_offset = (field_idx == 0) ? 0 : f1_lines;
    size_t field_height = (field_idx == 0)
                              ? f1_lines
                              : static_cast<size_t>(vp.frame_height) - f1_lines;

    auto black_slice =
        get_line_slice_ire(representation, frame_id, line_offset, line,
                           start_us, length_us, field_height, vp);

    if (black_slice.empty()) {
      continue;
    }

    psnr_sum += calculate_psnr(black_slice);
    ++psnr_count;
  }

  if (psnr_count == 0) {
    ORC_LOG_TRACE("BlackPSNRObserver: No valid black level data for frame {}",
                  frame_id);
    return;
  }

  double psnr_db = psnr_sum / static_cast<double>(psnr_count);
  context.set(FieldID(frame_id * 2), "black_psnr", "psnr_db", psnr_db);
  ORC_LOG_DEBUG("BlackPSNRObserver: Frame {} psnr={:.2f} dB", frame_id,
                psnr_db);
}

std::vector<double> BlackPSNRObserver::get_line_slice_ire(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    size_t line_offset, size_t field_line, double start_us, double length_us,
    size_t field_height, const SourceParameters& vp) const {
  std::vector<double> result;

  // Adjust for 1-based line numbering
  size_t line_index = field_line - 1;

  // Range check
  if (line_index >= field_height) {
    return result;
  }

  // Calculate samples per microsecond
  const bool uses_pal_line_timing = (vp.system == VideoSystem::PAL);
  double us_per_line = uses_pal_line_timing ? 64.0 : 63.5;
  double samples_per_us =
      static_cast<double>(vp.frame_width_nominal) / us_per_line;

  // Calculate sample positions
  size_t start_sample = static_cast<size_t>(start_us * samples_per_us);
  size_t length_samples = static_cast<size_t>(length_us * samples_per_us);

  // Range check
  if (start_sample + length_samples >
      static_cast<size_t>(vp.frame_width_nominal)) {
    return result;
  }

  // Get the line data; use get_line_samples() for composite sources so the
  // field-level buffer in the VFR is used instead of a full-frame LRU load.
  size_t frame_line = line_offset + line_index;
  auto line_samples_buf = std::vector<int16_t>{};
  const int16_t* line_data = nullptr;
  if (representation.has_separate_channels()) {
    line_data = representation.get_line_luma(frame_id, frame_line);
  } else {
    line_samples_buf = representation.get_line_samples(frame_id, frame_line);
    line_data = line_samples_buf.empty() ? nullptr : line_samples_buf.data();
  }
  if (!line_data) {
    return result;
  }

  // CVBS_U10_4FSC signal levels from SourceParameters.
  double black_10b = static_cast<double>(vp.blanking_level);
  double white_10b = static_cast<double>(vp.white_level);
  double ire_scale = 100.0 / (white_10b - black_10b);

  // Convert samples to IRE values
  result.reserve(length_samples);
  for (size_t i = 0; i < length_samples; ++i) {
    double sample = static_cast<double>(line_data[start_sample + i]);
    double ire = (sample - black_10b) * ire_scale;
    result.push_back(ire);
  }

  return result;
}

double BlackPSNRObserver::calculate_psnr(
    const std::vector<double>& data) const {
  if (data.empty()) {
    return 0.0;
  }

  // PSNR uses 100 IRE as the reference signal
  double signal = 100.0;
  double noise = calc_std(data);

  // For very low noise (essentially perfect signal), cap at a reasonable
  // maximum
  if (noise <= 0.001) {
    return 80.0;  // Cap at 80 dB for effectively noiseless signals
  }

  return 20.0 * std::log10(signal / noise);
}

double BlackPSNRObserver::calc_mean(const std::vector<double>& data) const {
  if (data.empty()) {
    return 0.0;
  }

  double sum = std::accumulate(data.begin(), data.end(), 0.0);
  return sum / static_cast<double>(data.size());
}

double BlackPSNRObserver::calc_std(const std::vector<double>& data) const {
  if (data.empty()) {
    return 0.0;
  }

  double mean = calc_mean(data);
  double sum_squared_diff = 0.0;

  for (double value : data) {
    double diff = value - mean;
    sum_squared_diff += diff * diff;
  }

  return std::sqrt(sum_squared_diff / static_cast<double>(data.size()));
}

}  // namespace orc
