/*
 * File:        edge_ring_certify.h
 * Module:      orc-stage-plugin-edge-ring-correct
 * Purpose:     Held-out certification of an edge-ring correction: measures
 *              on frames the fit did not see, and a paired bootstrap bound
 *              against leaving the frames uncorrected
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_EDGE_RING_CORRECT_EDGE_RING_CERTIFY_H
#define ORC_STAGE_EDGE_RING_CORRECT_EDGE_RING_CERTIFY_H

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "edge_ring.h"

namespace orc::edge_ring {

// A field read for certification: its picture, its correction and
// measurement features, and its measures uncorrected.
struct FlatStats {
  double noise_sum = 0.0;  // squared high-pass luma over flat areas
  double noise_count = 0.0;
  double level_sum = 0.0;  // luma change over large flat areas
  double level_count = 0.0;
};

class CertifyField {
 public:
  explicit CertifyField(FieldImage field);
  const ProfileSums& before() const { return before_; }
  const FlatStats& flat_before() const { return flat_before_; }
  // The measures with |window| applied at |gains|, its frame line-art or not.
  std::pair<ProfileSums, FlatStats> after(const WindowCorrection& window,
                                          const Gains& gains,
                                          bool line_art) const;

 private:
  FieldImage field_;
  std::vector<Event> events_;
  Features measured_;
  ProfileSums before_;
  std::vector<uint8_t> flat_, large_flat_;
  FlatStats flat_before_;
  FieldImage luma_before_;
  FlatStats flat_stats(const FieldImage& luma) const;
};

// Measures, per 1000 of the step (or line depth): the RMS of the rising and
// falling steps' excess at +2..+12 samples (post) and -8..-2 (pre) and its
// mean at +3..+8 (tail); the RMS of the thin lines' asymmetry at +2..+10
// (halo) and its value at +3 (halo3); and over flat areas the luma noise
// (dB) and level change. Each is the count-weighted mean over frames of the
// frame's own measure, so content mixes do not cancel.
// (rise: isolated rising steps; fill: fill-edge rising steps.)
inline constexpr std::array<const char*, 11> kEdgeMeasures = {
    "rise_post", "rise_pre", "rise_tail", "fill_post", "fill_pre", "fill_tail",
    "fall_post", "fall_pre", "fall_tail", "halo",      "halo3"};

struct MeasureComparison {
  double before = 0.0;
  double after = 0.0;
  double low = 0.0;   // 2.5 % of the bootstrapped (after - before)
  double high = 0.0;  // 97.5 %
  bool measured = false;
};

struct Comparison {
  std::map<std::string, MeasureComparison> measures;
  int rises = 0, fills = 0, falls = 0, lines = 0;
  int line_art_frames = 0;
  // The bootstrapped 97.5 % bound of the summed edge measures' change.
  double gain_bound = 0.0;
  // The share of frames (of those with any edge measure) on which the summed
  // edge measures improved.
  double frame_share = 0.0;
};

// Paired comparison over |fields| (frame pairs: fields 2k and 2k+1) of the
// window applied (null: identity) against identity. When the window adapts,
// each field's gains come from the OTHER field of its frame, so no field is
// measured with a gain estimated on itself.
Comparison compare(const std::vector<CertifyField>& fields,
                   const WindowCorrection* window, uint64_t seed);

struct Certificate {
  bool accepted = false;
  std::vector<std::string> reasons;
  Comparison comparison;
};

// Bands: 0.1 % of the step on every edge measure (all eight), 0.05 dB on the
// noise, 0.1 IRE on the level; and a certain gain of at least 0.5 % of the step
// on the summed edge measures (smaller gains are within the measures' own
// interpolation bias on clean pictures).
inline constexpr double kEdgeBand = 1.0;
inline constexpr double kNoiseBandDb = 0.05;
inline constexpr double kLevelBand = 0.001;
// ... and the summed edge measures must improve on at least this share of
// the certifying frames (a gain carried by a few frames is not certain).
inline constexpr double kMinFrameShare = 0.6;
inline constexpr double kMinGain = 5.0;
inline constexpr int kBootstrapDraws = 300;

Certificate certify(const std::vector<CertifyField>& fields,
                    const WindowCorrection& window);

// The candidates tried in order (the first certified is taken): per-frame
// gains then fixed gains, every non-empty subset of the fitted components,
// largest first, at full and then half gain.
std::vector<WindowCorrection> candidates(const WindowCorrection& fitted);

}  // namespace orc::edge_ring

#endif  // ORC_STAGE_EDGE_RING_CORRECT_EDGE_RING_CERTIFY_H
