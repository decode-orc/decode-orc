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

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
// difference itself (a low percentile of its box-mean magnitude), the plane's
// motion is expressed in units of that noise, and the largest of the three
// planes drives the weight, so the motion thresholds follow the material and
// a chroma-only change still counts as motion.
//
// The noise scales applied to a frame are the ones measured on the previous
// frame pair, so a frame is measured and blended in a single pass; the first
// filtered frame of a sequence measures itself first. A frame whose motion
// covers more than half the picture (a scene cut) is left to its own
// per-sample weights, which are zero wherever the picture changed, and its
// noise measurement is discarded so a cut cannot loosen the thresholds for
// the frames after it.
//
// The filter is applied in place and keeps its own copy of the result (in
// single precision, which is far finer than the output's 16-bit quantisation)
// as the next previous frame, so it must see frames strictly in presentation
// order. The design is described in docs-tech/chroma-decoder-denoise-plan.md.
//
// Thread safety: an instance is not thread-safe. One instance per output
// sequence, driven from one thread at a time. With Settings::threads above 1
// filter() splits the active lines into bands and runs them on a private
// worker pool; every row is measured by the same arithmetic whichever band
// holds it, so the band split changes nothing in the output.
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

    // Threads filter() may use for its line bands, including the calling
    // thread. Values below 2 run everything on the calling thread.
    int32_t threads = 1;
  };

  // Diagnostics for the most recent filter() call.
  struct Stats {
    // Whether the frame was blended with the previous frame. False when the
    // filter is disabled, on the first frame, after a geometry change, and
    // when the cut guard fired.
    bool filtered = false;

    // Whether the cut guard fired: more than half of the active samples were
    // moving, so the frame's noise measurement was discarded.
    bool cut_detected = false;

    // Noise scale of each plane's box-mean frame difference, in sample
    // codes, after clamping: the scales in force for the next frame. Zero
    // when no measurement has been made (and, for U and V, on frames without
    // chroma).
    double sigma_y = 0.0;
    double sigma_u = 0.0;
    double sigma_v = 0.0;

    // Fraction of active samples whose motion measure exceeded the moving
    // threshold. Zero when no measurement was made.
    double moving_fraction = 0.0;
  };

  explicit TemporalDenoiser(const Settings& settings);
  ~TemporalDenoiser();
  TemporalDenoiser(const TemporalDenoiser&) = delete;
  TemporalDenoiser& operator=(const TemporalDenoiser&) = delete;

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
  // Rows beyond its own that a chunk's vertical box (which slides inward at
  // the top and bottom of the active area) can reach on each side.
  static constexpr int32_t kChunkHalo = 2 * kSameFieldRowStep;
  // Rows measured and blended together so the scratch stays cache-resident.
  static constexpr int32_t kChunkRows = 32;
  // Smallest band worth handing to a thread of its own.
  static constexpr int32_t kMinBandRows = 64;

  enum Plane : size_t { kPlaneY = 0, kPlaneU = 1, kPlaneV = 2, kPlaneCount };

  struct ActiveRect {
    int32_t x_start = 0;
    int32_t x_end = 0;
    int32_t line_start = 0;
    int32_t line_end = 0;
    int32_t width() const { return x_end - x_start; }
    int32_t lines() const { return line_end - line_start; }
    bool empty() const { return width() <= 0 || lines() <= 0; }
  };

  // The previous filtered frame, kept in single precision.
  struct PreviousFrame {
    int32_t width = 0;
    int32_t height = 0;
    bool has_chroma = false;
    std::array<std::vector<float>, kPlaneCount> planes;
  };

  // Per-band scratch, reused across frames.
  struct PlaneScratch {
    std::vector<float> diff_row;
    // Four-sample running sums of the signed difference for the band's rows
    // and kChunkHalo rows on each side of them.
    std::vector<float> row_sums;
    std::vector<float> motion;  // one chunk
    std::vector<uint32_t> histogram;
  };
  struct BandScratch {
    std::array<PlaneScratch, kPlaneCount> planes;
    std::vector<double> weights;  // one row of blend weights
    size_t moving = 0;
  };

  // Private worker pool for the line bands; see temporal_denoiser.cpp.
  class BandPool;

  // The band task receives its band index and its row range within the
  // active rectangle.
  using BandTask = std::function<void(int32_t, int32_t, int32_t)>;

  ActiveRect clamped_active_rect(const ComponentFrame& frame) const;
  bool geometry_matches(const ComponentFrame& frame) const;

  // Number of bands the active lines are split into for this frame.
  int32_t band_count(int32_t lines) const;

  // Run task over the active lines split into bands, band 0 on the calling
  // thread and the rest on the pool, and wait for all of them.
  void run_bands(int32_t lines, int32_t bands, const BandTask& task);

  // One pass over the active rectangle in bands and chunks: measure every
  // plane's box-mean difference against previous_ (filling the band
  // histograms) and, when blend is set, blend the frame towards previous_
  // with the scales in sigma_ (filling the band moving counts).
  void frame_pass(ComponentFrame& frame, const ActiveRect& rect, int32_t bands,
                  bool has_chroma, bool blend);

  // Noise scale from the merged band histograms of one plane, clamped.
  double estimate_sigma(Plane plane, int32_t bands);

  void pass_through(const ComponentFrame& frame);

  Settings settings_;
  Stats stats_;

  bool has_previous_ = false;
  PreviousFrame previous_;

  // Noise scales in force, per plane; measured on the previous frame pair.
  bool has_sigma_ = false;
  std::array<double, kPlaneCount> sigma_ = {};

  std::vector<uint32_t> histogram_;
  std::vector<BandScratch> bands_;
  std::unique_ptr<BandPool> pool_;
};

#endif  // TEMPORAL_DENOISER_H
