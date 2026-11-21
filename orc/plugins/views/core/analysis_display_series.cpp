/*
 * File:        analysis_display_series.cpp
 * Module:      orc-view_core
 * Purpose:     Decimate the analysis sinks' per-frame records for their graphs
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "analysis_display_series.h"

#include <orc/view-support/analysis_series_decimator.h>

#include <cmath>

namespace orc::analysis_view {

namespace {

using orc::view::decimate_series;
using orc::view::DecimatedBucket;
using orc::view::SeriesPoint;

// Representative frame number for a display bucket: its centre frame, matching
// the previous SNR/burst graph labelling.
int32_t bucket_centre(int32_t frame_start, int32_t frame_end) {
  return frame_start + (frame_end - frame_start) / 2;
}

// Fill the frame-range/provenance fields shared by every display point from a
// decimated bucket. `frame_label` is the x-axis position for the point.
AnalysisDisplayBucket make_bucket(const DecimatedBucket& b,
                                  int32_t frame_label) {
  AnalysisDisplayBucket out;
  out.frame_start = b.frame_start;
  out.frame_end = b.frame_end;
  out.frame_label = frame_label;
  // Bucket width: all analysed frames spanned, including any without a value.
  out.contributing_frames = static_cast<int32_t>(b.record_count);
  out.has_data = b.value_count > 0;
  return out;
}

// True when at least one point aggregates more than one analysed frame, i.e.
// the display is bucketed rather than one-point-per-frame.
template <typename PointT>
bool series_is_decimated(const std::vector<PointT>& points) {
  for (const auto& p : points) {
    if (p.bucket.frame_end > p.bucket.frame_start) {
      return true;
    }
  }
  return false;
}

// Reduce the full-resolution per-frame dropout series to at most `max_points`
// display points. Counts and lengths are summed within each bucket (the graph
// shows dropout activity per bucket); the bucket is labelled with its last
// frame, matching the previous graph labelling.
std::vector<DropoutDisplayPoint> decimate_dropout_series(
    const std::vector<analysis_wire::DropoutFrame>& full,
    std::size_t max_points) {
  std::vector<SeriesPoint> length_series;
  std::vector<SeriesPoint> count_series;
  length_series.reserve(full.size());
  count_series.reserve(full.size());
  for (const auto& fs : full) {
    length_series.push_back(SeriesPoint{
        fs.frame_number, static_cast<double>(fs.dropout_length_samples),
        fs.has_data});
    count_series.push_back(SeriesPoint{
        fs.frame_number, static_cast<double>(fs.dropout_count), fs.has_data});
  }

  const auto len_buckets = decimate_series(length_series, max_points);
  const auto cnt_buckets = decimate_series(count_series, max_points);

  std::vector<DropoutDisplayPoint> out;
  out.reserve(len_buckets.size());
  for (std::size_t i = 0; i < len_buckets.size(); ++i) {
    DropoutDisplayPoint d;
    d.bucket = make_bucket(len_buckets[i], len_buckets[i].frame_end);
    d.dropout_length_samples =
        static_cast<int64_t>(std::llround(len_buckets[i].sum));
    d.dropout_count = static_cast<int32_t>(std::llround(cnt_buckets[i].sum));
    out.push_back(d);
  }
  return out;
}

// Reduce the full-resolution per-frame SNR series to at most `max_points`
// display points, averaging each metric within a bucket and labelling the
// bucket with its centre frame.
std::vector<SNRDisplayPoint> decimate_snr_series(
    const std::vector<analysis_wire::SnrFrame>& full, std::size_t max_points) {
  std::vector<SeriesPoint> white_series;
  std::vector<SeriesPoint> black_series;
  white_series.reserve(full.size());
  black_series.reserve(full.size());
  for (const auto& fs : full) {
    white_series.push_back(
        SeriesPoint{fs.frame_number, fs.white_snr, fs.has_white_snr});
    black_series.push_back(
        SeriesPoint{fs.frame_number, fs.black_psnr, fs.has_black_psnr});
  }

  const auto white_buckets = decimate_series(white_series, max_points);
  const auto black_buckets = decimate_series(black_series, max_points);

  std::vector<SNRDisplayPoint> out;
  out.reserve(white_buckets.size());
  for (std::size_t i = 0; i < white_buckets.size(); ++i) {
    SNRDisplayPoint d;
    const int32_t label =
        bucket_centre(white_buckets[i].frame_start, white_buckets[i].frame_end);
    d.bucket = make_bucket(white_buckets[i], label);
    d.has_white_snr = white_buckets[i].value_count > 0;
    d.white_snr = white_buckets[i].mean;
    d.has_black_psnr = black_buckets[i].value_count > 0;
    d.black_psnr = black_buckets[i].mean;
    // A point carries data if either metric contributed a value.
    d.bucket.has_data = d.has_white_snr || d.has_black_psnr;
    out.push_back(d);
  }
  return out;
}

// Reduce the full-resolution per-frame burst-level series to at most
// `max_points` display points, averaging within a bucket.
std::vector<BurstLevelDisplayPoint> decimate_burst_series(
    const std::vector<analysis_wire::BurstLevelFrame>& full,
    std::size_t max_points) {
  std::vector<SeriesPoint> series;
  series.reserve(full.size());
  for (const auto& fs : full) {
    series.push_back(
        SeriesPoint{fs.frame_number, fs.median_burst_10bit, fs.has_data});
  }

  const auto buckets = decimate_series(series, max_points);

  std::vector<BurstLevelDisplayPoint> out;
  out.reserve(buckets.size());
  for (const auto& b : buckets) {
    BurstLevelDisplayPoint d;
    d.bucket = make_bucket(b, bucket_centre(b.frame_start, b.frame_end));
    d.median_burst_10bit = b.mean;
    out.push_back(d);
  }
  return out;
}

}  // namespace

DropoutDisplaySeries make_dropout_series(
    const std::vector<analysis_wire::DropoutFrame>& frames,
    int32_t total_frames, std::size_t max_points) {
  DropoutDisplaySeries series;
  series.points = decimate_dropout_series(frames, max_points);
  series.total_frames = total_frames;
  series.decimated = series_is_decimated(series.points);
  return series;
}

SNRDisplaySeries make_snr_series(
    const std::vector<analysis_wire::SnrFrame>& frames, int32_t total_frames,
    std::size_t max_points) {
  SNRDisplaySeries series;
  series.points = decimate_snr_series(frames, max_points);
  series.total_frames = total_frames;
  series.decimated = series_is_decimated(series.points);
  return series;
}

BurstLevelDisplaySeries make_burst_level_series(
    const std::vector<analysis_wire::BurstLevelFrame>& frames,
    int32_t total_frames, std::size_t max_points) {
  BurstLevelDisplaySeries series;
  series.points = decimate_burst_series(frames, max_points);
  series.total_frames = total_frames;
  series.decimated = series_is_decimated(series.points);
  return series;
}

}  // namespace orc::analysis_view
