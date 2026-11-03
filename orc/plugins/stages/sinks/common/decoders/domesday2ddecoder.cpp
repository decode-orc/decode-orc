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

#include <orc/support/logging.h>

#include <algorithm>
#include <cmath>
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

// ── Phase 4's parameters (adaptive.py DEFAULTS, hanover.masked_average) ──

// The vertical fit: lambda = kLambda0 + kLambdaDetail x the luma detail.
constexpr double kLambda0 = 0.01;
constexpr double kLambdaDetail = 5.0;
constexpr double kCharbonnierEps = 0.002;
constexpr int kReweightings = 6;
// The luma detail: the 3.5-5 MHz band's local r.m.s. over 5 rows x 15
// samples.
constexpr double kDetailLowHz = 3.5e6;
constexpr double kDetailHighHz = 5.0e6;
constexpr size_t kDetailRows = 5;
constexpr size_t kDetailSamples = 15;
// The reassignment: soft shrinkage against the blanked lines' chroma-band
// noise (raster rows 12-31, field lines 7-16, samples 200-1099), its
// envelope over 3 rows x 9 samples, gated by d / (d + kDetailGate).
constexpr double kNoiseLowHz = 3.0e6;
constexpr double kNoiseHighHz = 5.9e6;
constexpr size_t kBlankFirstRow = 12;
constexpr size_t kBlankEndRow = 32;
constexpr size_t kBlankFirstSample = 200;
constexpr size_t kBlankEndSample = 1100;
constexpr size_t kEnvelopeRows = 3;
constexpr size_t kEnvelopeSamples = 9;
constexpr double kMinEnvelopeSquare = 1e-12;
constexpr double kDetailGate = 0.003;
// The masked averaging: rows agree within kMaskThreshold (vector
// difference, smoothed over kMaskSmoothing samples), the mask falling to 0
// over kMaskSoft beyond it.
constexpr double kMaskThreshold = 0.04;
constexpr double kMaskSoft = 0.01;
constexpr size_t kMaskSmoothing = 9;

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

