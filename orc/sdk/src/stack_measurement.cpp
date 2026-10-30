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
#include <map>

namespace orc {

namespace {

// Raw-sample refinement window around the low-passed peak.
constexpr int32_t kFineSearch = 2;

// Whole lags either side used to interpolate the raw correlation between
// them, and the Kaiser window's beta over that span.
constexpr int32_t kFractionTaps = 8;
constexpr double kFractionKaiserBeta = 6.0;

// Step of the search for the interpolated peak, in samples.
constexpr double kFractionStep = 0.005;

constexpr double kPi = 3.14159265358979323846;

// Modified Bessel function of the first kind, order 0, by its power series
// (libc++ has no std::cyl_bessel_i).  Converges fast for the window's small
// arguments.
double bessel_i0(double x) {
  double sum = 1.0;
  double term = 1.0;
  const double q = x * x / 4.0;
  for (int k = 1; k < 50 && term > 1e-12 * sum; ++k) {
    term *= q / (static_cast<double>(k) * static_cast<double>(k));
    sum += term;
  }
  return sum;
}

// Kaiser window of |beta| at |x| in [-1, 1].
double kaiser(double x, double beta) {
  const double t = 1.0 - x * x;
  if (t <= 0.0) return 0.0;
  return bessel_i0(beta * std::sqrt(t)) / bessel_i0(beta);
}

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

// Noise blocks: decimated rows and columns, so 8 lines by 32 samples.
constexpr size_t kBlockRows = 2;
constexpr size_t kBlockColumns = 16;

// A block is flat when the spread of the common picture within it is no
// more than this many times that of the flattest tenth of the frame's
// blocks.  The spread of pure noise over 32 samples varies by about a
// quarter, so on a flat frame every block passes; on a busy one only blocks
// whose picture adds less than about twice the common picture's noise do.
constexpr double kFlatBlockQuantile = 0.1;
constexpr double kFlatBlockSpread = 4.0;

// Fewest blocks a frame needs before its flat ones can be told apart.
constexpr size_t kMinFlatBlocks = 20;

// A frame's flat blocks are used only when its flattest tenth is no more
// than this many times as wide as the flattest probe frame's.  A frame of
// film or saturated colour has no block as flat as a blank frame's, and its
// flattest tenth still carries the captures' disagreement over grain and
// colour: on Community South a run of colourful frames read pair variances
// of 1000 against 52 on the frames either side, and split one pressing into
// six.
constexpr double kFlatFrameSpread = 4.0;

// A pair's block variance more than this many times its median over the
// frame's flat blocks holds sparkle or an edge rather than noise.
constexpr double kBlockOutlier = 4.0;

// Spread of the common low-passed picture, in codes, below which the frames
// cannot show a capture's gain.
constexpr double kMinLevelSpread = 20.0;

// Spread of v_ik − v_jk over the other captures k, as a fraction of the
// median pair variance, below which captures i and j are taken to share a
// pressing.
constexpr double kPressingSpread = 0.25;

// Weight of the rows that settle what the pair variances leave open: with two
// pressings their shared noise appears only as a sum, which these split
// evenly; a pressing beside only lone captures has shared noise that cannot
// be told from theirs, which these leave with them.
constexpr double kNoiseRidge = 1e-3;

// Smallest own-noise variance a capture is given.
constexpr double kMinPressingVariance =
    kMinPressingNoiseSigma * kMinPressingNoiseSigma;

// Reweighted solves of the noise model.  A pair variance is measured to an
// accuracy proportional to its size, so each pair is weighted by the inverse
// of the variance the last solve models for it.  Solving with equal weights
// lets one capture whose differences carry something besides noise steer the
// others: on real captures a capture whose chroma did not line up with the
// rest over a few hundred frames (pair variances ~1000 against ~150) drove
// another capture's noise to the floor and gave it 93 % of the weight.
// Weighted this way the odd capture's pairs are fitted loosely and the rest
// keep their usual noise, while sets of normal captures solve as before.
constexpr size_t kRelativeFitPasses = 8;

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

// Solution of the |unknowns| equations whose rows, each with its right-hand
// side appended, are |a|; nullopt when they are singular.  Gauss-Jordan
// elimination with partial pivoting.
std::optional<std::vector<double>> solve_linear(
    std::vector<std::vector<double>> a, size_t unknowns) {
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
  return solve_linear(std::move(a), unknowns);
}

// The model's covariance of two captures' noise: what they share of their
// pressing's and their family's, and on the diagonal all three terms.
// Captures held from neighbouring fits may carry slightly different figures
// for one pressing, so the shared terms are averaged to keep it symmetric.
double model_covariance(const PressingNoise& a, const PressingNoise& b,
                        bool same) {
  if (same) return a.own_variance + a.shared_variance + a.family_variance;
  double c = 0.0;
  if (a.family == b.family) {
    c += 0.5 * (a.family_variance + b.family_variance);
    if (a.group == b.group) c += 0.5 * (a.shared_variance + b.shared_variance);
  }
  return c;
}

// The variance the model gives the difference of two captures.
double model_pair_variance(const PressingNoise& a, const PressingNoise& b) {
  return model_covariance(a, a, true) + model_covariance(b, b, true) -
         2.0 * model_covariance(a, b, false);
}

// Half the mean variance of capture i's measured pairs, or 0 with none.
double half_mean_pair_variance(
    size_t n, const std::vector<std::optional<double>>& pair_variance,
    size_t i) {
  double sum = 0.0;
  double pairs = 0.0;
  for (size_t j = 0; j < n; ++j) {
    if (j == i) continue;
    const std::optional<double> v =
        pair_variance[std::min(i, j) * n + std::max(i, j)];
    if (!v) continue;
    sum += *v;
    pairs += 1.0;
  }
  return pairs > 0.0 ? sum / pairs / 2.0 : 0.0;
}

// The captures in at least one measured pair, in order.
std::vector<size_t> measured_captures(
    size_t n, const std::vector<std::optional<double>>& pair_variance) {
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
  return members;
}

// Group |members| by pressing from the spread of v_ik − v_jk; returns each
// member's group, numbered from 0.  |pair_variance| is indexed i * n + j for
// i < j.  Fewer than 4 captures cannot show a jump, so they form one group.
std::vector<size_t> group_members_by_pressing(
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
    : coarse_(kMaxSampleGridShift),
      fine_(kMaxSampleGridShift + std::max(kFineSearch, kFractionTaps + 1)) {}

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

  // The raw correlation is that of two band-limited signals, so it is itself
  // band-limited and its whole-lag values determine it between them:
  // windowed-sinc interpolation finds its peak to a fraction of a sample.
  auto interpolated = [this](double tau) {
    const auto centre = static_cast<int32_t>(std::lround(tau));
    double sum = 0.0;
    for (int32_t k = centre - kFractionTaps; k <= centre + kFractionTaps; ++k) {
      if (k < -fine_.max_lag || k > fine_.max_lag) continue;
      const auto c = fine_.at(k);
      if (!c) continue;
      const double x = tau - k;
      const double sinc =
          std::abs(x) < 1e-12 ? 1.0 : std::sin(kPi * x) / (kPi * x);
      sum += *c * sinc * kaiser(x / (kFractionTaps + 1), kFractionKaiserBeta);
    }
    return sum;
  };
  double best_tau = best_lag;
  double best_value = interpolated(best_tau);
  for (double tau = best_lag - 1.0; tau <= best_lag + 1.0;
       tau += kFractionStep) {
    const double value = interpolated(tau);
    if (value > best_value) {
      best_value = value;
      best_tau = tau;
    }
  }
  result.fractional_offset = best_tau;
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

LumaLevelMeter::LumaLevelMeter(size_t inputs)
    : inputs_(inputs), sums_(inputs) {}

void LumaLevelMeter::add_frame(
    const std::vector<const std::vector<StackSample>*>& planes,
    const std::vector<const std::vector<uint8_t>*>& masks, VideoSystem system,
    size_t nominal_width, size_t height) {
  const size_t frame_samples =
      frame_line_sample_offset(system, nominal_width, height);
  std::vector<size_t> valid;
  for (size_t i = 0; i < inputs_ && i < planes.size() && i < masks.size();
       ++i) {
    if (!planes[i] || !masks[i]) continue;
    if (planes[i]->size() < frame_samples || masks[i]->size() < frame_samples) {
      continue;
    }
    valid.push_back(i);
  }
  if (valid.size() < 2) return;
  const size_t v = valid.size();

  // Low-passed luma (4 samples, one subcarrier cycle) against the mean of
  // every capture's, wherever none is flagged.
  std::vector<double> luma(v);
  const size_t field1_lines = std::min(field1_line_count(system), height);
  for (size_t line = 0; line < height; line += kNoiseLineStep) {
    const size_t field = line < field1_lines ? 0 : 1;
    if (line - field * field1_lines < kFieldBlankingLines) continue;
    const size_t start = frame_line_sample_offset(system, nominal_width, line);
    const size_t length = frame_line_sample_count(system, nominal_width, line);
    const size_t first = active_first(length);
    const size_t last = active_last(length);
    for (size_t x = first; x + kGridLumaBox <= last; x += kNoiseSampleStep) {
      const size_t off = start + x;
      bool all_clear = true;
      for (size_t a = 0; a < v && all_clear; ++a) {
        all_clear = (*masks[valid[a]])[off] == 0;
      }
      if (!all_clear) continue;
      double common = 0.0;
      for (size_t a = 0; a < v; ++a) {
        const auto& plane = *planes[valid[a]];
        double sum = 0.0;
        for (size_t k = 0; k < kGridLumaBox; ++k) sum += plane[off + k];
        luma[a] = sum / static_cast<double>(kGridLumaBox);
        common += luma[a];
      }
      common /= static_cast<double>(v);
      for (size_t a = 0; a < v; ++a) {
        LevelSums& sums = sums_[valid[a]];
        sums.n += 1.0;
        sums.x += common;
        sums.y += luma[a];
        sums.xx += common * common;
        sums.xy += common * luma[a];
      }
    }
  }
}

void apply_luma_level(std::vector<StackSample>& plane,
                      const LevelMatch& level) {
  if (level.gain <= 0.0) return;
  const double scale = 1.0 / level.gain;
  const double offset = level.mean - (level.mean + level.level) * scale;
  for (StackSample& s : plane) {
    const double mapped = std::round(static_cast<double>(s) * scale + offset);
    s = static_cast<StackSample>(std::clamp(mapped,
                                            static_cast<double>(INT16_MIN),
                                            static_cast<double>(INT16_MAX)));
  }
}

PairNoiseMeter::PairNoiseMeter(size_t inputs)
    : inputs_(inputs), pairs_(inputs * inputs), levels_(inputs) {}

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
  const size_t v = valid.size();

  // The decimated rows of active picture, each with its field.
  struct Row {
    size_t start;
    size_t length;
    size_t field;
  };
  std::vector<Row> rows;
  const size_t field1_lines = std::min(field1_line_count(system), height);
  for (size_t line = 0; line < height; line += kNoiseLineStep) {
    const size_t field = line < field1_lines ? 0 : 1;
    const size_t in_field = line - field * field1_lines;
    if (in_field < kFieldBlankingLines) continue;
    rows.push_back({frame_line_sample_offset(system, nominal_width, line),
                    frame_line_sample_count(system, nominal_width, line),
                    field});
  }

  // Whole-picture differences, the fallback, and each capture's level.
  for (const Row& row : rows) {
    const size_t first = active_first(row.length);
    const size_t last = active_last(row.length);
    for (size_t x = first; x < last; x += kNoiseSampleStep) {
      const size_t off = row.start + x;
      for (size_t a = 0; a < v; ++a) {
        const size_t i = valid[a];
        if ((*masks[i])[off]) continue;
        for (size_t b = a + 1; b < v; ++b) {
          const size_t j = valid[b];
          if ((*masks[j])[off]) continue;
          pairs_[i * n + j].add(static_cast<int32_t>((*planes[i])[off]) -
                                static_cast<int32_t>((*planes[j])[off]));
        }
      }
    }
  }
  levels_.add_frame(planes, masks, system, nominal_width, height);

  // Blocks of kBlockRows rows of one field by kBlockColumns decimated
  // samples, and the spread of the common picture within each.
  struct Block {
    std::vector<size_t> offsets;
    std::vector<uint8_t> clear;  // per valid capture: no sample flagged
    double spread = 0.0;
  };
  std::vector<Block> blocks;
  std::vector<double> common;
  for (size_t r = 0; r + kBlockRows <= rows.size(); r += kBlockRows) {
    bool one_field = true;
    size_t length = rows[r].length;
    for (size_t k = 1; k < kBlockRows; ++k) {
      one_field = one_field && rows[r + k].field == rows[r].field;
      length = std::min(length, rows[r + k].length);
    }
    if (!one_field) continue;
    const size_t first = active_first(length);
    const size_t last = active_last(length);
    const size_t width = kBlockColumns * kNoiseSampleStep;
    for (size_t bx = first; bx + width <= last; bx += width) {
      Block block;
      for (size_t k = 0; k < kBlockRows; ++k) {
        for (size_t x = bx; x < bx + width; x += kNoiseSampleStep) {
          block.offsets.push_back(rows[r + k].start + x);
        }
      }
      block.clear.assign(v, 1);
      size_t clear = 0;
      for (size_t a = 0; a < v; ++a) {
        const auto& mask = *masks[valid[a]];
        for (size_t off : block.offsets) {
          if (mask[off]) {
            block.clear[a] = 0;
            break;
          }
        }
        clear += block.clear[a];
      }
      if (clear < 2) continue;

      common.assign(block.offsets.size(), 0.0);
      for (size_t a = 0; a < v; ++a) {
        if (!block.clear[a]) continue;
        const auto& plane = *planes[valid[a]];
        for (size_t k = 0; k < block.offsets.size(); ++k) {
          common[k] += plane[block.offsets[k]];
        }
      }
      double sum = 0.0;
      double sum_sq = 0.0;
      for (double& c : common) {
        c /= static_cast<double>(clear);
        sum += c;
        sum_sq += c * c;
      }
      const auto count = static_cast<double>(common.size());
      block.spread = (sum_sq - sum * sum / count) / (count - 1.0);
      blocks.push_back(std::move(block));
    }
  }
  if (blocks.size() < kMinFlatBlocks) return;

  std::vector<double> spreads;
  spreads.reserve(blocks.size());
  for (const Block& block : blocks) spreads.push_back(block.spread);
  const auto nth = spreads.begin() + static_cast<std::ptrdiff_t>(
                                         kFlatBlockQuantile *
                                         static_cast<double>(spreads.size()));
  std::nth_element(spreads.begin(), nth, spreads.end());
  const double flat_limit = kFlatBlockSpread * *nth;

  // Each pair's sums in every flat block: the difference d about its block
  // mean, and the pair's low-passed luma l (one subcarrier cycle, the mean
  // of both captures') about its own, from which a luma gain difference k
  // is taken out at the end as var(d − k·l).  Pooled over the blocks whose
  // variance is not an outlier for that pair.
  std::vector<std::vector<FlatVariance>> block_sums(v * v);
  for (const Block& block : blocks) {
    if (block.spread > flat_limit) continue;
    const auto count = static_cast<double>(block.offsets.size());
    for (size_t a = 0; a < v; ++a) {
      if (!block.clear[a]) continue;
      const auto& pa = *planes[valid[a]];
      for (size_t b = a + 1; b < v; ++b) {
        if (!block.clear[b]) continue;
        const auto& pb = *planes[valid[b]];
        double sd = 0.0;
        double sl = 0.0;
        double sdd = 0.0;
        double sdl = 0.0;
        double sll = 0.0;
        for (size_t off : block.offsets) {
          const double d = static_cast<double>(pb[off]) - pa[off];
          double l = 0.0;
          for (size_t k = 0; k < kGridLumaBox; ++k) {
            l += static_cast<double>(pa[off + k]) + pb[off + k];
          }
          l /= static_cast<double>(2 * kGridLumaBox);
          sd += d;
          sl += l;
          sdd += d * d;
          sdl += d * l;
          sll += l * l;
        }
        FlatVariance sums;
        sums.dd = sdd - sd * sd / count;
        sums.dl = sdl - sd * sl / count;
        sums.ll = sll - sl * sl / count;
        sums.dof = count - 1.0;
        block_sums[a * v + b].push_back(sums);
      }
    }
  }
  FlatFrame frame;
  frame.spread = *nth;
  frame.pairs.resize(n * n);
  std::vector<double> variances;
  for (size_t a = 0; a < v; ++a) {
    for (size_t b = a + 1; b < v; ++b) {
      const std::vector<FlatVariance>& sums = block_sums[a * v + b];
      if (sums.empty()) continue;
      variances.clear();
      for (const FlatVariance& block : sums) {
        variances.push_back(block.dd / block.dof);
      }
      const double limit = kBlockOutlier * median_of(variances);
      FlatVariance& flat = frame.pairs[valid[a] * n + valid[b]];
      for (const FlatVariance& block : sums) {
        if (block.dd / block.dof > limit) continue;
        flat.dd += block.dd;
        flat.dl += block.dl;
        flat.ll += block.ll;
        flat.dof += block.dof;
      }
    }
  }
  flat_frames_.push_back(std::move(frame));
}

std::vector<PairNoiseMeter::FlatVariance> PairNoiseMeter::flat_variances()
    const {
  const size_t n = inputs_;
  std::vector<FlatVariance> pooled(n * n);
  if (flat_frames_.empty()) return pooled;
  double flattest = flat_frames_.front().spread;
  for (const FlatFrame& frame : flat_frames_) {
    flattest = std::min(flattest, frame.spread);
  }
  for (const FlatFrame& frame : flat_frames_) {
    if (frame.spread > kFlatFrameSpread * flattest) continue;
    for (size_t k = 0; k < n * n; ++k) {
      pooled[k].dd += frame.pairs[k].dd;
      pooled[k].dl += frame.pairs[k].dl;
      pooled[k].ll += frame.pairs[k].ll;
      pooled[k].dof += frame.pairs[k].dof;
    }
  }
  return pooled;
}

std::vector<std::optional<double>> PairNoiseMeter::pair_variances() const {
  const size_t n = inputs_;
  const std::vector<FlatVariance> flat_pairs = flat_variances();
  const std::vector<std::optional<LevelMatch>> level = levels();
  std::vector<std::optional<double>> variance(n * n);
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      const FlatVariance& flat = flat_pairs[i * n + j];
      if (flat.dof < static_cast<double>(kMinNoiseSamples)) {
        variance[i * n + j] = pairs_[i * n + j].robust_variance();
        continue;
      }
      // The luma gain difference between the pair, which leaves whatever
      // picture a block holds in its difference.
      double k = 0.0;
      const std::optional<LevelMatch>& li = level[i];
      const std::optional<LevelMatch>& lj = level[j];
      if (li && lj) k = 2.0 * (lj->gain - li->gain) / (li->gain + lj->gain);
      variance[i * n + j] =
          std::max(0.0, flat.dd - 2.0 * k * flat.dl + k * k * flat.ll) /
          flat.dof;
    }
  }
  return variance;
}

