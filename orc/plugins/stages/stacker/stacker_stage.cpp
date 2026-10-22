/*
 * File:        stacker_stage.cpp
 * Module:      orc-core
 * Purpose:     Multi-source CVBS_U10_4FSC frame stacking stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include <fmt/format.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/preview_helpers.h>
#include <stacker_stage.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include "efm_confidence_stack.h"

namespace orc {

namespace {

// ── Sample-grid alignment ───────────────────────────────────────────────────
//
// Independent decodes of the same disc need not start a line on the same 4FSC
// sample.  ld-decode locates lines from sync, and PAL's TBC layout does not
// pin subcarrier phase to sample index the way NTSC's 910 samples/line =
// 227.5 subcarrier cycles does — PAL carries 1135.0064 samples per line and
// closes the frame with 4 extra samples (EBU Tech. 3280-E §1.2) — so two
// captures of one disc can sit several samples apart.  One 4FSC sample is 90°
// of subcarrier, so combining composite samples across sources whose grids
// differ erodes chroma rather than reinforcing it.
//
// The offset is measured on low-passed luma.  Composite repeats every 4
// samples because of the subcarrier, so correlating raw composite finds a
// true offset L almost as strongly at L±4; a 4-sample box removes the
// subcarrier and leaves one peak.  The box also flattens that peak, so the
// lag it finds is refined on the raw samples within kFineSearch of it, where
// the subcarrier makes the exact lag stand out.
//
// Real captures measure up to 12 samples apart (0.7 µs of line-start
// placement, not a frame), and the offset drifts by a sample or more across a
// side, so it is measured per segment of frames rather than once per source.

// Largest grid difference searched for, either way.
constexpr int32_t kMaxSampleShift = 32;

// Raw-sample refinement window around the low-passed peak.
constexpr int32_t kFineSearch = 2;

// Width of the box that removes the subcarrier: one cycle at 4FSC.
constexpr size_t kLumaBox = 4;

// Normalised correlation a measured offset must reach to be acted on.
constexpr double kMinAlignCorrelation = 0.5;

// Probe frames per segment, the candidate positions they are taken from, and
// lines sampled per frame, when measuring an offset.
constexpr size_t kAlignProbeFrames = 4;
constexpr size_t kAlignProbeCandidates = 8;
constexpr size_t kAlignLinesPerFrame = 8;

// How many segments either side a segment that cannot be measured looks for
// a measured one to hold the offset from.
constexpr size_t kMaxSegmentSearch = 4;

// Translate one line so that sample n takes the value previously at
// n + |offset|, replicating the edge sample into the samples this vacates.
// The vacated samples sit at the line ends, which carry sync and blanking.
void shift_line_samples(VideoFrameRepresentation::sample_type* line,
                        size_t length, int32_t offset) {
  using sample_type = VideoFrameRepresentation::sample_type;
  if (offset == 0 || length == 0) return;

  const size_t shift = static_cast<size_t>(std::abs(offset));
  if (shift >= length) {
    std::fill(line, line + length, line[offset > 0 ? length - 1 : 0]);
    return;
  }
  if (offset > 0) {
    std::memmove(line, line + shift, (length - shift) * sizeof(sample_type));
    std::fill(line + length - shift, line + length, line[length - shift - 1]);
  } else {
    std::memmove(line + shift, line, (length - shift) * sizeof(sample_type));
    std::fill(line, line + shift, line[shift]);
  }
}

// Apply shift_line_samples() to every line of a flat frame buffer, honouring
// the non-orthogonal PAL line layout.
void shift_frame_samples(
    std::vector<VideoFrameRepresentation::sample_type>& frame,
    VideoSystem system, size_t nominal_width, size_t height, int32_t offset) {
  if (offset == 0) return;
  for (size_t line = 0; line < height; ++line) {
    const size_t start = frame_line_sample_offset(system, nominal_width, line);
    const size_t length = frame_line_sample_count(system, nominal_width, line);
    if (start + length > frame.size()) break;
    shift_line_samples(frame.data() + start, length, offset);
  }
}

// Move dropout runs with the samples they describe, clamped to their own line.
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

// Running cross-correlation sums for every lag in [-max_lag, max_lag].
struct LagCorrelation {
  explicit LagCorrelation(int32_t max_lag)
      : max_lag(max_lag),
        xy(2 * static_cast<size_t>(max_lag) + 1, 0.0),
        xx(xy.size(), 0.0),
        yy(xy.size(), 0.0) {}

  // Normalised correlation at |lag|, or nullopt when either side is flat.
  std::optional<double> at(int32_t lag) const {
    const auto slot = static_cast<size_t>(lag) + static_cast<size_t>(max_lag);
    const double denominator = std::sqrt(xx[slot] * yy[slot]);
    if (denominator <= 0.0) return std::nullopt;
    return xy[slot] / denominator;
  }

  int32_t max_lag;
  std::vector<double> xy;
  std::vector<double> xx;
  std::vector<double> yy;
};

// Accumulate the correlation of reference sample n against other sample
// n + lag over [first, last).  The same reference samples are used at every
// lag so the correlations stay comparable; the caller keeps max_lag samples
// of guard either side.
void accumulate_line_correlation(const std::vector<double>& a,
                                 const std::vector<double>& b, size_t first,
                                 size_t last, LagCorrelation& acc) {
  double mean_a = 0.0;
  double mean_b = 0.0;
  for (size_t n = first; n < last; ++n) {
    mean_a += a[n];
    mean_b += b[n];
  }
  const auto span = static_cast<double>(last - first);
  mean_a /= span;
  mean_b /= span;

  for (int32_t lag = -acc.max_lag; lag <= acc.max_lag; ++lag) {
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
    const auto slot =
        static_cast<size_t>(lag) + static_cast<size_t>(acc.max_lag);
    acc.xy[slot] += xy;
    acc.xx[slot] += xx;
    acc.yy[slot] += yy;
  }
}

// Accumulate both frames' lag correlations: |coarse| on luma low-passed by a
// kLumaBox box, |fine| on the raw samples.  Only active picture is used — the
// front of every line carries sync and burst, which repeat identically line
// to line and would correlate at any lag.
void accumulate_frame_correlation(
    const std::vector<VideoFrameRepresentation::sample_type>& reference,
    const std::vector<VideoFrameRepresentation::sample_type>& other,
    VideoSystem system, size_t nominal_width, size_t height,
    LagCorrelation& coarse, LagCorrelation& fine) {
  const size_t step = std::max<size_t>(1, height / (kAlignLinesPerFrame + 1));
  const auto guard =
      static_cast<size_t>(std::max(coarse.max_lag, fine.max_lag));

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

    // Active video starts around a fifth of the way into the line for both
    // 4FSC systems (PAL 10.5 µs of 64 µs, NTSC 9.5 µs of 63.5 µs).
    const size_t first = length / 5;
    const size_t last = length > 10 ? length - 10 : length;
    if (last <= first + 2 * guard + kLumaBox) continue;

    raw_a.assign(
        reference.begin() + static_cast<std::ptrdiff_t>(start),
        reference.begin() + static_cast<std::ptrdiff_t>(start + length));
    raw_b.assign(other.begin() + static_cast<std::ptrdiff_t>(start),
                 other.begin() + static_cast<std::ptrdiff_t>(start + length));

    // box[n] averages raw[n .. n + kLumaBox): the same delay for both lines,
    // so it does not move the lag.
    box_a.assign(length - kLumaBox + 1, 0.0);
    box_b.assign(length - kLumaBox + 1, 0.0);
    for (size_t n = 0; n + kLumaBox <= length; ++n) {
      for (size_t k = 0; k < kLumaBox; ++k) {
        box_a[n] += raw_a[n + k];
        box_b[n] += raw_b[n + k];
      }
    }

    accumulate_line_correlation(box_a, box_b, first + guard,
                                last - guard - kLumaBox, coarse);
    accumulate_line_correlation(raw_a, raw_b, first + guard, last - guard,
                                fine);
  }
}

// ── Field agreement ─────────────────────────────────────────────────────────
//
// Frame alignment matches pictures by number, so a frame whose field 2 came
// from a different picture (a player skip in mid-frame) or any remaining
// mapping error reaches the stacker under the right number with the wrong
// content.  Each input's fields are therefore compared with the per-sample
// median of all the inputs before they are combined.  Correct inputs score
// 0.93 or more on real captures, a foreign field under 0.1, so a fixed floor
// separates them.

// Correlation with the inputs' median an input's field must reach to be used.
constexpr double kMinFieldCorrelation = 0.8;

// Fields whose median varies less than this (10-bit codes, standard
// deviation of the low-passed luma) carry too little picture to judge: a
// black or flat field correlates as noise.
constexpr double kMinFieldDetail = 20.0;

// Decimation of the comparison grid, and the vertical-blanking lines skipped
// at the top of each field.
constexpr size_t kFieldCheckLineStep = 8;
constexpr size_t kFieldCheckSampleStep = 4;
constexpr size_t kFieldBlankingLines = 25;

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

double correlation(const std::vector<double>& a, const std::vector<double>& b) {
  const auto count = static_cast<double>(a.size());
  double mean_a = 0.0;
  double mean_b = 0.0;
  for (size_t k = 0; k < a.size(); ++k) {
    mean_a += a[k];
    mean_b += b[k];
  }
  mean_a /= count;
  mean_b /= count;
  double xy = 0.0;
  double xx = 0.0;
  double yy = 0.0;
  for (size_t k = 0; k < a.size(); ++k) {
    xy += (a[k] - mean_a) * (b[k] - mean_b);
    xx += (a[k] - mean_a) * (a[k] - mean_a);
    yy += (b[k] - mean_b) * (b[k] - mean_b);
  }
  const double denominator = std::sqrt(xx * yy);
  return denominator > 0.0 ? xy / denominator : 0.0;
}

double standard_deviation(const std::vector<double>& v) {
  double mean = 0.0;
  for (double x : v) mean += x;
  mean /= static_cast<double>(v.size());
  double sum = 0.0;
  for (double x : v) sum += (x - mean) * (x - mean);
  return std::sqrt(sum / static_cast<double>(v.size()));
}

// The output frame's id, for log messages: the id every contributing source
// is read at.
FrameID frame_id_of(const std::vector<FrameID>& source_ids) {
  for (FrameID id : source_ids) {
    if (id != UINT64_MAX) return id;
  }
  return 0;
}

// One byte per sample of a flat frame of |total| samples, 1 where |runs| flag
// the sample.
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

// ── Input noise ─────────────────────────────────────────────────────────────
//
// The Weighted mode weights each input by its noise and rejects values far
// from the per-sample median, so it needs each input's noise.  Once their
// grids are aligned the inputs carry the same picture, so the difference of
// two inputs is noise alone.  Each pair's variance is measured on a decimated
// grid of active picture.
//
// Captures of a side come from more than one pressing, and every disc of a
// pressing carries the noise recorded into its stamper.  The difference of
// two inputs of one pressing therefore carries only their own noise, while
// inputs of different pressings also differ by both pressings' noise:
//     v_ij = E_i + E_j                    same pressing
//     v_ij = E_i + E_j + G_a + G_b        pressings a and b
// where E is an input's own noise and G a pressing's shared noise.  Solving
// v_ij = σᵢ² + σⱼ² instead charges each input with the pressing noise it does
// not share with the rest of the stack, so the inputs of an under-represented
// pressing measure noisier, are weighted down, and pull the stack towards the
// larger group — while shared noise only averages out across groups.
//
// Pressings are told apart without knowing E: for two inputs of one pressing,
// v_ik − v_jk = E_i − E_j for every other input k, while for inputs of
// different pressings it jumps by the pressings' noise between a k of one
// pressing and a k of the other.  An input alone in its pressing cannot be
// told from one that is merely noisy, as its pressing noise looks like its
// own, so it is treated as noisy; for a lone input both lead to the same
// weight.  E and G are then solved from all the pairs by least squares, and
// each input weighted to minimise the noise of the combined picture.

// σ given to every input when nothing could be measured.  With kRejectScale
// and the default reject_sigma of 5 that rejects at 60 codes, ld-disc-stacker's
// smart threshold on the 10-bit scale.
constexpr double kFallbackNoiseSigma = 10.0;

// Smallest σ an input is given, so identical inputs keep a finite weight.
constexpr double kMinNoiseSigma = 0.5;

// The rejection scale σ̂ against the median input σ.  The study's best policy
// rejected at 5σ̂ with this scale.
constexpr double kRejectScale = 1.2;

// Decimation of the noise grid.
constexpr size_t kNoiseLineStep = 4;
constexpr size_t kNoiseSampleStep = 2;

// Pair differences further than this many robust σ from their median are
// picture leaking through — an edge a sub-sample grid residual leaves, or
// unflagged sparkle — rather than noise.
constexpr double kNoiseClip = 4.0;

// Differences a pair needs before its variance is trusted.
constexpr uint64_t kMinNoiseSamples = 1000;

// A segment's noise is logged again only when an input's σ moves by more
// than this fraction.
constexpr double kNoiseLogChange = 0.25;

// Spread of v_ik − v_jk over the other inputs k, as a fraction of the median
// pair variance, below which inputs i and j are taken to share a pressing.
constexpr double kPressingSpread = 0.25;

// Weight of the rows that settle what the pair variances leave open: with two
// pressings their shared noise appears only as a sum, which these split
// evenly; a pressing beside only lone inputs has shared noise that cannot be
// told from theirs, which these leave with them.
constexpr double kNoiseRidge = 1e-3;

// Histogram of one pair's sample differences, which are integers within
// ±1023 on the 10-bit scale.
class DifferenceHistogram {
 public:
  void add(int32_t difference) {
    ++bins_[slot(std::clamp(difference, -kRange, kRange))];
    ++count_;
  }

  // Variance of the differences within kNoiseClip robust standard deviations
  // of their median, or nullopt with too few differences.
  std::optional<double> robust_variance() const {
    if (count_ < kMinNoiseSamples) return std::nullopt;

    // Median, then the median absolute deviation from it.
    const uint64_t half = count_ / 2;
    int32_t median = -kRange;
    for (uint64_t seen = 0; median <= kRange; ++median) {
      seen += bins_[slot(median)];
      if (seen > half) break;
    }
    int32_t mad = 0;
    for (uint64_t seen = bins_[slot(median)];
         seen <= half && mad < 2 * kRange;) {
      ++mad;
      if (median - mad >= -kRange) seen += bins_[slot(median - mad)];
      if (median + mad <= kRange) seen += bins_[slot(median + mad)];
    }

    // 1.4826 · MAD estimates σ for Gaussian noise.  The clip is kept at a
    // couple of codes so near-identical inputs still measure.
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

 private:
  static constexpr int32_t kRange = 1023;
  static size_t slot(int32_t d) {
    return static_cast<size_t>(d) + static_cast<size_t>(kRange);
  }

  std::vector<uint64_t> bins_ = std::vector<uint64_t>(2 * kRange + 1, 0);
  uint64_t count_ = 0;
};

double median_of(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const size_t mid = v.size() / 2;
  return v.size() % 2 ? v[mid] : 0.5 * (v[mid - 1] + v[mid]);
}

using SourceNoise = StackedVideoFrameRepresentation::SourceNoise;

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
// i < j.  Fewer than 4 inputs cannot show a jump, so they form one group.
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

// Each input's noise against the picture common to every input, from the pair
// variances (indexed i * n + j for i < j).  Inputs in no measured pair are
// left unmeasured.
std::vector<std::optional<SourceNoise>> fit_noise_model(
    size_t n, const std::vector<std::optional<double>>& pair_variance) {
  std::vector<std::optional<SourceNoise>> result(n);

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
  // pressing with more than one input (a lone input's is part of its E).
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
    // Two inputs, or pairs that leave the system singular, cannot separate
    // their noise, so each input takes half the mean variance of its pairs.
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
  for (double& e : own) e = std::max(e, kMinNoiseSigma * kMinNoiseSigma);

  // The inputs of a pressing share its noise G, so the weights that minimise
  // the combined noise are, with S the sum of 1/E over the pressing,
  // (1/E_i) / (1 + G·S): each pressing counts roughly once however many of
  // its discs are stacked, and within it the cleaner discs count more.
  std::vector<double> inverse_sum(groups, 0.0);
  for (size_t p = 0; p < m; ++p) inverse_sum[group[p]] += 1.0 / own[p];
  for (size_t p = 0; p < m; ++p) {
    const size_t g = group[p];
    SourceNoise noise;
    noise.sigma = std::sqrt(own[p] + pressing[g]);
    noise.weight = (1.0 / own[p]) / (1.0 + pressing[g] * inverse_sum[g]);
    noise.group = g;
    result[members[p]] = noise;
  }
  return result;
}

// Round a combined value to the nearest sample.  Ties go to even (the default
// floating-point rounding mode), so the mean of two inputs, half of which
// fall on a tie, carries no bias either way.
int16_t round_sample(double value) {
  return static_cast<int16_t>(
      std::clamp<int64_t>(std::llrint(value), INT16_MIN, INT16_MAX));
}

// Dropout severities: a sample no input could supply, and a sample the
// inputs could not agree on (Weighted mode only).
constexpr uint8_t kHoleSeverity = 50;
constexpr uint8_t kLowConfidenceSeverity = 25;

// Builds one line's output dropout runs from per-sample marks, starting a new
// run wherever the severity changes.
class RunBuilder {
 public:
  explicit RunBuilder(std::vector<DropoutRun>& out) : out_(out) {}

  // Mark the sample at |offset|: severity 0 for a clean sample.
  void mark(uint64_t offset, uint8_t severity) {
    if (severity == severity_) return;
    finish(offset);
    start_ = offset;
    severity_ = severity;
  }

  // Close any open run at |end|.
  void finish(uint64_t end) {
    if (severity_ != 0 && end > start_) {
      DropoutRun r;
      r.frame_id = 0;
      r.sample_start = start_;
      r.sample_count = static_cast<uint32_t>(end - start_);
      r.severity = severity_;
      out_.push_back(r);
    }
    severity_ = 0;
  }

 private:
  std::vector<DropoutRun>& out_;
  uint64_t start_ = 0;
  uint8_t severity_ = 0;
};

}  // namespace

StackerStage::FieldAgreement StackerStage::check_field_agreement(
    const std::vector<std::vector<sample_type>>& planes,
    const std::vector<bool>& frame_valid,
    const std::vector<std::vector<DropoutRun>>& dropouts, VideoSystem system,
    size_t nominal_width, size_t height, FrameID frame_id) {
  const size_t num_sources = planes.size();
  FieldAgreement result;
  result.field_valid.assign(num_sources, {true, true});

  std::vector<size_t> valid;
  for (size_t i = 0; i < num_sources; ++i) {
    if (frame_valid[i]) valid.push_back(i);
  }
  if (valid.size() < 2) return result;

  const size_t field1_lines = std::min(field1_line_count(system), height);
  const std::array<std::pair<size_t, size_t>, 2> fields = {
      std::pair<size_t, size_t>{0, field1_lines},
      std::pair<size_t, size_t>{field1_lines, height}};

  for (size_t field = 0; field < 2; ++field) {
    const auto [field_first, field_end] = fields[field];

    // values[k] holds valid input k's low-passed luma on the decimated grid.
    std::vector<std::vector<double>> values(valid.size());
    std::vector<bool> masked;
    for (size_t line = field_first + kFieldBlankingLines; line < field_end;
         line += kFieldCheckLineStep) {
      const size_t start =
          frame_line_sample_offset(system, nominal_width, line);
      const size_t length =
          frame_line_sample_count(system, nominal_width, line);
      const size_t first = length / 5;
      const size_t last = length > 10 ? length - 10 : length;
      if (last <= first + kLumaBox) continue;

      // Samples any input flags as dropout are left out for every input, so
      // the inputs stay compared position for position.
      masked.assign(length, false);
      for (size_t i : valid) {
        for (const auto& run : dropouts[i]) {
          const uint64_t run_end = run.sample_start + run.sample_count;
          if (run_end <= start || run.sample_start >= start + length) continue;
          const size_t from =
              run.sample_start > start ? run.sample_start - start : 0;
          const size_t to = std::min<uint64_t>(run_end - start,
                                               static_cast<uint64_t>(length));
          std::fill(masked.begin() + static_cast<std::ptrdiff_t>(from),
                    masked.begin() + static_cast<std::ptrdiff_t>(to), true);
        }
      }

      for (size_t n = first; n + kLumaBox <= last; n += kFieldCheckSampleStep) {
        bool skip = false;
        for (size_t k = 0; k < kLumaBox; ++k) skip = skip || masked[n + k];
        if (skip) continue;
        for (size_t k = 0; k < valid.size(); ++k) {
          const auto& plane = planes[valid[k]];
          if (start + n + kLumaBox > plane.size()) {
            skip = true;
            break;
          }
        }
        if (skip) continue;
        for (size_t k = 0; k < valid.size(); ++k) {
          const auto* p = planes[valid[k]].data() + start + n;
          double sum = 0.0;
          for (size_t j = 0; j < kLumaBox; ++j) sum += p[j];
          values[k].push_back(sum / static_cast<double>(kLumaBox));
        }
      }
    }

    const size_t positions = values[0].size();
    if (positions < 16) continue;

    std::vector<double> median(positions);
    std::vector<double> column(valid.size());
    for (size_t p = 0; p < positions; ++p) {
      for (size_t k = 0; k < valid.size(); ++k) column[k] = values[k][p];
      std::sort(column.begin(), column.end());
      const size_t mid = column.size() / 2;
      median[p] = column.size() % 2 ? column[mid]
                                    : 0.5 * (column[mid - 1] + column[mid]);
    }
    if (standard_deviation(median) < kMinFieldDetail) continue;

    // With two inputs the median is their mean and says nothing about which
    // one is wrong, so the pair is judged against each other.
    std::vector<double> scores(valid.size());
    if (valid.size() == 2) {
      scores[0] = scores[1] = correlation(values[0], values[1]);
    } else {
      for (size_t k = 0; k < valid.size(); ++k) {
        scores[k] = correlation(values[k], median);
      }
    }

    const auto agreeing = static_cast<size_t>(
        std::count_if(scores.begin(), scores.end(),
                      [](double s) { return s >= kMinFieldCorrelation; }));

    if (agreeing >= 2) {
      for (size_t k = 0; k < valid.size(); ++k) {
        if (scores[k] >= kMinFieldCorrelation) continue;
        result.field_valid[valid[k]][field] = false;
        ORC_LOG_WARN(
            "StackerStage: frame {} field {}: input {} correlates with the "
            "other inputs at only {:.2f}, so it carries a different picture; "
            "leaving it out of this field",
            frame_id, field + 1, valid[k] + 1, scores[k]);
      }
    } else {
      result.disagreed[field] = true;
      ORC_LOG_WARN(
          "StackerStage: frame {} field {}: the inputs do not agree on the "
          "picture, so none can be preferred; stacking all of them and "
          "marking the field as dropout",
          frame_id, field + 1);
    }
  }

  return result;
}

void StackerStage::mark_disagreed_fields(
    const FieldAgreement& agreement, VideoSystem system, size_t nominal_width,
    size_t height, std::vector<DropoutRun>& output_dropouts) {
  const size_t field1_lines = std::min(field1_line_count(system), height);
  for (size_t field = 0; field < 2; ++field) {
    if (!agreement.disagreed[field]) continue;
    const size_t first_line = field == 0 ? 0 : field1_lines;
    const size_t end_line = field == 0 ? field1_lines : height;
    const uint64_t field_start =
        frame_line_sample_offset(system, nominal_width, first_line);
    const uint64_t field_end =
        frame_line_sample_offset(system, nominal_width, end_line);

    // The whole field replaces any residual runs inside it.
    output_dropouts.erase(
        std::remove_if(output_dropouts.begin(), output_dropouts.end(),
                       [&](const DropoutRun& r) {
                         return r.sample_start >= field_start &&
                                r.sample_start < field_end;
                       }),
        output_dropouts.end());
    for (size_t line = first_line; line < end_line; ++line) {
      DropoutRun r;
      r.frame_id = 0;
      r.sample_start = frame_line_sample_offset(system, nominal_width, line);
      r.sample_count = static_cast<uint32_t>(
          frame_line_sample_count(system, nominal_width, line));
      r.severity = 50;
      output_dropouts.push_back(r);
    }
  }
}

// ============================================================================
// StackedVideoFrameRepresentation
// ============================================================================

StackedVideoFrameRepresentation::StackedVideoFrameRepresentation(
    const std::vector<std::shared_ptr<const VideoFrameRepresentation>>& sources,
    StackerStage* stage)
    : VideoFrameRepresentationWrapper(sources.empty() ? nullptr : sources[0]),
      Artifact(ArtifactID("stacked_frame"), Provenance{}),
      sources_(sources),
      stage_(stage),
      stacked_frames_(kMaxCachedFrames),
      stacked_luma_(kMaxCachedFrames),
      stacked_chroma_(kMaxCachedFrames),
      stacked_dropouts_(kMaxCachedFrames),
      stacked_audio_(kMaxCachedFrames),
      stacked_efm_(kMaxCachedFrames),
      best_source_cache_(kMaxCachedFrames) {
  if (!sources_.empty()) {
    bool first_yc = sources_[0]->has_separate_channels();
    for (size_t i = 1; i < sources_.size(); ++i) {
      if (sources_[i]->has_separate_channels() != first_yc) {
        throw std::runtime_error(
            "StackerStage: all sources must have the same channel mode "
            "(all composite or all YC)");
      }
    }
  }
}

// Sources are aligned to the same frame ids but need not end together, so
// the stack covers the union of their ranges and every picture any input
// carries is kept.
FrameIDRange StackedVideoFrameRepresentation::frame_range() const {
  bool found = false;
  FrameIDRange range{};
  for (const auto& src : sources_) {
    if (!src || src->frame_count() == 0) continue;
    const FrameIDRange r = src->frame_range();
    if (!found) {
      range = r;
      found = true;
    } else {
      range.first = std::min(range.first, r.first);
      range.last = std::max(range.last, r.last);
    }
  }
  return range;
}

size_t StackedVideoFrameRepresentation::frame_count() const {
  for (const auto& src : sources_) {
    if (src && src->frame_count() > 0) {
      return static_cast<size_t>(frame_range().count());
    }
  }
  return 0;
}

bool StackedVideoFrameRepresentation::has_frame(FrameID id) const {
  for (const auto& src : sources_) {
    if (src && src->has_frame(id)) return true;
  }
  return false;
}

// Described by the first input with a real frame here, so a stacked frame is
// marked as padding only when every input is padding.
std::optional<FrameDescriptor>
StackedVideoFrameRepresentation::get_frame_descriptor(FrameID id) const {
  std::optional<FrameDescriptor> padding;
  for (const auto& src : sources_) {
    if (!src || !src->has_frame(id)) continue;
    auto desc = src->get_frame_descriptor(id);
    if (!desc) continue;
    if (!desc->is_padding_frame) {
      desc->frame_id = id;
      return desc;
    }
    if (!padding) padding = desc;
  }
  if (padding) padding->frame_id = id;
  return padding;
}

std::vector<VideoFrameRepresentation::sample_type>
StackedVideoFrameRepresentation::padding_frame_copy(FrameID id) const {
  for (const auto& src : sources_) {
    if (src && src->has_frame(id)) return src->get_frame_copy(id);
  }
  return {};
}

bool StackedVideoFrameRepresentation::has_separate_channels() const {
  return sources_.empty() ? false : sources_[0]->has_separate_channels();
}

std::vector<FrameID> StackedVideoFrameRepresentation::collect_source_frame_ids(
    FrameID ref_id) const {
  if (sources_.empty()) return {};

  // Inputs arrive frame-aligned — frame_map normalises each source onto the
  // disc's frame numbering and source_align applies any residual offset, both
  // driven by VBI frame numbers — so the same frame id is read from every
  // source.
  //
  // This deliberately does NOT re-align on colour_frame_index.  That index is
  // derived from the burst phase measured against the source's own 4FSC
  // sample grid (colour_frame_phase_observer.cpp), and PAL's TBC layout does
  // not pin subcarrier phase to sample index the way NTSC's 910 samples/line
  // = 227.5 subcarrier cycles does.  Two decodes of the same disc whose line
  // starts land one sample apart — 90° of subcarrier — therefore report
  // colour frame indices two apart while carrying the very same picture.
  // Searching neighbouring frames for a matching index then substitutes a
  // frame 1-4 away, displacing that source's video and, through
  // stack_audio(), its audio with it.  Measured on three captures of one
  // Domesday side, that fired on 54% of frames for the one capture whose grid
  // was a sample out.  Grid differences are corrected by shifting samples
  // (see sample_offsets()), not by moving to a different frame.

  std::vector<FrameID> ids;
  ids.reserve(sources_.size());

  static constexpr FrameID kInvalid = UINT64_MAX;

  for (const auto& src : sources_) {
    if (!src || !src->has_frame(ref_id)) {
      ids.push_back(kInvalid);
      continue;
    }
    auto desc = src->get_frame_descriptor(ref_id);
    if (desc && desc->is_padding_frame) {
      ids.push_back(kInvalid);
      continue;
    }
    ids.push_back(ref_id);
  }

  return ids;
}

std::vector<int32_t> StackedVideoFrameRepresentation::sample_offsets(
    FrameID id) const {
  std::vector<int32_t> offsets(sources_.size(), 0);
  if (sources_.size() < 2) return offsets;
  if (stage_ && !stage_->m_sample_align) return offsets;

  const auto [segment, segments] = segment_of(id);
  if (segments == 0) return offsets;

  std::lock_guard<std::mutex> lk(align_mutex_);
  const auto& own = segment_offsets(segment);
  for (size_t i = 1; i < sources_.size(); ++i) {
    const std::optional<int32_t> mine = own[i];
    if (mine) {
      offsets[i] = *mine;
      continue;
    }
    // Hold the offset from the nearest segment that could be measured,
    // earlier first on a tie.
    for (size_t distance = 1; distance <= kMaxSegmentSearch; ++distance) {
      std::optional<int32_t> held;
      if (segment >= distance) held = segment_offsets(segment - distance)[i];
      if (!held && segment + distance < segments) {
        held = segment_offsets(segment + distance)[i];
      }
      if (held) {
        offsets[i] = *held;
        break;
      }
    }
  }
  return offsets;
}

const std::vector<std::optional<int32_t>>&
StackedVideoFrameRepresentation::segment_offsets(size_t segment) const {
  auto it = segment_offsets_.find(segment);
  if (it == segment_offsets_.end()) {
    it = segment_offsets_.emplace(segment, measure_segment(segment)).first;
  }
  return it->second;
}

std::vector<std::optional<int32_t>>
StackedVideoFrameRepresentation::measure_segment(size_t segment) const {
  std::vector<std::optional<int32_t>> measured(sources_.size());
  measured[0] = 0;
  if (!source_) return measured;

  const auto params = source_->get_video_parameters();
  if (!params) return measured;

  const VideoSystem system = params->system;
  const auto nominal_width = static_cast<size_t>(params->frame_width_nominal);
  const auto height = static_cast<size_t>(params->frame_height);
  if (nominal_width == 0 || height == 0) return measured;

  const size_t frame_samples =
      frame_line_sample_offset(system, nominal_width, height);

  const FrameIDRange range = frame_range();
  const FrameID segment_first =
      range.first + static_cast<FrameID>(segment * kSampleAlignSegmentFrames);
  const size_t segment_length = std::min<size_t>(
      kSampleAlignSegmentFrames, range.count() - (segment_first - range.first));

  const bool yc = source_->has_separate_channels();

  auto usable = [](const VideoFrameRepresentation& src, FrameID id) {
    if (!src.has_frame(id)) return false;
    const auto desc = src.get_frame_descriptor(id);
    return desc.has_value() && !desc->is_padding_frame;
  };

  // Luma carries the picture detail the correlation needs, and for a YC
  // source it is the only plane that both channels' grids share.
  auto read = [&](const VideoFrameRepresentation& src, FrameID id) {
    std::vector<sample_type> out;
    if (yc) {
      const sample_type* plane = src.get_frame_luma(id);
      if (plane) out.assign(plane, plane + frame_samples);
    } else {
      out = src.get_frame_copy(id);
    }
    return out;
  };

  std::vector<LagCorrelation> coarse(sources_.size(),
                                     LagCorrelation(kMaxSampleShift));
  std::vector<LagCorrelation> fine(
      sources_.size(), LagCorrelation(kMaxSampleShift + kFineSearch));
  std::vector<size_t> probed(sources_.size(), 0);

  // Probe a spread of the segment rather than its head, which is often
  // lead-in carrying too little picture detail to correlate.
  for (size_t k = 1; k <= kAlignProbeCandidates; ++k) {
    const FrameID id =
        segment_first + static_cast<FrameID>((segment_length * k) /
                                             (kAlignProbeCandidates + 1));
    if (!usable(*source_, id)) continue;

    std::vector<sample_type> reference_frame;
    for (size_t i = 1; i < sources_.size(); ++i) {
      const auto& src = sources_[i];
      // A source that IS the reference shares its grid by construction.
      if (!src || src == source_ || probed[i] >= kAlignProbeFrames) continue;
      if (!usable(*src, id)) continue;

      if (reference_frame.empty()) reference_frame = read(*source_, id);
      const auto other_frame = read(*src, id);
      if (reference_frame.size() < frame_samples ||
          other_frame.size() < frame_samples) {
        continue;
      }

      accumulate_frame_correlation(reference_frame, other_frame, system,
                                   nominal_width, height, coarse[i], fine[i]);
      ++probed[i];
    }
  }

  const FrameID segment_last =
      segment_first + static_cast<FrameID>(segment_length) - 1;

  for (size_t i = 1; i < sources_.size(); ++i) {
    if (!sources_[i]) continue;
    if (sources_[i] == source_) {
      measured[i] = 0;
      continue;
    }
    if (probed[i] == 0) {
      ORC_LOG_DEBUG(
          "StackerStage: frames {}-{}: source {} has no frame in common with "
          "the reference to measure its sample grid against",
          segment_first, segment_last, i);
      continue;
    }

    // Coarse lag from the low-passed luma, where the subcarrier cannot alias
    // it by a cycle.
    std::optional<int32_t> coarse_lag;
    double best_correlation = 0.0;
    for (int32_t lag = -kMaxSampleShift; lag <= kMaxSampleShift; ++lag) {
      const auto c = coarse[i].at(lag);
      if (c && (!coarse_lag || *c > best_correlation)) {
        coarse_lag = lag;
        best_correlation = *c;
      }
    }
    if (!coarse_lag) {
      ORC_LOG_DEBUG(
          "StackerStage: frames {}-{}: source {} carries no picture detail to "
          "measure its sample grid against the reference",
          segment_first, segment_last, i);
      continue;
    }
    if (best_correlation < kMinAlignCorrelation) {
      ORC_LOG_DEBUG(
          "StackerStage: frames {}-{}: source {} correlates with the "
          "reference at only {:.2f} over {} frame(s); cannot measure its "
          "sample grid",
          segment_first, segment_last, i, best_correlation, probed[i]);
      continue;
    }

    // Exact lag from the raw samples near the coarse peak.
    int32_t best_lag = *coarse_lag;
    std::optional<double> best_fine;
    for (int32_t lag = *coarse_lag - kFineSearch;
         lag <= *coarse_lag + kFineSearch; ++lag) {
      const auto c = fine[i].at(lag);
      if (c && (!best_fine || *c > *best_fine)) {
        best_fine = c;
        best_lag = lag;
      }
    }

    measured[i] = best_lag;
    if (last_logged_offsets_.size() != sources_.size()) {
      last_logged_offsets_.assign(sources_.size(), 0);
    }
    if (best_lag != last_logged_offsets_[i]) {
      last_logged_offsets_[i] = best_lag;
      ORC_LOG_INFO(
          "StackerStage: frames {}-{}: source {} sample grid is {:+d} "
          "sample(s) from the reference (correlation {:.3f}); shifting it "
          "onto the reference grid before stacking",
          segment_first, segment_last, i, best_lag, best_correlation);
    }
  }

  return measured;
}

std::vector<StackedVideoFrameRepresentation::SourceNoise>
StackedVideoFrameRepresentation::weighting_noise(FrameID id) const {
  if (!stage_ || stage_->m_mode != StackerStage::StackingMode::WEIGHTED) {
    return {};
  }
  return noise_model(id);
}

std::pair<size_t, size_t> StackedVideoFrameRepresentation::segment_of(
    FrameID id) const {
  const FrameIDRange range = frame_range();
  const size_t total = range.count();
  if (total == 0) return {0, 0};
  const size_t segments =
      (total + kSampleAlignSegmentFrames - 1) / kSampleAlignSegmentFrames;
  const size_t position = id > range.first ? id - range.first : 0;
  return {std::min(position / kSampleAlignSegmentFrames, segments - 1),
          segments};
}

std::vector<StackedVideoFrameRepresentation::SourceNoise>
StackedVideoFrameRepresentation::noise_model(FrameID id) const {
  const size_t n = sources_.size();
  std::vector<SourceNoise> model(n);
  for (size_t i = 0; i < n; ++i) {
    model[i].sigma = kFallbackNoiseSigma;
    model[i].weight = 1.0;
    model[i].group = i;
  }
  if (n < 2) return model;

  const auto [segment, segments] = segment_of(id);
  if (segments == 0) return model;

  std::lock_guard<std::mutex> lk(noise_mutex_);
  std::vector<std::optional<SourceNoise>> resolved = segment_noise(segment);
  for (size_t i = 0; i < n; ++i) {
    // Hold the noise from the nearest segment that could be measured,
    // earlier first on a tie.
    for (size_t distance = 1; !resolved[i] && distance <= kMaxSegmentSearch;
         ++distance) {
      if (segment >= distance) {
        resolved[i] = segment_noise(segment - distance)[i];
      }
      if (!resolved[i] && segment + distance < segments) {
        resolved[i] = segment_noise(segment + distance)[i];
      }
    }
  }

  std::vector<double> sigmas;
  std::vector<double> weights;
  for (const auto& noise : resolved) {
    if (!noise) continue;
    sigmas.push_back(noise->sigma);
    weights.push_back(noise->weight);
  }
  if (sigmas.empty()) return model;

  // A source never measured nearby is given the typical figures, in a group
  // of its own.
  const double typical_sigma = median_of(sigmas);
  const double typical_weight = median_of(weights);
  for (size_t i = 0; i < n; ++i) {
    const std::optional<SourceNoise> noise = resolved[i];
    if (noise) {
      model[i] = *noise;
    } else {
      model[i].sigma = typical_sigma;
      model[i].weight = typical_weight;
      model[i].group = n + i;
    }
  }
  return model;
}

const std::vector<std::optional<StackedVideoFrameRepresentation::SourceNoise>>&
StackedVideoFrameRepresentation::segment_noise(size_t segment) const {
  auto it = segment_noise_.find(segment);
  if (it == segment_noise_.end()) {
    it = segment_noise_.emplace(segment, measure_segment_noise(segment)).first;
  }
  return it->second;
}

std::vector<std::optional<StackedVideoFrameRepresentation::SourceNoise>>
StackedVideoFrameRepresentation::measure_segment_noise(size_t segment) const {
  const size_t n = sources_.size();
  std::vector<std::optional<SourceNoise>> measured(n);
  if (!source_) return measured;

  const auto params = source_->get_video_parameters();
  if (!params) return measured;

  const VideoSystem system = params->system;
  const auto nominal_width = static_cast<size_t>(params->frame_width_nominal);
  const auto height = static_cast<size_t>(params->frame_height);
  if (nominal_width == 0 || height == 0) return measured;

  const size_t frame_samples =
      frame_line_sample_offset(system, nominal_width, height);

  const FrameIDRange range = frame_range();
  const FrameID segment_first =
      range.first + static_cast<FrameID>(segment * kSampleAlignSegmentFrames);
  const size_t segment_length = std::min<size_t>(
      kSampleAlignSegmentFrames, range.count() - (segment_first - range.first));
  const FrameID segment_last =
      segment_first + static_cast<FrameID>(segment_length) - 1;

  // The difference of two inputs is noise only once their grids agree.
  const std::vector<int32_t> offsets = sample_offsets(segment_first);
  const bool yc = source_->has_separate_channels();
  const size_t field1_lines = std::min(field1_line_count(system), height);

  std::vector<DifferenceHistogram> pairs(n * n);
  std::vector<std::vector<sample_type>> planes(n);
  std::vector<std::vector<uint8_t>> masks(n);
  size_t frames_used = 0;

  // Probe the same spread of the segment as the grid measurement.
  for (size_t k = 1;
       k <= kAlignProbeCandidates && frames_used < kAlignProbeFrames; ++k) {
    const FrameID id =
        segment_first + static_cast<FrameID>((segment_length * k) /
                                             (kAlignProbeCandidates + 1));
    std::vector<size_t> valid;
    for (size_t i = 0; i < n; ++i) {
      const auto& src = sources_[i];
      if (!src || !src->has_frame(id)) continue;
      const auto desc = src->get_frame_descriptor(id);
      if (!desc || desc->is_padding_frame) continue;

      // Luma for a YC source: the plane the grid was measured on.
      planes[i].clear();
      if (yc) {
        const sample_type* plane = src->get_frame_luma(id);
        if (plane) planes[i].assign(plane, plane + frame_samples);
      } else {
        planes[i] = src->get_frame_copy(id);
      }
      if (planes[i].size() < frame_samples) continue;

      auto runs = src->get_dropout_hints(id);
      shift_frame_samples(planes[i], system, nominal_width, height, offsets[i]);
      shift_dropout_runs(runs, system, nominal_width, height, offsets[i]);
      masks[i] = dropout_mask(runs, frame_samples);
      valid.push_back(i);
    }
    if (valid.size() < 2) continue;
    ++frames_used;

    for (size_t line = 0; line < height; line += kNoiseLineStep) {
      const size_t in_field = line < field1_lines ? line : line - field1_lines;
      if (in_field < kFieldBlankingLines) continue;
      const size_t start =
          frame_line_sample_offset(system, nominal_width, line);
      const size_t length =
          frame_line_sample_count(system, nominal_width, line);
      const size_t first = length / 5;
      const size_t last = length > 10 ? length - 10 : length;
      for (size_t x = first; x < last; x += kNoiseSampleStep) {
        const size_t off = start + x;
        for (size_t a = 0; a < valid.size(); ++a) {
          const size_t i = valid[a];
          if (masks[i][off]) continue;
          for (size_t b = a + 1; b < valid.size(); ++b) {
            const size_t j = valid[b];
            if (masks[j][off]) continue;
            pairs[i * n + j].add(static_cast<int32_t>(planes[i][off]) -
                                 static_cast<int32_t>(planes[j][off]));
          }
        }
      }
    }
  }

  std::vector<std::optional<double>> pair_variance(n * n);
  for (size_t i = 0; i < n; ++i) {
    for (size_t j = i + 1; j < n; ++j) {
      pair_variance[i * n + j] = pairs[i * n + j].robust_variance();
    }
  }
  measured = fit_noise_model(n, pair_variance);
  if (std::none_of(measured.begin(), measured.end(),
                   [](const auto& noise) { return noise.has_value(); })) {
    ORC_LOG_DEBUG(
        "StackerStage: frames {}-{}: too little in common between the inputs "
        "to measure their noise",
        segment_first, segment_last);
    return measured;
  }

  bool changed = last_logged_noise_.size() != n;
  for (size_t i = 0; i < n && !changed; ++i) {
    const std::optional<SourceNoise> noise = measured[i];
    if (noise && std::abs(noise->sigma - last_logged_noise_[i]) >
                     kNoiseLogChange * last_logged_noise_[i]) {
      changed = true;
    }
  }

  // σ and share of the weight per input, and the pressings when there is
  // more than one; inputs are numbered from 1 as in the other messages.
  double weight_total = 0.0;
  size_t groups = 0;
  for (const auto& noise : measured) {
    if (!noise) continue;
    weight_total += noise->weight;
    groups = std::max(groups, noise->group + 1);
  }
  std::string summary;
  for (size_t i = 0; i < n; ++i) {
    const std::optional<SourceNoise> noise = measured[i];
    if (!summary.empty()) summary += ", ";
    summary += noise ? fmt::format("{:.1f} ({:.0f}%)", noise->sigma,
                                   100.0 * noise->weight / weight_total)
                     : std::string("-");
  }
  if (groups > 1) {
    summary += "; pressings";
    for (size_t g = 0; g < groups; ++g) {
      std::string members;
      for (size_t i = 0; i < n; ++i) {
        const std::optional<SourceNoise> noise = measured[i];
        if (!noise || noise->group != g) continue;
        members += (members.empty() ? "" : ",") + std::to_string(i + 1);
      }
      summary += " {" + members + "}";
    }
  }
  if (changed) {
    last_logged_noise_.assign(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
      const std::optional<SourceNoise> noise = measured[i];
      last_logged_noise_[i] = noise ? noise->sigma : 0.0;
    }
    ORC_LOG_INFO(
        "StackerStage: frames {}-{}: input noise σ in codes (share of the "
        "weight) {}",
        segment_first, segment_last, summary);
  } else {
    ORC_LOG_DEBUG(
        "StackerStage: frames {}-{}: input noise σ in codes (share of the "
        "weight) {}",
        segment_first, segment_last, summary);
  }

  return measured;
}

FrameID StackedVideoFrameRepresentation::resolve_source_frame(
    size_t src_idx, FrameID ref_id) const {
  auto ids = collect_source_frame_ids(ref_id);
  if (src_idx >= ids.size()) {
    return UINT64_MAX;
  }
  return ids[src_idx];
}

size_t StackedVideoFrameRepresentation::get_source_count(FrameID ref_id) const {
  size_t count = 0;
  for (const auto& src : sources_) {
    if (!src || !src->has_frame(ref_id)) {
      continue;
    }
    auto desc = src->get_frame_descriptor(ref_id);
    if (desc && !desc->is_padding_frame) {
      ++count;
    }
  }
  return count;
}

size_t StackedVideoFrameRepresentation::get_best_source_index(
    FrameID id) const {
  std::lock_guard<std::mutex> lk(cache_mutex_);
  if (const auto* p = best_source_cache_.get_ptr(id)) {
    return *p;
  }

  auto src_ids = collect_source_frame_ids(id);
  size_t best = 0;
  size_t min_dropouts = SIZE_MAX;

  for (size_t i = 0; i < sources_.size() && i < src_ids.size(); ++i) {
    if (src_ids[i] == UINT64_MAX) {
      continue;
    }
    if (!sources_[i]) {
      continue;
    }
    auto runs = sources_[i]->get_dropout_hints(src_ids[i]);
    size_t total = 0;
    for (const auto& r : runs) {
      total += r.sample_count;
    }
    if (total < min_dropouts) {
      min_dropouts = total;
      best = i;
    }
  }

  best_source_cache_.put(id, best);
  return best;
}

void StackedVideoFrameRepresentation::ensure_frame_stacked(FrameID id) const {
  if (stacked_frames_.contains(id) && stacked_dropouts_.contains(id)) {
    return;
  }

  ORC_LOG_DEBUG("StackedVideoFrameRepresentation: stacking frame {}", id);

  auto src_ids = collect_source_frame_ids(id);
  std::vector<sample_type> stacked_samples;
  std::vector<DropoutRun> stacked_do;

  stage_->stack_frame(src_ids, sources_, sample_offsets(id),
                      weighting_noise(id), stacked_samples, stacked_do);
  if (stacked_samples.empty()) {
    // Every input is padding here.
    stacked_samples = padding_frame_copy(id);
  }

  // stack_frame builds runs without frame context; stamp this frame's ID so
  // downstream consumers (e.g. dropout_map) see consistent hints.
  for (auto& run : stacked_do) {
    run.frame_id = id;
  }

  stacked_frames_.put(id, std::move(stacked_samples));
  stacked_dropouts_.put(id, std::move(stacked_do));
}

void StackedVideoFrameRepresentation::ensure_frame_stacked_yc(
    FrameID id) const {
  if (stacked_luma_.contains(id) && stacked_chroma_.contains(id) &&
      stacked_dropouts_.contains(id)) {
    return;
  }

  ORC_LOG_DEBUG("StackedVideoFrameRepresentation: stacking YC frame {}", id);

  auto src_ids = collect_source_frame_ids(id);
  std::vector<sample_type> luma, chroma;
  std::vector<DropoutRun> dos;

  stage_->stack_frame_yc(src_ids, sources_, sample_offsets(id),
                         weighting_noise(id), luma, chroma, dos);

  // stack_frame_yc builds runs without frame context; stamp this frame's ID
  // so downstream consumers (e.g. dropout_map) see consistent hints.
  for (auto& run : dos) {
    run.frame_id = id;
  }

  stacked_luma_.put(id, std::move(luma));
  stacked_chroma_.put(id, std::move(chroma));
  stacked_dropouts_.put(id, std::move(dos));
}

// ── Flat access ──────────────────────────────────────────────────────────────

const VideoFrameRepresentation::sample_type*
StackedVideoFrameRepresentation::get_frame(FrameID id) const {
  std::lock_guard<std::mutex> lk(cache_mutex_);
  ensure_frame_stacked(id);
  const auto* p = stacked_frames_.get_ptr(id);
  return (p && !p->empty()) ? p->data() : nullptr;
}

const VideoFrameRepresentation::sample_type*
StackedVideoFrameRepresentation::get_line(FrameID id, size_t line) const {
  const sample_type* frame = get_frame(id);
  if (!frame) return nullptr;
  auto params = get_video_parameters();
  if (!params) return nullptr;
  if (line >= static_cast<size_t>(params->frame_height)) return nullptr;
  return frame + frame_line_sample_offset(
                     params->system,
                     static_cast<size_t>(params->frame_width_nominal), line);
}

std::vector<VideoFrameRepresentation::sample_type>
StackedVideoFrameRepresentation::get_frame_copy(FrameID id) const {
  {
    std::lock_guard<std::mutex> lk(cache_mutex_);
    if (stacked_frames_.contains(id) && stacked_dropouts_.contains(id)) {
      const auto* p = stacked_frames_.get_ptr(id);
      if (p) {
        return *p;
      }
    }
  }

  std::vector<sample_type> samples;
  std::vector<DropoutRun> dos;
  auto src_ids = collect_source_frame_ids(id);
  stage_->stack_frame(src_ids, sources_, sample_offsets(id),
                      weighting_noise(id), samples, dos);
  if (samples.empty()) {
    // Every input is padding here.
    samples = padding_frame_copy(id);
  }

  // stack_frame builds runs without frame context; stamp this frame's ID so
  // downstream consumers (e.g. dropout_map) see consistent hints.
  for (auto& run : dos) {
    run.frame_id = id;
  }

  {
    std::lock_guard<std::mutex> lk(cache_mutex_);
    if (!stacked_frames_.contains(id)) {
      stacked_frames_.put(id, samples);
      stacked_dropouts_.put(id, std::move(dos));
    }
  }
  return samples;
}

// ── YC access ────────────────────────────────────────────────────────────────

const VideoFrameRepresentation::sample_type*
StackedVideoFrameRepresentation::get_frame_luma(FrameID id) const {
  if (!has_separate_channels()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lk(cache_mutex_);
  ensure_frame_stacked_yc(id);
  const auto* p = stacked_luma_.get_ptr(id);
  return (p && !p->empty()) ? p->data() : nullptr;
}

const VideoFrameRepresentation::sample_type*
StackedVideoFrameRepresentation::get_frame_chroma(FrameID id) const {
  if (!has_separate_channels()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lk(cache_mutex_);
  ensure_frame_stacked_yc(id);
  const auto* p = stacked_chroma_.get_ptr(id);
  return (p && !p->empty()) ? p->data() : nullptr;
}

const VideoFrameRepresentation::sample_type*
StackedVideoFrameRepresentation::get_line_luma(FrameID id, size_t line) const {
  if (!has_separate_channels()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lk(cache_mutex_);
  ensure_frame_stacked_yc(id);
  const auto* p = stacked_luma_.get_ptr(id);
  if (!p || p->empty()) {
    return nullptr;
  }
  auto desc = get_frame_descriptor(id);
  if (!desc || line >= desc->height) {
    return nullptr;
  }
  return p->data() + frame_line_sample_offset(
                         desc->system, desc->samples_per_line_nominal, line);
}

const VideoFrameRepresentation::sample_type*
StackedVideoFrameRepresentation::get_line_chroma(FrameID id,
                                                 size_t line) const {
  if (!has_separate_channels()) {
    return nullptr;
  }
  std::lock_guard<std::mutex> lk(cache_mutex_);
  ensure_frame_stacked_yc(id);
  const auto* p = stacked_chroma_.get_ptr(id);
  if (!p || p->empty()) {
    return nullptr;
  }
  auto desc = get_frame_descriptor(id);
  if (!desc || line >= desc->height) {
    return nullptr;
  }
  return p->data() + frame_line_sample_offset(
                         desc->system, desc->samples_per_line_nominal, line);
}

// ── Dropout hints
// ─────────────────────────────────────────────────────────────

std::vector<DropoutRun> StackedVideoFrameRepresentation::get_dropout_hints(
    FrameID id) const {
  {
    std::lock_guard<std::mutex> lk(cache_mutex_);
    if (stacked_dropouts_.contains(id)) {
      const auto* p = stacked_dropouts_.get_ptr(id);
      if (p) {
        return *p;
      }
    }
  }

  get_frame_copy(id);  // populates the dropout cache as a side-effect

  std::lock_guard<std::mutex> lk(cache_mutex_);
  const auto* p = stacked_dropouts_.get_ptr(id);
  return p ? *p : std::vector<DropoutRun>{};
}

// ── Audio
// ─────────────────────────────────────────────────────────────────────

std::shared_ptr<const VideoFrameRepresentation>
StackedVideoFrameRepresentation::reference_audio_source() const {
  for (const auto& src : sources_) {
    if (src && src->has_audio()) {
      return src;
    }
  }
  return nullptr;
}

bool StackedVideoFrameRepresentation::pair_in_all_sources(size_t pair) const {
  for (const auto& src : sources_) {
    if (!src || pair >= src->audio_channel_pair_count()) return false;
  }
  return true;
}

size_t StackedVideoFrameRepresentation::audio_channel_pair_count() const {
  const auto ref = reference_audio_source();
  return ref ? ref->audio_channel_pair_count() : 0;
}

std::optional<AudioChannelPairDescriptor>
StackedVideoFrameRepresentation::get_audio_channel_pair_descriptor(
    size_t pair) const {
  const auto ref = reference_audio_source();
  return ref ? ref->get_audio_channel_pair_descriptor(pair) : std::nullopt;
}

std::vector<int32_t> StackedVideoFrameRepresentation::get_audio_samples(
    size_t pair, FrameID id) const {
  const auto ref = reference_audio_source();
  if (!ref || pair >= ref->audio_channel_pair_count()) {
    return {};
  }

  if (!pair_in_all_sources(pair)) {
    // Pairs not common to all inputs pass through from the per-frame best
    // source (falling back to the first source that carries the frame).
    const auto src_ids = collect_source_frame_ids(id);
    const size_t best = get_best_source_index(id);
    if (best < sources_.size() && src_ids[best] != UINT64_MAX &&
        sources_[best] && pair < sources_[best]->audio_channel_pair_count()) {
      return sources_[best]->get_audio_samples(pair, src_ids[best]);
    }
    for (size_t i = 0; i < sources_.size(); ++i) {
      if (sources_[i] && src_ids[i] != UINT64_MAX &&
          pair < sources_[i]->audio_channel_pair_count()) {
        return sources_[i]->get_audio_samples(pair, src_ids[i]);
      }
    }
    return {};
  }

  const FrameID key = audio_cache_key(id, pair);
  {
    std::lock_guard<std::mutex> lk(cache_mutex_);
    if (const auto* p = stacked_audio_.get_ptr(key)) {
      return *p;
    }
  }

  size_t best = get_best_source_index(id);
  auto src_ids = collect_source_frame_ids(id);
  auto result = stage_->stack_audio(pair, src_ids, sources_, best);

  std::lock_guard<std::mutex> lk(cache_mutex_);
  if (!stacked_audio_.contains(key)) {
    stacked_audio_.put(key, result);
  }
  return result;
}

// ── EFM ──────────────────────────────────────────────────────────────────────

bool StackedVideoFrameRepresentation::has_efm() const {
  for (const auto& src : sources_) {
    if (src && src->has_efm()) {
      return true;
    }
  }
  return false;
}

// An estimate, not a promise. Stacking preserves the combined stream's length
// in channel bits, not its t-value count: a transition the sources disagree
// about is one t-value in the capture that saw it and none in the capture that
// did not. Consumers that need the exact count must read the samples. This is
// used only to size progress reporting, so the first contributing source's
// count is close enough - and unlike asking which source is best, it costs
// nothing per frame.
uint32_t StackedVideoFrameRepresentation::get_efm_sample_count(
    FrameID id) const {
  for (const auto& src : sources_) {
    if (src && src->has_efm() && src->has_frame(id)) {
      return src->get_efm_sample_count(id);
    }
  }
  return 0;
}

std::vector<uint8_t> StackedVideoFrameRepresentation::get_efm_samples(
    FrameID id) const {
  if (!has_efm()) {
    return {};
  }

  {
    std::lock_guard<std::mutex> lk(cache_mutex_);
    if (const auto* p = stacked_efm_.get_ptr(id)) {
      return *p;
    }
  }

  size_t best = get_best_source_index(id);
  auto src_ids = collect_source_frame_ids(id);
  auto result = stage_->stack_efm(src_ids, sources_, best);

  std::lock_guard<std::mutex> lk(cache_mutex_);
  if (!stacked_efm_.contains(id)) {
    stacked_efm_.put(id, result);
  }
  return result;
}

// ============================================================================
// StackerStage
// ============================================================================

StackerStage::StackerStage() = default;

std::vector<ArtifactPtr> StackerStage::execute(
    const std::vector<ArtifactPtr>& inputs,
    const std::map<std::string, ParameterValue>& parameters,
    ObservationContext& /*observation_context*/) {
  if (inputs.empty()) {
    throw DAGExecutionError("StackerStage requires at least 1 input");
  }
  if (inputs.size() > 16) {
    throw DAGExecutionError("StackerStage supports maximum 16 inputs");
  }

  std::vector<std::shared_ptr<const VideoFrameRepresentation>> sources;
  for (const auto& inp : inputs) {
    auto vfr = std::dynamic_pointer_cast<const VideoFrameRepresentation>(inp);
    if (!vfr) {
      throw DAGExecutionError(
          "StackerStage: input is not a VideoFrameRepresentation");
    }
    sources.push_back(vfr);
  }

  // Serialize access to cached_output_ and cached_sources_: two concurrent
  // DAG render threads share the same stage instance, so the read-check-update
  // of the cache must be atomic to avoid heap corruption via shared_ptr races.
  std::shared_ptr<const VideoFrameRepresentation> result;
  {
    std::lock_guard<std::mutex> lk(execute_mutex_);

    // Only invalidate the cached stacked representation when the parameters
    // actually change. The DAG passes the node's stored parameters on every
    // execute(); resetting unconditionally discards the wrapper — and its warm
    // per-frame stack cache — on each call, forcing every analysis-sink sweep
    // to re-stack the whole source from scratch.
    if (!parameters.empty() && parameters != parameters_) {
      set_parameters(parameters);
      cached_output_.reset();
    }

    bool reuse = false;
    if (cached_output_ && cached_sources_.size() == sources.size()) {
      reuse = true;
      for (size_t i = 0; i < sources.size(); ++i) {
        if (cached_sources_[i] != sources[i]) {
          reuse = false;
          break;
        }
      }
    }

    if (reuse) {
      result = cached_output_;
    } else {
      result = process(sources);
      cached_output_ = result;
      cached_sources_ = sources;
    }
  }

  // When process() returns the single input source unchanged (passthrough),
  // the result is already an Artifact via the original input pointer.
  if (sources.size() == 1) {
    return {inputs[0]};
  }

  // Multi-source path: result is a StackedVideoFrameRepresentation which
  // inherits from both VideoFrameRepresentationWrapper and Artifact.
  auto stacked = std::const_pointer_cast<StackedVideoFrameRepresentation>(
      std::dynamic_pointer_cast<const StackedVideoFrameRepresentation>(result));
  if (!stacked) {
    // Fallback: should not happen
    return {inputs[0]};
  }
  return {stacked};
}

