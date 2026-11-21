/*
 * File:        analysis_series_decimator.h
 * Module:      decode-orc Plugin SDK (view-support tier)
 * Purpose:     Reduce a canonical per-frame analysis series to a bounded set of
 *              display buckets for graphing
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * STABILITY: PUBLIC — This header is part of the stable plugin SDK.
 */

#pragma once

// SDK TIER: view-support — see <orc/view-support/view_support_export.h>.

#include <orc/view-support/view_support_export.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace orc::view {

/**
 * @brief One per-frame sample fed into the decimator.
 *
 * A record whose metric is absent (e.g. observation failed for that frame) sets
 * has_value = false: it still occupies a slot in the series (and contributes to
 * a bucket's frame span) but is excluded from the value aggregates.
 */
struct SeriesPoint {
  int32_t frame_number = 0;  ///< Analysed frame number (1-based)
  double value = 0.0;        ///< Metric value for this frame
  bool has_value = false;    ///< True if `value` is meaningful for this frame
};

/**
 * @brief One decimated display bucket covering a contiguous run of records.
 *
 * frame_start / frame_end are the true frame numbers of the first and last
 * records in the bucket. The aggregates cover only the contributing records
 * (has_value == true); when value_count == 0 they are all zero.
 */
struct DecimatedBucket {
  int32_t frame_start = 0;  ///< Frame number of the first record in the bucket
  int32_t frame_end = 0;    ///< Frame number of the last record in the bucket
  std::size_t record_count =
      0;                        ///< Records in the bucket (incl. absent values)
  std::size_t value_count = 0;  ///< Records that contributed a value
  double sum = 0.0;             ///< Sum of contributing values (counts/lengths)
  double mean = 0.0;            ///< Mean of contributing values (level metrics)
  double min = 0.0;             ///< Minimum contributing value (0 if none)
  double max = 0.0;             ///< Maximum contributing value (0 if none)
};

/**
 * @brief Reduce a per-frame series to at most `max_points` contiguous buckets.
 *
 * Buckets are contiguous, equal-width runs of records: bucket width is
 * ceil(series.size() / max_points), so a series no longer than max_points is
 * returned one-record-per-bucket (pass-through). `max_points == 0` is treated
 * as 1. An empty series yields an empty result.
 *
 * Pure: no I/O, no clock, no shared state. O(series.size()).
 */
ORC_VIEW_SUPPORT_EXPORT std::vector<DecimatedBucket> decimate_series(
    const std::vector<SeriesPoint>& series, std::size_t max_points);

}  // namespace orc::view