size_t PairNoiseMeter::whole_picture_pairs() const {
  const size_t n = inputs_;
  const std::vector<FlatVariance> flat_pairs = flat_variances();
  size_t count = 0;
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      if (flat_pairs[i * n + j].dof < static_cast<double>(kMinNoiseSamples) &&
          pairs_[i * n + j].robust_variance()) {
        ++count;
      }
    }
  }
  return count;
}

std::vector<std::optional<LevelMatch>> LumaLevelMeter::levels() const {
  std::vector<std::optional<LevelMatch>> result(inputs_);
  std::vector<double> gains;
  std::vector<double> offsets;
  for (size_t i = 0; i < inputs_; ++i) {
    const LevelSums& s = sums_[i];
    if (s.n < static_cast<double>(kMinNoiseSamples)) continue;
    const double mean_x = s.x / s.n;
    const double mean_y = s.y / s.n;
    const double var_x = s.xx / s.n - mean_x * mean_x;
    if (var_x < kMinLevelSpread * kMinLevelSpread) continue;
    LevelMatch level;
    level.gain = (s.xy / s.n - mean_x * mean_y) / var_x;
    level.level = mean_y - mean_x;
    level.mean = mean_x;
    gains.push_back(level.gain);
    offsets.push_back(level.level);
    result[i] = level;
  }
  if (gains.empty()) return result;

  // Against the typical capture, so that an odd capture's pull on the common
  // picture, and the share of each capture's own noise in it, which lean
  // every fit the same way, cancel.
  const double typical_gain = median_of(gains);
  const double typical_level = median_of(offsets);
  for (auto& level : result) {
    if (!level) continue;
    level->gain /= typical_gain;
    level->level -= typical_level;
  }
  return result;
}

