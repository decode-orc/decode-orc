/*
 * File:        noise_reducer_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the component-frame noise reducer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/plugins/stages/sinks/common/decoders/noise_reducer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <random>
#include <vector>

namespace orc_unit_test {
namespace {

constexpr int32_t kWidth = 64;
constexpr int32_t kActiveXStart = 8;
constexpr int32_t kActiveXEnd = 56;
constexpr int32_t kActiveLineStart = 20;
constexpr int32_t kActiveLineEnd = 420;

constexpr double kLuma = 400.0;
constexpr double kChromaU = 20.0;
constexpr double kChromaV = -30.0;
constexpr double kStrengthDb = 6.0;
constexpr double kExact = 1e-9;

orc::SourceParameters make_params(int32_t width) {
  orc::SourceParameters p;
  p.system = orc::VideoSystem::NTSC;
  p.frame_width_nominal = width;
  p.active_video_start = kActiveXStart;
  p.active_video_end = kActiveXEnd;
  p.first_active_frame_line = kActiveLineStart;
  p.last_active_frame_line = kActiveLineEnd;
  return p;
}

NoiseReducer::Settings make_settings(double strength_db,
                                     double motion_scale = 1.0) {
  NoiseReducer::Settings s;
  s.strength_db = strength_db;
  s.motion_scale = motion_scale;
  s.active_x_start = kActiveXStart;
  s.active_x_end = kActiveXEnd;
  s.active_line_start = kActiveLineStart;
  s.active_line_end = kActiveLineEnd;
  return s;
}

ComponentFrame make_frame(double y, double u, double v, bool mono = false,
                          int32_t width = kWidth) {
  ComponentFrame frame;
  frame.init(make_params(width), mono);
  std::fill(frame.getY()->begin(), frame.getY()->end(), y);
  if (!mono) {
    std::fill(frame.getU()->begin(), frame.getU()->end(), u);
    std::fill(frame.getV()->begin(), frame.getV()->end(), v);
  }
  return frame;
}

ComponentFrame make_flat_frame(bool mono = false) {
  return make_frame(kLuma, kChromaU, kChromaV, mono);
}

void add_noise(ComponentFrame& frame, std::mt19937& rng, double sigma) {
  std::normal_distribution<double> noise(0.0, sigma);
  for (double& s : *frame.getY()) s += noise(rng);
  for (double& s : *frame.getU()) s += noise(rng);
  for (double& s : *frame.getV()) s += noise(rng);
}

// A still picture of 8 x 8 blocks at random levels, as unlike the previous
// one as a slide change: every frame of a sequence of such pictures carries
// different content.
ComponentFrame make_block_picture(std::mt19937& rng) {
  std::uniform_real_distribution<double> level(100.0, 700.0);
  ComponentFrame frame = make_frame(kLuma, kChromaU, kChromaV);
  constexpr int32_t kBlock = 8;
  const int32_t height = frame.getHeight();
  for (int32_t by = 0; by < height; by += kBlock) {
    for (int32_t bx = 0; bx < kWidth; bx += kBlock) {
      const double value = level(rng);
      for (int32_t line = by; line < std::min(height, by + kBlock); ++line) {
        double* y = frame.y(line);
        for (int32_t x = bx; x < std::min(kWidth, bx + kBlock); ++x) {
          y[x] = value;
        }
      }
    }
  }
  return frame;
}

// Variance of the active region of a plane about a known mean.
double active_variance(const std::vector<double>& plane, double mean) {
  double sum = 0.0;
  size_t count = 0;
  for (int32_t line = kActiveLineStart; line < kActiveLineEnd; ++line) {
    for (int32_t x = kActiveXStart; x < kActiveXEnd; ++x) {
      const double d = plane[static_cast<size_t>(line) * kWidth + x] - mean;
      sum += d * d;
      ++count;
    }
  }
  return sum / static_cast<double>(count);
}

double active_mean(const std::vector<double>& plane) {
  double sum = 0.0;
  size_t count = 0;
  for (int32_t line = kActiveLineStart; line < kActiveLineEnd; ++line) {
    for (int32_t x = kActiveXStart; x < kActiveXEnd; ++x) {
      sum += plane[static_cast<size_t>(line) * kWidth + x];
      ++count;
    }
  }
  return sum / static_cast<double>(count);
}

// Root mean square difference of the active luma of two frames.
double active_rms_difference(const ComponentFrame& a, const ComponentFrame& b) {
  double sum = 0.0;
  size_t count = 0;
  for (int32_t line = kActiveLineStart; line < kActiveLineEnd; ++line) {
    for (int32_t x = kActiveXStart; x < kActiveXEnd; ++x) {
      const double d = a.y(line)[x] - b.y(line)[x];
      sum += d * d;
      ++count;
    }
  }
  return std::sqrt(sum / static_cast<double>(count));
}

bool planes_identical(const ComponentFrame& a, const ComponentFrame& b) {
  return *a.getY() == *b.getY() && *a.getU() == *b.getU() &&
         *a.getV() == *b.getV();
}

// Expected steady-state output/input noise power ratio of the recursion.
double expected_variance_ratio(double strength_db) {
  const double alpha = NoiseReducer::blend_weight_for_strength_db(strength_db);
  return (1.0 - alpha) / (1.0 + alpha);
}

}  // namespace

TEST(NoiseReducerTest, BlendWeight_FollowsStrengthInDecibels) {
  EXPECT_DOUBLE_EQ(NoiseReducer::blend_weight_for_strength_db(0.0), 0.0);
  EXPECT_DOUBLE_EQ(NoiseReducer::blend_weight_for_strength_db(-3.0), 0.0);
  // r = 10^-0.6 = 0.2512; alpha = (1 - r) / (1 + r).
  EXPECT_NEAR(NoiseReducer::blend_weight_for_strength_db(6.0), 0.5985, 1e-3);
  EXPECT_NEAR(NoiseReducer::blend_weight_for_strength_db(12.0), 0.8807, 1e-3);
}

TEST(NoiseReducerTest, SpatialGain_RisesWithStrength_AndSaturates) {
  EXPECT_DOUBLE_EQ(NoiseReducer::spatial_gain_for_strength_db(0.0), 0.0);
  const double g3 = NoiseReducer::spatial_gain_for_strength_db(3.0);
  const double g6 = NoiseReducer::spatial_gain_for_strength_db(6.0);
  const double g9 = NoiseReducer::spatial_gain_for_strength_db(9.0);
  EXPECT_GT(g3, 0.0);
  EXPECT_LT(g3, 1.0);
  EXPECT_GT(g6, g3);
  // The window reaches just under 6 dB, so from there on the whole window
  // mean is used.
  EXPECT_DOUBLE_EQ(g6, 1.0);
  EXPECT_DOUBLE_EQ(g9, 1.0);
  EXPECT_DOUBLE_EQ(NoiseReducer::spatial_gain_for_strength_db(12.0), 1.0);
}

TEST(NoiseReducerTest, LeavesFrameAndStateUntouched_WhenStrengthIsZero) {
  NoiseReducer denoiser(make_settings(0.0));
  std::mt19937 rng(1);

  ComponentFrame first = make_flat_frame();
  add_noise(first, rng, 10.0);
  const ComponentFrame first_copy = first;
  denoiser.filter(first);
  EXPECT_TRUE(planes_identical(first, first_copy));

  ComponentFrame second = make_flat_frame();
  add_noise(second, rng, 10.0);
  const ComponentFrame second_copy = second;
  denoiser.filter(second);
  EXPECT_TRUE(planes_identical(second, second_copy));
  EXPECT_FALSE(denoiser.stats().filtered);
  EXPECT_FALSE(denoiser.stats().temporal);
  EXPECT_DOUBLE_EQ(denoiser.stats().sigma_y, 0.0);
}

TEST(NoiseReducerTest, FiltersFirstFrameSpatially_AndTheSecondTemporally) {
  constexpr double kNoiseSigma = 10.0;
  NoiseReducer denoiser(make_settings(kStrengthDb));
  std::mt19937 rng(2);

  // The first frame has nothing to average with, so it is smoothed within
  // itself: less noise than it came with, about the strength's worth.
  ComponentFrame first = make_flat_frame();
  add_noise(first, rng, kNoiseSigma);
  const ComponentFrame first_copy = first;
  denoiser.filter(first);
  EXPECT_FALSE(planes_identical(first, first_copy));
  EXPECT_TRUE(denoiser.stats().filtered);
  EXPECT_FALSE(denoiser.stats().temporal);
  EXPECT_TRUE(denoiser.stats().discontinuity);
  EXPECT_DOUBLE_EQ(denoiser.stats().sigma_y, 0.0);
  EXPECT_NEAR(denoiser.stats().sample_sigma_y, kNoiseSigma, 0.2 * kNoiseSigma);
  const double first_ratio =
      active_variance(*first.getY(), kLuma) / (kNoiseSigma * kNoiseSigma);
  EXPECT_LT(first_ratio, 0.5);
  EXPECT_GT(first_ratio, 0.1);

  ComponentFrame second = make_flat_frame();
  add_noise(second, rng, kNoiseSigma);
  const ComponentFrame second_copy = second;
  denoiser.filter(second);
  EXPECT_FALSE(planes_identical(second, second_copy));
  EXPECT_TRUE(denoiser.stats().temporal);
  EXPECT_FALSE(denoiser.stats().discontinuity);
  EXPECT_GT(denoiser.stats().sigma_y, 0.0);
}

TEST(NoiseReducerTest, ResetsAndFiltersSpatially_WhenGeometryChanges) {
  NoiseReducer denoiser(make_settings(kStrengthDb));
  std::mt19937 rng(3);

  ComponentFrame a = make_flat_frame();
  add_noise(a, rng, 10.0);
  denoiser.filter(a);
  ComponentFrame b = make_flat_frame();
  add_noise(b, rng, 10.0);
  denoiser.filter(b);
  ASSERT_TRUE(denoiser.stats().temporal);

  // Narrower frame: a discontinuity, then filtered temporally from it.
  ComponentFrame narrow = make_frame(kLuma, kChromaU, kChromaV, false, 32);
  add_noise(narrow, rng, 10.0);
  denoiser.filter(narrow);
  EXPECT_TRUE(denoiser.stats().discontinuity);
  EXPECT_FALSE(denoiser.stats().temporal);

  ComponentFrame narrow2 = make_frame(kLuma, kChromaU, kChromaV, false, 32);
  add_noise(narrow2, rng, 10.0);
  denoiser.filter(narrow2);
  EXPECT_TRUE(denoiser.stats().temporal);

  // Same size but no chroma planes: also a geometry change.
  ComponentFrame mono = make_frame(kLuma, 0.0, 0.0, true, 32);
  add_noise(mono, rng, 10.0);
  denoiser.filter(mono);
  EXPECT_TRUE(denoiser.stats().discontinuity);
  EXPECT_FALSE(denoiser.stats().temporal);
}

TEST(NoiseReducerTest, FiltersNextFrameSpatially_AfterReset) {
  NoiseReducer denoiser(make_settings(kStrengthDb));
  std::mt19937 rng(4);

  for (int i = 0; i < 3; ++i) {
    ComponentFrame frame = make_flat_frame();
    add_noise(frame, rng, 10.0);
    denoiser.filter(frame);
  }
  ASSERT_TRUE(denoiser.stats().temporal);

  denoiser.reset();

  ComponentFrame after_reset = make_flat_frame();
  add_noise(after_reset, rng, 10.0);
  denoiser.filter(after_reset);
  EXPECT_TRUE(denoiser.stats().discontinuity);
  EXPECT_FALSE(denoiser.stats().temporal);

  ComponentFrame next = make_flat_frame();
  add_noise(next, rng, 10.0);
  denoiser.filter(next);
  EXPECT_TRUE(denoiser.stats().temporal);
}

TEST(NoiseReducerTest, EstimatesNoiseScales_WithinTwentyPercent) {
  // The second frame is measured against the first frame's output, which
  // the spatial filter has already quietened: the difference has the sum of
  // the two variances, and the 4 x 3 box mean divides that by 12.
  constexpr double kNoiseSigma = 10.0;

  for (const bool mono : {true, false}) {
    NoiseReducer denoiser(make_settings(kStrengthDb));
    std::mt19937 rng(5);

    ComponentFrame first = make_flat_frame(mono);
    add_noise(first, rng, kNoiseSigma);
    denoiser.filter(first);
    const double previous_variance = active_variance(*first.getY(), kLuma);

    ComponentFrame second = make_flat_frame(mono);
    add_noise(second, rng, kNoiseSigma);
    denoiser.filter(second);
    ASSERT_TRUE(denoiser.stats().temporal) << "mono=" << mono;

    const double expected =
        std::sqrt((kNoiseSigma * kNoiseSigma + previous_variance) / 12.0);
    EXPECT_NEAR(denoiser.stats().sigma_y, expected, 0.2 * expected)
        << "mono=" << mono;
    EXPECT_NEAR(denoiser.stats().sample_sigma_y, kNoiseSigma, 0.2 * kNoiseSigma)
        << "mono=" << mono;
    if (mono) {
      EXPECT_DOUBLE_EQ(denoiser.stats().sigma_u, 0.0);
      EXPECT_DOUBLE_EQ(denoiser.stats().sigma_v, 0.0);
      EXPECT_DOUBLE_EQ(denoiser.stats().sample_sigma_u, 0.0);
    } else {
      EXPECT_NEAR(denoiser.stats().sigma_u, expected, 0.2 * expected);
      EXPECT_NEAR(denoiser.stats().sigma_v, expected, 0.2 * expected);
      EXPECT_NEAR(denoiser.stats().sample_sigma_u, kNoiseSigma,
                  0.3 * kNoiseSigma);
    }
  }
}

TEST(NoiseReducerTest, IgnoresResidualSubcarrier_RepeatingEveryFourSamples) {
  NoiseReducer denoiser(make_settings(kStrengthDb));
  const double alpha = NoiseReducer::blend_weight_for_strength_db(kStrengthDb);

  ComponentFrame first = make_flat_frame();
  denoiser.filter(first);

  // A 4fsc residual: any four consecutive samples sum to zero.
  constexpr double kPattern[4] = {24.0, 8.0, -24.0, -8.0};
  ComponentFrame second = make_flat_frame();
  for (int32_t line = 0; line < second.getHeight(); ++line) {
    double* y = second.y(line);
    for (int32_t x = 0; x < kWidth; ++x) y[x] += kPattern[x % 4];
  }
  denoiser.filter(second);

  ASSERT_TRUE(denoiser.stats().temporal);
  EXPECT_DOUBLE_EQ(denoiser.stats().moving_fraction, 0.0);
  // No motion measured anywhere, so sigma sits at its lower clamp.
  EXPECT_LE(denoiser.stats().sigma_y, 0.25);

  // The residual is treated as still picture and attenuated by (1 - alpha).
  for (int32_t line = kActiveLineStart; line < kActiveLineEnd; ++line) {
    const double* y = second.y(line);
    for (int32_t x = kActiveXStart; x < kActiveXEnd; ++x) {
      EXPECT_NEAR(y[x], kLuma + (1.0 - alpha) * kPattern[x % 4], kExact)
          << "line " << line << " x " << x;
    }
  }
}

TEST(NoiseReducerTest, GivesChromaTheSameWeightAsLuma) {
  NoiseReducer denoiser(make_settings(kStrengthDb));
  const double alpha = NoiseReducer::blend_weight_for_strength_db(kStrengthDb);

  ComponentFrame first = make_flat_frame();
  denoiser.filter(first);

  // A change well below the still threshold on every plane.
  constexpr double kStep = 0.5;
  ComponentFrame second =
      make_frame(kLuma + kStep, kChromaU + kStep, kChromaV + kStep);
  denoiser.filter(second);
  ASSERT_TRUE(denoiser.stats().temporal);

  const double expected_step = (1.0 - alpha) * kStep;
  for (int32_t line = kActiveLineStart; line < kActiveLineEnd; ++line) {
    const double* y = second.y(line);
    const double* u = second.u(line);
    const double* v = second.v(line);
    for (int32_t x = kActiveXStart; x < kActiveXEnd; ++x) {
      EXPECT_NEAR(y[x], kLuma + expected_step, kExact);
      EXPECT_NEAR(u[x], kChromaU + expected_step, kExact);
      EXPECT_NEAR(v[x], kChromaV + expected_step, kExact);
    }
  }

  // Outside the active rectangle nothing is touched.
  EXPECT_DOUBLE_EQ(second.y(kActiveLineStart)[kActiveXStart - 1],
                   kLuma + kStep);
  EXPECT_DOUBLE_EQ(second.u(kActiveLineEnd)[kActiveXStart], kChromaU + kStep);
}

TEST(NoiseReducerTest, ConvergesToExpectedNoiseVariance_OnStillPicture) {
  constexpr double kNoiseSigma = 10.0;
  constexpr int kFrames = 40;
  const double expected_ratio = expected_variance_ratio(kStrengthDb);

  for (const bool mono : {true, false}) {
    NoiseReducer denoiser(make_settings(kStrengthDb));
    std::mt19937 rng(6);
    ComponentFrame frame;
    for (int i = 0; i < kFrames; ++i) {
      frame = make_flat_frame(mono);
      add_noise(frame, rng, kNoiseSigma);
      denoiser.filter(frame);
    }
    ASSERT_TRUE(denoiser.stats().temporal) << "mono=" << mono;
    EXPECT_FALSE(denoiser.stats().discontinuity) << "mono=" << mono;

    const double input_variance = kNoiseSigma * kNoiseSigma;
    const double ratio_y =
        active_variance(*frame.getY(), kLuma) / input_variance;
    EXPECT_NEAR(ratio_y, expected_ratio, 0.1 * expected_ratio)
        << "mono=" << mono;
    if (!mono) {
      const double ratio_u =
          active_variance(*frame.getU(), kChromaU) / input_variance;
      const double ratio_v =
          active_variance(*frame.getV(), kChromaV) / input_variance;
      EXPECT_NEAR(ratio_u, expected_ratio, 0.1 * expected_ratio);
      EXPECT_NEAR(ratio_v, expected_ratio, 0.1 * expected_ratio);
    }
  }
}

TEST(NoiseReducerTest, SpatialFilter_ReducesFlatNoiseByTheStrength) {
  // A lone frame (no previous frame) is filtered spatially only. On flat
  // noisy picture the noise power must fall by about the strength up to the
  // window's reach of just under 6 dB, beyond which it saturates. The range
  // is read from 1200 samples a plane on this small frame, so allow a
  // quarter.
  constexpr double kNoiseSigma = 10.0;
  double ratio_at_6db = 0.0;
  for (const double strength_db : {3.0, 6.0, 9.0}) {
    NoiseReducer denoiser(make_settings(strength_db));
    std::mt19937 rng(9);
    ComponentFrame frame = make_flat_frame();
    add_noise(frame, rng, kNoiseSigma);
    denoiser.filter(frame);
    ASSERT_TRUE(denoiser.stats().discontinuity);

    const double ratio_y =
        active_variance(*frame.getY(), kLuma) / (kNoiseSigma * kNoiseSigma);
    const double ratio_u =
        active_variance(*frame.getU(), kChromaU) / (kNoiseSigma * kNoiseSigma);
    if (strength_db <= 6.0) {
      const double expected = std::pow(10.0, -strength_db / 10.0);
      EXPECT_NEAR(ratio_y, expected, 0.25 * expected) << strength_db << " dB";
      EXPECT_NEAR(ratio_u, expected, 0.25 * expected) << strength_db << " dB";
      if (strength_db == 6.0) ratio_at_6db = ratio_y;
    } else {
      EXPECT_NEAR(ratio_y, ratio_at_6db, 0.1 * ratio_at_6db)
          << strength_db << " dB";
    }
  }
}

TEST(NoiseReducerTest, SpatialFilter_KeepsAnEdgeAboveTheNoise) {
  // A vertical luma edge of 300 codes in 10 codes of noise: the samples on
  // each side of it must stay on their own side (the window excludes
  // neighbours further than a few noise scales away), and the flat picture
  // either side is still smoothed.
  constexpr double kNoiseSigma = 10.0;
  constexpr double kLeft = 200.0;
  constexpr double kRight = 500.0;
  constexpr int32_t kEdge = 32;

  NoiseReducer denoiser(make_settings(kStrengthDb));
  std::mt19937 rng(10);
  ComponentFrame frame = make_flat_frame();
  for (int32_t line = 0; line < frame.getHeight(); ++line) {
    double* y = frame.y(line);
    for (int32_t x = 0; x < kWidth; ++x) y[x] = x < kEdge ? kLeft : kRight;
  }
  add_noise(frame, rng, kNoiseSigma);
  denoiser.filter(frame);
  ASSERT_TRUE(denoiser.stats().discontinuity);

  double left_sum = 0.0;
  double right_sum = 0.0;
  size_t count = 0;
  for (int32_t line = kActiveLineStart; line < kActiveLineEnd; ++line) {
    const double* y = frame.y(line);
    EXPECT_NEAR(y[kEdge - 1], kLeft, 4.0 * kNoiseSigma) << "line " << line;
    EXPECT_NEAR(y[kEdge], kRight, 4.0 * kNoiseSigma) << "line " << line;
    left_sum += (y[kEdge - 1] - kLeft) * (y[kEdge - 1] - kLeft);
    right_sum += (y[kEdge] - kRight) * (y[kEdge] - kRight);
    ++count;
  }
  // The edge samples are smoothed along the edge, not across it: their
  // noise still drops.
  EXPECT_LT(left_sum / static_cast<double>(count),
            0.7 * kNoiseSigma * kNoiseSigma);
  EXPECT_LT(right_sum / static_cast<double>(count),
            0.7 * kNoiseSigma * kNoiseSigma);
}

TEST(NoiseReducerTest, LeavesMovingEdgeFreeOfTrails) {
  constexpr double kBackground = 100.0;
  constexpr double kBar = 700.0;
  constexpr int32_t kBarWidth = 8;
  constexpr int32_t kBarStep = 4;
  constexpr int32_t kBarStart = 20;

  const auto paint_bar = [](ComponentFrame& frame, int32_t start) {
    for (int32_t line = 0; line < frame.getHeight(); ++line) {
      double* y = frame.y(line);
      for (int32_t x = start; x < start + kBarWidth; ++x) y[x] = kBar;
    }
  };

  // Noise-free: the whole frame must come out exactly as it went in.
  {
    NoiseReducer denoiser(make_settings(kStrengthDb));
    ComponentFrame first = make_frame(kBackground, kChromaU, kChromaV);
    paint_bar(first, kBarStart);
    denoiser.filter(first);

    ComponentFrame second = make_frame(kBackground, kChromaU, kChromaV);
    paint_bar(second, kBarStart + kBarStep);
    const ComponentFrame second_copy = second;
    denoiser.filter(second);

    EXPECT_TRUE(denoiser.stats().temporal);
    EXPECT_FALSE(denoiser.stats().discontinuity);
    EXPECT_TRUE(planes_identical(second, second_copy));
  }

  // Noisy: the samples the edge vacated and the samples it arrived on get no
  // temporal weight at all, so they carry no trace of the bar's old
  // position (a noise sample beyond the spatial range is kept as it is, so
  // allow five sigma), while the still background is smoothed.
  {
    constexpr double kNoiseSigma = 5.0;
    NoiseReducer denoiser(make_settings(kStrengthDb));
    std::mt19937 rng(7);
    ComponentFrame first = make_frame(kBackground, kChromaU, kChromaV);
    paint_bar(first, kBarStart);
    add_noise(first, rng, kNoiseSigma);
    denoiser.filter(first);

    ComponentFrame second = make_frame(kBackground, kChromaU, kChromaV);
    paint_bar(second, kBarStart + kBarStep);
    add_noise(second, rng, kNoiseSigma);
    const ComponentFrame second_copy = second;
    denoiser.filter(second);

    ASSERT_TRUE(denoiser.stats().temporal);
    for (int32_t line = kActiveLineStart; line < kActiveLineEnd; ++line) {
      for (int32_t x = kBarStart; x < kBarStart + kBarStep; ++x) {
        EXPECT_NEAR(second.y(line)[x], kBackground, 5.0 * kNoiseSigma)
            << "line " << line << " x " << x;
      }
      for (int32_t x = kBarStart + kBarWidth;
           x < kBarStart + kBarWidth + kBarStep; ++x) {
        EXPECT_NEAR(second.y(line)[x], kBar, 5.0 * kNoiseSigma)
            << "line " << line << " x " << x;
      }
    }
    // The still background away from the bar was blended.
    EXPECT_NE(second.y(kActiveLineStart + 10)[kActiveXEnd - 2],
              second_copy.y(kActiveLineStart + 10)[kActiveXEnd - 2]);
  }
}

TEST(NoiseReducerTest, FiltersSceneCutSpatially_AndRecoversAfterIt) {
  constexpr double kNoiseSigma = 10.0;
  constexpr double kAfterCutLuma = 700.0;
  const double expected_ratio = expected_variance_ratio(kStrengthDb);

  NoiseReducer denoiser(make_settings(kStrengthDb));
  std::mt19937 rng(8);

  for (int i = 0; i < 10; ++i) {
    ComponentFrame frame = make_flat_frame();
    add_noise(frame, rng, kNoiseSigma);
    denoiser.filter(frame);
  }
  ASSERT_TRUE(denoiser.stats().temporal);

  // The cut frame is smoothed within itself and carries nothing of the
  // picture before it.
  ComponentFrame cut = make_frame(kAfterCutLuma, kChromaU, kChromaV);
  add_noise(cut, rng, kNoiseSigma);
  const ComponentFrame cut_copy = cut;
  denoiser.filter(cut);
  EXPECT_FALSE(planes_identical(cut, cut_copy));
  EXPECT_TRUE(denoiser.stats().discontinuity);
  EXPECT_FALSE(denoiser.stats().temporal);
  EXPECT_GT(denoiser.stats().moving_fraction, 0.5);
  EXPECT_GT(denoiser.stats().scale_ratio, 2.0);
  EXPECT_NEAR(active_mean(*cut.getY()), kAfterCutLuma, 1.0);
  EXPECT_LT(active_variance(*cut.getY(), kAfterCutLuma),
            0.5 * kNoiseSigma * kNoiseSigma);

  // The next frame blends with the cut frame, not with the picture before it.
  ComponentFrame after = make_frame(kAfterCutLuma, kChromaU, kChromaV);
  add_noise(after, rng, kNoiseSigma);
  denoiser.filter(after);
  EXPECT_TRUE(denoiser.stats().temporal);
  EXPECT_FALSE(denoiser.stats().discontinuity);
  EXPECT_NEAR(active_mean(*after.getY()), kAfterCutLuma, 1.0);

  ComponentFrame frame;
  for (int i = 0; i < 30; ++i) {
    frame = make_frame(kAfterCutLuma, kChromaU, kChromaV);
    add_noise(frame, rng, kNoiseSigma);
    denoiser.filter(frame);
  }
  const double ratio = active_variance(*frame.getY(), kAfterCutLuma) /
                       (kNoiseSigma * kNoiseSigma);
  EXPECT_NEAR(ratio, expected_ratio, 0.1 * expected_ratio);
}

TEST(NoiseReducerTest, TreatsUnrelatedStills_AsDiscontinuities) {
  // A run of different pictures, each with its own noise, as a disc of
  // stills decodes. Every one must be filtered within itself: closer to its
  // own clean picture than the noisy input was, with none of the previous
  // picture blended in.
  constexpr double kNoiseSigma = 10.0;
  NoiseReducer denoiser(make_settings(kStrengthDb));
  std::mt19937 rng(11);

  for (int i = 0; i < 6; ++i) {
    const ComponentFrame clean = make_block_picture(rng);
    ComponentFrame frame = clean;
    add_noise(frame, rng, kNoiseSigma);
    const ComponentFrame noisy = frame;
    denoiser.filter(frame);

    EXPECT_TRUE(denoiser.stats().discontinuity) << "frame " << i;
    EXPECT_FALSE(denoiser.stats().temporal) << "frame " << i;
    const double input_error = active_rms_difference(noisy, clean);
    const double output_error = active_rms_difference(frame, clean);
    EXPECT_LT(output_error, 0.8 * input_error) << "frame " << i;
  }

  // The same picture again, continuing: now the frames average.
  const ComponentFrame still = make_block_picture(rng);
  for (int i = 0; i < 3; ++i) {
    ComponentFrame frame = still;
    add_noise(frame, rng, kNoiseSigma);
    denoiser.filter(frame);
  }
  EXPECT_TRUE(denoiser.stats().temporal);
  EXPECT_FALSE(denoiser.stats().discontinuity);
}

TEST(NoiseReducerTest, WithoutSpatial_LeavesUnrelatedStillsAsTheyCame) {
  // The still-only mode: a disc of stills passes through untouched, and a
  // held picture still averages.
  constexpr double kNoiseSigma = 10.0;
  NoiseReducer::Settings settings = make_settings(kStrengthDb);
  settings.spatial = false;
  NoiseReducer denoiser(settings);
  std::mt19937 rng(11);

  for (int i = 0; i < 6; ++i) {
    ComponentFrame frame = make_block_picture(rng);
    add_noise(frame, rng, kNoiseSigma);
    const ComponentFrame noisy = frame;
    denoiser.filter(frame);
    EXPECT_TRUE(denoiser.stats().discontinuity) << "frame " << i;
    EXPECT_TRUE(planes_identical(frame, noisy)) << "frame " << i;
  }

  const double expected_ratio = expected_variance_ratio(kStrengthDb);
  ComponentFrame frame;
  for (int i = 0; i < 40; ++i) {
    frame = make_flat_frame();
    add_noise(frame, rng, kNoiseSigma);
    denoiser.filter(frame);
  }
  ASSERT_TRUE(denoiser.stats().temporal);
  const double ratio =
      active_variance(*frame.getY(), kLuma) / (kNoiseSigma * kNoiseSigma);
  EXPECT_NEAR(ratio, expected_ratio, 0.1 * expected_ratio);
}

TEST(NoiseReducerTest, WithoutSpatial_LeavesMovingPictureAsItCame) {
  constexpr double kNoiseSigma = 10.0;
  constexpr double kMovedLuma = 700.0;
  constexpr int32_t kMovedFirst = 100;
  constexpr int32_t kMovedLast = 200;
  NoiseReducer::Settings settings = make_settings(kStrengthDb);
  settings.spatial = false;
  NoiseReducer denoiser(settings);
  std::mt19937 rng(12);

  for (int i = 0; i < 10; ++i) {
    ComponentFrame frame = make_flat_frame();
    add_noise(frame, rng, kNoiseSigma);
    denoiser.filter(frame);
  }

  // A quarter of the picture changes: the rest still averages, the change
  // keeps its input samples exactly, clear of the motion box at its edges.
  ComponentFrame frame = make_flat_frame();
  for (int32_t line = kMovedFirst; line <= kMovedLast; ++line) {
    std::fill(frame.y(line), frame.y(line) + kWidth, kMovedLuma);
  }
  add_noise(frame, rng, kNoiseSigma);
  const ComponentFrame noisy = frame;
  denoiser.filter(frame);
  ASSERT_TRUE(denoiser.stats().temporal);
  for (int32_t line = kMovedFirst + 8; line <= kMovedLast - 8; ++line) {
    for (int32_t x = kActiveXStart + 4; x < kActiveXEnd - 4; ++x) {
      ASSERT_EQ(frame.y(line)[x], noisy.y(line)[x])
          << "line " << line << " sample " << x;
    }
  }
  EXPECT_LT(active_rms_difference(frame, make_flat_frame()),
            active_rms_difference(noisy, make_flat_frame()));
}

TEST(NoiseReducerTest, MayNotContinue_MakesAStillFrameADiscontinuity) {
  constexpr double kNoiseSigma = 10.0;
  NoiseReducer::Settings settings = make_settings(kStrengthDb);
  settings.spatial = false;
  NoiseReducer denoiser(settings);
  std::mt19937 rng(13);
  for (int i = 0; i < 5; ++i) {
    ComponentFrame frame = make_flat_frame();
    add_noise(frame, rng, kNoiseSigma);
    denoiser.filter(frame);
  }
  ASSERT_TRUE(denoiser.stats().temporal);

  // The same picture, but the caller says it is not a continuation.
  ComponentFrame frame = make_flat_frame();
  add_noise(frame, rng, kNoiseSigma);
  const ComponentFrame noisy = frame;
  denoiser.filter(frame, /*may_continue=*/false);
  EXPECT_TRUE(denoiser.stats().discontinuity);
  EXPECT_TRUE(planes_identical(frame, noisy));

  // And the next frame continues from it.
  ComponentFrame next = make_flat_frame();
  add_noise(next, rng, kNoiseSigma);
  denoiser.filter(next);
  EXPECT_TRUE(denoiser.stats().temporal);
}