std::shared_ptr<const VideoFrameRepresentation> StackerStage::process(
    const std::vector<std::shared_ptr<const VideoFrameRepresentation>>& sources)
    const {
  if (sources.empty()) {
    return nullptr;
  }
  if (sources.size() == 1) {
    return sources[0];
  }

  return std::make_shared<StackedVideoFrameRepresentation>(
      sources, const_cast<StackerStage*>(this));
}

// ── Core stacking
// ─────────────────────────────────────────────────────────────

StackerStage::NoiseWeights StackerStage::noise_weights(
    const std::vector<SourceNoise>& model, size_t num_sources) const {
  NoiseWeights noise;
  noise.weight.assign(num_sources, 1.0);
  double typical = kFallbackNoiseSigma;
  if (model.size() == num_sources && num_sources > 0) {
    std::vector<double> sigmas(num_sources);
    for (size_t i = 0; i < num_sources; ++i) {
      noise.weight[i] = model[i].weight;
      sigmas[i] = model[i].sigma;
    }
    typical = median_of(sigmas);
  }
  noise.reject_distance = m_reject_sigma * kRejectScale * typical;
  return noise;
}

void StackerStage::stack_frame(
    const std::vector<FrameID>& source_ids,
    const std::vector<std::shared_ptr<const VideoFrameRepresentation>>& sources,
    const std::vector<int32_t>& sample_offsets,
    const std::vector<SourceNoise>& noise_model,
    std::vector<sample_type>& output_samples,
    std::vector<DropoutRun>& output_dropouts) const {
  if (source_ids.size() != sources.size()) {
    return;
  }

  std::optional<FrameDescriptor> ref_desc;
  size_t ref_src = 0;
  for (size_t i = 0; i < source_ids.size(); ++i) {
    if (source_ids[i] == UINT64_MAX || !sources[i]) {
      continue;
    }
    ref_desc = sources[i]->get_frame_descriptor(source_ids[i]);
    if (ref_desc) {
      ref_src = i;
      break;
    }
  }
  if (!ref_desc) {
    return;
  }

  size_t height = ref_desc->height;
  size_t nominal_width = ref_desc->samples_per_line_nominal;

  auto params = sources[ref_src]->get_video_parameters();
  const VideoSystem system = params ? params->system : VideoSystem::PAL;
  int32_t black_level = params ? params->black_level : 282;

  // Total sample count respects non-orthogonal PAL layout.
  const size_t total = frame_line_sample_offset(system, nominal_width, height);
  output_samples.resize(total, static_cast<sample_type>(black_level));
  output_dropouts.clear();

  std::vector<std::vector<sample_type>> all_frames(sources.size());
  std::vector<bool> frame_valid(sources.size(), false);
  std::vector<std::vector<DropoutRun>> all_dropouts(sources.size());

  for (size_t i = 0; i < sources.size(); ++i) {
    if (source_ids[i] == UINT64_MAX || !sources[i]) {
      continue;
    }
    if (!sources[i]->has_frame(source_ids[i])) {
      continue;
    }
    auto d = sources[i]->get_frame_descriptor(source_ids[i]);
    if (!d || d->is_padding_frame) {
      continue;
    }
    all_frames[i] = sources[i]->get_frame_copy(source_ids[i]);
    if (!all_frames[i].empty()) {
      frame_valid[i] = true;
      all_dropouts[i] = sources[i]->get_dropout_hints(source_ids[i]);

      // Bring this source onto the reference's sample grid before its samples
      // are combined with the others'.
      const int32_t offset = i < sample_offsets.size() ? sample_offsets[i] : 0;
      shift_frame_samples(all_frames[i], system, nominal_width, height, offset);
      shift_dropout_runs(all_dropouts[i], system, nominal_width, height,
                         offset);
    }
  }

  const FieldAgreement agreement =
      check_field_agreement(all_frames, frame_valid, all_dropouts, system,
                            nominal_width, height, frame_id_of(source_ids));

  std::vector<std::vector<uint8_t>> masks(sources.size());
  for (size_t i = 0; i < sources.size(); ++i) {
    if (frame_valid[i]) masks[i] = dropout_mask(all_dropouts[i], total);
  }
  const NoiseWeights noise = noise_weights(noise_model, sources.size());

  size_t n_threads = static_cast<size_t>(m_thread_count);
  if (n_threads == 0) {
    n_threads = std::thread::hardware_concurrency();
    if (n_threads == 0) {
      n_threads = 4;
    }
  }
  if (n_threads == 1 || height < n_threads * 4) {
    n_threads = 1;
  }

  size_t total_do = 0;
  size_t total_stacked = 0;

  if (n_threads == 1) {
    process_lines_range(0, height, nominal_width, system, all_frames,
                        frame_valid, agreement.field_valid, masks, noise,
                        sources.size(), black_level, output_samples,
                        output_dropouts, total_do, total_stacked);
  } else {
    std::vector<std::thread> threads;
    std::vector<std::vector<DropoutRun>> thread_dos(n_threads);
    std::vector<size_t> thread_do(n_threads, 0);
    std::vector<size_t> thread_st(n_threads, 0);
    size_t lpt = (height + n_threads - 1) / n_threads;

    for (size_t t = 0; t < n_threads; ++t) {
      size_t s = t * lpt;
      size_t e = std::min(s + lpt, height);
      if (s >= height) {
        break;
      }
      threads.emplace_back([&, t, s, e]() {
        process_lines_range(s, e, nominal_width, system, all_frames,
                            frame_valid, agreement.field_valid, masks, noise,
                            sources.size(), black_level, output_samples,
                            thread_dos[t], thread_do[t], thread_st[t]);
      });
    }
    for (auto& th : threads) {
      th.join();
    }
    for (size_t t = 0; t < n_threads; ++t) {
      output_dropouts.insert(output_dropouts.end(), thread_dos[t].begin(),
                             thread_dos[t].end());
      total_do += thread_do[t];
      total_stacked += thread_st[t];
    }
  }

  mark_disagreed_fields(agreement, system, nominal_width, height,
                        output_dropouts);

  ORC_LOG_DEBUG("StackerStage::stack_frame: {} dropouts, {} stacked pixels",
                output_dropouts.size(), total_stacked);
  (void)total_do;
}

