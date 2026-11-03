/*
 * File:        domesday2d.cpp
 * Module:      orc-sdk-support
 * Purpose:     The woven PAL frame raster, its burst-fitted carrier lattice,
 *              per-row demodulation and the line-alternating chroma matrix,
 *              shared by the Domesday2D decoder and its calibration
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <orc/support/domesday2d.h>

#include <algorithm>
#include <cmath>

namespace orc::domesday2d {

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
// fit_matrix(): pairs whose alternating half is larger than this share of
// their mean are picture, not error (Phase 2's largest error is about 0.17).
constexpr double kMaxAlternatingShare = 0.3;

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
  u.assign(kRows * kLine, 0.0);
  v.assign(kRows * kLine, 0.0);
  for (size_t row = kFirstActiveRow; row < kEndActiveRow; ++row) {
    const RowCarrier c = carrier(lattice, row, phase_deg);
    const double s = lattice.s[row];
    const double* x = chroma.data() + (row * kLine);
    double* pu = u.data() + (row * kLine);
    double* pv = v.data() + (row * kLine);
    for (size_t n = 0; n < kLine; ++n) {
      // z = 2 x e^(-j theta): U = -Im(z) = 2 x sin(theta), V = s Re(z).
      pu[n] = 2.0 * x[n] * c.sin_at(n);
      pv[n] = s * 2.0 * x[n] * c.cos_at(n);
    }
  }
}

void remodulate(const Plane& u, const Plane& v, const Lattice& lattice,
                double phase_deg, Plane& chroma) {
  chroma.assign(kRows * kLine, 0.0);
  for (size_t row = 0; row < kRows; ++row) {
    const RowCarrier c = carrier(lattice, row, phase_deg);
    const double s = lattice.s[row];
    const double* pu = u.data() + (row * kLine);
    const double* pv = v.data() + (row * kLine);
    double* out = chroma.data() + (row * kLine);
    for (size_t n = 0; n < kLine; ++n) {
      out[n] = (pu[n] * c.sin_at(n)) + (s * pv[n] * c.cos_at(n));
    }
  }
}

void filter_rows_horizontal(Plane& plane) {
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
  std::vector<double> in(kLine + kHorizontalTaps - 1, 0.0);
  for (size_t row = 0; row < kRows; ++row) {
    double* x = plane.data() + (row * kLine);
    std::copy(x, x + kLine, in.begin() + kHorizontalReach);
    // Tap by tap along the whole row: each sample still sums its taps in
    // order, and the inner loop vectorises.
    std::fill(x, x + kLine, 0.0);
    for (size_t k = 0; k < taps.size(); ++k) {
      const double tap = taps[k];
      const double* from = in.data() + k;
      for (size_t n = 0; n < kLine; ++n) x[n] += tap * from[n];
    }
  }
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
  const auto w = [&](size_t row, size_t n) {
    return std::complex<double>(u[(row * kLine) + n], v[(row * kLine) + n]);
  };
  const auto flat_along = [&](size_t row, size_t n) {
    const auto x = w(row, n);
    return std::abs(x - w(row, (n + kLine - kFlatSpan) % kLine)) <
               kFlatTolerance &&
           std::abs(x - w(row, (n + kFlatSpan) % kLine)) < kFlatTolerance;
  };
  for (size_t row = kFirstActiveRow; row + 4 < kEndActiveRow; ++row) {
    if (lattice.s[row] == lattice.s[row + 2]) continue;
    for (size_t n = kFlatFirstSample; n < kFlatEndSample; ++n) {
      const auto a = w(row, n);
      const auto b = w(row + 2, n);
      if (std::abs(a) <= kMinFlatChroma || std::abs(b) <= kMinFlatChroma) {
        continue;
      }
      if (!flat_along(row, n) || !flat_along(row + 2, n)) continue;
      if (std::abs(a - w(row + 4, n)) >= kFlatTolerance) continue;
      const bool a_plus = lattice.s[row] > 0.0;
      pairs.plus.push_back(a_plus ? a : b);
      pairs.minus.push_back(a_plus ? b : a);
    }
  }
}

std::optional<Matrix> fit_matrix(const FlatPairs& pairs, size_t minimum,
                                 size_t* used) {
  // half = M mean, by least squares over the kept pairs: the normal
  // equations of the two columns of M share the 2 x 2 Gram matrix of mean.
  double xx = 0.0;
  double xy = 0.0;
  double yy = 0.0;
  std::array<double, 4> cross{};  // sum mean_i half_j
  size_t count = 0;
  for (size_t i = 0; i < pairs.plus.size(); ++i) {
    const auto mean = (pairs.plus[i] + pairs.minus[i]) / 2.0;
    const auto half = (pairs.plus[i] - pairs.minus[i]) / 2.0;
    if (std::abs(half) > kMaxAlternatingShare * std::abs(mean)) continue;
    xx += mean.real() * mean.real();
    xy += mean.real() * mean.imag();
    yy += mean.imag() * mean.imag();
    cross[0] += mean.real() * half.real();
    cross[1] += mean.imag() * half.real();
    cross[2] += mean.real() * half.imag();
    cross[3] += mean.imag() * half.imag();
    ++count;
  }
  if (used) *used = count;
  const double det = (xx * yy) - (xy * xy);
  if (count < minimum || det <= 0.0) return std::nullopt;
  // Row j of M solves [xx xy; xy yy] m_j = [sum re*half_j, sum im*half_j].
  const auto solve = [&](double a, double b) {
    return std::array<double, 2>{((yy * a) - (xy * b)) / det,
                                 ((xx * b) - (xy * a)) / det};
  };
  const auto top = solve(cross[0], cross[1]);
  const auto bottom = solve(cross[2], cross[3]);
  return Matrix{top[0], top[1], bottom[0], bottom[1]};
}

}  // namespace orc::domesday2d
