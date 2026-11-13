/*
 * File:        symmetric_fir_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for symmetric_fir(): the same to the bit as the
 *              plain per-output loop over the taps, for every radius around
 *              its four-tap passes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/support/symmetric_fir.h>

#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

namespace orc_unit_test {
namespace {

// One output at a time, the taps in order.
std::vector<double> plain_fir(const std::vector<double>& taps, int32_t radius,
                              const double* x, size_t count) {
  std::vector<double> out(count);
  for (size_t i = 0; i < count; ++i) {
    const double* centre = x + i;
    double sum = taps[0] * centre[0];
    for (int32_t k = 1; k <= radius; ++k) {
      sum += taps[static_cast<size_t>(k)] * (centre[-k] + centre[k]);
    }
    out[i] = sum;
  }
  return out;
}

TEST(SymmetricFir, IsThePlainLoopToTheBit) {
  std::mt19937 rng(11);
  std::normal_distribution<double> sample(0.0, 300.0);
  std::normal_distribution<double> tap(0.0, 0.05);
  for (const int32_t radius : {0, 1, 2, 3, 4, 5, 7, 8, 9, 31, 127}) {
    for (const size_t count : {size_t{0}, size_t{1}, size_t{3}, size_t{950}}) {
      std::vector<double> taps(static_cast<size_t>(radius) + 1);
      for (double& t : taps) t = tap(rng);
      std::vector<double> signal(count + (2 * static_cast<size_t>(radius)));
      for (double& s : signal) s = sample(rng);
      // Signed zeros among the samples.
      for (size_t i = 0; i < signal.size(); i += 7) signal[i] = -0.0;
      const double* x = signal.data() + radius;
      std::vector<double> out(count);
      orc::symmetric_fir(taps.data(), radius, x, count, out.data());
      const auto expected = plain_fir(taps, radius, x, count);
      if (count == 0) continue;
      EXPECT_EQ(
          std::memcmp(out.data(), expected.data(), count * sizeof(double)), 0)
          << "radius " << radius << " count " << count;
    }
  }
}

}  // namespace
}  // namespace orc_unit_test
