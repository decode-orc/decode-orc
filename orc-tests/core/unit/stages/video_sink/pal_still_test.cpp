/*
 * File:        pal_still_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for PAL Still's frame raster support: the lattice
 *              fitted to the bursts, the phase tables that demodulate and
 *              remodulate a row, the line-alternating matrix, its rotation
 *              fitted alone, and its basis at a chroma phase
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/sinks/common/decoders/pal_still.h"

#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <vector>

namespace orc_unit_test {
namespace {

namespace d2d = orc::pal_still;

constexpr double kPi = 3.14159265358979323846;

// The 4fsc PAL lattice of a field: phi falls 90 degrees a line from
// |phi0|, and the V-switch alternates from |s0| on field line 1.
struct FieldLattice {
  double phi0;
  double s0;
};

double expected_phi(const FieldLattice& f, size_t line) {
  return f.phi0 - (90.0 * static_cast<double>(line));
}

double expected_s(const FieldLattice& f, size_t line) {
  return f.s0 * ((line % 2 == 0) ? 1.0 : -1.0);
}

// A raster whose only content is each row's burst, at phi + 45 degrees on an
// s = +1 row and phi + 135 on s = -1, of amplitude |burst|.
d2d::Plane burst_raster(const FieldLattice fields[2], double burst) {
  d2d::Plane raster(d2d::kRows * d2d::kLine, 0.0);
  for (size_t row = 0; row < d2d::kRows; ++row) {
    const FieldLattice& f = fields[row % 2];
    const size_t line = row / 2;
    const double angle =
        expected_phi(f, line) + (expected_s(f, line) > 0.0 ? 45.0 : 135.0);
    for (size_t n = d2d::kBurstStart; n < d2d::kBurstEnd; ++n) {
      raster[(row * d2d::kLine) + n] =
          burst * std::cos((static_cast<double>(n) * kPi / 2.0) +
                           (angle * kPi / 180.0));
    }
  }
  return raster;
}

double wrap_degrees(double a) { return std::remainder(a, 360.0); }

TEST(PalStillTest, LatticeFitsTheBursts) {
  // Field 2 one quarter turn and a switch away from field 1, as any frame
  // of the eight-field sequence might be.
  const FieldLattice fields[2] = {{37.0, 1.0}, {37.0 + 180.0 + 90.0, -1.0}};
  const auto lattice = d2d::fit_lattice(burst_raster(fields, 0.17));
  for (size_t row = 0; row < d2d::kRows; ++row) {
    const FieldLattice& f = fields[row % 2];
    EXPECT_NEAR(wrap_degrees(lattice.phi_deg[row] - expected_phi(f, row / 2)),
                0.0, 1e-9)
        << row;
    EXPECT_EQ(lattice.s[row], expected_s(f, row / 2)) << row;
  }
  EXPECT_NEAR(lattice.burst_amplitude, 0.17, 1e-9);
}

// Every shift between rows of one field is a multiple of 90 degrees: two
// field lines apart, the carrier is inverted and the switch the same.
TEST(PalStillTest, PhaseTables) {
  const FieldLattice fields[2] = {{-12.0, -1.0}, {168.0, 1.0}};
  const auto lattice = d2d::fit_lattice(burst_raster(fields, 0.17));
  for (size_t row = d2d::kFirstActiveRow; row + 4 < d2d::kEndActiveRow; ++row) {
    EXPECT_NEAR(
        wrap_degrees(lattice.phi_deg[row + 4] - lattice.phi_deg[row] - 180.0),
        0.0, 1e-9);
    EXPECT_EQ(lattice.s[row + 4], lattice.s[row]);
    EXPECT_EQ(lattice.s[row + 2], -lattice.s[row]);
  }
}

// Chroma of constant (U, V) remodulated on the lattice and demodulated
// again reads (U, V) after the horizontal filter, on every picture row.
TEST(PalStillTest, DemodulateUndoesRemodulate) {
  const FieldLattice fields[2] = {{80.0, 1.0}, {260.0, -1.0}};
  const auto lattice = d2d::fit_lattice(burst_raster(fields, 0.17));
  const double u_value = -0.21;
  const double v_value = 0.33;
  d2d::Plane u(d2d::kRows * d2d::kLine, u_value);
  d2d::Plane v(d2d::kRows * d2d::kLine, v_value);
  d2d::Plane chroma;
  d2d::remodulate(u, v, lattice, 0.0, chroma);
  d2d::Plane du;
  d2d::Plane dv;
  d2d::demodulate(chroma, lattice, 0.0, du, dv);
  d2d::filter_rows_horizontal(du);
  d2d::filter_rows_horizontal(dv);
  for (size_t row = 0; row < d2d::kRows; ++row) {
    const bool picture =
        row >= d2d::kFirstActiveRow && row < d2d::kEndActiveRow;
    for (size_t n = 16; n + 16 < d2d::kLine; n += 37) {
      const size_t k = (row * d2d::kLine) + n;
      EXPECT_NEAR(du[k], picture ? u_value : 0.0, 1e-3) << row << " " << n;
      EXPECT_NEAR(dv[k], picture ? v_value : 0.0, 1e-3) << row << " " << n;
    }
  }
}

TEST(PalStillTest, MatrixAndItsInverse) {
  const FieldLattice fields[2] = {{0.0, 1.0}, {180.0, -1.0}};
  const auto lattice = d2d::fit_lattice(burst_raster(fields, 0.17));
  const d2d::Matrix m{0.01, 0.13, -0.09, -0.02};
  d2d::Plane u(d2d::kRows * d2d::kLine, 0.2);
  d2d::Plane v(d2d::kRows * d2d::kLine, -0.1);
  d2d::apply_matrix(m, lattice, true, u, v);
  // A row of switch s now reads (I + s M) W.
  for (const size_t row : {size_t{100}, size_t{101}, size_t{102}}) {
    const double s = lattice.s[row];
    const size_t k = row * d2d::kLine;
    EXPECT_NEAR(u[k], 0.2 + (s * ((m[0] * 0.2) + (m[1] * -0.1))), 1e-12);
    EXPECT_NEAR(v[k], -0.1 + (s * ((m[2] * 0.2) + (m[3] * -0.1))), 1e-12);
  }
  d2d::apply_matrix(m, lattice, false, u, v);
  for (size_t k = 0; k < u.size(); k += 997) {
    EXPECT_NEAR(u[k], 0.2, 1e-12);
    EXPECT_NEAR(v[k], -0.1, 1e-12);
  }
}

// Planes of flat colour, a band of rows for each of |colours|.
void fill_bands(const std::vector<std::complex<double>>& colours, d2d::Plane& u,
                d2d::Plane& v) {
  u.assign(d2d::kRows * d2d::kLine, 0.0);
  v.assign(d2d::kRows * d2d::kLine, 0.0);
  const size_t band =
      (d2d::kEndActiveRow - d2d::kFirstActiveRow + colours.size() - 1) /
      colours.size();
  for (size_t row = d2d::kFirstActiveRow; row < d2d::kEndActiveRow; ++row) {
    const auto& w = colours[(row - d2d::kFirstActiveRow) / band];
    for (size_t n = 0; n < d2d::kLine; ++n) {
      u[(row * d2d::kLine) + n] = w.real();
      v[(row * d2d::kLine) + n] = w.imag();
    }
  }
}

// A frame of one hue, which cannot determine the whole matrix: the
// rotation alone, with the symmetric part known, reads exactly.
TEST(PalStillTest, FitsTheRotationOnOneHue) {
  const FieldLattice fields[2] = {{0.0, 1.0}, {180.0, -1.0}};
  const auto lattice = d2d::fit_lattice(burst_raster(fields, 0.17));
  const d2d::Matrix symmetric{-0.01, 0.02, 0.02, -0.03};
  for (const double theta : {0.11, 0.23, -0.04}) {
    const d2d::Matrix m = d2d::with_rotation(symmetric, theta);
    d2d::Plane u;
    d2d::Plane v;
    fill_bands({{0.12, -0.05}}, u, v);
    d2d::apply_matrix(m, lattice, true, u, v);
    d2d::FlatPairs pairs;
    d2d::add_flat_pairs(u, v, lattice, pairs);
    size_t used = 0;
    const auto fitted = d2d::fit_rotation(pairs, symmetric, 1000, &used);
    ASSERT_TRUE(fitted.has_value()) << theta;
    EXPECT_GT(used, 1000u);
    EXPECT_NEAR(*fitted, theta, 1e-9);
  }
}

// A rotation of 0.25, near the alternating-half cut of Phase 7's fit, read
// in full; too few pairs, or a rotation beyond kMaxRotation, reads none.
TEST(PalStillTest, FitRotationLimits) {
  const FieldLattice fields[2] = {{0.0, 1.0}, {180.0, -1.0}};
  const auto lattice = d2d::fit_lattice(burst_raster(fields, 0.17));
  const d2d::Matrix none{0.0, 0.0, 0.0, 0.0};
  d2d::Plane u;
  d2d::Plane v;
  fill_bands({{0.2, 0.1}, {-0.15, 0.3}}, u, v);
  d2d::apply_matrix(d2d::with_rotation(none, 0.25), lattice, true, u, v);
  d2d::FlatPairs pairs;
  d2d::add_flat_pairs(u, v, lattice, pairs);
  const auto fitted = d2d::fit_rotation(pairs, none, 1000, nullptr);
  ASSERT_TRUE(fitted.has_value());
  EXPECT_NEAR(*fitted, 0.25, 1e-9);
  EXPECT_FALSE(d2d::fit_rotation(pairs, none, pairs.plus.size() + 1, nullptr));

  fill_bands({{0.2, 0.1}}, u, v);
  d2d::apply_matrix(d2d::with_rotation(none, 0.55), lattice, true, u, v);
  d2d::FlatPairs far;
  d2d::add_flat_pairs(u, v, lattice, far);
  // The first pass's cut keeps the pairs (0.55 < 0.6 of the mean), and the
  // fit reads 0.55: beyond kMaxRotation, so none.
  EXPECT_FALSE(d2d::fit_rotation(far, none, 1000, nullptr).has_value());
}

TEST(PalStillTest, InvertibleMatrices) {
  EXPECT_TRUE(d2d::invertible({0.0, 0.0, 0.0, 0.0}));
  EXPECT_TRUE(d2d::invertible({-0.009, 0.051, -0.106, -0.024}));
  EXPECT_TRUE(d2d::invertible({0.0, 0.5, -0.5, 0.0}));
  // Within the parameters' +/-0.5 but singular, or nearly.
  EXPECT_FALSE(d2d::invertible({-0.5, 0.5, 0.5, -0.5}));
  EXPECT_FALSE(d2d::invertible({0.5, 0.5, 0.5, 0.5}));
  EXPECT_FALSE(d2d::invertible({-0.45, 0.45, 0.45, -0.45}));
}

// Rows carrying (U, V) with the error M, demodulated at a chroma phase:
// corrected by M in that phase's basis, both switch states read the same,
// (c I + n J M) W.
TEST(PalStillTest, InPhaseBasisUndoesTheTurnedRows) {
  const FieldLattice fields[2] = {{80.0, 1.0}, {260.0, -1.0}};
  const auto lattice = d2d::fit_lattice(burst_raster(fields, 0.17));
  const d2d::Matrix m{-0.009, 0.13, -0.106, -0.024};
  const double u_value = 0.2;
  const double v_value = -0.1;
  for (const double phase : {0.0, 12.0, -30.0}) {
    d2d::Plane u(d2d::kRows * d2d::kLine, u_value);
    d2d::Plane v(d2d::kRows * d2d::kLine, v_value);
    d2d::apply_matrix(m, lattice, true, u, v);
    d2d::Plane chroma;
    d2d::remodulate(u, v, lattice, 0.0, chroma);
    d2d::demodulate_filtered(chroma, lattice, phase, u, v);
    const auto turned = d2d::in_phase_basis(m, phase);
    ASSERT_TRUE(turned.has_value());
    if (phase == 0.0) {
      for (size_t i = 0; i < 4; ++i) EXPECT_NEAR((*turned)[i], m[i], 1e-15);
    }
    d2d::apply_matrix(*turned, lattice, false, u, v);
    const double c = std::cos(phase * kPi / 180.0);
    const double n = std::sin(phase * kPi / 180.0);
    // A W with A = c I + n J M, J M = [[M_VU, M_VV], [-M_UU, -M_UV]].
    const double want_u = ((c + (n * m[2])) * u_value) + (n * m[3] * v_value);
    const double want_v = (-n * m[0] * u_value) + ((c - (n * m[1])) * v_value);
    // Four samples averaged: the demodulation's 2 fsc ripple, which the
    // horizontal filter leaves at about 1e-3, has a period of two.
    for (const size_t row :
         {size_t{200}, size_t{201}, size_t{202}, size_t{203}}) {
      double mean_u = 0.0;
      double mean_v = 0.0;
      for (size_t n = 600; n < 604; ++n) {
        mean_u += u[(row * d2d::kLine) + n] / 4.0;
        mean_v += v[(row * d2d::kLine) + n] / 4.0;
      }
      EXPECT_NEAR(mean_u, want_u, 1e-6) << phase << " " << row;
      EXPECT_NEAR(mean_v, want_v, 1e-6) << phase << " " << row;
    }
  }
  // At 90 degrees no chroma remains to correct.
  EXPECT_FALSE(d2d::in_phase_basis(m, 90.0).has_value());
}

}  // namespace
}  // namespace orc_unit_test
