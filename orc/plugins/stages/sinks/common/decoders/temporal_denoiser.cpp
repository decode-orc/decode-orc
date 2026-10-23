/*
 * File:        temporal_denoiser.cpp
 * Module:      orc-core
 * Purpose:     Motion-adaptive recursive temporal noise reduction for
 *              decoded component frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "temporal_denoiser.h"

#include <algorithm>
#include <cmath>

namespace {

// Motion thresholds in units of each plane's estimated noise scale. Below the
// still threshold the full blend applies; above the moving threshold the
// sample passes through; between them the weight falls along a smoothstep.
constexpr double kStillThresholdScale = 2.5;
constexpr double kMovingThresholdScale = 5.0;

// The noise scale is read off the box-mean difference's distribution at this
// percentile. On still noisy picture the box-mean difference is close to a
// zero-mean normal, so its magnitude is half-normal and the percentile
// relates to the normal's scale by a constant: for the 30th percentile,
// q = sigma * Phi^-1(0.65).
constexpr double kNoisePercentile = 0.30;
constexpr double kHalfNormalPercentileToScale = 0.38532;

// Histogram of the box-mean difference used for the percentile: 256 bins of a
// quarter of a sample code, covering 0 to 64 codes. Larger values land in
// the top bin.
constexpr int32_t kHistogramBins = 256;
constexpr double kHistogramBinWidth = 0.25;
constexpr double kHistogramBinsPerCode = 1.0 / kHistogramBinWidth;

// Clamp on the estimated noise scale, in sample codes.
constexpr double kSigmaMin = 0.25;
constexpr double kSigmaMax = 8.0;

// When more than this fraction of the active samples are moving the frame is
// treated as a scene cut and passed through unchanged.
constexpr double kCutGuardFraction = 0.5;

double smoothstep(double edge0, double edge1, double x) {
  if (edge1 <= edge0) return x < edge0 ? 0.0 : 1.0;
  const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

}  // namespace

TemporalDenoiser::TemporalDenoiser(const Settings& settings)
    : settings_(settings) {}

double TemporalDenoiser::blend_weight_for_strength_db(double strength_db) {
  if (strength_db <= 0.0) return 0.0;
  const double r = std::pow(10.0, -strength_db / 10.0);
  return (1.0 - r) / (1.0 + r);
}

void TemporalDenoiser::reset() {
  has_previous_ = false;
  previous_has_chroma_ = false;
}

TemporalDenoiser::ActiveRect TemporalDenoiser::clamped_active_rect(
    const ComponentFrame& frame) const {
  ActiveRect rect;
  rect.x_start = std::clamp(settings_.active_x_start, 0, frame.getWidth());
  rect.x_end = std::clamp(settings_.active_x_end, 0, frame.getWidth());
  rect.line_start =
      std::clamp(settings_.active_line_start, 0, frame.getHeight());
  rect.line_end = std::clamp(settings_.active_line_end, 0, frame.getHeight());
  return rect;
}

bool TemporalDenoiser::geometry_matches(const ComponentFrame& frame) const {
  if (frame.getWidth() != previous_.getWidth()) return false;
  if (frame.getHeight() != previous_.getHeight()) return false;
  if (frame.getY()->size() != previous_.getY()->size()) return false;
  const bool frame_has_chroma = !frame.getU()->empty();
  return frame_has_chroma == previous_has_chroma_;
}

void TemporalDenoiser::pass_through(const ComponentFrame& frame) {
  previous_ = frame;
  previous_has_chroma_ = !frame.getU()->empty();
  has_previous_ = true;
  stats_.filtered = false;
}

void TemporalDenoiser::box_difference(const std::vector<double>& current,
                                      const std::vector<double>& previous,
                                      int32_t frame_width,
                                      const ActiveRect& rect) {
  const int32_t width = rect.width();
  const int32_t lines = rect.lines();
  row_sums_.resize(static_cast<size_t>(width) * lines);
  diff_row_.resize(width);
  plane_motion_.resize(static_cast<size_t>(width) * lines);

  // Horizontal pass: running sum of the signed difference over a window of
  // kBoxWidth consecutive samples. The window is centred on x - 1 .. x + 2
  // and slides inward at the edges rather than shrinking, so every sample is
  // measured over one full subcarrier cycle and a 4fsc residual cancels
  // everywhere.
  const int32_t last_window_start = std::max(0, width - kBoxWidth);
  for (int32_t r = 0; r < lines; ++r) {
    const size_t row_offset =
        static_cast<size_t>(rect.line_start + r) * frame_width + rect.x_start;
    const double* cur = current.data() + row_offset;
    const double* prev = previous.data() + row_offset;
    for (int32_t x = 0; x < width; ++x) diff_row_[x] = cur[x] - prev[x];

    double* sums = row_sums_.data() + static_cast<size_t>(r) * width;
    int32_t window_start = 0;
    double sum = 0.0;
    for (int32_t i = 0; i < std::min(kBoxWidth, width); ++i) {
      sum += diff_row_[i];
    }
    for (int32_t x = 0; x < width; ++x) {
      const int32_t wanted_start = std::clamp(x - 1, 0, last_window_start);
      while (window_start < wanted_start) {
        sum += diff_row_[window_start + kBoxWidth] - diff_row_[window_start];
        ++window_start;
      }
      sums[x] = sum;
    }
  }

  // Vertical pass: kBoxLines same-field lines (kSameFieldRowStep rows apart),
  // the window sliding inward at the top and bottom of the active area, then
  // the magnitude of the box mean.
  const int32_t box_count = std::min(kBoxWidth, width) * kBoxLines;
  const double inverse_area = 1.0 / static_cast<double>(box_count);
  const int32_t reach = kSameFieldRowStep * (kBoxLines / 2);
  const bool can_slide = lines > 2 * reach;
  for (int32_t r = 0; r < lines; ++r) {
    int32_t centre = r;
    if (can_slide) centre = std::clamp(r, reach, lines - 1 - reach);
    const int32_t above = std::max(0, centre - kSameFieldRowStep);
    const int32_t below = std::min(lines - 1, centre + kSameFieldRowStep);
    const double* s_above =
        row_sums_.data() + static_cast<size_t>(above) * width;
    const double* s_centre =
        row_sums_.data() + static_cast<size_t>(centre) * width;
    const double* s_below =
        row_sums_.data() + static_cast<size_t>(below) * width;
    double* motion = plane_motion_.data() + static_cast<size_t>(r) * width;
    for (int32_t x = 0; x < width; ++x) {
      motion[x] =
          inverse_area * std::fabs(s_above[x] + s_centre[x] + s_below[x]);
    }
  }
}

double TemporalDenoiser::estimate_sigma(size_t count) {
  histogram_.assign(kHistogramBins, 0);
  for (size_t i = 0; i < count; ++i) {
    const int32_t bin = std::min(
        kHistogramBins - 1,
        static_cast<int32_t>(plane_motion_[i] * kHistogramBinsPerCode));
    ++histogram_[bin];
  }

  // Walk to the bin holding the percentile and interpolate within it.
  const double target = kNoisePercentile * static_cast<double>(count);
  double cumulative = 0.0;
  double percentile_value = kHistogramBins * kHistogramBinWidth;
  for (int32_t bin = 0; bin < kHistogramBins; ++bin) {
    const double in_bin = histogram_[bin];
    if (cumulative + in_bin >= target) {
      const double fraction =
          in_bin > 0.0 ? (target - cumulative) / in_bin : 0.0;
      percentile_value = (bin + fraction) * kHistogramBinWidth;
      break;
    }
    cumulative += in_bin;
  }

  const double sigma = percentile_value / kHalfNormalPercentileToScale;
  return std::clamp(sigma, kSigmaMin, kSigmaMax);
}

double TemporalDenoiser::measure_plane(const std::vector<double>& current,
                                       const std::vector<double>& previous,
                                       int32_t frame_width,
                                       const ActiveRect& rect) {
  box_difference(current, previous, frame_width, rect);
  const size_t count = static_cast<size_t>(rect.width()) * rect.lines();
  const double sigma = estimate_sigma(count);
  const double inverse_sigma = 1.0 / sigma;
  for (size_t i = 0; i < count; ++i) {
    motion_[i] = std::max(motion_[i], plane_motion_[i] * inverse_sigma);
  }
  return sigma;
}

void TemporalDenoiser::filter(ComponentFrame& frame) {
  if (settings_.strength_db <= 0.0) return;

  stats_ = Stats{};
  const ActiveRect rect = clamped_active_rect(frame);
  if (!has_previous_ || rect.empty() || !geometry_matches(frame)) {
    pass_through(frame);
    return;
  }

  const bool has_chroma = !frame.getU()->empty() && !frame.getV()->empty();
  const int32_t frame_width = frame.getWidth();
  const int32_t width = rect.width();
  const int32_t lines = rect.lines();
  const size_t count = static_cast<size_t>(width) * lines;

  // Motion in units of each plane's own noise scale, the largest plane
  // winning, so a change in any plane counts and a noisier plane does not
  // dominate the measure.
  motion_.assign(count, 0.0);
  stats_.sigma_y =
      measure_plane(*frame.getY(), *previous_.getY(), frame_width, rect);
  if (has_chroma) {
    stats_.sigma_u =
        measure_plane(*frame.getU(), *previous_.getU(), frame_width, rect);
    stats_.sigma_v =
        measure_plane(*frame.getV(), *previous_.getV(), frame_width, rect);
  }

  const double still_threshold = kStillThresholdScale * settings_.motion_scale;
  const double moving_threshold =
      kMovingThresholdScale * settings_.motion_scale;

  size_t moving = 0;
  for (size_t i = 0; i < count; ++i) {
    if (motion_[i] > moving_threshold) ++moving;
  }
  stats_.moving_fraction =
      static_cast<double>(moving) / static_cast<double>(count);

  if (stats_.moving_fraction > kCutGuardFraction) {
    pass_through(frame);
    stats_.cut_detected = true;
    return;
  }

  const double alpha = blend_weight_for_strength_db(settings_.strength_db);
  for (int32_t r = 0; r < lines; ++r) {
    const int32_t row = rect.line_start + r;
    const double* motion = motion_.data() + static_cast<size_t>(r) * width;
    double* y = frame.y(row) + rect.x_start;
    double* py = previous_.y(row) + rect.x_start;
    double* u = has_chroma ? frame.u(row) + rect.x_start : nullptr;
    double* pu = has_chroma ? previous_.u(row) + rect.x_start : nullptr;
    double* v = has_chroma ? frame.v(row) + rect.x_start : nullptr;
    double* pv = has_chroma ? previous_.v(row) + rect.x_start : nullptr;
    for (int32_t x = 0; x < width; ++x) {
      const double w = alpha * (1.0 - smoothstep(still_threshold,
                                                 moving_threshold, motion[x]));
      y[x] += w * (py[x] - y[x]);
      py[x] = y[x];
      if (has_chroma) {
        u[x] += w * (pu[x] - u[x]);
        pu[x] = u[x];
        v[x] += w * (pv[x] - v[x]);
        pv[x] = v[x];
      }
    }
  }
  stats_.filtered = true;
}
