/*
 * File:        stack_measurement.cpp
 * Module:      orc-sdk-support
 * Purpose:     Measurements between captures of one disc: sample-grid offset,
 *              pair noise, and the pressing noise model
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/stack_measurement.h>

#include <cmath>
#include <cstring>

namespace orc {

namespace {

// Raw-sample refinement window around the low-passed peak.
constexpr int32_t kFineSearch = 2;

// Lines sampled per frame when measuring a grid offset.
constexpr size_t kGridLinesPerFrame = 8;

// Decimation of the noise grid.
constexpr size_t kNoiseLineStep = 4;
constexpr size_t kNoiseSampleStep = 2;

// Pair differences further than this many robust σ from their median are
// picture leaking through rather than noise.
constexpr double kNoiseClip = 4.0;

// Differences a pair needs before its variance is trusted.
constexpr uint64_t kMinNoiseSamples = 1000;

// Spread of v_ik − v_jk over the other captures k, as a fraction of the
// median pair variance, below which captures i and j are taken to share a
// pressing.
constexpr double kPressingSpread = 0.25;

// Weight of the rows that settle what the pair variances leave open: with two
// pressings their shared noise appears only as a sum, which these split
// evenly; a pressing beside only lone captures has shared noise that cannot
// be told from theirs, which these leave with them.
constexpr double kNoiseRidge = 1e-3;

// Active picture within a line: from about a fifth of the way in for both
// 4FSC systems (PAL 10.5 µs of 64 µs, NTSC 9.5 µs of 63.5 µs) to just short
// of the end.
size_t active_first(size_t length) { return length / 5; }
size_t active_last(size_t length) { return length > 10 ? length - 10 : length; }

// Translate one line so that sample n takes the value previously at
// n + |offset|, replicating the edge sample into the samples this vacates.
void shift_line_samples(StackSample* line, size_t length, int32_t offset) {
  if (offset == 0 || length == 0) return;

  const size_t shift = static_cast<size_t>(std::abs(offset));
  if (shift >= length) {
    std::fill(line, line + length, line[offset > 0 ? length - 1 : 0]);
    return;
  }
  if (offset > 0) {
    std::memmove(line, line + shift, (length - shift) * sizeof(StackSample));
    std::fill(line + length - shift, line + length, line[length - shift - 1]);
  } else {
    std::memmove(line + shift, line, (length - shift) * sizeof(StackSample));
    std::fill(line, line + shift, line[shift]);
  }
}

// Accumulate the correlation of reference sample n against other sample
// n + lag over [first, last) into |xy|, |xx| and |yy|, one slot per lag in
// [-max_lag, max_lag].  The same reference samples are used at every lag so
// the correlations stay comparable; the caller keeps max_lag samples of guard
// either side.
void accumulate_line_correlation(const std::vector<double>& a,
                                 const std::vector<double>& b, size_t first,
                                 size_t last, int32_t max_lag,
                                 std::vector<double>& acc_xy,
                                 std::vector<double>& acc_xx,
                                 std::vector<double>& acc_yy) {
  double mean_a = 0.0;
  double mean_b = 0.0;
  for (size_t n = first; n < last; ++n) {
    mean_a += a[n];
    mean_b += b[n];
  }
  const auto span = static_cast<double>(last - first);
  mean_a /= span;
  mean_b /= span;

  for (int32_t lag = -max_lag; lag <= max_lag; ++lag) {
    double xy = 0.0;
    double xx = 0.0;
    double yy = 0.0;
    for (size_t n = first; n < last; ++n) {
      const double x = a[n] - mean_a;
      const double y =
          b[static_cast<size_t>(static_cast<int64_t>(n) + lag)] - mean_b;
      xy += x * y;
      xx += x * x;
      yy += y * y;
    }
    const auto slot = static_cast<size_t>(lag) + static_cast<size_t>(max_lag);
    acc_xy[slot] += xy;
    acc_xx[slot] += xx;
    acc_yy[slot] += yy;
  }
}

double median_of(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const size_t mid = v.size() / 2;
  return v.size() % 2 ? v[mid] : 0.5 * (v[mid - 1] + v[mid]);
}

// Least-squares solution of rows · x = rhs over |unknowns|; nullopt when the
// normal equations are singular.
std::optional<std::vector<double>> least_squares(
    const std::vector<std::vector<double>>& rows,
    const std::vector<double>& rhs, size_t unknowns) {
  std::vector<std::vector<double>> a(unknowns,
                                     std::vector<double>(unknowns + 1, 0.0));
  for (size_t r = 0; r < rows.size(); ++r) {
    for (size_t p = 0; p < unknowns; ++p) {
      for (size_t q = 0; q < unknowns; ++q) a[p][q] += rows[r][p] * rows[r][q];
      a[p][unknowns] += rows[r][p] * rhs[r];
    }
  }

  // Gauss-Jordan elimination with partial pivoting.
  for (size_t col = 0; col < unknowns; ++col) {
    size_t pivot = col;
    for (size_t row = col + 1; row < unknowns; ++row) {
      if (std::abs(a[row][col]) > std::abs(a[pivot][col])) pivot = row;
    }
    if (std::abs(a[pivot][col]) < 1e-9) return std::nullopt;
    std::swap(a[col], a[pivot]);
    for (size_t row = 0; row < unknowns; ++row) {
      if (row == col) continue;
      const double factor = a[row][col] / a[col][col];
      for (size_t k = col; k <= unknowns; ++k) a[row][k] -= factor * a[col][k];
    }
  }

  std::vector<double> x(unknowns);
  for (size_t p = 0; p < unknowns; ++p) x[p] = a[p][unknowns] / a[p][p];
  return x;
}

// Group |members| by pressing from the spread of v_ik − v_jk; returns each
// member's group, numbered from 0.  |pair_variance| is indexed i * n + j for
// i < j.  Fewer than 4 captures cannot show a jump, so they form one group.
std::vector<size_t> group_by_pressing(
    size_t n, const std::vector<size_t>& members,
    const std::vector<std::optional<double>>& pair_variance) {
  const size_t m = members.size();
  std::vector<size_t> group(m, 0);
  if (m < 4) return group;

  auto variance = [&](size_t p, size_t q) {
    return pair_variance[std::min(members[p], members[q]) * n +
                         std::max(members[p], members[q])];
  };

  std::vector<double> all;
  for (size_t p = 0; p < m; ++p) {
    for (size_t q = p + 1; q < m; ++q) {
      const std::optional<double> v = variance(p, q);
      if (v) all.push_back(*v);
    }
  }
  if (all.empty()) return group;
  const double threshold = kPressingSpread * median_of(all);

  std::vector<std::vector<double>> spread(m, std::vector<double>(m, 0.0));
  for (size_t p = 0; p < m; ++p) {
    for (size_t q = p + 1; q < m; ++q) {
      double lowest = 0.0;
      double highest = 0.0;
      size_t count = 0;
      for (size_t k = 0; k < m; ++k) {
        if (k == p || k == q) continue;
        const std::optional<double> vp = variance(p, k);
        const std::optional<double> vq = variance(q, k);
        if (!vp || !vq) continue;
        const double d = *vp - *vq;
        lowest = count == 0 ? d : std::min(lowest, d);
        highest = count == 0 ? d : std::max(highest, d);
        ++count;
      }
      spread[p][q] = spread[q][p] = count >= 2 ? highest - lowest : 0.0;
    }
  }

  // Average-linkage merging while the closest two clusters are within the
  // threshold.
  std::vector<std::vector<size_t>> clusters(m);
  for (size_t p = 0; p < m; ++p) clusters[p] = {p};
  while (clusters.size() > 1) {
    double best = 0.0;
    size_t best_a = 0;
    size_t best_b = 0;
    bool found = false;
    for (size_t ca = 0; ca < clusters.size(); ++ca) {
      for (size_t cb = ca + 1; cb < clusters.size(); ++cb) {
        double sum = 0.0;
        for (size_t p : clusters[ca]) {
          for (size_t q : clusters[cb]) sum += spread[p][q];
        }
        const double linkage = sum / static_cast<double>(clusters[ca].size() *
                                                         clusters[cb].size());
        if (!found || linkage < best) {
          best = linkage;
          best_a = ca;
          best_b = cb;
          found = true;
        }
      }
    }
    if (best >= threshold) break;
    clusters[best_a].insert(clusters[best_a].end(), clusters[best_b].begin(),
                            clusters[best_b].end());
    clusters.erase(clusters.begin() + static_cast<std::ptrdiff_t>(best_b));
  }
  for (size_t c = 0; c < clusters.size(); ++c) {
    for (size_t p : clusters[c]) group[p] = c;
  }
  return group;
}

}  // namespace

// ── Frame geometry ──────────────────────────────────────────────────────────

size_t field1_line_count(VideoSystem system) {
  switch (system) {
    case VideoSystem::PAL:
      return static_cast<size_t>(kPalField1Lines);
    case VideoSystem::PAL_M:
      return static_cast<size_t>(kPalMField1Lines);
    case VideoSystem::NTSC:
    default:
      return static_cast<size_t>(kNtscField1Lines);
  }
}

std::vector<uint8_t> dropout_mask(const std::vector<DropoutRun>& runs,
                                  size_t total) {
  std::vector<uint8_t> mask(total, 0);
  for (const auto& run : runs) {
    if (run.sample_start >= total) continue;
    const auto end = static_cast<size_t>(
        std::min<uint64_t>(run.sample_start + run.sample_count, total));
    std::fill(mask.begin() + static_cast<std::ptrdiff_t>(run.sample_start),
              mask.begin() + static_cast<std::ptrdiff_t>(end), uint8_t{1});
  }
  return mask;
}

void shift_frame_samples(std::vector<StackSample>& frame, VideoSystem system,
                         size_t nominal_width, size_t height, int32_t offset) {
  if (offset == 0) return;
  for (size_t line = 0; line < height; ++line) {
    const size_t start = frame_line_sample_offset(system, nominal_width, line);
    const size_t length = frame_line_sample_count(system, nominal_width, line);
    if (start + length > frame.size()) break;
    shift_line_samples(frame.data() + start, length, offset);
  }
}

void shift_dropout_runs(std::vector<DropoutRun>& runs, VideoSystem system,
                        size_t nominal_width, size_t height, int32_t offset) {
  if (offset == 0) return;
  for (auto& run : runs) {
    const auto [line, sample_in_line] = frame_flat_offset_to_line_sample(
        system, nominal_width, run.sample_start);
    if (line >= height) continue;

    const size_t start = frame_line_sample_offset(system, nominal_width, line);
    const size_t length = frame_line_sample_count(system, nominal_width, line);
    const auto limit = static_cast<int64_t>(length);

    int64_t first = static_cast<int64_t>(sample_in_line) - offset;
    int64_t last = first + static_cast<int64_t>(run.sample_count);
    first = std::clamp<int64_t>(first, 0, limit);
    last = std::clamp<int64_t>(last, 0, limit);

    run.sample_start = start + static_cast<uint64_t>(first);
    run.sample_count = static_cast<uint32_t>(last - first);
  }
}

// ── Sample-grid offset ──────────────────────────────────────────────────────

SampleGridMeter::LagCorrelation::LagCorrelation(int32_t max_lag)
    : max_lag(max_lag),
      xy(2 * static_cast<size_t>(max_lag) + 1, 0.0),
      xx(xy.size(), 0.0),
      yy(xy.size(), 0.0) {}

std::optional<double> SampleGridMeter::LagCorrelation::at(int32_t lag) const {
  const auto slot = static_cast<size_t>(lag) + static_cast<size_t>(max_lag);
  const double denominator = std::sqrt(xx[slot] * yy[slot]);
  if (denominator <= 0.0) return std::nullopt;
  return xy[slot] / denominator;
}

SampleGridMeter::SampleGridMeter()
    : coarse_(kMaxSampleGridShift), fine_(kMaxSampleGridShift + kFineSearch) {}

void SampleGridMeter::add_frame(const std::vector<StackSample>& reference,
                                const std::vector<StackSample>& other,
                                VideoSystem system, size_t nominal_width,
                                size_t height) {
  // Only active picture is used — the front of every line carries sync and
  // burst, which repeat identically line to line and would correlate at any
  // lag.
  const size_t step = std::max<size_t>(1, height / (kGridLinesPerFrame + 1));
  const auto guard =
      static_cast<size_t>(std::max(coarse_.max_lag, fine_.max_lag));

  std::vector<double> raw_a;
  std::vector<double> raw_b;
  std::vector<double> box_a;
  std::vector<double> box_b;

  for (size_t line = step; line < height; line += step) {
    const size_t start = frame_line_sample_offset(system, nominal_width, line);
    const size_t length = frame_line_sample_count(system, nominal_width, line);
    if (start + length > reference.size() || start + length > other.size()) {
      break;
    }

    const size_t first = active_first(length);
    const size_t last = active_last(length);
    if (last <= first + 2 * guard + kGridLumaBox) continue;

    raw_a.assign(
        reference.begin() + static_cast<std::ptrdiff_t>(start),
        reference.begin() + static_cast<std::ptrdiff_t>(start + length));
    raw_b.assign(other.begin() + static_cast<std::ptrdiff_t>(start),
                 other.begin() + static_cast<std::ptrdiff_t>(start + length));

    // box[n] averages raw[n .. n + kGridLumaBox): the same delay for both
    // lines, so it does not move the lag.
    box_a.assign(length - kGridLumaBox + 1, 0.0);
    box_b.assign(length - kGridLumaBox + 1, 0.0);
    for (size_t n = 0; n + kGridLumaBox <= length; ++n) {
      for (size_t k = 0; k < kGridLumaBox; ++k) {
        box_a[n] += raw_a[n + k];
        box_b[n] += raw_b[n + k];
      }
    }

    accumulate_line_correlation(box_a, box_b, first + guard,
                                last - guard - kGridLumaBox, coarse_.max_lag,
                                coarse_.xy, coarse_.xx, coarse_.yy);
    accumulate_line_correlation(raw_a, raw_b, first + guard, last - guard,
                                fine_.max_lag, fine_.xy, fine_.xx, fine_.yy);
  }
  ++frames_;
}

SampleGridMeter::Result SampleGridMeter::measure() const {
  Result result;

  // Coarse lag from the low-passed luma, where the subcarrier cannot alias it
  // by a cycle.
  std::optional<int32_t> coarse_lag;
  for (int32_t lag = -kMaxSampleGridShift; lag <= kMaxSampleGridShift; ++lag) {
    const auto c = coarse_.at(lag);
    if (c && (!coarse_lag || *c > result.correlation)) {
      coarse_lag = lag;
      result.correlation = *c;
    }
  }
  if (!coarse_lag) {
    result.flat = true;
    return result;
  }
  if (result.correlation < kMinGridCorrelation) return result;

  // Exact lag from the raw samples near the coarse peak.
  int32_t best_lag = *coarse_lag;
  std::optional<double> best_fine;
  for (int32_t lag = *coarse_lag - kFineSearch;
       lag <= *coarse_lag + kFineSearch; ++lag) {
    const auto c = fine_.at(lag);
    if (c && (!best_fine || *c > *best_fine)) {
      best_fine = c;
      best_lag = lag;
    }
  }
  result.offset = best_lag;
  return result;
}

// ── Pair noise ──────────────────────────────────────────────────────────────

std::optional<double> DifferenceHistogram::robust_variance() const {
  if (count_ < kMinNoiseSamples) return std::nullopt;

  // Median, then the median absolute deviation from it.
  const uint64_t half = count_ / 2;
  int32_t median = -kRange;
  for (uint64_t seen = 0; median <= kRange; ++median) {
    seen += bins_[slot(median)];
    if (seen > half) break;
  }
  int32_t mad = 0;
  for (uint64_t seen = bins_[slot(median)]; seen <= half && mad < 2 * kRange;) {
    ++mad;
    if (median - mad >= -kRange) seen += bins_[slot(median - mad)];
    if (median + mad <= kRange) seen += bins_[slot(median + mad)];
  }

  // 1.4826 · MAD estimates σ for Gaussian noise.  The clip is kept at a couple
  // of codes so near-identical captures still measure.
  const double clip = std::max(kNoiseClip * 1.4826 * mad, 2.0);
  double n = 0.0;
  double sum = 0.0;
  double sum_sq = 0.0;
  for (int32_t d = -kRange; d <= kRange; ++d) {
    const uint64_t c = bins_[slot(d)];
    if (c == 0 || std::abs(d - median) > clip) continue;
    const auto w = static_cast<double>(c);
    n += w;
    sum += w * d;
    sum_sq += w * d * d;
  }
  if (n < 2.0) return std::nullopt;
  const double mean = sum / n;
  return std::max(0.0, sum_sq / n - mean * mean);
}

PairNoiseMeter::PairNoiseMeter(size_t inputs)
    : inputs_(inputs), pairs_(inputs * inputs) {}

void PairNoiseMeter::add_frame(
    const std::vector<const std::vector<StackSample>*>& planes,
    const std::vector<const std::vector<uint8_t>*>& masks, VideoSystem system,
    size_t nominal_width, size_t height) {
  const size_t n = inputs_;
  const size_t frame_samples =
      frame_line_sample_offset(system, nominal_width, height);
  std::vector<size_t> valid;
  for (size_t i = 0; i < n && i < planes.size() && i < masks.size(); ++i) {
    if (!planes[i] || !masks[i]) continue;
    if (planes[i]->size() < frame_samples || masks[i]->size() < frame_samples) {
      continue;
    }
    valid.push_back(i);
  }
  if (valid.size() < 2) return;

  const size_t field1_lines = std::min(field1_line_count(system), height);
  for (size_t line = 0; line < height; line += kNoiseLineStep) {
    const size_t in_field = line < field1_lines ? line : line - field1_lines;
    if (in_field < kFieldBlankingLines) continue;
    const size_t start = frame_line_sample_offset(system, nominal_width, line);
    const size_t length = frame_line_sample_count(system, nominal_width, line);
    const size_t first = active_first(length);
    const size_t last = active_last(length);
    for (size_t x = first; x < last; x += kNoiseSampleStep) {
      const size_t off = start + x;
      for (size_t a = 0; a < valid.size(); ++a) {
        const size_t i = valid[a];
        if ((*masks[i])[off]) continue;
        for (size_t b = a + 1; b < valid.size(); ++b) {
          const size_t j = valid[b];
          if ((*masks[j])[off]) continue;
          pairs_[i * n + j].add(static_cast<int32_t>((*planes[i])[off]) -
                                static_cast<int32_t>((*planes[j])[off]));
        }
      }
    }
  }
}

std::vector<std::optional<double>> PairNoiseMeter::pair_variances() const {
  const size_t n = inputs_;
  std::vector<std::optional<double>> variance(n * n);
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      variance[i * n + j] = pairs_[i * n + j].robust_variance();
    }
  }
  return variance;
}

// ── Pressing noise model ────────────────────────────────────────────────────

std::vector<std::optional<PressingNoise>> fit_pressing_noise(
    size_t n, const std::vector<std::optional<double>>& pair_variance) {
  std::vector<std::optional<PressingNoise>> result(n);

  std::vector<size_t> members;
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = 0; j < n; ++j) {
      if (i == j) continue;
      if (pair_variance[std::min(i, j) * n + std::max(i, j)]) {
        members.push_back(i);
        break;
      }
    }
  }
  const size_t m = members.size();
  if (m < 2) return result;

  const std::vector<size_t> group =
      group_by_pressing(n, members, pair_variance);
  const size_t groups = *std::max_element(group.begin(), group.end()) + 1;
  std::vector<size_t> group_size(groups, 0);
  for (size_t g : group) ++group_size[g];

  // Unknowns: each member's own noise E, then the shared noise G of each
  // pressing with more than one capture (a lone capture's is part of its E).
  std::vector<std::optional<size_t>> shared(groups);
  size_t unknowns = m;
  for (size_t g = 0; g < groups; ++g) {
    if (group_size[g] >= 2) shared[g] = unknowns++;
  }

  std::vector<std::vector<double>> rows;
  std::vector<double> rhs;
  for (size_t p = 0; p < m; ++p) {
    for (size_t q = p + 1; q < m; ++q) {
      const std::optional<double> v =
          pair_variance[members[p] * n + members[q]];
      if (!v) continue;
      std::vector<double> row(unknowns, 0.0);
      row[p] = 1.0;
      row[q] = 1.0;
      if (group[p] != group[q]) {
        const std::optional<size_t> gp = shared[group[p]];
        const std::optional<size_t> gq = shared[group[q]];
        if (gp) row[*gp] = 1.0;
        if (gq) row[*gq] = 1.0;
      }
      rows.push_back(std::move(row));
      rhs.push_back(*v);
    }
  }
  for (size_t g = 0; g < groups; ++g) {
    const std::optional<size_t> index = shared[g];
    if (!index) continue;
    std::vector<double> row(unknowns, 0.0);
    row[*index] = kNoiseRidge;
    rows.push_back(std::move(row));
    rhs.push_back(0.0);
  }

  std::vector<double> own(m, 0.0);
  std::vector<double> pressing(groups, 0.0);
  const auto solution = least_squares(rows, rhs, unknowns);
  if (solution) {
    for (size_t p = 0; p < m; ++p) own[p] = (*solution)[p];
    for (size_t g = 0; g < groups; ++g) {
      const std::optional<size_t> index = shared[g];
      if (index) pressing[g] = std::max(0.0, (*solution)[*index]);
    }
  } else {
    // Two captures, or pairs that leave the system singular, cannot separate
    // their noise, so each capture takes half the mean variance of its pairs.
    for (size_t p = 0; p < m; ++p) {
      double sum = 0.0;
      double pairs = 0.0;
      for (size_t q = 0; q < m; ++q) {
        if (q == p) continue;
        const std::optional<double> v =
            pair_variance[std::min(members[p], members[q]) * n +
                          std::max(members[p], members[q])];
        if (!v) continue;
        sum += *v;
        pairs += 1.0;
      }
      own[p] = pairs > 0.0 ? sum / pairs / 2.0 : 0.0;
    }
  }
  for (double& e : own) {
    e = std::max(e, kMinPressingNoiseSigma * kMinPressingNoiseSigma);
  }

  // The captures of a pressing share its noise G, so the weights that
  // minimise the combined noise are, with S the sum of 1/E over the pressing,
  // (1/E_i) / (1 + G·S): each pressing counts roughly once however many of its
  // discs are stacked, and within it the cleaner discs count more.
  std::vector<double> inverse_sum(groups, 0.0);
  for (size_t p = 0; p < m; ++p) inverse_sum[group[p]] += 1.0 / own[p];
  for (size_t p = 0; p < m; ++p) {
    const size_t g = group[p];
    PressingNoise noise;
    noise.sigma = std::sqrt(own[p] + pressing[g]);
    noise.weight = (1.0 / own[p]) / (1.0 + pressing[g] * inverse_sum[g]);
    noise.group = g;
    result[members[p]] = noise;
  }
  return result;
}

}  // namespace orc
