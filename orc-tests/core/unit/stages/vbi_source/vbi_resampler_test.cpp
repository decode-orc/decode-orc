/*
 * File:        vbi_resampler_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for band-limited resampling of VBI records
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_resampler.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <random>
#include <vector>

namespace orc {
namespace {

// bt8x8 PAL samples at 8 x fsc and the output lattice is 4 x fsc, so every
// card capture in the first milestone decimates by exactly two.
constexpr double kSourceRateHz = 35468950.0;
constexpr double kOutputRateHz = 17734475.0;
constexpr double kDecimationRatio = 2.0;
constexpr double kOutputNyquistHz = kOutputRateHz / 2.0;

// ETSI EN 300 706: the WST carrier is 444 x fH = 6.9375 MHz, which sits at
// 78 % of the output Nyquist frequency.  Losing it to the anti-alias filter
// or to its own alias would destroy the data the stage exists to carry.
constexpr double kWSTBitRateHz = 6937500.0;

constexpr double kPi = 3.14159265358979323846;

// A placement that reads a record from |first_position| in steps of |ratio|.
VBIDataPlacement window_at(double ratio, double first_position,
                           uint32_t count) {
  VBIDataPlacement placement;
  placement.source_samples_per_output_sample = ratio;
  placement.source_position_at_output_zero = first_position;
  placement.output_begin = 0;
  placement.output_end = count;
  return placement;
}

std::vector<double> sine_wave(double frequency_hz, double sample_rate_hz,
                              size_t sample_count, double phase_radians) {
  std::vector<double> samples(sample_count, 0.0);
  for (size_t index = 0; index < sample_count; ++index) {
    const double time = static_cast<double>(index) / sample_rate_hz;
    samples[index] = std::sin(2.0 * kPi * frequency_hz * time + phase_radians);
  }
  return samples;
}

double root_mean_square(const std::vector<double>& samples) {
  if (samples.empty()) {
    return 0.0;
  }
  double sum_of_squares = 0.0;
  for (const double sample : samples) {
    sum_of_squares += sample * sample;
  }
  return std::sqrt(sum_of_squares / static_cast<double>(samples.size()));
}

// Amplitude of a single tone after resampling, relative to the unit-amplitude
// input.  The tone is the only thing in the record, so its root mean square
// carries its amplitude whether or not it has folded to a new frequency.
double relative_tone_amplitude(double frequency_hz) {
  constexpr uint32_t kRecordSamples = 6000;
  constexpr double kFirstPosition = 1000.37;
  constexpr uint32_t kOutputSamples = 1800;

  const VBIRecordResampler resampler(
      window_at(kDecimationRatio, kFirstPosition, kOutputSamples),
      kRecordSamples);

  const std::vector<double> source =
      sine_wave(frequency_hz, kSourceRateHz, kRecordSamples, 0.31);

  std::vector<double> output;
  resampler.resample(source, output);

  const double input_rms = 1.0 / std::sqrt(2.0);
  return root_mean_square(output) / input_rms;
}

double to_decibels(double ratio) { return 20.0 * std::log10(ratio); }

TEST(VBIResampler, TheWSTCarrierSurvivesDecimationToFourTimesSubcarrier) {
  const double loss_db = -to_decibels(relative_tone_amplitude(kWSTBitRateHz));

  EXPECT_LT(loss_db, 0.5) << "WST carrier loss " << loss_db << " dB";
  EXPECT_GT(loss_db, -0.5) << "WST carrier gain " << -loss_db << " dB";
}

// The whole passband the data occupies must come through, not just the
// carrier: half the bit rate is where the clock run-in's energy sits.
TEST(VBIResampler, ThePassbandUpToTheCarrierIsFlatWithinHalfADecibel) {
  constexpr int kSteps = 16;
  for (int step = 1; step <= kSteps; ++step) {
    const double frequency = kWSTBitRateHz * static_cast<double>(step) / kSteps;
    const double amplitude = relative_tone_amplitude(frequency);
    EXPECT_LT(std::abs(to_decibels(amplitude)), 0.5)
        << "frequency " << frequency << " Hz";
  }
}

// Sample dropping would put everything above the output Nyquist straight back
// onto the data.  Nothing up there may survive at a level that matters.
TEST(VBIResampler, EnergyAboveTheOutputNyquistIsAttenuatedBySixtyDecibels) {
  // Up to 45 % of the source rate; beyond that the source itself is aliased
  // and the tone is no longer the frequency it was asked for.
  constexpr double kLowest = kOutputNyquistHz * 1.015;
  constexpr double kHighest = 0.45 * kSourceRateHz;
  constexpr int kSteps = 28;
  for (int step = 0; step <= kSteps; ++step) {
    const double frequency =
        kLowest + (kHighest - kLowest) * static_cast<double>(step) / kSteps;
    const double attenuation_db =
        -to_decibels(relative_tone_amplitude(frequency));
    EXPECT_GT(attenuation_db, 60.0) << "frequency " << frequency << " Hz";
  }
}

TEST(VBIResampler, TheDecimationCutoffSitsBelowTheOutputNyquist) {
  const VBIRecordResampler resampler(window_at(kDecimationRatio, 100.0, 64),
                                     2044);

  const double cutoff_hz =
      resampler.cutoff_fraction_of_source_rate() * kSourceRateHz;
  EXPECT_GT(cutoff_hz, kWSTBitRateHz);
  EXPECT_LT(cutoff_hz, kOutputNyquistHz);
}

// Linear phase, stated as the property that matters: a symmetric pulse stays
// symmetric about its own centre rather than developing a leading or trailing
// tail.  A resampler that ran late would bias every placement by the same
// amount, which is exactly the failure the calibrated capture offset exists to
// remove.
TEST(VBIResampler, ASymmetricPulseStaysSymmetricAboutItsCentre) {
  constexpr uint32_t kRecordSamples = 1200;
  constexpr double kCentre = 600.0;
  constexpr double kHalfWidth = 24.0;
  constexpr uint32_t kOutputSamples = 200;

  std::vector<double> source(kRecordSamples, 0.0);
  for (size_t index = 0; index < kRecordSamples; ++index) {
    const double offset = static_cast<double>(index) - kCentre;
    if (std::abs(offset) < kHalfWidth) {
      source[index] = 0.5 * (1.0 + std::cos(kPi * offset / kHalfWidth));
    }
  }

  // The window is centred on the pulse and steps by two, so output sample n
  // and output sample (count - 1 - n) are equidistant from the centre.
  const double first_position =
      kCentre - kDecimationRatio * (kOutputSamples - 1u) / 2.0;
  const VBIRecordResampler resampler(
      window_at(kDecimationRatio, first_position, kOutputSamples),
      kRecordSamples);

  std::vector<double> output;
  resampler.resample(source, output);
  ASSERT_EQ(output.size(), kOutputSamples);

  for (uint32_t index = 0; index < kOutputSamples / 2u; ++index) {
    EXPECT_NEAR(output[index], output[kOutputSamples - 1u - index], 1e-9)
        << "output sample " << index;
  }
}

// A constant region must resample to the same constant at every fractional
// phase.  The quiet back porch that logic 0 is read from is exactly such a
// region, so any phase-dependent gain would ripple the level reference.
TEST(VBIResampler, ConstantInputIsReproducedExactlyAtEveryFractionalPhase) {
  const std::vector<double> source(800, 137.25);

  for (int step = 0; step < 64; ++step) {
    const double first_position = 200.0 + static_cast<double>(step) / 64.0;
    const VBIRecordResampler resampler(
        window_at(kDecimationRatio, first_position, 100), 800);

    std::vector<double> output;
    resampler.resample(source, output);
    ASSERT_EQ(output.size(), 100u);
    for (const double sample : output) {
      EXPECT_NEAR(sample, 137.25, 1e-9) << "phase step " << step;
    }
  }
}

// Positions off either end of the record read the nearest stored sample, so a
// quiet head or tail keeps its own level instead of being pulled towards an
// invented zero.
TEST(VBIResampler, PositionsOutsideTheRecordReadTheNearestStoredSample) {
  const std::vector<double> source(600, 42.0);

  // A window that opens before the record and closes after it.
  const VBIRecordResampler resampler(window_at(kDecimationRatio, -60.0, 360),
                                     600);

  std::vector<double> output;
  resampler.resample(source, output);
  ASSERT_EQ(output.size(), 360u);
  for (const double sample : output) {
    EXPECT_NEAR(sample, 42.0, 1e-9);
  }
}

TEST(VBIResampler, AnEmptyRecordProducesNoSignal) {
  const VBIRecordResampler resampler(window_at(kDecimationRatio, 100.0, 8),
                                     2044);

  std::vector<double> output;
  resampler.resample({}, output);
  ASSERT_EQ(output.size(), 8u);
  for (const double sample : output) {
    EXPECT_DOUBLE_EQ(sample, 0.0);
  }
}

// A source whose rate is a whole multiple of the output's reads every sample
// at the same fractional delay, so the kernel is resolved once for the whole
// run rather than once per output sample.
TEST(VBIResampler, AWholeNumberRatioResolvesToASinglePhaseRow) {
  const VBIRecordResampler whole(window_at(kDecimationRatio, 120.5, 900), 2044);
  EXPECT_EQ(whole.phase_row_count(), 1u);
  EXPECT_EQ(whole.output_count(), 900u);
  EXPECT_EQ(whole.taps(), 128u);

  // A ratio that is not a whole number needs more of them, and the phase bank
  // is what bounds how many.
  const VBIRecordResampler fractional(window_at(1.5225, 120.5, 900), 2044);
  EXPECT_GT(fractional.phase_row_count(), 1u);
  EXPECT_LE(fractional.phase_row_count(), 1024u);
}

// A capture already on the output's own lattice — the same rate, and a whole
// number of samples between the two origins — is copied.  There is nothing to
// interpolate and nothing that could alias, so filtering it would attenuate
// the top of the data band to no purpose.
TEST(VBIResampler, ARecordAlreadyOnTheOutputLatticeIsCopied) {
  // What a TBC-derived capture gives: the same rate, an offset of zero, and a
  // data window that opens part-way along the line.
  VBIDataPlacement placement;
  placement.source_samples_per_output_sample = 1.0;
  placement.source_position_at_output_zero = 0.0;
  placement.output_begin = 147;
  placement.output_end = 892;

  const VBIRecordResampler passthrough(placement, 910);

  EXPECT_EQ(passthrough.taps(), 1u);
  EXPECT_EQ(passthrough.phase_row_count(), 1u);
  EXPECT_DOUBLE_EQ(passthrough.cutoff_fraction_of_source_rate(), 0.5);

  std::vector<double> record(910, 0.0);
  for (size_t index = 0; index < record.size(); ++index) {
    record[index] = static_cast<double>(index) * 3.0 + 7.0;
  }

  std::vector<double> output;
  passthrough.resample(record, output);
  ASSERT_EQ(output.size(), 745u);
  for (size_t index = 0; index < output.size(); ++index) {
    EXPECT_DOUBLE_EQ(output[index], record[index + 147u]) << "sample " << index;
  }
}

// A unity ratio at a fractional offset is still a filter: the requested
// positions fall between stored samples, so the waveform has to be
// interpolated.  Its cutoff follows the source's own Nyquist, there being no
// bandwidth to discard.
TEST(VBIResampler, AUnityRatioAtAFractionalOffsetStillInterpolates) {
  const VBIRecordResampler interpolator(window_at(1.0, -100.5, 64), 2044);
  const VBIRecordResampler decimator(window_at(kDecimationRatio, -100.0, 64),
                                     2044);

  EXPECT_GT(interpolator.taps(), 1u);
  EXPECT_GT(interpolator.cutoff_fraction_of_source_rate(),
            decimator.cutoff_fraction_of_source_rate());
  EXPECT_LT(interpolator.cutoff_fraction_of_source_rate(), 0.5);
}

// A ratio that is not a usable number must not produce a kernel that silently
// misplaces every sample.
TEST(VBIResampler, AnUnusableRatioProducesNothing) {
  const VBIRecordResampler zero_ratio(window_at(0.0, 100.0, 64), 2044);
  const VBIRecordResampler negative_ratio(window_at(-2.0, 100.0, 64), 2044);
  const VBIRecordResampler empty_record(window_at(kDecimationRatio, 100.0, 64),
                                        0);

  EXPECT_EQ(zero_ratio.output_count(), 0u);
  EXPECT_EQ(negative_ratio.output_count(), 0u);
  EXPECT_EQ(empty_record.output_count(), 0u);

  std::vector<double> output;
  zero_ratio.resample(std::vector<double>(600, 1.0), output);
  EXPECT_TRUE(output.empty());
}

// A frame's records resampled together give each record what it gives alone,
// to the bit: the same products, added in the same order, only worked four
// records abreast (and, for a few records, several output samples abreast).
// Checked at a whole-number ratio, at a ratio that is not one (the cx23885's
// 27 MHz onto 4 x fsc NTSC, a phase row per output sample), at windows running
// off both ends of the record whose lengths leave output samples over, and on
// a copied record; for every count of records from one up past two full
// passes, with an empty and a short record among them; and with and without
// AVX2.
TEST(VBIResampler, RecordsResampledTogetherGiveWhatEachGivesAlone) {
  struct Window {
    double ratio;
    double first_position;
    uint32_t count;
  };
  const Window windows[] = {
      {kDecimationRatio, 120.5, 700},
      {27000000.0 / (4.0 * 315000000.0 / 88.0), 263.3, 760},
      {1.8857, -90.25, 901},
      {1.8857, 140.5, 763},
      {1.0, 7.0, 745},
  };
  constexpr uint32_t kRecordSamples = 1440;

  std::mt19937 random(20261012);
  std::uniform_int_distribution<int> byte(0, 255);
  std::uniform_real_distribution<double> fraction(-0.5, 0.5);

  for (const Window& window : windows) {
    for (const bool use_avx2 : {false, true}) {
      VBIResamplerConfig config;
      config.use_avx2 = use_avx2;
      const VBIRecordResampler resampler(
          window_at(window.ratio, window.first_position, window.count),
          kRecordSamples, config);
      const uint32_t outputs = resampler.output_count();
      ASSERT_EQ(outputs, window.count);

      for (const size_t count : {1, 2, 3, 4, 5, 7, 8, 13, 16, 17, 24, 35}) {
        std::vector<std::vector<double>> records(count);
        for (size_t index = 0; index < count; ++index) {
          // Every fifth record has a fraction on its samples, every seventh
          // is short, and the third is empty.
          const size_t length =
              index == 2
                  ? 0
                  : (index % 7 == 6 ? kRecordSamples / 3 : kRecordSamples);
          records[index].resize(length);
          for (double& sample : records[index]) {
            sample = static_cast<double>(byte(random)) +
                     (index % 5 == 4 ? fraction(random) : 0.0);
          }
        }
        std::vector<const std::vector<double>*> sources;
        for (const std::vector<double>& record : records) {
          sources.push_back(&record);
        }

        std::vector<double> together;
        resampler.resample_records(sources, together);
        ASSERT_EQ(together.size(), count * outputs);

        std::vector<double> alone;
        for (size_t index = 0; index < count; ++index) {
          resampler.resample(records[index], alone);
          ASSERT_EQ(alone.size(), outputs);
          EXPECT_EQ(std::memcmp(alone.data(), together.data() + index * outputs,
                                outputs * sizeof(double)),
                    0)
              << "ratio " << window.ratio << ", " << count << " records"
              << (use_avx2 ? " with" : " without") << " AVX2, record " << index;
        }
      }
    }
  }
}

}  // namespace
}  // namespace orc