TEST(NoiseReducerTest, MotionScale_RaisesTheMotionThresholds) {
  // A luma step of 3 codes on a tenth of the picture, against noise-free
  // history, is 12 noise units (sigma clamps at 0.25): fully moving at
  // motion_scale 1 (thresholds 2.5 and 5), partly still at motion_scale 4
  // (thresholds 10 and 20), so the output differs between the two. The step
  // covers a minority of the picture so it does not shift the noise
  // percentile, and the stepped band is flat inside, so the spatial filter
  // leaves it as it is.
  constexpr double kStep = 3.0;
  constexpr int32_t kStepLines = 40;
  const auto paint_step = [](ComponentFrame& frame) {
    for (int32_t line = kActiveLineStart; line < kActiveLineStart + kStepLines;
         ++line) {
      double* y = frame.y(line);
      for (int32_t x = 0; x < kWidth; ++x) y[x] += kStep;
    }
  };

  NoiseReducer strict(make_settings(kStrengthDb, 1.0));
  NoiseReducer lenient(make_settings(kStrengthDb, 4.0));
  for (NoiseReducer* denoiser : {&strict, &lenient}) {
    ComponentFrame first = make_flat_frame();
    denoiser->filter(first);
  }

  ComponentFrame strict_frame = make_flat_frame();
  paint_step(strict_frame);
  strict.filter(strict_frame);
  ComponentFrame lenient_frame = make_flat_frame();
  paint_step(lenient_frame);
  lenient.filter(lenient_frame);

  const double* strict_y = strict_frame.y(kActiveLineStart + 5);
  const double* lenient_y = lenient_frame.y(kActiveLineStart + 5);
  EXPECT_DOUBLE_EQ(strict_y[kActiveXStart + 5], kLuma + kStep);
  EXPECT_LT(lenient_y[kActiveXStart + 5], kLuma + kStep);
  EXPECT_GT(lenient_y[kActiveXStart + 5], kLuma);
}

