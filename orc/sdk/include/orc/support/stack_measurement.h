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
#include <string>
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
    // The same offset to a fraction of a sample, from the raw correlation
    // interpolated between whole lags; set whenever offset is.  Only offset
    // is corrected: this reports what a whole-sample shift leaves.
    std::optional<double> fractional_offset;
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

// One capture's luma against the picture common to every capture, from
// low-passed active picture: its samples follow gain · (common − mean) +
// mean + level, where mean is the common picture's mean, both relative to
// the typical (median) capture's.  Captures of one
// disc through different players and decodes differ by a few per cent in
// gain and a few codes in level; the noise measurement below is immune to
// that, but a stack that averages them averages slightly different
// contrasts, and its per-sample rejection sees the difference as
// disagreement: on Community South one capture 7 % up in gain and 11 codes
// up in level sat 60 codes off the others on white, against a rejection
// distance of 80.
struct LevelMatch {
  double gain = 1.0;
  double level = 0.0;
  double mean = 0.0;
};

// Accumulates each capture's luma level against the picture common to every
// capture over frames of the same picture whose grids already agree,
// skipping flagged samples.  Works on the same decimated active picture as
// PairNoiseMeter.
class LumaLevelMeter {
 public:
  explicit LumaLevelMeter(size_t inputs);

  // |planes[i]| and |masks[i]| are capture i's frame and dropout mask, or null
  // when it has no frame here.
  void add_frame(const std::vector<const std::vector<StackSample>*>& planes,
                 const std::vector<const std::vector<uint8_t>*>& masks,
                 VideoSystem system, size_t nominal_width, size_t height);

  // Each capture's level; nullopt where the frames carried too little
  // picture to show a gain.
  std::vector<std::optional<LevelMatch>> levels() const;

 private:
  struct LevelSums {
    double n = 0.0;
    double x = 0.0;
    double y = 0.0;
    double xx = 0.0;
    double xy = 0.0;
  };

  size_t inputs_;
  std::vector<LevelSums> sums_;
};

// Bring a capture's samples onto the typical capture's level: each sample s
// becomes (s − mean − level) / gain + mean.  The whole frame is mapped, sync
// and blanking with the picture, so the active picture has no edge.
void apply_luma_level(std::vector<StackSample>& plane, const LevelMatch& level);

// Accumulates the noise of every pair of captures over frames of the same
// picture whose grids already agree, skipping flagged samples.
//
// A pair's difference is noise alone only where the picture cannot leak into
// it.  Two captures that differ in gain by a few per cent, or that sit a
// fraction of a sample apart, difference every edge and grating into a
// picture-shaped residue, and on a detailed picture (a test card) that
// residue doubled the measured noise of the three captures of Community
// South whose gain differed from the rest — they were weighted out of the
// stack, though their noise was the only noise not shared with the others.
// The meter therefore works on blocks of 32 samples by 8 lines of active
// picture on a decimated grid, and keeps only the blocks where the picture
// common to every capture is flat: no wider than a few times the flattest
// tenth of the frame's blocks, which on a flat frame is every block.  Of the
// frames, only those whose flattest tenth is about as flat as the flattest
// frame's count, as a frame of film or saturated colour has no truly flat
// block, and its least busy blocks still carry the captures' disagreement
// over grain and colour.  Each pair's variance within a block is taken about
// the block's own mean, so a difference in level never counts, and what
// picture a kept block does hold, which leaks through the pair's luma gain
// difference (levels() measures it), is taken out with that much of the
// pair's low-passed luma.  The variances are pooled over the flat blocks; a
// block whose variance for a pair is far above the pair's typical block
// variance in that frame holds unflagged sparkle and is left out.  When a
// pair gathers too few flat samples (a dense picture, few frames) its
// variance falls back to the robust variance of its differences over the
// whole decimated picture, as DifferenceHistogram measures it.
class PairNoiseMeter {
 public:
  explicit PairNoiseMeter(size_t inputs);

  // |planes[i]| and |masks[i]| are capture i's frame and dropout mask, or null
  // when it has no frame here.
  void add_frame(const std::vector<const std::vector<StackSample>*>& planes,
                 const std::vector<const std::vector<uint8_t>*>& masks,
                 VideoSystem system, size_t nominal_width, size_t height);

  // Each pair's noise variance, indexed i * n + j for i < j.
  std::vector<std::optional<double>> pair_variances() const;

  // Pairs whose variance came from the whole picture rather than from flat
  // blocks.
  size_t whole_picture_pairs() const;

  // Each capture's level against the picture common to every capture;
  // nullopt where the frames carried too little picture to show a gain.
  std::vector<std::optional<LevelMatch>> levels() const {
    return levels_.levels();
  }

 private:
  // A pair's sums over flat blocks, each about its block's mean: the
  // difference d, the pair's low-passed luma l, and the degrees of freedom.
  struct FlatVariance {
    double dd = 0.0;
    double dl = 0.0;
    double ll = 0.0;
    double dof = 0.0;
  };
  // One frame's flat-block sums per pair, and the spread of its flattest
  // tenth of blocks.
  struct FlatFrame {
    double spread = 0.0;
    std::vector<FlatVariance> pairs;
  };

