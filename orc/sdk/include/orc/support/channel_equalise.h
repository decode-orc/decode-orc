/*
 * File:        channel_equalise.h
 * Module:      decode-orc Plugin SDK (support)
 * Purpose:     The inverse of the PAL LaserVision CAV edge smear, and the
 *              burst-referenced chroma gain that must follow it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/stage/common_types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// PAL LaserVision CAV discs carry an edge smear from mastering: a one-pole
// tail after every step,
//   H(z) = (1 - c) + c (1 - a) / (1 - a z^-1),  a = exp(-1 / tau),
// c the share of the step in the tail and tau its time constant in 4fsc
// samples (Domesday CAV sides: c 0.24, tau 6.1). Its inverse is the IIR
//   1 / H(z) = (1 - a z^-1) / ((1 - c a) - (1 - c) a z^-1),
// whose pole lies inside the unit circle for 0 <= c < 1. The inverse lifts
// the subcarrier back by what the smear took (2.2 dB at 0.24 / 6.1), so a
// chroma gain referenced to the colour burst follows it.

namespace orc::channel_equalise {

// The nominal burst amplitude, in units of white - blanking: a clean
// videosynth PAL signal and a capture of Domesday's Community South DS1
// agree on it to 0.4 %.
inline constexpr double kNominalBurstAmplitude = 0.1702;

// The parameter range a measured smear may take. A tail under two samples
// cannot be told from a generator's own edge shaping; a fraction near 0.5
// is the fit degenerating on a signal without a smear.
inline constexpr double kMaxFraction = 0.45;
inline constexpr double kMinTauSamples = 2.0;
inline constexpr double kMaxTauSamples = 50.0;

// A second, fast component of the smear, for the part of the tail within a
// few samples of an edge:
//   H(z) = 1 + c (L_tau(z) - 1) + f (L_fast(z) - 1),
// L_t(z) = (1 - a) / (1 - a z^-1), a = exp(-1 / t). Its fraction is signed:
// negative where the tail starts more slowly than the slow component alone
// would (the Domesday sides), positive for a tail confined near the edge.
inline constexpr double kMinFastFraction = -0.45;
inline constexpr double kMaxFastFraction = 0.3;
inline constexpr double kMinFastTauSamples = 0.5;
inline constexpr double kMaxFastTauSamples = 4.5;

struct EdgeSmear {
  double fraction = 0.0;
  double tau_samples = 1.0;
  // The fast component; 0 leaves the one-pole smear above.
  double fast_fraction = 0.0;
  double fast_tau_samples = 1.0;

  bool active() const { return fraction > 0.0 || fast_fraction != 0.0; }
  bool operator==(const EdgeSmear& o) const {
    return fraction == o.fraction && tau_samples == o.tau_samples &&
           fast_fraction == o.fast_fraction &&
           fast_tau_samples == o.fast_tau_samples;
  }
  bool operator!=(const EdgeSmear& o) const { return !(*this == o); }
};

// Whether the smear's inverse is stable with a margin (its poles within
// 0.98 of the origin). A one-pole smear within its range always is.
bool invertible(const EdgeSmear& smear);

// The smear's inverse over one contiguous stream, in place, its state primed
// to the first sample's level. Without a fast component this is the
// one-pole inverse; with one, the second-order inverse
//   1 / H(z) = D(z) / N(z),  D = (1 - a z^-1)(1 - b z^-1),
//   N = (1 - c - f) D + c (1 - a)(1 - b z^-1) + f (1 - b)(1 - a z^-1),
// a and b the slow and fast poles. A smear that is not invertible() leaves
// the stream as it is.
void unsmear(std::vector<double>& stream, const EdgeSmear& smear);

// The median burst amplitude of a PAL frame-flat buffer over the picture
// lines of both fields (field lines 23-310), in units of white - blanking.
// std::nullopt when the buffer is not a whole PAL frame.
std::optional<double> burst_amplitude(const std::vector<double>& frame,
                                      size_t samples_per_line,
                                      double blanking_level,
                                      double white_level);

// The furthest the chroma gain may move a frame's burst, either way. A real
// frame's burst, through the smear inverse, needs a gain between about 0.5
// and 1.5. One further out is no burst to go by: a stacked frame whose inputs
// carry one field in antiphase (a player hunting on BBC City Disc,
// Financial 2) cancels that field's burst and chroma, and its median burst
// read 0.014 of white. Scaled to the nominal, the shelf's gain of about 12
// swung the samples from -3000 to 4700 codes and turned the picture number
// on lines 17 and 18 into another valid one.
inline constexpr double kMaxBurstGain = 3.0;

// The gain that brings |amplitude| to the nominal burst, or 1 when that
// gain would exceed kMaxBurstGain either way.
double burst_gain(double amplitude);

// Scales each line's subcarrier band by |gain|: x + (gain - 1) h * x, h the
// 5-tap (1 - cos wT)^2 shelf [1, -4, 6, -4, 1] / 4 (0 at DC, 1 at fsc), the
// line's end samples extended.
void apply_chroma_gain(std::vector<double>& frame, size_t samples_per_line,
                       double gain);

// What equalise_frame() did to a frame.
struct FrameResult {
  std::vector<int16_t> frame;
  // The gain applied; 1 when the burst could not be measured or lay too far
  // from the nominal to go by (see kMaxBurstGain).
  double gain = 1.0;
};

// A PAL composite frame (frame-flat, |samples_per_line| nominal) through the
// inverse smear, in transmission order, then the burst-referenced chroma
// gain. The caller passes the frame through unchanged when |smear| is not
// active.
FrameResult equalise_frame(const int16_t* frame, size_t samples_total,
                           size_t samples_per_line, double blanking_level,
                           double white_level, const EdgeSmear& smear);

// The same with a smear for each field: the first field's lines (frame-flat
// 0-312) through |first_field|'s inverse, the second's through
// |second_field|'s, each primed at its own first sample. Equal smears are
// one pass over the frame, as above.
FrameResult equalise_frame(const int16_t* frame, size_t samples_total,
                           size_t samples_per_line, double blanking_level,
                           double white_level, const EdgeSmear& first_field,
                           const EdgeSmear& second_field);

}  // namespace orc::channel_equalise
