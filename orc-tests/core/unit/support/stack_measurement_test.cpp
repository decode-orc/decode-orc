/*
 * File:        stack_measurement_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the pair noise meter and the pressing noise
 *              model: grouping, the noise fit and the check of the fit
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/stack_measurement.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <random>
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

// ── Synthetic captures through the pair noise meter ─────────────────────────

namespace orc_unit_test {
namespace {

constexpr size_t kPalWidth = 1135;
constexpr size_t kPalHeight = 625;

size_t pal_frame_samples() {
  return orc::frame_line_sample_offset(orc::VideoSystem::PAL, kPalWidth,
                                       kPalHeight);
}

// A test-card-like picture about mid-grey: flat areas, a band of fine
// gratings and a ramp, so a capture whose gain differs from the others'
// leaves picture in every pair difference outside the flat areas.  |busy|
// covers the whole picture in detail instead, with no flat block anywhere.
double card(size_t line, size_t n, bool busy) {
  const double x = static_cast<double>(n);
  if (busy) return 512.0 + 200.0 * std::sin(0.9 * x + 0.3 * line);
  const size_t in_field = line % 313;
  if (in_field > 120 && in_field < 200) {
    return 512.0 + 150.0 * std::sin(0.8 * x);
  }
  if (n > 800) return 312.0 + 0.8 * (x - 800.0);
  return 512.0;
}

// One capture: gain and level against the picture, its own noise, and the
// pressing whose stamper noise it carries (none when |pressing| is 0).
struct Capture {
  double gain = 1.0;
  double level = 0.0;
  double own_sigma = 0.0;
  size_t pressing = 0;
};

// Frames of each capture, the stamper noise of pressing p (σ |stamper[p]|)
// shared by every capture of it.  Also returns each capture's noise planes
// so a combination's real noise can be measured.
struct Frames {
  std::vector<std::vector<std::vector<orc::StackSample>>>
      planes;                                          // [frame][capture]
  std::vector<std::vector<std::vector<float>>> noise;  // [frame][capture]
};
Frames make_frames(const std::vector<Capture>& captures,
                   const std::vector<double>& stamper, size_t frames,
                   const std::vector<bool>& busy = {}) {
  const size_t total = pal_frame_samples();
  Frames out;
  std::mt19937 rng(12345);
  std::normal_distribution<double> gauss(0.0, 1.0);
  for (size_t f = 0; f < frames; ++f) {
    const bool frame_busy = f < busy.size() && busy[f];
    std::vector<std::vector<float>> pressing_noise(stamper.size());
    for (size_t p = 1; p < stamper.size(); ++p) {
      pressing_noise[p].resize(total);
      for (float& v : pressing_noise[p]) {
        v = static_cast<float>(stamper[p] * gauss(rng));
      }
    }
    std::vector<std::vector<orc::StackSample>> planes;
    std::vector<std::vector<float>> noises;
    for (const Capture& c : captures) {
      std::vector<orc::StackSample> plane(total);
      std::vector<float> noise(total);
      for (size_t line = 0; line < kPalHeight; ++line) {
        const size_t start = orc::frame_line_sample_offset(
            orc::VideoSystem::PAL, kPalWidth, line);
        const size_t length = orc::frame_line_sample_count(
            orc::VideoSystem::PAL, kPalWidth, line);
        for (size_t n = 0; n < length && start + n < total; ++n) {
          const size_t off = start + n;
          double e = c.own_sigma * gauss(rng);
          if (c.pressing != 0) e += pressing_noise[c.pressing][off];
          noise[off] = static_cast<float>(e);
          const double v = c.gain * (card(line, n, frame_busy) - 512.0) +
                           512.0 + c.level + e;
          plane[off] = static_cast<orc::StackSample>(
              std::clamp(std::lround(v), 0L, 1023L));
        }
      }
      planes.push_back(std::move(plane));
      noises.push_back(std::move(noise));
    }
    out.planes.push_back(std::move(planes));
    out.noise.push_back(std::move(noises));
  }
  return out;
}

orc::PairNoiseMeter meter_of(const Frames& frames, size_t n) {
  orc::PairNoiseMeter meter(n);
  const std::vector<uint8_t> clear(pal_frame_samples(), 0);
  for (const auto& frame : frames.planes) {
    std::vector<const std::vector<orc::StackSample>*> planes;
    std::vector<const std::vector<uint8_t>*> masks;
    for (const auto& plane : frame) {
      planes.push_back(&plane);
      masks.push_back(&clear);
    }
    meter.add_frame(planes, masks, orc::VideoSystem::PAL, kPalWidth,
                    kPalHeight);
  }
  return meter;
}

// Variance of Σ wᵢ·noiseᵢ (weights normalised) over the active picture of
// every frame: the real noise of a stack with those weights.
double combined_variance(const Frames& frames, std::vector<double> weights) {
  double total = 0.0;
  for (double w : weights) total += w;
  for (double& w : weights) w /= total;
  double sum_sq = 0.0;
  double count = 0.0;
  for (const auto& frame : frames.noise) {
    for (size_t off = 60 * kPalWidth; off < 280 * kPalWidth; off += 3) {
      double e = 0.0;
      for (size_t i = 0; i < weights.size(); ++i)
        e += weights[i] * frame[i][off];
      sum_sq += e * e;
      count += 1.0;
    }
  }
  return sum_sq / count;
}

// Σ⁻¹1 normalised, by Gauss-Jordan elimination.
std::vector<double> optimum_weights(std::vector<std::vector<double>> sigma) {
  const size_t n = sigma.size();
  for (auto& row : sigma) row.push_back(1.0);
  for (size_t col = 0; col < n; ++col) {
    for (size_t row = 0; row < n; ++row) {
      if (row == col) continue;
      const double f = sigma[row][col] / sigma[col][col];
      for (size_t k = col; k <= n; ++k) sigma[row][k] -= f * sigma[col][k];
    }
  }
  std::vector<double> w(n);
  double total = 0.0;
  for (size_t i = 0; i < n; ++i) {
    w[i] = sigma[i][n] / sigma[i][i];
    total += w[i];
  }
  for (double& x : w) x /= total;
  return w;
}

}  // namespace

// A capture 5 % up in gain and 6 codes up in level differences every
// grating and ramp into its pairs: over the whole picture that reads as
// noise, and on Community South it doubled three captures' measured noise.
// On flat blocks, about each block's own mean, it does not count.
TEST(StackMeasurement, PairNoiseMeter_GainAndLevelDoNotReadAsNoise) {
  const std::vector<Capture> captures = {{1.0, 0.0, 5.0, 0},
                                         {1.0, 0.0, 5.0, 0},
                                         {1.0, 0.0, 5.0, 0},
                                         {1.05, 6.0, 5.0, 0}};
  const Frames frames = make_frames(captures, {0.0}, 3);
  const orc::PairNoiseMeter meter = meter_of(frames, captures.size());

  const auto variance = meter.pair_variances();
  for (size_t i = 0; i < captures.size(); ++i) {
    for (size_t j = i + 1; j < captures.size(); ++j) {
      ASSERT_TRUE(variance[i * captures.size() + j]) << i << "-" << j;
      EXPECT_NEAR(*variance[i * captures.size() + j], 50.0, 5.0)
          << i << "-" << j;
    }
  }
  EXPECT_EQ(meter.whole_picture_pairs(), 0u);

  // The fixture does leak: over the whole picture the pair reads higher.
  orc::DifferenceHistogram whole;
  for (size_t off = 0; off < pal_frame_samples(); off += 2) {
    whole.add(frames.planes[0][0][off] - frames.planes[0][3][off]);
  }
  ASSERT_TRUE(whole.robust_variance());
  EXPECT_GT(*whole.robust_variance(), 58.0);

  const auto levels = meter.levels();
  ASSERT_TRUE(levels[3]);
  EXPECT_NEAR(levels[3]->gain, 1.05, 0.005);
  EXPECT_NEAR(levels[3]->level, 6.0, 1.0);
  ASSERT_TRUE(levels[0]);
  EXPECT_NEAR(levels[0]->gain, 1.0, 0.005);
}

// Frames with no flat block at all, on which one capture's picture is out of
// step with the rest, must not count when flat frames are available.
TEST(StackMeasurement, PairNoiseMeter_BusyFramesDoNotCount) {
  const std::vector<Capture> captures = {
      {1.0, 0.0, 4.0, 0}, {1.08, 0.0, 4.0, 0}, {1.0, 0.0, 4.0, 0}};
  const Frames frames =
      make_frames(captures, {0.0}, 4, {false, true, false, true});
  const auto variance = meter_of(frames, captures.size()).pair_variances();
  for (size_t k : {1u, 2u, 5u}) {
    ASSERT_TRUE(variance[k]) << k;
    EXPECT_NEAR(*variance[k], 32.0, 3.5) << k;
  }
}

// The Community South structure: eight captures sharing one stamper's noise,
// a capture of a pressing of its own 7 % up in gain, and two of a third
// pressing 3.5 % down.  Their own noise is all alike, so the three outside
// the eight carry the only noise the eight do not share and should carry
// about half the weight; the stacker gave them 1 % each.
TEST(StackMeasurement, Model_CommunitySouthStructureIsWeightedNearTheOptimum) {
  std::vector<Capture> captures;
  for (size_t i = 0; i < 8; ++i) captures.push_back({1.0, 0.0, 8.6, 1});
  captures.push_back({1.07, 8.0, 14.1, 0});
  captures.push_back({0.965, -5.0, 10.2, 2});
  captures.push_back({0.965, -5.0, 10.2, 2});
  const std::vector<double> stamper = {0.0, 10.5, 9.2};
  const size_t n = captures.size();
  const Frames frames = make_frames(captures, stamper, 2);
  const orc::PairNoiseMeter meter = meter_of(frames, n);
  const auto variance = meter.pair_variances();

  const auto groups = orc::group_by_pressing(n, variance);
  for (size_t i = 0; i < n; ++i) ASSERT_TRUE(groups[i]) << i;
  for (size_t i = 1; i < 8; ++i) EXPECT_EQ(*groups[i], *groups[0]) << i;
  EXPECT_NE(*groups[8], *groups[0]);
  EXPECT_NE(*groups[9], *groups[0]);
  EXPECT_NE(*groups[9], *groups[8]);
  EXPECT_EQ(*groups[10], *groups[9]);

  const auto noise = orc::fit_pressing_noise(n, variance, groups);
  std::vector<std::vector<double>> sigma(n, std::vector<double>(n, 0.0));
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < n; ++j) {
      if (i == j) {
        sigma[i][j] = captures[i].own_sigma * captures[i].own_sigma;
      }
      if (captures[i].pressing != 0 &&
          captures[i].pressing == captures[j].pressing) {
        sigma[i][j] +=
            stamper[captures[i].pressing] * stamper[captures[i].pressing];
      }
    }
  }
  for (size_t i = 0; i < n; ++i) {
    ASSERT_TRUE(noise[i]) << i;
    EXPECT_NEAR(noise[i]->sigma, std::sqrt(sigma[i][i]),
                0.1 * std::sqrt(sigma[i][i]))
        << i;
  }

  const std::vector<double> optimum = optimum_weights(sigma);
  const std::vector<double> weights = shares(noise);
  for (size_t i = 0; i < n; ++i) {
    EXPECT_NEAR(weights[i], optimum[i], 0.03) << i;
  }

  const orc::PressingFitCheck check =
      orc::check_pressing_fit(n, variance, noise);
  EXPECT_TRUE(check.trusted) << check.worst_misfit;
  EXPECT_LE(check.weighted_sigma, check.equal_sigma);
  EXPECT_LE(check.weighted_sigma, check.best_single_sigma);

  // The real noise of the stack: never worse than the mean or the best
  // single capture, and within a few per cent of the best possible.
  const double weighted = combined_variance(frames, weights);
  const double equal = combined_variance(frames, std::vector<double>(n, 1.0));
  const double best_possible = combined_variance(frames, optimum);
  double best_single = sigma[0][0];
  for (size_t i = 0; i < n; ++i)
    best_single = std::min(best_single, sigma[i][i]);
  EXPECT_LT(weighted, equal);
  EXPECT_LT(equal, best_single);
  EXPECT_LT(weighted, 1.05 * best_possible);
}

// Measured on the eleven captures of Community South over a window of
// single-frame stills (output frames 30960-31099).
std::vector<std::optional<double>> community_south_stills() {
  const size_t n = 11;
  const std::vector<double> measured = {
      28.9,  30.5,  34.6,  33.9,  39.9,  48.2,  55.0,  310.3, 311.8, 305.4,
      29.1,  32.9,  34.1,  38.6,  48.2,  53.4,  307.0, 311.0, 302.5, 35.3,
      36.0,  40.3,  50.5,  55.3,  310.2, 311.3, 303.6, 38.9,  42.8,  51.9,
      57.8,  310.9, 313.5, 307.2, 42.4,  50.3,  56.3,  305.9, 307.2, 300.2,
      37.8,  43.6,  285.7, 287.4, 278.9, 52.0,  295.5, 296.9, 288.0, 296.5,
      298.1, 289.5, 342.5, 335.0, 83.0};
  std::vector<std::optional<double>> variance(n * n);
  size_t k = 0;
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) variance[i * n + j] = measured[k++];
  }
  return variance;
}

// The pressing test splits the eight captures that share one stamper
// family's noise into its two pressings, five and three, and no flat
// pressing model with that split explains the pairs between the lone
// capture 9 and the pair 10-11 (9-11 measured 335, modelled 524), so that
// fit is not trusted and a stack would fall back to equal weights.
TEST(StackMeasurement, CheckPressingFit_CommunitySouthStillsFlatAreNotTrusted) {
  const size_t n = 11;
  const auto variance = community_south_stills();
  const auto groups = orc::group_by_pressing(n, variance);
  for (size_t i = 1; i < 5; ++i) EXPECT_EQ(groups[i], groups[0]) << i;
  for (size_t i = 6; i < 8; ++i) EXPECT_EQ(groups[i], groups[5]) << i;
  EXPECT_NE(groups[5], groups[0]);
  const auto noise = orc::fit_pressing_noise(n, variance, groups);
  const orc::PressingFitCheck check =
      orc::check_pressing_fit(n, variance, noise);
  EXPECT_FALSE(check.trusted) << check.worst_misfit;
  EXPECT_GT(check.worst_misfit, orc::kMaxPressingMisfit);

  // Stacked equally instead, each σ the capture's spread against the rest,
  // and the weights of any subset of them equal too.
  const auto equal = orc::equal_weight_noise(n, variance, noise);
  std::vector<orc::PressingNoise> subset;
  for (size_t i = 0; i < n; ++i) {
    ASSERT_TRUE(equal[i]) << i;
    EXPECT_DOUBLE_EQ(equal[i]->weight, 1.0);
    double sum = 0.0;
    for (size_t j = 0; j < n; ++j) {
      if (j != i) sum += *variance[std::min(i, j) * n + std::max(i, j)];
    }
    EXPECT_NEAR(equal[i]->sigma, std::sqrt(sum / (n - 1) / 2.0), 1e-9) << i;
    if (i % 3 == 0) subset.push_back(*equal[i]);
  }
  const std::vector<double> weights = orc::pressing_weights(subset);
  for (double w : weights) EXPECT_NEAR(w, weights.front(), 1e-12);
}

// Grouped into families, the two pressings of the eight are found to share
// a mother, and the nested model explains every pair.
TEST(StackMeasurement, GroupIntoFamilies_CommunitySouthStillsAreTrusted) {
  const size_t n = 11;
  const auto variance = community_south_stills();
  const auto groups = orc::group_by_pressing(n, variance);
  const auto families = orc::group_into_families(n, variance, groups);
  for (size_t i = 0; i < n; ++i) ASSERT_TRUE(families[i]) << i;
  for (size_t i = 1; i < 8; ++i) EXPECT_EQ(*families[i], *families[0]) << i;
  EXPECT_NE(*families[8], *families[0]);
  EXPECT_NE(*families[9], *families[0]);
  EXPECT_NE(*families[9], *families[8]);
  EXPECT_EQ(*families[10], *families[9]);

  const auto noise = orc::fit_pressing_noise(n, variance);
  const orc::PressingFitCheck check =
      orc::check_pressing_fit(n, variance, noise);
  EXPECT_TRUE(check.trusted) << check.worst_misfit;
  EXPECT_LE(check.weighted_sigma, check.equal_sigma);
  EXPECT_LE(check.weighted_sigma, check.best_single_sigma);

  // The three captures outside the family carry the only noise the eight
  // do not share, so they carry about half the weight.
  double outside = 0.0;
  double total = 0.0;
  for (size_t i = 0; i < n; ++i) {
    ASSERT_TRUE(noise[i]) << i;
    total += noise[i]->weight;
    if (i >= 8) outside += noise[i]->weight;
  }
  EXPECT_GT(outside / total, 0.4);
  EXPECT_LT(outside / total, 0.7);
}

// Pressings that share nothing are not merged into a family: the merge
// leaves the misfit where it was.
TEST(StackMeasurement, GroupIntoFamilies_UnrelatedPressingsStayApart) {
  const size_t n = 6;
  // Two pressings of three, own noise 30, pressing noise 100 and 150.
  std::vector<std::optional<double>> variance(n * n);
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      const bool same = (i < 3) == (j < 3);
      variance[i * n + j] = same ? 60.0 : 60.0 + 100.0 + 150.0;
    }
  }
  const auto groups = orc::group_by_pressing(n, variance);
  const auto families = orc::group_into_families(n, variance, groups);
  ASSERT_TRUE(families[0] && families[3]);
  EXPECT_NE(*families[0], *families[3]);
}

// Two pressings of one family and two more pressings each on its own: the
// nested model recovers each term, and the weights are Σ⁻¹1 for its
// covariance.  (With only three pressings a flat model fits any family
// exactly, its noise taken into the third pressing's, so a family shows only
// beside at least two others.)
TEST(StackMeasurement, FitPressingNoise_RecoversAFamily) {
  const size_t n = 9;
  const std::vector<size_t> pressing = {0, 0, 0, 1, 1, 2, 2, 3, 3};
  const std::vector<size_t> family = {0, 0, 0, 0, 0, 1, 1, 2, 2};
  const double own = 25.0;
  const std::vector<double> shared = {20.0, 30.0, 60.0, 40.0};
  const std::vector<double> mother = {150.0, 0.0, 0.0};
  std::vector<std::vector<double>> sigma(n, std::vector<double>(n, 0.0));
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < n; ++j) {
      if (i == j) sigma[i][j] += own;
      if (pressing[i] == pressing[j]) sigma[i][j] += shared[pressing[i]];
      if (family[i] == family[j]) sigma[i][j] += mother[family[i]];
    }
  }
  std::vector<std::optional<double>> variance(n * n);
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      variance[i * n + j] = sigma[i][i] + sigma[j][j] - 2.0 * sigma[i][j];
    }
  }

  // The flat model cannot explain it.
  const auto groups = orc::group_by_pressing(n, variance);
  EXPECT_GT(orc::check_pressing_fit(
                n, variance, orc::fit_pressing_noise(n, variance, groups))
                .worst_misfit,
            orc::kMaxPressingMisfit);

  const auto noise = orc::fit_pressing_noise(n, variance);
  const orc::PressingFitCheck check =
      orc::check_pressing_fit(n, variance, noise);
  EXPECT_TRUE(check.trusted);
  EXPECT_LT(check.worst_misfit, 0.01);
  for (size_t i = 0; i < n; ++i) {
    ASSERT_TRUE(noise[i]) << i;
    EXPECT_NEAR(noise[i]->own_variance, own, 1.0) << i;
    EXPECT_EQ(noise[i]->family == noise[0]->family, family[i] == 0) << i;
  }
  EXPECT_NEAR(noise[0]->shared_variance, shared[0], 1.0);
  EXPECT_NEAR(noise[3]->shared_variance, shared[1], 1.0);
  EXPECT_NEAR(noise[0]->family_variance, mother[0], 1.0);
  EXPECT_NEAR(noise[5]->shared_variance, shared[2], 1.0);
  EXPECT_NEAR(noise[7]->shared_variance, shared[3], 1.0);
  const std::vector<double> optimum = optimum_weights(sigma);
  const std::vector<double> weights = shares(noise);
  for (size_t i = 0; i < n; ++i) {
    EXPECT_NEAR(weights[i], optimum[i], 0.01) << i;
  }
}

// Captures 7 % up and 3.5 % down in gain, with levels of their own, are
// brought to the typical capture's: what is left of each is its noise.
TEST(StackMeasurement, ApplyLumaLevel_BringsACaptureToTheTypical) {
  std::vector<Capture> captures(4, Capture{1.0, 0.0, 4.0, 0});
  captures[1] = {1.07, 11.0, 4.0, 0};
  captures[3] = {0.965, -6.0, 4.0, 0};
  const size_t n = captures.size();
  Frames frames = make_frames(captures, {0.0}, 1);
  orc::LumaLevelMeter meter(n);
  const std::vector<uint8_t> clear(pal_frame_samples(), 0);
  std::vector<const std::vector<orc::StackSample>*> planes;
  std::vector<const std::vector<uint8_t>*> masks;
  for (const auto& plane : frames.planes[0]) {
    planes.push_back(&plane);
    masks.push_back(&clear);
  }
  meter.add_frame(planes, masks, orc::VideoSystem::PAL, kPalWidth, kPalHeight);
  const auto levels = meter.levels();
  for (size_t i = 0; i < n; ++i) {
    ASSERT_TRUE(levels[i]) << i;
    EXPECT_NEAR(levels[i]->gain, captures[i].gain, 0.005) << i;
  }

  // Each capture's picture, the plane less its own noise, against the
  // first's, before and after.
  auto worst = [&](size_t i) {
    double largest = 0.0;
    for (size_t off = 60 * kPalWidth; off < 280 * kPalWidth; off += 5) {
      const double a = frames.planes[0][i][off] - frames.noise[0][i][off];
      const double b = frames.planes[0][0][off] - frames.noise[0][0][off];
      largest = std::max(largest, std::abs(a - b));
    }
    return largest;
  };
  EXPECT_GT(worst(1), 20.0);
  for (size_t i = 1; i < n; ++i) {
    orc::apply_luma_level(frames.planes[0][i], *levels[i]);
    for (float& e : frames.noise[0][i]) {
      e = static_cast<float>(e / levels[i]->gain);
    }
    EXPECT_LT(worst(i), 3.0) << i;
  }
}

// The weights of a subset of one fit are that subset's own Σ⁻¹1: copies of
// another frame of the disc share noise only with each other.
TEST(StackMeasurement, PressingWeights_ASubsetIsWeightedAsItsOwnOptimum) {
  std::vector<orc::PressingNoise> captures(5);
  const std::vector<size_t> group = {0, 0, 0, 1, 2};
  const std::vector<size_t> family = {0, 0, 0, 0, 1};
  for (size_t i = 0; i < captures.size(); ++i) {
    captures[i].group = group[i];
    captures[i].family = family[i];
    captures[i].own_variance = 20.0 + 5.0 * static_cast<double>(i);
    captures[i].shared_variance = group[i] == 0 ? 30.0 : 0.0;
    captures[i].family_variance = family[i] == 0 ? 100.0 : 0.0;
  }
  auto sigma_of = [&](const std::vector<size_t>& members) {
    std::vector<std::vector<double>> sigma(
        members.size(), std::vector<double>(members.size(), 0.0));
    for (size_t p = 0; p < members.size(); ++p) {
      for (size_t q = 0; q < members.size(); ++q) {
        const auto& a = captures[members[p]];
        const auto& b = captures[members[q]];
        if (p == q) sigma[p][q] += a.own_variance;
        if (a.family == b.family) sigma[p][q] += a.family_variance;
        if (a.group == b.group) sigma[p][q] += a.shared_variance;
      }
    }
    return sigma;
  };
  for (const std::vector<size_t>& members :
       {std::vector<size_t>{0, 1, 2, 3, 4}, std::vector<size_t>{0, 3},
        std::vector<size_t>{1, 2, 4}}) {
    std::vector<orc::PressingNoise> subset;
    for (size_t i : members) subset.push_back(captures[i]);
    const std::vector<double> weights = orc::pressing_weights(subset);
    const std::vector<double> optimum = optimum_weights(sigma_of(members));
    double total = 0.0;
    for (double w : weights) total += w;
    for (size_t k = 0; k < members.size(); ++k) {
      EXPECT_NEAR(weights[k] / total, optimum[k], 1e-9) << members.size();
    }
  }

  // Two of a pressing alone carry more each than they do among all five,
  // where they share its noise with a third and the family's with a fourth.
  const std::vector<double> all = orc::pressing_weights(captures);
  const std::vector<double> pair =
      orc::pressing_weights({captures[0], captures[1]});
  EXPECT_GT(pair[0], all[0]);
}

// Captures the model explains exactly are trusted, and its predictions for
// the stack follow from its E and G.
TEST(StackMeasurement, CheckPressingFit_AnExactModelIsTrusted) {
  const auto groups = std::vector<std::optional<size_t>>(kCaptures, size_t{0});
  const std::vector<double> own = {36.0, 64.0, 49.0, 81.0, 100.0};
  std::vector<std::optional<double>> variance(kCaptures * kCaptures);
  for (size_t i = 0; i < kCaptures; ++i) {
    for (size_t j = i + 1; j < kCaptures; ++j) {
      variance[i * kCaptures + j] = own[i] + own[j];
    }
  }
  const auto noise = orc::fit_pressing_noise(kCaptures, variance, groups);
  const orc::PressingFitCheck check =
      orc::check_pressing_fit(kCaptures, variance, noise);
  EXPECT_TRUE(check.trusted);
  EXPECT_LT(check.worst_misfit, 0.01);
  EXPECT_EQ(check.pairs, 10u);
  EXPECT_NEAR(check.best_single_sigma, 6.0, 0.01);
  double inverse = 0.0;
  double sum = 0.0;
  for (double e : own) {
    inverse += 1.0 / e;
    sum += e;
  }
  EXPECT_NEAR(check.weighted_sigma, std::sqrt(1.0 / inverse), 0.01);
  EXPECT_NEAR(check.equal_sigma, std::sqrt(sum) / kCaptures, 0.01);
}

// Two stamper groups of three and two discs, and three discs each alone in
// its pressing.  What a disc adds is how much noisier the stack would be
// without it: a lone disc's noise is its own, so the stack loses most
// without one of them, while a disc of a group leaves its stamper's noise
// still carried by the others.
TEST(StackMeasurement, CombinedSigma_LoneDiscsAddTheMost) {
  constexpr size_t n = 8;
  const std::vector<int> group = {0, 0, 0, 1, 1, -1, -1, -1};
  constexpr double kOwn = 20.0;
  constexpr double kStamper = 30.0;
  // A lone disc's stamper noise is its own: no other disc shares it.
  auto own = [&](size_t i) { return group[i] < 0 ? kOwn + kStamper : kOwn; };
  std::vector<std::optional<double>> variance(n * n);
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      double v = own(i) + own(j);
      if (group[i] >= 0 && group[j] >= 0 && group[i] != group[j]) {
        v += 2.0 * kStamper;
      } else if ((group[i] >= 0) != (group[j] >= 0)) {
        v += kStamper;
      }
      variance[i * n + j] = v;
    }
  }
  const auto noise = orc::fit_pressing_noise(n, variance);
  const orc::PressingFitCheck check =
      orc::check_pressing_fit(n, variance, noise);
  ASSERT_TRUE(check.trusted) << check.worst_misfit;

  std::vector<orc::PressingNoise> all;
  for (const auto& capture : noise) {
    ASSERT_TRUE(capture);
    all.push_back(*capture);
  }
  const double stack = orc::combined_sigma(all);
  EXPECT_NEAR(stack, check.weighted_sigma, 1e-9);
  EXPECT_LT(stack, check.best_single_sigma);

  std::vector<double> without(n);
  for (size_t i = 0; i < n; ++i) {
    std::vector<orc::PressingNoise> others;
    for (size_t k = 0; k < n; ++k) {
      if (k != i) others.push_back(all[k]);
    }
    without[i] = orc::combined_sigma(others);
    EXPECT_GT(without[i], stack) << i;
  }
  for (size_t lone = 5; lone < n; ++lone) {
    for (size_t grouped = 0; grouped < 5; ++grouped) {
      EXPECT_GT(without[lone], without[grouped]) << lone << " " << grouped;
    }
  }
  EXPECT_EQ(orc::combined_sigma({}), 0.0);
}

}  // namespace orc_unit_test

namespace orc_unit_test {
namespace {

// An NTSC-geometry frame whose every line is a sum of random sinusoids below
// 0.3 of the sample rate, delayed by |delay| samples: the delay is exact
// however fractional.
std::vector<orc::StackSample> sinusoid_frame(double delay) {
  constexpr size_t kWidth = 910;
  constexpr size_t kHeight = 525;
  std::mt19937 rng(7);
  std::uniform_real_distribution<double> frequency(0.01, 0.3);
  std::uniform_real_distribution<double> phase(0.0, 2.0 * M_PI);
  std::vector<orc::StackSample> frame(kWidth * kHeight);
  for (size_t line = 0; line < kHeight; ++line) {
    double f[12];
    double p[12];
    for (size_t k = 0; k < 12; ++k) {
      f[k] = frequency(rng);
      p[k] = phase(rng);
    }
    for (size_t n = 0; n < kWidth; ++n) {
      double v = 500.0;
      for (size_t k = 0; k < 12; ++k) {
        v += 30.0 *
             std::cos(2.0 * M_PI * f[k] * (static_cast<double>(n) - delay) +
                      p[k]);
      }
      frame[line * kWidth + n] = static_cast<orc::StackSample>(std::lround(v));
    }
  }
  return frame;
}

}  // namespace

// The grid meter reports the whole-sample offset it corrects and, from the
// interpolated raw correlation, the fraction that correction leaves.
TEST(StackMeasurement, SampleGridMeter_MeasuresAFractionalOffset) {
  const auto reference = sinusoid_frame(0.0);
  for (const double delay : {-0.3, 0.5, 2.3, -1.3}) {
    SCOPED_TRACE(delay);
    orc::SampleGridMeter meter;
    meter.add_frame(reference, sinusoid_frame(delay), orc::VideoSystem::NTSC,
                    910, 525);
    const auto result = meter.measure();
    ASSERT_TRUE(result.offset.has_value());
    ASSERT_TRUE(result.fractional_offset.has_value());
    EXPECT_NEAR(*result.fractional_offset, delay, 0.03);
    EXPECT_LE(std::abs(*result.offset - delay), 0.5 + 0.03);
  }
}

}  // namespace orc_unit_test
