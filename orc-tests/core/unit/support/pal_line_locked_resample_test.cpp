/*
 * File:        pal_line_locked_resample_test.cpp
 * Module:      orc-tests/core/unit/support
 * Purpose:     Unit tests for the line-locked PAL ↔ 4fsc lattice resample and
 *              its coordinate maps (issue #369)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/pal_line_locked_resample.h>

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace orc_unit_test {
namespace {

namespace pll = orc::pal_line_locked;

constexpr double kPi = 3.14159265358979323846;
constexpr int64_t kLine = orc::kPalSamplesPerLineNominal;  // 1135
constexpr double kCarrierAmplitude = 300.0;
constexpr double kPedestal = 512.0;

// Lattice position of 0H of frame row |row| (row × 709379 / 625).
double row_origin(int row) {
  return static_cast<double>(row) * 709379.0 / 625.0;
}

// A line-locked field of |field| carrying the subcarrier exactly as ld-decode
// stores it: sample m is taken one line-locked period (64 µs / 1135) after
// sample m − 1, starting at 0H of the field's first row. In lattice samples
// that period is 709379 / 709375, so against a 1135-sample grid the carrier
// (fsc = lattice rate / 4) drifts +0.576° per line — the 25 Hz offset.
std::vector<double> line_locked_carrier_field(int field, double phase0) {
  const size_t n = pll::line_locked_field_samples(field);
  const double t0 = row_origin(field == 1 ? 0 : orc::kPalField1Lines);
  std::vector<double> v(n);
  for (size_t m = 0; m < n; ++m) {
    const double t = t0 + static_cast<double>(m) * 709379.0 / 709375.0;
    v[m] = kPedestal + kCarrierAmplitude * std::cos(kPi / 2.0 * t + phase0);
  }
  return v;
}

// Phase of the carrier over samples [first, first + 1000) of |x| (sample n of
// |x| at frame position origin + n), relative to the lattice phase
// π/2 × frame position. Zero when the carrier sits on the lattice.
double phase_error_deg(const std::vector<double>& x, size_t first,
                       double origin, double phase0) {
  std::complex<double> acc(0.0, 0.0);
  for (size_t n = first; n < first + 1000; ++n) {
    const double t = origin + static_cast<double>(n);
    acc += (x[n] - kPedestal) *
           std::exp(std::complex<double>(0.0, -(kPi / 2.0 * t + phase0)));
  }
  return std::arg(acc) * 180.0 / kPi;
}

std::vector<double> to_lattice(int field, const std::vector<double>& in) {
  std::vector<double> out(pll::lattice_field_samples(field));
  pll::to_lattice(field, in.data(), out.data(),
                  [](float v) { return static_cast<double>(v); });
  return out;
}

std::vector<double> to_line_locked(int field, const std::vector<double>& in) {
  std::vector<double> out(pll::line_locked_field_samples(field));
  pll::to_line_locked(field, in.data(), out.data(),
                      [](float v) { return static_cast<double>(v); });
  return out;
}

}  // namespace

TEST(PalLineLockedResampleTest, FieldGeometry_MatchesTheFrameLayout) {
  EXPECT_EQ(pll::line_locked_field_samples(1), 313u * 1135u);
  EXPECT_EQ(pll::line_locked_field_samples(2), 312u * 1135u);
  EXPECT_EQ(pll::lattice_field_samples(1), 313u * 1135u + 2u);
  EXPECT_EQ(pll::lattice_field_samples(2), 312u * 1135u + 2u);
  EXPECT_EQ(pll::lattice_field_start(1), 0u);
  EXPECT_EQ(pll::lattice_field_start(2), 313u * 1135u + 2u);
}

// 0H of every row lands where the CVBS spec puts it: row k at k × 709379 / 625
// of the frame (k × 1135.0064 samples).
TEST(PalLineLockedResampleTest, LatticePosition_PutsEveryRowOriginOnTheSpec) {
  for (int row : {0, 1, 100, 312}) {
    EXPECT_NEAR(pll::lattice_position(1, static_cast<double>(row * kLine)),
                row_origin(row), 1e-9)
        << "row " << row;
  }
  for (int row : {313, 314, 500, 624}) {
    const double m = static_cast<double>((row - 313) * kLine);
    EXPECT_NEAR(static_cast<double>(pll::lattice_field_start(2)) +
                    pll::lattice_position(2, m),
                row_origin(row), 1e-9)
        << "row " << row;
  }
}

TEST(PalLineLockedResampleTest, Positions_AreInverseMaps) {
  for (int field : {1, 2}) {
    for (double m : {0.0, 1.5, 1000.0, 200000.25, 354000.0}) {
      EXPECT_NEAR(
          pll::line_locked_position(field, pll::lattice_position(field, m)), m,
          1e-7);
    }
  }
}

// The acceptance check of issue #369: a carrier with the 25 Hz offset baked in
// line-locked comes out on the lattice — no drift down the field and the same
// lattice in both fields.
TEST(PalLineLockedResampleTest,
     ToLattice_PutsTheLineLockedCarrierOnTheLattice_InBothFields) {
  const double phase0 = 0.3;
  for (int field : {1, 2}) {
    const auto in = line_locked_carrier_field(field, phase0);
    const auto out = to_lattice(field, in);
    const double origin = static_cast<double>(pll::lattice_field_start(field));

    // Before the fix the same measurement on the line-locked samples drifts
    // by ~0.576° per line; check the input really carries that drift.
    const double raw_top =
        phase_error_deg(in, 10 * kLine + 100, origin, phase0);
    const double raw_bottom =
        phase_error_deg(in, 300 * kLine + 100, origin, phase0);
    EXPECT_NEAR(raw_bottom - raw_top, 290.0 * 0.576, 1.0);

    double first = 0.0;
    double last = 0.0;
    for (int line = 10; line <= 300; line += 10) {
      const double e = phase_error_deg(
          out, static_cast<size_t>(line) * kLine + 100, origin, phase0);
      EXPECT_NEAR(e, 0.0, 0.1) << "field " << field << " line " << line;
      if (line == 10) first = e;
      last = e;
    }
    EXPECT_LT(std::abs(last - first) / 290.0, 0.05)
        << "residual drift, field " << field;
  }
}

TEST(PalLineLockedResampleTest, ToLattice_ConstantFieldStaysConstant) {
  for (int field : {1, 2}) {
    const std::vector<double> in(pll::line_locked_field_samples(field), 700.0);
    const auto out = to_lattice(field, in);
    ASSERT_EQ(out.size(), pll::lattice_field_samples(field));
    for (size_t j = 0; j < out.size(); j += 997) {
      EXPECT_NEAR(out[j], 700.0, 1e-3);
    }
    EXPECT_NEAR(out.back(), 700.0, 1e-3);
  }
}

// Field 1's first sample sits exactly on line-locked sample 0.
TEST(PalLineLockedResampleTest, ToLattice_Field1FirstSampleIsExact) {
  std::vector<double> in(pll::line_locked_field_samples(1), 0.0);
  in[0] = 1000.0;
  const auto out = to_lattice(1, in);
  EXPECT_NEAR(out[0], 1000.0, 1e-3);
}

// Lattice → line-locked is the TBC Sink's inverse: a band-limited field comes
// back within one 10-bit code.
TEST(PalLineLockedResampleTest, RoundTrip_ReturnsTheLineLockedField) {
  for (int field : {1, 2}) {
    const size_t n = pll::line_locked_field_samples(field);
    std::vector<double> in(n);
    for (size_t m = 0; m < n; ++m) {
      const double x = static_cast<double>(m);
      in[m] = kPedestal + 200.0 * std::cos(kPi / 2.0 * x * 1.0001 + 0.7) +
              150.0 * std::sin(2.0 * kPi * x / 97.0) +
              60.0 * std::cos(2.0 * kPi * x * 0.31);
    }
    const auto back = to_line_locked(field, to_lattice(field, in));
    ASSERT_EQ(back.size(), n);
    double worst = 0.0;
    for (size_t m = 64; m + 64 < n; ++m) {
      worst = std::max(worst, std::abs(back[m] - in[m]));
    }
    // Two passes of the kernel's gain ripple (≤ 0.11 % per pass up to fsc)
    // on a ~410-code swing: under one code.
    EXPECT_LT(worst, 1.0) << "field " << field;
  }
}

// Coordinate spans round outward so a marked region never shrinks.
TEST(PalLineLockedResampleTest, LatticeSpan_CoversTheMovedSamples) {
  // Field 1, sample 0 maps exactly onto lattice sample 0.
  auto s = pll::lattice_span(1, 0, 0);
  EXPECT_EQ(s.first, 0);
  EXPECT_EQ(s.last, 0);

  // Field 2, sample 0 sits 0.0032 samples into the lattice field.
  s = pll::lattice_span(2, 0, 0);
  EXPECT_EQ(s.first, 0);
  EXPECT_EQ(s.last, 1);

  // Near the bottom of field 1 the picture has moved ~2 samples right.
  const int64_t m0 = 300 * kLine + 500;
  s = pll::lattice_span(1, m0, m0 + 49);
  EXPECT_EQ(s.first, static_cast<int64_t>(std::floor(
                         pll::lattice_position(1, static_cast<double>(m0)))));
  EXPECT_EQ(s.last, static_cast<int64_t>(std::ceil(pll::lattice_position(
                        1, static_cast<double>(m0 + 49)))));
  EXPECT_EQ(s.first, m0 + 1);

  // The last line-locked sample is clamped to the lattice field.
  const auto last1 =
      static_cast<int64_t>(pll::line_locked_field_samples(1)) - 1;
  s = pll::lattice_span(1, last1, last1);
  EXPECT_EQ(s.last, static_cast<int64_t>(pll::lattice_field_samples(1)) - 1);
}

TEST(PalLineLockedResampleTest, LineLockedSpan_InvertsLatticeSpanOutward) {
  for (int field : {1, 2}) {
    for (int64_t m : {int64_t{0}, int64_t{12345}, int64_t{200 * kLine + 7},
                      int64_t{311 * kLine + 1100}}) {
      const auto lat = pll::lattice_span(field, m, m + 30);
      const auto back = pll::line_locked_span(field, lat.first, lat.last);
      EXPECT_LE(back.first, m);
      EXPECT_GE(back.last, m + 30);
      EXPECT_GE(back.first, m - 2);
      EXPECT_LE(back.last, m + 32);
    }
  }
  // Lattice sample 0 of field 2 precedes line-locked sample 0; clamp to it.
  const auto s = pll::line_locked_span(2, 0, 0);
  EXPECT_EQ(s.first, 0);
  EXPECT_EQ(s.last, 0);
}

}  // namespace orc_unit_test
