/*
 * File:        edge_ring_certify.cpp
 * Module:      orc-stage-plugin-edge-ring-correct
 * Purpose:     Held-out certification of an edge-ring correction
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "edge_ring_certify.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

namespace orc::edge_ring {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
// Flat: the smoothed luma's gradient below this (0.5 IRE a sample).
constexpr double kFlatGradient = 0.005;
// A frame's own measure needs this many of the features it is taken on.
constexpr int kMinFrameFeatures = 5;
// A component is certified only with this many measured features.
constexpr int kMinSteps = 30;
constexpr int kMinLines = 40;

std::vector<double> gaussian_kernel(double sigma) {
  const int radius = static_cast<int>(4.0 * sigma + 0.5);
  std::vector<double> k(2 * radius + 1);
  double s = 0.0;
  for (int i = -radius; i <= radius; ++i) {
    s += k[i + radius] = std::exp(-0.5 * i * i / (sigma * sigma));
  }
  for (auto& v : k) v /= s;
  return k;
}

int mirror(int j, int n) {
  while (j < 0 || j >= n) {
    if (j < 0) j = -j - 1;
    if (j >= n) j = 2 * n - j - 1;
  }
  return j;
}

// Gaussian smoothing along lines (sigma_x) and down the field (sigma_y,
// 0 for none), mirrored at the ends.
FieldImage blur(const FieldImage& f, double sigma_y, double sigma_x) {
  FieldImage a = f;
  const auto kx = gaussian_kernel(sigma_x);
  const int rx = static_cast<int>(kx.size() / 2);
  for (int r = 0; r < f.rows; ++r) {
    const double* x = f.row(r);
    double* o = a.row(r);
    for (int i = 0; i < f.width; ++i) {
      double s = 0.0;
      for (int k = -rx; k <= rx; ++k)
        s += kx[k + rx] * x[mirror(i + k, f.width)];
      o[i] = s;
    }
  }
  if (sigma_y <= 0.0) return a;
  FieldImage b = a;
  const auto ky = gaussian_kernel(sigma_y);
  const int ry = static_cast<int>(ky.size() / 2);
  for (int r = 0; r < f.rows; ++r) {
    double* o = b.row(r);
    for (int i = 0; i < f.width; ++i) {
      double s = 0.0;
      for (int k = -ry; k <= ry; ++k)
        s += ky[k + ry] * a.row(mirror(r + k, f.rows))[i];
      o[i] = s;
    }
  }
  return b;
}

FieldImage comb_luma(const FieldImage& f) {
  FieldImage y = f;
  for (int r = 2; r < f.rows - 2; ++r) {
    for (int i = 0; i < f.width; ++i) {
      y.row(r)[i] =
          0.25 * (f.row(r - 2)[i] + 2.0 * f.row(r)[i] + f.row(r + 2)[i]);
    }
  }
  return y;
}

double percentile(std::vector<double> v, double q) {
  v.erase(std::remove_if(v.begin(), v.end(),
                         [](double x) { return !std::isfinite(x); }),
          v.end());
  if (v.empty()) return kNaN;
  std::sort(v.begin(), v.end());
  const double pos = q / 100.0 * (v.size() - 1);
  const size_t i = static_cast<size_t>(std::floor(pos));
  const double f = pos - i;
  return i + 1 < v.size() ? v[i] * (1.0 - f) + v[i + 1] * f : v[i];
}

// One frame's per-measure value and weight (the features it is taken on).
struct FrameMeasures {
  std::map<std::string, std::pair<double, double>> v;
  FlatStats flat;
};

// 1-sample bins of a 1/kUp-sample table: their mean times and sums.
template <size_t N>
void coarse(const std::array<double, N>& sum,
            const std::array<double, N>& count, double t0,
            std::vector<double>& times, std::vector<double>& mean) {
  const size_t groups = N / kUp;
  times.assign(groups, 0.0);
  mean.assign(groups, kNaN);
  for (size_t g = 0; g < groups; ++g) {
    double s = 0.0, c = 0.0, t = 0.0;
    for (int k = 0; k < kUp; ++k) {
      const size_t j = g * kUp + k;
      s += sum[j];
      c += count[j];
      t += t0 + static_cast<double>(j) / kUp;
    }
    times[g] = t / kUp;
    if (c > 0) mean[g] = s / c;
  }
}

double rms_over(const std::vector<double>& t, const std::vector<double>& m,
                double lo, double hi) {
  double s = 0.0;
  int n = 0;
  for (size_t g = 0; g < t.size(); ++g) {
    if (t[g] < lo || t[g] > hi || !std::isfinite(m[g])) continue;
    s += m[g] * m[g];
    ++n;
  }
  return n ? 1000.0 * std::sqrt(s / n) : kNaN;
}

double mean_over(const std::vector<double>& t, const std::vector<double>& m,
                 double lo, double hi) {
  double s = 0.0;
  int n = 0;
  for (size_t g = 0; g < t.size(); ++g) {
    if (t[g] < lo || t[g] > hi || !std::isfinite(m[g])) continue;
    s += m[g];
    ++n;
  }
  return n ? 1000.0 * std::abs(s / n) : kNaN;
}

FrameMeasures frame_measures(const ProfileSums& p, const FlatStats& flat) {
  FrameMeasures out;
  out.flat = flat;
  std::vector<double> t, m;
  auto steps = [&](const StepTable& sum, const StepTable& count, int n,
                   const std::string& name) {
    coarse(sum, count, -kPre, t, m);
    const bool ok = n >= kMinFrameFeatures;
    out.v[name + "_post"] = {ok ? rms_over(t, m, 2, 12) : kNaN, n};
    out.v[name + "_pre"] = {ok ? rms_over(t, m, -8, -2) : kNaN, n};
    out.v[name + "_tail"] = {ok ? mean_over(t, m, 3, 8) : kNaN, n};
  };
  steps(p.rise_sum, p.rise_count, p.rises, "rise");
  steps(p.fill_sum, p.fill_count, p.fills, "fill");
  steps(p.fall_sum, p.fall_count, p.falls, "fall");
  coarse(p.line_sum, p.line_count, 0.0, t, m);
  const bool ok = p.lines >= kMinFrameFeatures;
  out.v["halo"] = {ok ? rms_over(t, m, 2, 10) : kNaN, p.lines};
  out.v["halo3"] = {ok ? mean_over(t, m, 2.5, 3.5) : kNaN, p.lines};
  return out;
}

// The weighted aggregate of every measure over frames.
std::map<std::string, double> aggregate(const std::vector<FrameMeasures>& f,
                                        const std::vector<double>& w) {
  std::map<std::string, double> out;
  for (const auto& [name, unused] : f.front().v) {
    double num = 0.0, den = 0.0;
    for (size_t k = 0; k < f.size(); ++k) {
      const auto& [v, n] = f[k].v.at(name);
      if (!std::isfinite(v)) continue;
      num += w[k] * n * v;
      den += w[k] * n;
    }
    out[name] = den > 0 ? num / den : kNaN;
  }
  double ns = 0.0, nc = 0.0, ls = 0.0, lc = 0.0;
  for (size_t k = 0; k < f.size(); ++k) {
    ns += w[k] * f[k].flat.noise_sum;
    nc += w[k] * f[k].flat.noise_count;
    ls += w[k] * f[k].flat.level_sum;
    lc += w[k] * f[k].flat.level_count;
  }
  out["noise_db"] = 10.0 * std::log10(std::max(ns, 1e-30) / std::max(nc, 1.0));
  out["level"] = ls / std::max(lc, 1.0);
  return out;
}

FlatStats operator+(FlatStats a, const FlatStats& b) {
  a.noise_sum += b.noise_sum;
  a.noise_count += b.noise_count;
  a.level_sum += b.level_sum;
  a.level_count += b.level_count;
  return a;
}

}  // namespace

CertifyField::CertifyField(FieldImage field) : field_(std::move(field)) {
  events_ = find_events(field_);
  measured_ = measured_features(field_, events_);
  before_ = measure(field_, measured_);
  before_.events = static_cast<int>(events_.size());
  before_.pairs = count_pairs(events_);
  luma_before_ = comb_luma(field_);
  const FieldImage smooth = blur(luma_before_, 0.7, 2.0);
  const int rows = field_.rows, width = field_.width;
  flat_.assign(static_cast<size_t>(rows) * width, 0);
  for (int r = kFirstRow; r < std::min(kLastRow, rows); ++r) {
    for (int i = kFirstSample; i < std::min(kLastSample, width); ++i) {
      const double* s = smooth.row(r);
      const double gx = i == 0           ? s[1] - s[0]
                        : i == width - 1 ? s[i] - s[i - 1]
                                         : 0.5 * (s[i + 1] - s[i - 1]);
      const double gy =
          r == 0          ? smooth.row(1)[i] - s[i]
          : r == rows - 1 ? s[i] - smooth.row(r - 1)[i]
                          : 0.5 * (smooth.row(r + 1)[i] - smooth.row(r - 1)[i]);
      flat_[static_cast<size_t>(r) * width + i] =
          std::abs(gx) + std::abs(gy) < kFlatGradient;
    }
  }
  // 5 x 17 erosion, outside the field counting as not flat
  large_flat_.assign(flat_.size(), 0);
  for (int r = 2; r < rows - 2; ++r) {
    for (int i = 8; i < width - 8; ++i) {
      bool all = true;
      for (int dr = -2; dr <= 2 && all; ++dr) {
        for (int di = -8; di <= 8 && all; ++di) {
          all = flat_[static_cast<size_t>(r + dr) * width + i + di] != 0;
        }
      }
      large_flat_[static_cast<size_t>(r) * width + i] = all;
    }
  }
  flat_before_ = flat_stats(luma_before_);
}

FlatStats CertifyField::flat_stats(const FieldImage& luma) const {
  FlatStats s;
  const FieldImage low = blur(luma, 0.0, 2.0);
  for (size_t j = 0; j < luma.v.size(); ++j) {
    if (flat_[j]) {
      const double hp = luma.v[j] - low.v[j];
      s.noise_sum += hp * hp;
      s.noise_count += 1;
    }
    if (large_flat_[j]) {
      s.level_sum += luma.v[j] - luma_before_.v[j];
      s.level_count += 1;
    }
  }
  return s;
}

std::pair<ProfileSums, FlatStats> CertifyField::after(
    const WindowCorrection& window, const Gains& gains, bool line_art) const {
  FieldImage out = field_;
  apply(correction_features(field_, events_, pair_mode(line_art, gains)),
        window, gains, out);
  return {measure(out, measured_), flat_stats(comb_luma(out))};
}

Comparison compare(const std::vector<CertifyField>& fields,
                   const WindowCorrection* window, uint64_t seed) {
  Comparison c;
  const size_t frames = fields.size() / 2;
  std::vector<FrameMeasures> before, after;
  for (size_t k = 0; k < frames; ++k) {
    ProfileSums pb, pa;
    FlatStats fb, fa;
    for (int p = 0; p < 2; ++p) {
      const auto& f = fields[2 * k + p];
      pb += f.before();
      fb = fb + f.flat_before();
      c.rises += f.before().rises;
      c.fills += f.before().fills;
      c.falls += f.before().falls;
      c.lines += f.before().lines;
      if (window == nullptr) {
        pa += f.before();
        fa = fa + f.flat_before();
        continue;
      }
      ProfileSums frame = fields[2 * k].before();
      frame += fields[2 * k + 1].before();
      const bool line_art = is_line_art(frame);
      Gains g =
          window->adapt
              ? frame_gains(*window, fields[2 * k + 1 - p].before(), line_art)
              : window->gains;
      if (!line_art) g.line = 0.0;
      const auto [s, fl] = f.after(*window, g, line_art);
      pa += s;
      fa = fa + fl;
    }
    if (is_line_art(pb)) ++c.line_art_frames;
    before.push_back(frame_measures(pb, fb));
    after.push_back(frame_measures(pa, fa));
  }
  if (frames == 0) return c;
  // per frame: the summed change of the edge measures it has
  int with_edges = 0, improved = 0;
  for (size_t k = 0; k < frames; ++k) {
    double sum = 0.0;
    bool any = false;
    for (const char* name : kEdgeMeasures) {
      const double b = before[k].v.at(name).first;
      const double a = after[k].v.at(name).first;
      if (std::isfinite(a - b)) {
        sum += a - b;
        any = true;
      }
    }
    if (any) {
      ++with_edges;
      improved += sum < 0.0;
    }
  }
  c.frame_share = with_edges ? static_cast<double>(improved) / with_edges : 0.0;
  const std::vector<double> ones(frames, 1.0);
  const auto pb = aggregate(before, ones);
  const auto pa = aggregate(after, ones);
  std::map<std::string, std::vector<double>> draws;
  std::vector<double> total(kBootstrapDraws, 0.0);
  std::vector<bool> any_total(kBootstrapDraws, false);
  std::mt19937_64 rng(seed);
  std::uniform_int_distribution<size_t> pick(0, frames - 1);
  for (int d = 0; d < kBootstrapDraws; ++d) {
    std::vector<double> w(frames, 0.0);
    for (size_t k = 0; k < frames; ++k) w[pick(rng)] += 1.0;
    const auto b = aggregate(before, w);
    const auto a = aggregate(after, w);
    for (const auto& [name, v] : b) draws[name].push_back(a.at(name) - v);
    for (const char* name : kEdgeMeasures) {
      const double x = a.at(name) - b.at(name);
      if (std::isfinite(x)) {
        total[d] += x;
        any_total[d] = true;
      }
    }
  }
  for (const auto& [name, v] : draws) {
    MeasureComparison m;
    m.before = pb.at(name);
    m.after = pa.at(name);
    m.measured = std::isfinite(m.before);
    m.low = percentile(v, 2.5);
    m.high = percentile(v, 97.5);
    c.measures[name] = m;
  }
  std::vector<double> t;
  for (int d = 0; d < kBootstrapDraws; ++d) {
    if (any_total[d]) t.push_back(total[d]);
  }
  c.gain_bound = t.empty() ? kNaN : percentile(t, 97.5);
  return c;
}

Certificate certify(const std::vector<CertifyField>& fields,
                    const WindowCorrection& window) {
  Certificate cert;
  cert.comparison = compare(fields, &window, 1);
  const auto& c = cert.comparison;
  const bool measurable_rise = c.rises >= kMinSteps;
  const bool measurable_fill = c.fills >= kMinSteps;
  const bool measurable_fall = c.falls >= kMinSteps;
  const bool measurable_line = c.lines >= kMinLines;
  if (window.gains.rise != 0 && !measurable_rise) {
    cert.reasons.push_back("isolated rising steps not measured");
  }
  if (window.gains.fill != 0 && !measurable_fill) {
    cert.reasons.push_back("fill-edge rising steps not measured");
  }
  if (window.gains.fall != 0 && !measurable_fall) {
    cert.reasons.push_back("falling steps not measured");
  }
  if (window.gains.line != 0 && !measurable_line)
    cert.reasons.push_back("thin lines not measured");
  if (window.gains.line != 0 && c.line_art_frames < kMinLineArtFrames) {
    cert.reasons.push_back("too few line-art frames for the line term");
  }
  bool any = false;
  for (const char* name : kEdgeMeasures) {
    const std::string n(name);
    const bool ok = n.rfind("rise", 0) == 0   ? measurable_rise
                    : n.rfind("fill", 0) == 0 ? measurable_fill
                    : n.rfind("fall", 0) == 0 ? measurable_fall
                                              : measurable_line;
    if (!ok) continue;
    const auto& m = c.measures.at(n);
    if (!std::isfinite(m.high)) continue;
    any = true;
    if (m.high > kEdgeBand) cert.reasons.push_back(n + " may be worse");
  }
  if (c.measures.at("noise_db").high > kNoiseBandDb)
    cert.reasons.push_back("noise may be higher");
  const auto& lv = c.measures.at("level");
  if (std::max(-lv.low, lv.high) > kLevelBand)
    cert.reasons.push_back("levels may change");
  if (!any) {
    cert.reasons.push_back("nothing measurable");
  } else if (!(c.gain_bound < -kMinGain)) {
    cert.reasons.push_back("no certain gain");
  } else if (c.frame_share < kMinFrameShare) {
    cert.reasons.push_back("better on too few frames");
  }
  cert.accepted = cert.reasons.empty();
  return cert;
}

std::vector<WindowCorrection> candidates(const WindowCorrection& fitted) {
  // components in the order rise, fall, line, fill; every non-empty subset of
  // those fitted, largest first, in itertools.combinations order
  constexpr int kComponents = 4;
  const bool has[kComponents] = {fitted.gains.rise != 0, fitted.gains.fall != 0,
                                 fitted.gains.line != 0,
                                 fitted.gains.fill != 0};
  std::vector<int> present;
  for (int k = 0; k < kComponents; ++k) {
    if (has[k]) present.push_back(k);
  }
  const int n = static_cast<int>(present.size());
  std::vector<std::array<bool, kComponents>> subsets;
  for (int size = n; size >= 1; --size) {
    std::vector<int> idx(size);
    for (int k = 0; k < size; ++k) idx[k] = k;
    while (true) {
      std::array<bool, kComponents> s{};
      for (int k : idx) s[present[k]] = true;
      subsets.push_back(s);
      int k = size - 1;
      while (k >= 0 && idx[k] == n - size + k) --k;
      if (k < 0) break;
      ++idx[k];
      for (int j = k + 1; j < size; ++j) idx[j] = idx[j - 1] + 1;
    }
  }
  std::vector<WindowCorrection> out;
  for (const auto& [adapt, scale] :
       {std::pair{true, 1.0}, std::pair{false, 1.0}, std::pair{true, 0.5},
        std::pair{false, 0.5}}) {
    for (const auto& s : subsets) {
      WindowCorrection w = fitted;
      w.adapt = adapt;
      w.gains = Gains{s[0] ? scale : 0.0, s[1] ? scale : 0.0,
                      s[2] ? scale : 0.0, s[3] ? scale : 0.0};
      out.push_back(w);
    }
  }
  return out;
}

}  // namespace orc::edge_ring