// ── Pressing noise model ────────────────────────────────────────────────────

std::vector<std::optional<size_t>> group_by_pressing(
    size_t n, const std::vector<std::optional<double>>& pair_variance) {
  std::vector<std::optional<size_t>> pressing(n);
  const std::vector<size_t> members = measured_captures(n, pair_variance);
  const std::vector<size_t> group =
      group_members_by_pressing(n, members, pair_variance);
  for (size_t p = 0; p < members.size(); ++p) pressing[members[p]] = group[p];
  return pressing;
}

std::vector<std::optional<PressingNoise>> fit_pressing_noise(
    size_t n, const std::vector<std::optional<double>>& pair_variance) {
  const std::vector<std::optional<size_t>> groups =
      group_by_pressing(n, pair_variance);
  return fit_pressing_noise(n, pair_variance, groups,
                            group_into_families(n, pair_variance, groups));
}

std::vector<std::optional<PressingNoise>> fit_pressing_noise(
    size_t n, const std::vector<std::optional<double>>& pair_variance,
    const std::vector<std::optional<size_t>>& groups_given) {
  return fit_pressing_noise(n, pair_variance, groups_given, groups_given);
}

std::vector<std::optional<size_t>> group_into_families(
    size_t n, const std::vector<std::optional<double>>& pair_variance,
    const std::vector<std::optional<size_t>>& groups) {
  struct Candidate {
    std::vector<std::optional<size_t>> family;
    double misfit = 0.0;
    double clock = 0.0;
  };
  auto evaluate = [&](std::vector<std::optional<size_t>> families) {
    const auto noise = fit_pressing_noise(n, pair_variance, groups, families);
    Candidate candidate;
    candidate.family = std::move(families);
    candidate.misfit = check_pressing_fit(n, pair_variance, noise).worst_misfit;
    // Spread of ln σ² over the captures.
    double sum = 0.0;
    double sum_sq = 0.0;
    double count = 0.0;
    for (const auto& capture : noise) {
      if (!capture) continue;
      const double l = std::log(capture->sigma * capture->sigma);
      sum += l;
      sum_sq += l * l;
      count += 1.0;
    }
    if (count > 0.0) {
      const double mean = sum / count;
      candidate.clock = std::sqrt(std::max(0.0, sum_sq / count - mean * mean));
    }
    return candidate;
  };
  std::vector<std::optional<size_t>> family = groups;
  double current = evaluate(family).misfit;
  for (;;) {
    std::vector<size_t> labels;
    for (const auto& f : family) {
      if (f && std::find(labels.begin(), labels.end(), *f) == labels.end()) {
        labels.push_back(*f);
      }
    }
    std::vector<Candidate> merges;
    double best_misfit = current - kFamilyMisfitGain;
    for (size_t a = 0; a < labels.size(); ++a) {
      for (size_t b = a + 1; b < labels.size(); ++b) {
        std::vector<std::optional<size_t>> merged = family;
        for (auto& f : merged) {
          if (f && *f == labels[b]) f = labels[a];
        }
        merges.push_back(evaluate(std::move(merged)));
        best_misfit = std::min(best_misfit, merges.back().misfit);
      }
    }
    if (best_misfit >= current - kFamilyMisfitGain) break;

    // Of the merges that fit about as well as the best, the one that leaves
    // the captures' noise most alike.
    const Candidate* chosen = nullptr;
    for (const Candidate& candidate : merges) {
      if (candidate.misfit > best_misfit + kFamilyMisfitGain) continue;
      if (!chosen || candidate.clock < chosen->clock) chosen = &candidate;
    }
    family = chosen->family;
    current = chosen->misfit;
  }

  // Number the families from 0 in order of their first capture.
  std::map<size_t, size_t> numbering;
  for (auto& f : family) {
    if (f) f = numbering.emplace(*f, numbering.size()).first->second;
  }
  return family;
}

