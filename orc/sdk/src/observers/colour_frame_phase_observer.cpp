/*
 * File:        colour_frame_phase_observer.cpp
 * Module:      orc-sdk-support
 * Purpose:     Per-field colour-sequence phase observer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_line_util.h>
#include <orc/support/logging.h>
#include <orc/support/observers/colour_frame_phase_observer.h>
#include <orc/support/secam_line_reference.h>

#include <cmath>
#include <utility>

namespace orc {

namespace {

// Number of burst samples to demodulate per measurement (10 subcarrier cycles).
constexpr int kBurstCount = 40;

// Amplitude below which burst is considered absent (far below spec minimum of
// ~50 ADU; catches blank tape without false-triggering on normal content).
constexpr double kMinBurstAmplitude = 20.0;

// I/Q demodulate kBurstCount samples starting at abs_offset and return the
// carrier angle in [0, 360). Returns -1.0 when amplitude < kMinBurstAmplitude.
// Optional outputs receive raw amplitude, I, and Q regardless of threshold.
double measure_burst_angle(const int16_t* frame_data, size_t abs_offset,
                           int32_t blanking_10bit,
                           double* out_amplitude = nullptr,
                           double* out_I = nullptr, double* out_Q = nullptr) {
  const int phase_base = static_cast<int>(abs_offset % 4);
  const int16_t* burst_ptr = frame_data + abs_offset;

  // Quadrature demodulation at 4FSC: cos/sin values cycle
  // {1,0,-1,0}/{0,1,0,-1}.
  double I = 0.0, Q = 0.0;
  for (int n = 0; n < kBurstCount; ++n) {
    const double ac = static_cast<double>(burst_ptr[n]) - blanking_10bit;
    switch ((phase_base + n) % 4) {
      case 0:
        I += ac;
        break;
      case 1:
        Q += ac;
        break;
      case 2:
        I -= ac;
        break;
      case 3:
        Q -= ac;
        break;
      default:
        break;
    }
  }

  if (out_I) *out_I = I;
  if (out_Q) *out_Q = Q;
  const double amplitude = std::sqrt(I * I + Q * Q);
  if (out_amplitude) *out_amplitude = amplitude;
  if (amplitude < kMinBurstAmplitude) return -1.0;

  double angle_deg = std::atan2(Q, I) * (180.0 / M_PI);
  if (angle_deg < 0.0) angle_deg += 360.0;
  return angle_deg;
}

// Measure burst amplitude at frame-flat line |flat_line|.
double line_burst_amplitude(const int16_t* data, VideoSystem sys,
                            size_t spl_nom, size_t burst_start,
                            int32_t blanking, size_t flat_line) {
  const size_t off =
      frame_line_sample_offset(sys, spl_nom, flat_line) + burst_start;
  double amp = 0.0;
  measure_burst_angle(data, off, blanking, &amp);
  return amp;
}

// Measure the Q component and amplitude at frame-flat line |flat_line|.
// Q > 0 indicates a "rising" burst (positive-going first zero crossing),
// matching ld-decode's compute_line_bursts() rising determination.
std::pair<double, double> line_burst_Q(const int16_t* data, VideoSystem sys,
                                       size_t spl_nom, size_t burst_start,
                                       int32_t blanking, size_t flat_line) {
  const size_t off =
      frame_line_sample_offset(sys, spl_nom, flat_line) + burst_start;
  double amp = 0.0, Q = 0.0;
  measure_burst_angle(data, off, blanking, &amp, nullptr, &Q);
  return {amp, Q};
}

// Field lines 0-9 hold vertical sync and the burst-blanking sequence, and the
// last few lines of each field fall in the next blanking sequence, so neither
// carries a reliable burst; field_minus_u_angle() reads the lines in between.
constexpr size_t kFirstSteadyBurstFieldLine = 10;
constexpr size_t kSteadyBurstTailLines = 8;

// A clean field's unit burst vectors sum to cos 45° ≈ 0.7 of the line count
// (the ±45° swing cancels); anything far below that is not a coherent PAL or
// PAL_M burst sequence.
constexpr double kMinBurstCoherence = 0.35;

// Wrap an angle in degrees into (-180, 180].
double wrap_degrees(double deg) {
  deg = std::fmod(deg, 360.0);
  if (deg > 180.0) deg -= 360.0;
  if (deg <= -180.0) deg += 360.0;
  return deg;
}

// Angle of the -U axis, in [0, 360), against the frame's own 4FSC sample grid,
// for the field whose first frame-flat line is |flat_start|.
//
// PAL and PAL_M swing the burst ±45° about -U line by line (the V switch), so
// summing the burst vectors of consecutive lines cancels the swing and leaves
// -U.  Unit vectors, so a dropout or a hot line cannot outvote the rest.
// Returns -1.0 when the field carries no coherent burst.
//
// The 4FSC samples are locked to the subcarrier, so the angle is one value for
// the whole frame: the two fields of a frame read the same -U.
double field_minus_u_angle(const int16_t* data, VideoSystem sys, size_t spl_nom,
                           size_t burst_start, int32_t blanking,
                           size_t flat_start, size_t field_lines) {
  double sum_I = 0.0, sum_Q = 0.0;
  int counted = 0;
  for (size_t fl = kFirstSteadyBurstFieldLine;
       fl + kSteadyBurstTailLines < field_lines; ++fl) {
    double amp = 0.0, I = 0.0, Q = 0.0;
    measure_burst_angle(
        data,
        frame_line_sample_offset(sys, spl_nom, flat_start + fl) + burst_start,
        blanking, &amp, &I, &Q);
    if (amp < kMinBurstAmplitude) continue;
    sum_I += I / amp;
    sum_Q += Q / amp;
    ++counted;
  }
  if (counted == 0) return -1.0;
  if (std::hypot(sum_I, sum_Q) < kMinBurstCoherence * counted) return -1.0;

  double angle_deg = std::atan2(sum_Q, sum_I) * (180.0 / M_PI);
  if (angle_deg < 0.0) angle_deg += 360.0;
  return angle_deg;
}

// ----------------------------------------------------------------------------
// Where -U falls in each frame of the 4-frame (8-field) sequence.
//
// EBU Tech. 3280-E §1.1.1 and Fig. 1-2: the 4FSC samples fall at 45°, 135°,
// 225° and 315° of the +U axis, so the burst (+U ±135°) is sampled on the
// demodulator's own axes and -U sits midway between them.  The CVBS File
// Format Specification puts PAL_M on the same sampling phase.
//
// A frame is 709,379 samples for PAL (≡ 3 mod 4) and 477,225 for PAL_M
// (≡ 1 mod 4), so the subcarrier is a quarter cycle on, one way or the other,
// at the next frame start, and against each frame's own grid -U steps 90° a
// frame: +90° for PAL, -90° for PAL_M.  In the frames ld-decode numbers
// fields 1/2, 3/4, 5/6 and 7/8 that puts -U at 225°, 315°, 45° and 135° for
// PAL, and at 225°, 135°, 45° and 315° for PAL_M.  Measured, the PAL angles
// hold for ld-decode's own field_phase_id on pal-basic.tbc, pal-parallel.tbc
// and pal-cut-decoded.tbc (all through the 4FSC lattice of
// orc/support/pal_line_locked_resample.h), on the native .cvbs of the same
// pal-cut decode, and for videosynth's standard-phase PAL, where frame 1 of
// the CVBS specification's sequence reads 1/2.  Both hold for videosynth's
// standard-phase PAL_M.  Real captures sit within about 30° of these angles.
// ----------------------------------------------------------------------------
constexpr double kMinusUFrame1Deg = 225.0;
constexpr double kPalMinusUStepPerFrameDeg = 90.0;
constexpr double kPalMMinusUStepPerFrameDeg = -90.0;

// The PAL 8-field half is decided by which of two -U angles 180° apart the
// field is nearer.  A field within this many degrees of the midway point is
// left unknown: that is where a decode whose line starts sit one 4FSC sample
// from the standard origin lands, and either answer would be a guess.
constexpr double kPalHalfDecisionMarginDeg = 20.0;

// ============================================================================
// PAL per-field phase ID (1-8).
//
// EBU Tech. 3280-E §1.1.1.
// Numbered as ld-decode FieldPAL.determine_field_number() numbers them
// (lddecode/core.py:3004), and steps 1-2 mirror it.
//
// is_ld_first: whether this field is ld-decode isFirstField=true. ld-decode
// always writes the isFirstField=True field first in the TBC, so VFR field 1
// → is_ld_first=true and VFR field 2 → is_ld_first=false for all systems.
//
// Line numbering: ld-decode's scale_field() writes TBC line 0 from
// linelocs[lineoffset + 1] while lineslice(l) reads linelocs[l + lineoffset],
// so the per-field lineoffset cancels: TBC line = ld-decode field line - 1
// for BOTH fields.
// ============================================================================
int pal_field_phase_id(const int16_t* data, VideoSystem sys, size_t spl_nom,
                       size_t burst_start, int32_t blanking, size_t flat_start,
                       size_t field_lines, bool is_ld_first) {
  // Reference burst amplitude: ld-decode uses burstmedian across all active
  // lines; we approximate with a fixed VBI line that always carries burst.
  // ld-decode field line 20 → TBC frame-flat line 19.
  const double ref_amp = line_burst_amplitude(data, sys, spl_nom, burst_start,
                                              blanking, flat_start + 19);
  if (ref_amp < kMinBurstAmplitude) return -1;

  // Step 1: detect burst presence/absence on ld-decode field line 6.
  // EBU Tech. 3280-E: the colour burst is gated off on this line in certain
  // field positions, enabling 4-field identification.
  // ld-decode field line 6 → TBC frame-flat line 5.
  const double amp6 = line_burst_amplitude(data, sys, spl_nom, burst_start,
                                           blanking, flat_start + 5);
  bool hasburst;
  if (amp6 >= ref_amp * 0.8) {
    hasburst = true;
  } else if (amp6 < ref_amp * 0.2) {
    hasburst = false;
  } else {
    // Ambiguous level (e.g. interference or dropout on gate line).
    return -1;
  }

  // Step 2: map (isFirstField, hasBurst) to the 4-field position m4.
  // ld-decode map4: {(T,F):1, (F,T):2, (T,T):3, (F,F):4}.
  // The burst-blanking sequence repeats every 4 fields, so m4 fixes the
  // frame's parity in the 4-frame sequence: fields 1/2 or 5/6 for m4 1-2,
  // fields 3/4 or 7/8 for m4 3-4.  It reads amplitudes only, so it does not
  // depend on where the samples fall.
  int m4;
  if (is_ld_first && !hasburst) {
    m4 = 1;
  } else if (!is_ld_first && hasburst) {
    m4 = 2;
  } else if (is_ld_first && hasburst) {
    m4 = 3;
  } else {
    m4 = 4;  // !is_ld_first && !hasburst
  }
  const int parity = (m4 <= 2) ? 0 : 1;

  // Step 3: pick between the two frames of that parity (positions |parity|
  // and |parity| + 2), whose -U angles lie 180° apart, by the nearer one.
  //
  // ld-decode decides this from a rising/falling burst vote.  Reading that
  // vote as the sign of Q against the sample grid fails at the standard
  // sampling phase: there the burst lies on the demodulator's axes, and in
  // half the fields on the Q = 0 axis, so the vote became a coin toss
  // (issue #380).  Comparing the field's -U angle with the expected ones
  // leaves ±90° of margin for the same decision.
  const double minus_u = field_minus_u_angle(data, sys, spl_nom, burst_start,
                                             blanking, flat_start, field_lines);
  if (minus_u < 0.0) return -1;
  const double offset = std::fabs(wrap_degrees(
      minus_u - (kMinusUFrame1Deg + kPalMinusUStepPerFrameDeg * parity)));
  int frame_pos;
  if (offset < 90.0 - kPalHalfDecisionMarginDeg) {
    frame_pos = parity;
  } else if (offset > 90.0 + kPalHalfDecisionMarginDeg) {
    frame_pos = parity + 2;
  } else {
    return -1;
  }
  return frame_pos * 2 + (is_ld_first ? 1 : 2);
}

// ============================================================================
// PAL_M per-field phase ID (1-8).
//
// ITU-R BT.1700-1 Annex 1 Part B. fsc = 227.25 fH, so at 4FSC a 909-sample
// line is 227.25 subcarrier cycles and a 525-line frame is 119306.25: the
// subcarrier steps 90 degrees per frame against the frame's own sample grid
// and the 8-field sequence is 4 frames long. The two fields of a frame share
// that step.
//
// PAL's method does not carry over. Its burst-blanking test reads field line
// 6, which on 525 lines is in vertical sync and never carries burst, so the
// frame's position comes from the -U angle alone (field_minus_u_angle()),
// classified to the nearest of the four expected angles.  The angle is taken
// in the frame buffer's own sample grid, so this is only comparable within one
// source; a decode one 4FSC sample from another shifts every id by one frame.
// See colour_frame_phase_query.h.
// ============================================================================
int pal_m_field_phase_id(const int16_t* data, VideoSystem sys, size_t spl_nom,
                         size_t burst_start, int32_t blanking,
                         size_t flat_start, size_t field_lines,
                         bool is_ld_first) {
  const double minus_u = field_minus_u_angle(data, sys, spl_nom, burst_start,
                                             blanking, flat_start, field_lines);
  if (minus_u < 0.0) return -1;

  // The -U axis falls at 225, 135, 45 and 315 degrees in successive frames.
  // Frame 0 at 225 degrees takes ids 1/2, matching PAL's first frame.
  const int frame_pos =
      static_cast<int>(std::lround((minus_u - kMinusUFrame1Deg) /
                                   kPalMMinusUStepPerFrameDeg) +
                       4) %
      4;
  return frame_pos * 2 + (is_ld_first ? 1 : 2);
}

// ============================================================================
// NTSC per-field phase ID (1-4).
//
// SMPTE 244M-2003 §3.2; SMPTE 170M-2004 §13.2.
// Counts the burst vote as ld-decode FieldNTSC.compute_burst_offsets()
// (lddecode/core.py:3236) does.
//
// is_ld_first: VFR field 1 (top) → true, VFR field 2 (bottom) → false.
//
// The frame is on the CVBS File Format Specification v1.8.1 NTSC line origin
// (0H 57/90 of a sample before stored sample 0), so the stored grid fixes the
// colour frame: ids 1/2 are colour frame A, 3/4 colour frame B.  SMPTE
// 244M-2003 §4.1.2: in colour frame A, stored sample 125 of stored frame line 9
// is a +I (123°) sample.
//
// ld-decode counts the same vote on its own NTSC .tbc origin, two samples
// (180° of subcarrier) later, where colour frame A reads as rising on even
// lines of field 1.  tbc_source moves those frames onto the specification's
// origin (common/tbc-format/ntsc_tbc_origin.h), and there the same burst reads
// falling, so field14 is the falling-burst majority here.  The ids then equal
// ld-decode's own fieldPhaseID for its frames, and TBC Sink writes them back
// unchanged with the samples restored to ld-decode's origin.
//
// The NTSC comb's getLinePhase() (sinks/common/decoders/comb.cpp) reads the
// ids against the same stored grid.
// ============================================================================
int ntsc_field_phase_id(const int16_t* data, VideoSystem sys, size_t spl_nom,
                        size_t burst_start, int32_t blanking, size_t flat_start,
                        size_t field_lines, bool is_ld_first) {
  // Count rising bursts on even field-relative lines, starting past the
  // vertical sync area (lines 0-7 have no burst).
  // ld-decode: rising_sum counts even lines; threshold = total_analyzed / 4.
  // Here total_analyzed ≈ 2 × even_count, so the equivalent threshold is
  // even_count / 2.
  int rising_sum = 0, even_count = 0;
  for (size_t fl = 8; fl < field_lines; fl += 2) {
    auto [amp, Q] = line_burst_Q(data, sys, spl_nom, burst_start, blanking,
                                 flat_start + fl);
    if (amp >= kMinBurstAmplitude) {
      rising_sum += (Q > 0.0) ? 1 : 0;
      ++even_count;
    }
  }

  if (even_count == 0) return -1;

  // Rising on the majority of even lines, against the field's own grid.
  bool rising = rising_sum > (even_count / 2);

  // measure_burst_angle() derives its 4FSC I/Q reference from the frame-flat
  // sample offset. Field 2 starts at flat_start * spl_nom samples; when that
  // is ≡ 2 (mod 4) the demodulation reference is rotated 180° relative to
  // field 1 (NTSC: 263 lines × 910 samples ≡ 2 mod 4), inverting the Q sign
  // and thus the rising determination. Compensate so the vote is taken
  // against the field's own start, as ld-decode takes it.
  if ((flat_start * spl_nom) % 4 == 2) rising = !rising;

  // ld-decode's field14 (fields 1 and 4) is a rising vote on its own origin,
  // two samples later than this frame's: a falling vote here.
  const bool field14 = !rising;

  // ld-decode NTSC map4: {(isFirstField, field14): fieldPhaseID}.
  if (is_ld_first && field14) return 1;
  if (!is_ld_first && !field14) return 2;
  if (is_ld_first && !field14) return 3;
  return 4;  // !is_ld_first && field14
}

// ============================================================================
// SECAM line identification and 2-frame sequence.
//
// CVBS File Format Specification v1.8.1, Preset SECAM, "SECAM 2-frame
// sequence": D'R and D'B lines alternate continuously, and 625 is odd, so the
// assignment of a line number inverts each frame.  In sequence frame 1 the
// even stored frame lines (odd broadcast lines) carry D'R; in frame 2 they
// carry D'B.  ITU-R BT.1700-1 Annex 1 Part C Fig. 16: frame 1 holds fields 1
// and 2 of the 4-field sequence, where broadcast line 7 carries D'R.
//
// Each line is identified as BT.1700 Part C Table 4 item 13 describes:
//  - primarily from the undeviated sub-carrier reference on the line-blanking
//    back porch, fOR = 282 fH on a D'R line and fOB = 272 fH on a D'B line
//    (Table 4 item 5), the method item 13 prefers;
//  - cross-checked against the field identification signals on lines 7-15
//    and 320-328, where present (Fig. 17 trapezoids).
//
// This is a frequency measurement on each line, not a phase measurement
// against the sample lattice, so the result is the same on any source, phase
// locked or not, and it is comparable across sources.  The specification
// forbids a locked SECAM source, and Phase 4a's lock gating (ids unknown on an
// unlocked source) does not apply here.
// ============================================================================

// Stored frame lines in the 625-line flat buffer: field 1 holds broadcast
// lines 1-313 at stored lines 0-312, field 2 broadcast lines 314-625 at
// 313-624, so stored line k is broadcast line k + 1 in both fields.

// The back-porch reference window and its reading: secam_line_reference.h.

// BT.1700 Part C Fig. 17: an identification trapezoid starts at the rest
// frequency at the end of line blanking (10.5 us) and settles 15 us (D'R) or
// 18 us (D'B, up to 30 us) later.  Ramp start is read from 11 us to 13 us, the
// settled level from 42 us to 58 us, after 0H.  These are the windows Video
// Sink's secam decoder reads.
constexpr double kSecamIdRampStartUs = 11.0;
constexpr double kSecamIdRampEndUs = 13.0;
constexpr double kSecamIdSettledStartUs = 42.0;
constexpr double kSecamIdSettledEndUs = 58.0;

// Field lines (0-based in the field) carrying the identification signals:
// broadcast lines 7-15 in field 1 (stored 6-14) and 320-328 in field 2
// (stored 319-327, field lines 6-14).
constexpr size_t kSecamIdFirstFieldLine = 6;
constexpr size_t kSecamIdLastFieldLine = 14;

// The settled identification deviation, nominal +350 kHz (D'R) / -350 kHz
// (D'B) from rest (BT.470-6 item 2.18), must be 200-500 kHz from rest in the
// line type's direction, and the ramp must start within 120 kHz of rest, so
// picture or data content on those lines cannot pose as identification.
constexpr double kSecamIdRestToleranceHz = 120.0e3;
constexpr double kSecamIdMinimumSwingHz = 200.0e3;
constexpr double kSecamIdMaximumSwingHz = 500.0e3;

// BT.1700 Part C Table 4 item 10d: the sub-carrier peak-to-peak amplitude 2 M0
// is 23 % of the luminance amplitude.  A line counts when its reference reads
// at least a fifth of M0 (Video Sink's secam decoder uses the same colour-kill
// fraction).
constexpr double kSecamM0PerLumaAmplitude = 0.23 / 2.0;
constexpr double kSecamMinimumFractionOfM0 = 0.2;

// A field needs this many reference lines, or identification lines, and this
// fraction of them agreeing, to decide.  A PAL burst near fOR on every line
// does not alternate and fails the agreement test.
constexpr int kSecamMinimumReferenceLines = 8;
constexpr int kSecamMinimumIdLines = 3;
constexpr double kSecamMinimumAgreement = 0.6;

// Accumulates per-line votes on whether the even stored frame lines of a
// field carry D'R.
struct SecamParityVote {
  double score = 0.0;
  int lines = 0;

  void add(size_t stored_line, bool carries_dr) {
    const bool even = (stored_line % 2) == 0;
    score += (carries_dr == even) ? 1.0 : -1.0;
    ++lines;
  }

  // +1 when the even stored lines carry D'R, -1 when they carry D'B, 0 when
  // too few lines voted or they do not agree.
  int decide(int minimum_lines) const {
    if (lines < minimum_lines) return 0;
    if (std::fabs(score) < kSecamMinimumAgreement * lines) return 0;
    return score > 0.0 ? 1 : -1;
  }
};

// The SECAM line sequence of one field: +1 when its even stored frame lines
// carry D'R, -1 when they carry D'B, 0 when it cannot be decided or the two
// identification methods disagree.
int secam_field_sequence(const int16_t* data, size_t spl_nom, size_t flat_start,
                         size_t field_lines, double minimum_amplitude,
                         double sample_rate_hz) {
  const auto us_to_sample = [&](double us) {
    return static_cast<size_t>(std::lround(us * 1.0e-6 * sample_rate_hz));
  };
  const size_t ramp_first = us_to_sample(kSecamIdRampStartUs);
  const size_t ramp_end = us_to_sample(kSecamIdRampEndUs);
  const size_t settled_first = us_to_sample(kSecamIdSettledStartUs);
  const size_t settled_end =
      std::min(us_to_sample(kSecamIdSettledEndUs), spl_nom);

  SecamParityVote reference;
  SecamParityVote identification;
  for (size_t fl = 0; fl < field_lines; ++fl) {
    const size_t stored = flat_start + fl;
    const int16_t* line =
        data + frame_line_sample_offset(VideoSystem::SECAM, spl_nom, stored);

    const SecamLineReading ref = secam_line_frequency(
        line, kSecamReferenceFirst, kSecamReferenceEnd, sample_rate_hz);
    if (ref.amplitude >= minimum_amplitude) {
      const int type = secam_reference_line_type(ref);
      if (type != 0) reference.add(stored, type > 0);
    }

    if (fl < kSecamIdFirstFieldLine || fl > kSecamIdLastFieldLine) continue;
    const SecamLineReading ramp =
        secam_line_frequency(line, ramp_first, ramp_end, sample_rate_hz);
    const SecamLineReading settled =
        secam_line_frequency(line, settled_first, settled_end, sample_rate_hz);
    if (settled.amplitude < minimum_amplitude) continue;
    if (std::fabs(ramp.frequency_hz - kSecamFor) <= kSecamIdRestToleranceHz) {
      const double swing = settled.frequency_hz - kSecamFor;
      if (swing >= kSecamIdMinimumSwingHz && swing <= kSecamIdMaximumSwingHz) {
        identification.add(stored, true);
      }
    } else if (std::fabs(ramp.frequency_hz - kSecamFob) <=
               kSecamIdRestToleranceHz) {
      const double swing = kSecamFob - settled.frequency_hz;
      if (swing >= kSecamIdMinimumSwingHz && swing <= kSecamIdMaximumSwingHz) {
        identification.add(stored, false);
      }
    }
  }

  const int by_reference = reference.decide(kSecamMinimumReferenceLines);
  const int by_identification = identification.decide(kSecamMinimumIdLines);
  if (by_reference != 0 && by_identification != 0 &&
      by_reference != by_identification) {
    return 0;
  }
  return by_reference != 0 ? by_reference : by_identification;
}

// Convert a field_phase_id to a colour_frame_index.
// PAL/PAL_M: phase pairs {1,2}→1, {3,4}→2, {5,6}→3, {7,8}→4.
// SECAM:     phase pairs {1,2}→1, {3,4}→2.
// NTSC:      phase pairs {1,2}→0, {3,4}→1.
// Returns -1 for absent/unknown phase.
int32_t phase_to_colour_index(int32_t phase_id, VideoSystem sys) {
  if (phase_id < 1) return -1;
  if (sys == VideoSystem::NTSC) return (phase_id - 1) / 2;
  return (phase_id - 1) / 2 + 1;
}

// Records the SECAM sequence position of both fields of |frame_id|.
//
// field_phase_id: fields 1/2 of sequence frame 1 are 1/2, of frame 2 3/4.
// colour_frame_index: the sequence frame, 1 or 2.  A frame whose two fields
// come from different sequence frames (an edit, or a mis-paired field) keeps
// each field's own field_phase_id, and both fields' colour_frame_index is -1:
// the frame as a whole has no sequence position.
void process_secam_frame(const VideoFrameRepresentation& representation,
                         const SourceParameters& vp, FrameID frame_id,
                         IObservationContext& context) {
  const bool is_yc = representation.has_separate_channels();
  const int16_t* frame_data = is_yc ? representation.get_frame_chroma(frame_id)
                                    : representation.get_frame(frame_id);
  int32_t phase[2] = {-1, -1};
  if (frame_data && vp.white_level > vp.blanking_level) {
    const double minimum_amplitude =
        kSecamMinimumFractionOfM0 * kSecamM0PerLumaAmplitude *
        static_cast<double>(vp.white_level - vp.blanking_level);
    const size_t spl_nom = static_cast<size_t>(kSecamSamplesPerLineNominal);
    const size_t f1_lines = field1_lines(VideoSystem::SECAM);
    const size_t f2_lines = static_cast<size_t>(kSecamFrameLines) - f1_lines;
    const int seq[2] = {
        secam_field_sequence(frame_data, spl_nom, 0, f1_lines,
                             minimum_amplitude, kSecamSampleRate),
        secam_field_sequence(frame_data, spl_nom, f1_lines, f2_lines,
                             minimum_amplitude, kSecamSampleRate)};
    for (int f = 0; f < 2; ++f) {
      if (seq[f] == 0) continue;
      const int sequence_frame = seq[f] > 0 ? 1 : 2;
      phase[f] = (sequence_frame - 1) * 2 + f + 1;
    }
  } else if (!frame_data) {
    ORC_LOG_TRACE("ColourFramePhaseObserver: No {} data for frame {}",
                  is_yc ? "chroma" : "frame", frame_id);
  }

  int32_t index[2] = {phase_to_colour_index(phase[0], VideoSystem::SECAM),
                      phase_to_colour_index(phase[1], VideoSystem::SECAM)};
  if (index[0] > 0 && index[1] > 0 && index[0] != index[1]) {
    index[0] = -1;
    index[1] = -1;
  }
  for (int f = 0; f < 2; ++f) {
    const FieldID fid(frame_id * 2 + static_cast<FrameID>(f));
    context.set(fid, "colour_frame_phase", "field_phase_id", phase[f]);
    context.set(fid, "colour_frame_phase", "colour_frame_index", index[f]);
  }
  ORC_LOG_DEBUG(
      "ColourFramePhaseObserver: SECAM frame {} f1_phase={} f2_phase={} "
      "(colour={})",
      frame_id, phase[0], phase[1], index[0]);
}

}  // namespace

void ColourFramePhaseObserver::process_frame(
    const VideoFrameRepresentation& representation, FrameID frame_id,
    IObservationContext& context) {
  auto vp_opt = representation.get_video_parameters();
  if (!vp_opt.has_value()) {
    ORC_LOG_TRACE("ColourFramePhaseObserver: No video parameters for frame {}",
                  frame_id);
    return;
  }
  const auto& vp = vp_opt.value();

  // SECAM identifies its sequence by frequency, not phase, so it is measured
  // before, and regardless of, the lock gate below (see secam_field_sequence).
  if (vp.system == VideoSystem::SECAM) {
    process_secam_frame(representation, vp, frame_id, context);
    return;
  }

  // CVBS file format spec v1.8.1 (signal-state-presets.md, "Subcarrier phase
  // analysis"): a sample's subcarrier phase can be computed from its position
  // only when the source is phase locked.  On an unlocked source the burst
  // at fixed sample positions still gives a regular sequence, but it says
  // nothing about the colour sequence, so both fields are reported unknown.
  if (!vp.is_subcarrier_locked) {
    for (const FrameID f : {frame_id * 2, frame_id * 2 + 1}) {
      context.set(FieldID(f), "colour_frame_phase", "field_phase_id",
                  int32_t{-1});
      context.set(FieldID(f), "colour_frame_phase", "colour_frame_index",
                  int32_t{-1});
    }
    ORC_LOG_TRACE(
        "ColourFramePhaseObserver: Frame {} source is not subcarrier phase "
        "locked; ids unknown",
        frame_id);
    return;
  }

  const bool is_yc = representation.has_separate_channels();
  const int16_t* frame_data = is_yc ? representation.get_frame_chroma(frame_id)
                                    : representation.get_frame(frame_id);
  if (!frame_data) {
    ORC_LOG_TRACE("ColourFramePhaseObserver: No {} data for frame {}",
                  is_yc ? "chroma" : "frame", frame_id);
    return;
  }

  const int32_t blanking =
      is_yc ? (vp.chroma_dc_offset >= 0 ? vp.chroma_dc_offset : 512)
            : vp.blanking_level;

  size_t spl_nom = 0;
  size_t burst_start = 0;
  switch (vp.system) {
    case VideoSystem::PAL:
      spl_nom = static_cast<size_t>(kPalSamplesPerLineNominal);
      burst_start = static_cast<size_t>(kPalColourBurstStart);
      break;
    case VideoSystem::NTSC:
      spl_nom = static_cast<size_t>(kNtscSamplesPerLine);
      burst_start = static_cast<size_t>(kNtscColourBurstStart);
      break;
    case VideoSystem::PAL_M:
      spl_nom = static_cast<size_t>(kPalMSamplesPerLine);
      burst_start = static_cast<size_t>(kPalMColourBurstStart);
      break;
    case VideoSystem::SECAM:  // handled by process_secam_frame()
    case VideoSystem::Unknown:
      return;
  }

  const size_t f1_lines = field1_lines(vp.system);
  const size_t f2_lines =
      static_cast<size_t>(frame_lines_from_system(vp.system)) - f1_lines;

  // ld-decode always writes the isFirstField=True field first in the TBC
  // (lddecode/core.py:4088 skips a leading second field), so VFR field 1 =
  // isFirstField=True and VFR field 2 = isFirstField=False for all systems.
  const bool f1_is_ld_first = true;
  const bool f2_is_ld_first = false;

  // Compute per-field phase IDs.
  int32_t f1_phase = -1;
  int32_t f2_phase = -1;
  switch (vp.system) {
    case VideoSystem::PAL:
      f1_phase = static_cast<int32_t>(
          pal_field_phase_id(frame_data, vp.system, spl_nom, burst_start,
                             blanking, 0, f1_lines, f1_is_ld_first));
      f2_phase = static_cast<int32_t>(
          pal_field_phase_id(frame_data, vp.system, spl_nom, burst_start,
                             blanking, f1_lines, f2_lines, f2_is_ld_first));
      break;
    case VideoSystem::PAL_M:
      f1_phase = static_cast<int32_t>(
          pal_m_field_phase_id(frame_data, vp.system, spl_nom, burst_start,
                               blanking, 0, f1_lines, f1_is_ld_first));
      f2_phase = static_cast<int32_t>(
          pal_m_field_phase_id(frame_data, vp.system, spl_nom, burst_start,
                               blanking, f1_lines, f2_lines, f2_is_ld_first));
      break;
    case VideoSystem::NTSC:
      f1_phase = static_cast<int32_t>(
          ntsc_field_phase_id(frame_data, vp.system, spl_nom, burst_start,
                              blanking, 0, f1_lines, f1_is_ld_first));
      f2_phase = static_cast<int32_t>(
          ntsc_field_phase_id(frame_data, vp.system, spl_nom, burst_start,
                              blanking, f1_lines, f2_lines, f2_is_ld_first));
      break;
    case VideoSystem::SECAM:
    case VideoSystem::Unknown:
      break;
  }

  // Store both field_phase_id and colour_frame_index for each field.
  const FieldID fid1(frame_id * 2);
  context.set(fid1, "colour_frame_phase", "field_phase_id", f1_phase);
  context.set(fid1, "colour_frame_phase", "colour_frame_index",
              phase_to_colour_index(f1_phase, vp.system));

  const FieldID fid2(frame_id * 2 + 1);
  context.set(fid2, "colour_frame_phase", "field_phase_id", f2_phase);
  context.set(fid2, "colour_frame_phase", "colour_frame_index",
              phase_to_colour_index(f2_phase, vp.system));

  ORC_LOG_DEBUG(
      "ColourFramePhaseObserver: Frame {} f1_phase={} (colour={}) "
      "f2_phase={} (colour={})",
      frame_id, f1_phase, phase_to_colour_index(f1_phase, vp.system), f2_phase,
      phase_to_colour_index(f2_phase, vp.system));
}

}  // namespace orc
