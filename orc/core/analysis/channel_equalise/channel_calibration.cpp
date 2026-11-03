/*
 * File:        channel_calibration.cpp
 * Module:      analysis
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
#include <limits>

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

// equalise.py's guards: a fit is a smear only when clear of the parameter
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
// (numpy's irfft(rfft(x) * exp(2 pi j k frac))).
std::vector<double> fractional_advance(const std::vector<double>& x,
                                       double frac) {
  const size_t n = x.size();
  const size_t bins = (n / 2) + 1;
  std::vector<std::complex<double>> spectrum(bins);
  for (size_t k = 0; k < bins; ++k) {
    std::complex<double> acc;
    for (size_t t = 0; t < n; ++t) {
      const double angle = -2.0 * kPi * static_cast<double>((k * t) % n) /
                           static_cast<double>(n);
      acc += x[t] * std::complex<double>(std::cos(angle), std::sin(angle));
    }
    const double shift =
        2.0 * kPi * static_cast<double>(k) * frac / static_cast<double>(n);
    spectrum[k] = acc * std::complex<double>(std::cos(shift), std::sin(shift));
  }
  // An even length's Nyquist bin is real in irfft.
  if (n % 2 == 0) spectrum[bins - 1] = spectrum[bins - 1].real();
  std::vector<double> y(n);
  for (size_t t = 0; t < n; ++t) {
    double acc = spectrum[0].real();
    for (size_t k = 1; k < bins; ++k) {
      const double angle =
          2.0 * kPi * static_cast<double>((k * t) % n) / static_cast<double>(n);
      const double term = (spectrum[k].real() * std::cos(angle)) -
                          (spectrum[k].imag() * std::sin(angle));
      acc += (n % 2 == 0 && k == bins - 1) ? term : 2.0 * term;
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

  // The prototype's bounds on the bar's height (IRE), the pulse's area (IRE
  // us) and the base (IRE). The area's bound is below a 2T pulse's own and
  // holds on every capture measured: the fitted tails, and the Phase 5
  // values, are those of the bounded fit.
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

std::vector<double> aligned_line19_average(
    const VideoFrameRepresentation& source, const std::vector<FrameID>& frames,
    size_t* frames_used) {
  const auto params = source.get_video_parameters();
  const auto length = static_cast<size_t>(
      std::lround((kSegmentStopUs - kSegmentStartUs) * kSamplesPerUs));
  std::vector<double> sum(length, 0.0);
  size_t used = 0;
  if (params && params->white_level > params->blanking_level) {
    const double blanking = params->blanking_level;
    const double scale = 100.0 / (params->white_level - params->blanking_level);
    for (const FrameID id : frames) {
      const auto desc = source.get_frame_descriptor(id);
      if (!desc) continue;
      const size_t width = desc->samples_per_line_nominal;
      const int16_t* raw = source.get_line(id, kVitsLine);
      if (!raw) continue;
      std::vector<double> line(width);
      for (size_t i = 0; i < width; ++i) line[i] = (raw[i] - blanking) * scale;
      const auto edge = bar_edge(line);
      if (!edge) continue;  // a dropout on the bar
      const double start = *edge + (kSegmentStartUs * kSamplesPerUs);
      const double whole = std::floor(start);
      const auto first = static_cast<std::ptrdiff_t>(whole) -
                         static_cast<std::ptrdiff_t>(kShiftPad);
      const auto end =
          first + static_cast<std::ptrdiff_t>(length + kShiftPad + kShiftPad);
      if (first < 0 || end > static_cast<std::ptrdiff_t>(width)) continue;
      const auto shifted = fractional_advance(
          {line.begin() + first, line.begin() + end}, start - whole);
      for (size_t i = 0; i < length; ++i) sum[i] += shifted[kShiftPad + i];
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

}  // namespace orc
