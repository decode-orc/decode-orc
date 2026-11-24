/*
 * File:        window_select.cpp
 * Module:      orc-stage-plugin-channel-equalise
 * Purpose:     Choosing, for a window of frames, a correction that is never
 *              worse than the upstream per-source one
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "window_select.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <random>

namespace orc::window_select {

namespace {

namespace ce = channel_equalise;

constexpr double kSamplesPerUs = 4.0 * 4433618.75 / 1.0e6;
// The rising edge's index in an aligned segment (it starts 3 us before).
const size_t kEdge = static_cast<size_t>(std::lround(3.0 * kSamplesPerUs));

constexpr double kTailTolerance = 0.001;
constexpr double kNoiseTolerance = 0.01;
constexpr double kFloor = 0.001;
constexpr double kPenalty = 50.0;

double median(std::vector<double> v) {
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

double slice_median(const std::vector<double>& y, size_t first, size_t last) {
  if (first >= last || last > y.size()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return median({y.begin() + static_cast<std::ptrdiff_t>(first),
                 y.begin() + static_cast<std::ptrdiff_t>(last)});
}

double sample_at(const std::vector<double>& y, double x) {
  if (x <= 0.0) return y.front();
  const auto i = static_cast<size_t>(std::floor(x));
  if (i + 1 >= y.size()) return y.back();
  const double f = x - static_cast<double>(i);
  return y[i] + (f * (y[i + 1] - y[i]));
}

double mean_at(const std::vector<double>& y, double at, int first, int last) {
  double s = 0.0;
  for (int k = first; k <= last; ++k) s += sample_at(y, at + k);
  return s / static_cast<double>(last - first + 1);
}

// The bar's tails of a corrected mean segment, and where its fall is.
struct BarTails {
  std::array<double, 4> tails{};  // rise 3-8, fall 3-8, rise 9-30, fall 9-30
  double fall = 0.0;
  double height = 0.0;
};

std::optional<BarTails> bar_tails(const std::vector<double>& y) {
  if (y.size() < kEdge + 60 || kEdge < 13) return std::nullopt;
  const double base = slice_median(y, 0, kEdge - 12);
  size_t fall_index = 0;
  for (size_t i = kEdge + 20; i < y.size(); ++i) {
    if (y[i] < base + 50.0) {
      fall_index = i;
      break;
    }
  }
  if (fall_index < kEdge + 42) return std::nullopt;
  const double top = slice_median(y, kEdge + 20, fall_index - 20);
  const double height = top - base;
  if (!(height > 0.0)) return std::nullopt;
  const double mid = base + (height / 2.0);
  size_t j = fall_index - 1;
  while (j > kEdge && y[j] < mid) --j;
  const double fall =
      static_cast<double>(j) + ((y[j] - mid) / (y[j] - y[j + 1]));
  const auto whole = static_cast<size_t>(fall);
  if (whole + 60 > y.size()) return std::nullopt;
  const double post = slice_median(y, whole + 20, whole + 34);
  const auto edge = static_cast<double>(kEdge);
  BarTails t;
  t.tails = {std::abs((top - mean_at(y, edge, 3, 8)) / height),
             std::abs((mean_at(y, fall, 3, 8) - post) / height),
             std::abs((top - mean_at(y, edge, 9, 30)) / height),
             std::abs((mean_at(y, fall, 9, 30) - post) / height)};
  t.fall = fall;
  t.height = height;
  return t;
}

// A split's data through one correction: each parity's corrected segments
// and the edges' corrected profiles.
struct Prepared {
  std::array<std::vector<std::vector<double>>, 2> segments;
  picture_edges::Profiles profiles;
};

Prepared prepare(const Split& split, const Correction& correction) {
  Prepared p;
  for (size_t parity = 0; parity < 2; ++parity) {
    for (const auto& s : split.segments[parity]) {
      auto y = s;
      ce::unsmear(y, correction[parity]);
      p.segments[parity].push_back(std::move(y));
    }
  }
  p.profiles = picture_edges::profiles(split.edges, correction);
  return p;
}

bool has_frames(const Split& split) {
  return split.segment_frames[0].size() == split.segments[0].size() &&
         split.segment_frames[1].size() == split.segments[1].size() &&
         split.edge_frames.size() == split.edges.size();
}

// Each field and edge through its own frame's correction (has_frames()).
Prepared prepare(const Split& split, const CorrectionAt& correction) {
  Prepared p;
  for (size_t parity = 0; parity < 2; ++parity) {
    const auto& rows = split.segments[parity];
    for (size_t i = 0; i < rows.size(); ++i) {
      auto y = rows[i];
      ce::unsmear(y, correction(split.segment_frames[parity][i])[parity]);
      p.segments[parity].push_back(std::move(y));
    }
  }
  p.profiles = picture_edges::profiles_each(split.edges, [&](size_t e) {
    return correction(split.edge_frames[e])[split.edges.field[e] == 0 ? 0 : 1];
  });
  return p;
}

// Resampling indices: per parity into the segments, and into the edges.
struct Draw {
  std::array<std::vector<size_t>, 2> fields;
  std::vector<size_t> edges;
};

Draw everything(const Prepared& p) {
  Draw d;
  for (size_t parity = 0; parity < 2; ++parity) {
    for (size_t i = 0; i < p.segments[parity].size(); ++i) {
      d.fields[parity].push_back(i);
    }
  }
  return d;  // empty edges: every edge
}

Measures measure_prepared(const Prepared& p, const Draw& draw) {
  Measures m;
  constexpr size_t kMinFields = 8;  // as a robust average needs
  for (size_t parity = 0; parity < 2; ++parity) {
    const auto& rows = draw.fields[parity];
    if (rows.size() < kMinFields) continue;
    const size_t length = p.segments[parity][rows[0]].size();
    std::vector<double> mean(length, 0.0);
    for (const size_t r : rows) {
      for (size_t i = 0; i < length; ++i) mean[i] += p.segments[parity][r][i];
    }
    for (double& v : mean) v /= static_cast<double>(rows.size());
    const auto t = bar_tails(mean);
    if (!t) continue;
    const size_t base = parity * kPerParity;
    for (size_t k = 0; k < 4; ++k) {
      m.value[base + k] = t->tails[k];
      m.present[base + k] = true;
    }
    const auto whole = static_cast<size_t>(t->fall);
    std::vector<size_t> flat;
    for (size_t i = kEdge + 30; i + 30 < whole; ++i) flat.push_back(i);
    for (size_t i = whole + 20; i < whole + 60 && i < length; ++i) {
      flat.push_back(i);
    }
    std::vector<double> energy;
    for (const size_t r : rows) {
      const auto& s = p.segments[parity][r];
      double sum = 0.0;
      for (const size_t i : flat) sum += s[i] - mean[i];
      const double offset = sum / static_cast<double>(flat.size());
      double e = 0.0;
      for (const size_t i : flat) {
        const double d = s[i] - mean[i] - offset;
        e += d * d;
      }
      energy.push_back(e / static_cast<double>(flat.size()));
    }
    m.value[base + kNoise] = std::sqrt(median(std::move(energy))) / t->height;
    m.present[base + kNoise] = true;
  }
  if (const auto t = picture_edges::tails(p.profiles, draw.edges)) {
    m.value[kPicture3_8] = std::abs(t->t3_8);
    m.value[kPicture9_20] = std::abs(t->t9_20);
    m.present[kPicture3_8] = true;
    m.present[kPicture9_20] = true;
  }
  return m;
}

double quantile(std::vector<double> v, double q) {
  std::sort(v.begin(), v.end());
  const double pos = q * static_cast<double>(v.size() - 1);
  const auto i = static_cast<size_t>(std::floor(pos));
  const double f = pos - static_cast<double>(i);
  if (i + 1 >= v.size()) return v.back();
  return v[i] + (f * (v[i + 1] - v[i]));
}

bool in_ranges(const ce::EdgeSmear& s) {
  return s.fraction >= 0.0 && s.fraction <= ce::kMaxFraction &&
         s.fast_fraction >= ce::kMinFastFraction &&
         s.fast_fraction <= ce::kMaxFastFraction &&
         s.fraction + s.fast_fraction < 0.6 &&
         s.fast_tau_samples >= ce::kMinFastTauSamples &&
         s.fast_tau_samples <= ce::kMaxFastTauSamples && s.tau_samples >= 2.5 &&
         s.tau_samples <= ce::kMaxTauSamples &&
         s.tau_samples >= 1.5 * s.fast_tau_samples && ce::invertible(s);
}

// Nelder-Mead in any dimension (scipy's defaults: reflection 1, expansion
// 2, contraction 0.5, shrink 0.5; first simplex 5 % along each non-zero
// coordinate, 0.00025 along a zero one).
std::vector<double> nelder_mead(
    const std::function<double(const std::vector<double>&)>& f,
    const std::vector<double>& start, double x_tolerance, double f_tolerance,
    int max_iterations, double* best_value) {
  const size_t dim = start.size();
  std::vector<std::vector<double>> x(dim + 1, start);
  std::vector<double> v(dim + 1);
  for (size_t i = 0; i < dim; ++i) {
    x[i + 1][i] = start[i] != 0.0 ? 1.05 * start[i] : 0.00025;
  }
  for (size_t i = 0; i <= dim; ++i) v[i] = f(x[i]);
  for (int it = 0; it < max_iterations; ++it) {
    std::vector<size_t> order(dim + 1);
    for (size_t i = 0; i <= dim; ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&v](size_t a, size_t b) { return v[a] < v[b]; });
    auto xs = x;
    auto vs = v;
    for (size_t i = 0; i <= dim; ++i) {
      x[i] = xs[order[i]];
      v[i] = vs[order[i]];
    }
    double xs_spread = 0.0;
    double fs_spread = 0.0;
    for (size_t i = 1; i <= dim; ++i) {
      for (size_t k = 0; k < dim; ++k) {
        xs_spread = std::max(xs_spread, std::abs(x[i][k] - x[0][k]));
      }
      fs_spread = std::max(fs_spread, std::abs(v[i] - v[0]));
    }
    if (xs_spread <= x_tolerance && fs_spread <= f_tolerance) break;
    std::vector<double> centroid(dim, 0.0);
    for (size_t i = 0; i < dim; ++i) {
      for (size_t k = 0; k < dim; ++k) {
        centroid[k] += x[i][k] / static_cast<double>(dim);
      }
    }
    auto along = [&](double t) {
      std::vector<double> p(dim);
      for (size_t k = 0; k < dim; ++k) {
        p[k] = centroid[k] + (t * (x[dim][k] - centroid[k]));
      }
      return p;
    };
    const auto reflected = along(-1.0);
    const double fr = f(reflected);
    if (fr < v[0]) {
      const auto expanded = along(-2.0);
      const double fe = f(expanded);
      x[dim] = fe < fr ? expanded : reflected;
      v[dim] = std::min(fe, fr);
      continue;
    }
    if (fr < v[dim - 1]) {
      x[dim] = reflected;
      v[dim] = fr;
      continue;
    }
    bool shrink = false;
    const auto contracted = along(fr < v[dim] ? -0.5 : 0.5);
    const double fc = f(contracted);
    if ((fr < v[dim] && fc <= fr) || (fr >= v[dim] && fc < v[dim])) {
      x[dim] = contracted;
      v[dim] = fc;
    } else {
      shrink = true;
    }
    if (shrink) {
      for (size_t i = 1; i <= dim; ++i) {
        for (size_t k = 0; k < dim; ++k) {
          x[i][k] = x[0][k] + (0.5 * (x[i][k] - x[0][k]));
        }
        v[i] = f(x[i]);
      }
    }
  }
  const auto best =
      static_cast<size_t>(std::min_element(v.begin(), v.end()) - v.begin());
  if (best_value) *best_value = v[best];
  return x[best];
}

}  // namespace

bool is_noise(size_t measure) {
  return measure == kNoise || measure == kPerParity + kNoise;
}

Measures measure(const Split& split, const Correction& correction) {
  const auto p = prepare(split, correction);
  return measure_prepared(p, everything(p));
}

Measures measure(const Split& split, const CorrectionAt& correction) {
  if (!has_frames(split)) return {};
  const auto p = prepare(split, correction);
  return measure_prepared(p, everything(p));
}

namespace {

// certified() on data already through the candidate (|pc|), upstream's
// correction (|pu|) and the identity (|pi|).
bool certified_prepared(const Prepared& pc, const Prepared& pu,
                        const Prepared& pi, bool against_identity,
                        int resamples, uint32_t seed) {
  const auto full_u = measure_prepared(pu, everything(pu));
  std::mt19937 rng(seed);
  std::array<std::vector<double>, kMeasures> vs_upstream;
  std::array<std::vector<double>, kMeasures> vs_identity;
  bool compared = false;
  for (int b = 0; b < resamples; ++b) {
    Draw d;
    for (size_t parity = 0; parity < 2; ++parity) {
      const size_t n = pc.segments[parity].size();
      if (n == 0) continue;
      std::uniform_int_distribution<size_t> pick(0, n - 1);
      for (size_t i = 0; i < n; ++i) d.fields[parity].push_back(pick(rng));
    }
    const size_t edges = pc.profiles.size();
    if (edges > 0) {
      std::uniform_int_distribution<size_t> pick(0, edges - 1);
      for (size_t i = 0; i < edges; ++i) d.edges.push_back(pick(rng));
    }
    const auto mc = measure_prepared(pc, d);
    const auto mu = measure_prepared(pu, d);
    const auto mi = measure_prepared(pi, d);
    for (size_t k = 0; k < kMeasures; ++k) {
      const bool against_i = against_identity && !is_noise(k);
      // A measure the reference yields and the candidate does not is a
      // failure, not a measure to skip.
      if (!mc.present[k]) {
        if (mu.present[k] || (against_i && mi.present[k])) return false;
        continue;
      }
      if (mu.present[k]) {
        vs_upstream[k].push_back(mc.value[k] - mu.value[k]);
        compared = true;
      }
      if (against_i && mi.present[k]) {
        vs_identity[k].push_back(mc.value[k] - mi.value[k]);
        compared = true;
      }
    }
  }
  // Nothing measurable is no evidence.
  if (!compared) return false;
  for (size_t k = 0; k < kMeasures; ++k) {
    const double tolerance =
        is_noise(k) ? kNoiseTolerance * full_u.value[k] : kTailTolerance;
    if (!vs_upstream[k].empty() &&
        quantile(vs_upstream[k], 0.975) > tolerance) {
      return false;
    }
    if (!vs_identity[k].empty() &&
        quantile(vs_identity[k], 0.975) > kTailTolerance) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool certified(const Split& split, const Correction& candidate,
               const Correction& upstream, int resamples, uint32_t seed) {
  return certified_prepared(prepare(split, candidate), prepare(split, upstream),
                            prepare(split, Correction{}), true, resamples,
                            seed);
}

bool certified(const Split& split, const CorrectionAt& candidate,
               const Correction& upstream, bool against_identity, int resamples,
               uint32_t seed) {
  if (!has_frames(split)) return false;
  return certified_prepared(prepare(split, candidate), prepare(split, upstream),
                            prepare(split, Correction{}), against_identity,
                            resamples, seed);
}

std::optional<Correction> constrained_search(
    const Split& split, const Correction& upstream,
    const std::vector<Correction>& starts) {
  const auto mu = measure(split, upstream);
  const auto mi = measure(split, Correction{});
  constexpr double kInfeasible = 1e9;
  auto correction_of = [](const std::vector<double>& q) {
    Correction c;
    for (size_t parity = 0; parity < 2; ++parity) {
      const size_t o = 4 * parity;
      c[parity] = {q[o + 2], std::exp(q[o + 3]), q[o], std::exp(q[o + 1])};
    }
    return c;
  };
  auto cost = [&](const std::vector<double>& q) {
    const auto c = correction_of(q);
    for (const auto& s : c) {
      if (!in_ranges(s)) return kInfeasible;
    }
    const auto m = measure(split, c);
    double j = 0.0;
    bool any = false;
    for (size_t k = 0; k < kMeasures; ++k) {
      if (!m.present[k]) {
        // Making a measure disappear is no way to improve on it.
        if (mu.present[k] || (!is_noise(k) && mi.present[k])) {
          return kInfeasible;
        }
        continue;
      }
      any = true;
      const double v = m.value[k];
      const double reference =
          mu.present[k]
              ? (is_noise(k) ? mu.value[k] : std::max(mu.value[k], kFloor))
              : std::max(v, kFloor);
      j += (v / reference) + (kPenalty * std::max(0.0, (v / reference) - 1.0));
      if (!is_noise(k) && mi.present[k]) {
        j += kPenalty *
             std::max(0.0, (v - mi.value[k]) / std::max(mi.value[k], kFloor));
      }
    }
    return any ? j : kInfeasible;
  };
  double best = kInfeasible;
  std::vector<double> best_q;
  for (const auto& start : starts) {
    std::vector<double> q0;
    for (size_t parity = 0; parity < 2; ++parity) {
      const auto& s = start[parity];
      const double fast_tau = std::clamp(s.fast_tau_samples, 0.55, 4.4);
      const double tau =
          std::clamp(s.tau_samples, std::max(2.6, 1.6 * fast_tau), 49.0);
      q0.insert(q0.end(), {s.fast_fraction, std::log(fast_tau), s.fraction,
                           std::log(tau)});
    }
    if (!(cost(q0) < kInfeasible)) continue;
    double value = kInfeasible;
    const auto q = nelder_mead(cost, q0, 1e-4, 1e-6, 1200, &value);
    if (value < best) {
      best = value;
      best_q = q;
    }
  }
  if (best_q.empty()) return std::nullopt;
  return correction_of(best_q);
}

Choice choose(const Split& fit, const Split& select, const Correction& upstream,
              const std::optional<Correction>& tracked) {
  std::vector<Choice> candidates;
  const bool upstream_active = upstream[0].active() || upstream[1].active();
  if (upstream_active) {
    std::vector<Correction> starts;
    if (tracked && (*tracked)[0].active() && (*tracked)[1].active()) {
      starts.push_back(*tracked);
    }
    starts.push_back(upstream);
    if (const auto c = constrained_search(fit, upstream, starts)) {
      candidates.push_back({"constrained", *c});
    }
  }
  if (tracked && ((*tracked)[0].active() || (*tracked)[1].active())) {
    candidates.push_back({"tracked", *tracked});
  }
  for (const auto& c : candidates) {
    if (certified(select, c.correction, upstream)) return c;
  }
  return {"upstream", upstream};
}

}  // namespace orc::window_select
