/*
 * File:        smear_track.h
 * Module:      orc-stage-plugin-channel-equalise
 * Purpose:     An edge smear that varies along a source: knots of measured
 *              smear, interpolated per frame
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_CHANNEL_EQUALISE_SMEAR_TRACK_H
#define ORC_STAGE_CHANNEL_EQUALISE_SMEAR_TRACK_H

#include <orc/stage/common_types.h>
#include <orc/support/channel_equalise.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace orc {

// The smear measured at one frame: the slow component and, optionally, the
// fast one (channel_equalise::EdgeSmear). Fractions of 0 are a frame with no
// smear to correct: none was measured there, or nothing could be measured.
struct SmearKnot {
  int64_t frame = 0;
  double fraction = 0.0;
  double tau_samples = channel_equalise::kMinTauSamples;
  double fast_fraction = 0.0;
  double fast_tau_samples = 1.0;

  bool has_smear() const { return fraction > 0.0 || fast_fraction != 0.0; }

  bool operator==(const SmearKnot& other) const {
    return frame == other.frame && fraction == other.fraction &&
           tau_samples == other.tau_samples &&
           fast_fraction == other.fast_fraction &&
           fast_tau_samples == other.fast_tau_samples;
  }
};

// Knots in frame order. A frame between two knots takes the straight line
// between them; one before the first or after the last takes that knot.
class SmearTrack {
 public:
  SmearTrack() = default;

  // std::nullopt when the knots are not in strictly increasing frame order,
  // or a knot's smear is outside the stage's range (a fraction from 0 to
  // kMaxFraction, a time constant from kMinTauSamples to kMaxTauSamples; a
  // fast fraction from kMinFastFraction to kMaxFastFraction, its time
  // constant from kMinFastTauSamples to kMaxFastTauSamples) or not
  // invertible.
  static std::optional<SmearTrack> from_knots(std::vector<SmearKnot> knots);

  // The text form: knots separated by ";", each "frame:fraction:tau", or
  // "frame:fraction:tau:fast_fraction:fast_tau" when it has a fast
  // component; fractions to 4 decimals and time constants to 3. An empty
  // string is an empty track; std::nullopt when the text is malformed or its
  // knots would be refused by from_knots().
  static std::optional<SmearTrack> parse(const std::string& text);
  std::string to_string() const;

  bool empty() const { return knots_.empty(); }
  const std::vector<SmearKnot>& knots() const { return knots_; }

  // The smear at |frame|; an empty track has none.
  channel_equalise::EdgeSmear at(int64_t frame) const;

  // The track with every knot dropped whose smear the straight line between
  // its kept neighbours gives within |fraction_tolerance| and
  // |tau_tolerance| (both components), so a long track stays short. Knots
  // where the smear turns on or off are kept, and so is knot i wherever
  // |pinned|[i] is set. The tolerances bound the knots' parameters only; the
  // Channel Tracking tool certifies the simplified track's effect itself.
  SmearTrack simplified(double fraction_tolerance, double tau_tolerance,
                        const std::vector<bool>& pinned = {}) const;

 private:
  std::vector<SmearKnot> knots_;
};

}  // namespace orc

#endif  // ORC_STAGE_CHANNEL_EQUALISE_SMEAR_TRACK_H