void StackerStage::stack_frame_yc(
    const std::vector<FrameID>& source_ids,
    const std::vector<std::shared_ptr<const VideoFrameRepresentation>>& sources,
    const std::vector<int32_t>& sample_offsets,
    const std::vector<SourceNoise>& noise_model,
    std::vector<sample_type>& output_luma,
    std::vector<sample_type>& output_chroma,
    std::vector<DropoutRun>& output_dropouts) const {
  if (source_ids.size() != sources.size()) {
    return;
  }

  std::optional<FrameDescriptor> ref_desc;
  size_t ref_src = 0;
  for (size_t i = 0; i < source_ids.size(); ++i) {
    if (source_ids[i] == UINT64_MAX || !sources[i]) {
      continue;
    }
    ref_desc = sources[i]->get_frame_descriptor(source_ids[i]);
    if (ref_desc) {
      ref_src = i;
      break;
    }
  }
  if (!ref_desc) {
    return;
  }

  size_t height = ref_desc->height;
  size_t nominal_width = ref_desc->samples_per_line_nominal;

  auto params = sources[ref_src]->get_video_parameters();
  const VideoSystem system = params ? params->system : VideoSystem::PAL;
  int32_t black_level = params ? params->black_level : 282;

  const size_t total = frame_line_sample_offset(system, nominal_width, height);
  output_luma.resize(total, static_cast<sample_type>(black_level));
  output_chroma.resize(total, static_cast<sample_type>(black_level));
  output_dropouts.clear();

  std::vector<std::vector<sample_type>> all_luma(sources.size());
  std::vector<std::vector<sample_type>> all_chroma(sources.size());
  std::vector<bool> frame_valid(sources.size(), false);
  std::vector<std::vector<DropoutRun>> all_dropouts(sources.size());

  for (size_t i = 0; i < sources.size(); ++i) {
    if (source_ids[i] == UINT64_MAX || !sources[i]) {
      continue;
    }
    if (!sources[i]->has_frame(source_ids[i])) {
      continue;
    }
    auto d = sources[i]->get_frame_descriptor(source_ids[i]);
    if (!d || d->is_padding_frame) {
      continue;
    }
    const sample_type* lp = sources[i]->get_frame_luma(source_ids[i]);
    const sample_type* cp = sources[i]->get_frame_chroma(source_ids[i]);
    if (!lp || !cp) {
      continue;
    }
    all_luma[i].assign(lp, lp + total);
    all_chroma[i].assign(cp, cp + total);
    frame_valid[i] = true;
    all_dropouts[i] = sources[i]->get_dropout_hints(source_ids[i]);

    // Bring this source onto the reference's sample grid before its samples
    // are combined with the others'.
    const int32_t offset = i < sample_offsets.size() ? sample_offsets[i] : 0;
    shift_frame_samples(all_luma[i], system, nominal_width, height, offset);
    shift_frame_samples(all_chroma[i], system, nominal_width, height, offset);
    shift_dropout_runs(all_dropouts[i], system, nominal_width, height, offset);
  }

  // Luma carries the picture the fields are compared on.
  const FieldAgreement agreement =
      check_field_agreement(all_luma, frame_valid, all_dropouts, system,
                            nominal_width, height, frame_id_of(source_ids));

  std::vector<std::vector<uint8_t>> masks(sources.size());
  for (size_t i = 0; i < sources.size(); ++i) {
    if (frame_valid[i]) masks[i] = dropout_mask(all_dropouts[i], total);
  }
  const NoiseWeights noise = noise_weights(noise_model, sources.size());

  size_t n_threads = static_cast<size_t>(m_thread_count);
  if (n_threads == 0) {
    n_threads = std::thread::hardware_concurrency();
    if (n_threads == 0) {
      n_threads = 4;
    }
  }
  if (n_threads == 1 || height < n_threads * 4) {
    n_threads = 1;
  }

  size_t total_do = 0;
  size_t total_stacked = 0;

  if (n_threads == 1) {
    process_lines_range_yc(
        0, height, nominal_width, system, all_luma, all_chroma, frame_valid,
        agreement.field_valid, masks, noise, sources.size(), black_level,
        output_luma, output_chroma, output_dropouts, total_do, total_stacked);
  } else {
    std::vector<std::thread> threads;
    std::vector<std::vector<DropoutRun>> thread_dos(n_threads);
    std::vector<size_t> thread_do(n_threads, 0);
    std::vector<size_t> thread_st(n_threads, 0);
    size_t lpt = (height + n_threads - 1) / n_threads;

    for (size_t t = 0; t < n_threads; ++t) {
      size_t s = t * lpt;
      size_t e = std::min(s + lpt, height);
      if (s >= height) {
        break;
      }
      threads.emplace_back([&, t, s, e]() {
        process_lines_range_yc(s, e, nominal_width, system, all_luma,
                               all_chroma, frame_valid, agreement.field_valid,
                               masks, noise, sources.size(), black_level,
                               output_luma, output_chroma, thread_dos[t],
                               thread_do[t], thread_st[t]);
      });
    }
    for (auto& th : threads) {
      th.join();
    }
    for (size_t t = 0; t < n_threads; ++t) {
      output_dropouts.insert(output_dropouts.end(), thread_dos[t].begin(),
                             thread_dos[t].end());
    }
  }
  mark_disagreed_fields(agreement, system, nominal_width, height,
                        output_dropouts);
  (void)total_do;
  (void)total_stacked;
}

