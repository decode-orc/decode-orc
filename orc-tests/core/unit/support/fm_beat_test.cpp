/*
 * File:        fm_beat_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the FM-beat model: the flat-colour pre-gate, a
 *              known beat fitted and removed, and the fit's peak at the
 *              carrier the beat was cut with
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/support/fm_beat.h>

#include <cmath>
#include <cstdint>
#include <vector>

#include "../include/pal_colour_bars_source.h"

namespace orc_unit_test {
namespace {

namespace fm_beat = orc::fm_beat;

constexpr double kCarrier = 7.09e6;

std::array<fm_beat::FieldGeometry, 2> fields_of(
    const PalColourBarsSource& source) {
  return fm_beat::frame_fields(*source.get_frame_descriptor(0),
                               PalColourBarsSource::parameters());
}

// RMS over a field's picture of a - b, in codes.
double picture_rms(const std::vector<int16_t>& a, const std::vector<int16_t>& b,
                   const fm_beat::FieldGeometry& field) {
  double sum = 0.0;
  size_t count = 0;
  for (size_t line = field.picture_first; line < field.picture_end; ++line) {
    const size_t base = orc::frame_line_sample_offset(
        field.system, field.samples_per_line, field.first_line + line);
    for (int32_t n = field.active_start; n < field.active_end; ++n) {
      const double d =
          a[base + static_cast<size_t>(n)] - b[base + static_cast<size_t>(n)];
      sum += d * d;
      ++count;
    }
  }
  return std::sqrt(sum / static_cast<double>(count));
}

TEST(FmBeatTest, PreGateCountsFlatColourOnly) {
  const PalColourBarsSource bars({});
  PalColourBarsSource::Options grey_options;
  grey_options.colour = false;
  const PalColourBarsSource grey(grey_options);
  for (const auto& field : fields_of(bars)) {
    EXPECT_GE(fm_beat::flat_colour_samples(bars.get_frame(0), field),
              fm_beat::kMinFlatColourSamples);
    EXPECT_EQ(fm_beat::flat_colour_samples(grey.get_frame(0), field), 0u);
  }
}

TEST(FmBeatTest, FitRemovesAKnownBeat) {
  PalColourBarsSource::Options options;
  options.beat_carrier_hz = kCarrier;
  const PalColourBarsSource source(options);
  const auto input = source.get_frame_copy(0);
  const auto clean = source.clean(0);
  auto output = input;
  for (const auto& field : fields_of(source)) {
    const fm_beat::FieldBeat beat(input.data(), field, kCarrier);
    const auto fit = beat.fit();
    EXPECT_EQ(fit.lines_corrected, field.picture_end - field.picture_first);
    beat.subtract(fit, output.data());
    const double before = picture_rms(input, clean, field);
    const double after = picture_rms(output, clean, field);
    EXPECT_GT(before, 2.0);
    EXPECT_LT(after, 0.1 * before);
  }
  // Nothing outside the picture is written.
  for (size_t line : {5u, 20u, 313u + 5u}) {
    const size_t base =
        orc::frame_line_sample_offset(orc::VideoSystem::PAL, 1135, line);
    for (size_t n = 0; n < 1135; ++n) {
      EXPECT_EQ(output[base + n], input[base + n]);
    }
  }
}

// The fit removes most where the carrier is the one the beat was cut with:
// the principle the calibration searches by.
TEST(FmBeatTest, FitPeaksAtTheCarrier) {
  PalColourBarsSource::Options options;
  options.beat_carrier_hz = kCarrier;
  const PalColourBarsSource source(options);
  const auto field = fields_of(source)[0];
  const fm_beat::FieldBeat beat(source.get_frame(0), field,
                                fm_beat::kNominalCarrierBlackHz);
  const double offset = kCarrier - fm_beat::kNominalCarrierBlackHz;
  const double at_carrier = beat.fit(offset).removed_energy;
  for (double miss : {-6.0e3, -3.0e3, 3.0e3, 6.0e3}) {
    EXPECT_LT(beat.fit(offset + miss).removed_energy, at_carrier) << miss;
  }
}

// Noise alone, on flat colour, is not taken for a beat.
TEST(FmBeatTest, NoiseIsNotABeat) {
  PalColourBarsSource::Options options;
  options.noise_codes = 6;
  const PalColourBarsSource source(options);
  for (const auto& field : fields_of(source)) {
    const fm_beat::FieldBeat beat(source.get_frame(0), field, kCarrier);
    EXPECT_EQ(beat.fit().lines_corrected, 0u);
  }
}

}  // namespace
}  // namespace orc_unit_test
