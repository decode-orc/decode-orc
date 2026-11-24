/*
 * File:        edge_ring.h
 * Module:      orc-stage-plugin-edge-ring-correct
 * Purpose:     The edge ring of PAL LaserDisc pictures, measured and removed
 *              on the composite field: sharp steps and thin dark lines found
 *              on the field's luma, and a template subtracted at each
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_EDGE_RING_CORRECT_EDGE_RING_H
#define ORC_STAGE_EDGE_RING_CORRECT_EDGE_RING_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace orc::edge_ring {

// Templates are tabulated every 1/kUp sample: a step's from kPre samples
// before its 50 % crossing to kTail after, a thin line's from its centre to
// kTail after.
inline constexpr int kUp = 4;
inline constexpr int kTail = 20;
inline constexpr int kPre = 9;
inline constexpr int kStepBins = (kTail + kPre) * kUp + 1;
inline constexpr int kLineBins = kTail * kUp + 1;

// The picture searched: field lines [kFirstRow, kLastRow) and samples
// [kFirstSample, kLastSample) of a 1135-sample PAL line.
inline constexpr int kFirstRow = 24;
inline constexpr int kLastRow = 308;
inline constexpr int kFirstSample = 190;
inline constexpr int kLastSample = 1100;
inline constexpr int kSamplesPerLine = 1135;

// Features: steps of at least 25 IRE; thin dark lines at least 15 IRE deep
// on ground of at least 45 IRE (60 IRE for measurement).
inline constexpr double kMinStep = 0.25;
inline constexpr double kMinLineDepth = 0.15;
inline constexpr double kMinLineGround = 0.45;
inline constexpr double kMinMeasuredLineGround = 0.6;
// Subcarrier amplitude (5 IRE) in both of a feature's far grounds below which
// the composite line is taken as luma there.
inline constexpr double kMaxGroundChroma = 0.05;

using StepTable = std::array<double, kStepBins>;
using LineTable = std::array<double, kLineBins>;

// One field, normalised so that black is 0 and white 1.
struct FieldImage {
  int rows = 0;
  int width = 0;
  std::vector<double> v;
  double* row(int r) { return v.data() + static_cast<size_t>(r) * width; }
  const double* row(int r) const {
    return v.data() + static_cast<size_t>(r) * width;
  }
};

// The field of |line_count| lines starting at frame line |first_line| of a
// frame of |samples_per_line| samples a line.
FieldImage field_image(const int16_t* frame, size_t samples_per_line,
                       size_t first_line, size_t line_count, double black,
                       double white);

// A step (size = signed step S, level = level before it) or a thin dark line
// (size = depth D, level = ground). mode 0: located on the composite line
// itself (no chroma around it); 1: on the vertical [1 2 1] comb.
struct Feature {
  int row = 0;
  double position = 0.0;
  double level = 0.0;
  double size = 0.0;
  int mode = 0;
};

struct Features {
  std::vector<Feature> steps;
  std::vector<Feature> lines;
};

// strict: the measurement features (flat grounds both sides, vertically
// aligned where combed); otherwise the correction features.
Features find_features(const FieldImage& field, bool strict);

// Binned excess of the measurement features' profiles, summed: steps
// (rising, falling) against the ideal step, lines as the asymmetry of their
// profile (after minus before).
struct ProfileSums {
  StepTable rise_sum{}, rise_count{}, fall_sum{}, fall_count{};
  LineTable line_sum{}, line_count{};
  int rises = 0, falls = 0, lines = 0;
  ProfileSums& operator+=(const ProfileSums& other);
};

ProfileSums measure(const FieldImage& field, const Features& measured);

struct Templates {
  StepTable rise{};
  StepTable fall{};
  LineTable line{};
};

struct Gains {
  double rise = 0.0;
  double fall = 0.0;
  double line = 0.0;
  bool any() const { return rise != 0.0 || fall != 0.0 || line != 0.0; }
};

// out = field with |gains| times the templates subtracted at the correction
// features (out must be a copy of field).
void apply(const FieldImage& field, const Features& correction,
           const Templates& templates, const Gains& gains, FieldImage& out);

// One window's correction. adapt: the gains are scaled per frame by the
// frame's own measured excess (frame_gains).
struct WindowCorrection {
  int64_t first_frame = 0;
  int64_t end_frame = 0;
  bool adapt = false;
  Gains gains;
  Templates templates;
};

// A frame is line-art (maps, line drawings) with at least this many measured
// thin lines over its two fields. Only line-art frames get the line term:
// elsewhere the thin dark features' asymmetry is the picture's own.
inline constexpr int kLineArtLines = 100;
// The line term needs this many line-art frames to be fitted and certified.
inline constexpr int kMinLineArtFrames = 8;

// The frame's gains: per component, the least-squares scale of the frame's
// measured excess onto the template, clipped to [0, 2] (lines [0, 1], and 0
// unless |line_art|), times the window's gain; with too few features, the
// window's gain (lines: none).
Gains frame_gains(const WindowCorrection& window, const ProfileSums& frame,
                  bool line_art);

// Templates from the fit frames' sums; a component needs 30 measured steps
// of its direction (40 lines). |line_art_sums| are the sums over the
// line-art frames only, of which there are |line_art_frames|: the line
// template is fitted on them, and only with kMinLineArtFrames.
// std::nullopt when no component has enough.
std::optional<WindowCorrection> fit(const ProfileSums& sums,
                                    const ProfileSums& line_art_sums,
                                    int line_art_frames);

// A whole frame corrected: both fields, gains per frame when adapting.
// Returns false (frame untouched) when nothing was corrected.
bool correct_frame(const int16_t* input, int16_t* output,
                   size_t samples_per_line, size_t height, double black,
                   double white, const WindowCorrection& window);

}  // namespace orc::edge_ring

#endif  // ORC_STAGE_EDGE_RING_CORRECT_EDGE_RING_H