std::vector<double> pressing_weights(
    const std::vector<PressingNoise>& captures) {
  const size_t m = captures.size();
  std::vector<std::vector<double>> a(m, std::vector<double>(m + 1, 1.0));
  for (size_t p = 0; p < m; ++p) {
    for (size_t q = 0; q < m; ++q) {
      a[p][q] = model_covariance(captures[p], captures[q], p == q);
    }
  }
  std::optional<std::vector<double>> weights = solve_linear(std::move(a), m);
  if (!weights) {
    weights.emplace(m);
    for (size_t p = 0; p < m; ++p) {
      (*weights)[p] =
          1.0 / std::max(model_covariance(captures[p], captures[p], true),
                         kMinPressingVariance);
    }
  }
  // Under a covariance of this form every entry is positive; rounding in a
  // near-singular one is not allowed to make one negative.
  for (double& w : *weights) w = std::max(w, 0.0);
  return *weights;
}

std::string pressing_name(size_t group) {
  std::string name(1, static_cast<char>('A' + group % 26));
  if (group >= 26) name += std::to_string(group / 26 + 1);
  return name;
}

double combined_sigma(const std::vector<PressingNoise>& captures) {
  if (captures.empty()) return 0.0;
  // pressing_weights() solves Σw = 1, so its weights sum to 1ᵀΣ⁻¹1.
  double total = 0.0;
  for (double w : pressing_weights(captures)) total += w;
  return total > 0.0 ? std::sqrt(1.0 / total) : 0.0;
}