// ── Per-line processing
// ───────────────────────────────────────────────────────

void StackerStage::process_lines_range(
    size_t start_line, size_t end_line, size_t width, VideoSystem system,
    const std::vector<std::vector<sample_type>>& all_frames,
    const std::vector<bool>& frame_valid,
    const std::vector<std::array<bool, 2>>& field_valid,
    const std::vector<std::vector<uint8_t>>& dropout_masks,
    const NoiseWeights& noise, size_t num_sources, int32_t black_level,
    std::vector<sample_type>& output_samples,
    std::vector<DropoutRun>& output_dropouts, size_t& total_dropouts,
    size_t& total_stacked) const {
  const size_t field1_lines = field1_line_count(system);
  std::vector<int16_t> good_values;
  std::vector<double> good_weights;
  std::vector<int16_t> flagged_values;
  good_values.reserve(num_sources);
  good_weights.reserve(num_sources);
  flagged_values.reserve(num_sources);

  for (size_t y = start_line; y < end_line; ++y) {
    RunBuilder runs(output_dropouts);
    const size_t line_base = frame_line_sample_offset(system, width, y);
    const size_t line_len = frame_line_sample_count(system, width, y);
    const size_t field = y < field1_lines ? 0 : 1;

    for (size_t x = 0; x < line_len; ++x) {
      good_values.clear();
      good_weights.clear();
      flagged_values.clear();

      const size_t off = line_base + x;
      for (size_t si = 0; si < num_sources; ++si) {
        if (!frame_valid[si] || !field_valid[si][field] ||
            off >= all_frames[si].size()) {
          continue;
        }
        const int16_t val = all_frames[si][off];
        if (!dropout_masks[si][off]) {
          good_values.push_back(val);
          good_weights.push_back(noise.weight[si]);
        } else {
          flagged_values.push_back(val);
        }
      }

      int16_t stacked;
      uint8_t severity = 0;
      if (good_values.empty()) {
        // Every input is flagged here.  diff_dod may recover values when the
        // inputs agree, and passthrough keeps the flagged values rather than
        // black, but either way the values come from flagged samples, so the
        // output stays marked.
        severity = kHoleSeverity;
        std::vector<int16_t> recovered;
        if (num_sources >= 3 && !m_no_diff_dod && !flagged_values.empty()) {
          recovered = diff_dod(flagged_values);
        }
        if (!recovered.empty()) {
          stacked = stack_mode(recovered);
        } else if (m_passthrough && !flagged_values.empty()) {
          stacked = stack_mode(flagged_values);
        } else {
          stacked = static_cast<int16_t>(black_level);
        }
        ++total_dropouts;
      } else if (m_mode == StackingMode::WEIGHTED) {
        bool low_confidence = false;
        stacked = stack_weighted(good_values, good_weights,
                                 noise.reject_distance, low_confidence);
        if (low_confidence) severity = kLowConfidenceSeverity;
        ++total_stacked;
      } else {
        stacked = stack_mode(good_values);
        ++total_stacked;
      }
      runs.mark(off, severity);
      output_samples[off] = stacked;
    }
    runs.finish(line_base + line_len);
  }
}

