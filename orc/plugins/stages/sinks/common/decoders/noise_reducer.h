/*
 * File:        noise_reducer.h
 * Module:      orc-core
 * Purpose:     Noise reduction for decoded component frames: a
 *              motion-adaptive recursive temporal filter, with a spatial
 *              filter where the picture does not continue
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef NOISE_REDUCER_H
#define NOISE_REDUCER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "componentframe.h"

// Noise reducer for the chroma decoders' component output.
//
// Each incoming frame is blended towards the previous *filtered* frame
// wherever the picture is still, and filtered spatially wherever it is not:
//
//   out = in + w * (previous - in) + m * (spatial(in) - in)
//
// where m in [0, 1] is the per-sample motion measure, w = alpha * (1 - m) is
// the temporal blend (alpha derived from the strength in dB), and spatial()
// is a small range-weighted window over the incoming frame whose strength is
// matched to alpha. Still picture therefore averages over time and keeps its
// detail; moving picture, and any frame that does not continue the previous
// one, is smoothed within the frame instead, so no part of the output is
// left at the input noise level.
//
// Motion is measured per plane and per sample as the magnitude of the signed
// frame difference after a box mean over four samples by three same-field
// lines; four samples is one subcarrier cycle at 4fsc, so residual
// subcarrier in Y does not register as motion. Before anything is blended a
// pre-pass over a subsample of the picture measures three things from the
// same box:
//
// - the temporal noise scale of each plane: a low percentile of the box-mean
//   frame difference, which on still noisy picture is pure noise;
// - a within-frame anchor for it: the same percentile of the same box
//   operator applied between the incoming frame and itself shifted by two
//   subcarrier cycles, which on noise has the same distribution as the
//   frame difference however the decoder has filtered the noise, but cannot
//   be inflated by a change of picture;
// - the noise scale of the differences between a sample and its spatial
//   window neighbours, per plane, which sets the spatial filter's range.
//
// A frame is a discontinuity when its temporal noise scale reaches the
// clamp (no decoder output is that noisy), when its temporal percentile
// exceeds the anchor by more than a fixed ratio (the frame-to-frame change
// is larger than noise over most of the picture: a cut, or an unrelated
// still such as a slide), or when more than half of the picture is moving
// (a pan or a large fast movement, which averaging over time cannot help).
// A discontinuity is filtered spatially only and becomes the new previous
// frame, and its temporal measurement is never used, so a run of unrelated
// stills cannot loosen the thresholds the way it would if each were measured
// as a noisy continuation of the last. The first frame of a sequence and a
// frame whose geometry differs from the previous one are discontinuities
// too.
//
// The filter is applied in place and keeps its own copy of the result (in
// single precision, which is far finer than the output's 16-bit
// quantisation) as the next previous frame, so it must see frames strictly
// in presentation order. The design is described in
// docs-tech/chroma-decoder-denoise-plan.md.
//
// Thread safety: an instance is not thread-safe. One instance per output
// sequence, driven from one thread at a time. With Settings::threads above 1
// filter() splits the active lines into bands and runs them on a private
// worker pool; every row is measured by the same arithmetic whichever band
// holds it, so the band split changes nothing in the output.
class NoiseReducer {
 public:
  struct Settings {
    // Noise reduction on still picture in dB. 0 (or less) disables the
    // filter: filter() returns without touching the frame or the state. The
    // spatial filter is matched to the same figure, as far as its window
    // allows.
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
    // Whether the frame was changed at all. False when the filter is
    // disabled or the active rectangle is empty.
    bool filtered = false;

    // Whether the frame was blended towards the previous frame.
    bool temporal = false;

    // Whether the frame was judged not to continue the previous one (first
    // frame, geometry change, scene change, unrelated still, or mostly
    // moving) and so was filtered spatially only.
    bool discontinuity = false;

    // Noise scale of each plane's box-mean frame difference, in sample
    // codes, after clamping: the scales the motion measure used. Zero on a
    // discontinuity (and, for U and V, on frames without chroma).
    double sigma_y = 0.0;
    double sigma_u = 0.0;
    double sigma_v = 0.0;

    // Per-sample noise of each plane, in sample codes, from the differences
    // between neighbouring samples of the frame (the scale the spatial
    // filter's range follows, divided by sqrt 2).
    double sample_sigma_y = 0.0;
    double sample_sigma_u = 0.0;
    double sample_sigma_v = 0.0;

    // Fraction of the measured samples whose motion measure exceeded the
    // moving threshold. Zero when the frame had no previous frame to measure
    // against.
    double moving_fraction = 0.0;

    // Ratio of the luma temporal percentile to its within-frame anchor, the
    // measure behind the discontinuity decision. Zero when the frame had no
    // previous frame to measure against.
    double scale_ratio = 0.0;
  };

  explicit NoiseReducer(const Settings& settings);
  ~NoiseReducer();
  NoiseReducer(const NoiseReducer&) = delete;
  NoiseReducer& operator=(const NoiseReducer&) = delete;

  // Forget the previous frame. The next frame is a discontinuity: filtered
  // spatially only, and kept as the new previous frame.
  void reset();

  // Filter frame in place and keep the result as the next previous frame.
  // A frame whose geometry (width, height, presence of chroma) differs from
  // the previous frame resets the filter first.
  void filter(ComponentFrame& frame);

  const Settings& settings() const { return settings_; }
  const Stats& stats() const { return stats_; }

  // Steady-state blend weight for a still-picture noise reduction of
  // strength_db. With r = 10^(-dB/10) the recursion's noise power ratio
  // (1 - w) / (1 + w) equals r when w = (1 - r) / (1 + r).
  static double blend_weight_for_strength_db(double strength_db);

  // Share of the spatial window mean mixed into a sample so that the
  // window's noise power reduction on flat picture matches strength_db, as
  // far as the window's effective tap count allows (the share saturates at
  // 1).
  static double spatial_gain_for_strength_db(double strength_db);

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
  // Rows beyond its own that the spatial window reaches on each side: the
  // same-field lines above and below.
  static constexpr int32_t kSpatialRowReach = kSameFieldRowStep;
  // Samples on each side of a sample in the spatial window.
  static constexpr int32_t kSpatialSampleReach = 2;
  // A row with at least one moving sample in this many is filtered as a
  // whole, vectorised; a row with fewer is filtered sample by sample.
  static constexpr int32_t kSpatialRowShare = 8;
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

  // What the pre-pass measured on the incoming frame.
  struct Measurement {
    // Temporal box noise scale per plane, clamped. Only meaningful when the
    // frame had a previous frame to measure against.
    std::array<double, kPlaneCount> sigma = {};
    // Noise scale of the differences between a sample and its spatial
    // window neighbours, per plane, clamped.
    std::array<double, kPlaneCount> difference_sigma = {};
    // Unclamped luma percentiles behind the discontinuity decision.
    double temporal_percentile_y = 0.0;
    double anchor_percentile_y = 0.0;
    double moving_fraction = 0.0;
  };

  // Per-band scratch, reused across frames.
  struct PlaneScratch {
    std::vector<float> diff_row;
    // Four-sample running sums of the signed difference for the band's rows
    // and kChunkHalo rows on each side of them.
    std::vector<float> row_sums;
    std::vector<float> motion;   // one chunk
    std::vector<float> spatial;  // one chunk of spatially filtered samples
    // Unfiltered copies of the kSpatialRowReach rows above the current chunk
    // and below the band, which the frame no longer holds (or another band
    // may already have blended) when the chunk's spatial window needs them.
    std::vector<double> raw_above;
    std::vector<double> raw_below;
    std::vector<double> raw_swap;
  };
  struct BandScratch {
    std::array<PlaneScratch, kPlaneCount> planes;
    std::vector<float> mix;  // one chunk of motion measures m
    // The spatial window's three rows in single precision, and its row
    // accumulators.
    std::vector<float> window_rows;
    std::vector<float> sum_weights;
    std::vector<float> sum_values;
  };

  // Private worker pool for the line bands; see noise_reducer.cpp.
  class BandPool;

  // The band task receives its band index and its row range within the
  // active rectangle.
  using BandTask = std::function<void(int32_t, int32_t, int32_t)>;

  ActiveRect clamped_active_rect(const ComponentFrame& frame) const;
  bool geometry_matches(const ComponentFrame& frame) const;

  // Size previous_ for frame's geometry without filling it; the pass writes
  // every active sample before it is read again.
  void prepare_previous(const ComponentFrame& frame);

  // Number of bands the active lines are split into for this frame.
  int32_t band_count(int32_t lines) const;

  // Run task over the active lines split into bands, band 0 on the calling
  // thread and the rest on the pool, and wait for all of them.
  void run_bands(int32_t lines, int32_t bands, const BandTask& task);

  // Pre-pass over a subsample of the active rectangle filling measurement_.
  // With continuing set the frame is also measured against previous_.
  void measure(const ComponentFrame& frame, const ActiveRect& rect,
               bool has_chroma, bool continuing);

  // One pass over the active rectangle in bands and chunks. With temporal
  // set, measure every plane's box-mean difference against previous_ and
  // blend the frame towards previous_ where it is still and towards its
  // spatial filter where it is moving; otherwise filter every sample
  // spatially. Either way the result becomes previous_.
  void frame_pass(ComponentFrame& frame, const ActiveRect& rect, int32_t bands,
                  bool has_chroma, bool temporal);

  Settings settings_;
  Stats stats_;

  bool has_previous_ = false;
  PreviousFrame previous_;

  Measurement measurement_;
  std::array<std::vector<uint32_t>, kPlaneCount> temporal_histogram_;
  std::array<std::vector<uint32_t>, kPlaneCount> horizontal_histogram_;
  std::array<std::vector<uint32_t>, kPlaneCount> vertical_histogram_;
  std::vector<uint32_t> anchor_histogram_;
  std::vector<float> measure_scratch_;

  std::vector<BandScratch> bands_;
  std::unique_ptr<BandPool> pool_;
};

#endif  // NOISE_REDUCER_H
