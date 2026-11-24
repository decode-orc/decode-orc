/*
 * File:        channel_calibration.cpp
 * Module:      orc-stage-plugin-channel-equalise
 * Purpose:     Measures a PAL LaserVision source's edge smear from the VITS
 *              on field 1 line 19, for Channel Equalise
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "channel_calibration.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/channel_equalise.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <functional>
#include <limits>
#include <utility>

#include "picture_edges.h"
#include "window_select.h"

namespace orc {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kRate = 4.0 * 4433618.75;
constexpr double kSamplesPerUs = kRate / 1.0e6;

// Field 1 line 19, frame-flat.
constexpr size_t kVitsLine = 18;

// The averaged segment: 3 us before the bar edge to 17 us after it.
constexpr double kSegmentStartUs = -3.0;
constexpr double kSegmentStopUs = 17.0;
// Samples either side of the segment carried through the fractional shift.
constexpr size_t kShiftPad = 64;

// The bar edge: the 50 % crossing of the 2 MHz low-passed line, with the
// line under 15 IRE from 1 us to 0.5 us before it and over 85 IRE from 0.5
// us to 1 us after it.
constexpr double kEdgeLevel = 50.0;
constexpr double kEdgeLow = 15.0;
constexpr double kEdgeHigh = 85.0;
constexpr size_t kLowpassTaps = 81;
constexpr double kLowpassHz = 2.0e6;

// The guards: a fit is a smear only when clear of the parameter
// bounds and long enough not to be the generator's edge shaping, and when
// it explains the tails clearly better than no smear.
constexpr double kMinResidualGain = 0.8;

// The search: the fraction and time constant bounds of the fit.
constexpr double kFitMaxFraction = 0.5;
constexpr double kFitMinTau = 0.5;
constexpr double kFitMaxTau = 100.0;

// ── Filters ────────────────────────────────────────────────────────────────

// scipy.signal.firwin(81, 2 MHz): a Hamming-windowed sinc of unit DC gain.
const std::vector<double>& lowpass_taps() {
  static const std::vector<double> taps = [] {
    std::vector<double> h(kLowpassTaps);
    const double fc = kLowpassHz / (kRate / 2.0);
    const double mid = static_cast<double>(kLowpassTaps - 1) / 2.0;
    double sum = 0.0;
    for (size_t n = 0; n < kLowpassTaps; ++n) {
      const double m = static_cast<double>(n) - mid;
      const double x = kPi * fc * m;
      const double sinc = m == 0.0 ? 1.0 : std::sin(x) / x;
      const double window =
          0.54 - (0.46 * std::cos(2.0 * kPi * static_cast<double>(n) /
                                  static_cast<double>(kLowpassTaps - 1)));
      h[n] = fc * sinc * window;
      sum += h[n];
    }
    for (double& v : h) v /= sum;
    return h;
  }();
  return taps;
}

// One FIR pass, the history before the first sample held at its level.
std::vector<double> fir_pass(const std::vector<double>& x,
                             const std::vector<double>& h) {
  std::vector<double> y(x.size());
  for (size_t n = 0; n < x.size(); ++n) {
    double acc = 0.0;
    for (size_t k = 0; k < h.size(); ++k) {
      acc += h[k] * (n >= k ? x[n - k] : x.front());
    }
    y[n] = acc;
  }
  return y;
}

// scipy.signal.filtfilt(h, 1, x): odd extension by 3 filter lengths,
// forward and backward passes with steady-state initial conditions.
std::vector<double> filtfilt(const std::vector<double>& x,
                             const std::vector<double>& h) {
  const size_t pad = 3 * h.size();
  if (x.size() <= pad) return x;
  std::vector<double> ext;
  ext.reserve(x.size() + (2 * pad));
  for (size_t k = pad; k >= 1; --k) ext.push_back((2.0 * x.front()) - x[k]);
  ext.insert(ext.end(), x.begin(), x.end());
  const size_t last = x.size() - 1;
  for (size_t k = 1; k <= pad; ++k) {
    ext.push_back((2.0 * x.back()) - x[last - k]);
  }
  auto y = fir_pass(ext, h);
  std::reverse(y.begin(), y.end());
  y = fir_pass(y, h);
  std::reverse(y.begin(), y.end());
  return {y.begin() + static_cast<std::ptrdiff_t>(pad),
          y.begin() + static_cast<std::ptrdiff_t>(pad + x.size())};
}

// The bar's 50 % rising edge on a line in IRE, to a fraction of a sample.
std::optional<double> bar_edge(const std::vector<double>& line) {
  const auto smooth = filtfilt(line, lowpass_taps());
  const auto width = static_cast<size_t>(kSamplesPerUs);
  const size_t half = width / 2;
  for (size_t n = width; n + width < line.size(); ++n) {
    if (!(smooth[n - 1] < kEdgeLevel && kEdgeLevel <= smooth[n])) continue;
    const double before = *std::max_element(
        smooth.begin() + static_cast<std::ptrdiff_t>(n - width),
        smooth.begin() + static_cast<std::ptrdiff_t>(n - half));
    const double after = *std::min_element(
        smooth.begin() + static_cast<std::ptrdiff_t>(n + half),
        smooth.begin() + static_cast<std::ptrdiff_t>(n + width));
    if (before < kEdgeLow && after > kEdgeHigh) {
      const double a = smooth[n - 1];
      const double b = smooth[n];
      return static_cast<double>(n - 1) + ((kEdgeLevel - a) / (b - a));
    }
  }
  return std::nullopt;
}

// |x| advanced by |frac| samples, circularly, by a phase ramp on its DFT
// (numpy's irfft(rfft(x) * exp(2 pi j k frac))). The DFT's twiddles are
// tabulated once per call: every field of a source is shifted when the smear
// is tracked along it.
std::vector<double> fractional_advance(const std::vector<double>& x,
                                       double frac) {
  const size_t n = x.size();
  const size_t bins = (n / 2) + 1;
  std::vector<double> cosine(n);
  std::vector<double> sine(n);
  for (size_t m = 0; m < n; ++m) {
    const double angle =
        2.0 * kPi * static_cast<double>(m) / static_cast<double>(n);
    cosine[m] = std::cos(angle);
    sine[m] = std::sin(angle);
  }
  std::vector<std::complex<double>> spectrum(bins);
  for (size_t k = 0; k < bins; ++k) {
    double re = 0.0;
    double im = 0.0;
    size_t m = 0;
    for (size_t t = 0; t < n; ++t) {
      re += x[t] * cosine[m];
      im -= x[t] * sine[m];
      m += k;
      if (m >= n) m -= n;
    }
    const double shift =
        2.0 * kPi * static_cast<double>(k) * frac / static_cast<double>(n);
    spectrum[k] = std::complex<double>(re, im) *
                  std::complex<double>(std::cos(shift), std::sin(shift));
  }
  // An even length's Nyquist bin is real in irfft.
  if (n % 2 == 0) spectrum[bins - 1] = spectrum[bins - 1].real();
  std::vector<double> y(n);
  for (size_t t = 0; t < n; ++t) {
    double acc = spectrum[0].real();
    size_t m = t;
    for (size_t k = 1; k < bins; ++k) {
      const double term =
          (spectrum[k].real() * cosine[m]) - (spectrum[k].imag() * sine[m]);
      acc += (n % 2 == 0 && k == bins - 1) ? term : 2.0 * term;
      m += t;
      if (m >= n) m -= n;
    }
    y[t] = acc / static_cast<double>(n);
  }
  return y;
}

// ── The fit ────────────────────────────────────────────────────────────────

// videosynth's edge_smear on a signal: (1 - a) x + a onepole(x), the
// one-pole primed to the first sample.
std::vector<double> smear(const std::vector<double>& x, double fraction,
                          double tau_samples) {
  const double pole = std::exp(-1.0 / tau_samples);
  std::vector<double> y(x.size());
  double acc = x.front();
  for (size_t i = 0; i < x.size(); ++i) {
    acc = (pole * acc) + ((1.0 - pole) * x[i]);
    y[i] = ((1.0 - fraction) * x[i]) + (fraction * acc);
  }
  return y;
}

// A coefficient's bounds in the linear part of the fit.
struct Bounds {
  double lower = -std::numeric_limits<double>::infinity();
  double upper = std::numeric_limits<double>::infinity();
};

// Least squares of |target| on the columns of |basis| (at most three) over
// the samples |use|, each coefficient within its |bounds|: the RMS residual.
// The problem is a small convex quadratic, so every assignment of the
// coefficients to free, at the lower bound or at the upper bound is solved
// (free ones by the normal equations) and the best feasible one kept.
double linear_residual(const std::vector<std::vector<double>>& basis,
                       const std::vector<double>& target,
                       const std::vector<size_t>& use,
                       const std::vector<Bounds>& bounds) {
  const size_t m = basis.size();
  std::array<std::array<double, 3>, 3> gram{};
  std::array<double, 3> cross{};
  double energy = 0.0;
  for (size_t i = 0; i < m; ++i) {
    for (size_t j = 0; j < m; ++j) {
      double s = 0.0;
      for (const size_t n : use) s += basis[i][n] * basis[j][n];
      gram[i][j] = s;
    }
    double s = 0.0;
    for (const size_t n : use) s += basis[i][n] * target[n];
    cross[i] = s;
  }
  for (const size_t n : use) energy += target[n] * target[n];

  size_t assignments = 1;
  for (size_t i = 0; i < m; ++i) assignments *= 3;
  double best = std::numeric_limits<double>::infinity();
  for (size_t code = 0; code < assignments; ++code) {
    // 0 free, 1 at the lower bound, 2 at the upper bound.
    std::array<int, 3> state{};
    std::array<double, 3> coef{};
    bool valid = true;
    for (size_t i = 0, c = code; i < m; ++i, c /= 3) {
      state[i] = static_cast<int>(c % 3);
      if (state[i] == 1) coef[i] = bounds[i].lower;
      if (state[i] == 2) coef[i] = bounds[i].upper;
      if (state[i] != 0 && !std::isfinite(coef[i])) valid = false;
    }
    if (!valid) continue;
    // The free coefficients' normal equations, the fixed ones moved to the
    // right-hand side.
    std::array<size_t, 3> free{};
    size_t k = 0;
    for (size_t i = 0; i < m; ++i) {
      if (state[i] == 0) free[k++] = i;
    }
    std::array<std::array<double, 4>, 3> a{};
    for (size_t r = 0; r < k; ++r) {
      for (size_t c = 0; c < k; ++c) a[r][c] = gram[free[r]][free[c]];
      double rhs = cross[free[r]];
      for (size_t i = 0; i < m; ++i) {
        if (state[i] != 0) rhs -= gram[free[r]][i] * coef[i];
      }
      a[r][k] = rhs;
    }
    for (size_t c = 0; c < k && valid; ++c) {
      size_t pivot = c;
      for (size_t r = c + 1; r < k; ++r) {
        if (std::abs(a[r][c]) > std::abs(a[pivot][c])) pivot = r;
      }
      std::swap(a[c], a[pivot]);
      if (std::abs(a[c][c]) < 1e-300) {
        valid = false;
        break;
      }
      for (size_t r = c + 1; r < k; ++r) {
        const double f = a[r][c] / a[c][c];
        for (size_t j = c; j <= k; ++j) a[r][j] -= f * a[c][j];
      }
    }
    if (!valid) continue;
    for (size_t c = k; c-- > 0;) {
      double s = a[c][k];
      for (size_t j = c + 1; j < k; ++j) s -= a[c][j] * coef[free[j]];
      coef[free[c]] = s / a[c][c];
      if (coef[free[c]] < bounds[free[c]].lower ||
          coef[free[c]] > bounds[free[c]].upper) {
        valid = false;
      }
    }
    if (!valid) continue;
    // |B c - y|^2 = c' G c - 2 c' B'y + y'y.
    double sum = energy;
    for (size_t i = 0; i < m; ++i) {
      sum -= 2.0 * coef[i] * cross[i];
      for (size_t j = 0; j < m; ++j) sum += coef[i] * gram[i][j] * coef[j];
    }
    best = std::min(best, std::max(sum, 0.0));
  }
  return std::sqrt(best / static_cast<double>(use.size()));
}

}  // namespace

SmearVerdict judge_smear_fit(const std::optional<SmearFit>& fit) {
  if (!fit) return SmearVerdict::CannotTell;
  const bool explained =
      fit->residual_ire < kMinResidualGain * fit->residual_no_smear_ire;
  if (fit->fraction >= channel_equalise::kMaxFraction ||
      fit->tau_samples < channel_equalise::kMinTauSamples) {
    return explained ? SmearVerdict::CannotTell : SmearVerdict::NoSmear;
  }
  // The fit searches past the stage's limit, so a long smear is told apart
  // from none rather than clipped to the limit; it is still not applied.
  if (explained && fit->tau_samples > channel_equalise::kMaxTauSamples) {
    return SmearVerdict::OutOfRange;
  }
  return explained ? SmearVerdict::Smear : SmearVerdict::NoSmear;
}

std::optional<SmearFit> fit_smear_segment(const std::vector<double>& segment,
                                          double start_us) {
  const size_t n = segment.size();
  if (n < 16) return std::nullopt;
  std::vector<double> t(n);
  for (size_t i = 0; i < n; ++i) {
    t[i] = start_us + (static_cast<double>(i) / kSamplesPerUs);
  }
  // numpy.convolve(avg, ones(5) / 5, 'same').
  std::vector<double> smooth(n);
  for (size_t i = 0; i < n; ++i) {
    double s = 0.0;
    for (int d = -2; d <= 2; ++d) {
      const auto k = static_cast<std::ptrdiff_t>(i) + d;
      if (k >= 0 && k < static_cast<std::ptrdiff_t>(n)) {
        s += segment[static_cast<size_t>(k)];
      }
    }
    smooth[i] = s / 5.0;
  }
  const double rise = 0.0;
  std::optional<double> fall;
  for (size_t i = 0; i < n; ++i) {
    if (t[i] > 5.0 && smooth[i] < kEdgeLevel) {
      fall = t[i];
      break;
    }
  }
  if (!fall) return std::nullopt;
  std::optional<size_t> pulse_index;
  for (size_t i = 0; i < n; ++i) {
    if (t[i] > *fall + 2.0 && t[i] < *fall + 6.0 &&
        (!pulse_index || smooth[i] > smooth[*pulse_index])) {
      pulse_index = i;
    }
  }
  if (!pulse_index) return std::nullopt;
  const double pulse = t[*pulse_index];

  constexpr double kGuardUs = 0.4;
  std::vector<size_t> use;
  for (size_t i = 0; i < n; ++i) {
    const double x = t[i];
    if ((x > -2.5 && x < rise - 0.2) ||
        (x > rise + kGuardUs && x < *fall - 0.2) ||
        (x > *fall + kGuardUs && x < pulse - 0.3) ||
        (x > pulse + kGuardUs && x < pulse + 3.0)) {
      use.push_back(i);
    }
  }
  if (use.size() < 8) return std::nullopt;

  // The ideal: the bar as a rectangle, the pulse as an impulse at the
  // sample nearest its peak, and a base level. The smear is linear and
  // keeps a constant, so for a given (fraction, tau) the model is linear in
  // the bar's height, the pulse's area and the base, which are solved for
  // exactly; only (fraction, tau) are searched.
  std::vector<double> bar(n, 0.0);
  std::vector<double> impulse(n, 0.0);
  const std::vector<double> ones(n, 1.0);
  for (size_t i = 0; i < n; ++i) {
    if (t[i] >= rise && t[i] < *fall) bar[i] = 1.0;
  }
  size_t nearest = 0;
  for (size_t i = 1; i < n; ++i) {
    if (std::abs(t[i] - pulse) < std::abs(t[nearest] - pulse)) nearest = i;
  }
  impulse[nearest] = 1.0;

  // Bounds on the bar's height (IRE), the pulse's area (IRE us) and the
  // base (IRE). The area's bound is below a 2T pulse's own and holds on
  // every capture measured: the fitted tails, and the Domesday CAV figures
  // (fraction 0.24, tau 6.1 samples), are those of the bounded fit.
  const std::vector<Bounds> bounds = {{50.0, 150.0}, {0.0, 2.0}, {-10.0, 10.0}};
  for (double& v : impulse) v *= kSamplesPerUs;
  const auto residual = [&](double fraction, double tau) {
    return linear_residual(
        {smear(bar, fraction, tau), smear(impulse, fraction, tau), ones},
        segment, use, bounds);
  };

  SmearFit fit;
  fit.residual_no_smear_ire =
      linear_residual({bar, ones}, segment, use, {Bounds{}, Bounds{}});

  // A grid over the bounds, then a pattern search from its best.
  constexpr int kFractionSteps = 50;
  constexpr int kTauSteps = 60;
  const double log_min = std::log(kFitMinTau);
  const double log_max = std::log(kFitMaxTau);
  double best_fraction = 0.0;
  double best_log_tau = std::log(8.0);
  double best = residual(best_fraction, std::exp(best_log_tau));
  for (int i = 0; i <= kFractionSteps; ++i) {
    const double fraction = kFitMaxFraction * i / kFractionSteps;
    for (int j = 0; j <= kTauSteps; ++j) {
      const double log_tau = log_min + ((log_max - log_min) * j / kTauSteps);
      const double r = residual(fraction, std::exp(log_tau));
      if (r < best) {
        best = r;
        best_fraction = fraction;
        best_log_tau = log_tau;
      }
    }
  }
  double step_fraction = kFitMaxFraction / kFractionSteps;
  double step_log_tau = (log_max - log_min) / kTauSteps;
  while (step_fraction > 1e-6) {
    bool moved = false;
    for (const auto& [df, dt] :
         std::array<std::pair<double, double>, 4>{{{step_fraction, 0.0},
                                                   {-step_fraction, 0.0},
                                                   {0.0, step_log_tau},
                                                   {0.0, -step_log_tau}}}) {
      const double fraction =
          std::clamp(best_fraction + df, 0.0, kFitMaxFraction);
      const double log_tau = std::clamp(best_log_tau + dt, log_min, log_max);
      const double r = residual(fraction, std::exp(log_tau));
      if (r < best) {
        best = r;
        best_fraction = fraction;
        best_log_tau = log_tau;
        moved = true;
      }
    }
    if (!moved) {
      step_fraction /= 2.0;
      step_log_tau /= 2.0;
    }
  }
  fit.fraction = best_fraction;
  fit.tau_samples = std::exp(best_log_tau);
  fit.residual_ire = best;
  return fit;
}

namespace {

size_t segment_length() {
  return static_cast<size_t>(
      std::lround((kSegmentStopUs - kSegmentStartUs) * kSamplesPerUs));
}

// A line of the source in IRE, or empty.
std::vector<double> line_ire(const VideoFrameRepresentation& source, FrameID id,
                             size_t line, double blanking, double scale) {
  const auto desc = source.get_frame_descriptor(id);
  if (!desc) return {};
  const size_t width = desc->samples_per_line_nominal;
  const int16_t* raw = source.get_line(id, line);
  if (!raw) return {};
  std::vector<double> out(width);
  for (size_t i = 0; i < width; ++i) out[i] = (raw[i] - blanking) * scale;
  return out;
}

}  // namespace

std::optional<std::vector<double>> aligned_bar_segment(
    const std::vector<double>& line_ire) {
  const size_t length = segment_length();
  const auto edge = bar_edge(line_ire);
  if (!edge) return std::nullopt;  // a dropout on the bar, or no bar
  const double start = *edge + (kSegmentStartUs * kSamplesPerUs);
  const double whole = std::floor(start);
  const auto first = static_cast<std::ptrdiff_t>(whole) -
                     static_cast<std::ptrdiff_t>(kShiftPad);
  const auto end =
      first + static_cast<std::ptrdiff_t>(length + kShiftPad + kShiftPad);
  if (first < 0 || end > static_cast<std::ptrdiff_t>(line_ire.size())) {
    return std::nullopt;
  }
  const auto shifted = fractional_advance(
      {line_ire.begin() + first, line_ire.begin() + end}, start - whole);
  return std::vector<double>(
      shifted.begin() + static_cast<std::ptrdiff_t>(kShiftPad),
      shifted.begin() + static_cast<std::ptrdiff_t>(kShiftPad + length));
}

std::vector<double> aligned_line19_average(
    const VideoFrameRepresentation& source, const std::vector<FrameID>& frames,
    size_t* frames_used) {
  const auto params = source.get_video_parameters();
  std::vector<double> sum(segment_length(), 0.0);
  size_t used = 0;
  if (params && params->white_level > params->blanking_level) {
    const double blanking = params->blanking_level;
    const double scale = 100.0 / (params->white_level - params->blanking_level);
    for (const FrameID id : frames) {
      const auto line = line_ire(source, id, kVitsLine, blanking, scale);
      if (line.empty()) continue;
      const auto segment = aligned_bar_segment(line);
      if (!segment) continue;
      for (size_t i = 0; i < sum.size(); ++i) sum[i] += (*segment)[i];
      ++used;
    }
  }
  if (frames_used) *frames_used = used;
  if (used == 0) return {};
  for (double& v : sum) v /= static_cast<double>(used);
  return sum;
}

ChannelCalibration calibrate_channel(
    const VideoFrameRepresentation& source,
    const ChannelCalibrationOptions& options,
    const ChannelCalibrationProgress& progress) {
  ChannelCalibration result;
  const auto params = source.get_video_parameters();
  if (!params || params->system != VideoSystem::PAL ||
      source.has_separate_channels()) {
    return result;
  }
  auto report = [&](int percent, const std::string& status) {
    return !progress || progress(percent, status);
  };

  // The windows: inner, middle and outer radius, or the whole source when it
  // is too short for three.
  const size_t count = source.frame_count();
  const FrameID origin = source.frame_range().first;
  const size_t length = std::min(options.window_frames, count);
  std::vector<size_t> starts;
  if (length == 0) return result;
  if (count < 3 * length) {
    starts = {0};
  } else {
    const size_t margin = std::min(options.edge_margin_frames, count / 10);
    const size_t last = count - length;
    starts = {std::min(margin, last), std::min(count / 2, last),
              std::min(count - margin, last)};
  }

  std::vector<ChannelWindow> windows;
  for (size_t w = 0; w < starts.size(); ++w) {
    if (!report(static_cast<int>(100 * w / starts.size()),
                "Fitting the VITS")) {
      return {};
    }
    ChannelWindow window;
    window.first = origin + starts[w];
    window.frames = (starts.size() == 1) ? count : length;
    std::vector<FrameID> frames(window.frames);
    for (size_t k = 0; k < window.frames; ++k) frames[k] = window.first + k;
    const auto average =
        aligned_line19_average(source, frames, &window.frames_used);
    if (!average.empty()) {
      window.fit = fit_smear_segment(average, kSegmentStartUs);
    }
    window.verdict = judge_smear_fit(window.fit);
    windows.push_back(window);
  }
  report(100, "Done");

  result = combine_channel_windows(std::move(windows));
  ORC_LOG_DEBUG("Channel calibration: {} windows, smear {:.3f} / {:.2f}",
                result.windows.size(), result.fraction, result.tau_samples);
  return result;
}

ChannelCalibration combine_channel_windows(std::vector<ChannelWindow> windows) {
  ChannelCalibration result;
  std::vector<double> fractions;
  std::vector<double> taus;
  bool any_out_of_range = false;
  bool any_no_smear = false;
  for (const auto& window : windows) {
    if (window.verdict == SmearVerdict::Smear && window.fit) {
      fractions.push_back(window.fit->fraction);
      taus.push_back(window.fit->tau_samples);
    } else if (window.verdict == SmearVerdict::OutOfRange) {
      any_out_of_range = true;
    } else if (window.verdict == SmearVerdict::NoSmear) {
      any_no_smear = true;
    }
  }
  result.windows = std::move(windows);

  const auto median = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    const size_t mid = v.size() / 2;
    return (v.size() % 2 == 1) ? v[mid] : (v[mid - 1] + v[mid]) / 2.0;
  };
  if (!fractions.empty()) {
    result.verdict = SmearVerdict::Smear;
    result.fraction = median(fractions);
    result.tau_samples = median(taus);
  } else if (any_out_of_range) {
    result.verdict = SmearVerdict::OutOfRange;
  } else if (any_no_smear) {
    result.verdict = SmearVerdict::NoSmear;
  }
  return result;
}

// ── Tracking ───────────────────────────────────────────────────────────────

namespace {

// Field 2's line 19, frame-flat.
constexpr size_t kPalFieldLines = 313;
constexpr size_t kVitsLineField2 = kPalFieldLines + kVitsLine;
// A source shorter than this many times the picture stride has its picture
// read proportionally more often (every frame below this many frames).
constexpr size_t kPictureFramesWanted = 256;
// A knot without a slow component still needs a valid time constant.
constexpr double kMinTauSamplesKnot = channel_equalise::kMinTauSamples;

// The tail reading's window: 3 to 8 samples after the rising edge, against
// the bar top from 20 samples after it and the level before it.
constexpr size_t kTailFirst = 3;
constexpr size_t kTailLast = 8;
constexpr size_t kTopFirst = 20;
constexpr size_t kTopLength = 130;
constexpr size_t kBaseGuard = 12;

double median_of(std::vector<double> v) {
  if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
  const size_t mid = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid),
                   v.end());
  const double upper = v[mid];
  if (v.size() % 2 == 1) return upper;
  const double lower = *std::max_element(
      v.begin(), v.begin() + static_cast<std::ptrdiff_t>(mid));
  return (lower + upper) / 2.0;
}

}  // namespace

double segment_tail(const std::vector<double>& segment) {
  const auto edge =
      static_cast<size_t>(std::lround(-kSegmentStartUs * kSamplesPerUs));
  if (segment.size() < edge + kTopFirst + kTopLength || edge <= kBaseGuard) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double base = median_of(
      {segment.begin(),
       segment.begin() + static_cast<std::ptrdiff_t>(edge - kBaseGuard)});
  const auto top_first =
      segment.begin() + static_cast<std::ptrdiff_t>(edge + kTopFirst);
  const double top = median_of(
      {top_first, top_first + static_cast<std::ptrdiff_t>(kTopLength)});
  const double height = top - base;
  if (!(height > 0.0)) return std::numeric_limits<double>::quiet_NaN();
  double sum = 0.0;
  for (size_t i = edge + kTailFirst; i <= edge + kTailLast; ++i) {
    sum += segment[i];
  }
  const double mean = sum / static_cast<double>(kTailLast - kTailFirst + 1);
  return (top - mean) / height;
}

// ── The two-component fit ──────────────────────────────────────────────────

namespace {

constexpr int kFlatFirst = 3;
constexpr int kFlatLast = 30;
constexpr size_t kTopGuard = 20;
constexpr size_t kPostFirst = 20;
constexpr size_t kPostLast = 34;

// Linear interpolation of |y| at fractional index |x|, clamped to its ends.
double sample_at(const std::vector<double>& y, double x) {
  if (x <= 0.0) return y.front();
  const auto i = static_cast<size_t>(std::floor(x));
  if (i + 1 >= y.size()) return y.back();
  const double f = x - static_cast<double>(i);
  return y[i] + (f * (y[i + 1] - y[i]));
}

double slice_median(const std::vector<double>& y, size_t first, size_t last) {
  if (first >= last || last > y.size()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return median_of({y.begin() + static_cast<std::ptrdiff_t>(first),
                    y.begin() + static_cast<std::ptrdiff_t>(last)});
}

bool feasible(const channel_equalise::EdgeSmear& s) {
  namespace ce = channel_equalise;
  return s.fraction >= 0.0 && s.fraction <= ce::kMaxFraction &&
         s.fast_fraction >= ce::kMinFastFraction &&
         s.fast_fraction <= ce::kMaxFastFraction &&
         s.fraction + s.fast_fraction < 0.6 &&
         s.fast_tau_samples >= ce::kMinFastTauSamples &&
         s.fast_tau_samples <= ce::kMaxFastTauSamples && s.tau_samples >= 2.5 &&
         s.tau_samples <= ce::kMaxTauSamples &&
         s.tau_samples >= 1.5 * s.fast_tau_samples && ce::invertible(s);
}

double sum_of_squares(const std::vector<double>& v) {
  double s = 0.0;
  for (const double x : v) s += x * x;
  return s;
}

// Nelder-Mead (reflection 1, expansion 2, contraction 0.5, shrink 0.5), the
// first simplex 5 % along each non-zero coordinate (0.00025 along a zero
// one), until the simplex lies within |x_tolerance| and its values within
// |f_tolerance| of the best, or after |max_iterations|.
std::pair<std::array<double, 4>, double> nelder_mead(
    const std::function<double(const std::array<double, 4>&)>& f,
    const std::array<double, 4>& start, double x_tolerance, double f_tolerance,
    int max_iterations) {
  constexpr size_t kDim = 4;
  std::array<std::array<double, 4>, kDim + 1> x{};
  std::array<double, kDim + 1> v{};
  x[0] = start;
  for (size_t i = 0; i < kDim; ++i) {
    x[i + 1] = start;
    x[i + 1][i] = start[i] != 0.0 ? 1.05 * start[i] : 0.00025;
  }
  for (size_t i = 0; i <= kDim; ++i) v[i] = f(x[i]);
  for (int iteration = 0; iteration < max_iterations; ++iteration) {
    std::array<size_t, kDim + 1> order{};
    for (size_t i = 0; i <= kDim; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&v](size_t a, size_t b) { return v[a] < v[b]; });
    std::array<std::array<double, 4>, kDim + 1> xs{};
    std::array<double, kDim + 1> vs{};
    for (size_t i = 0; i <= kDim; ++i) {
      xs[i] = x[order[i]];
      vs[i] = v[order[i]];
    }
    x = xs;
    v = vs;
    double x_spread = 0.0;
    double f_spread = 0.0;
    for (size_t i = 1; i <= kDim; ++i) {
      for (size_t k = 0; k < kDim; ++k) {
        x_spread = std::max(x_spread, std::abs(x[i][k] - x[0][k]));
      }
      f_spread = std::max(f_spread, std::abs(v[i] - v[0]));
    }
    if (x_spread <= x_tolerance && f_spread <= f_tolerance) break;
    std::array<double, 4> centroid{};
    for (size_t i = 0; i < kDim; ++i) {
      for (size_t k = 0; k < kDim; ++k) centroid[k] += x[i][k] / kDim;
    }
    auto along = [&](double t) {
      std::array<double, 4> p{};
      for (size_t k = 0; k < kDim; ++k) {
        p[k] = centroid[k] + (t * (x[kDim][k] - centroid[k]));
      }
      return p;
    };
    const auto reflected = along(-1.0);
    const double fr = f(reflected);
    if (fr < v[0]) {
      const auto expanded = along(-2.0);
      const double fe = f(expanded);
      if (fe < fr) {
        x[kDim] = expanded;
        v[kDim] = fe;
      } else {
        x[kDim] = reflected;
        v[kDim] = fr;
      }
      continue;
    }
    if (fr < v[kDim - 1]) {
      x[kDim] = reflected;
      v[kDim] = fr;
      continue;
    }
    bool shrink = false;
    if (fr < v[kDim]) {
      const auto contracted = along(-0.5);
      const double fc = f(contracted);
      if (fc <= fr) {
        x[kDim] = contracted;
        v[kDim] = fc;
      } else {
        shrink = true;
      }
    } else {
      const auto contracted = along(0.5);
      const double fc = f(contracted);
      if (fc < v[kDim]) {
        x[kDim] = contracted;
        v[kDim] = fc;
      } else {
        shrink = true;
      }
    }
    if (shrink) {
      for (size_t i = 1; i <= kDim; ++i) {
        for (size_t k = 0; k < kDim; ++k) {
          x[i][k] = x[0][k] + (0.5 * (x[i][k] - x[0][k]));
        }
        v[i] = f(x[i]);
      }
    }
  }
  size_t best = 0;
  for (size_t i = 1; i <= kDim; ++i) {
    if (v[i] < v[best]) best = i;
  }
  return {x[best], v[best]};
}

}  // namespace

std::vector<double> edge_flatness(const std::vector<double>& y) {
  const auto edge =
      static_cast<size_t>(std::lround(-kSegmentStartUs * kSamplesPerUs));
  if (y.size() < edge + kTopGuard + 2 || edge <= kBaseGuard) return {};
  const double base = slice_median(y, 0, edge - kBaseGuard);
  size_t fall_index = 0;
  for (size_t i = edge + kTopGuard; i < y.size(); ++i) {
    if (y[i] < base + 50.0) {
      fall_index = i;
      break;
    }
  }
  if (fall_index < edge + (2 * kTopGuard) + 2) return {};
  const double top = slice_median(y, edge + kTopGuard, fall_index - kTopGuard);
  const double height = top - base;
  if (!(height > 0.0)) return {};
  const double mid = base + (height / 2.0);
  size_t j = fall_index - 1;
  while (j > edge && y[j] < mid) --j;
  const double fall =
      static_cast<double>(j) + ((y[j] - mid) / (y[j] - y[j + 1]));
  const auto fall_whole = static_cast<size_t>(fall);
  if (fall_whole + kPostLast > y.size() ||
      edge + static_cast<size_t>(kFlatLast) >= y.size()) {
    return {};
  }
  const double post =
      slice_median(y, fall_whole + kPostFirst, fall_whole + kPostLast);
  std::vector<double> out;
  for (int o = kFlatFirst; o <= kFlatLast; ++o) {
    out.push_back((top - y[edge + static_cast<size_t>(o)]) / height);
  }
  for (int o = kFlatFirst; o <= kFlatLast; ++o) {
    out.push_back((sample_at(y, fall + o) - post) / height);
  }
  return out;
}

double predicted_tail(const channel_equalise::EdgeSmear& smear, int first,
                      int last) {
  double sum = 0.0;
  for (int k = first; k <= last; ++k) {
    if (smear.fraction != 0.0) {
      sum += smear.fraction * std::exp(-k / smear.tau_samples);
    }
    if (smear.fast_fraction != 0.0) {
      sum += smear.fast_fraction * std::exp(-k / smear.fast_tau_samples);
    }
  }
  return sum / static_cast<double>(last - first + 1);
}

std::optional<TwoComponentFit> fit_two_component(
    const std::vector<double>& segment) {
  const auto raw = edge_flatness(segment);
  if (raw.empty()) return std::nullopt;
  TwoComponentFit fit;
  const double n = static_cast<double>(raw.size());
  const size_t per_edge = raw.size() / 2;
  constexpr size_t kTailSamples = 6;  // 3 to 8 samples after the edge
  for (size_t i = 0; i < per_edge; ++i) {
    if (i < kTailSamples) {
      fit.rise_tail += raw[i] / kTailSamples;
      fit.fall_tail += raw[per_edge + i] / kTailSamples;
    } else {
      const auto long_samples = static_cast<double>(per_edge - kTailSamples);
      fit.rise_long_tail += raw[i] / long_samples;
      fit.fall_long_tail += raw[per_edge + i] / long_samples;
    }
  }
  fit.residual_identity = std::sqrt(sum_of_squares(raw) / n);

  auto smear_of = [](const std::array<double, 4>& q) {
    return channel_equalise::EdgeSmear{q[2], std::exp(q[3]), q[0],
                                       std::exp(q[1])};
  };
  constexpr double kInfeasible = 1e9;
  auto cost = [&](const std::array<double, 4>& q) {
    const auto smear = smear_of(q);
    if (!feasible(smear)) return kInfeasible;
    std::vector<double> y = segment;
    channel_equalise::unsmear(y, smear);
    const auto d = edge_flatness(y);
    if (d.size() != raw.size()) return kInfeasible;
    return sum_of_squares(d);
  };
  // (fast fraction, fast tau, fraction, tau).
  constexpr std::array<std::array<double, 4>, 5> kStarts = {{
      {0.0, 1.5, 0.2, 6.0},
      {-0.2, 3.0, 0.3, 6.0},
      {0.1, 1.0, 0.0, 8.0},
      {0.0, 1.5, 0.0, 8.0},
      {0.0, 1.0, 0.1, 20.0},
  }};
  double best = std::numeric_limits<double>::infinity();
  std::array<double, 4> best_q{};
  for (const auto& s : kStarts) {
    const std::array<double, 4> q0 = {s[0], std::log(s[1]), s[2],
                                      std::log(s[3])};
    const auto [q, v] = nelder_mead(cost, q0, 1e-4, 1e-10, 1500);
    if (v < best) {
      best = v;
      best_q = q;
    }
  }
  if (!(best < kInfeasible)) return std::nullopt;
  fit.smear = smear_of(best_q);
  fit.residual = std::sqrt(best / n);
  return fit;
}

SmearVerdict judge_two_component(const std::optional<TwoComponentFit>& fit,
                                 bool near_edge) {
  constexpr double kExplained = 0.8;
  constexpr double kMinLongTail = 0.005;
  constexpr double kNearExplained = 0.7;
  constexpr double kMinNearTail = 0.006;
  if (!fit) return SmearVerdict::CannotTell;
  if (fit->residual < kExplained * fit->residual_identity &&
      std::min(fit->rise_long_tail, fit->fall_long_tail) >= kMinLongTail) {
    return SmearVerdict::Smear;
  }
  if (near_edge && std::min(fit->rise_tail, fit->fall_tail) >= kMinNearTail &&
      fit->residual < kNearExplained * fit->residual_identity) {
    return SmearVerdict::Smear;
  }
  return SmearVerdict::NoSmear;
}

namespace {

// The fields of |fields| a robust average keeps: those whose RMS distance
// from the per-sample median is under 3 times the median distance. None when
// fewer than 4 are given.
std::vector<const FieldSegment*> robust_keep(
    const std::vector<const FieldSegment*>& fields) {
  std::vector<const FieldSegment*> kept;
  if (fields.size() < 4) return kept;
  const size_t length = fields.front()->segment.size();
  std::vector<double> med(length);
  std::vector<double> column(fields.size());
  for (size_t i = 0; i < length; ++i) {
    for (size_t s = 0; s < fields.size(); ++s) {
      column[s] = fields[s]->segment[i];
    }
    med[i] = median_of(column);
  }
  std::vector<double> distance(fields.size());
  for (size_t s = 0; s < fields.size(); ++s) {
    double sum = 0.0;
    for (size_t i = 0; i < length; ++i) {
      const double d = fields[s]->segment[i] - med[i];
      sum += d * d;
    }
    distance[s] = std::sqrt(sum / static_cast<double>(length));
  }
  const double limit = 3.0 * median_of(distance);
  for (size_t s = 0; s < fields.size(); ++s) {
    if (distance[s] < limit) kept.push_back(fields[s]);
  }
  return kept;
}

std::vector<double> mean_of(const std::vector<std::vector<double>>& rows) {
  std::vector<double> mean(rows.front().size(), 0.0);
  for (const auto& r : rows) {
    for (size_t i = 0; i < r.size(); ++i) mean[i] += r[i];
  }
  for (double& v : mean) v /= static_cast<double>(rows.size());
  return mean;
}

bool any_smear(const SmearTrack& track) {
  const auto& k = track.knots();
  return std::any_of(k.begin(), k.end(),
                     [](const SmearKnot& knot) { return knot.has_smear(); });
}

SmearKnot knot_of(int64_t frame, const channel_equalise::EdgeSmear& s) {
  SmearKnot k;
  k.frame = frame;
  if (s.active()) {
    k.fraction = s.fraction;
    k.tau_samples = s.fraction > 0.0 ? s.tau_samples : kMinTauSamplesKnot;
    k.fast_fraction = s.fast_fraction;
    k.fast_tau_samples = s.fast_fraction != 0.0 ? s.fast_tau_samples : 1.0;
  }
  return k;
}

// The track the stage applies for |knots|: simplified, then through the
// text form the stage parameter holds.
SmearTrack applied_track(const std::vector<SmearKnot>& knots,
                         const std::vector<bool>& pinned,
                         const ChannelTrackOptions& options) {
  const auto track = SmearTrack::from_knots(knots);
  if (!track) return {};
  const auto simple = track->simplified(options.fraction_tolerance,
                                        options.tau_tolerance, pinned);
  return SmearTrack::parse(simple.to_string()).value_or(simple);
}

}  // namespace

ChannelTrack track_channel_fields(const std::vector<FieldSegment>& fields,
                                  const std::vector<PictureSample>& pictures,
                                  FrameID first, size_t frame_count,
                                  const channel_equalise::EdgeSmear& upstream,
                                  const ChannelTrackOptions& options,
                                  const ChannelCalibrationProgress& progress) {
  ChannelTrack result;
  result.fields_measured = fields.size();
  result.upstream = upstream;
  const window_select::Correction upstream_correction = {upstream, upstream};
  const size_t step = std::max<size_t>(options.block_frames, 1);
  const auto half = static_cast<int64_t>(options.half_window_frames);
  const auto picture_half =
      static_cast<int64_t>(options.picture_half_window_frames);
  const size_t windows =
      frame_count > 0 ? std::max<size_t>((frame_count + step - 1) / step, 1)
                      : 0;

  // A window's frames split by frame number: 0 fits, 1 certifies, and 2 is
  // left for checking.
  const auto splits_of = [&](int64_t centre) {
    std::array<std::array<std::vector<const FieldSegment*>, 2>, 2> by_split;
    for (const auto& f : fields) {
      const auto frame = static_cast<int64_t>(f.frame);
      if (frame < centre - half || frame > centre + half) continue;
      const int64_t split = frame % 3;
      if (split > 1) continue;
      by_split[static_cast<size_t>(split)][f.field == 0 ? 0 : 1].push_back(&f);
    }
    std::array<window_select::Split, 2> splits;
    for (size_t sp = 0; sp < 2; ++sp) {
      for (size_t parity = 0; parity < 2; ++parity) {
        for (const FieldSegment* f : robust_keep(by_split[sp][parity])) {
          splits[sp].segments[parity].emplace_back(f->segment.begin(),
                                                   f->segment.end());
          splits[sp].segment_frames[parity].push_back(
              static_cast<int64_t>(f->frame));
        }
      }
    }
    for (const auto& p : pictures) {
      const auto frame = static_cast<int64_t>(p.frame);
      if (frame < centre - picture_half || frame > centre + picture_half) {
        continue;
      }
      const int64_t split = frame % 3;
      if (split > 1) continue;
      auto& into = splits[static_cast<size_t>(split)];
      into.edges.append(p.edges);
      into.edge_frames.insert(into.edge_frames.end(), p.edges.size(), frame);
    }
    return splits;
  };

  std::vector<int64_t> centres;
  std::vector<window_select::Correction> chosen;
  for (size_t w = 0; w < windows; ++w) {
    if (progress && !progress(static_cast<int>(80 * w / windows),
                              "Choosing the correction along the source")) {
      return {};
    }
    ChannelTrackBlock block;
    block.first = first + (w * step);
    block.frames = std::min(step, frame_count - (w * step));
    const auto centre = static_cast<int64_t>(block.first) +
                        static_cast<int64_t>(block.frames / 2);
    const auto splits = splits_of(centre);
    block.fields_used =
        splits[0].segments[0].size() + splits[0].segments[1].size();
    block.edges_used = splits[1].edges.size();
    // The tracked candidate: each parity's two-component fit on the fit
    // split.
    std::optional<window_select::Correction> tracked;
    window_select::Correction t{};
    bool any = false;
    for (size_t parity = 0; parity < 2; ++parity) {
      const auto& rows = splits[0].segments[parity];
      if (rows.size() < options.min_fields) continue;
      const auto fit = fit_two_component(mean_of(rows));
      if (fit &&
          judge_two_component(fit, options.near_edge) == SmearVerdict::Smear) {
        t[parity] = fit->smear;
        any = true;
      }
    }
    if (any) tracked = t;
    const auto choice = window_select::choose(splits[0], splits[1],
                                              upstream_correction, tracked);
    block.choice = choice.name;
    centres.push_back(centre);
    chosen.push_back(choice.correction);
    result.blocks.push_back(std::move(block));
  }

  // Each window's own correction is certified; the track the stage applies
  // must be too. Simplifying it moves the smear between the knots it keeps,
  // and any frame between two knots takes a blend of their corrections, so
  // each window is certified again on the track as the stage applies it
  // (simplified, through the text form, interpolated frame by frame). Where
  // that fails, the knots around the window are kept exactly; where it fails
  // on those, the window (or, when it already keeps upstream's, the
  // neighbours whose blend reaches it) is set back to upstream's correction.
  const size_t n = centres.size();
  std::vector<bool> is_upstream(n);
  for (size_t w = 0; w < n; ++w) {
    is_upstream[w] = result.blocks[w].choice == "upstream";
  }
  // Upstream's correction as the text form gives it.
  const auto rounded = [&](const channel_equalise::EdgeSmear& s) {
    return applied_track({knot_of(0, s)}, {}, options).at(0);
  };
  const window_select::Correction upstream_applied = {rounded(upstream),
                                                      rounded(upstream)};
  // The knots whose interpolation reaches window |w|'s frames.
  const auto reach = [&](size_t w) {
    size_t lo = w;
    while (lo > 0 && centres[lo] > centres[w] - half) --lo;
    size_t hi = w;
    while (hi + 1 < n && centres[hi] < centres[w] + half) ++hi;
    return std::make_pair(lo, hi);
  };
  std::vector<bool> pinned(n, false);
  std::vector<std::vector<double>> passed(n);
  std::array<SmearTrack, 2> tracks;
  for (size_t round = 0; round <= 2 * n; ++round) {
    if (progress && !progress(80 + static_cast<int>(round < 19 ? round : 19),
                              "Certifying the track as it is applied")) {
      return {};
    }
    for (size_t parity = 0; parity < 2; ++parity) {
      std::vector<SmearKnot> knots;
      for (size_t w = 0; w < n; ++w) {
        knots.push_back(knot_of(centres[w], chosen[w][parity]));
      }
      tracks[parity] = applied_track(knots, pinned, options);
    }
    const window_select::CorrectionAt applied = [&tracks](int64_t frame) {
      return window_select::Correction{tracks[0].at(frame),
                                       tracks[1].at(frame)};
    };
    std::vector<size_t> failing;
    for (size_t w = 0; w < n; ++w) {
      std::vector<double> signature;
      bool as_upstream = true;
      for (int64_t f = centres[w] - half; f <= centres[w] + half; ++f) {
        const auto c = applied(f);
        for (size_t parity = 0; parity < 2; ++parity) {
          as_upstream = as_upstream && c[parity] == upstream_applied[parity];
          signature.insert(
              signature.end(),
              {c[parity].fraction, c[parity].tau_samples,
               c[parity].fast_fraction, c[parity].fast_tau_samples});
        }
      }
      // Upstream's own correction needs no certificate; nor does what a
      // window was certified with already.
      if (as_upstream || signature == passed[w]) continue;
      const auto splits = splits_of(centres[w]);
      if (window_select::certified(splits[1], applied, upstream_correction,
                                   !is_upstream[w])) {
        passed[w] = std::move(signature);
      } else {
        failing.push_back(w);
      }
    }
    if (failing.empty()) break;
    bool changed = false;
    for (const size_t w : failing) {
      const auto [lo, hi] = reach(w);
      bool pinned_now = false;
      for (size_t v = lo; v <= hi; ++v) {
        if (!pinned[v]) {
          pinned[v] = true;
          pinned_now = true;
        }
      }
      if (pinned_now) {
        changed = true;
        continue;
      }
      for (size_t v = lo; v <= hi; ++v) {
        if (is_upstream[v] || (v != w && !is_upstream[w])) continue;
        chosen[v] = upstream_correction;
        is_upstream[v] = true;
        result.blocks[v].choice = "upstream";
        result.blocks[v].reverted = true;
        changed = true;
      }
    }
    if (!changed) break;
  }
  for (size_t w = 0; w < n; ++w) {
    result.blocks[w].verdict = (chosen[w][0].active() || chosen[w][1].active())
                                   ? SmearVerdict::Smear
                                   : SmearVerdict::NoSmear;
  }
  // No correction anywhere is no track: the stage then passes every frame
  // through.
  if (any_smear(tracks[0]) || any_smear(tracks[1])) {
    result.track = tracks[0];
    result.track_field2 = tracks[1];
  }
  if (progress) progress(100, "Done");
  return result;
}

ChannelTrack track_channel(const VideoFrameRepresentation& source,
                           const ChannelTrackOptions& options,
                           const ChannelCalibrationProgress& progress) {
  const auto params = source.get_video_parameters();
  if (!params || params->system != VideoSystem::PAL ||
      source.has_separate_channels() ||
      !(params->white_level > params->blanking_level)) {
    return {};
  }
  const double blanking = params->blanking_level;
  const double excursion = params->white_level - params->blanking_level;
  const double scale = 100.0 / excursion;
  const size_t count = source.frame_count();
  const FrameID origin = source.frame_range().first;

  // Upstream's per-source smear: what every window must not be worse than.
  const auto calibration =
      calibrate_channel(source, ChannelCalibrationOptions{},
                        [&progress](int percent, const std::string& status) {
                          return !progress || progress(percent / 10, status);
                        });
  channel_equalise::EdgeSmear upstream;
  if (calibration.verdict == SmearVerdict::Smear) {
    upstream = {calibration.fraction, calibration.tau_samples};
  }

  std::vector<FieldSegment> fields;
  std::vector<PictureSample> pictures;
  std::array<size_t, 2> result_edges{};
  size_t read = 0;
  // A short source's picture is read more often, so that each window's
  // certifying third has picture edges to judge a correction on.
  const size_t stride = std::max<size_t>(
      std::min(options.picture_stride_frames, count / kPictureFramesWanted), 1);
  for (size_t k = 0; k < count; ++k) {
    if ((k % 256) == 0 && progress &&
        !progress(10 + static_cast<int>(60 * k / std::max<size_t>(count, 1)),
                  "Measuring the VITS of every field")) {
      return {};
    }
    const FrameID id = origin + k;
    for (const size_t line : {kVitsLine, kVitsLineField2}) {
      const auto ire = line_ire(source, id, line, blanking, scale);
      if (ire.empty()) continue;
      ++read;
      const auto segment = aligned_bar_segment(ire);
      if (!segment) continue;
      if (!std::isfinite(segment_tail(*segment))) continue;
      fields.push_back({id, line == kVitsLine ? 0U : 1U,
                        std::vector<float>(segment->begin(), segment->end())});
    }
    if (k % stride != 0) continue;
    const auto desc = source.get_frame_descriptor(id);
    if (!desc) continue;
    const size_t width = desc->samples_per_line_nominal;
    PictureSample sample;
    sample.frame = id;
    for (uint8_t parity = 0; parity < 2; ++parity) {
      // The first field is 313 lines from flat line 0, the second the 312
      // after it.
      const size_t first_line = parity == 0 ? 0 : kPalFieldLines;
      const size_t lines = parity == 0 ? kPalFieldLines : kPalFieldLines - 1;
      std::vector<double> field(lines * width, 0.0);
      bool complete = true;
      for (size_t line = 0; line < lines; ++line) {
        const size_t flat = first_line + line;
        if (flat >= desc->height) {
          complete = false;
          break;
        }
        const int16_t* raw = source.get_line(id, flat);
        if (!raw) {
          complete = false;
          break;
        }
        for (size_t i = 0; i < width; ++i) {
          field[(line * width) + i] = (raw[i] - blanking) / excursion;
        }
      }
      if (!complete) continue;
      const auto edges = picture_edges::find(field, width, parity,
                                             options.max_edges_per_field);
      result_edges[parity] += edges.size();
      sample.edges.append(edges);
    }
    pictures.push_back(std::move(sample));
  }
  auto stage_progress = [&progress](int percent, const std::string& status) {
    return !progress || progress(70 + ((percent * 3) / 10), status);
  };
  auto result = track_channel_fields(fields, pictures, origin, count, upstream,
                                     options, stage_progress);
  result.fields_read = read;
  result.picture_edges_found = result_edges;
  ORC_LOG_DEBUG("Channel tracking: {} of {} fields measured, {} + {} knots",
                result.fields_measured, read, result.track.knots().size(),
                result.track_field2.knots().size());
  return result;
}

}  // namespace orc
