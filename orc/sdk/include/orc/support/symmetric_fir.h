/*
 * File:        symmetric_fir.h
 * Module:      decode-orc Plugin SDK (support)
 * Purpose:     A symmetric FIR over a run of samples, with each output summed
 *              in tap order
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <cstddef>
#include <cstdint>

namespace orc {

// out[i] = taps[0] x[i] + sum over k = 1..radius of taps[k] (x[i - k] +
// x[i + k]), for i in [0, count). x must be readable |radius| samples before
// its first and after its last output.
//
// Each output is summed in tap order, k = 1 first, so the result is the same
// to the bit as the plain loop over the taps for one output at a time. The
// loop over the outputs is innermost, so it vectorises, and takes four taps a
// pass, so each output is loaded and stored once per four taps instead of
// once per tap.
inline void symmetric_fir(const double* taps, int32_t radius, const double* x,
                          size_t count, double* out) {
  for (size_t i = 0; i < count; ++i) out[i] = taps[0] * x[i];
  int32_t k = 1;
  for (; k + 3 <= radius; k += 4) {
    const double t0 = taps[k];
    const double t1 = taps[k + 1];
    const double t2 = taps[k + 2];
    const double t3 = taps[k + 3];
    const double* b0 = x - k;
    const double* b1 = b0 - 1;
    const double* b2 = b0 - 2;
    const double* b3 = b0 - 3;
    const double* a0 = x + k;
    const double* a1 = a0 + 1;
    const double* a2 = a0 + 2;
    const double* a3 = a0 + 3;
    for (size_t i = 0; i < count; ++i) {
      double sum = out[i];
      sum += t0 * (b0[i] + a0[i]);
      sum += t1 * (b1[i] + a1[i]);
      sum += t2 * (b2[i] + a2[i]);
      sum += t3 * (b3[i] + a3[i]);
      out[i] = sum;
    }
  }
  for (; k <= radius; ++k) {
    const double tap = taps[k];
    const double* before = x - k;
    const double* after = x + k;
    for (size_t i = 0; i < count; ++i) {
      out[i] += tap * (before[i] + after[i]);
    }
  }
}

}  // namespace orc
