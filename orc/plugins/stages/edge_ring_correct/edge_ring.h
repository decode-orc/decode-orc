/*
 * File:        edge_ring.h
 * Module:      orc-stage-plugin-edge-ring-correct
 * Purpose:     The edge ring of PAL LaserDisc pictures, measured and removed
 *              on the composite field: every sharp transition of the
 *              field's luma is an event, and a template is subtracted at
 *              each
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_EDGE_RING_CORRECT_EDGE_RING_H
#define ORC_STAGE_EDGE_RING_CORRECT_EDGE_RING_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace orc::edge_ring {

// Templates are tabulated every 1/kUp sample: a step's from kPre samples
// before its centre to kTail after (used from +2), a thin line's from its
// rising edge to kTail after (used from kLineStart).
inline constexpr int kUp = 4;
inline constexpr int kTail = 20;
inline constexpr int kPre = 9;
inline constexpr int kStepBins = (kTail + kPre) * kUp + 1;
inline constexpr int kLineBins = kTail * kUp + 1;
inline constexpr double kLineStart = 1.0;

// The picture searched: field lines [kFirstRow, kLastRow) and samples
// [kFirstSample, kLastSample) of a 1135-sample PAL line.
inline constexpr int kFirstRow = 24;
inline constexpr int kLastRow = 308;
inline constexpr int kFirstSample = 190;
inline constexpr int kLastSample = 1100;
inline constexpr int kSamplesPerLine = 1135;

// Events: transitions of the notched luma of at least 12 IRE, their
// increments above 0.3 IRE a sample, at most 6 samples above 40 % of their
// peak increment (sharp). A falling event followed within kPairMax samples
// by a rising one is a thin dark line.
inline constexpr double kMinEvent = 0.12;
inline constexpr double kEventIncrement = 0.003;
inline constexpr int kMaxEventSpan = 6;
inline constexpr double kPairMax = 7.0;

// Measurement: only isolated events (no other event within kIsolatedBefore
// samples before, or kIsolatedAfter after); steps of at least 20 IRE with a
// flat lead-in; thin lines at least 15 IRE deep on the same flat ground of at
// least 45 IRE both sides.
inline constexpr int kIsolatedBefore = 16;
inline constexpr int kIsolatedAfter = 22;
inline constexpr double kMinMeasuredStep = 0.2;
inline constexpr double kMinLineDepth = 0.15;
inline constexpr double kMinLineGround = 0.45;
// Subcarrier amplitude (5 IRE) in both of a feature's far grounds below which
// the composite line is taken as luma there (else the vertical comb, for
// vertically aligned features only).
inline constexpr double kMaxGroundChroma = 0.05;

// A frame is line-art (maps, line drawings, text pages) with at least this
// many thin-line pairs over its two fields making up at least this fraction
// of its events. Only line-art frames get the line term.
inline constexpr int kLineArtPairs = 4000;
inline constexpr double kLineArtFraction = 0.30;
// The line term needs this many line-art frames to be fitted and certified.
inline constexpr int kMinLineArtFrames = 8;

// Templates stop before the next event on their row: kStepGuard samples
// before a step (clear of its measured pre-edge region), kLineGuard before a
// thin line (the strokes of lettering, close lines).
inline constexpr double kStepGuard = 12.0;
inline constexpr double kLineGuard = 1.5;
// A feature with more than kDenseMax events within kDenseAfter samples after
// it (where its template would act) is in dense texture (stipple, hatching)
// and left alone. Texture before it does not matter: the edge of a hatched
// fill onto clean paper is corrected.
inline constexpr int kDenseAfter = 24;
inline constexpr int kDenseMax = 3;

// The rising ring depends on where the edge starts and ends (ld-decode's
// response is level-dependent): rising templates are per start-level and
// end-level bin, and per edge class (0: isolated, flat lead-in; 1: fill edge,
// grey or textured lead-in).
inline constexpr std::array<double, 2> kStartEdges = {0.15, 0.40};
inline constexpr std::array<double, 4> kEndEdges = {0.65, 0.72, 0.79, 0.86};
inline constexpr int kLevelBins =
    static_cast<int>((kStartEdges.size() + 1) * (kEndEdges.size() + 1));
inline constexpr int kRiseClasses = 2;
// A bin's own shape needs this many pooled edges.
inline constexpr int kMinBinEdges = 200;
int level_bin(double start, double end);

using StepTable = std::array<double, kStepBins>;
using LineTable = std::array<double, kLineBins>;

// One field, normalised so that black is 0 and white 1.
struct FieldImage {
  int rows = 0;
  int width = 0;
  std::vector<double> v;
  // dropout-flagged samples (rows x width), empty when there are none: no
  // event is found on them and nothing is measured across them
  std::vector<uint8_t> dropout;
  bool dropped(int r, int a, int b) const;  // any flagged sample in [a, b)
  double* row(int r) { return v.data() + static_cast<size_t>(r) * width; }
  const double* row(int r) const {
    return v.data() + static_cast<size_t>(r) * width;
  }
};

// Marks the dropout runs that fall on the field (its frame lines
// [first_line, first_line + rows)) of a PAL frame.
void mark_dropouts(const std::vector<std::pair<uint64_t, uint32_t>>& runs,
                   size_t samples_per_line, size_t first_line,
                   FieldImage& field);

// The field of |line_count| lines starting at frame line |first_line| of a
// PAL frame of |samples_per_line| samples a line.
FieldImage field_image(const int16_t* frame, size_t samples_per_line,
                       size_t first_line, size_t line_count, double black,
                       double white);

// A sharp transition: its row, increment centroid and total step.
struct Event {
  int row = 0;
  double centre = 0.0;
  double step = 0.0;
};

// Every sharp transition of the field, found on its [1 2 2 2 1]/8 notched
// luma (zeros at the subcarrier and half the sample rate; symmetric with unit
// gain, so a transition's step and centroid are the composite's), so the same
// for vertical, slanted and curved edges, in colour or not. In row order.
std::vector<Event> find_events(const FieldImage& field);

// The number of thin-line pairs among |events|.
int count_pairs(const std::vector<Event>& events);

// A step (position = centre, size = step, level = level before it) or a thin
// dark line (position = midpoint, size = depth, level = ground, first/last =
// its falling and rising events). mode 0: measured on the composite line
// itself; 1: on the vertical [1 2 1] comb.
struct Feature {
  int row = 0;
  double position = 0.0;
  double level = 0.0;
  double size = 0.0;
  int mode = 0;
  double first = 0.0;
  double last = 0.0;
  double event_step = 0.0;
  // rising steps: edge class and level bin; every correction feature: the
  // last sample its template may change (+infinity: none)
  int edge_class = 0;
  int bin = 0;
  double limit = 1e18;
};

struct Features {
  std::vector<Feature> steps;
  std::vector<Feature> lines;
};

// What the correction does with thin-line pairs: the line template; both
// step templates; or nothing (a pair never gets one step template without
// the other).
enum class PairMode { Lines, Untouched };

// The correction features of |field|: every event outside dense texture,
// split by |mode|, with each template's stop and each rising step's class
// and level bin.
Features correction_features(const FieldImage& field,
                             const std::vector<Event>& events, PairMode mode);

// The measurement features: isolated events, steps measured against their
// plateau +8..+20 (event_step keeps the event's own step), lines against
// their ground.
Features measured_features(const FieldImage& field,
                           const std::vector<Event>& events);

// Binned excess of the measured features' profiles, summed: steps (rising,
// falling) against the ideal step; lines as the asymmetry of the side after
// their rising edge against the side before their falling edge. With the
// steps' plateau-to-event step ratios (kappa) and the field's event counts.
using LevelTables = std::array<std::array<StepTable, kLevelBins>, kRiseClasses>;

struct ProfileSums {
  // rise: isolated rising steps; fill: fill-edge rising steps
  StepTable rise_sum{}, rise_count{}, fill_sum{}, fill_count{};
  StepTable fall_sum{}, fall_count{};
  LineTable line_sum{}, line_count{};
  int rises = 0, fills = 0, falls = 0, lines = 0;
  int events = 0, pairs = 0;
  std::vector<double> rise_kappa, fall_kappa;
  // rising excess per class and level bin, for the pooled level templates
  LevelTables level_sum{}, level_count{};
  ProfileSums& operator+=(const ProfileSums& other);
};

ProfileSums measure(const FieldImage& field, const Features& measured);

// Whether a frame whose two fields' sums are |frame| is line-art.
bool is_line_art(const ProfileSums& frame);

struct Templates {
  StepTable rise{};
  StepTable fall{};
  LineTable line{};
};

struct Gains {
  double rise = 0.0;  // isolated rising steps
  double fall = 0.0;
  double line = 0.0;
  double fill = 0.0;  // fill-edge rising steps
  bool any() const {
    return rise != 0.0 || fall != 0.0 || line != 0.0 || fill != 0.0;
  }
};

// Rising templates per class and level bin, pooled over a source's fit
// frames; a sparse bin takes its class's all-level template, a sparse class
// both classes'.
LevelTables level_templates(const ProfileSums& pooled);

// One window's correction. adapt: the gains are scaled per frame by the
// frame's own measured excess (frame_gains). kappa: the full step over the
// event's own step, per direction, so the correction scales each event's step
// to the measurement's definition.
struct WindowCorrection {
  int64_t first_frame = 0;
  int64_t end_frame = 0;
  bool adapt = false;
  Gains gains;
  double kappa_rise = 1.0;
  double kappa_fall = 1.0;
  Templates templates;  // rise unused: rising steps take |levels|
  std::shared_ptr<const LevelTables> levels;
};

PairMode pair_mode(bool line_art, const Gains& gains);

// out = field with the templates subtracted at the correction features
// (out must be a copy of field). Rising steps take the window's level
// templates.
void apply(const Features& correction, const WindowCorrection& window,
           const Gains& gains, FieldImage& out);

// The frame's gains: falls and lines scaled by the least-squares projection
// of the frame's measured excess onto the template, clipped to [0, 2] (lines
// [0, 1], and 0 unless |line_art|), times the window's gain; with too few
// features, the window's gain (lines: none). Rising steps keep the window's
// gains.
Gains frame_gains(const WindowCorrection& window, const ProfileSums& frame,
                  bool line_art);

// Templates from a window's fit frames: |sums| over all of them,
// |line_art_sums| over the |line_art_frames| line-art ones (the line
// template, which needs kMinLineArtFrames); rising steps take |levels|. A
// component needs 30 measured features. std::nullopt when none has enough.
std::optional<WindowCorrection> fit(const ProfileSums& sums,
                                    const ProfileSums& line_art_sums,
                                    int line_art_frames,
                                    std::shared_ptr<const LevelTables> levels);

// A whole frame corrected: both fields, gains per frame when adapting.
// Returns false (frame untouched) when nothing was corrected.
// |dropouts|: the frame's dropout runs (flat sample offset, length).
bool correct_frame(
    const int16_t* input, int16_t* output, size_t samples_per_line,
    size_t height, double black, double white, const WindowCorrection& window,
    const std::vector<std::pair<uint64_t, uint32_t>>& dropouts = {});

}  // namespace orc::edge_ring

#endif  // ORC_STAGE_EDGE_RING_CORRECT_EDGE_RING_H