std::vector<std::optional<PressingNoise>> fit_pressing_noise(
    size_t n, const std::vector<std::optional<double>>& pair_variance,
    const std::vector<std::optional<size_t>>& groups_given,
    const std::vector<std::optional<size_t>>& families_given) {
  std::vector<std::optional<PressingNoise>> result(n);

  const std::vector<size_t> members = measured_captures(n, pair_variance);
  const size_t m = members.size();
  if (m < 2) return result;

  // Number the members' pressings and families from 0, giving a member
  // without one a group or family of its own.  A pressing takes the family
  // of its first member.
  auto number = [&](const std::vector<std::optional<size_t>>& given) {
    std::vector<size_t> label(m);
    std::map<size_t, size_t> numbering;
    std::vector<bool> labelled(m, false);
    for (size_t p = 0; p < m; ++p) {
      const size_t i = members[p];
      const std::optional<size_t> g =
          i < given.size() ? given[i] : std::nullopt;
      if (g) {
        label[p] = numbering.emplace(*g, numbering.size()).first->second;
        labelled[p] = true;
      }
    }
    size_t count = numbering.size();
    for (size_t p = 0; p < m; ++p) {
      if (!labelled[p]) label[p] = count++;
    }
    return std::make_pair(label, count);
  };
  const auto [group, groups] = number(groups_given);
  const auto [family_of_member, families] = number(families_given);
  std::vector<size_t> family(groups, 0);
  for (size_t p = m; p-- > 0;) family[group[p]] = family_of_member[p];
  std::vector<size_t> group_size(groups, 0);
  for (size_t g : group) ++group_size[g];
  std::vector<size_t> family_size(families, 0);
  for (size_t g = 0; g < groups; ++g) {
    if (group_size[g] > 0) ++family_size[family[g]];
  }

  // Unknowns: each member's own noise E, then the shared noise G of each
  // pressing with more than one capture (a lone capture's is part of its E),
  // then the noise F of each family with more than one pressing (a lone
  // pressing's is part of its G, or of its capture's E).
  std::vector<std::optional<size_t>> shared(groups);
  size_t unknowns = m;
  for (size_t g = 0; g < groups; ++g) {
    if (group_size[g] >= 2) shared[g] = unknowns++;
  }
  std::vector<std::optional<size_t>> mother(families);
  for (size_t f = 0; f < families; ++f) {
    if (family_size[f] >= 2) mother[f] = unknowns++;
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
        const size_t fp = family[group[p]];
        const size_t fq = family[group[q]];
        if (fp != fq) {
          if (const std::optional<size_t>& mp = mother[fp]) row[*mp] = 1.0;
          if (const std::optional<size_t>& mq = mother[fq]) row[*mq] = 1.0;
        }
      }
      rows.push_back(std::move(row));
      rhs.push_back(*v);
    }
  }
  for (size_t u = m; u < unknowns; ++u) {
    std::vector<double> row(unknowns, 0.0);
    row[u] = kNoiseRidge;
    rows.push_back(std::move(row));
    rhs.push_back(0.0);
  }

  std::vector<double> own(m, 0.0);
  std::vector<double> pressing(groups, 0.0);
  std::vector<double> lineage(families, 0.0);
  std::optional<std::vector<double>> solution =
      least_squares(rows, rhs, unknowns);
  const size_t pair_rows = rhs.size() - (unknowns - m);
  for (size_t pass = 0; solution && pass < kRelativeFitPasses; ++pass) {
    // Weights relative to their mean, so the ridge rows keep their effect.
    std::vector<double> weight(pair_rows);
    double weight_sum = 0.0;
    for (size_t r = 0; r < pair_rows; ++r) {
      double modelled = 0.0;
      for (size_t u = 0; u < unknowns; ++u) {
        const double x = u < m ? std::max((*solution)[u], kMinPressingVariance)
                               : std::max((*solution)[u], 0.0);
        modelled += rows[r][u] * x;
      }
      weight[r] = 1.0 / modelled;
      weight_sum += weight[r];
    }
    std::vector<std::vector<double>> weighted_rows = rows;
    std::vector<double> weighted_rhs = rhs;
    for (size_t r = 0; r < pair_rows; ++r) {
      const double w = weight[r] * static_cast<double>(pair_rows) / weight_sum;
      for (double& a : weighted_rows[r]) a *= w;
      weighted_rhs[r] *= w;
    }
    auto refined = least_squares(weighted_rows, weighted_rhs, unknowns);
    if (!refined) break;
    solution = std::move(refined);
  }
  if (solution) {
    for (size_t p = 0; p < m; ++p) own[p] = (*solution)[p];
    for (size_t g = 0; g < groups; ++g) {
      const std::optional<size_t> index = shared[g];
      if (index) pressing[g] = std::max(0.0, (*solution)[*index]);
    }
    for (size_t f = 0; f < families; ++f) {
      const std::optional<size_t> index = mother[f];
      if (index) lineage[f] = std::max(0.0, (*solution)[*index]);
    }
  } else {
    // Two captures, or pairs that leave the system singular, cannot separate
    // their noise, so each capture takes half the mean variance of its pairs.
    for (size_t p = 0; p < m; ++p) {
      own[p] = half_mean_pair_variance(n, pair_variance, members[p]);
    }
  }
  for (double& e : own) e = std::max(e, kMinPressingVariance);

  // The captures of a pressing share its noise G, and those of a family its
  // noise F, so the weights that minimise the combined noise are Σ⁻¹1: with
  // no families, and S the sum of 1/E over a pressing, (1/E_i) / (1 + G·S).
  // Each pressing counts roughly once however many of its discs are stacked,
  // each family roughly once however many of its pressings, and within them
  // the cleaner discs count more.
  std::vector<PressingNoise> fitted(m);
  for (size_t p = 0; p < m; ++p) {
    const size_t g = group[p];
    PressingNoise& noise = fitted[p];
    noise.group = g;
    noise.family = family[g];
    noise.own_variance = own[p];
    noise.shared_variance = pressing[g];
    noise.family_variance = lineage[family[g]];
    noise.sigma = std::sqrt(own[p] + pressing[g] + lineage[family[g]]);
  }
  const std::vector<double> weights = pressing_weights(fitted);
  for (size_t p = 0; p < m; ++p) {
    fitted[p].weight = weights[p];
    result[members[p]] = fitted[p];
  }
  return result;
}

