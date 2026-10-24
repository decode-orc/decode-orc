/*
 * File:        stack_measurement_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the pressing noise model: grouping and the
 *              noise fit from pair variances
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/support/stack_measurement.h>

#include <cmath>
#include <optional>
#include <vector>

namespace orc_unit_test {
namespace {

constexpr size_t kCaptures = 5;

// Pair variances indexed i * n + j for i < j, from {i, j, variance} with
// inputs numbered from 1 as the stacker logs them.
struct Pair {
  size_t i;
  size_t j;
  double variance;
};
std::vector<std::optional<double>> pair_variances(
    const std::vector<Pair>& pairs) {
  std::vector<std::optional<double>> v(kCaptures * kCaptures);
  for (const Pair& p : pairs) v[(p.i - 1) * kCaptures + (p.j - 1)] = p.variance;
  return v;
}

// Each capture's share of the total weight.
std::vector<double> shares(
    const std::vector<std::optional<orc::PressingNoise>>& noise) {
  double total = 0.0;
  for (const auto& n : noise) total += n ? n->weight : 0.0;
  std::vector<double> out;
  for (const auto& n : noise) out.push_back(n ? n->weight / total : 0.0);
  return out;
}

}  // namespace

// Pair variances of five captures of one pressing, all independent noise,
// fit exactly.
TEST(StackMeasurement, FitPressingNoise_RecoversIndependentNoise) {
  const std::vector<double> own = {36.0, 64.0, 49.0, 81.0, 100.0};
  std::vector<Pair> pairs;
  for (size_t i = 0; i < kCaptures; ++i) {
    for (size_t j = i + 1; j < kCaptures; ++j) {
      pairs.push_back({i + 1, j + 1, own[i] + own[j]});
    }
  }
  const auto noise = orc::fit_pressing_noise(
      kCaptures, pair_variances(pairs),
      std::vector<std::optional<size_t>>(kCaptures, size_t{0}));
  for (size_t i = 0; i < kCaptures; ++i) {
    ASSERT_TRUE(noise[i]) << "capture " << i;
    EXPECT_NEAR(noise[i]->sigma, std::sqrt(own[i]), 0.01) << "capture " << i;
  }
}

// Measured on five captures of Volcanoes side 1, frames 38000-38499, where
// capture 4's chroma does not line up with the others' (its pairs ~1000, the
// rest ~150).  Fitted with equal weights, the spread among capture 4's pairs
// drove capture 1's noise to 1.5 codes and gave it 93 % of the weight; the
// others must keep their usual noise (6-12 codes elsewhere on the side) and
// only capture 4 be weighted down.
TEST(StackMeasurement, FitPressingNoise_OneMisfitCaptureDoesNotSteerTheOthers) {
  const auto variances = pair_variances({{1, 2, 181.3},
                                         {1, 3, 124.5},
                                         {1, 4, 925.1},
                                         {1, 5, 127.2},
                                         {2, 3, 174.3},
                                         {2, 4, 1276.4},
                                         {2, 5, 203.8},
                                         {3, 4, 1130.9},
                                         {3, 5, 146.2},
                                         {4, 5, 1116.3}});
  const auto noise = orc::fit_pressing_noise(
      kCaptures, variances,
      std::vector<std::optional<size_t>>(kCaptures, size_t{0}));
  const std::vector<double> share = shares(noise);
  for (size_t i : {0u, 1u, 2u, 4u}) {
    ASSERT_TRUE(noise[i]) << "capture " << i + 1;
    EXPECT_GT(noise[i]->sigma, 5.0) << "capture " << i + 1;
    EXPECT_LT(noise[i]->sigma, 13.0) << "capture " << i + 1;
    EXPECT_LT(share[i], 0.4) << "capture " << i + 1;
  }
  ASSERT_TRUE(noise[3]);
  EXPECT_GT(noise[3]->sigma, 25.0);
  EXPECT_LT(share[3], 0.05);
}

// The same captures over 16 frames across the side: one pressing.
TEST(StackMeasurement, GroupByPressing_VolcanoesAcrossTheSideIsOnePressing) {
  const auto groups =
      orc::group_by_pressing(kCaptures, pair_variances({{1, 2, 158.4},
                                                        {1, 3, 137.0},
                                                        {1, 4, 159.0},
                                                        {1, 5, 182.4},
                                                        {2, 3, 166.1},
                                                        {2, 4, 209.6},
                                                        {2, 5, 223.4},
                                                        {3, 4, 178.9},
                                                        {3, 5, 198.5},
                                                        {4, 5, 219.9}}));
  for (size_t i = 0; i < kCaptures; ++i) {
    ASSERT_TRUE(groups[i]) << "capture " << i + 1;
    EXPECT_EQ(*groups[i], 0u) << "capture " << i + 1;
  }
}

}  // namespace orc_unit_test
