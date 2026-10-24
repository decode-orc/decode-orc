/*
 * File:        frame_content.h
 * Module:      decode-orc Plugin SDK (support)
 * Purpose:     Measurements of what a frame shows: block signatures of each
 *              field, same-picture, sample and colour-phase tests between
 *              frames, and the noise they are judged against
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/stage/common_types.h>
#include <orc/stage/dropout/dropout_run.h>
#include <orc/support/stack_measurement.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace orc {

// Every decision here is made from two frames alone plus their noise, never
// by chaining frames into runs, so results do not depend on the order frames
// are visited.
//
// Frames are flat CVBS_U10_4FSC frames as VideoFrameRepresentation delivers
// them: field 1's lines, then field 2's.  For a Y/C source pass the luma
// plane to build_field_signatures and the chroma plane to
// build_chroma_signature.

// ── Geometry ────────────────────────────────────────────────────────────────

// Frames between repeats of the same subcarrier phase: 4 for 625-line PAL and
// PAL-M, whose phase advances a quarter cycle a frame, and 2 for NTSC, whose
// phase inverts every frame.
//   ITU-R BT.1700 Annex 1 Part B: 283.7516 cycles a line, 625 lines a frame.
//   SMPTE 170M-2004 Section 7: 227.5 cycles a line, 525 lines a frame.
size_t colour_frame_cadence(VideoSystem system);

// The levels same_picture measures a block's level against, to tell a gain
// drift or a fade from an offset.
struct PictureLevels {
  int32_t blanking = 0;  // 0 IRE
  int32_t white = 0;     // 100 IRE
};

// The CVBS_U10_4FSC levels of |system| (cvbs_signal_constants.h).
PictureLevels standard_picture_levels(VideoSystem system);

// ── Field signature ─────────────────────────────────────────────────────────
//
// Luma is low-passed by a 4-sample box, which nulls the 4FSC subcarrier, and
// averaged into a grid of blocks over the active picture of each field.
// Box positions touching a flagged sample are left out of their block.

inline constexpr size_t kSignatureBlocksX = 32;
inline constexpr size_t kSignatureBlocksY = 18;
inline constexpr size_t kSignatureBlocks =
    kSignatureBlocksX * kSignatureBlocksY;

// Lines of a block whose means are kept: a row is 16 field lines in PAL and
// 13 in NTSC.
inline constexpr size_t kMaxBlockLines = 24;

// A block with any flagged sample is not judged: leaving samples out of one
// frame's block but not the other's would compare different areas, and on
// picture with structure inside the block that moves its mean by far more
// than the noise.

struct FieldSignature {
  // Mean of the box-filtered luma in each block, in codes, row by row.
  std::vector<float> mean;
  // Standard deviation of the block mean for white sample noise of σ = 1,
  // from the box positions the block used; 0 when it used none.
  std::vector<float> noise_gain;
  // Fraction of the block's box positions that read a flagged sample.
  std::vector<float> dropout_fraction;
  // Mean of each of a block's lines, kMaxBlockLines a block, of which the
  // first row_lines[row] are used.
  std::vector<float> line_mean;
  std::vector<uint8_t> row_lines;
  // Levels the signature was built with, in codes.
  float blanking = 0.0F;
  float white = 0.0F;

  bool judged(size_t block) const {
    return noise_gain[block] > 0.0F && dropout_fraction[block] == 0.0F;
  }
};

// Signatures of both fields of |frame|.  O(active samples).
std::array<FieldSignature, 2> build_field_signatures(
    const std::vector<StackSample>& frame, VideoSystem system,
    size_t nominal_width, size_t height,
    const std::vector<DropoutRun>& dropouts, PictureLevels levels);

// ── Same picture ────────────────────────────────────────────────────────────
//
// Repeats of a still are not identical apart from noise.  Their level drifts
// slowly from frame to frame, by a gain that changes down the field: on
// Volcanoes side 1 the top of the picture moves by up to 4 % within four
// frames while the black border stays put.  The comparison therefore models a
// smooth change first, and judges what is left:
//   1. Each row of blocks is fitted, robustly, with an offset plus a gain on
//      the block's level above blanking.
//   2. The residual's robust spread is the pair's noise.  A pair whose noise
//      is more than kSamePictureNoiseRatio times the source's reference noise
//      has changed everywhere (a cut, a pan) and is different.
//   3. Every judged block's residual must be within kSamePictureBlockLimit
//      times the block's expected noise, which catches a local change (a
//      cursor, a caption).  A block over the limit is excused when the
//      excess lies on at most 3 of its lines: dropouts the decoder did not
//      flag are short and cover a line or three, while a real change covers
//      most of the block's lines.
//   4. The median relative level change, over blocks well above blanking,
//      must be within kSamePictureLevelChange, which catches a fade: drift
//      moves only some rows, a fade moves all of them.

// Limits measured on Volcanoes side 1 (docs-tech/still-frame-stacking-plan.md,
// Test material).
inline constexpr double kSamePictureBlockLimit = 5.5;
inline constexpr double kSamePictureNoiseRatio = 1.5;
inline constexpr double kSamePictureLevelChange = 0.01;

// Fewer than this fraction of the blocks judged gives no decision.
inline constexpr double kMinJudgedBlockFraction = 0.5;

enum class PictureMatch { Same, Different, Undecided };

struct FieldComparison {
  PictureMatch match = PictureMatch::Undecided;
  size_t judged_blocks = 0;
  // Judged blocks whose residual is over the block limit.
  size_t failed_blocks = 0;
  // Blocks over the limit only because of a disturbance on at most 3 of
  // their lines, such as an unflagged dropout; these do not fail the pair.
  size_t disturbed_blocks = 0;
  // Sample noise, in codes, from this pair's residuals.
  double pair_sigma = 0.0;
  // Largest residual, in multiples of that block's expected noise.
  double worst_block = 0.0;
  // Median relative change of the picture level above blanking.
  double level_change = 0.0;
};

struct SamePictureLimits {
  double block = kSamePictureBlockLimit;
  double noise_ratio = kSamePictureNoiseRatio;
  double level_change = kSamePictureLevelChange;
};

// Compare one field of two frames of a source whose reference noise is
// |sigma| codes, as BlockNoiseMeter measures it over matched pairs.  The
// block limit applies to the larger of |sigma| and the pair's own noise.
// O(blocks).
FieldComparison same_picture(const FieldSignature& a, const FieldSignature& b,
                             double sigma,
                             const SamePictureLimits& limits = {});

// ── Sample check ────────────────────────────────────────────────────────────
//
// The block means miss changes that leave a block's mean nearly where it
// was: a thin outline moved a few samples (limited animation, where a
// character moves over a held background) and a fade of small bright text on
// black, where too few blocks are bright enough for the level test.  So a
// pair the signatures accept is also compared sample by sample, per field:
//   1. The luma of both frames is low-passed by the 4-sample box and
//      differenced.  The difference is cut into blocks of
//      kSampleCheckBlockWidth samples by kSampleCheckBlockLines lines, and
//      each block's mean is removed, which takes out level drift.  The
//      kSampleCheckExcusedLines lines of a block furthest from its mean are
//      left out, as an unflagged dropout covers a line or three.
//   2. Each block's residual energy is judged against the median block
//      energy of the field pair, which is the pair's own noise.  A field
//      fails when any block is over kSampleCheckBlockLimit times it.
//   3. A field also fails as a fade when the difference follows the
//      picture's detail by a gain over kSampleCheckFadeGain, measured over
//      every judged block, and more than kSampleCheckRaisedFraction of the
//      blocks are over kSampleCheckRaisedLimit: the gain is then visible in
//      the residual and not only a drift on smooth picture.
// The outermost block columns are not judged: the edge between picture and
// blanking jitters from frame to frame.  Blocks with a flagged sample in
// either frame are not judged.
//
// Limits measured on Volcanoes side 1, the AIV Domesday ComN, NatA and NatB
// discs and cc_test (docs-tech/still-frame-stacking-plan.md, Test material).

inline constexpr size_t kSampleCheckBlockWidth = 16;
inline constexpr size_t kSampleCheckBlockLines = 8;
inline constexpr size_t kSampleCheckExcusedLines = 3;
inline constexpr double kSampleCheckBlockLimit = 5.0;
inline constexpr double kSampleCheckRaisedLimit = 2.5;
inline constexpr double kSampleCheckRaisedFraction = 0.01;
inline constexpr double kSampleCheckFadeGain = 0.004;

struct SampleCheckLimits {
  double block = kSampleCheckBlockLimit;
  double raised = kSampleCheckRaisedLimit;
  double raised_fraction = kSampleCheckRaisedFraction;
  double fade_gain = kSampleCheckFadeGain;
};

struct SampleComparison {
  // Undecided when fewer than kMinJudgedBlockFraction of the blocks are
  // judged.
  PictureMatch match = PictureMatch::Undecided;
  size_t blocks = 0;
  size_t judged_blocks = 0;
  // Judged blocks over the block limit.
  size_t failed_blocks = 0;
  // Median block residual energy, in codes² of the box-filtered luma.
  double median_energy = 0.0;
  // Largest block energy, in multiples of the median.
  double worst_block = 0.0;
  // Fraction of the judged blocks over the raised limit.
  double raised_fraction = 0.0;
  // Gain of the difference (b − a) on the picture's detail: −0.01 is b's
  // detail 1 % lower in contrast than a's.
  double detail_gain = 0.0;
};

// Compare field |field| (0 or 1) of two frames sample by sample, each with
// its own dropout runs.  For a Y/C source pass the luma planes.  The result
// is the same with the frames swapped, apart from the sign of detail_gain.
// O(active samples of the field).
SampleComparison compare_field_samples(
    const std::vector<StackSample>& a,
    const std::vector<DropoutRun>& a_dropouts,
    const std::vector<StackSample>& b,
    const std::vector<DropoutRun>& b_dropouts, size_t field, VideoSystem system,
    size_t nominal_width, size_t height, const SampleCheckLimits& limits = {});

// ── Colour phase ────────────────────────────────────────────────────────────
//
// A repeat can be stacked into a frame's composite only if its subcarrier
// phase is the same.  Phase is measured, not read from colour_frame_index,
// so a Frame Map gap or a one-sample grid slip is caught.
//
// The chroma band is taken by a horizontal band-pass centred on the
// subcarrier, (2x[n] − x[n−2] − x[n+2]) / 4, followed by a same-field
// vertical comb against the line whose subcarrier is inverted (two lines on
// in PAL and PAL-M, the next line in NTSC), which removes the luma detail
// near the subcarrier that would otherwise correlate at any phase.  The two
// frames' bands are correlated at lag 0 and compared with the correlation
// they would have at the same phase, which is read from each frame's own
// band at a lag of one subcarrier cycle (4 samples): chroma repeats there,
// noise does not.  So the decision is relative to the chroma level, and
// needs no noise model.
//
// The test assumes the frames already show the same picture: two different
// pictures read as quadrature.

// Below this expected correlation the chroma is too weak to decide.
inline constexpr double kMinChromaCorrelation = 0.2;

// Correlation, as a fraction of the expected, that reads as the same
// (or, negated, opposite) phase, and below which it reads as quadrature.
inline constexpr double kPhaseAgreementFraction = 0.6;
inline constexpr double kPhaseQuadratureFraction = 0.3;

struct ChromaSignature {
  // Chroma band on a decimated grid of active lines; positions_per_line
  // samples a line.
  std::vector<float> band;
  // 1 where no sample the band was taken from is flagged.
  std::vector<uint8_t> valid;
  size_t positions_per_line = 0;
  // The band's correlation with itself one subcarrier cycle along the line.
  double self_correlation = 0.0;
};

// The chroma band of |frame| (the composite frame, or the chroma plane of a
// Y/C source).  O(active samples / line step).
ChromaSignature build_chroma_signature(const std::vector<StackSample>& frame,
                                       VideoSystem system, size_t nominal_width,
                                       size_t height,
                                       const std::vector<DropoutRun>& dropouts);

enum class ColourPhaseRelation { Same, Opposite, Quadrature, Unknown };

struct ColourPhaseMeasure {
  ColourPhaseRelation relation = ColourPhaseRelation::Unknown;
  // Correlation of the two bands at lag 0.
  double correlation = 0.0;
  // Correlation expected at the same phase.
  double expected = 0.0;
};

// Phase of |b| relative to |a|.  O(band positions).
ColourPhaseMeasure colour_phase_relation(const ChromaSignature& a,
                                         const ChromaSignature& b);

// ── Duplicate reads ─────────────────────────────────────────────────────────

// True when the two frames' samples are bit-identical: the same physical read
// of the disc (a Frame Map duplicate), never an independent repeat.
bool is_duplicate_read(const std::vector<StackSample>& a,
                       const std::vector<StackSample>& b);

// Hash of a frame's samples (FNV-1a over 64-bit words, in four lanes), to find
// duplicate-read candidates without keeping every frame.  Hashes are for
// comparing within one run, not for storing.  O(samples).
uint64_t frame_sample_hash(const std::vector<StackSample>& frame);

// ── Noise ───────────────────────────────────────────────────────────────────

// Reference sample noise, in codes, from field pairs already found to show
// the same picture: the robust spread of their residuals, as same_picture
// takes them, scaled by each block's noise gain.  Both frames of a pair are
// taken to have the same noise.
class BlockNoiseMeter {
 public:
  void add(const FieldSignature& a, const FieldSignature& b);

  size_t blocks() const { return scaled_.size(); }

  // nullopt with fewer than kMinNoiseBlocks blocks.
  std::optional<double> sigma() const;

  static constexpr size_t kMinNoiseBlocks = 100;

 private:
  std::vector<double> scaled_;
};

// Sample noise, in codes, of one frame alone: the box-filtered luma against
// itself 8 samples (two subcarrier cycles) along the line, read at a low
// percentile so that picture detail does not inflate it.  The NoiseReducer's
// within-frame anchor uses the same operator.  nullopt when the frame has no
// active picture.
//
// This reads the noise across the whole band, which on LaserDisc rises with
// frequency, so it is larger than the reference same_picture takes: on
// Volcanoes side 1 it reads 11.4 codes on black where matched pairs give 4.6,
// and detail inflates it further.  It is a starting bound for finding the
// first matches, not a reference to judge them by.
std::optional<double> within_frame_noise(
    const std::vector<StackSample>& frame, VideoSystem system,
    size_t nominal_width, size_t height,
    const std::vector<DropoutRun>& dropouts);

}  // namespace orc
