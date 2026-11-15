/*
 * File:        composite_vectorscope.cpp
 * Module:      orc-core
 * Purpose:     Chroma demodulation from the composite carrier for the
 *              technical (measurement) vectorscope
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "composite_vectorscope.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace orc {
namespace {

constexpr double kTwoPi = 2.0 * M_PI;
constexpr double kRadiansToDegrees = 180.0 / M_PI;

// EBU Tech. 3280-E §1.2: PAL burst is 300 mV peak-to-peak on a 700 mV
// luminance range, i.e. 150/700 = 21.43 IRE peak.
constexpr double kPalNominalBurstIre = 100.0 * 150.0 / 700.0;
// SMPTE 170M-2004 §8.4: NTSC burst is 40 IRE peak-to-peak → 20 IRE peak.
constexpr double kNtscNominalBurstIre = 20.0;

// A line's burst must reach this fraction of the nominal amplitude before it is
// trusted as a phase reference; below it the line is treated as burst-free
// (vertical interval, mono source, or severe dropout).
constexpr double kBurstPresenceFraction = 0.25;

// Half-width, in burst-carrying lines, of the local fit that tracks the burst
// phase — the flywheel of the burst-locked reference.  At the 625-line rate a
// 65-line window is a loop time constant of about two milliseconds, the same
// order as the burst PLL of a bench vectorscope: long enough to average
// per-line noise down by around 8x, short enough to follow any drift a real
// timebase leaves behind.
constexpr size_t kReferenceTrackHalfWidth = 32;

// Quadrature reference for one line: sin/cos of the subcarrier at the four
// sample phases of 4FSC sampling, offset by the line's start phase.
struct LineReference {
  double sine[4];
  double cosine[4];
};

LineReference make_line_reference(double cycles_per_line, size_t line) {
  const double base =
      cycles_per_line * static_cast<double>(line);  // in subcarrier cycles
  const double frac = base - std::floor(base);
  LineReference ref{};
  for (int k = 0; k < 4; ++k) {
    const double radians = kTwoPi * (frac + (0.25 * k));
    ref.sine[k] = std::sin(radians);
    ref.cosine[k] = std::cos(radians);
  }
  return ref;
}

// ---------------------------------------------------------------------------
// Chroma band-pass
// ---------------------------------------------------------------------------
// An instrument band-limits around the subcarrier before demodulating.  Doing
// only enough filtering to reject the demodulation image leaves the whole luma
// band folded into the plot, and every sync edge and picture transition then
// throws a full-amplitude rotating vector across the display — which buries
// the trace a measurement scope exists to read.
//
// The kernel is the cascade of two parts, applied to the demodulated baseband:
//
//   * a symmetric one-subcarrier-cycle boxcar, weights [0.5, 1, 1, 1, 0.5]/4.
//     Spanning exactly one cycle of the reference, it is an *exact* null both
//     for the 2fsc product image and for the luminance the product detector
//     places at fsc.  Cascading keeps those nulls exact whatever follows.
//   * a raised-cosine low-pass of half-width 0.5·fs/chroma bandwidth, the same
//     shape and sizing the PAL decoder uses for its chroma filter, which is
//     what limits the passband to the chroma sidebands.
//
// EBU Tech. 3280-E §2.2 / ITU-R BT.470-6 Table 2: PAL chroma sidebands span
// about +1.07/−1.30 MHz.  The 0.93 divisor matches the decoder's empirical
// value for 4FSC-sampled material.
constexpr double kChromaBandwidthHz = 1100000.0 / 0.93;

// Symmetric FIR kernel, index 0 = centre tap, running to |half_width|.
struct SymmetricKernel {
  std::vector<double> taps;  // taps[0] is the centre
  size_t half_width() const { return taps.empty() ? 0 : taps.size() - 1; }
};

SymmetricKernel make_chroma_kernel(VideoSystem system) {
  const double sample_rate = sample_rate_from_system(system);
  const double cutoff_taps = 0.5 * sample_rate / kChromaBandwidthHz;
  const size_t rc_half = static_cast<size_t>(cutoff_taps);

  // Raised cosine, one side.
  std::vector<double> raised_cosine(rc_half + 1, 0.0);
  for (size_t f = 0; f <= rc_half; ++f) {
    raised_cosine[f] =
        1.0 + std::cos(M_PI * static_cast<double>(f) / cutoff_taps);
  }

  // One-subcarrier-cycle boxcar, one side (centre plus two).
  const double boxcar[3] = {1.0, 1.0, 0.5};

  // Convolve the two, both symmetric about zero, into a symmetric result.
  const size_t half = rc_half + 2;
  std::vector<double> full((2 * half) + 1, 0.0);
  for (int a = -2; a <= 2; ++a) {
    const double wa = boxcar[static_cast<size_t>(std::abs(a))];
    for (int b = -static_cast<int>(rc_half); b <= static_cast<int>(rc_half);
         ++b) {
      const double wb = raised_cosine[static_cast<size_t>(std::abs(b))];
      const size_t index = static_cast<size_t>(a + b) + half;
      full[index] += wa * wb;
    }
  }

  // Unit DC gain: the demodulated baseband's DC *is* the chroma vector, so the
  // recovered U/V must come out at the modulating amplitude.
  double sum = 0.0;
  for (double tap : full) sum += tap;

  SymmetricKernel kernel;
  kernel.taps.resize(half + 1);
  for (size_t f = 0; f <= half; ++f) {
    kernel.taps[f] = full[half + f] / sum;
  }
  return kernel;
}

// Product detector for one line.
//
// The chroma on a 4FSC line is x[i] = Y + U·sin(θ_i + φ) + V·cos(θ_i + φ),
// where θ_i is the reference phase at sample i and φ is the (constant) offset
// between the reference and the transmitted subcarrier.  Multiplying by
// 2sin(θ) and 2cos(θ) and low-pass filtering leaves
//   p = U cos φ − V sin φ,   q = U sin φ + V cos φ,
// i.e. (U, V) rotated by φ.
//
// The raw products are formed once per line and the band-pass then convolved
// over them, rather than re-forming them under every output tap.
//
// The band-pass runs over a whole requested range at a time rather than being
// re-entered at each output sample.  An acquisition asks for a quadrature pair
// at every sample of every line — 709 375 of them for a PAL frame — through a
// nineteen-tap kernel, so this is the loop that decides whether the vectorscope
// keeps up with a playing preview.  Two things make it cheap: the samples the
// kernel overhangs the ends of the range are replicated into a padded input, so
// the inner loop carries no bounds test, and the kernel is written out in full
// rather than exploited for symmetry, so all three memory streams run forwards
// and the compiler can vectorise them.  Halving the multiplies by folding the
// symmetric taps costs more than it saves once the reads run backwards.
class LineDemodulator {
 public:
  // Output samples convolved per pass; see begin_line().
  static constexpr size_t kConvolutionBlock = 128;

  explicit LineDemodulator(const SymmetricKernel& kernel)
      : half_(kernel.half_width()) {
    taps_.resize((2 * half_) + 1);
    for (size_t t = 0; t < taps_.size(); ++t) {
      taps_[t] = kernel.taps[(t > half_) ? (t - half_) : (half_ - t)];
    }
  }

  // Point the detector at |line| and band-limit its quadrature products over
  // the sample range [|first|, |last|), which must lie within the line.
  void begin_line(const int16_t* line, size_t length,
                  const LineReference& reference, size_t first, size_t last) {
    first_ = first;
    const size_t run = (last > first) ? (last - first) : 0;
    out_p_.assign(run, 0.0);
    out_q_.assign(run, 0.0);
    if (run == 0 || length == 0) return;

    // Edge replication into the kernel's overhang, which is what clamping the
    // tap index did when the convolution was entered per sample.
    const size_t padded = run + (2 * half_);
    pad_p_.resize(padded);
    pad_q_.resize(padded);
    double* const pad_p = pad_p_.data();
    double* const pad_q = pad_q_.data();
    for (size_t j = 0; j < padded; ++j) {
      const int64_t wanted =
          static_cast<int64_t>(first + j) - static_cast<int64_t>(half_);
      const size_t index =
          (wanted < 0) ? 0 : std::min(static_cast<size_t>(wanted), length - 1);
      const double sample = static_cast<double>(line[index]);
      const size_t phase = index & 3u;
      pad_p[j] = sample * 2.0 * reference.sine[phase];
      pad_q[j] = sample * 2.0 * reference.cosine[phase];
    }

    // The sweep runs a block of output at a time through an automatic
    // accumulator.  The accumulator has to be somewhere the compiler can see
    // cannot alias the padded input, or it must assume every tap's write may
    // have changed the input and reload it, which keeps the whole convolution
    // scalar — and this is the loop the cost of an acquisition is made of.
    double* const dest_p = out_p_.data();
    double* const dest_q = out_q_.data();
    const double* const taps = taps_.data();
    const size_t tap_count = taps_.size();
    for (size_t base = 0; base < run; base += kConvolutionBlock) {
      const size_t count = std::min(kConvolutionBlock, run - base);
      double block_p[kConvolutionBlock];
      double block_q[kConvolutionBlock];
      for (size_t i = 0; i < count; ++i) {
        block_p[i] = 0.0;
        block_q[i] = 0.0;
      }
      for (size_t t = 0; t < tap_count; ++t) {
        const double weight = taps[t];
        const double* const source_p = pad_p + base + t;
        const double* const source_q = pad_q + base + t;
        for (size_t i = 0; i < count; ++i) {
          block_p[i] += weight * source_p[i];
          block_q[i] += weight * source_q[i];
        }
      }
      for (size_t i = 0; i < count; ++i) {
        dest_p[base + i] = block_p[i];
        dest_q[base + i] = block_q[i];
      }
    }
  }

  // Band-limited quadrature pair at sample |index|, which must lie in the
  // range the last begin_line() covered.
  void at(size_t index, double& p, double& q) const {
    const size_t offset = index - first_;
    p = out_p_[offset];
    q = out_q_[offset];
  }

 private:
  size_t half_;
  std::vector<double> taps_;
  size_t first_ = 0;
  std::vector<double> pad_p_;
  std::vector<double> pad_q_;
  std::vector<double> out_p_;
  std::vector<double> out_q_;
};

// Per-line burst vector in the (unrotated) demodulated frame.
struct LineBurst {
  bool present = false;
  double p = 0.0;
  double q = 0.0;
  double magnitude = 0.0;
};

double mean_angle(const std::vector<double>& sines,
                  const std::vector<double>& cosines) {
  double sum_sin = 0.0;
  double sum_cos = 0.0;
  for (size_t i = 0; i < sines.size(); ++i) {
    sum_sin += sines[i];
    sum_cos += cosines[i];
  }
  return std::atan2(sum_sin, sum_cos);
}

double wrap_to_pi(double radians) {
  while (radians > M_PI) radians -= kTwoPi;
  while (radians < -M_PI) radians += kTwoPi;
  return radians;
}

}  // namespace

double nominal_burst_amplitude_ire(VideoSystem system) {
  // ITU-R BT.1700-1 Annex 1 Part B: PAL-M carries the 525-line burst level.
  return (system == VideoSystem::PAL) ? kPalNominalBurstIre
                                      : kNtscNominalBurstIre;
}

double subcarrier_cycles_per_line(VideoSystem system) {
  const int32_t frame_samples = frame_samples_from_system(system);
  const int32_t frame_lines = frame_lines_from_system(system);
  if (frame_samples <= 0 || frame_lines <= 0) {
    return 0.0;
  }
  // A 4FSC frame holds frame_samples/4 subcarrier cycles.
  // EBU Tech. 3280-E §1.1 (PAL: 283.7516) / SMPTE 244M-2003 §4.1 (NTSC: 227.5).
  return static_cast<double>(frame_samples) /
         (4.0 * static_cast<double>(frame_lines));
}

std::optional<VectorscopeData> extract_composite_vectorscope(
    const int16_t* frame, size_t frame_sample_count,
    const SourceParameters& parameters, uint64_t field_number,
    const CompositeVectorscopeOptions& options) {
  if (frame == nullptr || frame_sample_count == 0 || !parameters.is_valid() ||
      parameters.frame_height <= 0) {
    return std::nullopt;
  }

  const VideoSystem system = parameters.system;
  // CVBS File Format Specification v1.8.1, Preset SECAM: SECAM chroma is FM
  // on two sub-carriers with no burst phase, so there is nothing for a
  // quadrature vectorscope to show; the dialog says so
  // (vectorscopeNotApplicableReason in orc/gui/preview/vectorscope_geometry.h).
  if (system == VideoSystem::SECAM) {
    return std::nullopt;
  }
  const double cycles_per_line = subcarrier_cycles_per_line(system);
  if (cycles_per_line <= 0.0) {
    return std::nullopt;
  }

  const size_t spl = static_cast<size_t>(parameters.frame_width_nominal);
  const size_t frame_lines = static_cast<size_t>(parameters.frame_height);
  const size_t f1_lines = field1_lines(system);

  const double level_range = std::max(
      1.0,
      static_cast<double>(parameters.white_level - parameters.blanking_level));

  // ------------------------------------------------------------------
  // Line geometry helpers
  // ------------------------------------------------------------------
  auto line_span = [&](size_t line, size_t& offset, size_t& length) -> bool {
    offset = frame_line_sample_offset(system, spl, line);
    length = frame_line_sample_count(system, spl, line);
    return (offset + length) <= frame_sample_count;
  };

  const auto [burst_start_raw, burst_end_raw] = colour_burst_range(system);
  const size_t burst_start = static_cast<size_t>(std::max(burst_start_raw, 0));
  const size_t burst_end =
      static_cast<size_t>(std::max(burst_end_raw, burst_start_raw));

  size_t active_start = 0;
  size_t active_end = spl;
  if (parameters.active_video_start >= 0 &&
      parameters.active_video_end > parameters.active_video_start &&
      static_cast<size_t>(parameters.active_video_end) <= spl) {
    active_start = static_cast<size_t>(parameters.active_video_start);
    active_end = static_cast<size_t>(parameters.active_video_end);
  }

  // ------------------------------------------------------------------
  // Burst survey — one vector per line, in the unrotated demodulated frame.
  // ------------------------------------------------------------------
  const SymmetricKernel kernel = make_chroma_kernel(system);
  LineDemodulator demodulator(kernel);
  std::vector<LineBurst> bursts(frame_lines);
  const double burst_presence_threshold = kBurstPresenceFraction *
                                          nominal_burst_amplitude_ire(system) *
                                          level_range / 100.0;

  // The band-pass smears each end of the burst into the blanking either side,
  // so its half-width is trimmed off before averaging: what is left is burst
  // and nothing else.
  const size_t burst_guard = kernel.half_width();

  for (size_t line = 0; line < frame_lines; ++line) {
    size_t offset = 0;
    size_t length = 0;
    if (!line_span(line, offset, length)) continue;

    const size_t window_start = burst_start + burst_guard;
    const size_t window_end =
        (burst_end > burst_guard) ? (burst_end - burst_guard) : burst_end;
    if (window_end <= window_start || window_end > length) continue;

    demodulator.begin_line(frame + offset, length,
                           make_line_reference(cycles_per_line, line),
                           window_start, window_end);
    double sum_p = 0.0;
    double sum_q = 0.0;
    for (size_t i = window_start; i < window_end; ++i) {
      double p = 0.0;
      double q = 0.0;
      demodulator.at(i, p, q);
      sum_p += p;
      sum_q += q;
    }
    const double n = static_cast<double>(window_end - window_start);
    LineBurst& burst = bursts[line];
    burst.p = sum_p / n;
    burst.q = sum_q / n;
    burst.magnitude = std::hypot(burst.p, burst.q);
    burst.present = burst.magnitude >= burst_presence_threshold;
  }

  // ------------------------------------------------------------------
  // Reference recovery — a burst-locked flywheel, not an arithmetic model.
  //
  // make_line_reference() predicts a line's start phase from the nominal
  // subcarrier-cycles-per-line.  That is exact for an ld-decode 4FSC TBC, but
  // it is still only a prediction: a file whose lines sit on a plain
  // integer-sample grid, or one carrying any residual timebase drift, walks
  // away from it by a fraction of a degree per line.  Left uncorrected the
  // walk rotates each line's chroma by a different angle and a frame of colour
  // bars plots as arcs at the bar radii instead of dots on the targets.
  //
  // So the reference is measured rather than assumed.  Each line's burst is
  // the phase truth for that line, and an instrument feeds it to a burst-
  // locked oscillator whose loop is slow enough to average the noise out while
  // still following genuine drift.  That oscillator is a local straight-line
  // fit over ±kReferenceTrackHalfWidthLines lines here: unbiased on a drift
  // ramp (at the ends of a field as well as the middle, which a plain moving
  // average is not), while dividing per-line phase noise down by roughly the
  // square root of the window — so per-line phase error stays visible as
  // spread around the burst vectors, which is what the jitter readout reports.
  //
  // The PAL V-switch has to come off before the fit.  The burst alternates
  // ±45° about the −U axis (ITU-R BT.470-6 Table 2 item 2.16), so a line has
  // two candidate un-swung angles 90° apart, and only one of them continues
  // the previous line's.  Walking the burst lines in order and taking the
  // nearer candidate therefore recovers the −U axis uniquely: no assumption
  // about which line carries which switch state, and no requirement that the
  // two states appear equally often.  NTSC has no swing and the burst angle is
  // the reference directly.
  //
  // PAL_M swings with PAL, not with NTSC: ITU-R BT.1700-1 Annex 1 Part B
  // gives it PAL colour encoding — V-axis switch and ±45° burst swing — on
  // the 525-line raster whose signal levels it borrows from NTSC.  Left out
  // of the swing, its two burst states are read as ±45° of phase noise: the
  // fit is pulled about by the square wave, no line is classified +V or −V,
  // and the jitter readout reports the swing itself (≈45° rms) instead of the
  // timebase error it is there to measure.
  //
  // The two fields are tracked independently, so a phase step at the field
  // boundary that the predictor does not model is absorbed rather than smeared
  // across the join.
  // ------------------------------------------------------------------
  const size_t first_active_field_line =
      (parameters.first_active_frame_line > 0)
          ? static_cast<size_t>(parameters.first_active_frame_line / 2)
          : 0;
  const size_t last_active_field_line =
      (parameters.last_active_frame_line > 0)
          ? static_cast<size_t>(parameters.last_active_frame_line / 2)
          : f1_lines;

  const bool is_pal_switched =
      (system == VideoSystem::PAL || system == VideoSystem::PAL_M);
  const double swing = is_pal_switched ? (M_PI / 4.0) : 0.0;

  // Per-line −U axis direction in the unrotated demodulated frame.  Left
  // unwrapped: only its sine and cosine are ever used.
  std::vector<double> reference_angle(frame_lines, 0.0);
  std::vector<size_t> reference_lines;
  bool locked = false;

  for (size_t field = 0; field < 2; ++field) {
    const size_t line_offset = (field == 0) ? 0 : f1_lines;
    const size_t field_height =
        (field == 0) ? f1_lines : (frame_lines - f1_lines);
    const size_t begin = std::min(first_active_field_line, field_height);
    const size_t end = std::min(last_active_field_line, field_height);

    std::vector<size_t> lines;
    for (size_t field_line = begin; field_line < end; ++field_line) {
      const size_t line = line_offset + field_line;
      if (line >= frame_lines || !bursts[line].present) continue;
      lines.push_back(line);
    }
    if (lines.empty()) continue;
    locked = true;

    // Walk the burst lines, taking the un-swung candidate nearest the running
    // angle, and unwrap as we go so the fit below sees a continuous ramp
    // rather than a sawtooth.
    std::vector<double> unswung(lines.size(), 0.0);
    double previous =
        std::atan2(bursts[lines[0]].q, bursts[lines[0]].p) - swing;
    for (size_t k = 0; k < lines.size(); ++k) {
      const double angle = std::atan2(bursts[lines[k]].q, bursts[lines[k]].p);
      double step = wrap_to_pi(angle - swing - previous);
      if (swing != 0.0) {
        const double other = wrap_to_pi(angle + swing - previous);
        if (std::abs(other) < std::abs(step)) step = other;
      }
      unswung[k] = previous + step;
      previous = unswung[k];
    }

    // The window always holds its full complement of lines, sliding rather
    // than truncating at the ends of the field: fitting a short one-sided
    // window there and reading its edge would roughly double the variance the
    // fit contributes, and that lands straight in the jitter readout.
    const size_t span =
        std::min(lines.size() - 1, 2 * kReferenceTrackHalfWidth);
    for (size_t k = 0; k < lines.size(); ++k) {
      size_t first =
          (k > kReferenceTrackHalfWidth) ? (k - kReferenceTrackHalfWidth) : 0;
      if (first + span >= lines.size()) first = lines.size() - 1 - span;

      const double centre = static_cast<double>(lines[k]);
      double count = 0.0;
      double sum_x = 0.0;
      double sum_y = 0.0;
      double sum_xx = 0.0;
      double sum_xy = 0.0;
      for (size_t j = first; j <= first + span; ++j) {
        const double x = static_cast<double>(lines[j]) - centre;
        count += 1.0;
        sum_x += x;
        sum_y += unswung[j];
        sum_xx += x * x;
        sum_xy += x * unswung[j];
      }
      // Value of the fitted line at this line, i.e. its intercept at x = 0.
      const double denominator = (count * sum_xx) - (sum_x * sum_x);
      reference_angle[lines[k]] =
          (std::abs(denominator) > 1e-9)
              ? (((sum_xx * sum_y) - (sum_x * sum_xy)) / denominator)
              : (sum_y / count);
      reference_lines.push_back(lines[k]);
    }

    // Lines with no usable burst — the vertical interval, or a dropout that
    // took the back porch with it — take the reference from the burst lines
    // either side, and hold it flat beyond the outermost ones.
    size_t cursor = 0;
    for (size_t field_line = 0; field_line < field_height; ++field_line) {
      const size_t line = line_offset + field_line;
      if (line >= frame_lines) break;
      while (cursor + 1 < lines.size() && lines[cursor + 1] <= line) ++cursor;
      if (line == lines[cursor]) continue;
      if (line < lines.front()) {
        reference_angle[line] = reference_angle[lines.front()];
      } else if (line > lines.back()) {
        reference_angle[line] = reference_angle[lines.back()];
      } else {
        const size_t below = lines[cursor];
        const size_t above = lines[cursor + 1];
        const double fraction = static_cast<double>(line - below) /
                                static_cast<double>(above - below);
        reference_angle[line] = reference_angle[below] +
                                (fraction * wrap_to_pi(reference_angle[above] -
                                                       reference_angle[below]));
      }
    }
  }

  // (p, q) is (U, V) rotated by the line's reference angle less π, because the
  // burst-locked reference lies on the −U axis (ITU-R BT.470-6 Table 2 item
  // 2.16 for PAL; SMPTE 170M-2004 §8.4 for NTSC).  Undoing that rotation
  // recovers U and V without correcting the V-switch: on a −V line the
  // recovered V is negative, which is what the instrument is supposed to show.
  std::vector<double> reference_cos(frame_lines, 1.0);
  std::vector<double> reference_sin(frame_lines, 0.0);
  for (size_t line = 0; line < frame_lines; ++line) {
    reference_cos[line] = std::cos(reference_angle[line]);
    reference_sin[line] = std::sin(reference_angle[line]);
  }
  auto to_uv = [&](size_t line, double p, double q, double& u, double& v) {
    const double cosine = reference_cos[line];
    const double sine = reference_sin[line];
    u = -((p * cosine) + (q * sine));
    v = (p * sine) - (q * cosine);
  };

  // ------------------------------------------------------------------
  // Per-line V-switch state.
  // ------------------------------------------------------------------
  std::vector<VectorscopeLinePhase> line_phases(
      frame_lines, VectorscopeLinePhase::NotApplicable);
  if (locked && is_pal_switched) {
    for (size_t line = 0; line < frame_lines; ++line) {
      if (!bursts[line].present) continue;
      double u = 0.0;
      double v = 0.0;
      to_uv(line, bursts[line].p, bursts[line].q, u, v);
      line_phases[line] = (v >= 0.0) ? VectorscopeLinePhase::VPositive
                                     : VectorscopeLinePhase::VNegative;
    }
  }

  // ------------------------------------------------------------------
  // Sample emission.
  // ------------------------------------------------------------------
  // Lines are selected and reported in interlaced frame-line numbering — the
  // numbering the decoded acquisition already uses, and the one
  // SourceParameters states the active picture in — rather than in the
  // sequential-field order the flat buffer is laid out in.  A picture region
  // is one contiguous range there and two disjoint ones here, so a single
  // first/last range can only mean the same thing in both acquisitions if it
  // means interlaced lines.
  auto to_interlaced = [&](size_t flat) {
    return (flat < f1_lines) ? (2 * flat) : ((2 * (flat - f1_lines)) + 1);
  };

  size_t line_begin =
      std::min(static_cast<size_t>(options.first_line), frame_lines - 1);
  size_t line_end =
      (options.last_line == 0)
          ? (frame_lines - 1)
          : std::min(static_cast<size_t>(options.last_line), frame_lines - 1);
  if (line_begin > line_end) std::swap(line_begin, line_end);

  if (options.active_lines_only && parameters.first_active_frame_line >= 0 &&
      parameters.last_active_frame_line > parameters.first_active_frame_line) {
    line_begin = std::max(
        line_begin, static_cast<size_t>(parameters.first_active_frame_line));
    line_end = std::min(
        line_end, static_cast<size_t>(parameters.last_active_frame_line) - 1);
  }

  // Flat lines whose interlaced index falls in the range, in buffer order so
  // the samples stay grouped by line for the renderer's trace.
  std::vector<size_t> emit_lines;
  size_t emit_first_interlaced = frame_lines;
  size_t emit_last_interlaced = 0;
  if (line_begin <= line_end) {
    emit_lines.reserve(line_end - line_begin + 1);
    for (size_t line = 0; line < frame_lines; ++line) {
      const size_t interlaced = to_interlaced(line);
      if (interlaced < line_begin || interlaced > line_end) continue;
      emit_lines.push_back(line);
      emit_first_interlaced = std::min(emit_first_interlaced, interlaced);
      emit_last_interlaced = std::max(emit_last_interlaced, interlaced);
    }
  }
  if (emit_lines.empty()) {
    emit_first_interlaced = 0;
    emit_last_interlaced = 0;
  }

  size_t window_start = 0;
  size_t window_end = spl;
  switch (options.window) {
    case VectorscopeSampleWindow::BurstOnly:
      window_start = burst_start;
      window_end = burst_end;
      break;
    case VectorscopeSampleWindow::ActiveLine:
      window_start = active_start;
      window_end = active_end;
      break;
    case VectorscopeSampleWindow::WholeLine:
      break;
  }
  if (window_end <= window_start) {
    window_start = 0;
    window_end = spl;
  }

  const size_t line_count = emit_lines.size();
  const size_t window_width = window_end - window_start;
  const size_t estimated = line_count * window_width;
  size_t stride = 1;
  if (options.max_samples > 0 && estimated > options.max_samples) {
    stride = (estimated + options.max_samples - 1) / options.max_samples;
  }

  VectorscopeData data;
  data.field_number = field_number;
  data.system = system;
  data.cvbs_white = parameters.white_level;
  data.cvbs_blanking = parameters.blanking_level;
  data.acquisition_mode = VectorscopeAcquisitionMode::CompositeCarrier;
  data.sample_window = options.window;
  data.first_line = static_cast<uint32_t>(emit_first_interlaced);
  data.last_line = static_cast<uint32_t>(emit_last_interlaced);
  data.sample_stride = static_cast<uint32_t>(stride);
  data.width = static_cast<uint32_t>((window_width + stride - 1) / stride);
  data.height = static_cast<uint32_t>(line_count);
  data.samples.reserve((estimated / stride) + line_count);

  const double display_scale = 32767.0 / level_range;
  auto clamp_display = [](double value) {
    return std::clamp(value, -32767.0, 32767.0);
  };

  double picture_amplitude_sum = 0.0;
  size_t picture_sample_count = 0;

  for (size_t line : emit_lines) {
    size_t offset = 0;
    size_t length = 0;
    if (!line_span(line, offset, length)) continue;

    const size_t emit_end = std::min(window_end, length);
    if (emit_end <= window_start) continue;

    demodulator.begin_line(frame + offset, length,
                           make_line_reference(cycles_per_line, line),
                           window_start, emit_end);
    const size_t interlaced_line = to_interlaced(line);
    const uint8_t field_id = (line < f1_lines) ? 0 : 1;
    const VectorscopeLinePhase phase = line_phases[line];
    // The reference is a property of the line, so it is looked up once here
    // rather than once per sample: this loop runs for every sample of every
    // line of the frame.
    const double cosine = reference_cos[line];
    const double sine = reference_sin[line];

    for (size_t i = window_start; i < emit_end; i += stride) {
      double p = 0.0;
      double q = 0.0;
      demodulator.at(i, p, q);
      const double u = -((p * cosine) + (q * sine));
      const double v = (p * sine) - (q * cosine);

      VectorscopeSampleClass sample_class = VectorscopeSampleClass::Blanking;
      if (i >= active_start && i < active_end) {
        sample_class = VectorscopeSampleClass::Picture;
        // std::sqrt of the sum of squares rather than std::hypot: the guard
        // against intermediate overflow that hypot pays for on every call is
        // worth nothing on values already clamped to the display range, and
        // this runs once per emitted sample.
        picture_amplitude_sum += std::sqrt((u * u) + (v * v));
        ++picture_sample_count;
      } else if (i >= burst_start && i < burst_end) {
        sample_class = VectorscopeSampleClass::Burst;
      }

      data.samples.emplace_back(clamp_display(u * display_scale),
                                clamp_display(v * display_scale), field_id,
                                sample_class, phase,
                                static_cast<uint16_t>(interlaced_line));
    }
  }

  // ------------------------------------------------------------------
  // Instrument readouts.
  // ------------------------------------------------------------------
  VectorscopeMeasurements& measurements = data.measurements;
  measurements.burst_line_count = static_cast<uint32_t>(reference_lines.size());

  if (locked) {
    measurements.valid = true;

    double magnitude_sum = 0.0;
    // Per-V-switch-group phase statistics; a single group for NTSC.
    std::vector<double> group_sin[2];
    std::vector<double> group_cos[2];
    std::vector<double> group_angle[2];

    for (size_t line : reference_lines) {
      const LineBurst& burst = bursts[line];
      magnitude_sum += burst.magnitude;
      double u = 0.0;
      double v = 0.0;
      to_uv(line, burst.p, burst.q, u, v);
      const double angle = std::atan2(v, u);
      const size_t group =
          (is_pal_switched &&
           line_phases[line] == VectorscopeLinePhase::VNegative)
              ? 1u
              : 0u;
      group_sin[group].push_back(std::sin(angle));
      group_cos[group].push_back(std::cos(angle));
      group_angle[group].push_back(angle);
    }

    const double mean_magnitude =
        magnitude_sum / static_cast<double>(reference_lines.size());
    measurements.burst_amplitude_ire = (mean_magnitude / level_range) * 100.0;
    const double nominal_ire = nominal_burst_amplitude_ire(system);
    measurements.burst_amplitude_percent =
        (nominal_ire > 0.0)
            ? (measurements.burst_amplitude_ire / nominal_ire) * 100.0
            : 0.0;

    double squared_error_sum = 0.0;
    size_t phase_sample_count = 0;
    double group_mean[2] = {0.0, 0.0};
    bool group_used[2] = {false, false};
    for (size_t group = 0; group < 2; ++group) {
      if (group_angle[group].empty()) continue;
      group_used[group] = true;
      group_mean[group] = mean_angle(group_sin[group], group_cos[group]);
      for (double angle : group_angle[group]) {
        const double error = wrap_to_pi(angle - group_mean[group]);
        squared_error_sum += error * error;
        ++phase_sample_count;
      }
    }
    if (phase_sample_count > 0) {
      measurements.burst_phase_jitter_degrees =
          std::sqrt(squared_error_sum /
                    static_cast<double>(phase_sample_count)) *
          kRadiansToDegrees;
    }

    if (is_pal_switched && group_used[0] && group_used[1]) {
      // ITU-R BT.470-6 Table 2 item 2.16: the two burst vectors sit at
      // 135° and 225°, i.e. ±45° about the −U axis.
      const double separation =
          std::abs(wrap_to_pi(group_mean[0] - group_mean[1]));
      measurements.burst_phase_split_error_degrees =
          ((separation * 0.5) * kRadiansToDegrees) - 45.0;
    }

    if (picture_sample_count > 0 && mean_magnitude > 0.0) {
      measurements.chroma_to_burst_ratio =
          (picture_amplitude_sum / static_cast<double>(picture_sample_count)) /
          mean_magnitude;
    }
  }

  ORC_LOG_DEBUG(
      "Composite vectorscope: frame item {} lines {}..{} window {} stride {} "
      "→ {} samples; burst locked={} lines={} amplitude={:.2f} IRE "
      "jitter={:.2f}°",
      field_number, data.first_line, data.last_line,
      static_cast<int>(options.window), data.sample_stride, data.samples.size(),
      locked, measurements.burst_line_count, measurements.burst_amplitude_ire,
      measurements.burst_phase_jitter_degrees);

  return data;
}

std::optional<VectorscopeData> extract_composite_vectorscope(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    const CompositeVectorscopeOptions& options) {
  auto parameters = representation.get_video_parameters();
  if (!parameters.has_value() || !parameters->is_valid()) {
    return std::nullopt;
  }

  // For a Y/C source the chroma is already separated; demodulate that channel
  // rather than a composite that was never transmitted.  Its DC pedestal
  // (SourceParameters::chroma_dc_offset) needs no removal: the product
  // detector's one-cycle boxcar is an exact null for DC.
  const int16_t* frame = representation.has_separate_channels()
                             ? representation.get_frame_chroma(frame_id)
                             : representation.get_frame(frame_id);
  if (frame == nullptr) {
    return std::nullopt;
  }

  const size_t frame_samples = frame_line_sample_offset(
      parameters->system, static_cast<size_t>(parameters->frame_width_nominal),
      static_cast<size_t>(parameters->frame_height));

  return extract_composite_vectorscope(frame, frame_samples, *parameters,
                                       static_cast<uint64_t>(frame_id),
                                       options);
}

}  // namespace orc
