/*
 * File:        fm_beat.cpp
 * Module:      orc-sdk-support
 * Purpose:     The PAL LaserDisc FM-beat herringbone of one field, predicted
 *              from the field itself, fitted and subtracted
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/composite_split.h>
#include <orc/support/fm_beat.h>
#include <orc/support/frame_line_util.h>

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace orc::fm_beat {

namespace {

constexpr double kPi = composite_split::kPi;

// The beat model, as `_beat_basis` in the reference. The weight of the
// chroma's own phase modulation in the carrier phase, and the beat's
// amplitude law (f_c - f_sc) / scale * exp(alpha (f_c - pivot) / 1 MHz),
// which falls with brightness.
constexpr double kChromaPmWeight = 0.4;
constexpr double kAmplitudeLawAlpha = -1.0;
constexpr double kAmplitudeLawPivotHz = 7.5e6;
constexpr double kAmplitudeScaleHz = 3.0e9;
// Box lengths that strip the chroma's phase modulation from the carrier
// frequency before the amplitude law (4 samples null f_sc at 4fsc).
constexpr int32_t kCarrierBoxShort = 4;
constexpr int32_t kCarrierBoxLong = 17;
// ld-decode's PAL video emphasis: pre-emphasis has its zero at 1 / 400 ns
// and its pole at 1 / 100 ns; de-emphasis is its inverse.
constexpr double kEmphasisSlowS = 400.0e-9;
constexpr double kEmphasisFastS = 100.0e-9;
// The beat band: unity from low + ramp / 2 to high - ramp / 2, falling
// linearly to zero over |ramp| centred on each edge; a Kaiser-windowed FIR.
constexpr double kBandLowHz = 2.2e6;
constexpr double kBandHighHz = 3.9e6;
constexpr double kBandRampHz = 0.3e6;
constexpr int32_t kBandPassRadius = 127;
constexpr double kBandPassKaiserBeta = 8.0;

// The fit. Lines per amplitude window, and lines over which the beat's
// line-to-line phase advance is tracked (Hann windows).
constexpr int32_t kAmplitudeWindowLines = 15;
constexpr int32_t kTrackWindowLines = 9;
// The fit weight falls where the luma or the chroma envelope changes: a
// 25-sample sum of their slopes, in IRE, at which it has halved.
constexpr int32_t kEdgeSumSamples = 25;
constexpr double kEdgeIre = 15.9;
// Band power (a 25-sample mean of the band-passed field, as an RMS in IRE)
// above which the fit weight and the subtraction fall away: gratings and
// fine texture at 2-4 MHz carry far more than any beat.
constexpr int32_t kBandPowerSamples = 25;
constexpr double kFitBandRmsIre = 6.6;
constexpr double kSubtractBandRmsIre = 9.3;
// A line is corrected from |amplitude| / standard error = kMinSignificance,
// fully from twice it, and up to kMaxAmplitude, the largest beat constant a
// disc carries (Community South and North read 135-270); a larger fit is
// picture structure that happens to correlate.
constexpr double kMinSignificance = 4.0;
constexpr double kMaxAmplitude = 450.0;

// The pre-gate, as in the reference: x[n] - x[n + 2] is twice the chroma at
// 4fsc.
constexpr double kPreGateChromaIre = 10.6;
constexpr double kPreGateFlatIre = 4.0;
constexpr int32_t kPreGateChromaBox = 8;
constexpr int32_t kPreGateLumaBox = 4;
constexpr int32_t kPreGateLumaStep = 8;
constexpr int32_t kPreGateFlatBox = 16;

// A moving mean of |n| samples, window [i - n / 2, i - n / 2 + n - 1], the
// end samples repeated beyond the ends (scipy's uniform_filter1d, 'nearest').
std::vector<double> box(const std::vector<double>& v, int32_t n) {
  const auto len = static_cast<int64_t>(v.size());
  std::vector<double> out(v.size());
  if (len == 0) return out;
  auto at = [&](int64_t j) {
    return v[static_cast<size_t>(std::clamp<int64_t>(j, 0, len - 1))];
  };
  const int64_t lo = -(n / 2);
  double sum = 0.0;
  for (int64_t j = lo; j < lo + n; ++j) sum += at(j);
  for (int64_t i = 0; i < len; ++i) {
    out[static_cast<size_t>(i)] = sum / n;
    sum += at(i + lo + n) - at(i + lo);
  }
  return out;
}

// A centred convolution with a symmetric kernel, zero beyond the ends
// (numpy's convolve, 'same').
template <typename T>
std::vector<T> convolve_same(const std::vector<T>& v,
                             const std::vector<double>& kernel) {
  const auto len = static_cast<int64_t>(v.size());
  const auto half = static_cast<int64_t>(kernel.size() / 2);
  std::vector<T> out(v.size(), T{});
  for (int64_t i = 0; i < len; ++i) {
    T sum{};
    for (int64_t j = 0; j < static_cast<int64_t>(kernel.size()); ++j) {
      const int64_t m = i + j - half;
      if (m >= 0 && m < len) {
        sum += v[static_cast<size_t>(m)] * kernel[static_cast<size_t>(j)];
      }
    }
    out[static_cast<size_t>(i)] = sum;
  }
  return out;
}

// The |n| inner weights of a Hann window of n + 2 points (numpy's
// hanning(n + 2)[1:-1]).
std::vector<double> hann_inner(int32_t n) {
  std::vector<double> w(static_cast<size_t>(n));
  for (int32_t k = 1; k <= n; ++k) {
    w[static_cast<size_t>(k - 1)] =
        0.5 - (0.5 * std::cos(2.0 * kPi * k / (n + 1)));
  }
  return w;
}

double bessel_i0(double x) {
  double sum = 1.0;
  double term = 1.0;
  for (int k = 1; k < 50; ++k) {
    term *= (x / (2.0 * k)) * (x / (2.0 * k));
    sum += term;
    if (term < sum * 1e-12) break;
  }
  return sum;
}

double sinc(double x) { return x == 0.0 ? 1.0 : std::sin(kPi * x) / (kPi * x); }

// The beat band-pass, taps 0..radius of a symmetric FIR: the ideal
// trapezoid's response (a low-pass with a linear ramp is a sinc times a
// sinc), windowed.
std::vector<double> band_pass_taps(double sample_rate_hz) {
  std::vector<double> taps(static_cast<size_t>(kBandPassRadius) + 1);
  const double ramp = kBandRampHz / sample_rate_hz;
  for (int32_t k = 0; k <= kBandPassRadius; ++k) {
    auto low_pass = [&](double edge_hz) {
      const double fc = edge_hz / sample_rate_hz;
      return 2.0 * fc * sinc(2.0 * fc * k) * sinc(ramp * k);
    };
    const double r = static_cast<double>(k) / (kBandPassRadius + 1);
    const double window =
        bessel_i0(kBandPassKaiserBeta * std::sqrt(1.0 - (r * r))) /
        bessel_i0(kBandPassKaiserBeta);
    taps[static_cast<size_t>(k)] =
        (low_pass(kBandHighHz) - low_pass(kBandLowHz)) * window;
  }
  return taps;
}

// First-order IIR y[n] = b0 x[n] + b1 x[n-1] - a1 y[n-1]: the bilinear
// transform, pre-warped, of H(s) = (w2 / w1) (s + w1) / (s + w2), w = 1 / t.
struct Emphasis {
  double b0 = 1.0;
  double b1 = 0.0;
  double a1 = 0.0;

  Emphasis(double zero_time_s, double pole_time_s, double sample_rate_hz) {
    const double c = 2.0 * sample_rate_hz;
    const double w1 = c * std::tan((1.0 / zero_time_s) / c);
    const double w2 = c * std::tan((1.0 / pole_time_s) / c);
    const double gain = w2 / w1;
    b0 = gain * (c + w1) / (c + w2);
    b1 = gain * (w1 - c) / (c + w2);
    a1 = (w2 - c) / (c + w2);
  }

  std::complex<double> response(double omega) const {
    const std::complex<double> z1 = std::polar(1.0, -omega);
    return (b0 + (b1 * z1)) / (1.0 + (a1 * z1));
  }

  template <typename T>
  void apply(std::vector<T>& v) const {
    T x1{};
    T y1{};
    for (T& value : v) {
      const T y = (b0 * value) + (b1 * x1) - (a1 * y1);
      x1 = value;
      y1 = y;
      value = y;
    }
  }
};

std::vector<double> gradient_magnitude(const std::vector<double>& v) {
  const size_t n = v.size();
  std::vector<double> out(n, 0.0);
  if (n < 2) return out;
  out[0] = std::abs(v[1] - v[0]);
  out[n - 1] = std::abs(v[n - 1] - v[n - 2]);
  for (size_t i = 1; i + 1 < n; ++i) {
    out[i] = std::abs(v[i + 1] - v[i - 1]) / 2.0;
  }
  return out;
}

}  // namespace

std::array<FieldGeometry, 2> frame_fields(const FrameDescriptor& desc,
                                          const SourceParameters& params) {
  const size_t f1 = std::min(field1_lines(desc.system), desc.height);
  std::array<FieldGeometry, 2> fields{};
  for (size_t f = 0; f < 2; ++f) {
    FieldGeometry& g = fields[f];
    g.system = desc.system;
    g.samples_per_line = desc.samples_per_line_nominal;
    g.first_line = f == 0 ? 0 : f1;
    g.line_count = f == 0 ? f1 : desc.height - f1;
    g.picture_first = 0;
    g.picture_end = g.line_count;
    // Frame lines interleave the fields: field 1's line n is frame line 2n.
    if (params.first_active_frame_line >= 0 &&
        params.last_active_frame_line >= 0) {
      g.picture_first = static_cast<size_t>(params.first_active_frame_line / 2);
      const auto last =
          static_cast<size_t>(f == 0 ? (params.last_active_frame_line + 1) / 2
                                     : params.last_active_frame_line / 2);
      g.picture_end = std::min(g.line_count, last);
    }
    g.active_start = 0;
    g.active_end = static_cast<int32_t>(desc.samples_per_line_nominal);
    if (params.active_video_start >= 0 &&
        params.active_video_end > params.active_video_start) {
      g.active_start = params.active_video_start;
      g.active_end = std::min(params.active_video_end, g.active_end);
    }
    g.black_level = params.black_level;
    g.white_level = params.white_level;
  }
  return fields;
}

size_t flat_colour_samples(const int16_t* frame, const FieldGeometry& field) {
  const double codes_per_ire = (field.white_level - field.black_level) / 100.0;
  if (codes_per_ire <= 0.0) return 0;
  const int32_t width = field.active_end - field.active_start;
  if (width <= kPreGateLumaStep + 2) return 0;
  size_t count = 0;
  std::vector<double> a(static_cast<size_t>(width));
  std::vector<double> step(static_cast<size_t>(width - 2));
  std::vector<double> change(static_cast<size_t>(width - kPreGateLumaStep));
  for (size_t line = field.picture_first; line < field.picture_end; ++line) {
    const int16_t* x =
        frame + frame_line_sample_offset(field.system, field.samples_per_line,
                                         field.first_line + line);
    for (int32_t i = 0; i < width; ++i) {
      a[static_cast<size_t>(i)] = x[field.active_start + i] / codes_per_ire;
    }
    for (size_t i = 0; i < step.size(); ++i) {
      step[i] = std::abs(a[i] - a[i + 2]);
    }
    const auto chroma = box(step, kPreGateChromaBox);
    const auto luma = box(a, kPreGateLumaBox);
    for (size_t i = 0; i < change.size(); ++i) {
      change[i] = std::abs(luma[i + kPreGateLumaStep] - luma[i]);
    }
    const auto flat = box(change, kPreGateFlatBox);
    for (size_t i = 0; i < flat.size(); ++i) {
      if (chroma[i] * (kPi / 4.0) > kPreGateChromaIre &&
          flat[i] < kPreGateFlatIre) {
        ++count;
      }
    }
  }
  return count;
}

FieldBeat::FieldBeat(const int16_t* frame, const FieldGeometry& field,
                     double carrier_black_hz)
    : field_(field) {
  codes_per_ire_ = (field.white_level - field.black_level) / 100.0;
  if (codes_per_ire_ <= 0.0) codes_per_ire_ = 1.0;
  const double fs = sample_rate_from_system(field.system);
  const double fsc = fs / 4.0;
  width_ =
      static_cast<size_t>(std::max(0, field.active_end - field.active_start));

  // The field as one signal, its lines in time order, in IRE; and its
  // analytic chroma, line by line.
  std::vector<size_t> line_start(field.line_count + 1, 0);
  for (size_t l = 0; l < field.line_count; ++l) {
    line_start[l + 1] =
        line_start[l] + frame_line_sample_count(field.system,
                                                field.samples_per_line,
                                                field.first_line + l);
  }
  const size_t total = line_start[field.line_count];
  std::vector<double> ire(total);
  std::vector<std::complex<double>> chroma(total);
  const composite_split::ChromaSplitter splitter(field.system);
  std::vector<std::complex<double>> phasor;
  for (size_t l = 0; l < field.line_count; ++l) {
    const int16_t* x =
        frame + frame_line_sample_offset(field.system, field.samples_per_line,
                                         field.first_line + l);
    const auto length = static_cast<int32_t>(line_start[l + 1] - line_start[l]);
    splitter.demodulate(x, length, 0, length, phasor);
    for (int32_t n = 0; n < length; ++n) {
      const size_t i = line_start[l] + static_cast<size_t>(n);
      ire[i] = (x[n] - field.black_level) / codes_per_ire_;
      const auto q = static_cast<size_t>(n & 3);
      chroma[i] = phasor[static_cast<size_t>(n)] *
                  std::complex<double>(composite_split::kSubcarrierCos[q],
                                       composite_split::kSubcarrierSin[q]) /
                  codes_per_ire_;
    }
  }
  if (total == 0) return;

  // The carrier: the pre-emphasised composite sets it, less part of the
  // chroma's own phase modulation.
  const Emphasis pre(kEmphasisSlowS, kEmphasisFastS, fs);
  std::vector<double> emphasised(total);
  for (size_t i = 0; i < total; ++i) emphasised[i] = ire[i] - ire[0];
  pre.apply(emphasised);
  const std::complex<double> pre_at_fsc = pre.response(2.0 * kPi * fsc / fs);
  std::vector<double> carrier(total);
  for (size_t i = 0; i < total; ++i) {
    const double chroma_part = (chroma[i] * pre_at_fsc).real();
    carrier[i] = carrier_black_hz +
                 (kCarrierHzPerIre * (emphasised[i] + ire[0] -
                                      ((1.0 - kChromaPmWeight) * chroma_part)));
  }
  const auto smoothed = box(box(carrier, kCarrierBoxShort), kCarrierBoxLong);

  // The beat: the carrier's phase mixed with the chroma, then de-emphasised.
  std::vector<std::complex<double>> beat(total);
  double phase = 0.0;
  for (size_t i = 0; i < total; ++i) {
    phase = std::fmod(phase + (2.0 * kPi * carrier[i] / fs), 2.0 * kPi);
    const double lf = smoothed[i];
    const double amplitude =
        (lf - fsc) / kAmplitudeScaleHz *
        std::exp(kAmplitudeLawAlpha * (lf - kAmplitudeLawPivotHz) / 1.0e6);
    beat[i] = std::polar(amplitude, phase) * std::conj(chroma[i]);
  }
  const Emphasis de(kEmphasisFastS, kEmphasisSlowS, fs);
  de.apply(beat);

  // Edges of the luma and of the chroma envelope.
  std::vector<double> luma(total);
  std::vector<double> envelope(total);
  for (size_t i = 0; i < total; ++i) {
    luma[i] = ire[i] - chroma[i].real();
    envelope[i] = std::abs(chroma[i]);
  }
  const auto luma_slope = gradient_magnitude(box(box(luma, 4), 9));
  const auto envelope_slope = gradient_magnitude(box(envelope, 17));
  std::vector<double> slope(total);
  for (size_t i = 0; i < total; ++i) {
    slope[i] = luma_slope[i] + envelope_slope[i];
  }
  const auto edge = box(slope, kEdgeSumSamples);

  // The picture: the predicted beat and the field in the beat band, the fit
  // weight and the subtraction mask.
  const auto taps = band_pass_taps(fs);
  const auto band_pass_at = [&](const auto& v, int64_t centre) {
    using T = std::decay_t<decltype(v[0])>;
    T sum = taps[0] * v[static_cast<size_t>(centre)];
    for (int64_t k = 1; k <= kBandPassRadius; ++k) {
      const int64_t lo = centre - k;
      const int64_t hi = centre + k;
      T pair{};
      if (lo >= 0) pair += v[static_cast<size_t>(lo)];
      if (hi < static_cast<int64_t>(total)) pair += v[static_cast<size_t>(hi)];
      sum += taps[static_cast<size_t>(k)] * pair;
    }
    return sum;
  };
  const size_t lines = field.picture_end > field.picture_first
                           ? field.picture_end - field.picture_first
                           : 0;
  field_offset_.resize(lines);
  frame_offset_.resize(lines);
  basis_.resize(lines * width_);
  band_.resize(lines * width_);
  weight_.resize(lines * width_);
  mask_.resize(lines * width_);
  input_.resize(lines * width_);
  const int64_t half_power = kBandPowerSamples / 2;
  const double fit_power = kFitBandRmsIre * kFitBandRmsIre;
  const double subtract_power = kSubtractBandRmsIre * kSubtractBandRmsIre;
  std::vector<double> band_line(width_ + (2 * static_cast<size_t>(half_power)));
  for (size_t p = 0; p < lines; ++p) {
    const size_t l = field.picture_first + p;
    field_offset_[p] = line_start[l] + static_cast<size_t>(field.active_start);
    frame_offset_[p] =
        frame_line_sample_offset(field.system, field.samples_per_line,
                                 field.first_line + l) +
        static_cast<size_t>(field.active_start);
    const auto first = static_cast<int64_t>(field_offset_[p]);
    for (size_t j = 0; j < band_line.size(); ++j) {
      const int64_t n =
          std::clamp<int64_t>(first + static_cast<int64_t>(j) - half_power, 0,
                              static_cast<int64_t>(total) - 1);
      band_line[j] = band_pass_at(ire, n);
    }
    for (size_t i = 0; i < width_; ++i) {
      const int64_t n = first + static_cast<int64_t>(i);
      const size_t k = (p * width_) + i;
      basis_[k] = band_pass_at(beat, n);
      band_[k] = band_line[i + static_cast<size_t>(half_power)];
      double power = 0.0;
      for (int64_t j = 0; j < kBandPowerSamples; ++j) {
        const double b = band_line[i + static_cast<size_t>(j)];
        power += b * b;
      }
      power /= kBandPowerSamples;
      const double g =
          edge[static_cast<size_t>(n)] * kEdgeSumSamples / kEdgeIre;
      const double fit_ratio = power / fit_power;
      const double subtract_ratio = power / subtract_power;
      weight_[k] =
          1.0 / (1.0 + (g * g * g * g)) / (1.0 + (fit_ratio * fit_ratio));
      mask_[k] = 1.0 / (1.0 + (subtract_ratio * subtract_ratio));
      input_[k] = frame[frame_offset_[p] + i];
    }
  }
}

void FieldBeat::line_basis(size_t line, double offset_hz,
                           std::vector<std::complex<double>>& out) const {
  const auto* b = basis_.data() + (line * width_);
  out.assign(b, b + width_);
  if (offset_hz == 0.0) return;
  const double radians_per_sample =
      2.0 * kPi * offset_hz / sample_rate_from_system(field_.system);
  std::complex<double> rotation = std::polar(
      1.0, radians_per_sample * static_cast<double>(field_offset_[line]));
  const std::complex<double> step = std::polar(1.0, radians_per_sample);
  for (auto& value : out) {
    value *= rotation;
    rotation *= step;
  }
}

BeatFit FieldBeat::fit(double carrier_offset_hz) const {
  BeatFit result;
  result.carrier_offset_hz = carrier_offset_hz;
  const size_t lines = field_offset_.size();
  result.amplitude.assign(lines, {0.0, 0.0});
  if (lines == 0 || width_ == 0) return result;

  // Per line, the weighted normal equations of Re(c B) = cr b1 + ci b2.
  std::vector<double> g11(lines);
  std::vector<double> g22(lines);
  std::vector<double> g12(lines);
  std::vector<double> r1(lines);
  std::vector<double> r2(lines);
  double weighted_power = 0.0;
  double weight_sum = 0.0;
  std::vector<std::complex<double>> basis;
  for (size_t p = 0; p < lines; ++p) {
    line_basis(p, carrier_offset_hz, basis);
    double s11 = 0.0;
    double s22 = 0.0;
    double s12 = 0.0;
    double t1 = 0.0;
    double t2 = 0.0;
    for (size_t i = 0; i < width_; ++i) {
      const size_t k = (p * width_) + i;
      const std::complex<double> b = basis[i];
      const double w = weight_[k];
      const double d = band_[k];
      const double b1 = b.real();
      const double b2 = -b.imag();
      s11 += w * b1 * b1;
      s22 += w * b2 * b2;
      s12 += w * b1 * b2;
      t1 += w * b1 * d;
      t2 += w * b2 * d;
      weighted_power += w * d * d;
      weight_sum += w;
    }
    g11[p] = s11;
    g22[p] = s22;
    g12[p] = s12;
    r1[p] = t1;
    r2[p] = t2;
  }

  // The tracked phase: the angle of the weighted, smoothed z_l z*_(l-1),
  // integrated over the lines.
  std::vector<std::complex<double>> step(lines, {0.0, 0.0});
  std::complex<double> previous{0.0, 0.0};
  double previous_weight = 0.0;
  for (size_t p = 0; p < lines; ++p) {
    const double det = (g11[p] * g22[p]) - (g12[p] * g12[p]) +
                       (1e-9 * (g11[p] + g22[p]) * (g11[p] + g22[p])) + 1e-12;
    const std::complex<double> z((g22[p] * r1[p] - g12[p] * r2[p]) / det,
                                 (g11[p] * r2[p] - g12[p] * r1[p]) / det);
    const double wl = g11[p] + g22[p];
    if (p > 0) {
      step[p] = z * std::conj(previous) * std::sqrt(wl * previous_weight);
    }
    previous = z;
    previous_weight = wl;
  }
  const auto smoothed = convolve_same(step, hann_inner(kTrackWindowLines));
  std::vector<double> theta(lines);
  double sum = 0.0;
  for (size_t p = 0; p < lines; ++p) {
    sum += std::arg(smoothed[p]);
    theta[p] = sum;
  }
  const double theta0 = theta[0];
  for (double& t : theta) t -= theta0;

  // c_l = s e_l with s constant over a Hann window of lines: the rotated
  // normal equations, summed over the window.
  std::vector<double> a11(lines);
  std::vector<double> a22(lines);
  std::vector<double> a12(lines);
  std::vector<double> q1(lines);
  std::vector<double> q2(lines);
  for (size_t p = 0; p < lines; ++p) {
    const double er = std::cos(theta[p]);
    const double ei = std::sin(theta[p]);
    a11[p] = (er * er * g11[p]) + (2.0 * er * ei * g12[p]) + (ei * ei * g22[p]);
    a22[p] = (ei * ei * g11[p]) - (2.0 * er * ei * g12[p]) + (er * er * g22[p]);
    a12[p] = (-er * ei * g11[p]) + (((er * er) - (ei * ei)) * g12[p]) +
             (er * ei * g22[p]);
    q1[p] = (er * r1[p]) + (ei * r2[p]);
    q2[p] = (-ei * r1[p]) + (er * r2[p]);
  }
  const auto window = hann_inner(kAmplitudeWindowLines);
  const auto A11 = convolve_same(a11, window);
  const auto A22 = convolve_same(a22, window);
  const auto A12 = convolve_same(a12, window);
  const auto Q1 = convolve_same(q1, window);
  const auto Q2 = convolve_same(q2, window);
  double window_sum = 0.0;
  double window_squares = 0.0;
  for (double w : window) {
    window_sum += w;
    window_squares += w * w;
  }

  // The confidence gate: |s| over its standard error, the noise taken from
  // the weighted band power (conservative), and the band-limited noise
  // correlated over about fs / 2 / bandwidth samples.
  const double fs = sample_rate_from_system(field_.system);
  const double noise_power = weighted_power / std::max(weight_sum, 1.0);
  const double correlation = (fs / 2.0) / (kBandHighHz - kBandLowHz);
  for (size_t p = 0; p < lines; ++p) {
    const double det = (A11[p] * A22[p]) - (A12[p] * A12[p]) + 1e-12;
    const std::complex<double> s((A22[p] * Q1[p] - A12[p] * Q2[p]) / det,
                                 (A11[p] * Q2[p] - A12[p] * Q1[p]) / det);
    const double se =
        std::sqrt(noise_power / std::max((A11[p] + A22[p]) / 2.0, 1e-12) *
                  (window_squares / window_sum) * correlation);
    double gate = std::clamp(
        ((std::abs(s) / se) - kMinSignificance) / kMinSignificance, 0.0, 1.0);
    gate *= std::clamp(
        ((kMaxAmplitude * 1.25) - std::abs(s)) / (kMaxAmplitude * 0.25), 0.0,
        1.0);
    if (gate <= 0.0) continue;
    result.amplitude[p] = s * gate * std::polar(1.0, theta[p]);
    ++result.lines_corrected;
    line_basis(p, carrier_offset_hz, basis);
    for (size_t i = 0; i < width_; ++i) {
      const double removed =
          (result.amplitude[p] * basis[i]).real() * mask_[(p * width_) + i];
      result.removed_energy += removed * removed;
    }
  }
  return result;
}

void FieldBeat::subtract(const BeatFit& fit, int16_t* frame_out) const {
  const size_t lines = std::min(field_offset_.size(), fit.amplitude.size());
  std::vector<std::complex<double>> basis;
  for (size_t p = 0; p < lines; ++p) {
    const std::complex<double> c = fit.amplitude[p];
    if (c == std::complex<double>(0.0, 0.0)) continue;
    line_basis(p, fit.carrier_offset_hz, basis);
    for (size_t i = 0; i < width_; ++i) {
      const size_t k = (p * width_) + i;
      const double removed = (c * basis[i]).real() * mask_[k];
      const double value = input_[k] - (removed * codes_per_ire_);
      frame_out[frame_offset_[p] + i] = static_cast<int16_t>(
          std::clamp<int64_t>(std::llround(value), INT16_MIN, INT16_MAX));
    }
  }
}

}  // namespace orc::fm_beat
