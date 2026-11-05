/*
 * File:        pal_still.h
 * Module:      orc-chroma-decoders
 * Purpose:     The woven PAL frame raster, its burst-fitted carrier lattice,
 *              per-row demodulation and the line-alternating chroma matrix,
 *              shared by the PAL Still decoder and its calibration
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// PAL Still decodes a PAL still from its woven frame: field 1 line k on row
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

namespace orc::pal_still {

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
// degrees and the V-switch s (+1 or -1); and the median amplitude of the
// picture rows' bursts, in units of white - blanking (nominal 0.21).
struct Lattice {
  std::vector<double> phi_deg;
  std::vector<double> s;
  double burst_amplitude = 0.0;
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

// Whether I + M and I - M are both far enough from singular to undo:
// determinants of at least kMinDeterminant, so the correction scales no
// row's chroma by more than about 1 / kMinDeterminant. Matrices within the
// parameters' +/-0.5 can reach 0 (M_UU = M_VV = -0.5, M_UV = M_VU = 0.5).
inline constexpr double kMinDeterminant = 0.25;
bool invertible(const Matrix& m);

// Applies (I + s M)^-1 to each row (|inverse| false: removes the error) or
// I + s M (|inverse| true: puts it back), in place. |m| must be
// invertible() for |inverse| false.
void apply_matrix(const Matrix& m, const Lattice& lattice, bool inverse,
                  Plane& u, Plane& v);

// |m|, measured on rows demodulated at phase 0, as it reads on rows
// demodulated at |phase_deg| (the decoder's chroma_phase). Turning the
// reference by phi turns a row of switch s by s phi:
//   W_phi = (c I + s n J)(I + s M) W = (I + s B A^-1) A W,
//   A = c I + n J M,  B = c M + n J,
// with c = cos phi, n = sin phi and J = [[0, 1], [-1, 0]], so the matrix
// there is B A^-1: to first order M with its rotation moved by tan phi.
// Undoing it leaves A W, the picture desaturated by cos phi as PalColour's
// two-line average leaves it. std::nullopt when A is singular (phi near
// +/-90 degrees, where no chroma remains).
std::optional<Matrix> in_phase_basis(const Matrix& m, double phase_deg);

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
// field 2 rows above a field 1 pair, the field 1 rows below a field 2 pair:
// frame line k + 312 is field 2 line k - 1, with the same switch)
// alternates within |partner_tolerance| of it. A Hanover error repeats
// there; a colour stripe one field line high does not.
void add_flat_pairs(const Plane& u, const Plane& v, const Lattice& lattice,
                    double partner_tolerance, FlatPairs& pairs);

// |m| with its antisymmetric part, a rotation, replaced by |theta|:
// {m_UU, k + theta, k - theta, m_VV} with k = (m_UV + m_VU) / 2.
Matrix with_rotation(const Matrix& m, double theta);

// The rotation theta of M = S + theta J (J = [[0, 1], [-1, 0]]) with the
// symmetric part S held at |known|'s, by least squares: half - S mean =
// theta J mean has one unknown, which one hue determines (a fit of the
// whole matrix has 2 x 2 normal equations, singular on a frame of one hue). A
// first pass leaves out pairs whose alternating half exceeds kFirstCut of their
// mean; kCentredPasses more leave out those whose residual about the last
// pass's rotation exceeds kCentredCut of it (a cut on the half itself drops the
// pairs that carry a large rotation, and reads it low). std::nullopt with
// fewer than |minimum| pairs left in any pass, or a rotation beyond
// kMaxRotation. |used| reports the last pass's pairs.
// docs-tech/domesday2d-decoder-investigation.md, Phase 8;
// tools/domesday2d-bench/hanover.py (frame_matrix) is the prototype.
inline constexpr double kFirstCut = 0.6;
inline constexpr double kCentredCut = 0.3;
inline constexpr int kCentredPasses = 3;
inline constexpr double kMaxRotation = 0.5;
std::optional<double> fit_rotation(const FlatPairs& pairs, const Matrix& known,
                                   size_t minimum, size_t* used);

}  // namespace orc::pal_still
