/*
 * File:        secam_line_reference.h
 * Module:      decode-orc Plugin SDK (support)
 * Purpose:     Which colour difference (D'R or D'B) a SECAM line carries,
 *              read from the frequency of its back-porch sub-carrier
 *              reference
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/stage/cvbs_signal_constants.h>

#include <cmath>
#include <cstddef>

namespace orc {

// The back-porch reference window, kSecamSubcarrierReferenceStart/End less
// the 300 ns (5 sample) gate edges at each end: stored samples 104-180.  The
// guard also covers the up to 2 samples by which 0H moves through the field
// on the 4fsc lattice.
inline constexpr size_t kSecamReferenceGuard = 5;
inline constexpr size_t kSecamReferenceFirst =
    static_cast<size_t>(kSecamSubcarrierReferenceStart) + kSecamReferenceGuard;
inline constexpr size_t kSecamReferenceEnd =
    static_cast<size_t>(kSecamSubcarrierReferenceEnd) - kSecamReferenceGuard;

// A reference must lie within this of fOB..fOR to count; a line further out
// carries no SECAM reference.  Same tolerance as Video Sink's secam decoder.
inline constexpr double kSecamReferenceToleranceHz = 120.0e3;

struct SecamLineReading {
  double frequency_hz = 0.0;
  double amplitude = 0.0;  // sub-carrier peak, sample units
  // The share of the window's power (its level removed) that the
  // sub-carrier's amplitude accounts for: near 1 for a clean sub-carrier,
  // small for picture or noise.
  double purity = 0.0;
};

// Mean frequency, amplitude and purity of the sub-carrier over samples
// [first, end) of one line.
//
// The samples are mixed down by fs/4 (the 4FSC quadrature sequence 1, -j, -1,
// j) and summed in blocks of four.  The block sum removes the DC (blanking)
// level exactly and all but a few per cent of the image near fs/2, leaving the
// sub-carrier as a slowly rotating vector; the mean rotation between
// successive blocks is its offset from fs/4.  Valid for carriers within fs/8
// of fs/4 (2.2 to 6.7 MHz), which covers every SECAM deviation.
template <typename Sample>
SecamLineReading secam_line_frequency(const Sample* line, size_t first,
                                      size_t end, double sample_rate_hz) {
  SecamLineReading reading;
  if (end <= first + 8) return reading;
  const size_t blocks = (end - first) / 4;
  double prev_i = 0.0;
  double prev_q = 0.0;
  double sum_i = 0.0;
  double sum_q = 0.0;
  double sum_mag = 0.0;
  double sum = 0.0;
  double sum_squares = 0.0;
  for (size_t b = 0; b < blocks; ++b) {
    const Sample* s = line + first + (b * 4);
    for (size_t k = 0; k < 4; ++k) {
      const auto v = static_cast<double>(s[k]);
      sum += v;
      sum_squares += v * v;
    }
    // x0 - j x1 - x2 + j x3
    const double bi = static_cast<double>(s[0]) - static_cast<double>(s[2]);
    const double bq = static_cast<double>(s[3]) - static_cast<double>(s[1]);
    sum_mag += std::hypot(bi, bq);
    if (b > 0) {
      // B[b] * conj(B[b-1])
      sum_i += (bi * prev_i) + (bq * prev_q);
      sum_q += (bq * prev_i) - (bi * prev_q);
    }
    prev_i = bi;
    prev_q = bq;
  }
  const double step = std::atan2(sum_q, sum_i);  // radians per 4 samples
  reading.frequency_hz =
      (sample_rate_hz / 4.0) +
      (step / 4.0 * sample_rate_hz / (2.0 * 3.14159265358979323846));
  // A block of four sums the sub-carrier's quadrature phasor (peak / 2 per
  // sample) four times.
  reading.amplitude = sum_mag / static_cast<double>(blocks) / 2.0;
  const auto n = static_cast<double>(blocks * 4);
  const double mean = sum / n;
  const double power = (sum_squares / n) - (mean * mean);
  if (power > 0.0) {
    reading.purity = (reading.amplitude * reading.amplitude / 2.0) / power;
  }
  return reading;
}

// +1 when |reading| is a D'R reference (fOR), -1 a D'B one (fOB), 0 when it
// lies outside fOB..fOR by more than kSecamReferenceToleranceHz.  The caller
// decides first whether the reading's amplitude or purity makes it a
// sub-carrier at all.
inline int secam_reference_line_type(const SecamLineReading& reading) {
  if (reading.frequency_hz < kSecamFob - kSecamReferenceToleranceHz ||
      reading.frequency_hz > kSecamFor + kSecamReferenceToleranceHz) {
    return 0;
  }
  return reading.frequency_hz > 0.5 * (kSecamFor + kSecamFob) ? 1 : -1;
}

}  // namespace orc