// ── Checking the fit ────────────────────────────────────────────────────────

PressingFitCheck check_pressing_fit(
    size_t n, const std::vector<std::optional<double>>& pair_variance,
    const std::vector<std::optional<PressingNoise>>& noise) {
  PressingFitCheck check;
  struct Member {
    size_t index;
    PressingNoise noise;
  };
  std::vector<Member> members;
  for (size_t i = 0; i < n && i < noise.size(); ++i) {
    if (const std::optional<PressingNoise>& capture = noise[i]) {
      members.push_back({i, *capture});
    }
  }
  if (members.empty()) return check;

  // Pairs: the model's E_i + E_j, plus both pressings' G across pressings.
  double sum_sq = 0.0;
  for (size_t p = 0; p < members.size(); ++p) {
    for (size_t q = p + 1; q < members.size(); ++q) {
      const Member& a = members[p];
      const Member& b = members[q];
      const std::optional<double> measured =
          pair_variance[a.index * n + b.index];
      if (!measured || *measured <= 0.0) continue;
      const double modelled = model_pair_variance(a.noise, b.noise);
      const double misfit = std::abs(std::log(*measured / modelled));
      sum_sq += misfit * misfit;
      ++check.pairs;
      if (misfit > check.worst_misfit) {
        check.worst_misfit = misfit;
        check.worst_i = a.index;
        check.worst_j = b.index;
      }
    }
  }
  if (check.pairs > 0) {
    check.rms_misfit = std::sqrt(sum_sq / static_cast<double>(check.pairs));
  }

  // Variance of Σ wᵢxᵢ (Σ w = 1) about the common picture: wᵀΣw.
  auto predicted = [&](const std::vector<double>& weights) {
    double total = 0.0;
    for (double w : weights) total += w;
    double variance = 0.0;
    for (size_t p = 0; p < members.size(); ++p) {
      for (size_t q = 0; q < members.size(); ++q) {
        variance +=
            weights[p] * weights[q] *
            model_covariance(members[p].noise, members[q].noise, p == q);
      }
    }
    return std::sqrt(variance) / total;
  };
  std::vector<double> weighted;
  for (const Member& m : members) weighted.push_back(m.noise.weight);
  check.weighted_sigma = predicted(weighted);
  check.equal_sigma = predicted(std::vector<double>(members.size(), 1.0));
  check.best_single_sigma = members.front().noise.sigma;
  for (const Member& m : members) {
    check.best_single_sigma = std::min(check.best_single_sigma, m.noise.sigma);
  }

  check.trusted =
      members.size() < 3 || check.worst_misfit <= kMaxPressingMisfit;
  return check;
}

std::vector<std::optional<PressingNoise>> equal_weight_noise(
    size_t n, const std::vector<std::optional<double>>& pair_variance,
    const std::vector<std::optional<PressingNoise>>& noise) {
  std::vector<std::optional<PressingNoise>> result(n);
  std::vector<double> variances;
  for (size_t i = 0; i < n && i < noise.size(); ++i) {
    const std::optional<PressingNoise>& fitted = noise[i];
    if (!fitted) continue;
    const double variance = std::max(
        half_mean_pair_variance(n, pair_variance, i), kMinPressingVariance);
    variances.push_back(variance);
    PressingNoise equal;
    equal.sigma = std::sqrt(variance);
    equal.weight = 1.0;
    equal.group = fitted->group;
    equal.family = fitted->family;
    result[i] = equal;
  }
  if (variances.empty()) return result;
  const double common = median_of(variances);
  for (auto& equal : result) {
    if (equal) equal->own_variance = common;
  }
  return result;
}

}  // namespace orc