  // Each pair's sums pooled over the frames as flat as the flattest.
  std::vector<FlatVariance> flat_variances() const;

  size_t inputs_;
  std::vector<DifferenceHistogram> pairs_;
  std::vector<FlatFrame> flat_frames_;
  LumaLevelMeter levels_;
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
//
// Stampers are themselves made from mothers, and the stampers of one mother
// share its noise as well as each carrying their own.  So pressings fall into
// families, and captures of different pressings of one family differ by the
// two pressings' noise only, while captures of different families also
// differ by both families' noise:
//     v_ij = E_i + E_j + G_a + G_b                pressings a, b of one family
//     v_ij = E_i + E_j + G_a + G_b + F_x + F_y    families x and y
// The v_ik − v_jk test finds the pressings, and sees two pressings of one
// family as unrelated as any other two.  The families are found by fit
// instead: merging two that belong together explains the pairs far better
// than any flat grouping can.  On Community South eight captures of two
// pressings of one family read pair variances of 35-50 within a pressing,
// 52-85 across the two, and ~400 against the three captures of other
// families; no flat grouping fitted them to better than 45 %, the nested one
// to 4 %.

// Smallest σ a capture is given, so identical captures keep a finite weight.
inline constexpr double kMinPressingNoiseSigma = 0.5;

// One capture's noise against the picture common to every capture: its
// standard deviation in 10-bit codes, its stacking weight (relative to the
// others'), and the pressing group and family it was placed in, each
// numbered from 0.  own_variance, shared_variance and family_variance are
// the model's E, G and F for it (G is 0 for a capture alone in its pressing,
// whose pressing noise is in its E, and F is 0 for a pressing alone in its
// family, whose family noise is in its G).  The weight is the capture's
// entry of Σ⁻¹1 for the model's covariance Σ of every capture measured, so
// weights are comparable between captures fitted together.
struct PressingNoise {
  double sigma = 0.0;
  double weight = 0.0;
  size_t group = 0;
  double own_variance = 0.0;
  double shared_variance = 0.0;
  size_t family = 0;
  double family_variance = 0.0;
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

// Frames the pressing groups are decided from.  Pressings do not change
// within a side, so the captures are grouped once, from probe frames spread
// across the whole input.  Grouping from a few frames split one pressing into
// two to five in places on real captures: a frame or two in which picture
// leaks into some pairs' differences is enough to break the test.  Frames
// spread over the input dilute any such stretch, and fewer than this merged
// two pressings of Community South's that this many tell apart.  Twice as
// many candidates as probes are spread evenly, every other one tried first
// (pressing_probe_candidate()), so frames that cannot be used are replaced
// from nearby without bunching the probes towards the start.  The Stacker and
// the Source Alignment capture comparison both probe this way, so they group
// the same captures alike.
inline constexpr size_t kPressingProbeFrames = 16;
inline constexpr size_t kPressingProbeCandidates = 2 * kPressingProbeFrames;

// The |order|th candidate tried (from 0, below kPressingProbeCandidates), as
// an offset from the start of a stretch of |count| frames: the odd candidates
// of kPressingProbeCandidates spread evenly over it, then the even ones.
inline size_t pressing_probe_candidate(size_t order, size_t count) {
  constexpr size_t half = kPressingProbeCandidates / 2;
  const size_t k = order < half ? 2 * order + 1 : 2 * (order - half) + 2;
  return (count * k) / (kPressingProbeCandidates + 1);
}

// Each capture's noise from the pair variances (indexed i * n + j for i < j),
// with the captures grouped by |groups_given| (one entry per capture, as
// group_by_pressing() returns; a capture without a group is taken as alone
// in its pressing).  The weights minimise the noise of the combined picture,
// so each pressing counts roughly once however many of its discs there are.
// Captures in no measured pair are left unmeasured.
std::vector<std::optional<PressingNoise>> fit_pressing_noise(
    size_t n, const std::vector<std::optional<double>>& pair_variance,
    const std::vector<std::optional<size_t>>& groups_given);

// As above, with the pressings further grouped by |families_given| (one
// entry per capture; captures of one pressing must share a family, and a
// capture without one is taken as alone in its family).
std::vector<std::optional<PressingNoise>> fit_pressing_noise(
    size_t n, const std::vector<std::optional<double>>& pair_variance,
    const std::vector<std::optional<size_t>>& groups_given,
    const std::vector<std::optional<size_t>>& families_given);

// Each capture's family, numbered from 0, for the pressing groups |groups|
// (as group_by_pressing() returns them): starting from every pressing in a
// family of its own, two families are merged while a merge cuts the worst
// misfit (see check_pressing_fit()) by more than kFamilyMisfitGain.  nullopt
// for a capture without a group.
//
// The pair variances cannot always say which two to merge.  Of four
// clusters P, Q, R, S, merging P with Q and merging R with S give the same
// pair variances (one tree, rooted on either side of the edge between the
// two pairs), yet opposite weights: whichever pair is merged is taken to
// share noise, and is weighted as about one disc.  Every disc of a title
// went through the same generations, master to mother to stamper, so the
// noise each carries in all should be about the same, and of the merges that
// fit within kFamilyMisfitGain of the best, the one that leaves the
// captures' σ most alike (the smallest spread of ln σ²) is taken.  On
// Community South that puts the eight in one family, every capture's σ²
// within 10 % of 190; the other way round left the eight at 20-55 and the
// three others at 370-400, against the measured covariance's eleven alike.
std::vector<std::optional<size_t>> group_into_families(
    size_t n, const std::vector<std::optional<double>>& pair_variance,
    const std::vector<std::optional<size_t>>& groups);

// Cut in the worst misfit a merge into one family must make.  A merge of two
// pressings that share nothing fits them with no family noise and leaves the
// misfit where it was, give or take the fit's rounding.
inline constexpr double kFamilyMisfitGain = 0.02;

// As above, grouping the captures by pressing and into families from the
// same pair variances.
std::vector<std::optional<PressingNoise>> fit_pressing_noise(
    size_t n, const std::vector<std::optional<double>>& pair_variance);

// The weights, Σ⁻¹1, that minimise the noise of a combination of
// |captures| (as fit_pressing_noise() returned them, any subset of one fit)
// under their model's covariance Σ.  For every capture of one fit this is
// the weights the fit gave; for a subset it is what those captures should
// carry when combined only with each other, as the still-frame copies of
// one frame are: copies of another frame of the disc share no stamper noise
// with the frame they are stacked into, only with each other.
std::vector<double> pressing_weights(
    const std::vector<PressingNoise>& captures);

// Noise σ of the best combination of |captures| (any subset of one fit, as
// for pressing_weights()) under their model's covariance Σ: √(1 / 1ᵀΣ⁻¹1).
// Over every capture of a trusted fit this is the fit check's
// weighted_sigma; over all captures but one it is what the stack would be
// without that one, which says what that one adds.  0 for no captures.
double combined_sigma(const std::vector<PressingNoise>& captures);

// A pressing group or family, numbered from 0, as reports name it: A, B, ...
// then A2, B2 past Z.
std::string pressing_name(size_t group);

// ── Checking the fit ────────────────────────────────────────────────────────
//
// The weights fit_pressing_noise() gives are the best possible only if its
// model is right, and under its own model they can never do worse than an
// equal mean, so the model cannot be trusted to vouch for itself.  What can
// be checked is whether it explains the measurements it was fitted to: the
// pair variances it implies against the ones measured.  A model with the
// wrong pressings, or one fitted to pair variances that carry picture,
// misfits some pairs by tens of per cent, where a right one fits every pair
// to within the measurement's own scatter.  A stack whose model does not
// explain its inputs should not be trusted to weight them, and falls back to
// an equal mean, which no model can mislead.

// Largest misfit of any pair, as |ln(measured / modelled)|, that a model is
// trusted with: 35 %.
inline constexpr double kMaxPressingMisfit = 0.3;

struct PressingFitCheck {
  // Pairs compared.
  size_t pairs = 0;
  // Largest and root-mean-square |ln(measured / modelled)| over the pairs.
  double worst_misfit = 0.0;
  double rms_misfit = 0.0;
  // The pair, inputs numbered from 0, that misfit worst.
  size_t worst_i = 0;
  size_t worst_j = 0;
  // Noise σ the model predicts for the combined picture with the fitted
  // weights, with equal weights, and for the best single capture.
  double weighted_sigma = 0.0;
  double equal_sigma = 0.0;
  double best_single_sigma = 0.0;
  // True when the model explains every pair to within kMaxPressingMisfit.
  bool trusted = false;
};

// Check |noise|, as fit_pressing_noise() returned it, against the pair
// variances it was fitted to.  With fewer than 3 captures measured every
// model fits, and the check reports the model as trusted.
PressingFitCheck check_pressing_fit(
    size_t n, const std::vector<std::optional<double>>& pair_variance,
    const std::vector<std::optional<PressingNoise>>& noise);

// What a stack whose model is not trusted uses instead of |noise|: every
// capture weighted equally, with σ² half the mean variance of its pairs —
// the scale its samples spread about the others' at, which is what the
// stack's rejection needs — and the groups |noise| gave.  The model left
// behind is of independent captures of one common own variance, so that
// pressing_weights() also weights any subset of them equally.  The model's own
// σ will not do: with the wrong pressings it charges captures whose noise is
// shared with others with their own noise only, which on Community South
// set the rejection distance at under twice the real spread and threw away
// a tenth of the samples of the captures with independent noise.
std::vector<std::optional<PressingNoise>> equal_weight_noise(
    size_t n, const std::vector<std::optional<double>>& pair_variance,
    const std::vector<std::optional<PressingNoise>>& noise);

}  // namespace orc
