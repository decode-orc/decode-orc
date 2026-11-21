/*
 * File:        vbi_resampler.h
 * Module:      orc-stage-plugin-vbi_source
 * Purpose:     Band-limited resampling of VBI records onto the output lattice
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_VBI_RESAMPLER_H
#define ORC_VBI_RESAMPLER_H

#include <cstdint>
#include <vector>

#include "vbi_line_placement.h"

namespace orc {

// Shape of the band-limited interpolation kernel.
//
// The defaults are the 8 x fsc to 4 x fsc case the card captures need.  With a
// cutoff at 90 % of the output Nyquist the transition band closes about
// 8.7 MHz, which leaves the 6.9375 MHz WST carrier well inside the passband
// while putting everything that would fold onto it into the stopband
// (design §5.5).
struct VBIResamplerConfig {
  // Kernel half-width in source samples; the filter uses twice this many taps.
  //
  // This sets how narrow the transition band can be, and the WST carrier is
  // what makes it matter: at 6.9375 MHz the carrier is only 1 MHz below the
  // cutoff, so a shorter kernel would put the top of the data band inside the
  // filter's own rolloff and attenuate exactly the content the slicer reads.
  // A hundred and twenty-eight taps gives about 80 dB of stopband for the 2:1
  // case with room to spare either side.
  uint32_t half_width_samples = 64;

  // Kaiser window shape parameter.  Trades passband ripple and stopband
  // attenuation against transition width for a given tap count.
  double kaiser_beta = 8.0;

  // Cutoff as a fraction of the output Nyquist frequency.  Below one so the
  // transition band lands inside the output band instead of straddling Nyquist
  // and folding onto itself.
  double cutoff_fraction = 0.90;

  // Distinct fractional delays the kernel is resolved at, per source sample.
  //
  // A thousandth of a source sample is 28 ps at the bt8x8 rate, four orders of
  // magnitude below the tenth of an output sample placement is judged to, and
  // it bounds what the phase bank can cost on a source whose resampling ratio
  // is irrational.
  uint32_t phase_steps = 1024;

  // Whether resample_records() may work its fours of records in AVX2 where
  // the processor has it (x86-64, built with GCC or Clang).  The output is the
  // same to the bit either way — the same arithmetic in fewer instructions —
  // which is what turning it off lets a test hold it to.
  bool use_avx2 = true;
};

// Low-pass-and-resample in one pass, over the fixed window a placement gives.
//
// Decimating an 8 x fsc capture by dropping every other sample would alias the
// 6.9375 MHz WST carrier straight back onto itself, because it sits at 78 % of
// the 8.87 MHz output Nyquist; the anti-alias filter is not optional
// (design §5.5).  Filtering and fractional delay are the same operation here:
// the kernel is centred on the requested position, which both band-limits and
// interpolates, and its symmetry is what makes the group delay zero — a value
// asked for at position p is the waveform at p, not at p plus a filter delay.
//
// Nothing else happens to the samples.  No slicing, sharpening, level
// restoration or re-quantisation: a deconvolving slicer downstream recovers
// data by matching the blurred waveform it is given, and conditioning would
// destroy exactly the information it depends on (design §5.3.6).
//
// Every output sample of every data line of every frame reads its record at
// the same set of positions, because the placement is one map for the whole
// run.  The kernel is therefore resolved once, at construction: each output
// sample is reduced to the record sample its kernel is centred on and a row of
// weights, and the rows are shared by every output sample that lands on the
// same fractional delay.  A capture whose rate is a whole multiple of the
// output's — which is every card capture in the design's table — has one such
// delay and so one row, small enough to stay in the innermost cache for the
// whole of a frame.  What is left at run time is a flat multiply-accumulate
// per output sample with no arithmetic on the kernel at all.
//
// Each row is normalised to sum to one, which is what keeps a constant region
// such as the quiet back porch from rippling by the kernel's phase-to-phase
// gain variation, and what lets the amplitude map be applied to the output
// rather than to every stored sample.  Positions near either end of the record
// read the nearest stored sample: constant extension keeps a quiet head or
// tail at its own level rather than pulling it towards an invented zero, which
// would be a step edge in the middle of the sample path.
//
// Immutable after construction; const members are safe to call concurrently.
class VBIRecordResampler {
 public:
  // An empty resampler that produces nothing.  Construct with a placement to
  // obtain a usable one.
  VBIRecordResampler() = default;

  // |record_samples| is the valid sample count of a stored record, which fixes
  // where the kernel has to fall back on constant extension.
  VBIRecordResampler(const VBIDataPlacement& placement, uint32_t record_samples,
                     VBIResamplerConfig config = {});

  const VBIResamplerConfig& config() const { return config_; }

  // Output samples produced per record: placement.output_count().
  uint32_t output_count() const { return output_count_; }

  // Taps applied per output sample.
  uint32_t taps() const { return taps_; }

  // Distinct fractional delays the output window resolved to.  One for a whole
  // number resampling ratio.
  uint32_t phase_row_count() const {
    return taps_ != 0 ? static_cast<uint32_t>(bank_.size() / taps_) : 0;
  }

  // Filter cutoff as a fraction of the source sampling rate.
  double cutoff_fraction_of_source_rate() const { return cutoff_normalised_; }

  // Resample one record.  |out_samples| is resized to output_count() and holds
  // the waveform at output line samples placement.output_begin onwards, in the
  // record's own amplitude domain.
  void resample(const std::vector<double>& record,
                std::vector<double>& out_samples) const;

  // Resample several records at once, as a frame's are placed.  |out_samples|
  // is resized to records.size() x output_count(), and record k's waveform
  // starts at k x output_count().  Every output sample is the sum the
  // single-record form makes — the same products, added in the same order —
  // so the two agree to the bit.  What differs is that four records are worked
  // side by side, and several such fours at once — or, for fewer records,
  // several output samples at once: a sum is a chain of additions each waiting
  // on the last, and only independent chains let the processor overlap them
  // (four at a time in one AVX2 register where the processor has it, two in
  // SSE2 otherwise).  Each tap's weight is also read once for all of a four
  // rather than once a record, which matters at a ratio that is not a whole
  // number, whose phase bank is a row per output sample.
  void resample_records(const std::vector<const std::vector<double>*>& records,
                        std::vector<double>& out_samples) const;

 private:
  VBIResamplerConfig config_{};
  double cutoff_normalised_ = 0.0;
  uint32_t output_count_ = 0;
  uint32_t taps_ = 0;

  // Kernel rows, taps_ weights apiece, one per distinct fractional delay.
  std::vector<double> bank_;

  // Per output sample: the record sample the kernel's first tap reads, and the
  // bank row that weights it.
  std::vector<int32_t> first_tap_;
  std::vector<uint32_t> row_;

  // One record's output samples, written to |out|.
  void resample_into(const std::vector<double>& record, double* out) const;
};

}  // namespace orc

#endif  // ORC_VBI_RESAMPLER_H