// |taps| (symmetric) along each of rows [first, end) of |in|, the row's end
// samples extended.
void filter_rows(const Plane& in, const std::vector<double>& taps, size_t first,
                 size_t end, Plane& out) {
  const size_t reach = taps.size() / 2;
  std::vector<double> padded(kLine + (2 * reach));
  for (size_t row = first; row < end; ++row) {
    const double* x = in.data() + (row * kLine);
    std::fill(padded.begin(), padded.begin() + static_cast<ptrdiff_t>(reach),
              x[0]);
    std::copy(x, x + kLine, padded.begin() + static_cast<ptrdiff_t>(reach));
    std::fill(padded.end() - static_cast<ptrdiff_t>(reach), padded.end(),
              x[kLine - 1]);
    double* y = out.data() + (row * kLine);
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
}

// scipy.ndimage's 'reflect' boundary: d c b a | a b c d | d c b a.
size_t reflect(ptrdiff_t i, size_t n) {
  const auto size = static_cast<ptrdiff_t>(n);
  while (i < 0 || i >= size) {
    i = (i < 0) ? -i - 1 : (2 * size) - i - 1;
  }
  return static_cast<size_t>(i);
}

// The sums of |x| (|n| samples) over windows of |width| (odd) samples
// centred on each sample, the ends reflected, by a running sum.
void window_sums(const double* x, size_t n, size_t width, double* out,
                 std::vector<double>& padded) {
  const size_t half = width / 2;
  padded.resize(n + width - 1);
  for (size_t i = 0; i < padded.size(); ++i) {
    padded[i] =
        x[reflect(static_cast<ptrdiff_t>(i) - static_cast<ptrdiff_t>(half), n)];
  }
  double acc = 0.0;
  for (size_t i = 0; i < width; ++i) acc += padded[i];
  out[0] = acc;
  for (size_t i = 1; i < n; ++i) {
    acc += padded[i + width - 1] - padded[i - 1];
    out[i] = acc;
  }
}

// The mean of |in| over |rows| x |samples| around each sample (both odd),
// the raster's edges reflected.
void box_mean(const Plane& in, size_t rows, size_t samples, Plane& out,
              Plane& across) {
  across.resize(kRows * kLine);
  std::vector<double> padded;
  const double scale = 1.0 / static_cast<double>(samples * rows);
  for (size_t row = 0; row < kRows; ++row) {
    window_sums(in.data() + (row * kLine), kLine, samples,
                across.data() + (row * kLine), padded);
  }
  const auto half_y = static_cast<ptrdiff_t>(rows / 2);
  out.assign(kRows * kLine, 0.0);
  for (size_t row = 0; row < kRows; ++row) {
    double* y = out.data() + (row * kLine);
    for (ptrdiff_t d = -half_y; d <= half_y; ++d) {
      const double* x =
          across.data() +
          (reflect(static_cast<ptrdiff_t>(row) + d, kRows) * kLine);
      for (size_t n = 0; n < kLine; ++n) y[n] += x[n];
    }
    for (size_t n = 0; n < kLine; ++n) y[n] *= scale;
  }
}

bool any_nonzero(const d2d::Matrix& m) {
  return std::any_of(m.begin(), m.end(), [](double x) { return x != 0.0; });
}

// ── Phase 4's chroma path ────────────────────────────────────────────────

// The local r.m.s. of |luma|'s 3.5-5 MHz band, into |detail|.
void luma_detail(const Plane& luma, Plane& band, Plane& across, Plane& detail) {
  static const auto taps = band_taps(kDetailLowHz, kDetailHighHz);
  band.resize(kRows * kLine);
  filter_rows(luma, taps, 0, kRows, band);
  for (double& x : band) x *= x;
  box_mean(band, kDetailRows, kDetailSamples, detail, across);
  for (double& x : detail) x = std::sqrt(std::max(x, 0.0));
}

// The r.m.s. of the chroma band on the blanked lines.
double blank_noise(const Plane& raster, Plane& band) {
  static const auto taps = band_taps(kNoiseLowHz, kNoiseHighHz);
  band.resize(kRows * kLine);
  filter_rows(raster, taps, kBlankFirstRow, kBlankEndRow, band);
  double sum = 0.0;
  for (size_t row = kBlankFirstRow; row < kBlankEndRow; ++row) {
    for (size_t n = kBlankFirstSample; n < kBlankEndSample; ++n) {
      const double x = band[(row * kLine) + n];
      sum += x * x;
    }
  }
  return std::sqrt(sum /
                   static_cast<double>((kBlankEndRow - kBlankFirstRow) *
                                       (kBlankEndSample - kBlankFirstSample)));
}

// 1 at or below the threshold, 0 from kMaskSoft beyond it, the differences
// first smoothed along the row.
void ramp(Plane& difference, Plane& smoothed) {
  smoothed.resize(kRows * kLine);
  std::vector<double> padded;
  const double scale = 1.0 / static_cast<double>(kMaskSmoothing);
  for (size_t row = 0; row < kRows; ++row) {
    double* y = smoothed.data() + (row * kLine);
    window_sums(difference.data() + (row * kLine), kLine, kMaskSmoothing, y,
                padded);
    for (size_t n = 0; n < kLine; ++n) {
      const double mean = y[n] * scale;
      y[n] = std::clamp(1.0 - ((mean - kMaskThreshold) / kMaskSoft), 0.0, 1.0);
    }
  }
  difference.swap(smoothed);
}

// Partner-checked masked field-line averaging (hanover.masked_average()):
// rows r -/+ 2 are the field lines either side, wrapping at the raster's
// ends; the 312-line partner is the row above a field 1 (even) row and below
// a field 2 row.
void masked_average(Plane& u, Plane& v, Plane& dev_u, Plane& dev_v, Plane& step,
                    Plane& partner, Plane& smoothed) {
  const auto at = [](ptrdiff_t row) {
    const auto rows = static_cast<ptrdiff_t>(kRows);
    return static_cast<size_t>(((row % rows) + rows) % rows) * kLine;
  };
  dev_u.resize(kRows * kLine);
  dev_v.resize(kRows * kLine);
  step.resize(kRows * kLine);
  for (size_t row = 0; row < kRows; ++row) {
    const auto r = static_cast<ptrdiff_t>(row);
    const size_t above = at(r - 2);
    const size_t below = at(r + 2);
    const size_t here = row * kLine;
    for (size_t n = 0; n < kLine; ++n) {
      dev_u[here + n] = u[here + n] - ((0.5 * u[here + n]) +
                                       (0.25 * (u[above + n] + u[below + n])));
      dev_v[here + n] = v[here + n] - ((0.5 * v[here + n]) +
                                       (0.25 * (v[above + n] + v[below + n])));
      const double du = u[above + n] - u[below + n];
      const double dv = v[above + n] - v[below + n];
      step[here + n] = std::sqrt((du * du) + (dv * dv));
    }
  }
  partner.resize(kRows * kLine);
  for (size_t row = 0; row < kRows; ++row) {
    const auto r = static_cast<ptrdiff_t>(row);
    const size_t other = at(row % 2 == 0 ? r - 1 : r + 1);
    const size_t here = row * kLine;
    for (size_t n = 0; n < kLine; ++n) {
      const double du = dev_u[here + n] - dev_u[other + n];
      const double dv = dev_v[here + n] - dev_v[other + n];
      partner[here + n] = std::sqrt((du * du) + (dv * dv));
    }
  }
  ramp(step, smoothed);
  ramp(partner, smoothed);
  for (size_t i = 0; i < kRows * kLine; ++i) {
    const double m = step[i] * partner[i];
    u[i] -= m * dev_u[i];
    v[i] -= m * dev_v[i];
  }
}

// The vector-Charbonnier vertical fit over the picture rows, in place:
// six reweighted least-squares iterations, each one tridiagonal solve per
// column (rows on the outer loop, so each sweep runs along whole rows).
void vertical_fit(Plane& u, Plane& v, const Plane& detail) {
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
            0.5 * ((kLambda0 + (kLambdaDetail * detail[source(i, n)])) +
                   (kLambda0 + (kLambdaDetail * detail[source(i + 1, n)])));
      }
    }
    for (size_t i = 0; i < kCount; ++i) {
      for (size_t n = 0; n < w; ++n) {
        u0[at(i, n)] = u[source(i, n)];
        v0[at(i, n)] = v[source(i, n)];
      }
    }
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
  // Transform PAL's window: a raised cosine whose half-overlapped copies sum
  // to 1, so the inverse tiles add without an output window.
  for (int32_t y = 0; y < kTile; ++y) {
    const double wy = 0.5 - (0.5 * std::cos(2.0 * kPi * (y + 0.5) / kTile));
    for (int32_t x = 0; x < kTile; ++x) {
      const double wx = 0.5 - (0.5 * std::cos(2.0 * kPi * (x + 0.5) / kTile));
      window_[y][x] = wy * wx;
    }
  }
  fftReal_ = fftw_alloc_real(static_cast<size_t>(kTile) * kTile);
  fftComplex_ = fftw_alloc_complex(static_cast<size_t>(kTile) * kComplexX);
  std::lock_guard<std::mutex> lock(TransformPal::fftwPlannerMutex());
  forwardPlan_ =
      fftw_plan_dft_r2c_2d(kTile, kTile, fftReal_, fftComplex_, FFTW_MEASURE);
  inversePlan_ =
      fftw_plan_dft_c2r_2d(kTile, kTile, fftComplex_, fftReal_, FFTW_MEASURE);
}