// The YC path has no diff_dod: the recovery is a composite-signal heuristic.
// Weighted mode judges confidence on luma, which the noise was measured on.
void StackerStage::process_lines_range_yc(
    size_t start_line, size_t end_line, size_t width, VideoSystem system,
    const std::vector<std::vector<sample_type>>& all_luma,
    const std::vector<std::vector<sample_type>>& all_chroma,
    const std::vector<bool>& frame_valid,
    const std::vector<std::array<bool, 2>>& field_valid,
    const std::vector<std::vector<uint8_t>>& dropout_masks,
    const NoiseWeights& noise, size_t num_sources, int32_t black_level,
    std::vector<sample_type>& output_luma,
    std::vector<sample_type>& output_chroma,
    std::vector<DropoutRun>& output_dropouts, size_t& total_dropouts,
    size_t& total_stacked) const {
  const size_t field1_lines = field1_line_count(system);
  std::vector<int16_t> good_luma;
  std::vector<int16_t> good_chroma;
  std::vector<double> luma_weights;
  std::vector<double> chroma_weights;
  std::vector<int16_t> flagged_luma;
  std::vector<int16_t> flagged_chroma;

  for (size_t y = start_line; y < end_line; ++y) {
    RunBuilder runs(output_dropouts);
    const size_t line_base = frame_line_sample_offset(system, width, y);
    const size_t line_len = frame_line_sample_count(system, width, y);
    const size_t field = y < field1_lines ? 0 : 1;

    for (size_t x = 0; x < line_len; ++x) {
      good_luma.clear();
      good_chroma.clear();
      luma_weights.clear();
      chroma_weights.clear();
      flagged_luma.clear();
      flagged_chroma.clear();

      const size_t off = line_base + x;
      for (size_t si = 0; si < num_sources; ++si) {
        if (!frame_valid[si] || !field_valid[si][field] ||
            off >= all_luma[si].size()) {
          continue;
        }
        const bool flagged = dropout_masks[si][off] != 0;
        auto& luma = flagged ? flagged_luma : good_luma;
        auto& chroma = flagged ? flagged_chroma : good_chroma;
        luma.push_back(all_luma[si][off]);
        if (!flagged) luma_weights.push_back(noise.weight[si]);
        if (off < all_chroma[si].size()) {
          chroma.push_back(all_chroma[si][off]);
          if (!flagged) chroma_weights.push_back(noise.weight[si]);
        }
      }

      int16_t out_luma;
      int16_t out_chroma;
      uint8_t severity = 0;
      if (good_luma.empty()) {
        severity = kHoleSeverity;
        if (m_passthrough && !flagged_luma.empty()) {
          out_luma = stack_mode(flagged_luma);
          out_chroma = flagged_chroma.empty()
                           ? static_cast<int16_t>(black_level)
                           : stack_mode(flagged_chroma);
        } else {
          out_luma = static_cast<int16_t>(black_level);
          out_chroma = static_cast<int16_t>(black_level);
        }
        ++total_dropouts;
      } else if (m_mode == StackingMode::WEIGHTED) {
        bool low_confidence = false;
        out_luma = stack_weighted(good_luma, luma_weights,
                                  noise.reject_distance, low_confidence);
        if (low_confidence) severity = kLowConfidenceSeverity;
        bool chroma_low_confidence = false;
        out_chroma =
            good_chroma.empty()
                ? static_cast<int16_t>(black_level)
                : stack_weighted(good_chroma, chroma_weights,
                                 noise.reject_distance, chroma_low_confidence);
        ++total_stacked;
      } else {
        out_luma = stack_mode(good_luma);
        out_chroma = good_chroma.empty() ? static_cast<int16_t>(black_level)
                                         : stack_mode(good_chroma);
        ++total_stacked;
      }

      runs.mark(off, severity);
      output_luma[off] = out_luma;
      output_chroma[off] = out_chroma;
    }
    runs.finish(line_base + line_len);
  }
}

