/*
 * File:        edge_ring.cpp
 * Module:      orc-stage-plugin-edge-ring-correct
 * Purpose:     The edge ring of PAL LaserDisc pictures, measured and removed
 *              on the composite field
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "edge_ring.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/frame_line_util.h>

#include <algorithm>
#include <cmath>

namespace orc::edge_ring {

namespace {

constexpr double kPi = 3.14159265358979323846;

// Per-frame gains: at most this many times the window's; fewer measured
// features than these and the window's gain is used (lines: none).
constexpr double kMaxFrameGain = 2.0;
constexpr double kMaxFrameLineGain = 1.0;
constexpr int kMinFrameSteps = 8;
constexpr int kMinFrameLines = 12;
// A template is fitted from at least this many measured features.
constexpr int kMinFitSteps = 30;
constexpr int kMinFitLines = 40;

double step_time(int bin) { return -kPre + static_cast<double>(bin) / kUp; }
double line_time(int bin) { return static_cast<double>(bin) / kUp; }

// The vertical [1 2 1] comb over field lines r-2, r, r+2: PAL chroma is
// inverted two field lines on, so it cancels where the picture is vertically
// uniform.
FieldImage comb(const FieldImage& f) {
  FieldImage y = f;
  for (int r = 2; r < f.rows - 2; ++r) {
    const double* a = f.row(r - 2);
    const double* b = f.row(r);
    const double* c = f.row(r + 2);
    double* o = y.row(r);
    for (int i = 0; i < f.width; ++i) o[i] = 0.25 * (a[i] + 2.0 * b[i] + c[i]);
  }
  return y;
}

// [1 2 2 2 1] / 8 along the line: zeros at the subcarrier (a quarter of the
// sample rate) and at half the sample rate. It is symmetric, so crossings
// and centres found on it are unbiased.
FieldImage notch(const FieldImage& f) {
  FieldImage y = f;
  for (int r = 0; r < f.rows; ++r) {
    const double* x = f.row(r);
    double* o = y.row(r);
    for (int i = 2; i < f.width - 2; ++i) {
      o[i] =
          (x[i - 2] + 2.0 * x[i - 1] + 2.0 * x[i] + 2.0 * x[i + 1] + x[i + 2]) /
          8.0;
    }
  }
  return y;
}

double mean(const double* x, int a, int b) {
  double s = 0.0;
  for (int j = a; j < b; ++j) s += x[j];
  return s / (b - a);
}

double ptp(const double* x, int a, int b) {
  double lo = x[a], hi = x[a];
  for (int j = a; j < b; ++j) {
    lo = std::min(lo, x[j]);
    hi = std::max(hi, x[j]);
  }
  return hi - lo;
}

double median(const double* x, int a, int b) {
  std::vector<double> v(x + a, x + b);
  const size_t n = v.size();
  std::sort(v.begin(), v.end());
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

// The sub-sample position in [a, b) where x crosses halfway from lo to hi;
// -1 if it does not.
double crossing(const double* x, double lo, double hi, int a, int b) {
  const double m = 0.5 * (lo + hi);
  for (int j = a; j < b; ++j) {
    const double u = x[j] - m;
    const double v = x[j + 1] - m;
    if ((u < 0) != (v < 0) && u != v) return j + u / (u - v);
  }
  return -1.0;
}

// The sub-sample minimum of y within 2 samples of i (a parabola through the
// lowest sample and its neighbours); -1 if there is none.
double minimum_at(const double* y, int i) {
  int j = i;
  for (int k = i - 2; k <= i + 2; ++k) {
    if (y[k] < y[j]) j = k;
  }
  const double a = y[j - 1], b = y[j], c = y[j + 1];
  const double den = a - 2.0 * b + c;
  if (den <= 0.0) return -1.0;
  return j + 0.5 * (a - c) / den;
}

// Amplitude of the subcarrier (a quarter of the sample rate) in x[a, b),
// b - a a multiple of 4: I/Q demodulated and averaged over the window, so
// noise averages down and only a real subcarrier (chroma) remains.
double subcarrier_amplitude(const double* x, int a, int b) {
  double i_sum = 0.0, q_sum = 0.0;
  for (int j = a; j < b; ++j) {
    switch (j & 3) {
      case 0:
        i_sum += x[j];
        break;
      case 1:
        q_sum += x[j];
        break;
      case 2:
        i_sum -= x[j];
        break;
      default:
        q_sum -= x[j];
        break;
    }
  }
  return 2.0 * std::sqrt(i_sum * i_sum + q_sum * q_sum) / (b - a);
}

// 0: the composite line is luma at i (neither far ground carries a
// subcarrier); 1: use the comb.
int luma_mode(const FieldImage& f, int r, int i, int a0, int a1, int b0,
              int b1) {
  const double* x = f.row(r);
  return subcarrier_amplitude(x, i + a0, i + a1) < kMaxGroundChroma &&
                 subcarrier_amplitude(x, i + b0, i + b1) < kMaxGroundChroma
             ? 0
             : 1;
}

double table_at(const double* table, int bins, double t, double t0) {
  const double u = (t - t0) * kUp;
  const int i = static_cast<int>(std::floor(u));
  if (i < 0 || i + 1 >= bins) return 0.0;
  const double f = u - i;
  return table[i] * (1.0 - f) + table[i + 1] * f;
}

// Catmull-Rom interpolation of x at fractional position u.
double cubic(const double* x, int n, double u) {
  const int i = static_cast<int>(std::floor(u));
  const double f = u - i;
  if (i < 1 || i + 2 >= n) return x[std::min(std::max(i, 0), n - 1)];
  const double p0 = x[i - 1], p1 = x[i], p2 = x[i + 1], p3 = x[i + 2];
  return p1 + 0.5 * f *
                  (p2 - p0 +
                   f * (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3 +
                        f * (3.0 * (p1 - p2) + p3 - p0)));
}

void find_steps(const FieldImage& f, const FieldImage& y, const FieldImage& yn,
                bool strict, std::vector<Feature>& out) {
  const int last_row = std::min(kLastRow, f.rows - 2);
  for (int r = kFirstRow; r < last_row; ++r) {
    const double* n = yn.row(r);
    int i = kFirstSample;
    while (i < kLastSample) {
      const double s = n[i + 3] - n[i - 3];
      const double as = std::abs(s);
      if (as < kMinStep || as < std::abs(n[i + 4] - n[i - 2]) ||
          as < std::abs(n[i + 2] - n[i - 4])) {
        ++i;
        continue;
      }
      const int mode = luma_mode(f, r, i, -30, -6, 18, 34);
      const double* x = mode == 0 ? f.row(r) : y.row(r);
      const double lo = mean(x, i - 14, i - 4);
      const double hi =
          strict ? mean(x, i + 8, i + 20) : mean(x, i + 4, i + 12);
      const double step = hi - lo;
      bool ok = std::abs(step) >= kMinStep && step * s >= 0 &&
                ptp(x, i - 14, i - 4) <= (strict ? 0.06 : 0.1) * std::abs(step);
      if (ok && strict) ok = ptp(x, i + 8, i + 20) <= 0.15 * std::abs(step);
      if (ok && !strict) {
        for (int j = i + 2; j <= i + 12 && ok; ++j)
          ok = (x[j] - lo) / step >= 0.5;
      }
      if (ok) ok = std::abs(x[i + 2] - x[i - 2]) >= 0.8 * std::abs(step);
      double e0 = -1.0;
      if (ok) e0 = crossing(x, lo, hi, i - 3, i + 3);
      ok = ok && e0 >= 0.0;
      if (ok && mode == 1) {
        std::vector<double> ya(f.width), yb(f.width);
        const double* p = f.row(r - 2);
        const double* q = f.row(r);
        const double* w = f.row(r + 2);
        for (int k = i - 5; k <= i + 5; ++k) {
          ya[k] = 0.5 * (p[k] + q[k]);
          yb[k] = 0.5 * (q[k] + w[k]);
        }
        const double ca = crossing(ya.data(), lo, hi, i - 4, i + 4);
        const double cb = crossing(yb.data(), lo, hi, i - 4, i + 4);
        ok = ca >= 0.0 && cb >= 0.0 && std::abs(ca - cb) <= 0.5;
      }
      if (!ok) {
        ++i;
        continue;
      }
      out.push_back(Feature{r, e0, lo, step, mode});
      i += 10;
    }
  }
}

void find_lines(const FieldImage& f, const FieldImage& y, const FieldImage& yn,
                bool strict, std::vector<Feature>& out) {
  const double min_ground = strict ? kMinMeasuredLineGround : kMinLineGround;
  const int last_row = std::min(kLastRow, f.rows - 2);
  for (int r = kFirstRow; r < last_row; ++r) {
    const double* n = yn.row(r);
    for (int i = kFirstSample + 12; i < kLastSample - 6; ++i) {
      if (!(n[i] < n[i - 1] && n[i] <= n[i + 1])) continue;
      const int mode = luma_mode(f, r, i, -30, -6, 16, 40);
      const double* x = mode == 0 ? f.row(r) : y.row(r);
      int j = i;
      for (int k = i - 2; k <= i + 2; ++k) {
        if (x[k] < x[j]) j = k;
      }
      const double before = median(x, j - 20, j - 6);
      const double after = median(x, j + 10, j + 24);
      const double ground = 0.5 * (before + after);
      const double depth = ground - x[j];
      if (depth < kMinLineDepth || ground < min_ground) continue;
      if (std::abs(before - after) > (strict ? 0.1 : 0.2) * depth) continue;
      if (ptp(x, j - 20, j - 6) > (strict ? 0.2 : 0.4) * depth) continue;
      if (strict && ptp(x, j + 10, j + 24) > 0.35 * depth) continue;
      if (ground - x[j - 4] > 0.5 * depth || ground - x[j + 4] > 0.5 * depth) {
        continue;
      }
      const double centre = minimum_at(x, j);
      if (centre < 0.0 || std::abs(centre - j) > 1.0) continue;
      if (mode == 1) {
        std::vector<double> ya(f.width), yb(f.width);
        const double* p = f.row(r - 2);
        const double* q = f.row(r);
        const double* w = f.row(r + 2);
        for (int k = j - 5; k <= j + 5; ++k) {
          ya[k] = 0.5 * (p[k] + q[k]);
          yb[k] = 0.5 * (q[k] + w[k]);
        }
        const double ca = minimum_at(ya.data(), j);
        const double cb = minimum_at(yb.data(), j);
        if (ca < 0.0 || cb < 0.0 || std::abs(ca - cb) > 0.5) continue;
      }
      if (out.empty() || out.back().row != r ||
          centre - out.back().position > 8.0) {
        out.push_back(Feature{r, centre, ground, depth, mode});
      }
    }
  }
}

// gaussian_filter1d(sigma 1, radius 4, mirrored ends) of a table
template <size_t N>
std::array<double, N> smooth(const std::array<double, N>& t) {
  static const std::array<double, 9> w = [] {
    std::array<double, 9> k{};
    double s = 0.0;
    for (int i = -4; i <= 4; ++i) s += k[i + 4] = std::exp(-0.5 * i * i);
    for (auto& v : k) v /= s;
    return k;
  }();
  std::array<double, N> out{};
  const int n = static_cast<int>(N);
  for (int i = 0; i < n; ++i) {
    double s = 0.0;
    for (int k = -4; k <= 4; ++k) {
      int j = i + k;
      if (j < 0) j = -j - 1;
      if (j >= n) j = 2 * n - j - 1;
      s += w[k + 4] * t[j];
    }
    out[i] = s;
  }
  return out;
}

template <size_t N>
std::array<double, N> mean_table(const std::array<double, N>& sum,
                                 const std::array<double, N>& count) {
  std::array<double, N> t{};
  for (size_t i = 0; i < N; ++i) t[i] = count[i] > 0 ? sum[i] / count[i] : 0.0;
  return t;
}

StepTable step_template(const StepTable& sum, const StepTable& count) {
  // The post side only (t >= 2): the pre side measured on the composite's
  // luma does not carry over to the decoded luma (on the Domesday test card
  // a +3 % pre template made a -5 % dip after the PAL decoder).
  auto t = smooth(mean_table(sum, count));
  for (int j = 0; j < kStepBins; ++j) {
    const double g = step_time(j);
    if (g < 2.0) t[j] = 0.0;
    if (g > kTail - 4)
      t[j] *= 0.5 + 0.5 * std::cos(kPi * (g - (kTail - 4)) / 4.0);
  }
  return t;
}

LineTable line_template(const LineTable& sum, const LineTable& count) {
  auto t = smooth(mean_table(sum, count));
  for (int j = 0; j < kLineBins; ++j) {
    const double g = line_time(j);
    if (g < 2.0) t[j] = 0.0;
    if (g > kTail - 4)
      t[j] *= 0.5 + 0.5 * std::cos(kPi * (g - (kTail - 4)) / 4.0);
  }
  return t;
}

// least-squares scale of the measured mean excess onto the template, over
// the bins the mask selects that hold data
template <size_t N, typename Mask>
double projection(const std::array<double, N>& sum,
                  const std::array<double, N>& count,
                  const std::array<double, N>& table, Mask mask) {
  double num = 0.0, den = 0.0;
  for (size_t j = 0; j < N; ++j) {
    if (!mask(static_cast<int>(j)) || count[j] <= 0) continue;
    num += sum[j] / count[j] * table[j];
    den += table[j] * table[j];
  }
  return den > 0.0 ? num / den : 0.0;
}

bool step_fit_bin(int j) {
  const double g = step_time(j);
  return (g >= 2.0 && g <= 12.0) || (g >= -8.0 && g <= -2.0);
}
bool line_fit_bin(int j) {
  const double g = line_time(j);
  return g >= 2.0 && g <= 10.0;
}

}  // namespace

FieldImage field_image(const int16_t* frame, size_t samples_per_line,
                       size_t first_line, size_t line_count, double black,
                       double white) {
  FieldImage f;
  f.rows = static_cast<int>(line_count);
  f.width = static_cast<int>(samples_per_line);
  f.v.resize(line_count * samples_per_line);
  const double scale = 1.0 / (white - black);
  for (size_t r = 0; r < line_count; ++r) {
    const int16_t* p =
        frame + frame_line_sample_offset(VideoSystem::PAL, samples_per_line,
                                         first_line + r);
    double* o = f.row(static_cast<int>(r));
    for (size_t i = 0; i < samples_per_line; ++i) o[i] = (p[i] - black) * scale;
  }
  return f;
}

Features find_features(const FieldImage& field, bool strict) {
  Features out;
  if (field.width < kSamplesPerLine || field.rows < kLastRow + 2) return out;
  const FieldImage y = comb(field);
  const FieldImage yn = notch(field);
  find_steps(field, y, yn, strict, out.steps);
  find_lines(field, y, yn, strict, out.lines);
  return out;
}

ProfileSums& ProfileSums::operator+=(const ProfileSums& o) {
  for (int j = 0; j < kStepBins; ++j) {
    rise_sum[j] += o.rise_sum[j];
    rise_count[j] += o.rise_count[j];
    fall_sum[j] += o.fall_sum[j];
    fall_count[j] += o.fall_count[j];
  }
  for (int j = 0; j < kLineBins; ++j) {
    line_sum[j] += o.line_sum[j];
    line_count[j] += o.line_count[j];
  }
  rises += o.rises;
  falls += o.falls;
  lines += o.lines;
  return *this;
}

ProfileSums measure(const FieldImage& field, const Features& m) {
  ProfileSums s;
  if (m.steps.empty() && m.lines.empty()) return s;
  const FieldImage y = comb(field);
  for (const auto& e : m.steps) {
    const double* x = e.mode == 0 ? field.row(e.row) : y.row(e.row);
    const bool rise = e.size > 0;
    auto& sum = rise ? s.rise_sum : s.fall_sum;
    auto& count = rise ? s.rise_count : s.fall_count;
    (rise ? s.rises : s.falls) += 1;
    const int ib = static_cast<int>(std::floor(e.position));
    for (int idx = ib - kPre; idx < ib + kTail + 2; ++idx) {
      const double t = idx - e.position;
      if (t < -kPre || t > kTail || idx < 0 || idx >= field.width) continue;
      const int j = static_cast<int>(std::floor((t + kPre) * kUp + 0.5));
      if (j < 0 || j >= kStepBins) continue;
      const double p = (x[idx] - e.level) / e.size;
      sum[j] += p - (t > 0 ? 1.0 : 0.0);
      count[j] += 1;
    }
  }
  for (const auto& l : m.lines) {
    const double* x = l.mode == 0 ? field.row(l.row) : y.row(l.row);
    s.lines += 1;
    const int ib = static_cast<int>(std::floor(l.position));
    for (int idx = ib + 1; idx < ib + kTail + 2; ++idx) {
      const double t = idx - l.position;
      if (t < 0 || t > kTail || idx >= field.width) continue;
      const int j = static_cast<int>(std::floor(t * kUp + 0.5));
      if (j >= kLineBins) continue;
      const double after = (l.level - x[idx]) / l.size;
      const double before =
          (l.level - cubic(x, field.width, l.position - t)) / l.size;
      s.line_sum[j] += after - before;
      s.line_count[j] += 1;
    }
  }
  return s;
}

void apply(const FieldImage& field, const Features& c, const Templates& t,
           const Gains& g, FieldImage& out) {
  (void)field;
  for (const auto& e : c.steps) {
    const bool rise = e.size > 0;
    const double gain = rise ? g.rise : g.fall;
    if (gain == 0.0) continue;
    const double* table = rise ? t.rise.data() : t.fall.data();
    double* o = out.row(e.row);
    const int ib = static_cast<int>(std::floor(e.position));
    for (int idx = ib - kPre + 1; idx <= ib + kTail; ++idx) {
      const double tt = idx - e.position;
      if ((tt > -2.0 && tt < 2.0) || tt < -kPre || tt > kTail || idx < 0 ||
          idx >= out.width) {
        continue;
      }
      o[idx] -= gain * e.size * table_at(table, kStepBins, tt, -kPre);
    }
  }
  if (g.line == 0.0) return;
  for (const auto& l : c.lines) {
    double* o = out.row(l.row);
    const int ib = static_cast<int>(std::floor(l.position));
    for (int idx = ib + 2; idx <= ib + kTail; ++idx) {
      const double tt = idx - l.position;
      if (tt < 2.0 || tt > kTail || idx >= out.width) continue;
      o[idx] += g.line * l.size * table_at(t.line.data(), kLineBins, tt, 0.0);
    }
  }
}

Gains frame_gains(const WindowCorrection& w, const ProfileSums& f,
                  bool line_art) {
  Gains out;
  auto step_gain = [&](double g0, int n, const StepTable& sum,
                       const StepTable& count, const StepTable& table) {
    if (g0 == 0.0) return 0.0;
    if (n < kMinFrameSteps) return g0;
    return g0 * std::clamp(projection(sum, count, table, step_fit_bin), 0.0,
                           kMaxFrameGain);
  };
  out.rise = step_gain(w.gains.rise, f.rises, f.rise_sum, f.rise_count,
                       w.templates.rise);
  out.fall = step_gain(w.gains.fall, f.falls, f.fall_sum, f.fall_count,
                       w.templates.fall);
  if (line_art && w.gains.line != 0.0 && f.lines >= kMinFrameLines) {
    out.line =
        w.gains.line * std::clamp(projection(f.line_sum, f.line_count,
                                             w.templates.line, line_fit_bin),
                                  0.0, kMaxFrameLineGain);
  }
  return out;
}

std::optional<WindowCorrection> fit(const ProfileSums& s,
                                    const ProfileSums& line_art,
                                    int line_art_frames) {
  WindowCorrection w;
  if (s.rises >= kMinFitSteps) {
    w.templates.rise = step_template(s.rise_sum, s.rise_count);
    w.gains.rise = 1.0;
  }
  if (s.falls >= kMinFitSteps) {
    w.templates.fall = step_template(s.fall_sum, s.fall_count);
    w.gains.fall = 1.0;
  }
  if (s.lines >= kMinFitLines && line_art_frames >= kMinLineArtFrames) {
    w.templates.line = line_template(line_art.line_sum, line_art.line_count);
    w.gains.line = 1.0;
  }
  if (!w.gains.any()) return std::nullopt;
  return w;
}

bool correct_frame(const int16_t* input, int16_t* output,
                   size_t samples_per_line, size_t height, double black,
                   double white, const WindowCorrection& w) {
  if (!w.gains.any() || samples_per_line < kSamplesPerLine || white <= black) {
    return false;
  }
  const size_t first_field_lines =
      std::min(field1_lines(VideoSystem::PAL), height);
  const size_t starts[2] = {0, first_field_lines};
  const size_t counts[2] = {first_field_lines, height - first_field_lines};
  FieldImage fields[2];
  Features corrections[2];
  ProfileSums sums;
  for (int k = 0; k < 2; ++k) {
    fields[k] = field_image(input, samples_per_line, starts[k], counts[k],
                            black, white);
    corrections[k] = find_features(fields[k], false);
    if (w.adapt) {
      sums += measure(fields[k], find_features(fields[k], true));
    } else if (w.gains.line != 0.0) {
      sums.lines +=
          static_cast<int>(find_features(fields[k], true).lines.size());
    }
  }
  const bool line_art = sums.lines >= kLineArtLines;
  Gains gains = w.adapt ? frame_gains(w, sums, line_art) : w.gains;
  if (!line_art) gains.line = 0.0;
  if (!gains.any()) return false;
  bool changed = false;
  const double scale = white - black;
  for (int k = 0; k < 2; ++k) {
    if (corrections[k].steps.empty() && corrections[k].lines.empty()) continue;
    FieldImage out = fields[k];
    apply(fields[k], corrections[k], w.templates, gains, out);
    for (int r = 0; r < out.rows; ++r) {
      int16_t* o =
          output + frame_line_sample_offset(VideoSystem::PAL, samples_per_line,
                                            starts[k] + r);
      const double* a = out.row(r);
      const double* b = fields[k].row(r);
      for (int i = 0; i < out.width; ++i) {
        if (a[i] == b[i]) continue;
        const double v = std::round(a[i] * scale + black);
        const auto q = static_cast<int16_t>(std::clamp(v, 0.0, 1023.0));
        if (q != o[i]) {
          o[i] = q;
          changed = true;
        }
      }
    }
  }
  return changed;
}

}  // namespace orc::edge_ring
