/*
 * File:        domesday2d.h
 * Module:      decode-orc Plugin SDK (support)
 * Purpose:     The woven PAL frame raster, its burst-fitted carrier lattice,
 *              per-row demodulation and the line-alternating chroma matrix,
 *              shared by the Domesday2D decoder and its calibration
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Domesday2D decodes a PAL still from its woven frame: field 1 line k on row
// 2(k - 1) and field 2 line k on row 2(k - 1) + 1, 624 rows of 1135 samples
// from 0H, in units of white - blanking (blanking 0, white 1). On the 4fsc
// grid a row's chroma is
//   C = U sin(theta) + s V cos(theta),  theta = n pi / 2 + phi
// with n the sample after 0H, phi the row's U carrier phase and s its
// V-switch. The grid is locked to the subcarrier, so phi falls 90 degrees a
// line and every shift between rows is a multiple of 90 degrees; phi and s
// of each field are fitted to its bursts once (burst phase phi + 45 degrees
// on an s = +1 line, phi + 135 degrees on s = -1). Demodulated on its own
// carrier, every row gives U = -Im(z) and V = s Re(z), z = 2 x exp(-j
// theta). docs-tech/domesday2d-decoder-investigation.md, Phases 3 and 4;
// tools/domesday2d-bench/proto.py and hanover.py are the prototype.

namespace orc::domesday2d {

inline constexpr size_t kRows = 624;
inline constexpr size_t kLine = 1135;
// The picture rows: field lines 23-310.
inline constexpr size_t kFirstActiveRow = 44;
inline constexpr size_t kEndActiveRow = 620;
// The burst window, samples after 0H.
inline constexpr size_t kBurstStart = 98;
inline constexpr size_t kBurstEnd = 138;

inline constexpr double kSampleRate = 4.0 * 4433618.75;

// The frame raster, row-major, kRows x kLine.
using Plane = std::vector<double>;

// Fills |raster| from the lines |line|(field, k) returns (field 0 or 1, k the
// 0-based field line, 0-311), each at least kLine samples of 10-bit codes.
template <typename LineFn>
void fill_raster(LineFn line, double blanking_level, double white_level,
                 Plane& raster) {
  raster.resize(kRows * kLine);
  const double scale = 1.0 / (white_level - blanking_level);
  for (size_t row = 0; row < kRows; ++row) {
    const int16_t* in = line(row % 2, row / 2);
    double* out = raster.data() + (row * kLine);
    for (size_t n = 0; n < kLine; ++n) {
      out[n] = (static_cast<double>(in[n]) - blanking_level) * scale;
    }
  }
}

// The carrier lattice of a raster: per row, the U carrier phase phi in
// degrees and the V-switch s (+1 or -1).
struct Lattice {
  std::vector<double> phi_deg;
  std::vector<double> s;
};

// Fits each field's lattice to its bursts over the picture rows.
Lattice fit_lattice(const Plane& raster);

// U and V of each row demodulated on its own carrier, rows outside the
// picture zeroed. |phase_deg| turns every row's reference (the decoder's
// chroma_phase).
void demodulate(const Plane& chroma, const Lattice& lattice, double phase_deg,
                Plane& u, Plane& v);

// The chroma that carries (U, V) on the lattice.
void remodulate(const Plane& u, const Plane& v, const Lattice& lattice,
                double phase_deg, Plane& chroma);

// PalColour's horizontal raised cosine (1.1 MHz / 0.93), applied along each
// row in place, zero beyond the row's ends.
void filter_rows_horizontal(Plane& plane);

// demodulate() and then filter_rows_horizontal(), the same values, row by
// row without the unfiltered planes.
void demodulate_filtered(const Plane& chroma, const Lattice& lattice,
                         double phase_deg, Plane& u, Plane& v);

// The line-alternating chroma error: a row of switch s reads
//   W_s = (I + s M) W
// for the true W = (U, V). M is row-major: {M_UU, M_UV, M_VU, M_VV}.
using Matrix = std::array<double, 4>;

// Applies (I + s M)^-1 to each row (|inverse| false: removes the error) or
// I + s M (|inverse| true: puts it back), in place.
void apply_matrix(const Matrix& m, const Lattice& lattice, bool inverse,
                  Plane& u, Plane& v);

// The (s = +1, s = -1) row pairs of flat colour that M is fitted from.
struct FlatPairs {
  std::vector<std::complex<double>> plus;
  std::vector<std::complex<double>> minus;
};

// Adds the pairs of field-adjacent rows (r, r + 2) of flat colour of
// demodulated, horizontally filtered planes: both rows' chroma above 0.03,
// each within 0.02 of itself 8 samples either side and of the row 4 on.
void add_flat_pairs(const Plane& u, const Plane& v, const Lattice& lattice,
                    FlatPairs& pairs);

// add_flat_pairs(), keeping a pair only where the 312-line partner pair (the
// field 2 rows below a field 1 pair, the field 1 rows above a field 2 pair:
// the same switches) alternates within |partner_tolerance| of it. A Hanover
// error repeats there; a colour stripe one field line high does not.
void add_flat_pairs(const Plane& u, const Plane& v, const Lattice& lattice,
                    double partner_tolerance, FlatPairs& pairs);

// |m| with its antisymmetric part, a rotation, replaced by |theta|:
// {m_UU, k + theta, k - theta, m_VV} with k = (m_UV + m_VU) / 2.
Matrix with_rotation(const Matrix& m, double theta);

// The rotation in |m|: (m_UV - m_VU) / 2.
double rotation_of(const Matrix& m);

// M fitted to pooled pairs by least squares, leaving out pairs whose
// alternating half exceeds 0.3 of their mean (picture, not error).
// std::nullopt with fewer than |minimum| pairs left. |used| reports how
// many were.
std::optional<Matrix> fit_matrix(const FlatPairs& pairs, size_t minimum,
                                 size_t* used);

}  // namespace orc::domesday2d