Domesday2DDecoder::~Domesday2DDecoder() {
  {
    std::lock_guard<std::mutex> lock(TransformPal::fftwPlannerMutex());
    fftw_destroy_plan(forwardPlan_);
    fftw_destroy_plan(inversePlan_);
  }
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
  const double t2 = config_.transformThreshold * config_.transformThreshold;
  std::array<std::array<double, kComplexX>, kTile> magnitude{};
  std::array<std::array<bool, kComplexX>, kTile> kept{};
  for (int32_t y = 0; y < kTile; ++y) {
    for (int32_t x = kFirstX; x <= kTile / 2 - kFirstX; ++x) {
      const fftw_complex& bin = fftComplex_[(y * kComplexX) + x];
      magnitude[y][x] = (bin[0] * bin[0]) + (bin[1] * bin[1]);
    }
  }
  const auto agreement = [&](int32_t y, int32_t xs, int32_t ry, int32_t xr) {
    if (ry == y && xs == xr) return 2.0;
    const double a = magnitude[y][xs];
    const double b = magnitude[ry][xr];
    const double high = std::max(a, b);
    return high > kSilentBin ? std::min(a, b) / high : 1.0;
  };
  for (int32_t y = 0; y < kTile; ++y) {
    const int32_t ru = ((3 * kTile / 4) - y) & (kTile - 1);
    const int32_t rv = ((kTile / 4) - y) & (kTile - 1);
    for (int32_t xs = kFirstX; xs <= kCarrierX; ++xs) {
      const int32_t xr = (kTile / 2) - xs;
      const double agree_u = agreement(y, xs, ru, xr);
      const double agree_v = agreement(y, xs, rv, xr);
      const bool use_u = agree_u >= agree_v - kTieTolerance;
      if ((use_u ? agree_u : agree_v) < t2) continue;
      kept[y][xs] = true;
      kept[use_u ? ru : rv][xr] = true;
    }
  }
  for (int32_t y = 0; y < kTile; ++y) {
    for (int32_t x = 0; x < kComplexX; ++x) {
      if (!kept[y][x]) {
        fftComplex_[(y * kComplexX) + x][0] = 0.0;
        fftComplex_[(y * kComplexX) + x][1] = 0.0;
      }
    }
  }
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
      for (int32_t y = 0; y < kTile; ++y) {
        const int32_t row = top + y;
        const bool inside = row >= first_row && row < end_row;
        for (int32_t x = 0; x < kTile; ++x) {
          const int32_t n = left + x;
          const double value = (inside && n < static_cast<int32_t>(kLine))
                                   ? raster[(static_cast<size_t>(row) * kLine) +
                                            static_cast<size_t>(n)]
                                   : 0.0;
          fftReal_[(y * kTile) + x] = value * window_[y][x];
        }
      }
      fftw_execute(forwardPlan_);
      filterTile();
      fftw_execute(inversePlan_);
      for (int32_t y = 0; y < kTile; ++y) {
        const int32_t row = top + y;
        if (row < first_row || row >= end_row) continue;
        for (int32_t x = 0; x < kTile; ++x) {
          const int32_t n = left + x;
          if (n < kFirstSample || n >= kEndSample) continue;
          chroma[(static_cast<size_t>(row) * kLine) + static_cast<size_t>(n)] +=
              fftReal_[(y * kTile) + x] * scale;
        }
      }
    }
  }
}

