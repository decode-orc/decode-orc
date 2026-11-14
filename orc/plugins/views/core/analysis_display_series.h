/*
 * File:        analysis_display_series.h
 * Module:      orc-view_core
 * Purpose:     Decimated analysis display series for the analysis sinks'
 *              graphs, made from the per-frame records their stages send
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "../../stages/common/analysis-results/analysis_series_wire.h"

namespace orc::analysis_view {

/**
 * @brief Frame range and provenance shared by every decimated display point.
 *
 * When the analysed series is longer than the graph's display budget each point
 * aggregates a contiguous run of analysed frames [frame_start, frame_end]; when
 * it is shorter every point maps to exactly one analysed frame
 * (frame_start == frame_end, contributing_frames == 1). `frame_label` is the
 * representative frame plotted on the x-axis.
 */
struct AnalysisDisplayBucket {
  int32_t frame_start = 0;  ///< First analysed frame in the bucket (1-based)
  int32_t frame_end = 0;    ///< Last analysed frame in the bucket (1-based)
  int32_t frame_label = 0;  ///< Representative frame plotted on the x-axis
  int32_t contributing_frames =
      0;                  ///< Analysed frames aggregated into the bucket
  bool has_data = false;  ///< True if any contributing frame carried data
};

/**
 * @brief One decimated dropout display point. Counts and lengths are summed
 * across the bucket's contributing frames.
 */
struct DropoutDisplayPoint {
  AnalysisDisplayBucket bucket;
  int64_t dropout_length_samples = 0;  ///< Summed dropout length (samples)
  int32_t dropout_count = 0;           ///< Summed dropout run count
};

/**
 * @brief One decimated SNR display point. Level metrics are averaged across the
 * bucket's contributing frames.
 */
struct SNRDisplayPoint {
  AnalysisDisplayBucket bucket;
  bool has_white_snr = false;   ///< True if white_snr is meaningful
  double white_snr = 0.0;       ///< Mean white SNR across the bucket (dB)
  bool has_black_psnr = false;  ///< True if black_psnr is meaningful
  double black_psnr = 0.0;      ///< Mean black PSNR across the bucket (dB)
};

/**
 * @brief One decimated burst-level display point. The value is averaged across
 * the bucket's contributing frames.
 */
struct BurstLevelDisplayPoint {
  AnalysisDisplayBucket bucket;
  double median_burst_10bit =
      0.0;  ///< Mean median burst amplitude (10-bit units)
};

/**
 * @brief A decimated analysis series ready for a graph dialog.
 *
 * `total_frames` is the total frame count of the analysed source (used for the
 * x-axis extent and the frame marker); `decimated` is true when at least one
 * point aggregates more than one analysed frame, so the dialog can label the
 * view as bucketed rather than per-frame.
 */
template <typename PointT>
struct AnalysisDisplaySeries {
  std::vector<PointT> points;  ///< Decimated display points (≤ display budget)
  int32_t total_frames = 0;    ///< Total frames in the analysed source
  bool decimated = false;      ///< True when any point covers >1 analysed frame
};

using DropoutDisplaySeries = AnalysisDisplaySeries<DropoutDisplayPoint>;
using SNRDisplaySeries = AnalysisDisplaySeries<SNRDisplayPoint>;
using BurstLevelDisplaySeries = AnalysisDisplaySeries<BurstLevelDisplayPoint>;

/// Display points a graph is given at most: one per pixel column of a wide
/// window, far fewer than a disc side's frames.
constexpr std::size_t kDisplayPoints = 1000;

/// Reduce the per-frame dropout records to at most @p max_points buckets.
/// Counts and lengths are summed within a bucket, which is labelled with its
/// last frame.
DropoutDisplaySeries make_dropout_series(
    const std::vector<analysis_wire::DropoutFrame>& frames,
    int32_t total_frames, std::size_t max_points = kDisplayPoints);

/// Reduce the per-frame SNR records to at most @p max_points buckets,
/// averaging each metric and labelling a bucket with its centre frame.
SNRDisplaySeries make_snr_series(
    const std::vector<analysis_wire::SnrFrame>& frames, int32_t total_frames,
    std::size_t max_points = kDisplayPoints);

/// Reduce the per-frame burst records to at most @p max_points buckets,
/// averaging within each and labelling it with its centre frame.
BurstLevelDisplaySeries make_burst_level_series(
    const std::vector<analysis_wire::BurstLevelFrame>& frames,
    int32_t total_frames, std::size_t max_points = kDisplayPoints);

}  // namespace orc::analysis_view
