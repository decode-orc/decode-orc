/*
 * File:        channel_calibration_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the channel calibration: the smear fitted to a
 *              synthetic CCIR 17 bar and pulse, no smear where there is none,
 *              the guards that decide between them, and a smear longer than
 *              Channel Equalise accepts reported but never applied
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/core/analysis/channel_equalise/channel_calibration.h"

#include <gtest/gtest.h>
#include <orc/support/channel_equalise.h>

#include <cmath>
#include <vector>

#include "../../../../orc/core/analysis/channel_equalise/channel_calibration_analysis.h"
#include "../../../../orc/core/include/project.h"

namespace orc_unit_test {
namespace {

constexpr double kSamplesPerUs = 4.0 * 4433618.75 / 1.0e6;
constexpr double kStartUs = -3.0;

// The averaged segment of field 1 line 19 from 3 us before the bar edge: a
// 100 IRE bar of 10.1 us and a pulse 4.2 us after it (a raised cosine of 0.2
// us half-amplitude duration, its area 1.5 IRE us inside the fit's bound),
// through a smear of |fraction| / |tau| samples, with a small deterministic
// ripple for noise.
std::vector<double> segment(double fraction, double tau) {
  const auto n = static_cast<size_t>(std::lround(20.0 * kSamplesPerUs));
  std::vector<double> x(n);
  for (size_t i = 0; i < n; ++i) {
    const double t = kStartUs + (static_cast<double>(i) / kSamplesPerUs);
    double v = (t >= 0.0 && t < 10.1) ? 100.0 : 0.0;
    const double d = (t - 14.3) / 0.2;
    if (std::abs(d) < 1.0) v += 7.5 * 0.5 * (1.0 + std::cos(M_PI * d));
    x[i] = v;
  }
  if (fraction > 0.0) {
    const double pole = std::exp(-1.0 / tau);
    double acc = x.front();
    for (double& v : x) {
      acc = (pole * acc) + ((1.0 - pole) * v);
      v = ((1.0 - fraction) * v) + (fraction * acc);
    }
  }
  for (size_t i = 0; i < n; ++i) {
    x[i] += 0.3 * std::sin(static_cast<double>(i) * 2.399);
  }
  return x;
}

TEST(ChannelCalibrationTest, FitsTheSmear) {
  const auto fit = orc::fit_smear_segment(segment(0.24, 6.1), kStartUs);
  ASSERT_TRUE(fit.has_value());
  EXPECT_NEAR(fit->fraction, 0.24, 0.01);
  EXPECT_NEAR(fit->tau_samples, 6.1, 0.4);
  EXPECT_LT(fit->residual_ire, 0.8 * fit->residual_no_smear_ire);
  EXPECT_EQ(orc::judge_smear_fit(fit), orc::SmearVerdict::Smear);
}

TEST(ChannelCalibrationTest, NoSmearWithoutOne) {
  const auto fit = orc::fit_smear_segment(segment(0.0, 1.0), kStartUs);
  ASSERT_TRUE(fit.has_value());
  EXPECT_EQ(orc::judge_smear_fit(fit), orc::SmearVerdict::NoSmear);
}

// A segment that never falls back from the bar has nothing to fit.
TEST(ChannelCalibrationTest, NoBarNoFit) {
  const std::vector<double> held(355, 100.0);
  EXPECT_FALSE(orc::fit_smear_segment(held, kStartUs).has_value());
  EXPECT_EQ(orc::judge_smear_fit(std::nullopt), orc::SmearVerdict::CannotTell);
}

// The guards of tools/domesday2d-bench/equalise.py accept().
TEST(ChannelCalibrationTest, Guards) {
  const auto judge = [](double fraction, double tau, double res, double res0) {
    return orc::judge_smear_fit(orc::SmearFit{fraction, tau, res, res0});
  };
  EXPECT_EQ(judge(0.24, 6.1, 0.5, 1.0), orc::SmearVerdict::Smear);
  // Explains the tails no better than no smear.
  EXPECT_EQ(judge(0.24, 6.1, 0.9, 1.0), orc::SmearVerdict::NoSmear);
  // The degenerate fit of a signal without a smear.
  EXPECT_EQ(judge(0.5, 1.1, 0.95, 1.0), orc::SmearVerdict::NoSmear);
  // At the bounds and explaining the tails: neither.
  EXPECT_EQ(judge(0.5, 1.1, 0.5, 1.0), orc::SmearVerdict::CannotTell);
  EXPECT_EQ(judge(0.2, 1.5, 0.5, 1.0), orc::SmearVerdict::CannotTell);
}

// The fit searches the time constant past the stage's limit; a smear found
// there is told apart from no smear but is not one the stage can take.
TEST(ChannelCalibrationTest, LongSmearIsOutOfRange) {
  const auto fit = orc::fit_smear_segment(segment(0.24, 70.0), kStartUs);
  ASSERT_TRUE(fit.has_value());
  EXPECT_GT(fit->tau_samples, orc::channel_equalise::kMaxTauSamples);
  EXPECT_EQ(orc::judge_smear_fit(fit), orc::SmearVerdict::OutOfRange);
  // Just inside the limit it is a smear.
  EXPECT_EQ(orc::judge_smear_fit(orc::SmearFit{0.24, 49.0, 0.5, 1.0}),
            orc::SmearVerdict::Smear);
  // Not explaining the tails is no smear whatever the time constant.
  EXPECT_EQ(orc::judge_smear_fit(orc::SmearFit{0.24, 70.0, 0.9, 1.0}),
            orc::SmearVerdict::NoSmear);
}

orc::ChannelWindow window(orc::SmearVerdict verdict, double fraction,
                          double tau) {
  orc::ChannelWindow w;
  w.frames = 48;
  w.frames_used = 48;
  w.fit = orc::SmearFit{fraction, tau, 0.5, 1.0};
  w.verdict = verdict;
  return w;
}

TEST(ChannelCalibrationTest, OutOfRangeWindowsLeaveNothingToApply) {
  const auto calibration = orc::combine_channel_windows(
      {window(orc::SmearVerdict::OutOfRange, 0.24, 70.0),
       window(orc::SmearVerdict::OutOfRange, 0.25, 80.0),
       window(orc::SmearVerdict::NoSmear, 0.0, 1.0)});
  EXPECT_EQ(calibration.verdict, orc::SmearVerdict::OutOfRange);
  EXPECT_EQ(calibration.tau_samples, 0.0);

  orc::AnalysisResult result;
  orc::describe_channel_calibration(calibration, result);
  EXPECT_EQ(result.status, orc::AnalysisResult::Success);
  EXPECT_NE(result.summary.find("longer than Channel Equalise"),
            std::string::npos);
  EXPECT_TRUE(result.graphData.empty());
  ASSERT_EQ(result.items.size(), 3u);
  EXPECT_NE(result.items[0].message.find("beyond the stage's limits"),
            std::string::npos);

  orc::ChannelCalibrationAnalysisTool tool;
  const orc::Project project;
  EXPECT_FALSE(tool.applyToGraph(result, project, orc::NodeID{}));
  EXPECT_TRUE(result.parameterChanges.empty());
}

// One window within the limits is the source's smear; the others' fits are
// left out of the median.
TEST(ChannelCalibrationTest, SmearWindowsOutweighOutOfRange) {
  const auto calibration = orc::combine_channel_windows(
      {window(orc::SmearVerdict::OutOfRange, 0.24, 70.0),
       window(orc::SmearVerdict::Smear, 0.24, 6.1),
       window(orc::SmearVerdict::OutOfRange, 0.25, 80.0)});
  EXPECT_EQ(calibration.verdict, orc::SmearVerdict::Smear);
  EXPECT_DOUBLE_EQ(calibration.tau_samples, 6.1);

  orc::AnalysisResult result;
  orc::describe_channel_calibration(calibration, result);
  orc::ChannelCalibrationAnalysisTool tool;
  const orc::Project project;
  ASSERT_TRUE(tool.applyToGraph(result, project, orc::NodeID{}));
  const double tau =
      std::get<double>(result.parameterChanges.at("smear_tau_samples"));
  EXPECT_LE(tau, orc::channel_equalise::kMaxTauSamples);
}

}  // namespace
}  // namespace orc_unit_test
