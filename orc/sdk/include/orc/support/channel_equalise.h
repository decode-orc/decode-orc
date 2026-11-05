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

struct EdgeSmear {
  double fraction = 0.0;
  double tau_samples = 1.0;

  bool active() const { return fraction > 0.0; }
};

// The smear's inverse over one contiguous stream, in place, its state primed
// to the first sample's level.
void unsmear(std::vector<double>& stream, const EdgeSmear& smear);

// The median burst amplitude of a PAL frame-flat buffer over the picture
// lines of both fields (field lines 23-310), in units of white - blanking.
// std::nullopt when the buffer is not a whole PAL frame.
std::optional<double> burst_amplitude(const std::vector<double>& frame,
                                      size_t samples_per_line,
                                      double blanking_level,
                                      double white_level);

// The gain that brings |amplitude| to the nominal burst.
double burst_gain(double amplitude);

// Scales each line's subcarrier band by |gain|: x + (gain - 1) h * x, h the
// 5-tap (1 - cos wT)^2 shelf [1, -4, 6, -4, 1] / 4 (0 at DC, 1 at fsc), the
// line's end samples extended.
void apply_chroma_gain(std::vector<double>& frame, size_t samples_per_line,
                       double gain);

// What equalise_frame() did to a frame.
struct FrameResult {
  std::vector<int16_t> frame;
  // The gain applied; 1 when the burst could not be measured.
  double gain = 1.0;
};

// A PAL composite frame (frame-flat, |samples_per_line| nominal) through the
// inverse smear, in transmission order, then the burst-referenced chroma
// gain. The caller passes the frame through unchanged when |smear| is not
// active.
FrameResult equalise_frame(const int16_t* frame, size_t samples_total,
                           size_t samples_per_line, double blanking_level,
                           double white_level, const EdgeSmear& smear);

}  // namespace orc::channel_equalise
