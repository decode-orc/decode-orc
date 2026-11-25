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
// sample rate) and at half the sample rate.
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

bool is_pair(const std::vector<Event>& e, size_t k) {
  return k + 1 < e.size() && e[k + 1].row == e[k].row && e[k].step < 0 &&
         e[k + 1].step > 0 && e[k + 1].centre - e[k].centre <= kPairMax;
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
    if (g > kTail - 4) {
      t[j] *= 0.5 + 0.5 * std::cos(kPi * (g - (kTail - 4)) / 4.0);
    }
  }
  return t;
}

LineTable line_template(const LineTable& sum, const LineTable& count) {
  auto t = smooth(mean_table(sum, count));
  for (int j = 0; j < kLineBins; ++j) {
    const double g = line_time(j);
    if (g < kLineStart) t[j] = 0.0;
    if (g > kTail - 4) {
      t[j] *= 0.5 + 0.5 * std::cos(kPi * (g - (kTail - 4)) / 4.0);
    }
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

double median(std::vector<double> v) {
  if (v.empty()) return 1.0;
  std::sort(v.begin(), v.end());
  const size_t n = v.size();
  return n % 2 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

}  // namespace

bool FieldImage::dropped(int r, int a, int b) const {
  if (dropout.empty() || r < 0 || r >= rows) return false;
  const uint8_t* m = dropout.data() + static_cast<size_t>(r) * width;
  for (int i = std::max(a, 0); i < std::min(b, width); ++i) {
    if (m[i]) return true;
  }
  return false;
}

void mark_dropouts(const std::vector<std::pair<uint64_t, uint32_t>>& runs,
                   size_t samples_per_line, size_t first_line,
                   FieldImage& field) {
  for (const auto& [start, count] : runs) {
    const auto [line, sample] = frame_flat_offset_to_line_sample(
        VideoSystem::PAL, samples_per_line, start);
    if (line < first_line || line >= first_line + field.rows) continue;
    if (field.dropout.empty()) {
      field.dropout.assign(static_cast<size_t>(field.rows) * field.width, 0);
    }
    uint8_t* m = field.dropout.data() +
                 (line - first_line) * static_cast<size_t>(field.width);
    for (size_t i = sample; i < std::min<size_t>(sample + count, field.width);
         ++i) {
      m[i] = 1;
    }
  }
}

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

std::vector<Event> find_events(const FieldImage& field) {
  std::vector<Event> out;
  if (field.width < kSamplesPerLine || field.rows < kLastRow + 2) return out;
  const FieldImage yn = notch(field);
  for (int r = kFirstRow; r < kLastRow; ++r) {
    const double* y = yn.row(r);
    int i = kFirstSample - 8;
    while (i < kLastSample + 8) {
      const double d = y[i + 1] - y[i];
      if (std::abs(d) <= kEventIncrement) {
        ++i;
        continue;
      }
      const double sign = d > 0 ? 1.0 : -1.0;
      int j = i;
      double step = 0.0, moment = 0.0, peak = 0.0;
      while (j < kLastSample + 12) {
        const double dj = y[j + 1] - y[j];
        if (dj * sign <= kEventIncrement) break;
        step += dj;
        moment += (j + 0.5) * dj;
        peak = std::max(peak, std::abs(dj));
        ++j;
      }
      if (std::abs(step) >= kMinEvent) {
        int span = 0;
        for (int k = i; k < j; ++k) {
          if (std::abs(y[k + 1] - y[k]) >= 0.4 * peak) ++span;
        }
        const double c = moment / step;
        const int ic = static_cast<int>(c);
        if (span <= kMaxEventSpan && c >= kFirstSample && c < kLastSample &&
            !field.dropped(r, ic - 2, ic + 3)) {
          out.push_back(Event{r, c, step});
        }
      }
      i = j > i ? j : i + 1;
    }
  }
  return out;
}

int count_pairs(const std::vector<Event>& e) {
  int n = 0;
  for (size_t k = 0; k + 1 < e.size();) {
    if (is_pair(e, k)) {
      ++n;
      k += 2;
    } else {
      ++k;
    }
  }
  return n;
}

int level_bin(double start, double end) {
  int a = 0, b = 0;
  for (double e : kStartEdges) a += start > e;
  for (double e : kEndEdges) b += end > e;
  return a * static_cast<int>(kEndEdges.size() + 1) + b;
}

Features correction_features(const FieldImage& field,
                             const std::vector<Event>& e, PairMode mode) {
  Features out;
  if (e.empty()) return out;
  const FieldImage yn = notch(field);
  const FieldImage yc = comb(field);
  for (size_t k = 0; k < e.size();) {
    const bool pair = is_pair(e, k);
    const size_t last = pair ? k + 1 : k;
    const int r = e[k].row;
    // dense texture where the template would act: left alone
    int after = 0;
    for (size_t j = last + 1; j < e.size() && e[j].row == r &&
                              e[j].centre - e[last].centre <= kDenseAfter;
         ++j) {
      ++after;
    }
    double limit = 1e18;
    if (last + 1 < e.size() && e[last + 1].row == r) {
      // a thin line is corrected itself only on line-art frames; elsewhere it
      // is measured like an edge, so stop short of it as of a step
      const bool line_next = is_pair(e, last + 1) && mode == PairMode::Lines;
      limit = e[last + 1].centre - (line_next ? kLineGuard : kStepGuard);
    }
    if (after > kDenseMax) {
      k = last + 1;
      continue;
    }
    if (pair) {
      if (mode == PairMode::Lines) {
        Feature l;
        l.row = r;
        l.position = 0.5 * (e[k].centre + e[k + 1].centre);
        l.size = 0.5 * (e[k + 1].step - e[k].step);
        l.first = e[k].centre;
        l.last = e[k + 1].centre;
        l.limit = limit;
        out.lines.push_back(l);
      }
      k += 2;
      continue;
    }
    Feature s;
    s.row = r;
    s.position = e[k].centre;
    s.size = e[k].step;
    s.event_step = e[k].step;
    s.limit = limit;
    if (s.size > 0) {
      // start level (notched luma) and lead-in range on the luma row (the
      // composite where its grounds carry no subcarrier, else the comb), for
      // the rising template's level bin and class, as measured
      const int ic = static_cast<int>(s.position);
      s.level = mean(yn.row(r), ic - 14, ic - 5);
      const double* line = field.row(r);
      const bool chroma_free =
          subcarrier_amplitude(line, ic - 30, ic - 6) < kMaxGroundChroma &&
          subcarrier_amplitude(line, ic + 18, ic + 34) < kMaxGroundChroma;
      s.first = ptp(chroma_free ? line : yc.row(r), ic - 14, ic - 5);
    }
    out.steps.push_back(s);
    ++k;
  }
  return out;
}

Features measured_features(const FieldImage& f, const std::vector<Event>& e) {
  Features out;
  if (e.empty()) return out;
  const FieldImage y = comb(f);
  const FieldImage yn = notch(f);
  for (size_t k = 0; k < e.size();) {
    const int r = e[k].row;
    const double c = e[k].centre;
    const bool pair = is_pair(e, k);
    const size_t last = pair ? k + 1 : k;
    const double cl = e[last].centre;
    const bool prev_ok = k == 0 || e[k - 1].row != r ||
                         c - e[k - 1].centre > (pair ? 20 : kIsolatedBefore);
    const bool next_ok = last + 1 >= e.size() || e[last + 1].row != r ||
                         e[last + 1].centre - cl > kIsolatedAfter;
    const int ic = static_cast<int>(c);
    if (prev_ok && next_ok && ic - 32 >= 0 &&
        static_cast<int>(cl) + 36 < f.width && c >= kFirstSample + 20) {
      const double* line = f.row(r);
      const int mode =
          subcarrier_amplitude(line, ic - 30, ic - 6) < kMaxGroundChroma &&
                  subcarrier_amplitude(line, ic + 18, ic + 34) <
                      kMaxGroundChroma
              ? 0
              : 1;
      const double* x = mode == 0 ? line : y.row(r);
      bool ok = true;
      if (mode == 1) {
        std::vector<double> ya(f.width), yb(f.width);
        const double* p = f.row(r - 2);
        const double* w = f.row(r + 2);
        for (int j = ic - 8; j <= static_cast<int>(cl) + 8; ++j) {
          ya[j] = 0.5 * (p[j] + line[j]);
          yb[j] = 0.5 * (line[j] + w[j]);
        }
        double ca, cb;
        if (pair) {
          const int m = static_cast<int>(0.5 * (c + cl) + 0.5);
          ca = minimum_at(ya.data(), m);
          cb = minimum_at(yb.data(), m);
        } else {
          const double lo0 = mean(x, ic - 14, ic - 5);
          const double hi0 = lo0 + e[k].step;
          ca = crossing(ya.data(), lo0, hi0, ic - 4, ic + 4);
          cb = crossing(yb.data(), lo0, hi0, ic - 4, ic + 4);
        }
        ok = ca >= 0 && cb >= 0 && std::abs(ca - cb) <= 0.5;
        // the comb mixes rows r-2 and r+2: they must carry nothing within the
        // measured span but this feature's own counterpart, or their
        // corrections leak into the measurement
        for (int rr : {r - 2, r + 2}) {
          auto j = std::lower_bound(
              e.begin(), e.end(), rr,
              [](const Event& a, int row) { return a.row < row; });
          for (; ok && j != e.end() && j->row == rr; ++j) {
            const double cj = j->centre;
            if (cj < c - 22 || cj > cl + 24) continue;
            if (std::abs(cj - c) <= 1.5 || std::abs(cj - cl) <= 1.5) continue;
            ok = false;
          }
        }
      }
      // no dropout-flagged sample within the measured span (row r, and r +- 2
      // when combed)
      for (int rr : {r - 2, r, r + 2}) {
        if (rr != r && mode == 0) continue;
        if (ok && f.dropped(rr, ic - 22, static_cast<int>(cl) + 25)) ok = false;
      }
      if (ok && pair) {
        const double ce = 0.5 * (c + cl);
        const int ie = static_cast<int>(ce);
        const double depth = 0.5 * (e[last].step - e[k].step);
        const double before = mean(x, ie - 20, ie - 8);
        const double after = mean(x, ie + 10, ie + 22);
        // same ground both sides (a line bordering a fill is not a thin line
        // on ground) and flat grounds
        if (depth >= kMinLineDepth && before >= kMinLineGround &&
            std::abs(e[last].step + e[k].step) <= 0.2 * depth &&
            std::abs(after - before) <= 0.15 * depth &&
            ptp(x, ie - 20, ie - 8) <= 0.2 * depth) {
          Feature l;
          l.row = r;
          l.position = ce;
          l.level = before;
          l.size = depth;
          l.mode = mode;
          l.first = c;
          l.last = cl;
          out.lines.push_back(l);
        }
      } else if (ok) {
        // falls need a flat lead-in; rises only what follows them (the edge
        // of a hatched or stippled fill onto paper is measured, its start
        // level the fill's mean on the notched luma)
        const double s0 = std::abs(e[k].step);
        const bool rise = e[k].step > 0;
        // flat lead-in on the measured row: falls need it; for rises it sets
        // the class, judged exactly as the correction judges it
        const bool flat = ptp(x, ic - 14, ic - 5) <= 0.1 * s0;
        if (s0 >= kMinMeasuredStep && (rise || flat)) {
          const double lo = rise ? mean(yn.row(r), ic - 14, ic - 5)
                                 : mean(x, ic - 14, ic - 5);
          const double hi = mean(x, ic + 8, ic + 20);
          const double sp = hi - lo;
          if (sp * e[k].step > 0 && std::abs(sp) >= kMinMeasuredStep) {
            Feature s;
            s.row = r;
            s.position = c;
            s.level = lo;
            s.size = sp;
            s.mode = mode;
            s.event_step = e[k].step;
            s.edge_class = rise && !flat ? 1 : 0;
            s.bin = level_bin(lo, hi);
            out.steps.push_back(s);
          }
        }
      }
    }
    k = last + 1;
  }
  return out;
}

ProfileSums& ProfileSums::operator+=(const ProfileSums& o) {
  for (int j = 0; j < kStepBins; ++j) {
    rise_sum[j] += o.rise_sum[j];
    rise_count[j] += o.rise_count[j];
    fill_sum[j] += o.fill_sum[j];
    fill_count[j] += o.fill_count[j];
    fall_sum[j] += o.fall_sum[j];
    fall_count[j] += o.fall_count[j];
  }
  for (int j = 0; j < kLineBins; ++j) {
    line_sum[j] += o.line_sum[j];
    line_count[j] += o.line_count[j];
  }
  for (int c = 0; c < kRiseClasses; ++c) {
    for (int b = 0; b < kLevelBins; ++b) {
      for (int j = 0; j < kStepBins; ++j) {
        level_sum[c][b][j] += o.level_sum[c][b][j];
        level_count[c][b][j] += o.level_count[c][b][j];
      }
    }
  }
  rises += o.rises;
  fills += o.fills;
  falls += o.falls;
  lines += o.lines;
  events += o.events;
  pairs += o.pairs;
  rise_kappa.insert(rise_kappa.end(), o.rise_kappa.begin(), o.rise_kappa.end());
  fall_kappa.insert(fall_kappa.end(), o.fall_kappa.begin(), o.fall_kappa.end());
  return *this;
}

ProfileSums measure(const FieldImage& field, const Features& m) {
  ProfileSums s;
  if (m.steps.empty() && m.lines.empty()) return s;
  const FieldImage y = comb(field);
  for (const auto& e : m.steps) {
    const double* x = e.mode == 0 ? field.row(e.row) : y.row(e.row);
    const bool rise = e.size > 0;
    const bool fill = rise && e.edge_class == 1;
    auto& sum = fill ? s.fill_sum : rise ? s.rise_sum : s.fall_sum;
    auto& count = fill ? s.fill_count : rise ? s.rise_count : s.fall_count;
    (fill ? s.fills : rise ? s.rises : s.falls) += 1;
    if (e.event_step != 0.0) {
      (rise ? s.rise_kappa : s.fall_kappa).push_back(e.size / e.event_step);
    }
    const int ib = static_cast<int>(std::floor(e.position));
    for (int idx = ib - kPre; idx < ib + kTail + 2; ++idx) {
      const double t = idx - e.position;
      if (t < -kPre || t > kTail || idx < 0 || idx >= field.width) continue;
      const int j = static_cast<int>(std::floor((t + kPre) * kUp + 0.5));
      if (j < 0 || j >= kStepBins) continue;
      const double p = (x[idx] - e.level) / e.size - (t > 0 ? 1.0 : 0.0);
      sum[j] += p;
      count[j] += 1;
      if (rise) {
        s.level_sum[e.edge_class][e.bin][j] += p;
        s.level_count[e.edge_class][e.bin][j] += 1;
      }
    }
  }
  for (const auto& l : m.lines) {
    const double* x = l.mode == 0 ? field.row(l.row) : y.row(l.row);
    s.lines += 1;
    const int ib = static_cast<int>(std::floor(l.last));
    for (int idx = ib + 1; idx < ib + kTail + 2; ++idx) {
      const double t = idx - l.last;
      if (t < 0 || t > kTail || idx >= field.width) continue;
      const int j = static_cast<int>(std::floor(t * kUp + 0.5));
      if (j >= kLineBins) continue;
      const double after = (l.level - x[idx]) / l.size;
      const double before =
          (l.level - cubic(x, field.width, l.first - t)) / l.size;
      s.line_sum[j] += after - before;
      s.line_count[j] += 1;
    }
  }
  return s;
}

bool is_line_art(const ProfileSums& frame) {
  return frame.pairs >= kLineArtPairs &&
         frame.pairs >= kLineArtFraction * std::max(frame.events, 1);
}

PairMode pair_mode(bool line_art, const Gains& g) {
  // thin-line pairs get the line template on line-art frames and are left
  // alone elsewhere (as two steps they made the test card's thin lines and
  // photographs' fine detail worse)
  return line_art && g.line != 0.0 ? PairMode::Lines : PairMode::Untouched;
}

LevelTables level_templates(const ProfileSums& p) {
  const double span = kPre + kTail + 1;  // counts an edge adds to a bin row
  StepTable both_sum{}, both_count{};
  std::array<StepTable, kRiseClasses> class_sum{}, class_count{};
  for (int c = 0; c < kRiseClasses; ++c) {
    for (int b = 0; b < kLevelBins; ++b) {
      for (int j = 0; j < kStepBins; ++j) {
        class_sum[c][j] += p.level_sum[c][b][j];
        class_count[c][j] += p.level_count[c][b][j];
      }
    }
    for (int j = 0; j < kStepBins; ++j) {
      both_sum[j] += class_sum[c][j];
      both_count[j] += class_count[c][j];
    }
  }
  auto edges = [&](const StepTable& count) {
    double n = 0.0;
    for (double v : count) n += v;
    return n / span;
  };
  const StepTable both = step_template(both_sum, both_count);
  LevelTables out{};
  for (int c = 0; c < kRiseClasses; ++c) {
    const StepTable all = edges(class_count[c]) >= kMinBinEdges
                              ? step_template(class_sum[c], class_count[c])
                              : both;
    for (int b = 0; b < kLevelBins; ++b) {
      out[c][b] = edges(p.level_count[c][b]) >= kMinBinEdges
                      ? step_template(p.level_sum[c][b], p.level_count[c][b])
                      : all;
    }
  }
  return out;
}

void apply(const Features& c, const WindowCorrection& w, const Gains& g,
           FieldImage& out) {
  const auto& t = w.templates;
  for (const auto& e : c.steps) {
    const bool rise = e.size > 0;
    const double step = e.size * (rise ? w.kappa_rise : w.kappa_fall);
    const double* table = t.fall.data();
    double gain = g.fall;
    if (rise) {
      if (!w.levels) continue;
      // class: flat lead-in (isolated) or not (fill edge); bin: start level
      // and start + step
      const int cls = e.first <= 0.1 * e.event_step ? 0 : 1;
      gain = cls == 0 ? g.rise : g.fill;
      table = (*w.levels)[cls][level_bin(e.level, e.level + step)].data();
    }
    if (gain == 0.0) continue;
    double* o = out.row(e.row);
    const int ib = static_cast<int>(std::floor(e.position));
    for (int idx = ib - kPre + 1; idx <= ib + kTail; ++idx) {
      const double tt = idx - e.position;
      if ((tt > -2.0 && tt < 2.0) || tt < -kPre || tt > kTail || idx < 0 ||
          idx >= out.width || idx > e.limit) {
        continue;
      }
      o[idx] -= gain * step * table_at(table, kStepBins, tt, -kPre);
    }
  }
  if (g.line == 0.0) return;
  for (const auto& l : c.lines) {
    double* o = out.row(l.row);
    const int ib = static_cast<int>(std::floor(l.last));
    for (int idx = ib + 1; idx <= ib + kTail; ++idx) {
      const double tt = idx - l.last;
      if (tt < kLineStart || tt > kTail || idx >= out.width || idx > l.limit) {
        continue;
      }
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
  // rising steps keep the window's gains (their templates are per level)
  out.rise = w.gains.rise;
  out.fill = w.gains.fill;
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
                                    int line_art_frames,
                                    std::shared_ptr<const LevelTables> levels) {
  WindowCorrection w;
  w.levels = std::move(levels);
  std::vector<double> rises = s.rise_kappa;
  if (w.levels && s.rises >= kMinFitSteps) w.gains.rise = 1.0;
  if (w.levels && s.fills >= kMinFitSteps) w.gains.fill = 1.0;
  if (w.gains.rise != 0.0 || w.gains.fill != 0.0) w.kappa_rise = median(rises);
  if (s.falls >= kMinFitSteps) {
    w.templates.fall = step_template(s.fall_sum, s.fall_count);
    w.kappa_fall = median(s.fall_kappa);
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
                   double white, const WindowCorrection& w,
                   const std::vector<std::pair<uint64_t, uint32_t>>& dropouts) {
  if (!w.gains.any() || samples_per_line < kSamplesPerLine || white <= black) {
    return false;
  }
  const size_t first_field_lines =
      std::min(field1_lines(VideoSystem::PAL), height);
  const size_t starts[2] = {0, first_field_lines};
  const size_t counts[2] = {first_field_lines, height - first_field_lines};
  FieldImage fields[2];
  std::vector<Event> events[2];
  ProfileSums sums;
  for (int k = 0; k < 2; ++k) {
    fields[k] = field_image(input, samples_per_line, starts[k], counts[k],
                            black, white);
    mark_dropouts(dropouts, samples_per_line, starts[k], fields[k]);
    events[k] = find_events(fields[k]);
    ProfileSums s = measure(fields[k], measured_features(fields[k], events[k]));
    s.events = static_cast<int>(events[k].size());
    s.pairs = count_pairs(events[k]);
    sums += s;
  }
  const bool line_art = is_line_art(sums);
  Gains gains = w.adapt ? frame_gains(w, sums, line_art) : w.gains;
  if (!line_art) gains.line = 0.0;
  if (!gains.any()) return false;
  const PairMode mode = pair_mode(line_art, gains);
  bool changed = false;
  const double scale = white - black;
  for (int k = 0; k < 2; ++k) {
    const Features c = correction_features(fields[k], events[k], mode);
    if (c.steps.empty() && c.lines.empty()) continue;
    FieldImage out = fields[k];
    apply(c, w, gains, out);
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
