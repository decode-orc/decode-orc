/*
 * File:        temporal_denoiser_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the component-frame temporal denoiser
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/plugins/stages/sinks/common/decoders/temporal_denoiser.h"

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

TemporalDenoiser::Settings make_settings(double strength_db,
                                         double motion_scale = 1.0) {
  TemporalDenoiser::Settings s;
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

bool planes_identical(const ComponentFrame& a, const ComponentFrame& b) {
  return *a.getY() == *b.getY() && *a.getU() == *b.getU() &&
         *a.getV() == *b.getV();
}

// Expected steady-state output/input noise power ratio of the recursion.
double expected_variance_ratio(double strength_db) {
  const double alpha =
      TemporalDenoiser::blend_weight_for_strength_db(strength_db);
  return (1.0 - alpha) / (1.0 + alpha);
}

}  // namespace

TEST(TemporalDenoiserTest, BlendWeight_FollowsStrengthInDecibels) {
  EXPECT_DOUBLE_EQ(TemporalDenoiser::blend_weight_for_strength_db(0.0), 0.0);
  EXPECT_DOUBLE_EQ(TemporalDenoiser::blend_weight_for_strength_db(-3.0), 0.0);
  // r = 10^-0.6 = 0.2512; alpha = (1 - r) / (1 + r).
  EXPECT_NEAR(TemporalDenoiser::blend_weight_for_strength_db(6.0), 0.5985,
              1e-3);
  EXPECT_NEAR(TemporalDenoiser::blend_weight_for_strength_db(12.0), 0.8807,
              1e-3);
}

TEST(TemporalDenoiserTest, LeavesFrameAndStateUntouched_WhenStrengthIsZero) {
  TemporalDenoiser denoiser(make_settings(0.0));
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
  EXPECT_DOUBLE_EQ(denoiser.stats().sigma_y, 0.0);
}

TEST(TemporalDenoiserTest, PassesFirstFrameThrough_AndFiltersTheSecond) {
  TemporalDenoiser denoiser(make_settings(kStrengthDb));
  std::mt19937 rng(2);

  ComponentFrame first = make_flat_frame();
  add_noise(first, rng, 10.0);
  const ComponentFrame first_copy = first;
  denoiser.filter(first);
  EXPECT_TRUE(planes_identical(first, first_copy));
  EXPECT_FALSE(denoiser.stats().filtered);
  EXPECT_FALSE(denoiser.stats().cut_detected);

  ComponentFrame second = make_flat_frame();
  add_noise(second, rng, 10.0);
  const ComponentFrame second_copy = second;
  denoiser.filter(second);
  EXPECT_FALSE(planes_identical(second, second_copy));
  EXPECT_TRUE(denoiser.stats().filtered);
  EXPECT_FALSE(denoiser.stats().cut_detected);
  EXPECT_GT(denoiser.stats().sigma_y, 0.0);
}

TEST(TemporalDenoiserTest, ResetsAndPassesThrough_WhenGeometryChanges) {
  TemporalDenoiser denoiser(make_settings(kStrengthDb));
  std::mt19937 rng(3);

  ComponentFrame a = make_flat_frame();
  add_noise(a, rng, 10.0);
  denoiser.filter(a);
  ComponentFrame b = make_flat_frame();
  add_noise(b, rng, 10.0);
  denoiser.filter(b);
  ASSERT_TRUE(denoiser.stats().filtered);

  // Narrower frame: pass through, then filter again from it.
  ComponentFrame narrow = make_frame(kLuma, kChromaU, kChromaV, false, 32);
  add_noise(narrow, rng, 10.0);
  const ComponentFrame narrow_copy = narrow;
  denoiser.filter(narrow);
  EXPECT_TRUE(planes_identical(narrow, narrow_copy));
  EXPECT_FALSE(denoiser.stats().filtered);

  ComponentFrame narrow2 = make_frame(kLuma, kChromaU, kChromaV, false, 32);
  add_noise(narrow2, rng, 10.0);
  denoiser.filter(narrow2);
  EXPECT_TRUE(denoiser.stats().filtered);

  // Same size but no chroma planes: also a geometry change.
  ComponentFrame mono = make_frame(kLuma, 0.0, 0.0, true, 32);
  add_noise(mono, rng, 10.0);
  const ComponentFrame mono_copy = mono;
  denoiser.filter(mono);
  EXPECT_TRUE(planes_identical(mono, mono_copy));
  EXPECT_FALSE(denoiser.stats().filtered);
}

TEST(TemporalDenoiserTest, PassesNextFrameThrough_AfterReset) {
  TemporalDenoiser denoiser(make_settings(kStrengthDb));
  std::mt19937 rng(4);

  for (int i = 0; i < 3; ++i) {
    ComponentFrame frame = make_flat_frame();
    add_noise(frame, rng, 10.0);
    denoiser.filter(frame);
  }
  ASSERT_TRUE(denoiser.stats().filtered);

  denoiser.reset();

  ComponentFrame after_reset = make_flat_frame();
  add_noise(after_reset, rng, 10.0);
  const ComponentFrame after_reset_copy = after_reset;
  denoiser.filter(after_reset);
  EXPECT_TRUE(planes_identical(after_reset, after_reset_copy));
  EXPECT_FALSE(denoiser.stats().filtered);

  ComponentFrame next = make_flat_frame();
  add_noise(next, rng, 10.0);
  denoiser.filter(next);
  EXPECT_TRUE(denoiser.stats().filtered);
}

TEST(TemporalDenoiserTest, EstimatesNoiseScale_WithinTwentyPercent) {
  // Two independent noisy frames: the difference has variance 2 sigma^2 and
  // the 4 x 3 box mean divides that by 12.
  constexpr double kNoiseSigma = 10.0;
  const double expected = kNoiseSigma * std::sqrt(2.0 / 12.0);

  for (const bool mono : {true, false}) {
    TemporalDenoiser denoiser(make_settings(kStrengthDb));
    std::mt19937 rng(5);

    ComponentFrame first = make_flat_frame(mono);
    add_noise(first, rng, kNoiseSigma);
    denoiser.filter(first);
    ComponentFrame second = make_flat_frame(mono);
    add_noise(second, rng, kNoiseSigma);
    denoiser.filter(second);

    ASSERT_TRUE(denoiser.stats().filtered) << "mono=" << mono;
    EXPECT_NEAR(denoiser.stats().sigma_y, expected, 0.2 * expected)
        << "mono=" << mono;
    if (mono) {
      EXPECT_DOUBLE_EQ(denoiser.stats().sigma_u, 0.0);
      EXPECT_DOUBLE_EQ(denoiser.stats().sigma_v, 0.0);
    } else {
      EXPECT_NEAR(denoiser.stats().sigma_u, expected, 0.2 * expected);
      EXPECT_NEAR(denoiser.stats().sigma_v, expected, 0.2 * expected);
    }
  }
}

TEST(TemporalDenoiserTest,
     IgnoresResidualSubcarrier_RepeatingEveryFourSamples) {
  TemporalDenoiser denoiser(make_settings(kStrengthDb));
  const double alpha =
      TemporalDenoiser::blend_weight_for_strength_db(kStrengthDb);

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

  ASSERT_TRUE(denoiser.stats().filtered);
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

TEST(TemporalDenoiserTest, GivesChromaTheSameWeightAsLuma) {
  TemporalDenoiser denoiser(make_settings(kStrengthDb));
  const double alpha =
      TemporalDenoiser::blend_weight_for_strength_db(kStrengthDb);

  ComponentFrame first = make_flat_frame();
  denoiser.filter(first);

  // A change well below the still threshold on every plane.
  constexpr double kStep = 0.5;
  ComponentFrame second =
      make_frame(kLuma + kStep, kChromaU + kStep, kChromaV + kStep);
  denoiser.filter(second);
  ASSERT_TRUE(denoiser.stats().filtered);

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

TEST(TemporalDenoiserTest, ConvergesToExpectedNoiseVariance_OnStillPicture) {
  constexpr double kNoiseSigma = 10.0;
  constexpr int kFrames = 40;
  const double expected_ratio = expected_variance_ratio(kStrengthDb);

  for (const bool mono : {true, false}) {
    TemporalDenoiser denoiser(make_settings(kStrengthDb));
    std::mt19937 rng(6);
    ComponentFrame frame;
    for (int i = 0; i < kFrames; ++i) {
      frame = make_flat_frame(mono);
      add_noise(frame, rng, kNoiseSigma);
      denoiser.filter(frame);
    }
    ASSERT_TRUE(denoiser.stats().filtered) << "mono=" << mono;
    EXPECT_FALSE(denoiser.stats().cut_detected) << "mono=" << mono;

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

TEST(TemporalDenoiserTest, LeavesMovingEdgeUntouched) {
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
    TemporalDenoiser denoiser(make_settings(kStrengthDb));
    ComponentFrame first = make_frame(kBackground, kChromaU, kChromaV);
    paint_bar(first, kBarStart);
    denoiser.filter(first);

    ComponentFrame second = make_frame(kBackground, kChromaU, kChromaV);
    paint_bar(second, kBarStart + kBarStep);
    const ComponentFrame second_copy = second;
    denoiser.filter(second);

    EXPECT_TRUE(denoiser.stats().filtered);
    EXPECT_FALSE(denoiser.stats().cut_detected);
    EXPECT_TRUE(planes_identical(second, second_copy));
  }

  // Noisy: the samples the edge vacated and the samples it arrived on get no
  // blend weight at all, so they equal the input exactly while the still
  // background is smoothed.
  {
    TemporalDenoiser denoiser(make_settings(kStrengthDb));
    std::mt19937 rng(7);
    ComponentFrame first = make_frame(kBackground, kChromaU, kChromaV);
    paint_bar(first, kBarStart);
    add_noise(first, rng, 5.0);
    denoiser.filter(first);

    ComponentFrame second = make_frame(kBackground, kChromaU, kChromaV);
    paint_bar(second, kBarStart + kBarStep);
    add_noise(second, rng, 5.0);
    const ComponentFrame second_copy = second;
    denoiser.filter(second);

    ASSERT_TRUE(denoiser.stats().filtered);
    for (int32_t line = kActiveLineStart; line < kActiveLineEnd; ++line) {
      for (int32_t x = kBarStart; x < kBarStart + kBarStep; ++x) {
        EXPECT_DOUBLE_EQ(second.y(line)[x], second_copy.y(line)[x]);
      }
      for (int32_t x = kBarStart + kBarWidth;
           x < kBarStart + kBarWidth + kBarStep; ++x) {
        EXPECT_DOUBLE_EQ(second.y(line)[x], second_copy.y(line)[x]);
      }
    }
    // The still background away from the bar was blended.
    EXPECT_NE(second.y(kActiveLineStart + 10)[kActiveXEnd - 2],
              second_copy.y(kActiveLineStart + 10)[kActiveXEnd - 2]);
  }
}

TEST(TemporalDenoiserTest, PassesSceneCutThrough_AndRecoversAfterIt) {
  constexpr double kNoiseSigma = 10.0;
  constexpr double kAfterCutLuma = 700.0;
  const double expected_ratio = expected_variance_ratio(kStrengthDb);

  TemporalDenoiser denoiser(make_settings(kStrengthDb));
  std::mt19937 rng(8);

  for (int i = 0; i < 10; ++i) {
    ComponentFrame frame = make_flat_frame();
    add_noise(frame, rng, kNoiseSigma);
    denoiser.filter(frame);
  }
  ASSERT_TRUE(denoiser.stats().filtered);

  ComponentFrame cut = make_frame(kAfterCutLuma, kChromaU, kChromaV);
  add_noise(cut, rng, kNoiseSigma);
  const ComponentFrame cut_copy = cut;
  denoiser.filter(cut);
  EXPECT_TRUE(planes_identical(cut, cut_copy));
  EXPECT_TRUE(denoiser.stats().cut_detected);
  EXPECT_FALSE(denoiser.stats().filtered);
  EXPECT_GT(denoiser.stats().moving_fraction, 0.5);

  // The next frame blends with the cut frame, not with the picture before it.
  ComponentFrame after = make_frame(kAfterCutLuma, kChromaU, kChromaV);
  add_noise(after, rng, kNoiseSigma);
  denoiser.filter(after);
  EXPECT_TRUE(denoiser.stats().filtered);
  EXPECT_FALSE(denoiser.stats().cut_detected);
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

TEST(TemporalDenoiserTest, MotionScale_RaisesTheMotionThresholds) {
  // A luma step of 3 codes on a tenth of the picture, against noise-free
  // history, is 12 noise units (sigma clamps at 0.25): fully moving at
  // motion_scale 1 (thresholds 2.5 and 5), partly still at motion_scale 4
  // (thresholds 10 and 20), so the output differs between the two. The step
  // covers a minority of the picture so it does not shift the noise
  // percentile.
  constexpr double kStep = 3.0;
  constexpr int32_t kStepLines = 40;
  const auto paint_step = [](ComponentFrame& frame) {
    for (int32_t line = kActiveLineStart; line < kActiveLineStart + kStepLines;
         ++line) {
      double* y = frame.y(line);
      for (int32_t x = 0; x < kWidth; ++x) y[x] += kStep;
    }
  };

  TemporalDenoiser strict(make_settings(kStrengthDb, 1.0));
  TemporalDenoiser lenient(make_settings(kStrengthDb, 4.0));
  for (TemporalDenoiser* denoiser : {&strict, &lenient}) {
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

TEST(TemporalDenoiserTest, SplitsIntoLineBands_WithoutChangingTheOutput) {
  // The same noisy sequence with a moving bar through one thread and through
  // seven (an uneven split of the 400 active lines, so bands start and end
  // on arbitrary rows and the top and bottom bands carry the inward-sliding
  // vertical window) must give bit-identical frames and statistics.
  constexpr double kNoiseSigma = 8.0;
  constexpr double kBarLuma = 700.0;
  constexpr int32_t kBarWidth = 8;
  constexpr int32_t kBarStep = 4;
  constexpr int32_t kFrames = 6;

  TemporalDenoiser::Settings banded_settings = make_settings(kStrengthDb);
  banded_settings.threads = 7;
  TemporalDenoiser serial(make_settings(kStrengthDb));
  TemporalDenoiser banded(banded_settings);

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
    EXPECT_EQ(serial.stats().filtered, banded.stats().filtered);
    EXPECT_DOUBLE_EQ(serial.stats().sigma_y, banded.stats().sigma_y);
    EXPECT_DOUBLE_EQ(serial.stats().sigma_u, banded.stats().sigma_u);
    EXPECT_DOUBLE_EQ(serial.stats().moving_fraction,
                     banded.stats().moving_fraction);
  }
  EXPECT_TRUE(serial.stats().filtered);
}

}  // namespace orc_unit_test
