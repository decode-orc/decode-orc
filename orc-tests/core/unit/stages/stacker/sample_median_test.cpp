/*
 * File:        sample_median_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the Stacker's sorting-network medians
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/stacker/sample_median.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace orc_unit_test {
namespace {

using orc::kMaxNetworkValues;

// The median as the stacker took it before the networks: an insertion sort,
// the middle two truncated for an even count.
int16_t insertion_median(const std::vector<int16_t>& values) {
  if (values.empty()) return 0;
  std::vector<int16_t> v;
  for (const int16_t value : values) {
    auto at = v.end();
    while (at != v.begin() && *(at - 1) > value) --at;
    v.insert(at, value);
  }
  const size_t n = v.size();
  if (n % 2 == 0) {
    return static_cast<int16_t>(
        (static_cast<int32_t>(v[(n / 2) - 1]) + v[n / 2]) / 2);
  }
  return v[n / 2];
}

// The Weighted mode's median as it took it, by std::nth_element.
double selected_median(std::vector<int16_t> values) {
  const size_t n = values.size();
  const auto mid = values.begin() + static_cast<std::ptrdiff_t>(n / 2);
  std::nth_element(values.begin(), mid, values.end());
  double median = *mid;
  if (n % 2 == 0) {
    median = 0.5 * (median + *std::max_element(values.begin(), mid));
  }
  return median;
}

// Random sets of |count| values over a narrow, a sample-like and the whole
// int16 range, so ties, runs and extremes all occur.
std::vector<std::vector<int16_t>> random_sets(size_t count, uint32_t seed) {
  std::mt19937 rng(seed);
  std::vector<std::vector<int16_t>> sets;
  const std::array<std::pair<int32_t, int32_t>, 3> ranges{
      {{-2, 2}, {0, 1023}, {INT16_MIN, INT16_MAX}}};
  for (const auto& [low, high] : ranges) {
    std::uniform_int_distribution<int32_t> value(low, high);
    for (size_t t = 0; t < 3000; ++t) {
      std::vector<int16_t> set(count);
      for (auto& v : set) v = static_cast<int16_t>(value(rng));
      sets.push_back(std::move(set));
    }
  }
  return sets;
}

TEST(SampleMedianTest, MatchesTheInsertionSortForEveryCount) {
  for (size_t count = 1; count <= kMaxNetworkValues + 8; ++count) {
    for (const auto& set :
         random_sets(count, static_cast<uint32_t>(count) * 7919U)) {
      ASSERT_EQ(orc::median_sample(set), insertion_median(set))
          << "count " << count;
    }
  }
}

TEST(SampleMedianTest, MiddleValuesMatchTheSelectedMedian) {
  for (size_t count = 3; count <= kMaxNetworkValues + 8; ++count) {
    for (const auto& set :
         random_sets(count, static_cast<uint32_t>(count) * 104729U)) {
      const orc::MiddleValues m = orc::middle_values(set.data(), set.size());
      double median = m.upper;
      if (count % 2 == 0) median = 0.5 * (median + m.lower);
      ASSERT_EQ(median, selected_median(set)) << "count " << count;
    }
  }
}

TEST(SampleMedianTest, NetworksSortEveryCount) {
  for (size_t count = 0; count <= kMaxNetworkValues; ++count) {
    for (auto set : random_sets(count, static_cast<uint32_t>(count) + 1U)) {
      std::vector<int32_t> keys(set.begin(), set.end());
      orc::sort_by_network(keys.data(), keys.size());
      ASSERT_TRUE(std::is_sorted(keys.begin(), keys.end()))
          << "count " << count;
      std::sort(set.begin(), set.end());
      ASSERT_TRUE(std::equal(keys.begin(), keys.end(), set.begin()))
          << "count " << count;
    }
  }
}

TEST(SampleMedianTest, GroupMediansMatchEachSubsetsMedian) {
  std::mt19937 rng(5);
  for (size_t count = 0; count <= 16; ++count) {
    for (size_t group_count = 1; group_count <= 6; ++group_count) {
      std::uniform_int_distribution<size_t> group(0, group_count - 1);
      for (auto set : random_sets(
               count, static_cast<uint32_t>((count * 31) + group_count))) {
        std::vector<uint8_t> groups(count);
        for (auto& g : groups) g = static_cast<uint8_t>(group(rng));
        std::vector<int32_t> medians(2 * group_count);
        orc::group_medians(set.data(), groups.data(), count, group_count,
                           medians.data());
        for (size_t g = 0; g < group_count; ++g) {
          std::vector<int16_t> own;
          std::vector<int16_t> rest;
          for (size_t i = 0; i < count; ++i) {
            (groups[i] == g ? own : rest).push_back(set[i]);
          }
          const auto expected = [](const std::vector<int16_t>& subset) {
            return subset.empty() ? orc::kNoMedian
                                  : int32_t{insertion_median(subset)};
          };
          ASSERT_EQ(medians[2 * g], expected(own))
              << "count " << count << " group " << g;
          ASSERT_EQ(medians[(2 * g) + 1], expected(rest))
              << "count " << count << " group " << g;
        }
      }
    }
  }
}

TEST(SampleMedianTest, EmptyIsZero) { EXPECT_EQ(orc::median_sample({}), 0); }

}  // namespace
}  // namespace orc_unit_test
