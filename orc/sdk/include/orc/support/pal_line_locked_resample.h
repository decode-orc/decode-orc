/*
 * File:        pal_line_locked_resample.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Conversion between line-locked PAL fields (ld-decode .tbc) and
 *              the CVBS_U10_4FSC subcarrier-locked frame lattice
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/stage/cvbs_signal_constants.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// ld-decode's PAL `.tbc` output is line-locked: every stored line is exactly
// 1135 samples and 0H sits at the same sample position on every line. The
// CVBS_U10_4FSC PAL frame is one uniform 4fsc lattice of 709,379 samples
// (1135.0064 samples per line, 0H tracking the analogue line period; CVBS
// spec, video-standard-presets, Preset PAL "Stored frame alignment").
//
// Within a field both grids are uniform in time, so converting between them is
// a uniform resample with a ratio of 709,375 : 709,379 (625 × 1135 line-locked
// samples per frame period against 709,379 lattice samples). Copying the
// line-locked samples straight into the frame instead leaves the subcarrier
// drifting +0.576° per line against the phase its sample position implies, and
// the picture up to 2 samples away from where the lattice puts it (issue #369).
//
// Field geometry in the frame-flat buffer (EBU Tech. 3280-E §1.3.1):
//   field 1: frame rows 0–312,   313 × 1135 line-locked samples
//            → 313 × 1135 + 2 lattice samples starting at frame offset 0
//   field 2: frame rows 313–624, 312 × 1135 line-locked samples
//            → 312 × 1135 + 2 lattice samples starting at 313 × 1135 + 2
//
// 0H of frame row k sits at lattice position k × 709379 / 625. Row 0 is the
// frame origin; row 313 lands 2/625 (0.0032) samples after field 2's first
// lattice sample. Each field is line-locked to its own first 0H, so lattice
// sample j of field f sits at line-locked position
//     m = (625 × j − c_f) × 1135 / 709379,    c_1 = 0, c_2 = 2,
// and line-locked sample m sits at lattice position
//     j = (709379 × m + 1135 × c_f) / 709375.
// Both are exact rationals; every helper below works in integers.

namespace orc::pal_line_locked {

// Line-locked samples in one frame period (625 lines × 1135).
inline constexpr int64_t kLineLockedSamplesPerFrame =
    static_cast<int64_t>(kPalFrameLines) * kPalSamplesPerLineNominal;
// Lattice samples in one frame period.
inline constexpr int64_t kLatticeSamplesPerFrame = kPalFrameSamples;

static_assert(kLineLockedSamplesPerFrame == 709375);
static_assert(kLatticeSamplesPerFrame == 709379);

// Number of stored lines in PAL field 1 (313) or 2 (312).
inline constexpr int32_t field_lines(int field) {
  return field == 1 ? kPalField1Lines : kPalFrameLines - kPalField1Lines;
}

// Line-locked samples in one field: 313 × 1135 or 312 × 1135.
inline constexpr size_t line_locked_field_samples(int field) {
  return static_cast<size_t>(field_lines(field)) *
         static_cast<size_t>(kPalSamplesPerLineNominal);
}

// Lattice samples in one field: two more than the line-locked field.
inline constexpr size_t lattice_field_samples(int field) {
  return line_locked_field_samples(field) + 2;
}

// Frame-flat offset of the field's first lattice sample.
inline constexpr size_t lattice_field_start(int field) {
  return field == 1 ? 0 : lattice_field_samples(1);
}

static_assert(lattice_field_samples(1) + lattice_field_samples(2) ==
              static_cast<size_t>(kPalFrameSamples));

// Offset of the field's first 0H after its first lattice sample, in units of
// 1/625 lattice sample (c_f above).
inline constexpr int64_t field_origin_625ths(int field) {
  return field == 1 ? 0 : 2;
}

namespace detail {

// Floor division for a possibly negative numerator and a positive divisor.
inline constexpr int64_t floor_div(int64_t num, int64_t den) {
  const int64_t q = num / den;
  return (num % den != 0 && num < 0) ? q - 1 : q;
}

inline constexpr int64_t ceil_div(int64_t num, int64_t den) {
  return -floor_div(-num, den);
}

// Numerators of the two position maps; the denominators are the frame-period
// sample counts of the target grid's opposite.
inline constexpr int64_t lattice_numerator(int field, int64_t m) {
  return kLatticeSamplesPerFrame * m +
         kPalSamplesPerLineNominal * field_origin_625ths(field);
}

inline constexpr int64_t line_locked_numerator(int field, int64_t j) {
  return (kPalFrameLines * j - field_origin_625ths(field)) *
         kPalSamplesPerLineNominal;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Coordinate maps (field-relative sample indices)
// ---------------------------------------------------------------------------

// Lattice position of line-locked sample m of |field|.
inline double lattice_position(int field, double m) {
  return (static_cast<double>(kLatticeSamplesPerFrame) * m +
          static_cast<double>(kPalSamplesPerLineNominal *
                              field_origin_625ths(field))) /
         static_cast<double>(kLineLockedSamplesPerFrame);
}

// Line-locked position of lattice sample j of |field|.
inline double line_locked_position(int field, double j) {
  return (static_cast<double>(kPalFrameLines) * j -
          static_cast<double>(field_origin_625ths(field))) *
         static_cast<double>(kPalSamplesPerLineNominal) /
         static_cast<double>(kLatticeSamplesPerFrame);
}

// The lattice samples that cover line-locked samples [first, last] (both
// inclusive) of |field|: the floor of the first position and the ceiling of
// the last, so a marked region never shrinks when it moves grid. Clamped to
// the field.
struct SampleSpan {
  int64_t first = 0;
  int64_t last = 0;  // inclusive
};

inline SampleSpan lattice_span(int field, int64_t first, int64_t last) {
  const int64_t max_j = static_cast<int64_t>(lattice_field_samples(field)) - 1;
  SampleSpan s;
  s.first = detail::floor_div(detail::lattice_numerator(field, first),
                              kLineLockedSamplesPerFrame);
  s.last = detail::ceil_div(detail::lattice_numerator(field, last),
                            kLineLockedSamplesPerFrame);
  if (s.first < 0) s.first = 0;
  if (s.last > max_j) s.last = max_j;
  return s;
}

// The line-locked samples that cover lattice samples [first, last] (both
// inclusive) of |field|. Clamped to the field.
inline SampleSpan line_locked_span(int field, int64_t first, int64_t last) {
  const int64_t max_m =
      static_cast<int64_t>(line_locked_field_samples(field)) - 1;
  SampleSpan s;
  s.first = detail::floor_div(detail::line_locked_numerator(field, first),
                              kLatticeSamplesPerFrame);
  s.last = detail::ceil_div(detail::line_locked_numerator(field, last),
                            kLatticeSamplesPerFrame);
  if (s.first < 0) s.first = 0;
  if (s.last > max_m) s.last = max_m;
  return s;
}

// ---------------------------------------------------------------------------
// Resampling
// ---------------------------------------------------------------------------

namespace detail {

// 16-tap windowed sinc (cos² window over 17 taps), tabulated at
// kKernelPhases + 1 fractional positions and normalised to unity DC gain per
// phase. Against an ideal fractional delay it is within 0.2 % in gain and
// 0.07° in phase up to 0.4 fs (7.1 MHz at PAL 4fsc), so the chroma band and
// the luma band are carried through unchanged; the residual is far below the
// 10-bit quantisation of the output.
inline constexpr int kKernelTaps = 16;
inline constexpr int kKernelLeft = kKernelTaps / 2 - 1;  // taps base-7…base+8
inline constexpr int kKernelPhases = 1024;

struct KernelTable {
  std::array<float, static_cast<size_t>((kKernelPhases + 1) * kKernelTaps)>
      taps{};

  KernelTable() {
    constexpr double kPi = 3.14159265358979323846;
    for (int p = 0; p <= kKernelPhases; ++p) {
      const double frac = static_cast<double>(p) / kKernelPhases;
      double h[kKernelTaps];
      double sum = 0.0;
      for (int k = 0; k < kKernelTaps; ++k) {
        const double t = static_cast<double>(k - kKernelLeft) - frac;
        const double sinc = (t == 0.0) ? 1.0 : std::sin(kPi * t) / (kPi * t);
        const double w = std::cos(kPi * t / (kKernelTaps + 1));
        h[k] = sinc * w * w;
        sum += h[k];
      }
      for (int k = 0; k < kKernelTaps; ++k) {
        taps[static_cast<size_t>(p * kKernelTaps + k)] =
            static_cast<float>(h[k] / sum);
      }
    }
  }
};

inline const KernelTable& kernel_table() {
  static const KernelTable table;
  return table;
}

// Resample |in| (n_in samples) so output j is |in| evaluated at position
// (step × j + origin) / den, where step / den is close to 1. Positions that
// fall outside |in| read its edge sample. map(float) → output sample.
//
// Defined in this header so the per-sample work inlines: plugins build with
// default symbol visibility, so an out-of-line helper would cost a PLT call
// per sample.
template <typename In, typename Out, typename Map>
void resample_uniform(const In* in, size_t n_in, Out* out, size_t n_out,
                      int64_t step, int64_t origin, int64_t den, Map map) {
  if (n_in == 0 || n_out == 0) return;

  // Edge-padded float copy of the input, so the tap loop never bounds-checks.
  constexpr size_t kPad = static_cast<size_t>(kKernelTaps);
  thread_local std::vector<float> padded;
  padded.resize(n_in + 2 * kPad);
  {
    const float first = static_cast<float>(in[0]);
    const float last = static_cast<float>(in[n_in - 1]);
    for (size_t i = 0; i < kPad; ++i) padded[i] = first;
    for (size_t i = 0; i < n_in; ++i) {
      padded[kPad + i] = static_cast<float>(in[i]);
    }
    for (size_t i = 0; i < kPad; ++i) padded[kPad + n_in + i] = last;
  }
  const float* src = padded.data();
  const float* taps = kernel_table().taps.data();
  const int64_t lo = -static_cast<int64_t>(kPad) + kKernelLeft;
  const int64_t hi = static_cast<int64_t>(n_in + kPad) - kKernelTaps +
                     kKernelLeft;  // highest base with all taps in range

  // Walk the position exactly: base + rem / den.
  const int64_t step_whole = step / den;
  const int64_t step_rem = step % den;
  int64_t base = floor_div(origin, den);
  int64_t rem = origin - base * den;
  const double phase_scale =
      static_cast<double>(kKernelPhases) / static_cast<double>(den);

  // The fractional phase changes every few hundred samples and the base steps
  // by one almost everywhere, so the output splits into long runs that share
  // one kernel row — a plain FIR over consecutive inputs, which the compiler
  // vectorises across outputs.
  size_t j = 0;
  while (j < n_out) {
    const int phase = static_cast<int>(static_cast<double>(rem) * phase_scale +
                                       0.5);  // 0…kKernelPhases
    const int64_t run_base = base;
    size_t run = 0;
    do {
      ++run;
      const int64_t next_base_raw = base + step_whole;
      int64_t next_rem = rem + step_rem;
      int64_t next_base = next_base_raw;
      if (next_rem >= den) {
        next_rem -= den;
        ++next_base;
      }
      base = next_base;
      rem = next_rem;
      if (j + run >= n_out) break;
      const int next_phase =
          static_cast<int>(static_cast<double>(rem) * phase_scale + 0.5);
      if (next_phase != phase || base != run_base + static_cast<int64_t>(run))
        break;
    } while (true);

    const float* h = taps + static_cast<size_t>(phase) * kKernelTaps;
    const int64_t run_last = run_base + static_cast<int64_t>(run) - 1;
    if (run_base >= lo && run_last <= hi) {
      // The resample positions never leave the input by more than a sample,
      // so with kPad samples of padding every run takes this path.
      const float* s0 =
          src + (run_base + static_cast<int64_t>(kPad) - kKernelLeft);
      // Taps outer, outputs inner: each output still sums its taps in order
      // 0…15, but the inner loop has no reduction, so it vectorises.
      constexpr size_t kChunk = 256;
      float acc[kChunk];
      for (size_t c = 0; c < run; c += kChunk) {
        const size_t len = (run - c < kChunk) ? run - c : kChunk;
        const float* sc = s0 + c;
        for (size_t i = 0; i < len; ++i) acc[i] = h[0] * sc[i];
        for (int k = 1; k < kKernelTaps; ++k) {
          const float hk = h[k];
          const float* sk = sc + k;
          for (size_t i = 0; i < len; ++i) acc[i] += hk * sk[i];
        }
        for (size_t i = 0; i < len; ++i) out[j + c + i] = map(acc[i]);
      }
    } else {
      for (size_t i = 0; i < run; ++i) {
        int64_t b = run_base + static_cast<int64_t>(i);
        if (b < lo) b = lo;
        if (b > hi) b = hi;
        const float* s = src + (b + static_cast<int64_t>(kPad) - kKernelLeft);
        float acc = 0.0f;
        for (int k = 0; k < kKernelTaps; ++k) acc += h[k] * s[k];
        out[j + i] = map(acc);
      }
    }
    j += run;
  }
}

}  // namespace detail

// Resample a line-locked field (line_locked_field_samples(field) samples)
// onto the lattice (lattice_field_samples(field) samples written to |out|).
template <typename In, typename Out, typename Map>
void to_lattice(int field, const In* in, Out* out, Map map) {
  detail::resample_uniform(
      in, line_locked_field_samples(field), out, lattice_field_samples(field),
      kLineLockedSamplesPerFrame,
      -kPalSamplesPerLineNominal * field_origin_625ths(field),
      kLatticeSamplesPerFrame, map);
}

// Resample a lattice field (lattice_field_samples(field) samples) back to
// line-locked samples (line_locked_field_samples(field) written to |out|).
template <typename In, typename Out, typename Map>
void to_line_locked(int field, const In* in, Out* out, Map map) {
  detail::resample_uniform(
      in, lattice_field_samples(field), out, line_locked_field_samples(field),
      kLatticeSamplesPerFrame,
      kPalSamplesPerLineNominal * field_origin_625ths(field),
      kLineLockedSamplesPerFrame, map);
}

}  // namespace orc::pal_line_locked
