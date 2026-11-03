/*
 * File:        domesday2d_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for Domesday2D's frame raster support: the lattice
 *              fitted to the bursts, the phase tables that demodulate and
 *              remodulate a row, and the line-alternating matrix
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/support/domesday2d.h>

#include <cmath>
#include <complex>
#include <vector>

namespace orc_unit_test {
namespace {

namespace d2d = orc::domesday2d;

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

TEST(Domesday2DTest, LatticeFitsTheBursts) {
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
}

// Every shift between rows of one field is a multiple of 90 degrees: two
// field lines apart, the carrier is inverted and the switch the same.
TEST(Domesday2DTest, PhaseTables) {
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
TEST(Domesday2DTest, DemodulateUndoesRemodulate) {
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

TEST(Domesday2DTest, MatrixAndItsInverse) {
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

// Flat colour carrying a known matrix: the pooled fit reads it back.
TEST(Domesday2DTest, FitsTheMatrix) {
  const FieldLattice fields[2] = {{0.0, 1.0}, {180.0, -1.0}};
  const auto lattice = d2d::fit_lattice(burst_raster(fields, 0.17));
  const d2d::Matrix m{0.0, 0.06, -0.02, 0.0};
  d2d::FlatPairs pairs;
  // Four flat colours, a band of rows each.
  const std::complex<double> colours[] = {
      {0.2, 0.1}, {-0.15, 0.3}, {0.05, -0.25}, {-0.3, -0.1}};
  d2d::Plane u(d2d::kRows * d2d::kLine, 0.0);
  d2d::Plane v(d2d::kRows * d2d::kLine, 0.0);
  for (size_t row = d2d::kFirstActiveRow; row < d2d::kEndActiveRow; ++row) {
    const auto& w = colours[(row - d2d::kFirstActiveRow) / 144];
    for (size_t n = 0; n < d2d::kLine; ++n) {
      u[(row * d2d::kLine) + n] = w.real();
      v[(row * d2d::kLine) + n] = w.imag();
    }
  }
  d2d::apply_matrix(m, lattice, true, u, v);
  d2d::add_flat_pairs(u, v, lattice, pairs);
  ASSERT_GT(pairs.plus.size(), 1000u);
  size_t used = 0;
  const auto fitted = d2d::fit_matrix(pairs, 1000, &used);
  ASSERT_TRUE(fitted.has_value());
  EXPECT_GT(used, 1000u);
  for (size_t i = 0; i < 4; ++i) EXPECT_NEAR((*fitted)[i], m[i], 1e-9) << i;
  // Too few pairs: no fit.
  EXPECT_FALSE(d2d::fit_matrix(d2d::FlatPairs{}, 1000, &used).has_value());
}

}  // namespace
}  // namespace orc_unit_test
