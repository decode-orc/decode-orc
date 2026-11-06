/*
 * File:        fm_beat_calibration_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the FM beat calibration: the carrier a beat was
 *              cut with found again, and a source without flat colour
 *              reported as having none
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/core/analysis/fm_beat/fm_beat_calibration.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "../include/pal_colour_bars_source.h"

namespace orc_unit_test {
namespace {

// A narrower sweep than the tool's, to keep the test quick.
orc::FmBeatCalibrationOptions quick() {
  orc::FmBeatCalibrationOptions options;
  options.min_carrier_hz = 7.06e6;
  options.max_carrier_hz = 7.12e6;
  return options;
}

TEST(FmBeatCalibrationTest, FindsTheCarrier) {
  PalColourBarsSource::Options options;
  options.frames = 3;
  options.beat_carrier_hz = 7.0915e6;
  options.noise_codes = 4;
  const PalColourBarsSource source(options);
  int last_percent = -1;
  const auto result = orc::calibrate_fm_beat(
      source, quick(), [&](int percent, const std::string&) {
        EXPECT_GE(percent, last_percent);
        last_percent = percent;
        return true;
      });
  EXPECT_TRUE(result.found);
  EXPECT_EQ(result.probed_frames, 3u);
  EXPECT_EQ(result.fields, 6u);
  EXPECT_NEAR(result.carrier_black_hz, 7.0915e6, 500.0);
  EXPECT_GT(result.lines_corrected, result.picture_lines / 2);
  EXPECT_EQ(last_percent, 100);
}

TEST(FmBeatCalibrationTest, CarrierBeyondTheRangeIsNotFound) {
  PalColourBarsSource::Options options;
  options.frames = 3;
  options.beat_carrier_hz = 7.123e6;
  options.noise_codes = 4;
  const PalColourBarsSource source(options);
  // The beat removed rises to the range's last step, the carrier beyond it.
  auto search = quick();
  search.min_carrier_hz = 7.10e6;
  const auto result = orc::calibrate_fm_beat(source, search, {});
  ASSERT_GE(result.sweep.size(), 3u);
  for (size_t s = 0; s + 1 < result.sweep.size(); ++s) {
    EXPECT_LT(result.sweep[s].second, result.sweep.back().second);
  }
  EXPECT_FALSE(result.found);
  EXPECT_TRUE(result.peak_at_edge);
  EXPECT_EQ(result.carrier_black_hz, 0.0);
}

TEST(FmBeatCalibrationTest, RefinedCarrierStaysWithinTheRange) {
  PalColourBarsSource::Options options;
  options.frames = 3;
  options.beat_carrier_hz = 7.1195e6;
  options.noise_codes = 4;
  const PalColourBarsSource source(options);
  const auto search = quick();
  const auto result = orc::calibrate_fm_beat(source, search, {});
  EXPECT_TRUE(result.found);
  EXPECT_FALSE(result.peak_at_edge);
  EXPECT_LE(result.carrier_black_hz, search.max_carrier_hz);
  EXPECT_GE(result.carrier_black_hz, search.min_carrier_hz);
}

// A sweep from |first_mhz| in 2 kHz steps with the energies given.
std::vector<std::pair<double, double>> sweep_of(
    double first_mhz, const std::vector<double>& energies) {
  std::vector<std::pair<double, double>> sweep;
  for (size_t s = 0; s < energies.size(); ++s) {
    sweep.emplace_back((first_mhz + (0.002 * static_cast<double>(s))) * 1.0e6,
                       energies[s]);
  }
  return sweep;
}

TEST(FmBeatSweepPeakTest, PrefersAProminentPeakToABumpOnARisingSlope) {
  // The shape seen on a real source (#368): the carrier's peak at index 3,
  // then a slope rising to the range's end with bumps on it, the highest of
  // them (index 12) above the carrier's peak.
  const auto sweep =
      sweep_of(7.064, {2100, 2900, 4300, 5145, 4871, 3400, 2600, 2800, 3925,
                       4946, 6284, 6605, 6632, 5978, 6311, 7579});
  const auto peak = orc::fm_beat_sweep_peak(sweep);
  ASSERT_TRUE(peak.has_value());
  EXPECT_EQ(*peak, 3u);
}

TEST(FmBeatSweepPeakTest, ASlopeWithNoDipHasNoPeak) {
  const auto sweep = sweep_of(7.0, {1000, 2000, 3000, 4000, 5000});
  EXPECT_FALSE(orc::fm_beat_sweep_peak(sweep).has_value());
}

TEST(FmBeatSweepPeakTest, TheEndsAreNotPeaks) {
  const auto sweep = sweep_of(7.0, {9000, 3000, 4000, 2000, 9500});
  const auto peak = orc::fm_beat_sweep_peak(sweep);
  ASSERT_TRUE(peak.has_value());
  EXPECT_EQ(*peak, 2u);
}

TEST(FmBeatCalibrationTest, NoFlatColourNoFields) {
  PalColourBarsSource::Options options;
  options.colour = false;
  const PalColourBarsSource source(options);
  const auto result = orc::calibrate_fm_beat(source, quick(), {});
  EXPECT_FALSE(result.found);
  EXPECT_EQ(result.fields, 0u);
}

TEST(FmBeatCalibrationTest, Cancels) {
  PalColourBarsSource::Options options;
  options.beat_carrier_hz = 7.0915e6;
  const PalColourBarsSource source(options);
  const auto result = orc::calibrate_fm_beat(
      source, quick(), [](int, const std::string&) { return false; });
  EXPECT_FALSE(result.found);
  EXPECT_TRUE(result.sweep.empty());
}

}  // namespace
}  // namespace orc_unit_test
