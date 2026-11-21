/*
 * File:        analysis_series_decimator.cpp
 * Module:      orc-view-support
 * Purpose:     Reduce a canonical per-frame analysis series to a bounded set of
 *              display buckets for graphing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/view-support/analysis_series_decimator.h>

#include <algorithm>

namespace orc::view {

std::vector<DecimatedBucket> decimate_series(
    const std::vector<SeriesPoint>& series, std::size_t max_points) {
  std::vector<DecimatedBucket> buckets;
  if (series.empty()) return buckets;

  if (max_points == 0) max_points = 1;

  // Contiguous equal-width bins. ceil division so the number of buckets never
  // exceeds max_points; a series no longer than max_points is one-per-bucket.
  const std::size_t n = series.size();
  const std::size_t width = (n + max_points - 1) / max_points;

  buckets.reserve((n + width - 1) / width);

  for (std::size_t start = 0; start < n; start += width) {
    const std::size_t end = std::min(start + width, n);

    DecimatedBucket bucket;
    bucket.frame_start = series[start].frame_number;
    bucket.frame_end = series[end - 1].frame_number;
    bucket.record_count = end - start;

    bool first_value = true;
    for (std::size_t i = start; i < end; ++i) {
      const SeriesPoint& p = series[i];
      if (!p.has_value) continue;
      bucket.sum += p.value;
      ++bucket.value_count;
      if (first_value) {
        bucket.min = p.value;
        bucket.max = p.value;
        first_value = false;
      } else {
        bucket.min = std::min(bucket.min, p.value);
        bucket.max = std::max(bucket.max, p.value);
      }
    }

    if (bucket.value_count > 0) {
      bucket.mean = bucket.sum / static_cast<double>(bucket.value_count);
    }

    buckets.push_back(bucket);
  }

  return buckets;
}

}  // namespace orc::view
