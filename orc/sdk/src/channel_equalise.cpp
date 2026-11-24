/*
 * File:        channel_equalise.cpp
 * Module:      orc-sdk-support
 * Purpose:     The inverse of the PAL LaserVision CAV edge smear, and the
 *              burst-referenced chroma gain that must follow it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/channel_equalise.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/pal_line_locked_resample.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace orc::channel_equalise {

namespace {

constexpr size_t kPalFrameLines = 625;
constexpr size_t kPalField1Lines = 313;

// The picture lines whose bursts are measured: field lines 23-310 of each
// field (frame-flat 22-309 and 334-621).
constexpr size_t kFirstBurstFieldLine = 23;
constexpr size_t kLastBurstFieldLine = 310;

double median(std::vector<double> values) {
  const size_t n = values.size();
  const size_t mid = n / 2;
  const auto middle = values.begin() + static_cast<std::ptrdiff_t>(mid);
  std::nth_element(values.begin(), middle, values.end());
  const double upper = values[mid];
  if (n % 2 == 1) return upper;
  const double lower = *std::max_element(values.begin(), middle);
  return (lower + upper) / 2.0;
}

int16_t to_sample(double value) {
  const double r = std::nearbyint(value);
  return static_cast<int16_t>(
      std::clamp(r, static_cast<double>(std::numeric_limits<int16_t>::min()),
                 static_cast<double>(std::numeric_limits<int16_t>::max())));
}

}  // namespace

namespace {

// The second-order inverse's coefficients: numerator d, denominator n.
struct SecondOrder {
  std::array<double, 3> d{};
  std::array<double, 3> n{};
};

SecondOrder second_order(const EdgeSmear& smear) {
  const double a = std::exp(-1.0 / smear.tau_samples);
  const double b = std::exp(-1.0 / smear.fast_tau_samples);
  const double c = smear.fraction;
  const double f = smear.fast_fraction;
  SecondOrder q;
  q.d = {1.0, -(a + b), a * b};
  q.n = {((1.0 - c - f) * q.d[0]) + (c * (1.0 - a)) + (f * (1.0 - b)),
         ((1.0 - c - f) * q.d[1]) - (c * (1.0 - a) * b) - (f * (1.0 - b) * a),
         (1.0 - c - f) * q.d[2]};
  return q;
}

}  // namespace

bool invertible(const EdgeSmear& smear) {
  if (smear.fast_fraction == 0.0) {
    return smear.fraction >= 0.0 && smear.fraction < 1.0 &&
           smear.tau_samples > 0.0;
  }
  if (!(smear.tau_samples > 0.0) || !(smear.fast_tau_samples > 0.0)) {
    return false;
  }
  const auto q = second_order(smear);
  if (!(q.n[0] > 0.0)) return false;
  // The roots of z^2 + p z + r (N divided by its leading coefficient).
  const double p = q.n[1] / q.n[0];
  const double r = q.n[2] / q.n[0];
  constexpr double kMaxPole = 0.98;
  const double disc = (p * p) - (4.0 * r);
  if (disc < 0.0) return std::sqrt(r) < kMaxPole;
  const double root = std::sqrt(disc);
  return std::max(std::abs((-p + root) / 2.0), std::abs((-p - root) / 2.0)) <
         kMaxPole;
}

void unsmear(std::vector<double>& stream, const EdgeSmear& smear) {
  if (!smear.active() || stream.empty()) return;
  if (smear.fast_fraction != 0.0) {
    if (!invertible(smear)) return;
    const auto q = second_order(smear);
    // n0 y[k] = d0 x[k] + d1 x[k-1] + d2 x[k-2] - n1 y[k-1] - n2 y[k-2],
    // primed at the first sample's level (the DC gain is 1).
    const double inv = 1.0 / q.n[0];
    double x1 = stream.front();
    double x2 = x1;
    double y1 = x1;
    double y2 = x1;
    for (double& s : stream) {
      const double x = s;
      const double y = ((q.d[0] * x) + (q.d[1] * x1) + (q.d[2] * x2) -
                        (q.n[1] * y1) - (q.n[2] * y2)) *
                       inv;
      x2 = x1;
      x1 = x;
      y2 = y1;
      y1 = y;
      s = y;
    }
    return;
  }
  const double c = smear.fraction;
  const double a = std::exp(-1.0 / smear.tau_samples);
  // y[n] (1 - c a) = x[n] - a x[n-1] + (1 - c) a y[n-1], primed so that a
  // constant input at the first sample's level passes unchanged (the
  // filter's DC gain is 1).
  const double scale = 1.0 / (1.0 - (c * a));
  const double feedback = (1.0 - c) * a;
  double x_prev = stream.front();
  double y_prev = stream.front();
  for (double& s : stream) {
    const double x = s;
    const double y = (x - (a * x_prev) + (feedback * y_prev)) * scale;
    x_prev = x;
    y_prev = y;
    s = y;
  }
}

std::optional<double> burst_amplitude(const std::vector<double>& frame,
                                      size_t samples_per_line,
                                      double blanking_level,
                                      double white_level) {
  const double excursion = white_level - blanking_level;
  if (excursion <= 0.0 ||
      frame.size() < frame_line_sample_offset(
                         VideoSystem::PAL, samples_per_line, kPalFrameLines)) {
    return std::nullopt;
  }
  const auto start = static_cast<size_t>(kPalColourBurstStart);
  const auto end = static_cast<size_t>(kPalColourBurstEnd);
  // Room for the largest per-row shift (2 samples) as well.
  if (end + 2 > samples_per_line) return std::nullopt;
  const double length = static_cast<double>(end - start);
  std::vector<double> amplitudes;
  for (const int field : {1, 2}) {
    const size_t field_first = field == 1 ? size_t{0} : kPalField1Lines;
    for (size_t line = kFirstBurstFieldLine; line <= kLastBurstFieldLine;
         ++line) {
      const size_t flat = field_first + line - 1;
      const double* p =
          frame.data() +
          frame_line_sample_offset(VideoSystem::PAL, samples_per_line, flat);
      // On the 4fsc lattice 0H moves up to 2 samples later through the
      // field (CVBS File Format Specification v1.8.1, Preset PAL); the
      // window moves with it so it holds the same part of the burst on
      // every row.
      const auto shift = static_cast<size_t>(
          pal_line_locked::row_0h_shift(field, static_cast<int32_t>(line - 1)));
      // The window's component at fsc, demodulated against e^(-j pi n / 2)
      // on the line's own sample numbers. 40 samples are 10 whole cycles,
      // so the line's level cancels.
      double re = 0.0;
      double im = 0.0;
      for (size_t n = start + shift; n < end + shift; ++n) {
        switch (n & 3U) {
          case 0:
            re += p[n];
            break;
          case 1:
            im -= p[n];
            break;
          case 2:
            re -= p[n];
            break;
          default:
            im += p[n];
            break;
        }
      }
      amplitudes.push_back(2.0 * std::hypot(re, im) / length / excursion);
    }
  }
  return median(std::move(amplitudes));
}

double burst_gain(double amplitude) {
  if (!(amplitude > 0.0)) return 1.0;
  const double gain = kNominalBurstAmplitude / amplitude;
  if (gain > kMaxBurstGain || gain < 1.0 / kMaxBurstGain) return 1.0;
  return gain;
}

void apply_chroma_gain(std::vector<double>& frame, size_t samples_per_line,
                       double gain) {
  if (gain == 1.0) return;
  // [1, -4, 6, -4, 1] / 4: (6 - 8 cos wT + 2 cos 2wT) / 4 = (1 - cos wT)^2.
  const double scale = (gain - 1.0) / 4.0;
  std::vector<double> in;
  for (size_t line = 0; line < kPalFrameLines; ++line) {
    const size_t offset =
        frame_line_sample_offset(VideoSystem::PAL, samples_per_line, line);
    const size_t count =
        frame_line_sample_count(VideoSystem::PAL, samples_per_line, line);
    if (offset + count > frame.size() || count < 5) return;
    in.assign(frame.begin() + static_cast<std::ptrdiff_t>(offset),
              frame.begin() + static_cast<std::ptrdiff_t>(offset + count));
    const auto at = [&](std::ptrdiff_t n) {
      return in[static_cast<size_t>(std::clamp<std::ptrdiff_t>(
          n, 0, static_cast<std::ptrdiff_t>(count) - 1))];
    };
    for (size_t n = 0; n < count; ++n) {
      const auto k = static_cast<std::ptrdiff_t>(n);
      const double hp = at(k - 2) - (4.0 * at(k - 1)) + (6.0 * in[n]) -
                        (4.0 * at(k + 1)) + at(k + 2);
      frame[offset + n] = in[n] + (scale * hp);
    }
  }
}

FrameResult equalise_frame(const int16_t* frame, size_t samples_total,
                           size_t samples_per_line, double blanking_level,
                           double white_level, const EdgeSmear& smear) {
  return equalise_frame(frame, samples_total, samples_per_line, blanking_level,
                        white_level, smear, smear);
}

FrameResult equalise_frame(const int16_t* frame, size_t samples_total,
                           size_t samples_per_line, double blanking_level,
                           double white_level, const EdgeSmear& first_field,
                           const EdgeSmear& second_field) {
  FrameResult result;
  std::vector<double> stream(frame, frame + samples_total);
  if (first_field == second_field) {
    unsmear(stream, first_field);
  } else {
    const size_t split =
        std::min(samples_total,
                 frame_line_sample_offset(VideoSystem::PAL, samples_per_line,
                                          kPalField1Lines));
    std::vector<double> first(
        stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(split));
    std::vector<double> second(
        stream.begin() + static_cast<std::ptrdiff_t>(split), stream.end());
    unsmear(first, first_field);
    unsmear(second, second_field);
    std::copy(first.begin(), first.end(), stream.begin());
    std::copy(second.begin(), second.end(),
              stream.begin() + static_cast<std::ptrdiff_t>(split));
  }
  if (const auto amplitude = burst_amplitude(stream, samples_per_line,
                                             blanking_level, white_level)) {
    result.gain = burst_gain(*amplitude);
    apply_chroma_gain(stream, samples_per_line, result.gain);
  }
  result.frame.resize(samples_total);
  std::transform(stream.begin(), stream.end(), result.frame.begin(), to_sample);
  return result;
}

}  // namespace orc::channel_equalise
