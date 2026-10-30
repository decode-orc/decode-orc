/*
 * File:        vits_edge_tail.cpp
 * Module:      orc-core
 * Purpose:     Measure the slow tail of the VITS white bar's edges
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vits_edge_tail.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/frame_line_util.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace orc {

namespace {

// How far a decode may put an edge from its nominal place: up to 0.6 µs
// either side was seen (WhiteSNRObserver), with room to spare.
constexpr double kEdgeSearchUs = 1.0;

// The fit starts this long after the 50 % crossing, past the fast part of
// the edge and the ringing the channel's band limit leaves after it.
constexpr double kFitStartUs = 0.3;

// The tail fit after a falling edge stops this long before the 2T pulse,
// whose half-amplitude width is 0.2 µs on PAL and 0.25 µs on NTSC.
constexpr double kPulseMarginUs = 0.6;

// A line's levels must lie within this share of the white-to-blanking step of
// a bar's to be averaged in.
constexpr double kLevelTolerance = 0.15;

// Time constants the fit tries, log-spaced.  None is shorter than the
// stretch the fit skips: a tail that fast has settled before the fit starts
// and cannot be told from the edge's fast part, and allowing one lets the fit
// extrapolate the noise of its first samples back to the crossing.
constexpr double kMinTimeConstantUs = kFitStartUs;
constexpr double kMaxTimeConstantUs = 4.0;
constexpr size_t kTimeConstantSteps = 200;

double mean_of(const std::vector<double>& wave, size_t from, size_t to) {
  double sum = 0.0;
  for (size_t k = from; k < to; ++k) sum += wave[k];
  return to > from ? sum / static_cast<double>(to - from) : 0.0;
}

}  // namespace

VitsEdgeTailMeter::VitsEdgeTailMeter(VideoSystem system, size_t nominal_width,
                                     double blanking, double white)
    : system_(system),
      nominal_width_(nominal_width),
      samples_per_us_(static_cast<double>(nominal_width) /
                      (system == VideoSystem::PAL ? 64.0 : 63.5)),
      blanking_(blanking),
      white_(white),
      field_line_(system == VideoSystem::PAL ? 19 : 20) {
  // From the end of the level before each edge to the next feature: after a
  // rising edge the bar's own fall, after a falling one the 2T pulse.
  const double fall_us = system == VideoSystem::PAL ? 22.0 : 30.0;
  const double pulse_us = system == VideoSystem::PAL ? 26.0 : 34.0;
  rising_.nominal_us = 12.0;
  rising_.level_from = 9.0;
  rising_.level_to = 12.0 - kEdgeSearchUs;
  rising_.fit_to = fall_us - kEdgeSearchUs;
  rising_.rising = true;
  falling_.nominal_us = fall_us;
  falling_.level_from = fall_us - 4.0;
  falling_.level_to = fall_us - kEdgeSearchUs;
  falling_.fit_to = pulse_us - kPulseMarginUs;
  falling_.rising = false;
}

size_t VitsEdgeTailMeter::sample_at(double us) const {
  return static_cast<size_t>(std::lround(us * samples_per_us_));
}

size_t VitsEdgeTailMeter::bar_line(size_t field) const {
  return (field == 0 ? 0 : field1_lines(system_)) + field_line_ - 1;
}

void VitsEdgeTailMeter::add_frame(const std::vector<int16_t>& plane,
                                  size_t height) {
  for (size_t field = 0; field < 2; ++field) {
    const size_t line = bar_line(field);
    if (line >= height) continue;
    const size_t start =
        frame_line_sample_offset(system_, nominal_width_, line);
    const size_t length =
        frame_line_sample_count(system_, nominal_width_, line);
    if (start + length > plane.size()) continue;
    add_line(plane.data() + start, length);
  }
}

void VitsEdgeTailMeter::add_line(const int16_t* line, size_t length) {
  if (nominal_width_ == 0 || white_ <= blanking_) return;
  const double step = white_ - blanking_;
  auto level = [&](double from_us, double to_us) {
    const size_t from = sample_at(from_us);
    const size_t to = std::min(sample_at(to_us), length);
    double sum = 0.0;
    for (size_t k = from; k < to; ++k) sum += line[k];
    return to > from ? sum / static_cast<double>(to - from)
                     : std::numeric_limits<double>::quiet_NaN();
  };
  auto near = [&](double value, double expected) {
    return std::abs(value - expected) < kLevelTolerance * step;
  };
  // Black before the bar, white along it, black after it.
  const double before = level(rising_.level_from, rising_.level_to);
  const double bar = level(rising_.nominal_us + kEdgeSearchUs,
                           falling_.nominal_us - kEdgeSearchUs);
  const double after =
      level(falling_.nominal_us + kEdgeSearchUs, falling_.fit_to);
  if (!near(before, blanking_) || !near(bar, white_) ||
      !near(after, blanking_)) {
    return;
  }

  for (Edge* edge : {&rising_, &falling_}) {
    const size_t from = sample_at(edge->level_from);
    const size_t to = sample_at(edge->fit_to);
    if (to > length || to <= from) continue;
    if (edge->sum.empty()) edge->sum.assign(to - from, 0.0);
    for (size_t k = from; k < to; ++k) edge->sum[k - from] += line[k];
    ++edge->lines;
  }
}

std::optional<EdgeTail> VitsEdgeTailMeter::fit(const Edge& edge) const {
  if (edge.lines == 0 || edge.sum.empty()) return std::nullopt;
  std::vector<double> wave(edge.sum.size());
  for (size_t k = 0; k < wave.size(); ++k) {
    wave[k] = edge.sum[k] / static_cast<double>(edge.lines);
  }
  auto index = [&](double us) {
    const size_t at = sample_at(us);
    const size_t origin = sample_at(edge.level_from);
    return std::min(at > origin ? at - origin : size_t{0}, wave.size());
  };

  // The levels either side, the one after from the end of the fit window,
  // where the tail has settled.
  const double before =
      mean_of(wave, index(edge.level_from), index(edge.level_to));
  const size_t settled =
      index(edge.fit_to - 0.3 * (edge.fit_to - edge.nominal_us));
  const double after = mean_of(wave, settled, wave.size());
  const double mid = 0.5 * (before + after);

  // The 50 % crossing, to a fraction of a sample.
  const size_t search_from = index(edge.nominal_us - kEdgeSearchUs);
  const size_t search_to = index(edge.nominal_us + kEdgeSearchUs);
  std::optional<double> crossing;
  for (size_t k = search_from; k + 1 < search_to; ++k) {
    const double a = wave[k] - mid;
    const double b = wave[k + 1] - mid;
    if ((edge.rising ? (a < 0.0 && b >= 0.0) : (a > 0.0 && b <= 0.0))) {
      crossing = static_cast<double>(k) + a / (a - b);
      break;
    }
  }
  if (!crossing) return std::nullopt;

  // wave = A − B·exp(−t/τ) after the crossing, A and B by least squares at
  // each τ tried, t from the crossing.
  const auto first =
      static_cast<size_t>(std::ceil(*crossing + kFitStartUs * samples_per_us_));
  if (first + 8 >= wave.size()) return std::nullopt;
  double best_error = std::numeric_limits<double>::infinity();
  double best_a = 0.0;
  double best_b = 0.0;
  double best_tau = 0.0;
  const double ratio = std::pow(kMaxTimeConstantUs / kMinTimeConstantUs,
                                1.0 / (kTimeConstantSteps - 1));
  double tau = kMinTimeConstantUs;
  for (size_t step = 0; step < kTimeConstantSteps; ++step, tau *= ratio) {
    double s1 = 0.0, se = 0.0, see = 0.0, sy = 0.0, sey = 0.0, syy = 0.0;
    for (size_t k = first; k < wave.size(); ++k) {
      const double t = (static_cast<double>(k) - *crossing) / samples_per_us_;
      const double e = -std::exp(-t / tau);
      s1 += 1.0;
      se += e;
      see += e * e;
      sy += wave[k];
      sey += e * wave[k];
      syy += wave[k] * wave[k];
    }
    const double det = s1 * see - se * se;
    if (std::abs(det) < 1e-12) continue;
    const double a = (see * sy - se * sey) / det;
    const double b = (s1 * sey - se * sy) / det;
    const double error = syy - a * sy - b * sey;
    if (error < best_error) {
      best_error = error;
      best_a = a;
      best_b = b;
      best_tau = tau;
    }
  }
  if (!std::isfinite(best_error) || best_a == before) return std::nullopt;

  EdgeTail tail;
  tail.fraction = best_b / (best_a - before);
  if (std::abs(tail.fraction) >= kMinReportedEdgeTail) {
    tail.time_constant_us = best_tau;
  }
  tail.lines = edge.lines;
  return tail;
}

EdgeTailMeasurement VitsEdgeTailMeter::measure() const {
  return {fit(rising_), fit(falling_)};
}

}  // namespace orc