void Domesday2DDecoder::decodeRaster(const Plane& raster, Planes& out) {
  auto& w = work_;
  separate(raster, w.chroma);
  const auto lattice = d2d::fit_lattice(raster);
  const double phase = config_.chromaPhase;
  const bool correct = any_nonzero(config_.hanover);

  w.luma.resize(kRows * kLine);
  for (size_t i = 0; i < w.luma.size(); ++i) {
    w.luma[i] = raster[i] - w.chroma[i];
  }
  luma_detail(w.luma, w.band, w.across, w.detail);

  d2d::demodulate(w.chroma, lattice, phase, out.u, out.v);
  d2d::filter_rows_horizontal(out.u);
  d2d::filter_rows_horizontal(out.v);
  if (correct) {
    d2d::apply_matrix(config_.hanover, lattice, false, out.u, out.v);
  }
  masked_average(out.u, out.v, w.dev_u, w.dev_v, w.step, w.partner, w.smoothed);
  vertical_fit(out.u, out.v, w.detail);

  // The residual: the chroma the fit does not explain, formed against what
  // was sent (the correction put back).
  w.sent_u = out.u;
  w.sent_v = out.v;
  if (correct) {
    d2d::apply_matrix(config_.hanover, lattice, true, w.sent_u, w.sent_v);
  }
  d2d::remodulate(w.sent_u, w.sent_v, lattice, phase, w.encoded);
  // The residual's energy goes through the band plane, free by now.
  w.residual.resize(kRows * kLine);
  w.band.resize(kRows * kLine);
  for (size_t i = 0; i < w.residual.size(); ++i) {
    w.residual[i] = w.chroma[i] - w.encoded[i];
    w.band[i] = w.residual[i] * w.residual[i];
  }
  box_mean(w.band, kEnvelopeRows, kEnvelopeSamples, w.envelope, w.across);
  const double noise = blank_noise(raster, w.band);
  out.y.resize(kRows * kLine);
  for (size_t i = 0; i < out.y.size(); ++i) {
    const double env = std::sqrt(std::max(w.envelope[i], kMinEnvelopeSquare));
    const double gain = std::max(0.0, 1.0 - (noise / env)) * w.detail[i] /
                        (w.detail[i] + kDetailGate);
    out.y[i] = w.luma[i] + (gain * w.residual[i]);
  }
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
