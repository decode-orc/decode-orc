/*
 * File:        burst_level_observer.cpp
 * Module:      orc-sdk-support
 * Purpose:     Color burst median IRE level observer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/logging.h>
#include <orc/support/observers/burst_level_observer.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace orc {

void BurstLevelObserver::process_frame(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    IObservationContext& context) {
  auto vp_opt = representation.get_video_parameters();
  if (!vp_opt.has_value()) {
    ORC_LOG_TRACE("BurstLevelObserver: No video parameters for frame {}",
                  frame_id);
    return;
  }
  const auto& vp = vp_opt.value();

  // Colour burst range derived from video system constant.
  const auto [sys_burst_start, sys_burst_end] = colour_burst_range(vp.system);
  size_t burst_start_idx = static_cast<size_t>(sys_burst_start);
  size_t burst_end_idx = static_cast<size_t>(sys_burst_end);

  size_t f1_lines = field1_lines(vp.system);
  size_t line_width = static_cast<size_t>(vp.frame_width_nominal);

  // CVBS_U10_4FSC: threshold for outlier rejection in 10-bit sample domain.
  // 30 IRE → 30.0 / ire_per_unit where ire_per_unit = 100 / (white - blanking).
  const double outlier_threshold_10bit =
      30.0 * static_cast<double>(vp.white_level - vp.blanking_level) / 100.0;

  // peak = RMS * sqrt(2) for a pure sine burst
  static constexpr double k_sqrt2 = 1.41421356237309504880;

  // Collect samples from both field halves (3 lines each = up to 6 total),
  // producing one frame-level result rather than two field-level entries.
  std::array<double, 6> burst_levels_raw{};
  size_t burst_sample_count = 0;

  for (size_t field_idx = 0; field_idx < 2; ++field_idx) {
    size_t line_offset = (field_idx == 0) ? 0 : f1_lines;
    size_t field_height = (field_idx == 0)
                              ? f1_lines
                              : static_cast<size_t>(vp.frame_height) - f1_lines;

    size_t start_line = 11;
    size_t last_active_field_line =
        static_cast<size_t>(vp.last_active_frame_line / 2);
    size_t end_line = std::min(field_height - 10, last_active_field_line > 0
                                                      ? last_active_field_line
                                                      : field_height - 10);

    if (end_line <= start_line) {
      continue;
    }

    const std::array<size_t, 3> sample_lines = {
        start_line, start_line + (end_line - start_line) / 2, end_line - 1};

    for (size_t field_line : sample_lines) {
      if (burst_sample_count >= burst_levels_raw.size()) {
        break;
      }

      const auto line_data =
          representation.get_line_samples(frame_id, line_offset + field_line);
      if (line_data.empty()) {
        continue;
      }

      size_t burst_end = burst_end_idx;
      if (burst_end >= line_width) {
        burst_end = line_width - 1;
      }
      if (burst_end <= burst_start_idx) {
        continue;
      }

      const size_t n_samples = burst_end - burst_start_idx + 1;
      if (n_samples < 4) {
        continue;
      }

      double sum = 0.0;
      for (size_t i = burst_start_idx; i <= burst_end; ++i) {
        sum += static_cast<double>(line_data[i]);
      }
      const double mean = sum / static_cast<double>(n_samples);

      double sum_sq = 0.0;
      for (size_t i = burst_start_idx; i <= burst_end; ++i) {
        const double d = static_cast<double>(line_data[i]) - mean;
        sum_sq += d * d;
      }
      const double rms = std::sqrt(sum_sq / static_cast<double>(n_samples));
      const double peak_amplitude = rms * k_sqrt2;

      if (peak_amplitude > outlier_threshold_10bit) {
        continue;
      }

      burst_levels_raw[burst_sample_count++] = peak_amplitude;
    }
  }

  if (burst_sample_count == 0) {
    ORC_LOG_TRACE("BurstLevelObserver: No valid burst samples for frame {}",
                  frame_id);
    return;
  }

  double median_raw =
      calculate_median(burst_levels_raw.data(), burst_sample_count);

  context.set(FieldID(frame_id * 2), "burst_level", "median_burst_10bit",
              median_raw);
  ORC_LOG_DEBUG("BurstLevelObserver: Frame {} median_burst_10bit={:.2f}",
                frame_id, median_raw);
}

double BurstLevelObserver::calculate_median(const double* values,
                                            size_t count) const {
  if (count == 0) {
    return 0.0;
  }

  // At most 6 elements (3 lines × 2 field halves) — sort a local fixed-size
  // copy
  std::array<double, 6> sorted{};
  for (size_t i = 0; i < count; ++i) sorted[i] = values[i];
  std::sort(sorted.begin(),
            sorted.begin() + static_cast<std::ptrdiff_t>(count));

  if (count % 2 == 0) {
    return (sorted[count / 2 - 1] + sorted[count / 2]) / 2.0;
  }
  return sorted[count / 2];
}

}  // namespace orc