TEST(NoiseReducerTest, SplitsIntoLineBands_WithoutChangingTheOutput) {
  // The same noisy sequence with a moving bar through one thread and through
  // seven (an uneven split of the 400 active lines, so bands start and end
  // on arbitrary rows, the top and bottom bands carry the inward-sliding
  // vertical window, and the spatial windows at every band edge read rows
  // the neighbouring band is blending) must give bit-identical frames and
  // statistics. The first frame is spatially filtered throughout, the rest
  // around the bar.
  constexpr double kNoiseSigma = 8.0;
  constexpr double kBarLuma = 700.0;
  constexpr int32_t kBarWidth = 8;
  constexpr int32_t kBarStep = 4;
  constexpr int32_t kFrames = 6;

  NoiseReducer::Settings banded_settings = make_settings(kStrengthDb);
  banded_settings.threads = 7;
  NoiseReducer serial(make_settings(kStrengthDb));
  NoiseReducer banded(banded_settings);

  std::mt19937 rng(21);
  for (int32_t n = 0; n < kFrames; ++n) {
    ComponentFrame frame = make_flat_frame();
    const int32_t bar_start = kActiveXStart + kBarStep * n;
    for (int32_t line = kActiveLineStart; line < kActiveLineEnd; ++line) {
      double* y = frame.y(line);
      for (int32_t x = bar_start; x < bar_start + kBarWidth; ++x) {
        y[x] = kBarLuma;
      }
    }
    add_noise(frame, rng, kNoiseSigma);
    ComponentFrame copy = frame;

    serial.filter(frame);
    banded.filter(copy);

    EXPECT_TRUE(planes_identical(frame, copy)) << "frame " << n;
    EXPECT_EQ(serial.stats().temporal, banded.stats().temporal);
    EXPECT_EQ(serial.stats().discontinuity, banded.stats().discontinuity);
    EXPECT_DOUBLE_EQ(serial.stats().sigma_y, banded.stats().sigma_y);
    EXPECT_DOUBLE_EQ(serial.stats().sigma_u, banded.stats().sigma_u);
    EXPECT_DOUBLE_EQ(serial.stats().sample_sigma_y,
                     banded.stats().sample_sigma_y);
    EXPECT_DOUBLE_EQ(serial.stats().moving_fraction,
                     banded.stats().moving_fraction);
  }
  EXPECT_TRUE(serial.stats().temporal);
}

}  // namespace orc_unit_test
