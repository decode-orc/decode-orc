/*
 * File:        window_select.h
 * Module:      orc-stage-plugin-channel-equalise
 * Purpose:     Choosing, for a window of frames, a correction that is never
 *              worse than the upstream per-source one
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_CHANNEL_EQUALISE_WINDOW_SELECT_H
#define ORC_STAGE_CHANNEL_EQUALISE_WINDOW_SELECT_H

#include <orc/support/channel_equalise.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "picture_edges.h"

namespace orc::window_select {

// A correction: one smear per field parity (an inactive one leaves that
// field as it is).
using Correction = std::array<channel_equalise::EdgeSmear, 2>;

// A correction that may change from frame to frame: a track as the stage
// applies it.
using CorrectionAt = std::function<Correction(int64_t frame)>;

// What one split of a window's frames holds: each parity's aligned line-19
// segments (as aligned_bar_segment(), the fields a robust average would
// keep) and the picture edges of some of its frames. The frame of each
// segment and of each edge is needed only to apply a CorrectionAt.
struct Split {
  std::array<std::vector<std::vector<double>>, 2> segments;
  picture_edges::Edges edges;
  std::array<std::vector<int64_t>, 2> segment_frames;
  std::vector<int64_t> edge_frames;
};

// The measures a correction is judged on, each an error (smaller is
// better): per field parity the VITS bar's tails 3-8 and 9-30 samples after
// its rising and its falling edge, and the noise on its flat parts; and the
// picture edges' tails 3-8 and 9-20 samples after them. All in units of the
// step (white - blanking for the noise). A measure is absent when its data
// is (too few fields or edges).
enum Measure : size_t {
  kRise3_8,
  kFall3_8,
  kRise9_30,
  kFall9_30,
  kNoise,
  kPerParity,  // the five above, for parity 0 at 0..4 and parity 1 at 5..9
  kPicture3_8 = 2 * kPerParity,
  kPicture9_20,
  kMeasures
};
struct Measures {
  std::array<double, kMeasures> value{};
  std::array<bool, kMeasures> present{};
};
bool is_noise(size_t measure);

// The measures of |correction| on |split|.
Measures measure(const Split& split, const Correction& correction);
// The same with each field and edge corrected as its frame is (the split's
// frames must be given); every measure absent when they are not.
Measures measure(const Split& split, const CorrectionAt& correction);

// Whether |candidate| is certified on |split| against |upstream| and the
// identity: over |resamples| paired bootstrap resamples of the split's
// fields and edges, the 97.5th percentile of (candidate - upstream) is at
// most 0.1 % of the step on every tail and 1 % of upstream's noise on the
// noise, and of (candidate - identity) at most 0.1 % on every tail. A
// measure that upstream's correction (or, for a tail, the identity) yields
// and the candidate does not fails it, in any resample; so does a split on
// which no measure can be compared at all.
bool certified(const Split& split, const Correction& candidate,
               const Correction& upstream, int resamples = 200,
               uint32_t seed = 1);
// The same for a correction that changes from frame to frame (the split's
// frames must be given, or it is not certified). Without |against_identity|
// it is compared with upstream's alone.
bool certified(const Split& split, const CorrectionAt& candidate,
               const Correction& upstream, bool against_identity,
               int resamples = 200, uint32_t seed = 1);

// The correction a constrained search finds on |split|: one two-component
// smear per parity minimising the measures relative to upstream's, with a
// heavy penalty for any measure above upstream's or any tail above the
// identity's. A point lacking a measure that upstream's correction (or, for
// a tail, the identity) yields is infeasible. Nelder-Mead from each of
// |starts|. std::nullopt when no start is feasible.
std::optional<Correction> constrained_search(
    const Split& split, const Correction& upstream,
    const std::vector<Correction>& starts);

struct Choice {
  // "constrained", "tracked" or "upstream".
  std::string name;
  Correction correction;
};

// The window's correction: candidates fitted on |fit| (the tracked
// two-component fit per parity, given as |tracked|, and the constrained
// search from it and from upstream), each tried on |select| in that order;
// the first certified there is chosen, otherwise upstream's.
Choice choose(const Split& fit, const Split& select, const Correction& upstream,
              const std::optional<Correction>& tracked);

}  // namespace orc::window_select

#endif  // ORC_STAGE_CHANNEL_EQUALISE_WINDOW_SELECT_H
