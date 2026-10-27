/*
 * File:        white_snr_observer.cpp
 * Module:      orc-core
 * Purpose:     White SNR observer implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>
#include <white_snr_observer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

namespace orc {

void WhiteSNRObserver::process_frame(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    IObservationContext& context) {
  auto vp_opt = representation.get_video_parameters();
  if (!vp_opt.has_value()) {
    ORC_LOG_TRACE("WhiteSNRObserver: No video parameters for frame {}",
                  frame_id);
    return;
  }
  const auto& vp = vp_opt.value();

  // VITS white flag locations (lines from ld-process-vits), in µs from 0H,
  // the sync edge every stored line starts at.  Each window starts 2 µs
  // after its bar's nominal start, so no part of the rising edge reaches it:
  // a window on the edge measures where the capture's sample grid falls
  // rather than its noise, and a stack inherits its first input's grid.
  // Decodes put the edge up to 0.6 µs either side of nominal and some settle
  // over a further 1.3 µs (Community South line 19: 50 % at 11.9 µs, settled
  // at 13.3 µs; there the 12 µs start that ld-process-vits used read
  // 30.2-31.7 dB across one pressing and 14 µs reads 34.9-35.2 dB).
  // PAL: Line 19, 14μs start, 6μs length (UK ITS white bar, 12-22 μs)
  // NTSC: Line 20 (14μs, 12μs) (NTC-7 composite bar, 12-30 μs), Line 20
  // (52μs, 8μs) (FCC composite bar on field 2, 43.9-62 μs), Line 13 (14μs,
  // 14μs) (NTC-7 composite bar where a disc carries it there)

  struct WhiteConfig {
    size_t line;
    double start_us;
    double length_us;
  };

  static const std::array<WhiteConfig, 1> pal_configs{{{19, 14.0, 6.0}}};
  static const std::array<WhiteConfig, 3> ntsc_configs{
      {{20, 14.0, 12.0}, {20, 52.0, 8.0}, {13, 14.0, 14.0}}};

  const WhiteConfig* configs = nullptr;
  size_t configs_size = 0;
  if (vp.system == VideoSystem::PAL) {
    configs = pal_configs.data();
    configs_size = pal_configs.size();
  } else {
    configs = ntsc_configs.data();
    configs_size = ntsc_configs.size();
  }

  size_t f1_lines = field1_lines(vp.system);

  // Accumulate SNR from both field halves, then store one frame-level result.
  double white_ire_min = 90.0;
  double white_ire_max = 110.0;
  double snr_sum = 0.0;
  size_t snr_count = 0;

  for (size_t field_idx = 0; field_idx < 2; ++field_idx) {
    size_t line_offset = (field_idx == 0) ? 0 : f1_lines;
    size_t field_height = (field_idx == 0)
                              ? f1_lines
                              : static_cast<size_t>(vp.frame_height) - f1_lines;

    for (size_t i = 0; i < configs_size; ++i) {
      const auto& config = configs[i];
      auto white_slice = get_line_slice_ire(
          representation, frame_id, line_offset, config.line, config.start_us,
          config.length_us, field_height, vp);

      if (white_slice.empty()) {
        continue;
      }

      double white_mean = calc_mean(white_slice);
      if (white_mean >= white_ire_min && white_mean <= white_ire_max) {
        snr_sum += calculate_psnr(white_slice);
        ++snr_count;
        break;
      }
    }
  }

  if (snr_count == 0) {
    ORC_LOG_DEBUG("WhiteSNRObserver: No valid white flag found for frame {}",
                  frame_id);
    return;
  }

  double snr_db = snr_sum / static_cast<double>(snr_count);
  context.set(FieldID(frame_id * 2), "white_snr", "snr_db", snr_db);
  ORC_LOG_DEBUG("WhiteSNRObserver: Frame {} snr={:.2f} dB", frame_id, snr_db);
}

std::vector<double> WhiteSNRObserver::get_line_slice_ire(
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

double WhiteSNRObserver::calculate_psnr(const std::vector<double>& data) const {
  if (data.empty()) {
    return 0.0;
  }

  double signal = calc_mean(data);
  double noise = calc_std(data);

  if (noise <= 0.001) {
    return 80.0;
  }

  return 20.0 * std::log10(signal / noise);
}

double WhiteSNRObserver::calc_mean(const std::vector<double>& data) const {
  if (data.empty()) {
    return 0.0;
  }

  double sum = std::accumulate(data.begin(), data.end(), 0.0);
  return sum / static_cast<double>(data.size());
}

double WhiteSNRObserver::calc_std(const std::vector<double>& data) const {
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
