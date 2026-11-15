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
// SMPTE 244M-2003 §3.2.
// Mirrors ld-decode FieldNTSC.compute_burst_offsets() (lddecode/core.py:3236).
//
// is_ld_first: VFR field 1 (top) → true, VFR field 2 (bottom) → false.
//
// The id describes the burst against the stored sample grid, in ld-decode's
// convention, not the SMPTE 170M §13.2 colour frame.  NTSC's 910 samples a
// line are 227.5 subcarrier cycles, so the two differ by where a file puts
// 0H: ld-decode's NTSC output stores 0H about 1.5 samples after the first
// stored sample, the CVBS File Format Specification just before it, and those
// two origins are two samples (180° of subcarrier) apart.  A file on the
// specification's origin therefore reads colour frame A as ids 3/4 (issue
// #380).
//
// This is deliberate.  What consumers need is the grid relationship: the NTSC
// comb filter's getLinePhase() (sinks/common/decoders/comb.cpp) and
// ld-chroma-decoder reading a TBC Sink export both demodulate against the
// stored samples, and both read ids 1/4 as a rising burst on even lines of
// that grid.  An id taken from Sc/H at 0H would be right by SMPTE 170M and
// make both comb 180° wrong on one of the two origins.
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

  // field14 = True when the majority of even lines have rising burst.
  bool field14 = rising_sum > (even_count / 2);

  // measure_burst_angle() derives its 4FSC I/Q reference from the frame-flat
  // sample offset. Field 2 starts at flat_start * spl_nom samples; when that
  // is ≡ 2 (mod 4) the demodulation reference is rotated 180° relative to
  // field 1 (NTSC: 263 lines × 910 samples ≡ 2 mod 4), inverting the Q sign
  // and thus the rising determination. Compensate so field14 matches
  // ld-decode's zero-crossing convention (validated against ld-decode
  // fieldPhaseID metadata on a real NTSC capture).
  if ((flat_start * spl_nom) % 4 == 2) field14 = !field14;

  // ld-decode NTSC map4: {(isFirstField, field14): fieldPhaseID}.
  if (is_ld_first && field14) return 1;
  if (!is_ld_first && !field14) return 2;
  if (is_ld_first && !field14) return 3;
  return 4;  // !is_ld_first && field14
}

// Convert a field_phase_id to a colour_frame_index.
// PAL/PAL_M: phase pairs {1,2}→1, {3,4}→2, {5,6}→3, {7,8}→4.
// NTSC:      phase pairs {1,2}→0, {3,4}→1.
// Returns -1 for absent/unknown phase.
int32_t phase_to_colour_index(int32_t phase_id, VideoSystem sys) {
  if (phase_id < 1) return -1;
  if (sys == VideoSystem::NTSC) return (phase_id - 1) / 2;
  return (phase_id - 1) / 2 + 1;
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
    default:
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
    default:
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
