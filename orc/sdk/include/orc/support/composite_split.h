/*
 * File:        composite_split.h
 * Module:      decode-orc Plugin SDK (support)
 * Purpose:     A local split of one line of 4FSC composite into luma and
 *              baseband chroma, and the chroma's move to another line's
 *              subcarrier phase
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace orc::composite_split {

inline constexpr double kPi = 3.14159265358979323846;

// At 4FSC the subcarrier advances a quarter cycle a sample, so the reference
// e^{j n pi/2} that demodulates and re-modulates it takes four values.
inline constexpr std::array<double, 4> kSubcarrierCos{1.0, 0.0, -1.0, 0.0};
inline constexpr std::array<double, 4> kSubcarrierSin{0.0, 1.0, 0.0, -1.0};

// Bandwidth of the baseband chroma: PAL's U and V, and NTSC's I, reach
// 1.3 MHz either side of the subcarrier. NTSC's Q and PAL-M's are narrower,
// and the wider band only takes a little more luma with them.
inline constexpr double kChromaBandwidthHz = 1.3e6;

// Half-width of the low-pass that keeps the baseband chroma, in samples. A
// split reads this many samples beyond each end of its span.
inline constexpr int32_t kChromaFilterRadius = 16;

// The chroma at sample |n| of a line, from its baseband phasor there:
// Re{phasor e^{j n pi/2}}.
inline double remodulate(std::complex<double> phasor, int32_t n) {
  const auto k = static_cast<size_t>(n & 3);
  return (phasor.real() * kSubcarrierCos[k]) -
         (phasor.imag() * kSubcarrierSin[k]);
}

// The burst of one line as a unit phasor in the same frame as the split's
// chroma phasors, from samples [start, end): nullopt where its amplitude is
// below |min_amplitude| codes (no burst: a monochrome line or source).
inline std::optional<std::complex<double>> burst_reference(
    const int16_t* line, int32_t start, int32_t end, double min_amplitude) {
  // Whole cycles only, so the line's level drops out of the sum.
  const int32_t count = ((end - start) / 4) * 4;
  if (count <= 0) return std::nullopt;
  std::complex<double> sum{0.0, 0.0};
  for (int32_t n = start; n < start + count; ++n) {
    const auto k = static_cast<size_t>(n & 3);
    sum += static_cast<double>(line[n]) *
           std::complex<double>(kSubcarrierCos[k], -kSubcarrierSin[k]);
  }
  const std::complex<double> phasor = 2.0 * sum / static_cast<double>(count);
  const double amplitude = std::abs(phasor);
  if (amplitude < min_amplitude || amplitude <= 0.0) return std::nullopt;
  return phasor / amplitude;
}

// Whether two lines of one field, |field_line_distance| apart, carry
// opposite V-switch: on PAL and PAL-M the V phase inverts on every line.
inline bool v_switch_differs(VideoSystem system, int32_t field_line_distance) {
  const bool pal_colour =
      system == VideoSystem::PAL || system == VideoSystem::PAL_M;
  return pal_colour && (field_line_distance % 2) != 0;
}

// The distance in field lines to the nearest line of the same field whose
// chroma is a line's inverted: NTSC's subcarrier turns half a cycle a line;
// PAL's and PAL-M's a quarter (PAL's 0.0016 cycles more), with V-switch the
// same again two lines on.
inline int32_t comb_distance(VideoSystem system) {
  return system == VideoSystem::NTSC ? 1 : 2;
}

// A chroma phasor of one line, made relative to its burst: the same colour
// gives the same value on every line of NTSC, and on PAL and PAL-M on every
// line of the same V-switch. Between lines of opposite V-switch the values
// are each other's complex conjugate, as inverting V reflects both the colour
// and the burst's swing about the U axis, so V-switch need not be detected.
inline std::complex<double> to_burst_relative(std::complex<double> phasor,
                                              std::complex<double> burst) {
  return phasor * std::conj(burst);
}

inline std::complex<double> from_burst_relative(std::complex<double> relative,
                                                std::complex<double> burst) {
  return relative * burst;
}

// A burst-relative phasor of a line moved to a line of the other V-switch
// (see v_switch_differs()); unchanged otherwise.
inline std::complex<double> across_v_switch(std::complex<double> relative,
                                            bool differs) {
  return differs ? std::conj(relative) : relative;
}

// One span of a line, split: luma[i] and chroma[i] describe sample
// start + i, and luma[i] + remodulate(chroma[i], start + i) is that sample
// exactly.
struct SplitSpan {
  int32_t start = 0;
  std::vector<double> luma;
  std::vector<std::complex<double>> chroma;
};

// Splits spans of a system's lines. The chroma is the line demodulated
// against the 4FSC reference and low-passed to kChromaBandwidthHz (a
// Hann-windowed sinc, kChromaFilterRadius each side), so it carries the
// chroma band and any luma within it; the luma is what remains, so a split
// and its re-modulation give the line back exactly.
class ChromaSplitter {
 public:
  explicit ChromaSplitter(VideoSystem system) {
    const double cutoff = kChromaBandwidthHz / sample_rate_from_system(system);
    double sum = 0.0;
    for (int32_t k = -kChromaFilterRadius; k <= kChromaFilterRadius; ++k) {
      const double x = 2.0 * cutoff * k;
      const double sinc = k == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
      const double window =
          0.5 + (0.5 * std::cos(kPi * k / (kChromaFilterRadius + 1)));
      const double tap = 2.0 * cutoff * sinc * window;
      taps_[static_cast<size_t>(k + kChromaFilterRadius)] = tap;
      sum += tap;
    }
    for (double& tap : taps_) tap /= sum;
  }

  // The baseband chroma of samples [start, end) of a line of |length|
  // samples, scaled so that remodulate() gives the chroma back. Samples
  // beyond the line's ends repeat its end samples.
  void demodulate(const int16_t* line, int32_t length, int32_t start,
                  int32_t end, std::vector<std::complex<double>>& out) const {
    out.assign(static_cast<size_t>(std::max(0, end - start)), {0.0, 0.0});
    if (length <= 0) return;
    for (int32_t n = start; n < end; ++n) {
      double re = 0.0;
      double im = 0.0;
      for (int32_t k = -kChromaFilterRadius; k <= kChromaFilterRadius; ++k) {
        const int32_t m = std::clamp(n + k, 0, length - 1);
        const double sample =
            taps_[static_cast<size_t>(k + kChromaFilterRadius)] * line[m];
        // The reference's phase follows the sample read, not the clamp:
        // e^{-j (n + k) pi/2}.
        const auto q = static_cast<size_t>((n + k) & 3);
        re += sample * kSubcarrierCos[q];
        im -= sample * kSubcarrierSin[q];
      }
      out[static_cast<size_t>(n - start)] = {2.0 * re, 2.0 * im};
    }
  }

  // Samples [start, end) of a line of |length| samples, split.
  SplitSpan split(const int16_t* line, int32_t length, int32_t start,
                  int32_t end) const {
    SplitSpan span;
    span.start = start;
    demodulate(line, length, start, end, span.chroma);
    span.luma.resize(span.chroma.size());
    for (int32_t n = start; n < end; ++n) {
      const auto i = static_cast<size_t>(n - start);
      span.luma[i] = line[n] - remodulate(span.chroma[i], n);
    }
    return span;
  }

  // As split(), with |partner|, a line of the same field comb_distance()
  // away whose chroma is the line's inverted, as a vertical comb: half their
  // difference holds the line's chroma without the luma they share, so luma
  // in the chroma band (fine horizontal detail) stays luma. Where their luma
  // below the band differs by more than |max_luma_difference| codes, the
  // picture changes between them and the line's own split is used, blending
  // from half that difference.
  SplitSpan split_combed(const int16_t* line, const int16_t* partner,
                         int32_t length, int32_t start, int32_t end,
                         double max_luma_difference) const {
    SplitSpan span = split(line, length, start, end);
    const SplitSpan other = split(partner, length, start, end);
    const double full = max_luma_difference / 2.0;
    for (size_t i = 0; i < span.chroma.size(); ++i) {
      const double difference = std::abs(span.luma[i] - other.luma[i]);
      const double comb = std::clamp(
          (max_luma_difference - difference) / (max_luma_difference - full),
          0.0, 1.0);
      const auto n = static_cast<int32_t>(i) + start;
      const std::complex<double> combed =
          (span.chroma[i] - other.chroma[i]) / 2.0;
      span.chroma[i] = (comb * combed) + ((1.0 - comb) * span.chroma[i]);
      span.luma[i] = line[n] - remodulate(span.chroma[i], n);
    }
    return span;
  }

 private:
  std::array<double, (2 * kChromaFilterRadius) + 1> taps_{};
};

}  // namespace orc::composite_split
