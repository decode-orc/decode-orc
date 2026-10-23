/*
 * File:        temporal_denoiser.h
 * Module:      orc-core
 * Purpose:     Motion-adaptive recursive temporal noise reduction for
 *              decoded component frames
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef TEMPORAL_DENOISER_H
#define TEMPORAL_DENOISER_H

#include <cstdint>
#include <vector>

#include "componentframe.h"

// Recursive temporal noise reducer for the chroma decoders' component output.
//
// Each incoming frame is blended towards the previous *filtered* frame:
//
//   out = in + w * (previous - in)
//
// where w is the steady-state blend (derived from the strength in dB) scaled
// down to zero wherever the picture is judged to be moving. Motion is measured
// per plane and per sample as the magnitude of the signed frame difference
// after a box mean over four samples by three same-field lines; four samples
// is one subcarrier cycle at 4fsc, so residual subcarrier in Y does not
// register as motion. Each plane's noise level is estimated from the frame
// itself (a low percentile of its box-mean difference), the plane's motion is
// expressed in units of that noise, and the largest of the three planes
// drives the weight, so the motion thresholds follow the material and a
// chroma-only change still counts as motion.
//
// The filter is applied in place and keeps its own copy of the result as the
// next previous frame, so it must see frames strictly in presentation order.
// The design is described in docs-tech/chroma-decoder-denoise-plan.md.
//
// Thread safety: an instance is not thread-safe. One instance per output
// sequence, driven from one thread at a time.
class TemporalDenoiser {
 public:
  struct Settings {
    // Noise reduction on still picture in dB. 0 (or less) disables the
    // filter: filter() returns without touching the frame or the state.
    double strength_db = 0.0;

    // Multiplier on the motion thresholds. Above 1 treats more change as
    // still picture (more smoothing, more risk of trails), below 1 the
    // reverse.
    double motion_scale = 1.0;

    // Active picture rectangle in frame-flat coordinates of the frames
    // passed to filter(): samples [active_x_start, active_x_end) on lines
    // [active_line_start, active_line_end). Only this region is measured and
    // filtered. The rectangle is clamped to the frame.
    int32_t active_x_start = 0;
    int32_t active_x_end = 0;
    int32_t active_line_start = 0;
    int32_t active_line_end = 0;
  };

  // Diagnostics for the most recent filter() call.
  struct Stats {
    // Whether the frame was blended with the previous frame. False when the
    // filter is disabled, on the first frame, after a geometry change, and
    // when the cut guard fired.
    bool filtered = false;

    // Whether the cut guard passed the frame through unchanged.
    bool cut_detected = false;

    // Estimated noise scale of each plane's box-mean frame difference, in
    // sample codes, after clamping. Zero when no measurement was made (and,
    // for U and V, on frames without chroma).
    double sigma_y = 0.0;
    double sigma_u = 0.0;
    double sigma_v = 0.0;

    // Fraction of active samples whose motion measure exceeded the moving
    // threshold. Zero when no measurement was made.
    double moving_fraction = 0.0;
  };

  explicit TemporalDenoiser(const Settings& settings);

  // Forget the previous frame. The next frame passes through unchanged and
  // becomes the new previous frame.
  void reset();

  // Filter frame in place and keep the result as the next previous frame.
  // A frame whose geometry (width, height, presence of chroma) differs from
  // the previous frame resets the filter first and passes through.
  void filter(ComponentFrame& frame);

  const Settings& settings() const { return settings_; }
  const Stats& stats() const { return stats_; }

  // Steady-state blend weight for a still-picture noise reduction of
  // strength_db. With r = 10^(-dB/10) the recursion's noise power ratio
  // (1 - w) / (1 + w) equals r when w = (1 - r) / (1 + r).
  static double blend_weight_for_strength_db(double strength_db);

 private:
  // Horizontal box width: one subcarrier cycle at 4fsc.
  static constexpr int32_t kBoxWidth = 4;
  // Vertical box height in same-field lines (rows y - 2, y, y + 2).
  static constexpr int32_t kBoxLines = 3;
  // Frame-flat row distance between vertically adjacent same-field lines.
  static constexpr int32_t kSameFieldRowStep = 2;

  struct ActiveRect {
    int32_t x_start = 0;
    int32_t x_end = 0;
    int32_t line_start = 0;
    int32_t line_end = 0;
    int32_t width() const { return x_end - x_start; }
    int32_t lines() const { return line_end - line_start; }
    bool empty() const { return width() <= 0 || lines() <= 0; }
  };

  ActiveRect clamped_active_rect(const ComponentFrame& frame) const;
  bool geometry_matches(const ComponentFrame& frame) const;

  // Fill plane_motion_ with |box mean of (current - previous)| over rect.
  // current and previous are whole-frame planes.
  void box_difference(const std::vector<double>& current,
                      const std::vector<double>& previous, int32_t frame_width,
                      const ActiveRect& rect);

  // Noise scale estimated from the histogram of the first count entries of
  // plane_motion_, clamped.
  double estimate_sigma(size_t count);

  // box_difference() and estimate_sigma() for one plane, then fold the
  // plane's motion, in units of its noise scale, into motion_ as a maximum.
  // Returns the plane's noise scale.
  double measure_plane(const std::vector<double>& current,
                       const std::vector<double>& previous, int32_t frame_width,
                       const ActiveRect& rect);

  void pass_through(const ComponentFrame& frame);

  Settings settings_;
  Stats stats_;

  bool has_previous_ = false;
  ComponentFrame previous_;
  bool previous_has_chroma_ = false;

  // Scratch buffers sized to the active rectangle, reused across frames.
  std::vector<double> motion_;
  std::vector<double> plane_motion_;
  std::vector<double> row_sums_;
  std::vector<double> diff_row_;
  std::vector<uint32_t> histogram_;
};

#endif  // TEMPORAL_DENOISER_H
