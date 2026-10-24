/*
 * File:        stack_measurement.h
 * Module:      decode-orc Plugin SDK (support)
 * Purpose:     Measurements between captures of one disc: sample-grid offset,
 *              pair noise, and the pressing noise model
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/stage/common_types.h>
#include <orc/stage/dropout/dropout_run.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace orc {

// Flat frame samples in the CVBS_U10_4FSC 10-bit domain, as
// VideoFrameRepresentation::sample_type.
using StackSample = int16_t;

// ── Frame geometry ──────────────────────────────────────────────────────────

// Lines in field 1 of a flat frame: 313 for 625-line systems, 263 for 525.
size_t field1_line_count(VideoSystem system);

// Lines at the top of each field that carry blanking and VBI rather than
// picture.
inline constexpr size_t kFieldBlankingLines = 25;

// One byte per sample of a flat frame of |total| samples, 1 where |runs| flag
// the sample.
std::vector<uint8_t> dropout_mask(const std::vector<DropoutRun>& runs,
                                  size_t total);

// Translate every line of a flat frame so that sample n takes the value
// previously at n + |offset|, honouring the non-orthogonal PAL line layout.
// The edge sample is replicated into the samples this vacates, which sit at
// the line ends among sync and blanking.
void shift_frame_samples(std::vector<StackSample>& frame, VideoSystem system,
                         size_t nominal_width, size_t height, int32_t offset);

// Move dropout runs with the samples they describe, clamped to their own line.
void shift_dropout_runs(std::vector<DropoutRun>& runs, VideoSystem system,
                        size_t nominal_width, size_t height, int32_t offset);

// ── Sample-grid offset ──────────────────────────────────────────────────────
//
// Independent decodes of the same disc need not start a line on the same 4FSC
// sample, so two captures can sit several samples apart.  The offset is
// measured on low-passed luma: composite repeats every 4 samples because of
// the subcarrier, so correlating raw composite finds a true offset L almost
// as strongly at L±4, and a 4-sample box removes the subcarrier to leave one
// peak.  The box also flattens that peak, so the lag it finds is refined on
// the raw samples near it, where the subcarrier makes the exact lag stand out.

// Largest grid difference searched for, either way.
inline constexpr int32_t kMaxSampleGridShift = 32;

// Width of the box that removes the subcarrier: one cycle at 4FSC.
inline constexpr size_t kGridLumaBox = 4;

// Normalised correlation a measured offset must reach to be acted on.
inline constexpr double kMinGridCorrelation = 0.5;

// Accumulates the correlation of one capture against a reference over frames
// of the same picture, then reports the offset of its grid.
class SampleGridMeter {
 public:
  SampleGridMeter();

  // Add one frame of each.  Only a few lines of active picture are used.
  void add_frame(const std::vector<StackSample>& reference,
                 const std::vector<StackSample>& other, VideoSystem system,
                 size_t nominal_width, size_t height);

  size_t frames() const { return frames_; }

  struct Result {
    // The other capture's sample n matches the reference's sample n - offset;
    // nullopt when the frames carry no detail or correlate too weakly.
    std::optional<int32_t> offset;
    // Correlation of the low-passed luma at the offset found.
    double correlation = 0.0;
    // True when neither carried detail to correlate.
    bool flat = false;
  };
  Result measure() const;

 private:
  struct LagCorrelation {
    explicit LagCorrelation(int32_t max_lag);
    std::optional<double> at(int32_t lag) const;

    int32_t max_lag;
    std::vector<double> xy;
    std::vector<double> xx;
    std::vector<double> yy;
  };

  LagCorrelation coarse_;
  LagCorrelation fine_;
  size_t frames_ = 0;
};

// ── Pair noise ──────────────────────────────────────────────────────────────

// Histogram of one pair's sample differences, which are integers within
// ±1023 on the 10-bit scale.
class DifferenceHistogram {
 public:
  void add(int32_t difference) {
    ++bins_[slot(std::clamp(difference, -kRange, kRange))];
    ++count_;
  }

  // Variance of the differences within 4 robust standard deviations of their
  // median, or nullopt with fewer than 1000 differences.  Wider differences
  // are picture leaking through — an edge a sub-sample grid residual leaves,
  // or unflagged sparkle — rather than noise.
  std::optional<double> robust_variance() const;

 private:
  static constexpr int32_t kRange = 1023;
  static size_t slot(int32_t d) {
    return static_cast<size_t>(d) + static_cast<size_t>(kRange);
  }

  std::vector<uint64_t> bins_ = std::vector<uint64_t>(2 * kRange + 1, 0);
  uint64_t count_ = 0;
};

// Accumulates the difference of every pair of captures over frames of the
// same picture whose grids already agree, on a decimated grid of active
// picture, skipping flagged samples.
class PairNoiseMeter {
 public:
  explicit PairNoiseMeter(size_t inputs);

  // |planes[i]| and |masks[i]| are capture i's frame and dropout mask, or null
  // when it has no frame here.
  void add_frame(const std::vector<const std::vector<StackSample>*>& planes,
                 const std::vector<const std::vector<uint8_t>*>& masks,
                 VideoSystem system, size_t nominal_width, size_t height);

  // Each pair's robust difference variance, indexed i * n + j for i < j.
  std::vector<std::optional<double>> pair_variances() const;

 private:
  size_t inputs_;
  std::vector<DifferenceHistogram> pairs_;
};

// ── Pressing noise model ────────────────────────────────────────────────────
//
// Captures of a side come from more than one pressing, and every disc of a
// pressing carries the noise recorded into its stamper.  The difference of
// two captures of one pressing therefore carries only their own noise, while
// captures of different pressings also differ by both pressings' noise:
//     v_ij = E_i + E_j                    same pressing
//     v_ij = E_i + E_j + G_a + G_b        pressings a and b
// where E is a capture's own noise and G a pressing's shared noise.
//
// Pressings are told apart without knowing E: for two captures of one
// pressing, v_ik − v_jk = E_i − E_j for every other capture k, while for
// captures of different pressings it jumps by the pressings' noise between a
// k of one pressing and a k of the other.  This needs at least 4 captures.  A
// capture alone in its pressing cannot be told from one that is merely noisy,
// as its pressing noise looks like its own.  E and G are then solved from all
// the pairs by least squares.

// Smallest σ a capture is given, so identical captures keep a finite weight.
inline constexpr double kMinPressingNoiseSigma = 0.5;

// One capture's noise against the picture common to every capture: its
// standard deviation in 10-bit codes, its stacking weight (relative to the
// others'), and the pressing group it was placed in, numbered from 0.
struct PressingNoise {
  double sigma = 0.0;
  double weight = 0.0;
  size_t group = 0;
};

// Each capture's pressing group from the pair variances (indexed i * n + j
// for i < j), numbered from 0; nullopt for a capture in no measured pair.
// Fewer than 4 measured captures cannot show a pressing, so they form one
// group.  The test needs every pair difference to be noise alone, so a few
// frames in which picture leaks into some pairs can split one pressing:
// decide the groups from frames spread across the whole input, as pressings
// do not change within a side, rather than from any short stretch of it.
std::vector<std::optional<size_t>> group_by_pressing(
    size_t n, const std::vector<std::optional<double>>& pair_variance);

// Each capture's noise from the pair variances (indexed i * n + j for i < j),
// with the captures grouped by |groups_given| (one entry per capture, as
// group_by_pressing() returns; a capture without a group is taken as alone
// in its pressing).  The weights minimise the noise of the combined picture,
// so each pressing counts roughly once however many of its discs there are.
// Captures in no measured pair are left unmeasured.
std::vector<std::optional<PressingNoise>> fit_pressing_noise(
    size_t n, const std::vector<std::optional<double>>& pair_variance,
    const std::vector<std::optional<size_t>>& groups_given);

// As above, grouping the captures by pressing from the same pair variances.
std::vector<std::optional<PressingNoise>> fit_pressing_noise(
    size_t n, const std::vector<std::optional<double>>& pair_variance);

}  // namespace orc
