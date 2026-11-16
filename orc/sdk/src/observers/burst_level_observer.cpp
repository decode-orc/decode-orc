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
#include <orc/support/pal_line_locked_resample.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace orc {

namespace {

// The 300 ns gate edges of the SECAM back-porch sub-carrier reference, in
// samples at 4fsc (ITU-R BT.1700-1 Annex 1 Part C Table 5 item i).
constexpr size_t kSecamGateEdgeSamples = 5;

}  // namespace

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

  // SECAM has no colour burst.  Its colour_burst_range() is the undeviated
  // sub-carrier reference on the back porch (BT.1700-1 Annex 1 Part C Table 5
  // items i, b), and this observer reports that reference's peak level in its
  // place.  The window drops the 300 ns (5 sample) gate edges at each end,
  // which would read low.  The reference is fOR on a D'R line and fOB on a
  // D'B line, and the transmitter's high-frequency precorrection (Table 4
  // item 10d) gives them different levels (about 15 % and 12 % of the
  // luminance amplitude), so each reading averages a line pair.
  const bool is_secam = vp.system == VideoSystem::SECAM;
  if (is_secam) {
    burst_start_idx += kSecamGateEdgeSamples;
    burst_end_idx -= kSecamGateEdgeSamples;
  }

  // Peak burst (or SECAM reference) level of field line |field_line|, or
  // nullopt when the line is missing or reads as an outlier.
  const auto line_peak = [&](size_t field_idx, size_t line_offset,
                             size_t field_line) -> std::optional<double> {
    const auto line_data =
        representation.get_line_samples(frame_id, line_offset + field_line);
    if (line_data.empty()) {
      return std::nullopt;
    }

    // On the PAL 4fsc lattice 0H moves up to 2 samples later through the
    // field (CVBS File Format Specification v1.8.1, Preset PAL); the
    // window moves with it so it holds the same part of the
    // burst on the late row as on the early one.
    const size_t shift =
        is_625_line(vp.system)
            ? static_cast<size_t>(pal_line_locked::row_0h_shift(
                  static_cast<int>(field_idx) + 1,
                  static_cast<int32_t>(field_line)))
            : 0;
    const size_t burst_start = burst_start_idx + shift;
    size_t burst_end = burst_end_idx + shift;
    if (burst_end >= line_width) {
      burst_end = line_width - 1;
    }
    if (burst_end <= burst_start) {
      return std::nullopt;
    }

    const size_t n_samples = burst_end - burst_start + 1;
    if (n_samples < 4) {
      return std::nullopt;
    }

    double sum = 0.0;
    for (size_t i = burst_start; i <= burst_end; ++i) {
      sum += static_cast<double>(line_data[i]);
    }
    const double mean = sum / static_cast<double>(n_samples);

    double sum_sq = 0.0;
    for (size_t i = burst_start; i <= burst_end; ++i) {
      const double d = static_cast<double>(line_data[i]) - mean;
      sum_sq += d * d;
    }
    const double rms = std::sqrt(sum_sq / static_cast<double>(n_samples));
    const double peak_amplitude = rms * k_sqrt2;

    if (peak_amplitude > outlier_threshold_10bit) {
      return std::nullopt;
    }
    return peak_amplitude;
  };

  for (size_t field_idx = 0; field_idx < 2; ++field_idx) {
    size_t line_offset = (field_idx == 0) ? 0 : f1_lines;
    size_t field_height = (field_idx == 0)
                              ? f1_lines
                              : static_cast<size_t>(vp.frame_height) - f1_lines;

    // SECAM blanks its sub-carrier through field blanking except on the
    // identification lines, which a source need not carry, so it starts at
    // the first picture line of the field (broadcast lines 23 / 336).
    size_t start_line = is_secam ? 22 : 11;
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
      std::optional<double> level =
          line_peak(field_idx, line_offset, field_line);
      if (level && is_secam) {
        // Adjacent lines carry D'R and D'B, whose references differ in
        // level (see SECAM above), so a reading is the mean of the pair.
        const std::optional<double> next =
            line_peak(field_idx, line_offset, field_line + 1);
        level =
            next ? std::optional<double>((*level + *next) / 2.0) : std::nullopt;
      }
      if (level) burst_levels_raw[burst_sample_count++] = *level;
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