// ── Stacking mode
// ─────────────────────────────────────────────────────────────

int16_t StackerStage::stack_mode(const std::vector<int16_t>& values) const {
  if (values.empty()) {
    return 0;
  }

  StackingMode mode = m_mode;
  if (mode == StackingMode::AUTO) {
    mode = values.size() >= 3 ? StackingMode::SMART_MEAN : StackingMode::MEAN;
  }

  switch (mode) {
    case StackingMode::MEAN:
      return static_cast<int16_t>(compute_mean(values));
    case StackingMode::MEDIAN:
      return compute_median(values);
    case StackingMode::SMART_MEAN: {
      int16_t med = compute_median(values);
      int32_t sum = 0;
      size_t count = 0;
      for (int16_t v : values) {
        if (std::abs(static_cast<int32_t>(v) - med) < m_smart_threshold) {
          sum += v;
          ++count;
        }
      }
      if (count == 0) {
        return med;
      }
      return static_cast<int16_t>(sum / static_cast<int32_t>(count));
    }
    case StackingMode::WEIGHTED: {
      double sum = 0.0;
      for (int16_t v : values) sum += v;
      return round_sample(sum / static_cast<double>(values.size()));
    }
    case StackingMode::AUTO:
    default:
      return compute_median(values);
  }
}

