/*
 * File:        domesday2ddecoder.cpp
 * Module:      orc-core
 * Purpose:     Domesday2D: a PAL still-frame decoder that separates each
 *              frame on its woven raster
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "domesday2ddecoder.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "../video_parameter_safety.h"
#include "transformpal.h"

namespace {

namespace d2d = orc::domesday2d;
using d2d::kEndActiveRow;
using d2d::kFirstActiveRow;
using d2d::kLine;
using d2d::kRows;
using d2d::Plane;

constexpr double kPi = 3.14159265358979323846;

// Several steps take the rows in pairs.
static_assert(kRows % 2 == 0, "the raster's rows pair up");

// The vertical fit is also compiled for AVX2, and the loader picks the copy
// the CPU can run (an ifunc: GCC or Clang on x86-64 glibc). The fit is bound
// by double divides and square roots, which AVX2 does twice as many of a
// cycle as the baseline's SSE2. AVX2 without FMA cannot contract a * b + c,
// so both copies give the same results. Not under a sanitizer: the ifunc's
// resolver runs while the loader relocates the plugin, before the
// sanitizer's runtime is ready, and the instrumented resolver crashes it.
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define DOMESDAY2D_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define DOMESDAY2D_SANITIZED 1
#endif
#endif
#if defined(__x86_64__) && defined(__ELF__) && defined(__GLIBC__) && \
    defined(__has_attribute) && !defined(DOMESDAY2D_SANITIZED)
#if __has_attribute(target_clones)
#define DOMESDAY2D_CLONES __attribute__((target_clones("avx2", "default")))
#endif
#endif
#ifndef DOMESDAY2D_CLONES
#define DOMESDAY2D_CLONES
#endif

// ── Phase 4's parameters (adaptive.py DEFAULTS, hanover.masked_average) ──

// The vertical fit: lambda = kLambda0 + kLambdaNoise x the blank-line
// noise squared + kLambdaDetail x the luma detail (Phase 10: the floor
// rises with the noise, as a noise-weighted data term's would).
// kLambda0, kLambdaDetail and kDetailGate were tuned in Phase 10 on the
// softened, jittered bench (videosynth 049c712), the held-out set and 66
// real held runs; all three agree on them at up to a single capture's
// noise. At four times that noise a stronger fit (k up to 10) does better.
constexpr double kLambda0 = 0.03;
constexpr double kLambdaNoise = 10.0;
constexpr double kLambdaDetail = 2.0;
constexpr double kCharbonnierEps = 0.002;
constexpr int kReweightings = 6;
// The luma detail: the 3.5-5 MHz band's local r.m.s. over 5 rows x 15
// samples, H. The fit's is only what is not chroma the transform left in
// luma at colour edges (Phase 10): that has no 2.5-3.2 MHz band beside it
// and is a fraction of the chroma there, so H counts as far as it is
// kLeakRatio x that band's r.m.s. or exceeds kLeakChroma x the chroma's.
constexpr double kDetailLowHz = 3.5e6;
constexpr double kDetailHighHz = 5.0e6;
constexpr double kBelowChromaLowHz = 2.5e6;
constexpr double kBelowChromaHighHz = 3.2e6;
constexpr double kLeakRatio = 2.5;
constexpr double kLeakChroma = 0.25;
constexpr size_t kDetailRows = 5;
constexpr size_t kDetailSamples = 15;
// The reassignment: soft shrinkage against the blanked lines' chroma-band
// noise (raster rows 12-31, field lines 7-16, samples 200-1099), its
// envelope over 3 rows x 9 samples, gated by H / (H + kDetailGate).
constexpr double kNoiseLowHz = 3.0e6;
constexpr double kNoiseHighHz = 5.9e6;
constexpr size_t kBlankFirstRow = 12;
constexpr size_t kBlankEndRow = 32;
constexpr size_t kBlankFirstSample = 200;
constexpr size_t kBlankEndSample = 1100;
constexpr size_t kEnvelopeRows = 3;
constexpr size_t kEnvelopeSamples = 9;
// The per-frame Hanover rotation (Phases 7 and 8, hanover.frame_matrix()):
// the partner pair's alternation must match within this, and the fit needs
// this many pairs, or the frame takes the configured matrix.
constexpr double kPartnerTolerance = 0.03;
constexpr size_t kMinFramePairs = 1000;
constexpr double kMinEnvelopeSquare = 1e-12;
constexpr double kDetailGate = 0.03;
// The masked averaging: rows agree within kMaskThreshold (vector
// difference, smoothed over kMaskSmoothing samples), the mask falling to 0
// over kMaskSoft beyond it.
constexpr double kMaskThreshold = 0.04;
constexpr double kMaskSoft = 0.01;
constexpr size_t kMaskSmoothing = 9;

// The colour killer: a frame whose median burst amplitude is below
// PalColour's kill threshold (130000 / 128 of the 16-bit TBC range's white -
// blanking, on its burst phasor of half the amplitude) has no colour to
// decode. In units of white - blanking, about a quarter of the nominal 0.21.
const double kBurstKillAmplitude =
    2.0 * (130000.0 / 128.0) /
    static_cast<double>(orc::kTbcPalWhite - orc::kTbcPalBlanking);

// The decoded picture's samples (kPalActiveVideoStart-End): the transform's
// output is kept there only.
constexpr int32_t kFirstSample = 157;
constexpr int32_t kEndSample = 157 + 948;

// The transform's test: agreements of the two reflections closer than this
// are a tie (U wins), and a bin whose magnitude squared is below kSilentBin
// is empty. A tile's chroma bins are of order 10^2-10^5.
constexpr double kTieTolerance = 1e-9;
constexpr double kSilentBin = 1e-18;

// ── Filters ──────────────────────────────────────────────────────────────

// A 129-tap Blackman-windowed band-pass, as the still bench's band_filter().
constexpr size_t kBandTaps = 129;

std::vector<double> band_taps(double low_hz, double high_hz) {
  std::vector<double> taps(kBandTaps);
  const double lo = low_hz / d2d::kSampleRate;
  const double hi = high_hz / d2d::kSampleRate;
  constexpr size_t kHalf = kBandTaps / 2;
  const auto half = static_cast<double>(kHalf);
  for (size_t n = 0; n < kBandTaps; ++n) {
    const double m = static_cast<double>(n) - half;
    const auto lowpass = [m](double fc) {
      return m == 0.0 ? 2.0 * fc : std::sin(2.0 * kPi * fc * m) / (kPi * m);
    };
    const double t =
        2.0 * kPi * static_cast<double>(n) / static_cast<double>(kBandTaps - 1);
    const double window =
        0.42 - (0.5 * std::cos(t)) + (0.08 * std::cos(2.0 * t));
    taps[n] = (lowpass(hi) - lowpass(lo)) * window;
  }
  return taps;
}

// |taps| (symmetric) along the row |x| into |y|, the row's end samples
// extended; |padded| is scratch.
void filter_row(const double* x, const std::vector<double>& taps,
                std::vector<double>& padded, double* y) {
  const size_t reach = taps.size() / 2;
  padded.resize(kLine + (2 * reach));
  std::fill(padded.begin(), padded.begin() + static_cast<ptrdiff_t>(reach),
            x[0]);
  std::copy(x, x + kLine, padded.begin() + static_cast<ptrdiff_t>(reach));
  std::fill(padded.end() - static_cast<ptrdiff_t>(reach), padded.end(),
            x[kLine - 1]);
  // Tap by tap along the whole row: each sample still sums its taps in
  // order, and the inner loop vectorises.
  const double* centre = padded.data() + reach;
  for (size_t n = 0; n < kLine; ++n) y[n] = taps[reach] * centre[n];
  for (size_t k = 1; k <= reach; ++k) {
    const double tap = taps[reach + k];
    const double* ahead = centre + k;
    const double* behind = centre - k;
    for (size_t n = 0; n < kLine; ++n) {
      y[n] += tap * (ahead[n] + behind[n]);
    }
  }
}

// scipy.ndimage's 'reflect' boundary: d c b a | a b c d | d c b a.
size_t reflect(ptrdiff_t i, size_t n) {
  const auto size = static_cast<ptrdiff_t>(n);
  while (i < 0 || i >= size) {
    i = (i < 0) ? -i - 1 : (2 * size) - i - 1;
  }
  return static_cast<size_t>(i);
}

// The sums of each row |x|[r] (kLine samples) over windows of |width| (odd)
// samples centred on each sample, the ends reflected, by a running sum, into
// |out|[r]. Each sum waits on the one before it, so the rows' sums are
// interleaved: N independent chains instead of one.
template <size_t N>
void window_sums(const std::array<const double*, N>& x, size_t width,
                 const std::array<double*, N>& out,
                 std::vector<double>& padded) {
  const size_t half = width / 2;
  const size_t length = kLine + width - 1;
  padded.resize(N * length);
  std::array<const double*, N> p{};
  for (size_t r = 0; r < N; ++r) {
    double* row = padded.data() + (r * length);
    for (size_t i = 0; i < half; ++i) {
      row[i] = x[r][reflect(
          static_cast<ptrdiff_t>(i) - static_cast<ptrdiff_t>(half), kLine)];
      row[kLine + half + i] =
          x[r][reflect(static_cast<ptrdiff_t>(kLine + i), kLine)];
    }
    std::copy(x[r], x[r] + kLine, row + half);
    p[r] = row;
  }
  std::array<double, N> acc{};
  for (size_t r = 0; r < N; ++r) {
    for (size_t i = 0; i < width; ++i) acc[r] += p[r][i];
    out[r][0] = acc[r];
  }
  for (size_t i = 1; i < kLine; ++i) {
    for (size_t r = 0; r < N; ++r) {
      acc[r] += p[r][i + width - 1] - p[r][i - 1];
      out[r][i] = acc[r];
    }
  }
}

// Calls |fn|(y, scale) for each row with y the sum over |rows| (odd) rows of
// |across| around it, the raster's edges reflected, still to be multiplied
// by |scale| for the mean; |across| holds each row's window sums.
template <typename RowFn>
void box_rows(const Plane& across, size_t rows, size_t samples,
              std::vector<double>& sum, RowFn fn) {
  const double scale = 1.0 / static_cast<double>(samples * rows);
  const auto half_y = static_cast<ptrdiff_t>(rows / 2);
  sum.resize(kLine);
  for (size_t row = 0; row < kRows; ++row) {
    double* y = sum.data();
    const auto at = [&](ptrdiff_t d) {
      return across.data() +
             (reflect(static_cast<ptrdiff_t>(row) + d, kRows) * kLine);
    };
    // 0 + x is x: the first row is copied, the rest added in order.
    std::copy(at(-half_y), at(-half_y) + kLine, y);
    for (ptrdiff_t d = 1 - half_y; d <= half_y; ++d) {
      const double* x = at(d);
      for (size_t n = 0; n < kLine; ++n) y[n] += x[n];
    }
    fn(row, y, scale);
  }
}

bool any_nonzero(const d2d::Matrix& m) {
  return std::any_of(m.begin(), m.end(), [](double x) { return x != 0.0; });
}

// ── Phase 4's chroma path ────────────────────────────────────────────────

// |luma| = |raster| - |chroma|, and the local r.m.s. of its bands
// (|band_of|(x, high, low) filters a row): H, the 3.5-5 MHz band's, into
// |high|, and the fit's detail into |detail|. Row by row: each row's bands
// and chroma are squared and summed across, into |across|, |across_low| and
// |across_chroma|, while they are still in cache.
template <typename BandFn>
void luma_detail(const Plane& raster, const Plane& chroma, BandFn band_of,
                 Plane& luma, Plane& across, Plane& across_low,
                 Plane& across_chroma, Plane& high, Plane& detail,
                 std::vector<double>& row_a, std::vector<double>& row_b) {
  luma.resize(kRows * kLine);
  across.resize(kRows * kLine);
  across_low.resize(kRows * kLine);
  across_chroma.resize(kRows * kLine);
  std::vector<double> band(2 * kLine);
  std::vector<double> low(2 * kLine);
  std::vector<double> energy(2 * kLine);
  const auto sums = [&](std::vector<double>& x, Plane& to, size_t base) {
    window_sums<2>({x.data(), x.data() + kLine}, kDetailSamples,
                   {to.data() + base, to.data() + base + kLine}, row_b);
  };
  for (size_t row = 0; row < kRows; row += 2) {
    const size_t base = row * kLine;
    for (size_t n = 0; n < 2 * kLine; ++n) {
      luma[base + n] = raster[base + n] - chroma[base + n];
      energy[n] = chroma[base + n] * chroma[base + n];
    }
    band_of(luma.data() + base, band.data(), low.data());
    band_of(luma.data() + base + kLine, band.data() + kLine,
            low.data() + kLine);
    for (double& x : band) x *= x;
    for (double& x : low) x *= x;
    sums(band, across, base);
    sums(low, across_low, base);
    sums(energy, across_chroma, base);
  }
  const auto rms = [](double sum, double scale) {
    return std::sqrt(std::max(sum * scale, 0.0));
  };
  high.resize(kRows * kLine);
  box_rows(across, kDetailRows, kDetailSamples, row_a,
           [&](size_t row, const double* sum, double scale) {
             double* out = high.data() + (row * kLine);
             for (size_t n = 0; n < kLine; ++n) out[n] = rms(sum[n], scale);
           });
  // The chroma's r.m.s. first, then the detail over it in place.
  detail.resize(kRows * kLine);
  box_rows(across_chroma, kDetailRows, kDetailSamples, row_a,
           [&](size_t row, const double* sum, double scale) {
             double* out = detail.data() + (row * kLine);
             for (size_t n = 0; n < kLine; ++n) out[n] = rms(sum[n], scale);
           });
  box_rows(across_low, kDetailRows, kDetailSamples, row_a,
           [&](size_t row, const double* sum, double scale) {
             const size_t base = row * kLine;
             double* out = detail.data() + base;
             for (size_t n = 0; n < kLine; ++n) {
               const double h = high[base + n];
               out[n] = std::max(std::min(h, kLeakRatio * rms(sum[n], scale)),
                                 h - (kLeakChroma * out[n]));
             }
           });
}

// The r.m.s. of the chroma band on the blanked lines.
double blank_noise(const Plane& raster, std::vector<double>& padded) {
  static const auto taps = band_taps(kNoiseLowHz, kNoiseHighHz);
  std::vector<double> band(kLine);
  double sum = 0.0;
  for (size_t row = kBlankFirstRow; row < kBlankEndRow; ++row) {
    filter_row(raster.data() + (row * kLine), taps, padded, band.data());
    for (size_t n = kBlankFirstSample; n < kBlankEndSample; ++n) {
      sum += band[n] * band[n];
    }
  }
  return std::sqrt(sum /
                   static_cast<double>((kBlankEndRow - kBlankFirstRow) *
                                       (kBlankEndSample - kBlankFirstSample)));
}

// 1 at or below the threshold, 0 from kMaskSoft beyond it, from |y|, the
// differences' sums over kMaskSmoothing samples; in place.
void ramp(double* y) {
  const double scale = 1.0 / static_cast<double>(kMaskSmoothing);
  for (size_t n = 0; n < kLine; ++n) {
    const double mean = y[n] * scale;
    y[n] = std::clamp(1.0 - ((mean - kMaskThreshold) / kMaskSoft), 0.0, 1.0);
  }
}

// Partner-checked masked field-line averaging (hanover.masked_average()) of
// |u|, |v| into |out_u|, |out_v|: rows r -/+ 2 are the field lines either
// side, wrapping at the raster's ends; the 312-line partner is the row above
// a field 1 (even) row and below a field 2 row. Each row and its partner
// share their difference, so the rows are taken in those pairs, (623, 0),
// (1, 2), ..., (621, 622), and nothing leaves cache between the steps.
void masked_average(const Plane& u, const Plane& v, Plane& out_u, Plane& out_v,
                    std::vector<double>& scratch) {
  const auto at = [](ptrdiff_t row) {
    const auto rows = static_cast<ptrdiff_t>(kRows);
    return static_cast<size_t>(((row % rows) + rows) % rows) * kLine;
  };
  out_u.resize(kRows * kLine);
  out_v.resize(kRows * kLine);
  // Per pair member: dev_u, dev_v, step; then the shared partner difference
  // and the ramps' outputs.
  scratch.resize(10 * kLine);
  double* dev_u[2] = {scratch.data(), scratch.data() + kLine};
  double* dev_v[2] = {scratch.data() + (2 * kLine),
                      scratch.data() + (3 * kLine)};
  double* step[2] = {scratch.data() + (4 * kLine),
                     scratch.data() + (5 * kLine)};
  double* partner = scratch.data() + (6 * kLine);
  double* step_mask[2] = {scratch.data() + (7 * kLine),
                          scratch.data() + (8 * kLine)};
  double* partner_mask = scratch.data() + (9 * kLine);
  std::vector<double> padded;
  for (ptrdiff_t odd = -1; odd < static_cast<ptrdiff_t>(kRows) - 1; odd += 2) {
    const size_t rows[2] = {at(odd), at(odd + 1)};
    for (size_t k = 0; k < 2; ++k) {
      const auto r = (k == 0) ? odd : odd + 1;
      const size_t above = at(r - 2);
      const size_t below = at(r + 2);
      const size_t here = rows[k];
      for (size_t n = 0; n < kLine; ++n) {
        dev_u[k][n] = u[here + n] - ((0.5 * u[here + n]) +
                                     (0.25 * (u[above + n] + u[below + n])));
        dev_v[k][n] = v[here + n] - ((0.5 * v[here + n]) +
                                     (0.25 * (v[above + n] + v[below + n])));
        const double du = u[above + n] - u[below + n];
        const double dv = v[above + n] - v[below + n];
        step[k][n] = std::sqrt((du * du) + (dv * dv));
      }
    }
    // |a - b| is |b - a|: one difference serves both rows.
    for (size_t n = 0; n < kLine; ++n) {
      const double du = dev_u[1][n] - dev_u[0][n];
      const double dv = dev_v[1][n] - dev_v[0][n];
      partner[n] = std::sqrt((du * du) + (dv * dv));
    }
    window_sums<3>({partner, step[0], step[1]}, kMaskSmoothing,
                   {partner_mask, step_mask[0], step_mask[1]}, padded);
    ramp(partner_mask);
    for (size_t k = 0; k < 2; ++k) {
      ramp(step_mask[k]);
      const size_t here = rows[k];
      for (size_t n = 0; n < kLine; ++n) {
        const double m = step_mask[k][n] * partner_mask[n];
        out_u[here + n] = u[here + n] - (m * dev_u[k][n]);
        out_v[here + n] = v[here + n] - (m * dev_v[k][n]);
      }
    }
  }
}

// The vector-Charbonnier vertical fit over the picture rows, in place:
// six reweighted least-squares iterations, each one tridiagonal solve per
// column (rows on the outer loop, so each sweep runs along whole rows).
DOMESDAY2D_CLONES
void vertical_fit(Plane& u, Plane& v, const Plane& detail, double floor) {
  constexpr size_t kCount = kEndActiveRow - kFirstActiveRow;
  constexpr size_t kPairs = kCount - 1;
  // Each column is solved on its own, so the picture is taken in blocks of
  // columns whose working arrays stay in cache through all six iterations.
  constexpr size_t kBlock = 32;
  std::vector<double> lambda(kPairs * kBlock);
  std::vector<double> u0(kCount * kBlock);
  std::vector<double> v0(kCount * kBlock);
  std::vector<double> a(kCount * kBlock);
  std::vector<double> b(kCount * kBlock);
  std::vector<double> weight(kPairs * kBlock);
  std::vector<double> cp(kCount * kBlock);
  std::vector<double> dpa(kCount * kBlock);
  std::vector<double> dpb(kCount * kBlock);
  for (size_t first = 0; first < kLine; first += kBlock) {
    const size_t w = std::min(kBlock, kLine - first);
    const auto at = [&](size_t i, size_t n) { return (i * w) + n; };
    const auto source = [&](size_t i, size_t n) {
      return ((kFirstActiveRow + i) * kLine) + first + n;
    };
    // Each row difference takes the mean of its two rows' lambda.
    for (size_t i = 0; i < kPairs; ++i) {
      for (size_t n = 0; n < w; ++n) {
        lambda[at(i, n)] =
            0.5 * ((floor + (kLambdaDetail * detail[source(i, n)])) +
                   (floor + (kLambdaDetail * detail[source(i + 1, n)])));
      }
    }
    bool empty = true;
    for (size_t i = 0; i < kCount; ++i) {
      for (size_t n = 0; n < w; ++n) {
        u0[at(i, n)] = u[source(i, n)];
        v0[at(i, n)] = v[source(i, n)];
        empty = empty && u0[at(i, n)] == 0.0 && v0[at(i, n)] == 0.0;
      }
    }
    // The fit of a block without chroma (beyond the picture's samples) is
    // that block.
    if (empty) continue;
    std::copy(u0.begin(), u0.end(), a.begin());
    std::copy(v0.begin(), v0.end(), b.begin());
    for (int iteration = 0; iteration < kReweightings; ++iteration) {
      for (size_t i = 0; i < kPairs; ++i) {
        for (size_t n = 0; n < w; ++n) {
          const size_t k = at(i, n);
          const double da = a[k + w] - a[k];
          const double db = b[k + w] - b[k];
          const double g = std::sqrt((kCharbonnierEps * kCharbonnierEps) +
                                     (da * da) + (db * db));
          weight[k] = lambda[k] / g;
        }
      }
      // Thomas: sub-diagonal -w[i-1], diagonal 1 + w[i-1] + w[i],
      // super-diagonal -w[i].
      for (size_t n = 0; n < w; ++n) {
        const double diag = 1.0 + weight[n];
        cp[n] = -weight[n] / diag;
        dpa[n] = u0[n] / diag;
        dpb[n] = v0[n] / diag;
      }
      for (size_t i = 1; i < kCount; ++i) {
        for (size_t n = 0; n < w; ++n) {
          const size_t k = at(i, n);
          const double sub = -weight[k - w];
          const double sup = (i < kPairs) ? -weight[k] : 0.0;
          const double diag =
              1.0 + weight[k - w] + ((i < kPairs) ? weight[k] : 0.0);
          const double inverse = 1.0 / (diag - (sub * cp[k - w]));
          cp[k] = sup * inverse;
          dpa[k] = (u0[k] - (sub * dpa[k - w])) * inverse;
          dpb[k] = (v0[k] - (sub * dpb[k - w])) * inverse;
        }
      }
      const size_t last = (kCount - 1) * w;
      for (size_t n = 0; n < w; ++n) {
        a[last + n] = dpa[last + n];
        b[last + n] = dpb[last + n];
      }
      for (size_t i = kCount - 1; i-- > 0;) {
        for (size_t n = 0; n < w; ++n) {
          const size_t k = at(i, n);
          a[k] = dpa[k] - (cp[k] * a[k + w]);
          b[k] = dpb[k] - (cp[k] * b[k + w]);
        }
      }
    }
    for (size_t i = 0; i < kCount; ++i) {
      for (size_t n = 0; n < w; ++n) {
        u[source(i, n)] = a[at(i, n)];
        v[source(i, n)] = b[at(i, n)];
      }
    }
  }
}

}  // namespace

Domesday2DDecoder::Domesday2DDecoder(const Configuration& config)
    : config_(config) {
  // The configured matrix was measured at phase 0; the rows are
  // demodulated at chroma_phase. A matrix that cannot be undone, or a phase
  // near +/-90 degrees, leaves the rows uncorrected.
  const auto turned = d2d::in_phase_basis(config_.hanover, config_.chromaPhase);
  if (turned && d2d::invertible(*turned)) {
    hanover_ = *turned;
  } else {
    ORC_LOG_WARN(
        "Domesday2D: the Hanover matrix [{}, {}, {}, {}] at chroma phase {} "
        "cannot be undone; the rows are left uncorrected",
        config_.hanover[0], config_.hanover[1], config_.hanover[2],
        config_.hanover[3], config_.chromaPhase);
  }
  // Transform PAL's window: a raised cosine whose half-overlapped copies sum
  // to 1, so the inverse tiles add without an output window.
  for (int32_t y = 0; y < kTile; ++y) {
    const double wy = 0.5 - (0.5 * std::cos(2.0 * kPi * (y + 0.5) / kTile));
    for (int32_t x = 0; x < kTile; ++x) {
      const double wx = 0.5 - (0.5 * std::cos(2.0 * kPi * (x + 0.5) / kTile));
      window_[y][x] = wy * wx;
    }
  }
  // The detail band-pass as a convolution: the taps' spectrum, scaled for
  // FFTW's unnormalised inverse.
  detailReal_ = fftw_alloc_real(kDetailSize);
  detailComplex_ = fftw_alloc_complex(kDetailComplex);
  fftReal_ = fftw_alloc_real(static_cast<size_t>(kTile) * kTile);
  fftComplex_ = fftw_alloc_complex(static_cast<size_t>(kTile) * kComplexX);
  // FFTW_ESTIMATE throughout: a measured plan is chosen by timing, so two
  // runs could take different plans and differ in an occasional output
  // code. The estimated tile plans cost about 0.5 ms a frame more (Phase 8:
  // 4.9 ms against 4.4 for a frame's 2220 tiles).
  std::lock_guard<std::mutex> lock(TransformPal::fftwPlannerMutex());
  forwardPlan_ =
      fftw_plan_dft_r2c_2d(kTile, kTile, fftReal_, fftComplex_, FFTW_ESTIMATE);
  inversePlan_ =
      fftw_plan_dft_c2r_2d(kTile, kTile, fftComplex_, fftReal_, FFTW_ESTIMATE);
  detailForward_ =
      fftw_plan_dft_r2c_1d(static_cast<int>(kDetailSize), detailReal_,
                           detailComplex_, FFTW_ESTIMATE);
  detailInverse_ =
      fftw_plan_dft_c2r_1d(static_cast<int>(kDetailSize), detailComplex_,
                           detailReal_, FFTW_ESTIMATE);
  const double scale = 1.0 / static_cast<double>(kDetailSize);
  const auto kernel = [&](double low_hz, double high_hz,
                          std::vector<std::complex<double>>& out) {
    const auto taps = band_taps(low_hz, high_hz);
    std::fill(detailReal_, detailReal_ + kDetailSize, 0.0);
    std::copy(taps.begin(), taps.end(), detailReal_);
    fftw_execute(detailForward_);
    out.resize(kDetailComplex);
    for (size_t k = 0; k < kDetailComplex; ++k) {
      out[k] = std::complex<double>(detailComplex_[k][0] * scale,
                                    detailComplex_[k][1] * scale);
    }
  };
  kernel(kDetailLowHz, kDetailHighHz, detailKernel_);
  kernel(kBelowChromaLowHz, kBelowChromaHighHz, belowChromaKernel_);
  detailSpectrum_.resize(kDetailComplex);
}

Domesday2DDecoder::~Domesday2DDecoder() {
  {
    std::lock_guard<std::mutex> lock(TransformPal::fftwPlannerMutex());
    fftw_destroy_plan(forwardPlan_);
    fftw_destroy_plan(inversePlan_);
    fftw_destroy_plan(detailForward_);
    fftw_destroy_plan(detailInverse_);
  }
  fftw_free(detailReal_);
  fftw_free(detailComplex_);
  fftw_free(fftReal_);
  fftw_free(fftComplex_);
}

bool Domesday2DDecoder::configure(
    const ::orc::SourceParameters& videoParameters) {
  configurationValid_ = false;
  // The lattice and the 312-line partner are 625-line PAL's.
  if (videoParameters.system != orc::VideoSystem::PAL) {
    ORC_LOG_ERROR("Domesday2D decodes 625-line PAL sources only");
    return false;
  }
  const auto safety = ::orc::chroma_sink::sanitize_video_parameters(
      videoParameters, ::orc::chroma_sink::DecoderVideoProfile::PalColour);
  if (!safety.warnings.empty()) {
    ORC_LOG_WARN(
        "Domesday2DDecoder::configure(): Adjusted unsafe video parameters: {}",
        ::orc::chroma_sink::join_issues(safety.warnings));
  }
  if (!safety.ok) {
    ORC_LOG_ERROR(
        "Domesday2DDecoder::configure(): Invalid video parameters: {}",
        ::orc::chroma_sink::join_issues(safety.errors));
    return false;
  }
  if (safety.params.frame_width_nominal < static_cast<int32_t>(kLine) ||
      safety.params.white_level <= safety.params.blanking_level) {
    ORC_LOG_ERROR("Domesday2DDecoder::configure(): not a 4fsc PAL frame");
    return false;
  }
  videoParameters_ = safety.params;
  configurationValid_ = true;
  return true;
}

void Domesday2DDecoder::filterTile() {
  // Every bin of 0.5-1.5 fsc (x 4-12) is tested as U, against its partner
  // under y -> 3H/4 - y, and as V, under y -> H/4 - y, x -> 16 - x; the
  // better-agreeing reflection decides, and the pair is kept whole when the
  // smaller magnitude squared is at least threshold^2 of the larger. A
  // carrier bin (its own partner) is always kept.
  //
  // On a noise-free picture with symmetric structure the two reflections
  // often agree exactly, and which one wins would then depend on the FFT's
  // rounding; within kTieTolerance U wins, and bins below kSilentBin count
  // as empty, so the decision does not depend on the FFT library.
  constexpr int32_t kFirstX = kTile / 8;  // 0.5 fsc
  constexpr int32_t kCarrierX = kTile / 4;
  constexpr int32_t kEndX = (kTile / 2) - kFirstX + 1;
  const double t2 = config_.transformThreshold * config_.transformThreshold;
  std::array<std::array<double, kComplexX>, kTile> magnitude{};
  std::array<std::array<uint8_t, kComplexX>, kTile> kept{};
  for (int32_t y = 0; y < kTile; ++y) {
    for (int32_t x = kFirstX; x < kEndX; ++x) {
      const fftw_complex& bin = fftComplex_[(y * kComplexX) + x];
      magnitude[y][x] = (bin[0] * bin[0]) + (bin[1] * bin[1]);
    }
  }
  // Written without branches: the decisions follow the picture, so a branch
  // on them is mispredicted about as often as not, and below SSE4.1 even a
  // select on doubles is a branch. So min/max, and flags combined as bytes.
  // A silent pair's agreement is 1: its ratio's divisor is floored so the
  // ratio stays finite, then max() lifts it to 1.
  const auto agreement = [&](int32_t y, int32_t xs, int32_t ry, int32_t xr) {
    const double a = magnitude[y][xs];
    const double b = magnitude[ry][xr];
    const double high = std::max(a, b);
    const double ratio = std::min(a, b) / std::max(high, kSilentBin);
    return std::max(ratio, static_cast<double>(high <= kSilentBin));
  };
  for (int32_t y = 0; y < kTile; ++y) {
    const int32_t ru = ((3 * kTile / 4) - y) & (kTile - 1);
    const int32_t rv = ((kTile / 4) - y) & (kTile - 1);
    for (int32_t xs = kFirstX; xs <= kCarrierX; ++xs) {
      const int32_t xr = (kTile / 2) - xs;
      // A carrier bin is its own partner under one reflection, and kept.
      const bool self_u = ru == y && xs == xr;
      const bool self_v = rv == y && xs == xr;
      const double agree_u = self_u ? 2.0 : agreement(y, xs, ru, xr);
      const double agree_v = self_v ? 2.0 : agreement(y, xs, rv, xr);
      const auto use_u =
          static_cast<uint8_t>(agree_u >= agree_v - kTieTolerance);
      const auto keep = static_cast<uint8_t>(
          (use_u & static_cast<uint8_t>(agree_u >= t2)) |
          ((use_u ^ 1U) & static_cast<uint8_t>(agree_v >= t2)));
      kept[y][xs] |= keep;
      kept[ru][xr] |= keep & use_u;
      kept[rv][xr] |= keep & (use_u ^ 1U);
    }
  }
  for (int32_t y = 0; y < kTile; ++y) {
    double* row = fftComplex_[static_cast<ptrdiff_t>(y) * kComplexX];
    std::fill(row, row + (2 * static_cast<ptrdiff_t>(kFirstX)), 0.0);
    for (int32_t x = kFirstX; x < kEndX; ++x) {
      // All ones or all zeros: the bin kept or cleared to +0.
      const uint64_t mask = 0U - static_cast<uint64_t>(kept[y][x]);
      for (int32_t part = 0; part < 2; ++part) {
        uint64_t bits = 0;
        std::memcpy(&bits, &row[(2 * x) + part], sizeof(bits));
        bits &= mask;
        std::memcpy(&row[(2 * x) + part], &bits, sizeof(bits));
      }
    }
    std::fill(row + (2 * static_cast<ptrdiff_t>(kEndX)),
              row + (2 * static_cast<ptrdiff_t>(kComplexX)), 0.0);
  }
}

void Domesday2DDecoder::detailBand(const double* x, double* high, double* low) {
  // The row with its end samples extended by the taps' reach (filter_row()'s
  // padding), convolved: band sample n is the full convolution's n + 2 reach.
  // That convolution has kLine + 4 reach samples, and the circular one folds
  // those from kDetailSize on back onto the start, short of sample 2 reach
  // while the padded row fits.
  constexpr size_t kReach = kBandTaps / 2;
  static_assert(kLine + (2 * kReach) <= kDetailSize,
                "the detail convolution must not wrap into the row");
  std::fill(detailReal_, detailReal_ + kReach, x[0]);
  std::copy(x, x + kLine, detailReal_ + kReach);
  std::fill(detailReal_ + kReach + kLine, detailReal_ + kLine + (2 * kReach),
            x[kLine - 1]);
  std::fill(detailReal_ + kLine + (2 * kReach), detailReal_ + kDetailSize, 0.0);
  fftw_execute(detailForward_);
  for (size_t k = 0; k < kDetailComplex; ++k) {
    detailSpectrum_[k] = {detailComplex_[k][0], detailComplex_[k][1]};
  }
  // Each band from the one spectrum: the inverse overwrites its input.
  const auto band = [&](const std::vector<std::complex<double>>& kernel,
                        double* y) {
    for (size_t k = 0; k < kDetailComplex; ++k) {
      const std::complex<double> product = detailSpectrum_[k] * kernel[k];
      detailComplex_[k][0] = product.real();
      detailComplex_[k][1] = product.imag();
    }
    fftw_execute(detailInverse_);
    std::copy(detailReal_ + (2 * kReach), detailReal_ + (2 * kReach) + kLine,
              y);
  };
  band(detailKernel_, high);
  band(belowChromaKernel_, low);
}

void Domesday2DDecoder::separate(const Plane& raster, Plane& chroma) {
  // Tiles cover the picture rows and samples, half-overlapped; the rows
  // outside the picture read as 0, as do samples past the line's end.
  chroma.assign(kRows * kLine, 0.0);
  const auto first_row = static_cast<int32_t>(kFirstActiveRow);
  const auto end_row = static_cast<int32_t>(kEndActiveRow);
  const double scale = 1.0 / (kTile * kTile);
  for (int32_t top = first_row - kHalfTile; top < end_row; top += kHalfTile) {
    for (int32_t left = kFirstSample - kHalfTile; left < kEndSample;
         left += kHalfTile) {
      // The tile's samples inside the line, and those it adds to the output.
      const int32_t width = std::min(kTile, static_cast<int32_t>(kLine) - left);
      const int32_t keep_from = std::max(0, kFirstSample - left);
      const int32_t keep_to = std::min(kTile, kEndSample - left);
      for (int32_t y = 0; y < kTile; ++y) {
        const int32_t row = top + y;
        double* tile = fftReal_ + (static_cast<ptrdiff_t>(y) * kTile);
        if (row < first_row || row >= end_row) {
          std::fill(tile, tile + kTile, 0.0);
          continue;
        }
        const double* in =
            raster.data() + (static_cast<size_t>(row) * kLine) + left;
        for (int32_t x = 0; x < width; ++x) tile[x] = in[x] * window_[y][x];
        std::fill(tile + width, tile + kTile, 0.0);
      }
      fftw_execute(forwardPlan_);
      filterTile();
      fftw_execute(inversePlan_);
      for (int32_t y = 0; y < kTile; ++y) {
        const int32_t row = top + y;
        if (row < first_row || row >= end_row) continue;
        const double* tile = fftReal_ + (static_cast<ptrdiff_t>(y) * kTile);
        double* out = chroma.data() + (static_cast<size_t>(row) * kLine) + left;
        for (int32_t x = keep_from; x < keep_to; ++x) out[x] += tile[x] * scale;
      }
    }
  }
}

d2d::Matrix Domesday2DDecoder::frameMatrix(const d2d::Lattice& lattice) {
  if (!config_.hanoverPerFrame) return hanover_;
  // The frame's own rotation, fitted alone with the configured symmetric
  // part held: a picture's colour stripes can pass the partner check and
  // pull the gains and the symmetric leak (Phase 7), and a frame of one hue
  // determines the rotation but not the whole matrix (Phase 8).
  work_.pairs.plus.clear();
  work_.pairs.minus.clear();
  d2d::add_flat_pairs(work_.u, work_.v, lattice, kPartnerTolerance,
                      work_.pairs);
  const auto theta =
      d2d::fit_rotation(work_.pairs, hanover_, kMinFramePairs, nullptr);
  if (!theta) return hanover_;
  const d2d::Matrix own = d2d::with_rotation(hanover_, *theta);
  return d2d::invertible(own) ? own : hanover_;
}

void Domesday2DDecoder::decodeRaster(const Plane& raster, Planes& out) {
  auto& w = work_;
  const auto lattice = d2d::fit_lattice(raster);
  if (!(lattice.burst_amplitude >= kBurstKillAmplitude)) {
    // No burst to decode colour against (a monochrome source): no U or V,
    // and what the transform would separate as chroma stays in the luma.
    out.y = raster;
    out.u.assign(kRows * kLine, 0.0);
    out.v.assign(kRows * kLine, 0.0);
    return;
  }
  separate(raster, w.chroma);
  const double phase = config_.chromaPhase;

  // w.residual and w.u are free until the fit and the demodulation: the
  // low band's and the chroma's sums across.
  luma_detail(
      raster, w.chroma,
      [this](const double* x, double* high, double* low) {
        detailBand(x, high, low);
      },
      w.luma, w.across, w.residual, w.u, w.high, w.detail, w.row_a, w.row_b);
  const double noise = blank_noise(raster, w.row_a);

  d2d::demodulate_filtered(w.chroma, lattice, phase, w.u, w.v);
  const d2d::Matrix matrix = frameMatrix(lattice);
  const bool correct = any_nonzero(matrix);
  if (correct) {
    d2d::apply_matrix(matrix, lattice, false, w.u, w.v);
  }
  masked_average(w.u, w.v, out.u, out.v, w.rows);
  vertical_fit(out.u, out.v, w.detail,
               kLambda0 + (kLambdaNoise * noise * noise));

  // The residual: the chroma the fit does not explain, formed against what
  // was sent (the correction put back).
  const Plane* sent_u = &out.u;
  const Plane* sent_v = &out.v;
  if (correct) {
    w.u = out.u;
    w.v = out.v;
    d2d::apply_matrix(matrix, lattice, true, w.u, w.v);
    sent_u = &w.u;
    sent_v = &w.v;
  }
  d2d::remodulate(*sent_u, *sent_v, lattice, phase, w.residual);
  // Row by row, the residual and its energy's window sums across.
  w.row_a.resize(2 * kLine);
  for (size_t row = 0; row < kRows; row += 2) {
    const size_t base = row * kLine;
    double* residual = w.residual.data() + base;
    for (size_t n = 0; n < 2 * kLine; ++n) {
      residual[n] = w.chroma[base + n] - residual[n];
      w.row_a[n] = residual[n] * residual[n];
    }
    window_sums<2>({w.row_a.data(), w.row_a.data() + kLine}, kEnvelopeSamples,
                   {w.across.data() + base, w.across.data() + base + kLine},
                   w.row_b);
  }
  out.y.resize(kRows * kLine);
  box_rows(w.across, kEnvelopeRows, kEnvelopeSamples, w.row_a,
           [&](size_t row, const double* sum, double scale) {
             const size_t base = row * kLine;
             for (size_t n = 0; n < kLine; ++n) {
               const size_t i = base + n;
               const double env =
                   std::sqrt(std::max(sum[n] * scale, kMinEnvelopeSquare));
               const double gain = std::max(0.0, 1.0 - (noise / env)) *
                                   w.high[i] / (w.high[i] + kDetailGate);
               out.y[i] = w.luma[i] + (gain * w.residual[i]);
             }
           });
}

void Domesday2DDecoder::decodeFrame(const SourceField& first,
                                    const SourceField& second,
                                    ComponentFrame& frame) {
  const auto& p = videoParameters_;
  frame.init(p);
  d2d::fill_raster(
      [&](size_t field, size_t line) {
        return (field == 0 ? first : second).getLine(line);
      },
      p.blanking_level, p.white_level, work_.raster);
  decodeRaster(work_.raster, work_.planes);
  const Planes& planes = work_.planes;

  const double excursion = p.white_level - p.blanking_level;
  const double chroma_scale = excursion * config_.chromaGain;
  for (int32_t offset = 0; offset < 2; ++offset) {
    const int32_t first_line = (p.first_active_frame_line + 1 - offset) / 2;
    const int32_t last_line = (p.last_active_frame_line + 1 - offset) / 2;
    for (int32_t field_line = first_line; field_line < last_line;
         ++field_line) {
      const int32_t row = (field_line * 2) + offset;
      if (row < 0 || row >= static_cast<int32_t>(kRows)) continue;
      const int32_t line = p.active_area_cropping_applied
                               ? row - p.first_active_frame_line
                               : row;
      double* out_y = frame.y(line);
      double* out_u = frame.u(line);
      double* out_v = frame.v(line);
      const size_t base = static_cast<size_t>(row) * kLine;
      const int32_t end =
          std::min(p.active_video_end, static_cast<int32_t>(kLine));
      for (int32_t i = p.active_video_start; i < end; ++i) {
        const int32_t index =
            p.active_area_cropping_applied ? i - p.active_video_start : i;
        const size_t k = base + static_cast<size_t>(i);
        out_y[index] = p.blanking_level + (planes.y[k] * excursion);
        out_u[index] = planes.u[k] * chroma_scale;
        out_v[index] = planes.v[k] * chroma_scale;
      }
    }
  }
}

void Domesday2DDecoder::decodeFrames(
    const std::vector<SourceField>& inputFields, int32_t startIndex,
    int32_t endIndex, std::vector<ComponentFrame>& componentFrames) {
  if (!configurationValid_) {
    ORC_LOG_ERROR(
        "Domesday2DDecoder::decodeFrames(): Decoder configuration is invalid");
    return;
  }
  if (!inputFields.empty() && inputFields[0].is_yc) {
    // A Y/C source has no composite chroma to separate; the video sink
    // decodes it with pal2d instead.
    ORC_LOG_ERROR("Domesday2DDecoder: Y/C sources are not supported");
    return;
  }
  for (int32_t i = startIndex, k = 0; i + 1 < endIndex; i += 2, ++k) {
    const SourceField& a = inputFields[i];
    const SourceField& b = inputFields[i + 1];
    decodeFrame(a.is_first_field ? a : b, a.is_first_field ? b : a,
                componentFrames[k]);
  }
}
