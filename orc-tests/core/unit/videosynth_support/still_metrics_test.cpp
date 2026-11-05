/*
 * File:        still_metrics_test.cpp
 * Module:      orc-core-unit-tests
 * Purpose:     The videosynth suite's source-referenced still metrics, on
 *              synthetic pictures with known answers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <still_metrics.h>

#include <cmath>
#include <cstdint>
#include <vector>

namespace {

using orc_videosynth::ComponentPicture;
using orc_videosynth::Lab;
using orc_videosynth::StillMetricOptions;

constexpr double kPi = 3.14159265358979323846;
constexpr double kPalRate = 17734475.0;
constexpr size_t kWidth = 256;
constexpr size_t kHeight = 12;

StillMetricOptions pal_options() {
  StillMetricOptions options;
  options.system = orc::VideoSystem::PAL;
  options.sample_rate_hz = kPalRate;
  return options;
}

ComponentPicture flat(double y, double u, double v) {
  ComponentPicture p(kWidth, kHeight);
  std::fill(p.y.begin(), p.y.end(), y);
  std::fill(p.u.begin(), p.u.end(), u);
  std::fill(p.v.begin(), p.v.end(), v);
  return p;
}

std::vector<uint8_t> everything() {
  return std::vector<uint8_t>(kWidth * kHeight, 1);
}

TEST(VideosynthStillMetrics, identicalPicturesHaveNoError) {
  const auto truth = flat(0.5, 0.1, -0.2);
  const auto m =
      orc_videosynth::measure_still(truth, truth, everything(), pal_options());
  EXPECT_EQ(m.samples, (kWidth - 32) * kHeight);
  EXPECT_EQ(m.y_rms_pct, 0.0);
  EXPECT_EQ(m.delta_e_mean, 0.0);
  EXPECT_EQ(m.chroma_phase_rms_deg, 0.0);
  EXPECT_EQ(m.rgb_psnr_db, 120.0);
}

TEST(VideosynthStillMetrics, lumaOffsetGivesRmsAndPsnr) {
  const auto truth = flat(0.5, 0.0, 0.0);
  const auto decoded = flat(0.51, 0.0, 0.0);
  const auto m = orc_videosynth::measure_still(decoded, truth, everything(),
                                               pal_options());
  EXPECT_NEAR(m.y_rms_pct, 1.0, 1e-9);
  EXPECT_EQ(m.u_rms_pct, 0.0);
  // Every R'G'B' channel is 0.01 high: MSE 1e-4.
  EXPECT_NEAR(m.rgb_psnr_db, 40.0, 1e-9);
  EXPECT_GT(m.delta_e_mean, 0.0);
}

TEST(VideosynthStillMetrics, edgeExclusionAndMaskSelectSamples) {
  auto decoded = flat(0.5, 0.0, 0.0);
  const auto truth = flat(0.5, 0.0, 0.0);
  // Damage only the excluded first 16 columns and a masked-out row.
  for (size_t row = 0; row < kHeight; ++row) {
    for (size_t x = 0; x < 16; ++x) decoded.y[decoded.index(x, row)] = 1.0;
  }
  for (size_t x = 0; x < kWidth; ++x) decoded.y[decoded.index(x, 3)] = 0.0;
  auto mask = everything();
  for (size_t x = 0; x < kWidth; ++x) mask[(3 * kWidth) + x] = 0;
  const auto m =
      orc_videosynth::measure_still(decoded, truth, mask, pal_options());
  EXPECT_EQ(m.y_rms_pct, 0.0);
  EXPECT_EQ(m.samples, (kWidth - 32) * (kHeight - 1));
}

TEST(VideosynthStillMetrics, chromaRotationIsPhaseNotMagnitude) {
  const double c = 0.3;
  const double hue = 30.0 * kPi / 180.0;
  const auto truth = flat(0.5, c * std::cos(hue), c * std::sin(hue));
  const double rotated = hue + (5.0 * kPi / 180.0);
  const auto decoded = flat(0.5, c * std::cos(rotated), c * std::sin(rotated));
  const auto m = orc_videosynth::measure_still(decoded, truth, everything(),
                                               pal_options());
  EXPECT_EQ(m.chroma_samples, m.samples);
  EXPECT_NEAR(m.chroma_phase_rms_deg, 5.0, 1e-9);
  EXPECT_NEAR(m.chroma_magnitude_rms_pct, 0.0, 1e-12);
  EXPECT_NEAR(m.chroma_gain, 1.0, 1e-12);

  // A decode at 0.9 of the chroma.
  const auto weak = flat(0.5, 0.9 * c * std::cos(hue), 0.9 * c * std::sin(hue));
  const auto g =
      orc_videosynth::measure_still(weak, truth, everything(), pal_options());
  EXPECT_NEAR(g.chroma_gain, 0.9, 1e-12);
}

TEST(VideosynthStillMetrics, crossColourIsChromaOnColourlessLumaDetail) {
  // A 4 MHz luma grating with no colour; the decode adds U = 0.01.
  auto truth = flat(0.5, 0.0, 0.0);
  for (size_t row = 0; row < kHeight; ++row) {
    for (size_t x = 0; x < kWidth; ++x) {
      truth.y[truth.index(x, row)] =
          0.5 + (0.2 * std::sin(2.0 * kPi * 4.0e6 * x / kPalRate));
    }
  }
  auto decoded = truth;
  std::fill(decoded.u.begin(), decoded.u.end(), 0.01);
  const auto m = orc_videosynth::measure_still(decoded, truth, everything(),
                                               pal_options());
  EXPECT_EQ(m.cross_colour_samples, m.samples);
  EXPECT_NEAR(m.cross_colour_rms_pct, 1.0, 1e-9);

  // Flat luma has no detail to count.
  const auto flat_truth = flat(0.5, 0.0, 0.0);
  const auto none = orc_videosynth::measure_still(decoded, flat_truth,
                                                  everything(), pal_options());
  EXPECT_EQ(none.cross_colour_samples, 0u);
}

TEST(VideosynthStillMetrics, crossLumaIsSubcarrierBandLumaErrorAtChromaEdges) {
  // A chroma step at column 128; the decode adds a 4.43 MHz luma ripple of
  // amplitude 0.02 everywhere.
  auto truth = flat(0.5, 0.0, 0.0);
  for (size_t row = 0; row < kHeight; ++row) {
    for (size_t x = 128; x < kWidth; ++x) truth.v[truth.index(x, row)] = 0.3;
  }
  auto decoded = truth;
  for (size_t row = 0; row < kHeight; ++row) {
    for (size_t x = 0; x < kWidth; ++x) {
      decoded.y[decoded.index(x, row)] +=
          0.02 * std::sin(2.0 * kPi * 4433618.75 * x / kPalRate);
    }
  }
  const auto m = orc_videosynth::measure_still(decoded, truth, everything(),
                                               pal_options());
  // The step lies between columns 127 and 128; columns 126..129 lie within
  // a sample of it.
  EXPECT_EQ(m.cross_luma_samples, 4u * kHeight);
  EXPECT_NEAR(m.cross_luma_rms_pct, 2.0 / std::sqrt(2.0), 0.1);
}

TEST(VideosynthStillMetrics, edgeResponseMeasuresRiseAndOvershoot) {
  // A raised-cosine step 10 samples long, then a 5 % overshoot sample.
  ComponentPicture p(64, 2);
  for (size_t row = 0; row < 2; ++row) {
    for (size_t x = 0; x < 64; ++x) {
      double value = 0.0;
      if (x >= 30) value = 1.0;
      if (x >= 20 && x < 30) {
        value = 0.5 - (0.5 * std::cos(kPi * (x - 20.0) / 10.0));
      }
      if (x == 31) value = 1.05;
      p.y[p.index(x, row)] = value;
    }
  }
  const auto e = orc_videosynth::edge_response(p, orc_videosynth::Plane::Y,
                                               {0, 0, 64, 2}, kPalRate);
  ASSERT_TRUE(e.has_value());
  EXPECT_NEAR(e->step, 1.0, 0.01);
  // Raised cosine: 10 % at 2.05 samples, 90 % at 7.95; sampled and
  // interpolated linearly.
  EXPECT_NEAR(e->rise_ns * kPalRate / 1.0e9, 5.9, 0.15);
  EXPECT_NEAR(e->overshoot_pct, 5.0, 0.3);

  ComponentPicture level(64, 2);
  EXPECT_FALSE(orc_videosynth::edge_response(level, orc_videosynth::Plane::Y,
                                             {0, 0, 64, 2}, kPalRate)
                   .has_value());
}

// G. Sharma, W. Wu, E. N. Dalal (2005), Table 1: test pairs 1, 7, 17 and 34.
TEST(VideosynthStillMetrics, ciede2000MatchesSharmaTestData) {
  EXPECT_NEAR(orc_videosynth::ciede2000({50.0, 2.6772, -79.7751},
                                        {50.0, 0.0, -82.7485}),
              2.0425, 1e-4);
  EXPECT_NEAR(orc_videosynth::ciede2000({50.0, 0.0, 0.0}, {50.0, -1.0, 2.0}),
              2.3669, 1e-4);
  EXPECT_NEAR(orc_videosynth::ciede2000({50.0, 2.5, 0.0}, {73.0, 25.0, -18.0}),
              27.1492, 1e-4);
  EXPECT_NEAR(orc_videosynth::ciede2000({2.0776, 0.0795, -1.1350},
                                        {0.9033, -0.0636, -0.5514}),
              0.9082, 1e-4);
}

TEST(VideosynthStillMetrics, whiteIsLabWhite) {
  const Lab white =
      orc_videosynth::yuv_to_lab(1.0, 0.0, 0.0, orc::VideoSystem::PAL);
  EXPECT_NEAR(white.l, 100.0, 1e-9);
  EXPECT_NEAR(white.a, 0.0, 1e-9);
  EXPECT_NEAR(white.b, 0.0, 1e-9);
}

TEST(VideosynthStillMetrics, sourceRegionMapsPixelsToSamples) {
  const auto r = orc_videosynth::source_region(26, 16, 38, 544, 922);
  EXPECT_EQ(r.x, 33u);      // 26 * 922 / 720 = 33.3
  EXPECT_EQ(r.width, 48u);  // to 64 * 922 / 720 = 81.9
  EXPECT_EQ(r.y, 16u);
  EXPECT_EQ(r.height, 544u);
}

}  // namespace