// Two values cannot outvote each other, so they are always combined and only
// flagged when they disagree.  With three or more, values beyond the
// rejection distance from the median are left out; the median itself stands
// in when none is close to it.
int16_t StackerStage::stack_weighted(const std::vector<int16_t>& values,
                                     const std::vector<double>& weights,
                                     double reject_distance,
                                     bool& low_confidence) {
  low_confidence = false;
  const size_t n = values.size();
  if (n == 0) return 0;
  if (n == 1) return values[0];

  if (n == 2) {
    low_confidence =
        std::abs(static_cast<double>(values[0]) - values[1]) > reject_distance;
    return round_sample((weights[0] * values[0] + weights[1] * values[1]) /
                        (weights[0] + weights[1]));
  }

  std::vector<double> sorted(values.begin(), values.end());
  const double median = median_of(std::move(sorted));

  double weight_sum = 0.0;
  double weighted = 0.0;
  for (size_t k = 0; k < n; ++k) {
    if (std::abs(values[k] - median) > reject_distance) continue;
    weight_sum += weights[k];
    weighted += weights[k] * values[k];
  }
  const double result = weight_sum > 0.0 ? weighted / weight_sum : median;

  size_t agreeing = 0;
  for (int16_t v : values) {
    if (std::abs(v - result) <= reject_distance) ++agreeing;
  }
  low_confidence = 2 * agreeing <= n;
  return round_sample(result);
}

int16_t StackerStage::compute_median(std::vector<int16_t> v) const {
  if (v.empty()) {
    return 0;
  }
  size_t n = v.size();
  std::sort(v.begin(), v.end());
  if (n % 2 == 0) {
    return static_cast<int16_t>(
        (static_cast<int32_t>(v[n / 2 - 1]) + v[n / 2]) / 2);
  }
  return v[n / 2];
}

int32_t StackerStage::compute_mean(const std::vector<int16_t>& v) const {
  if (v.empty()) {
    return 0;
  }
  int64_t sum = 0;
  for (int16_t s : v) {
    sum += s;
  }
  return static_cast<int32_t>(sum / static_cast<int64_t>(v.size()));
}

std::vector<int16_t> StackerStage::diff_dod(
    const std::vector<int16_t>& input) const {
  if (input.size() < 3) {
    return {};
  }
  const double median = compute_median(input);
  const double window = std::abs(median) / 10.0;
  std::vector<int16_t> result;
  for (int16_t v : input) {
    if (v > median - window && v < median + window) {
      result.push_back(v);
    }
  }
  return result;
}

// ── Audio / EFM ──────────────────────────────────────────────────────────────

std::vector<int32_t> StackerStage::stack_audio(
    size_t pair, const std::vector<FrameID>& source_ids,
    const std::vector<std::shared_ptr<const VideoFrameRepresentation>>& sources,
    size_t best_src) const {
  if (m_audio_stacking_mode == AudioStackingMode::DISABLED) {
    if (best_src < sources.size() && source_ids[best_src] != UINT64_MAX &&
        sources[best_src] && sources[best_src]->has_audio()) {
      return sources[best_src]->get_audio_samples(pair, source_ids[best_src]);
    }
    return {};
  }

  // Inputs are stacked at the same frame ID across sources, so per-frame
  // pair counts always agree (audio_pairs_in_frame is a model invariant).
  std::vector<std::vector<int32_t>> all_audio;
  for (size_t i = 0; i < sources.size(); ++i) {
    if (source_ids[i] == UINT64_MAX || !sources[i]) {
      continue;
    }
    if (pair >= sources[i]->audio_channel_pair_count() ||
        !sources[i]->has_frame(source_ids[i])) {
      continue;
    }
    auto s = sources[i]->get_audio_samples(pair, source_ids[i]);
    if (!s.empty()) {
      all_audio.push_back(std::move(s));
    }
  }

  if (all_audio.empty()) {
    return {};
  }
  if (all_audio.size() == 1) {
    return all_audio[0];
  }

  size_t n = all_audio[0].size();
  std::vector<int32_t> result(n);
  for (size_t si = 0; si < n; ++si) {
    std::vector<int32_t> vals;
    for (const auto& a : all_audio) {
      if (si < a.size()) {
        vals.push_back(a[si]);
      }
    }
    if (m_audio_stacking_mode == AudioStackingMode::MEDIAN) {
      result[si] = audio_median(vals);
    } else {
      result[si] = audio_mean(vals);
    }
  }
  return result;
}

std::vector<uint8_t> StackerStage::stack_efm(
    const std::vector<FrameID>& source_ids,
    const std::vector<std::shared_ptr<const VideoFrameRepresentation>>& sources,
    size_t best_src) const {
  if (m_efm_stacking_mode == EFMStackingMode::DISABLED) {
    if (best_src < sources.size() && source_ids[best_src] != UINT64_MAX &&
        sources[best_src] && sources[best_src]->has_efm()) {
      return sources[best_src]->get_efm_samples(source_ids[best_src]);
    }
    return {};
  }

  std::vector<std::vector<uint8_t>> all_efm;
  size_t reference = 0;
  for (size_t i = 0; i < sources.size(); ++i) {
    if (source_ids[i] == UINT64_MAX || !sources[i]) {
      continue;
    }
    if (!sources[i]->has_efm() || !sources[i]->has_frame(source_ids[i])) {
      continue;
    }
    auto s = sources[i]->get_efm_samples(source_ids[i]);
    if (!s.empty()) {
      if (i == best_src) {
        reference = all_efm.size();
      }
      all_efm.push_back(std::move(s));
    }
  }

  if (all_efm.empty()) {
    return {};
  }

  // Confidence stacking works on the packed bytes: the doubt nibble is what it
  // weighs the sources by, and it emits a doubt of its own derived from how
  // far the sources agreed. See efm_confidence_stack.h.
  if (m_efm_stacking_mode == EFMStackingMode::CONFIDENCE) {
    return stack_efm_confidence(all_efm, reference);
  }

  // Mean and median are defined on t-values, not on the packed byte: taking
  // the mean or median of whole bytes would fold each producer's doubt nibble
  // into the t-value field and corrupt it. Strip the doubt first, so a stacked
  // t-value comes out with zero doubt - which packs to the bare t-value. That
  // is honest for these two modes: the stacked value is an average that no
  // producer vouched for, and the doubt of the inputs says nothing about it.
  for (auto& stream : all_efm) {
    for (auto& value : stream) {
      value = efm_tvalue(value);
    }
  }

  if (all_efm.size() == 1) {
    return all_efm[0];
  }

  size_t n = all_efm[0].size();
  std::vector<uint8_t> result(n);
  for (size_t si = 0; si < n; ++si) {
    std::vector<uint8_t> vals;
    for (const auto& e : all_efm) {
      if (si < e.size()) {
        vals.push_back(e[si]);
      }
    }
    if (m_efm_stacking_mode == EFMStackingMode::MEDIAN) {
      result[si] = efm_median(vals);
    } else {
      result[si] = efm_mean(vals);
    }
  }
  return result;
}

