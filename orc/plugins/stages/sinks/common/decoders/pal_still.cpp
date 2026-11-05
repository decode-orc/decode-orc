/*
 * File:        pal_still.cpp
 * Module:      orc-sdk-support
 * Purpose:     The woven PAL frame raster, its burst-fitted carrier lattice,
 *              per-row demodulation and the line-alternating chroma matrix,
 *              shared by the PAL Still decoder and its calibration
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "pal_still.h"

#include <algorithm>
#include <cmath>

namespace orc::pal_still {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDegrees = kPi / 180.0;

// PalColour's chroma bandwidth (palcolour.cpp): its raised cosine has
// radius 0.5 fs / bandwidth samples.
constexpr double kChromaBandwidthHz = 1100000.0 / 0.93;
constexpr int kHorizontalReach = 7;
constexpr size_t kHorizontalTaps = (2 * 7) + 1;

// The flat-colour test of add_flat_pairs().
constexpr double kMinFlatChroma = 0.03;
constexpr double kFlatTolerance = 0.02;
constexpr size_t kFlatSpan = 8;
// The truth grid's columns (samples 177-1098), less the span.
constexpr size_t kFlatFirstSample = 177 + kFlatSpan;
constexpr size_t kFlatEndSample = 177 + 922 - kFlatSpan;

// sin and cos of the carrier on |row| at sample n: theta = n pi / 2 + phi.
// On the 4fsc grid theta's quarter turns are exact, so each row needs only
// the sine and cosine of phi.
struct RowCarrier {
  double sin_phi;
  double cos_phi;

  // sin(n pi / 2 + phi) and cos(n pi / 2 + phi).
  double sin_at(size_t n) const {
    switch (n & 3U) {
      case 0:
        return sin_phi;
      case 1:
        return cos_phi;
      case 2:
        return -sin_phi;
      default:
        return -cos_phi;
    }
  }
  // The same, indexed by n mod 4: a table the compiler can vectorise.
  std::array<double, 4> sin_table() const {
    return {sin_phi, cos_phi, -sin_phi, -cos_phi};
  }
  std::array<double, 4> cos_table() const {
    return {cos_phi, -sin_phi, -cos_phi, sin_phi};
  }
  double cos_at(size_t n) const {
    switch (n & 3U) {
      case 0:
        return cos_phi;
      case 1:
        return -sin_phi;
      case 2:
        return -cos_phi;
      default:
        return sin_phi;
    }
  }
};

RowCarrier carrier(const Lattice& lattice, size_t row, double phase_deg) {
  const double phi = (lattice.phi_deg[row] + phase_deg) * kDegrees;
  return {std::sin(phi), std::cos(phi)};
}

bool active(size_t row) {
  return row >= kFirstActiveRow && row < kEndActiveRow;
}

// PalColour's horizontal raised cosine, normalised.
const std::array<double, kHorizontalTaps>& horizontal_taps() {
  static const std::array<double, kHorizontalTaps> taps = [] {
    std::array<double, kHorizontalTaps> h{};
    const double ca = 0.5 * kSampleRate / kChromaBandwidthHz;
    double sum = 0.0;
    for (int d = -kHorizontalReach; d <= kHorizontalReach; ++d) {
      const double r = std::min(ca, static_cast<double>(std::abs(d)));
      const double tap = 1.0 + std::cos(kPi * r / ca);
      h[static_cast<size_t>(d) + static_cast<size_t>(kHorizontalReach)] = tap;
      sum += tap;
    }
    for (double& tap : h) tap /= sum;
    return h;
  }();
  return taps;
}

// Row |x| demodulated on carrier |c| with V-switch |s|: U into |u|, V into |v|.
void demodulate_row(const double* x, const RowCarrier& c, double s, double* u,
                    double* v) {
  const auto sin_at = c.sin_table();
  const auto cos_at = c.cos_table();
  for (size_t n = 0; n < kLine; ++n) {
    // z = 2 x e^(-j theta): U = -Im(z) = 2 x sin(theta), V = s Re(z).
    u[n] = 2.0 * x[n] * sin_at[n & 3U];
    v[n] = s * 2.0 * x[n] * cos_at[n & 3U];
  }
}

// The taps along one row: |in| is the row with kHorizontalReach zeros either
// side, |out| kLine samples. Tap by tap along the whole row: each sample
// still sums its taps in order, and the inner loop vectorises.
void filter_row(const std::array<double, kHorizontalTaps>& taps,
                const double* in, double* out) {
  for (size_t n = 0; n < kLine; ++n) out[n] = taps[0] * in[n];
  for (size_t k = 1; k < taps.size(); ++k) {
    const double tap = taps[k];
    const double* from = in + k;
    for (size_t n = 0; n < kLine; ++n) out[n] += tap * from[n];
  }
}

}  // namespace

Lattice fit_lattice(const Plane& raster) {
  Lattice lattice;
  lattice.phi_deg.assign(kRows, 0.0);
  lattice.s.assign(kRows, 0.0);
  // Each row's burst as a phasor: z = 2 mean(x e^(-j pi n / 2)).
  std::vector<std::complex<double>> z(kRows);
  const double length = static_cast<double>(kBurstEnd - kBurstStart);
  for (size_t row = 0; row < kRows; ++row) {
    const double* x = raster.data() + (row * kLine);
    std::complex<double> acc;
    for (size_t n = kBurstStart; n < kBurstEnd; ++n) {
      const RowCarrier ref{0.0, 1.0};  // e^(-j pi n / 2)
      acc += x[n] * std::complex<double>(ref.cos_at(n), -ref.sin_at(n));
    }
    z[row] = 2.0 * acc / length;
  }
  std::vector<double> amplitudes;
  amplitudes.reserve(kEndActiveRow - kFirstActiveRow);
  for (size_t row = kFirstActiveRow; row < kEndActiveRow; ++row) {
    amplitudes.push_back(std::abs(z[row]));
  }
  const auto middle =
      amplitudes.begin() + static_cast<std::ptrdiff_t>(amplitudes.size() / 2);
  std::nth_element(amplitudes.begin(), middle, amplitudes.end());
  lattice.burst_amplitude = *middle;
  for (size_t field = 0; field < 2; ++field) {
    double best_magnitude = -1.0;
    double best_angle = 0.0;
    double best_s0 = 1.0;
    for (const double s0 : {1.0, -1.0}) {
      std::complex<double> sum;
      size_t count = 0;
      for (size_t row = field; row < kRows; row += 2) {
        if (!active(row)) continue;
        const size_t line = row / 2;
        const double sw = s0 * ((line % 2 == 0) ? 1.0 : -1.0);
        const double model =
            (-90.0 * static_cast<double>(line)) + (sw > 0.0 ? 45.0 : 135.0);
        const double residual = std::arg(z[row]) - (model * kDegrees);
        sum += std::polar(1.0, residual);
        ++count;
      }
      const std::complex<double> mean = sum / static_cast<double>(count);
      if (std::abs(mean) > best_magnitude) {
        best_magnitude = std::abs(mean);
        best_angle = std::arg(mean) / kDegrees;
        best_s0 = s0;
      }
    }
    for (size_t row = field; row < kRows; row += 2) {
      const size_t line = row / 2;
      lattice.phi_deg[row] = best_angle - (90.0 * static_cast<double>(line));
      lattice.s[row] = best_s0 * ((line % 2 == 0) ? 1.0 : -1.0);
    }
  }
  return lattice;
}

void demodulate(const Plane& chroma, const Lattice& lattice, double phase_deg,
                Plane& u, Plane& v) {
  u.resize(kRows * kLine);
  v.resize(kRows * kLine);
  for (size_t row = 0; row < kRows; ++row) {
    double* pu = u.data() + (row * kLine);
    double* pv = v.data() + (row * kLine);
    if (!active(row)) {
      std::fill(pu, pu + kLine, 0.0);
      std::fill(pv, pv + kLine, 0.0);
      continue;
    }
    demodulate_row(chroma.data() + (row * kLine),
                   carrier(lattice, row, phase_deg), lattice.s[row], pu, pv);
  }
}

void remodulate(const Plane& u, const Plane& v, const Lattice& lattice,
                double phase_deg, Plane& chroma) {
  chroma.resize(kRows * kLine);
  for (size_t row = 0; row < kRows; ++row) {
    const RowCarrier c = carrier(lattice, row, phase_deg);
    const double s = lattice.s[row];
    const auto sin_at = c.sin_table();
    const auto cos_at = c.cos_table();
    const double* pu = u.data() + (row * kLine);
    const double* pv = v.data() + (row * kLine);
    double* out = chroma.data() + (row * kLine);
    for (size_t n = 0; n < kLine; ++n) {
      out[n] = (pu[n] * sin_at[n & 3U]) + (s * pv[n] * cos_at[n & 3U]);
    }
  }
}

void filter_rows_horizontal(Plane& plane) {
  const auto& taps = horizontal_taps();
  std::vector<double> in(kLine + kHorizontalTaps - 1, 0.0);
  for (size_t row = 0; row < kRows; ++row) {
    double* x = plane.data() + (row * kLine);
    std::copy(x, x + kLine, in.begin() + kHorizontalReach);
    filter_row(taps, in.data(), x);
  }
}

void demodulate_filtered(const Plane& chroma, const Lattice& lattice,
                         double phase_deg, Plane& u, Plane& v) {
  const auto& taps = horizontal_taps();
  u.resize(kRows * kLine);
  v.resize(kRows * kLine);
  std::vector<double> in_u(kLine + kHorizontalTaps - 1, 0.0);
  std::vector<double> in_v(kLine + kHorizontalTaps - 1, 0.0);
  for (size_t row = 0; row < kRows; ++row) {
    double* pu = u.data() + (row * kLine);
    double* pv = v.data() + (row * kLine);
    if (!active(row)) {
      std::fill(pu, pu + kLine, 0.0);
      std::fill(pv, pv + kLine, 0.0);
      continue;
    }
    demodulate_row(chroma.data() + (row * kLine),
                   carrier(lattice, row, phase_deg), lattice.s[row],
                   in_u.data() + kHorizontalReach,
                   in_v.data() + kHorizontalReach);
    filter_row(taps, in_u.data(), pu);
    filter_row(taps, in_v.data(), pv);
  }
}

bool invertible(const Matrix& m) {
  for (const double s : {1.0, -1.0}) {
    const double det =
        ((1.0 + (s * m[0])) * (1.0 + (s * m[3]))) - (m[1] * m[2]);
    if (!(det >= kMinDeterminant)) return false;
  }
  return true;
}

std::optional<Matrix> in_phase_basis(const Matrix& m, double phase_deg) {
  const double c = std::cos(phase_deg * kDegrees);
  const double n = std::sin(phase_deg * kDegrees);
  // J M = [[M_VU, M_VV], [-M_UU, -M_UV]].
  const Matrix a{c + (n * m[2]), n * m[3], -n * m[0], c - (n * m[1])};
  const Matrix b{c * m[0], (c * m[1]) + n, (c * m[2]) - n, c * m[3]};
  const double det = (a[0] * a[3]) - (a[1] * a[2]);
  if (!(std::abs(det) >= kMinDeterminant)) return std::nullopt;
  // B A^-1, A^-1 = [[a3, -a1], [-a2, a0]] / det.
  return Matrix{((b[0] * a[3]) - (b[1] * a[2])) / det,
                ((b[1] * a[0]) - (b[0] * a[1])) / det,
                ((b[2] * a[3]) - (b[3] * a[2])) / det,
                ((b[3] * a[0]) - (b[2] * a[1])) / det};
}

void apply_matrix(const Matrix& m, const Lattice& lattice, bool inverse,
                  Plane& u, Plane& v) {
  // K = I + s M, or its inverse.
  std::array<Matrix, 2> k{};
  for (size_t i = 0; i < 2; ++i) {
    const double s = (i == 0) ? 1.0 : -1.0;
    const Matrix forward = {1.0 + (s * m[0]), s * m[1], s * m[2],
                            1.0 + (s * m[3])};
    if (inverse) {
      k[i] = forward;
    } else {
      const double det = (forward[0] * forward[3]) - (forward[1] * forward[2]);
      k[i] = {forward[3] / det, -forward[1] / det, -forward[2] / det,
              forward[0] / det};
    }
  }
  for (size_t row = 0; row < kRows; ++row) {
    const Matrix& a = k[lattice.s[row] > 0.0 ? 0 : 1];
    double* pu = u.data() + (row * kLine);
    double* pv = v.data() + (row * kLine);
    for (size_t n = 0; n < kLine; ++n) {
      const double x = pu[n];
      const double y = pv[n];
      pu[n] = (a[0] * x) + (a[1] * y);
      pv[n] = (a[2] * x) + (a[3] * y);
    }
  }
}

void add_flat_pairs(const Plane& u, const Plane& v, const Lattice& lattice,
                    FlatPairs& pairs) {
  add_flat_pairs(u, v, lattice, -1.0, pairs);
}

void add_flat_pairs(const Plane& u, const Plane& v, const Lattice& lattice,
                    double partner_tolerance, FlatPairs& pairs) {
  // Every test compares a magnitude with a threshold, so each is made on
  // squares: no square root in a loop that sees every sample of the
  // picture. The span stays inside each row (kFlatFirstSample and
  // kFlatEndSample allow for it).
  static_assert(kFlatFirstSample >= kFlatSpan &&
                kFlatEndSample + kFlatSpan <= kLine);
  const bool partner = partner_tolerance >= 0.0;
  const double partner2 = partner_tolerance * partner_tolerance;
  constexpr double kMin2 = kMinFlatChroma * kMinFlatChroma;
  constexpr double kFlat2 = kFlatTolerance * kFlatTolerance;
  const auto norm2 = [](double du, double dv) { return (du * du) + (dv * dv); };
  std::array<uint8_t, kLine> keep{};
  for (size_t row = kFirstActiveRow; row + 4 < kEndActiveRow; ++row) {
    if (lattice.s[row] == lattice.s[row + 2]) continue;
    const double* ua = u.data() + (row * kLine);
    const double* va = v.data() + (row * kLine);
    const double* ub = ua + (2 * kLine);
    const double* vb = va + (2 * kLine);
    const double* uc = ua + (4 * kLine);
    const double* vc = va + (4 * kLine);
    const size_t mate = (row % 2 == 0) ? row - 1 : row + 1;
    const double* um = u.data() + (mate * kLine);
    const double* vm = v.data() + (mate * kLine);
    const double* un = um + (2 * kLine);
    const double* vn = vm + (2 * kLine);
    const bool a_plus = lattice.s[row] > 0.0;
    // The tests without branches into |keep| (they vectorise), then the
    // kept pairs: on a map most samples have colour, and branching on each
    // test in turn mispredicts.
    for (size_t n = kFlatFirstSample; n < kFlatEndSample; ++n) {
      const size_t l = n - kFlatSpan;
      const size_t r = n + kFlatSpan;
      bool ok = (norm2(ua[n], va[n]) > kMin2) & (norm2(ub[n], vb[n]) > kMin2);
      ok &= norm2(ua[n] - uc[n], va[n] - vc[n]) < kFlat2;
      ok &= norm2(ua[n] - ua[l], va[n] - va[l]) < kFlat2;
      ok &= norm2(ua[n] - ua[r], va[n] - va[r]) < kFlat2;
      ok &= norm2(ub[n] - ub[l], vb[n] - vb[l]) < kFlat2;
      ok &= norm2(ub[n] - ub[r], vb[n] - vb[r]) < kFlat2;
      ok &= !partner | (norm2((ua[n] - ub[n]) - (um[n] - un[n]),
                              (va[n] - vb[n]) - (vm[n] - vn[n])) < partner2);
      keep[n] = static_cast<uint8_t>(ok);
    }
    for (size_t n = kFlatFirstSample; n < kFlatEndSample; ++n) {
      if (keep[n] == 0) continue;
      const std::complex<double> a(ua[n], va[n]);
      const std::complex<double> b(ub[n], vb[n]);
      pairs.plus.push_back(a_plus ? a : b);
      pairs.minus.push_back(a_plus ? b : a);
    }
  }
}

Matrix with_rotation(const Matrix& m, double theta) {
  const double k = (m[1] + m[2]) / 2.0;
  return Matrix{m[0], k + theta, k - theta, m[3]};
}

std::optional<double> fit_rotation(const FlatPairs& pairs, const Matrix& known,
                                   size_t minimum, size_t* used) {
  // With r = half - S mean and J mean = (mean_V, -mean_U), theta minimises
  // sum |r - theta J mean|^2: theta = sum r . J mean / sum |mean|^2.
  const double k = (known[1] + known[2]) / 2.0;
  std::optional<double> theta;
  size_t count = 0;
  for (int pass = 0; pass <= kCentredPasses; ++pass) {
    const double cut = (pass == 0) ? kFirstCut : kCentredCut;
    const double about = theta.value_or(0.0);
    double num = 0.0;
    double den = 0.0;
    count = 0;
    for (size_t i = 0; i < pairs.plus.size(); ++i) {
      const auto mean = (pairs.plus[i] + pairs.minus[i]) / 2.0;
      const auto half = (pairs.plus[i] - pairs.minus[i]) / 2.0;
      const double mu = mean.real();
      const double mv = mean.imag();
      const double ru = half.real() - ((known[0] * mu) + (k * mv));
      const double rv = half.imag() - ((k * mu) + (known[3] * mv));
      const double m2 = (mu * mu) + (mv * mv);
      // The first pass cuts on the half itself, later ones on the residual
      // about the last rotation.
      const double eu = (pass == 0) ? half.real() : ru - (about * mv);
      const double ev = (pass == 0) ? half.imag() : rv + (about * mu);
      if ((eu * eu) + (ev * ev) > cut * cut * m2) continue;
      num += (ru * mv) - (rv * mu);
      den += m2;
      ++count;
    }
    if (count < minimum || !(den > 0.0)) {
      theta.reset();
      break;
    }
    theta = num / den;
  }
  if (used) *used = count;
  if (!theta || !(std::abs(*theta) <= kMaxRotation)) return std::nullopt;
  return theta;
}

}  // namespace orc::pal_still
