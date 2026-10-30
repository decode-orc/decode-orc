/*
 * File:        truth_comparison_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the videosynth suite's truth comparison: band
 *              edges, the per-band error split, the error inside and outside
 *              sample runs, and the gain in dB
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/frame_line_util.h>
#include <truth_comparison.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace orc_unit_test {
namespace {

using orc_videosynth::BandErrorMeter;
using orc_videosynth::RunErrorMeter;
using orc_videosynth::Sample;

constexpr size_t kLine = 1000;

orc::SourceParameters pal_params() {
  orc::SourceParameters p;
  p.system = orc::VideoSystem::PAL;
  p.frame_width_nominal = orc::kPalSamplesPerLineNominal;
  p.frame_height = orc::kPalFrameLines;
  p.blanking_level = orc::kPalBlanking;
  p.white_level = orc::kPalWhite;
  p.active_video_start = orc::kPalActiveVideoStart;
  p.active_video_end = orc::kPalActiveVideoEnd;
  p.first_active_frame_line = orc::kPalFirstActiveFrameLine;
  p.last_active_frame_line = orc::kPalLastActiveFrameLine;
  return p;
}

TEST(VideosynthTruthComparison, palBandEdgesAreThePlansMegahertz) {
  const auto bands = orc_videosynth::composite_bands(orc::VideoSystem::PAL);
  ASSERT_EQ(bands.size(), 3u);
  EXPECT_EQ(bands[0].name, "luma_low");
  EXPECT_DOUBLE_EQ(bands[0].low_hz, 0.0);
  EXPECT_DOUBLE_EQ(bands[0].high_hz, 2.0e6);
  EXPECT_EQ(bands[1].name, "luma_high");
  EXPECT_DOUBLE_EQ(bands[1].high_hz, 3.5e6);
  EXPECT_EQ(bands[2].name, "chroma");
  EXPECT_DOUBLE_EQ(bands[2].high_hz, 5.5e6);
}

TEST(VideosynthTruthComparison, ntscBandEdgesScaleWithTheSubcarrier) {
  const auto bands = orc_videosynth::composite_bands(orc::VideoSystem::NTSC);
  ASSERT_EQ(bands.size(), 3u);
  // 2 MHz x 3.579545 / 4.43361875
  EXPECT_NEAR(bands[0].high_hz, 1.614728e6, 1.0);
  EXPECT_NEAR(bands[1].high_hz, 2.825775e6, 1.0);
  EXPECT_NEAR(bands[2].high_hz, 4.440504e6, 1.0);
}

TEST(VideosynthTruthComparison, ireIsBlankingToWhite) {
  EXPECT_DOUBLE_EQ(orc_videosynth::ire_per_code(pal_params()),
                   100.0 / (844.0 - 256.0));
}

TEST(VideosynthTruthComparison, gainIsTheErrorRatioInDecibels) {
  EXPECT_NEAR(*orc_videosynth::gain_db(2.0, 1.0), 6.0206, 1e-4);
  EXPECT_NEAR(*orc_videosynth::gain_db(1.0, 10.0), -20.0, 1e-12);
  EXPECT_FALSE(orc_videosynth::gain_db(1.0, 0.0).has_value());
  EXPECT_FALSE(orc_videosynth::gain_db(0.0, 1.0).has_value());
}

// An error of 10 codes of DC plus a 100-code sine at a quarter of the sample
// rate (the subcarrier): samples 10, 110, 10, -90. The DC is all in the low
// luma band and the sine all in chroma, exactly.
TEST(VideosynthTruthComparison, bandsSeparateDcFromSubcarrier) {
  std::vector<Sample> truth(kLine, 500);
  std::vector<Sample> output(kLine);
  constexpr Sample kQuarter[] = {10, 110, 10, -90};
  for (size_t n = 0; n < kLine; ++n) output[n] = 500 + kQuarter[n % 4];

  BandErrorMeter meter(orc::VideoSystem::PAL, 1.0);
  meter.add_line(output.data(), truth.data(), kLine);
  const auto rms = meter.rms_ire();
  ASSERT_EQ(rms.size(), 3u);
  EXPECT_NEAR(rms[0], 10.0, 1e-9);
  EXPECT_NEAR(rms[1], 0.0, 1e-9);
  EXPECT_NEAR(rms[2], 100.0 / std::sqrt(2.0), 1e-9);
  EXPECT_NEAR(meter.total_rms_ire(), std::sqrt(100.0 + 5000.0), 1e-9);
}

// A sine at bin 150 of 1000 (2.66 MHz on PAL's 17.73 MHz grid) lands in the
// high luma band; rounding to integer codes leaves a little everywhere.
TEST(VideosynthTruthComparison, midBandSineLandsInHighLuma) {
  std::vector<Sample> truth(kLine, 0);
  std::vector<Sample> output(kLine);
  for (size_t n = 0; n < kLine; ++n) {
    output[n] = static_cast<Sample>(
        std::lround(200.0 * std::sin(2.0 * M_PI * 150.0 * n / kLine)));
  }
  BandErrorMeter meter(orc::VideoSystem::PAL, 1.0);
  meter.add_line(output.data(), truth.data(), kLine);
  const auto rms = meter.rms_ire();
  EXPECT_NEAR(rms[0], 0.0, 0.2);
  EXPECT_NEAR(rms[1], 200.0 / std::sqrt(2.0), 0.2);
  EXPECT_NEAR(rms[2], 0.0, 0.2);
}

// Parseval: the total over all frequencies is the time-domain RMS.
TEST(VideosynthTruthComparison, totalIsTheTimeDomainRms) {
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> code(-300, 300);
  std::vector<Sample> output(911);  // odd: no Nyquist bin
  std::vector<Sample> truth(911);
  double energy = 0.0;
  for (size_t n = 0; n < output.size(); ++n) {
    output[n] = static_cast<Sample>(code(rng));
    truth[n] = static_cast<Sample>(code(rng));
    const double e = output[n] - truth[n];
    energy += e * e;
  }
  BandErrorMeter meter(orc::VideoSystem::NTSC, 0.5);
  meter.add_line(output.data(), truth.data(), output.size());
  EXPECT_NEAR(meter.total_rms_ire(), 0.5 * std::sqrt(energy / output.size()),
              1e-9);
  // The bands cover only part of the spectrum, so they hold less.
  double band_energy = 0.0;
  for (double r : meter.rms_ire()) band_energy += r * r;
  EXPECT_LT(band_energy, meter.total_rms_ire() * meter.total_rms_ire());
}

TEST(VideosynthTruthComparison, emptyMeterReportsZero) {
  BandErrorMeter meter(orc::VideoSystem::PAL, 1.0);
  EXPECT_EQ(meter.total_rms_ire(), 0.0);
  for (double r : meter.rms_ire()) EXPECT_EQ(r, 0.0);
  EXPECT_EQ(RunErrorMeter(1.0).result().inside_samples, 0u);
}

TEST(VideosynthTruthComparison, runErrorSplitsAtTheMask) {
  const std::vector<Sample> truth = {0, 0, 0, 0, 0, 0};
  const std::vector<Sample> output = {3, 4, 1, 1, 1, 1};
  const std::vector<uint8_t> inside = {1, 1, 0, 0, 0, 0};
  RunErrorMeter meter(2.0);
  meter.add_line(output.data(), truth.data(), inside.data(), truth.size());
  const auto split = meter.result();
  EXPECT_EQ(split.inside_samples, 2u);
  EXPECT_EQ(split.outside_samples, 4u);
  EXPECT_NEAR(split.inside_rms_ire, 2.0 * std::sqrt((9.0 + 16.0) / 2.0), 1e-12);
  EXPECT_NEAR(split.outside_rms_ire, 2.0, 1e-12);
}

// A PAL frame whose error is 5 codes over the active picture and 1000 codes
// everywhere else: the frame helpers see only the active picture, 576 lines
// of 948 samples.
TEST(VideosynthTruthComparison, frameHelpersMeasureOnlyTheActivePicture) {
  const auto params = pal_params();
  const size_t width = orc::kPalSamplesPerLineNominal;
  const size_t total = orc::frame_line_sample_offset(
      orc::VideoSystem::PAL, width, orc::kPalFrameLines);
  std::vector<Sample> truth(total, 0);
  std::vector<Sample> output(total, 1000);
  const size_t field1 = orc::kPalField1Lines;
  auto active_line = [&](size_t line) {
    const size_t start =
        orc::frame_line_sample_offset(orc::VideoSystem::PAL, width, line);
    for (int32_t n = params.active_video_start; n < params.active_video_end;
         ++n) {
      output[start + static_cast<size_t>(n)] = 5;
    }
  };
  for (size_t line = 22; line < 310; ++line) {
    active_line(line);
    active_line(field1 + line);
  }
  // One dropout run of 10 samples inside the picture, 20 codes off.
  const size_t run_start =
      orc::frame_line_sample_offset(orc::VideoSystem::PAL, width, 100) + 400;
  for (size_t n = 0; n < 10; ++n) output[run_start + n] = 20;
  orc::DropoutRun run;
  run.sample_start = run_start;
  run.sample_count = 10;

  BandErrorMeter bands(orc::VideoSystem::PAL, 1.0);
  bands.add_frame(output.data(), truth.data(), params);
  const double active = 576.0 * 948.0;
  EXPECT_NEAR(bands.total_rms_ire(),
              std::sqrt(((active - 10.0) * 25.0 + 10.0 * 400.0) / active),
              1e-9);

  RunErrorMeter runs(1.0);
  runs.add_frame(output.data(), truth.data(), total, {run}, params);
  const auto split = runs.result();
  EXPECT_EQ(split.inside_samples, 10u);
  EXPECT_EQ(split.outside_samples, 576u * 948u - 10u);
  EXPECT_NEAR(split.inside_rms_ire, 20.0, 1e-12);
  EXPECT_NEAR(split.outside_rms_ire, 5.0, 1e-12);
}

TEST(VideosynthTruthComparison, damageRunsAreTheDifferingSamplesPerLine) {
  const auto params = pal_params();
  const size_t width = orc::kPalSamplesPerLineNominal;
  const size_t total = orc::frame_line_sample_offset(
      orc::VideoSystem::PAL, width, orc::kPalFrameLines);
  std::vector<Sample> truth(total, 300);
  std::vector<Sample> capture = truth;
  // One run of 5 inside line 100, and one across the end of line 200 into
  // line 201, which is reported as two runs.
  const size_t a =
      orc::frame_line_sample_offset(orc::VideoSystem::PAL, width, 100) + 400;
  for (size_t n = 0; n < 5; ++n) capture[a + n] = 64;
  const size_t b =
      orc::frame_line_sample_offset(orc::VideoSystem::PAL, width, 201) - 3;
  for (size_t n = 0; n < 6; ++n) capture[b + n] = 64;

  const auto runs = orc_videosynth::damage_runs(capture.data(), truth.data(),
                                                params, total, 7);
  ASSERT_EQ(runs.size(), 3u);
  EXPECT_EQ(runs[0].sample_start, a);
  EXPECT_EQ(runs[0].sample_count, 5u);
  EXPECT_EQ(runs[0].frame_id, 7u);
  EXPECT_EQ(runs[1].sample_start, b);
  EXPECT_EQ(runs[1].sample_count, 3u);
  EXPECT_EQ(runs[2].sample_start, b + 3);
  EXPECT_EQ(runs[2].sample_count, 3u);
}

TEST(VideosynthTruthComparison, coverageCountsDamageInsideTheFlaggedRuns) {
  const auto params = pal_params();
  const size_t width = orc::kPalSamplesPerLineNominal;
  const size_t total = orc::frame_line_sample_offset(
      orc::VideoSystem::PAL, width, orc::kPalFrameLines);
  const size_t line =
      orc::frame_line_sample_offset(orc::VideoSystem::PAL, width, 100);
  auto run = [](size_t start, uint32_t count) {
    orc::DropoutRun r;
    r.sample_start = start;
    r.sample_count = count;
    return r;
  };
  // Damage of 10 in the picture, of which the flag covers 4; damage in the
  // line's blanking is not counted.
  const std::vector<orc::DropoutRun> damage = {run(line + 400, 10),
                                               run(line + 10, 20)};
  const std::vector<orc::DropoutRun> flagged = {run(line + 406, 30)};
  orc_videosynth::CoverageMeter meter;
  meter.add_frame(damage, flagged, total, params);
  EXPECT_EQ(meter.result().damaged_samples, 10u);
  EXPECT_EQ(meter.result().flagged_samples, 4u);
  EXPECT_DOUBLE_EQ(meter.result().flagged_fraction(), 0.4);
  EXPECT_DOUBLE_EQ(orc_videosynth::Coverage{}.flagged_fraction(), 1.0);
}

TEST(VideosynthTruthComparison, frameHelpersRejectParametersWithoutAnArea) {
  orc::SourceParameters params = pal_params();
  params.active_video_end = -1;
  std::vector<Sample> frame(10, 0);
  BandErrorMeter meter(orc::VideoSystem::PAL, 1.0);
  EXPECT_THROW(meter.add_frame(frame.data(), frame.data(), params),
               std::invalid_argument);
}

}  // namespace
}  // namespace orc_unit_test