namespace {

// Saturate a combined audio value to the 24-bit two's-complement range
// carried in int32_t (see audio_channel_pair.h).
int32_t saturate_audio_24bit(int64_t value) {
  constexpr int64_t kMin = -8388608;
  constexpr int64_t kMax = 8388607;
  return static_cast<int32_t>(std::clamp(value, kMin, kMax));
}

}  // namespace

int32_t StackerStage::audio_mean(const std::vector<int32_t>& v) const {
  if (v.empty()) {
    return 0;
  }
  int64_t s = 0;
  for (int32_t x : v) {
    s += x;
  }
  return saturate_audio_24bit(s / static_cast<int64_t>(v.size()));
}

int32_t StackerStage::audio_median(std::vector<int32_t> v) const {
  if (v.empty()) {
    return 0;
  }
  size_t n = v.size();
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n / 2),
                   v.end());
  if (n % 2 == 0) {
    std::nth_element(v.begin(),
                     v.begin() + static_cast<std::ptrdiff_t>((n - 1) / 2),
                     v.end());
    return saturate_audio_24bit(
        (static_cast<int64_t>(v[(n - 1) / 2]) + v[n / 2]) / 2);
  }
  return saturate_audio_24bit(v[n / 2]);
}

uint8_t StackerStage::efm_mean(const std::vector<uint8_t>& v) const {
  if (v.empty()) {
    return 0;
  }
  uint32_t s = 0;
  for (uint8_t x : v) {
    s += x;
  }
  return static_cast<uint8_t>(s / static_cast<uint32_t>(v.size()));
}

uint8_t StackerStage::efm_median(std::vector<uint8_t> v) const {
  if (v.empty()) {
    return 0;
  }
  size_t n = v.size();
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n / 2),
                   v.end());
  if (n % 2 == 0) {
    std::nth_element(v.begin(),
                     v.begin() + static_cast<std::ptrdiff_t>((n - 1) / 2),
                     v.end());
    return static_cast<uint8_t>(
        (static_cast<uint16_t>(v[(n - 1) / 2]) + v[n / 2]) / 2);
  }
  return v[n / 2];
}

// ── Parameters
// ────────────────────────────────────────────────────────────────

std::vector<ParameterDescriptor> StackerStage::get_parameter_descriptors(
    VideoSystem /*project_format*/, SourceType /*source_type*/) const {
  std::vector<ParameterDescriptor> d;

  d.push_back(
      {"mode", "Stacking Mode",
       "Per-pixel combining algorithm. Weighted = each source weighted by its "
       "measured noise, rejecting values far from the median and marking "
       "samples the sources disagree on as dropout. Auto = Smart Mean with 3+ "
       "usable sources, Mean otherwise",
       ParameterType::STRING,
       ParameterConstraints{
           std::nullopt,
           std::nullopt,
           ParameterValue{std::string("Weighted")},
           {"Weighted", "Auto", "Mean", "Median", "Smart Mean"},
           false,
           std::nullopt}});
  d.push_back(
      {"reject_sigma", "Rejection Distance",
       "Weighted mode: how far from the per-sample median, in multiples of "
       "the measured noise, a value may lie and still be used (2-10, default "
       "5)",
       ParameterType::DOUBLE,
       ParameterConstraints{ParameterValue{2.0},
                            ParameterValue{10.0},
                            ParameterValue{5.0},
                            {},
                            false,
                            ParameterDependency{"mode", {"Weighted"}}}});
  d.push_back({"smart_threshold", "Smart Threshold",
               "Threshold for Smart Mean mode (0-128, default 15)",
               ParameterType::INT32,
               ParameterConstraints{ParameterValue{static_cast<int32_t>(0)},
                                    ParameterValue{static_cast<int32_t>(128)},
                                    ParameterValue{static_cast<int32_t>(15)},
                                    {},
                                    false,
                                    std::nullopt}});
  d.push_back({"no_diff_dod", "Disable Differential Dropout Detection",
               "Strictly trust dropout markings; disable diff_dod recovery",
               ParameterType::BOOL,
               ParameterConstraints{std::nullopt,
                                    std::nullopt,
                                    ParameterValue{false},
                                    {},
                                    false,
                                    std::nullopt}});
  d.push_back({"passthrough", "Passthrough Universal Dropouts",
               "Where every source is flagged as dropout, output the sources' "
               "combined values (still marked) instead of black",
               ParameterType::BOOL,
               ParameterConstraints{std::nullopt,
                                    std::nullopt,
                                    ParameterValue{false},
                                    {},
                                    false,
                                    std::nullopt}});
  d.push_back(
      {"sample_align", "Align Source Sample Grids",
       "Measure each source's 4FSC sample grid against the first source and "
       "shift it into line before stacking. One sample is 90° of subcarrier, "
       "so a source a sample out erodes chroma when combined unshifted",
       ParameterType::BOOL,
       ParameterConstraints{std::nullopt,
                            std::nullopt,
                            ParameterValue{true},
                            {},
                            false,
                            std::nullopt}});
  d.push_back({"audio_stacking", "Audio Stacking Mode",
               "How to combine audio: Disabled | Mean | Median",
               ParameterType::STRING,
               ParameterConstraints{std::nullopt,
                                    std::nullopt,
                                    ParameterValue{std::string("Mean")},
                                    {"Disabled", "Mean", "Median"},
                                    false,
                                    std::nullopt}});
  d.push_back(
      {"efm_stacking", "EFM Stacking Mode",
       "How to combine EFM t-values: Disabled | Confidence | Mean | Median",
       ParameterType::STRING,
       ParameterConstraints{std::nullopt,
                            std::nullopt,
                            ParameterValue{std::string("Disabled")},
                            {"Disabled", "Confidence", "Mean", "Median"},
                            false,
                            std::nullopt}});
  return d;
}

std::map<std::string, ParameterValue> StackerStage::get_parameters() const {
  const char* mode_names[] = {"Auto", "Mean", "Median", "Smart Mean",
                              "Weighted"};
  const auto mi = static_cast<size_t>(m_mode);

  const char* audio_names[] = {"Disabled", "Mean", "Median"};
  const char* efm_names[] = {"Disabled", "Mean", "Median", "Confidence"};

  return {
      {"mode", ParameterValue{std::string(mode_names[mi])}},
      {"reject_sigma", ParameterValue{m_reject_sigma}},
      {"smart_threshold", ParameterValue{m_smart_threshold}},
      {"no_diff_dod", ParameterValue{m_no_diff_dod}},
      {"passthrough", ParameterValue{m_passthrough}},
      {"sample_align", ParameterValue{m_sample_align}},
      {"audio_stacking",
       ParameterValue{
           std::string(audio_names[static_cast<int>(m_audio_stacking_mode)])}},
      {"efm_stacking", ParameterValue{std::string(
                           efm_names[static_cast<int>(m_efm_stacking_mode)])}}};
}

bool StackerStage::set_parameters(
    const std::map<std::string, ParameterValue>& params) {
  parameters_ = params;

  for (const auto& [key, value] : params) {
    if (key == "mode") {
      if (const auto* v = std::get_if<std::string>(&value)) {
        // Smart Neighbor and Neighbor were never implemented and always
        // stacked as Median, which they are still read as.
        if (*v == "Weighted") {
          m_mode = StackingMode::WEIGHTED;
        } else if (*v == "Auto") {
          m_mode = StackingMode::AUTO;
        } else if (*v == "Mean") {
          m_mode = StackingMode::MEAN;
        } else if (*v == "Median" || *v == "Smart Neighbor" ||
                   *v == "Neighbor") {
          m_mode = StackingMode::MEDIAN;
        } else if (*v == "Smart Mean") {
          m_mode = StackingMode::SMART_MEAN;
        } else {
          ORC_LOG_WARN("StackerStage: unknown mode '{}'", *v);
          return false;
        }
      } else if (const auto* iv = std::get_if<int32_t>(&value)) {
        // Legacy integer modes: -1 Auto, 0 Mean, 1 Median, 2 Smart Mean,
        // 3 and 4 the neighbour modes.
        static constexpr StackingMode kLegacyModes[] = {
            StackingMode::AUTO,   StackingMode::MEAN,
            StackingMode::MEDIAN, StackingMode::SMART_MEAN,
            StackingMode::MEDIAN, StackingMode::MEDIAN};
        if (*iv < -1 || *iv > 4) {
          return false;
        }
        m_mode = kLegacyModes[*iv + 1];
      } else {
        return false;
      }
    } else if (key == "reject_sigma") {
      double k = 0.0;
      if (const auto* v = std::get_if<double>(&value)) {
        k = *v;
      } else if (const auto* iv = std::get_if<int32_t>(&value)) {
        k = *iv;
      } else {
        return false;
      }
      if (!(k >= 2.0 && k <= 10.0)) {
        return false;
      }
      m_reject_sigma = k;
    } else if (key == "smart_threshold") {
      if (const auto* v = std::get_if<int32_t>(&value)) {
        if (*v < 0 || *v > 128) {
          return false;
        }
        m_smart_threshold = *v;
      } else {
        return false;
      }
    } else if (key == "no_diff_dod") {
      if (const auto* v = std::get_if<bool>(&value)) {
        m_no_diff_dod = *v;
      } else {
        return false;
      }
    } else if (key == "passthrough") {
      if (const auto* v = std::get_if<bool>(&value)) {
        m_passthrough = *v;
      } else {
        return false;
      }
    } else if (key == "sample_align") {
      if (const auto* v = std::get_if<bool>(&value)) {
        m_sample_align = *v;
      } else {
        return false;
      }
    } else if (key == "audio_stacking") {
      if (const auto* v = std::get_if<std::string>(&value)) {
        if (*v == "Disabled") {
          m_audio_stacking_mode = AudioStackingMode::DISABLED;
        } else if (*v == "Mean") {
          m_audio_stacking_mode = AudioStackingMode::MEAN;
        } else if (*v == "Median") {
          m_audio_stacking_mode = AudioStackingMode::MEDIAN;
        } else {
          ORC_LOG_WARN("StackerStage: unknown audio_stacking '{}'", *v);
          return false;
        }
      } else {
        return false;
      }
    } else if (key == "efm_stacking") {
      if (const auto* v = std::get_if<std::string>(&value)) {
        if (*v == "Disabled") {
          m_efm_stacking_mode = EFMStackingMode::DISABLED;
        } else if (*v == "Mean") {
          m_efm_stacking_mode = EFMStackingMode::MEAN;
        } else if (*v == "Median") {
          m_efm_stacking_mode = EFMStackingMode::MEDIAN;
        } else if (*v == "Confidence") {
          m_efm_stacking_mode = EFMStackingMode::CONFIDENCE;
        } else {
          ORC_LOG_WARN("StackerStage: unknown efm_stacking '{}'", *v);
          return false;
        }
      } else {
        return false;
      }
    }
  }
  return true;
}

// ── Report / Preview
// ──────────────────────────────────────────────────────────

StagePreviewCapability StackerStage::get_preview_capability() const {
  std::lock_guard<std::mutex> lk(execute_mutex_);
  return PreviewHelpers::make_signal_preview_capability(cached_output_);
}

}  // namespace orc
