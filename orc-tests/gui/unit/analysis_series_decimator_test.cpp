/*
 * File:        analysis_series_decimator_test.cpp
 * Module:      orc-gui unit tests
 * Purpose:     Unit tests for the shared analysis display-decimation utility
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/view-support/analysis_series_decimator.h>

#include <vector>

namespace {

using orc::view::decimate_series;
using orc::view::DecimatedBucket;
using orc::view::SeriesPoint;

std::vector<SeriesPoint> make_series(int count, double base = 0.0) {
  std::vector<SeriesPoint> s;
  s.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    s.push_back(SeriesPoint{i + 1, base + static_cast<double>(i), true});
  }
  return s;
}

TEST(AnalysisSeriesDecimatorTest, EmptySeriesYieldsEmptyResult) {
  EXPECT_TRUE(decimate_series({}, 1000).empty());
}

TEST(AnalysisSeriesDecimatorTest, SeriesShorterThanMaxIsPassThrough) {
  const auto series = make_series(10);
  const auto buckets = decimate_series(series, 1000);

  ASSERT_EQ(buckets.size(), 10u);
  for (size_t i = 0; i < buckets.size(); ++i) {
    EXPECT_EQ(buckets[i].frame_start, static_cast<int32_t>(i) + 1);
    EXPECT_EQ(buckets[i].frame_end, static_cast<int32_t>(i) + 1);
    EXPECT_EQ(buckets[i].record_count, 1u);
    EXPECT_EQ(buckets[i].value_count, 1u);
    EXPECT_DOUBLE_EQ(buckets[i].sum, static_cast<double>(i));
    EXPECT_DOUBLE_EQ(buckets[i].mean, static_cast<double>(i));
  }
}

TEST(AnalysisSeriesDecimatorTest, ExactMultipleProducesEqualBuckets) {
  const auto series = make_series(100);
  const auto buckets = decimate_series(series, 10);

  ASSERT_EQ(buckets.size(), 10u);
  // width = ceil(100/10) = 10 → every bucket holds exactly 10 records.
  for (const auto& b : buckets) {
    EXPECT_EQ(b.record_count, 10u);
    EXPECT_EQ(b.value_count, 10u);
  }
  EXPECT_EQ(buckets.front().frame_start, 1);
  EXPECT_EQ(buckets.front().frame_end, 10);
  EXPECT_EQ(buckets.back().frame_start, 91);
  EXPECT_EQ(buckets.back().frame_end, 100);
}

TEST(AnalysisSeriesDecimatorTest, RemainderProducesShortFinalBucket) {
  const auto series = make_series(25);
  const auto buckets = decimate_series(series, 10);

  // width = ceil(25/10) = 3 → 9 buckets (3*8=24 records) + 1 record remainder.
  ASSERT_EQ(buckets.size(), 9u);
  for (size_t i = 0; i + 1 < buckets.size(); ++i) {
    EXPECT_EQ(buckets[i].record_count, 3u);
  }
  EXPECT_EQ(buckets.back().record_count, 1u);
  EXPECT_EQ(buckets.back().frame_start, 25);
  EXPECT_EQ(buckets.back().frame_end, 25);
  EXPECT_LE(buckets.size(), 10u);
}

TEST(AnalysisSeriesDecimatorTest, AggregatesAreCorrect) {
  std::vector<SeriesPoint> series{
      SeriesPoint{1, 4.0, true},
      SeriesPoint{2, 8.0, true},
      SeriesPoint{3, 6.0, true},
      SeriesPoint{4, 2.0, true},
  };
  const auto buckets = decimate_series(series, 1);

  ASSERT_EQ(buckets.size(), 1u);
  const auto& b = buckets.front();
  EXPECT_EQ(b.frame_start, 1);
  EXPECT_EQ(b.frame_end, 4);
  EXPECT_EQ(b.record_count, 4u);
  EXPECT_EQ(b.value_count, 4u);
  EXPECT_DOUBLE_EQ(b.sum, 20.0);
  EXPECT_DOUBLE_EQ(b.mean, 5.0);
  EXPECT_DOUBLE_EQ(b.min, 2.0);
  EXPECT_DOUBLE_EQ(b.max, 8.0);
}

TEST(AnalysisSeriesDecimatorTest, AbsentValuesCountedInSpanNotAggregates) {
  std::vector<SeriesPoint> series{
      SeriesPoint{1, 10.0, true},
      SeriesPoint{2, 0.0, false},  // absent metric
      SeriesPoint{3, 20.0, true},
  };
  const auto buckets = decimate_series(series, 1);

  ASSERT_EQ(buckets.size(), 1u);
  const auto& b = buckets.front();
  EXPECT_EQ(b.frame_start, 1);
  EXPECT_EQ(b.frame_end, 3);
  EXPECT_EQ(b.record_count, 3u);
  EXPECT_EQ(b.value_count, 2u);
  EXPECT_DOUBLE_EQ(b.sum, 30.0);
  EXPECT_DOUBLE_EQ(b.mean, 15.0);
  EXPECT_DOUBLE_EQ(b.min, 10.0);
  EXPECT_DOUBLE_EQ(b.max, 20.0);
}

TEST(AnalysisSeriesDecimatorTest, BucketWithNoValuesReportsZeroAggregates) {
  std::vector<SeriesPoint> series{
      SeriesPoint{1, 0.0, false},
      SeriesPoint{2, 0.0, false},
  };
  const auto buckets = decimate_series(series, 1);

  ASSERT_EQ(buckets.size(), 1u);
  const auto& b = buckets.front();
  EXPECT_EQ(b.value_count, 0u);
  EXPECT_DOUBLE_EQ(b.sum, 0.0);
  EXPECT_DOUBLE_EQ(b.mean, 0.0);
  EXPECT_DOUBLE_EQ(b.min, 0.0);
  EXPECT_DOUBLE_EQ(b.max, 0.0);
}

TEST(AnalysisSeriesDecimatorTest, MaxPointsZeroTreatedAsOne) {
  const auto series = make_series(5);
  const auto buckets = decimate_series(series, 0);

  ASSERT_EQ(buckets.size(), 1u);
  EXPECT_EQ(buckets.front().frame_start, 1);
  EXPECT_EQ(buckets.front().frame_end, 5);
  EXPECT_EQ(buckets.front().record_count, 5u);
}

TEST(AnalysisSeriesDecimatorTest, NeverExceedsMaxPoints) {
  const auto series = make_series(9999);
  const auto buckets = decimate_series(series, 1000);
  EXPECT_LE(buckets.size(), 1000u);
}

}  // namespace
