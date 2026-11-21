/*
 * File:        cvbs_signal_constants.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Normative CVBS_U10_4FSC signal constants for PAL, NTSC,
 *              PAL_M and SECAM
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>

#include <cstdint>
#include <utility>

// This header is the single authoritative source for CVBS_U10_4FSC signal
// constants.  No file other than this header may define these numeric values.
// Stages and tests must include this header and use the named constants.

namespace orc {

// ---------------------------------------------------------------------------
// PAL — EBU Tech. 3280-E
// ---------------------------------------------------------------------------

// EBU Tech. 3280-E §1.1.1 Table 1: PAL colour subcarrier frequency.
constexpr double kPalFsc = 4'433'618.75;

// EBU Tech. 3280-E §1.1.1 Table 1: 4FSC sample rate = 4 × fsc.
constexpr double kPalSampleRate = 17'734'475.0;

// EBU Tech. 3280-E §1.2: Nominal samples per line (non-integer due to
// non-orthogonal sampling).
constexpr double kPalSamplesPerLine = 1135.0064;

// EBU Tech. 3280-E §1.2: Total samples in a complete PAL frame.
// = 625 lines × 1135 samples + 4 extra samples on non-orthogonal lines.
constexpr int32_t kPalFrameSamples = 709'379;

// EBU Tech. 3280-E §1.1.1 Table 1: Total lines in a PAL frame.
constexpr int32_t kPalFrameLines = 625;

// EBU Tech. 3280-E §1.3.2: Lines in PAL field 1 (the 313-line field; CVBS
// convention places the longer field first in the flat frame buffer).
constexpr int32_t kPalField1Lines = 313;

// EBU Tech. 3280-E §2.5.2 Table 2: First active picture line per field
// (0-based index into the field line buffer).
// PAL: lines 1–22 (1-based) carry sync equalising/broad pulses and VBI; active
// picture begins at line 23 (1-based) = index 22 (0-based).  Matches
// ld-decode convention: first_active_frame_line = 44 → firstActiveFieldLine
// = 44 / 2 = 22.
constexpr int32_t kPalFirstActiveLine = 22;

// EBU Tech. 3280-E §1.2: Nominal integer samples per line (floor of 1135.0064).
// Use this everywhere a fixed per-line width of 1135 is required (TBC storage,
// frame_line_sample_offset, display width, etc.).
constexpr int32_t kPalSamplesPerLineNominal = 1135;

// EBU Tech. 3280-E §1.2: Maximum samples on any PAL line.
// Lines 312 and 624 (0-based) each carry 2 extra samples → 1137.
constexpr int32_t kPalMaxSamplesPerLine = 1137;

// EBU Tech. 3280-E §1.1.1 Table 1: Normative CVBS_U10_4FSC signal levels
// (10-bit domain). PAL carries no setup pedestal: black = blanking (0 IRE).
constexpr int32_t kPalSyncTip = 4;
constexpr int32_t kPalBlanking = 256;
constexpr int32_t kPalBlack = 256;  // 0x100: no pedestal; black = blanking
constexpr int32_t kPalWhite = 844;
constexpr int32_t kPalPeak = 1019;

// EBU Tech. 3280-E §1.2: normative placement of the 4 extra samples per PAL
// frame.  The standard concentrates them on the last line of each field:
//   Frame-flat line 312 (0-based = line 313 1-based, last of field 1): +2
//   Frame-flat line 624 (0-based = line 625 1-based, last of field 2): +2
//
// The array holds one entry per extra sample so that frame_line_sample_offset
// and frame_line_sample_count can iterate with a simple +1-per-match rule;
// duplicate entries indicate the named line carries 2 extras.
// Verified: 623×1135 + 2×1137 = 625×1135 + 4 = 709,375 + 4 = 709,379. ✓
constexpr int32_t kPalExtraSampleLines[4] = {312, 312, 624, 624};

// ---------------------------------------------------------------------------
// NTSC — SMPTE 244M-2003
// ---------------------------------------------------------------------------

// SMPTE 244M-2003 §4.1: NTSC colour subcarrier frequency.
constexpr double kNtscFsc = 315.0e6 / 88.0;

// SMPTE 244M-2003 §4.1: 4FSC sample rate = 4 × fsc.
constexpr double kNtscSampleRate = 4.0 * kNtscFsc;

// SMPTE 244M-2003 §4.1: Samples per line (orthogonal, exact integer).
constexpr int32_t kNtscSamplesPerLine = 910;

// SMPTE 244M-2003 §4.1: Total samples in a complete NTSC frame.
// = 525 lines × 910 samples/line.
constexpr int32_t kNtscFrameSamples = 477'750;

// SMPTE 244M-2003 §3.1: Total lines in an NTSC frame.
constexpr int32_t kNtscFrameLines = 525;

// SMPTE 244M-2003 §3.2: Lines in NTSC VFR field 1 (top spatial field).
// ld-decode stores the odd-scan field (NTSC Field 1, first temporal, 263 lines)
// at even file indices (isFirstField=true); this becomes VFR field 1 (top).
// The even-scan field (262 lines) follows at odd file indices → VFR field 2.
constexpr int32_t kNtscField1Lines = 263;

// SMPTE 170M-2004 §7.1 Table 1: First active picture line per field (0-based).
// NTSC: lines 1–20 (1-based) carry sync, equalising/broad pulses, and VBI
// (closed captions, VITS, etc.); active picture begins at line 21 (1-based) =
// index 20 (0-based).  Matches ld-decode convention: first_active_frame_line
// = 40 → firstActiveFieldLine = 40 / 2 = 20.
constexpr int32_t kNtscFirstActiveLine = 20;

// SMPTE 244M-2003 §4.2.1 Table 1: Normative CVBS_U10_4FSC signal levels.
constexpr int32_t kNtscSyncTip = 16;    // 0x010: -40 IRE sync tip
constexpr int32_t kNtscBlanking = 240;  // 0x0F0: 0 IRE blanking reference
// SMPTE 170M-2004 Table 1: black (setup) = 7.5 IRE above blanking.
// 240 + 7.5 x (800 - 240) / 100 = 240 + 42 = 282.
constexpr int32_t kNtscBlack = 282;  // 0x11A: +7.5 IRE picture black
constexpr int32_t kNtscWhite = 800;  // 0x320: +100 IRE white
// SMPTE 244M-2003 §4.2.4: maximum permitted 10-bit value = 0x3FB = 1019.
constexpr int32_t kNtscPeak = 1019;  // 0x3FB: maximum legal sample value

// ---------------------------------------------------------------------------
// PAL_M — ITU-R BT.1700-1 Annex 1 Part B
// ---------------------------------------------------------------------------

// ITU-R BT.1700-1 Annex 1 Part B: PAL_M colour subcarrier frequency.
// Derived from: fsc = (909/4) × (525 × 30000/1001) Hz.
constexpr double kPalMFsc = (909.0 / 4.0) * (525.0 * 30000.0 / 1001.0);

// ITU-R BT.1700-1 Annex 1 Part B: 4FSC sample rate = 4 × fsc.
constexpr double kPalMSampleRate = 4.0 * kPalMFsc;

// ITU-R BT.1700-1 Annex 1 Part B: Samples per line (orthogonal, exact integer).
constexpr int32_t kPalMSamplesPerLine = 909;

// ITU-R BT.1700-1 Annex 1 Part B: Total samples in a complete PAL_M frame.
// = 525 lines × 909 samples/line.
constexpr int32_t kPalMFrameSamples = 477'225;

// ITU-R BT.1700-1 Annex 1 Part B: Total lines in a PAL_M frame.
constexpr int32_t kPalMFrameLines = 525;

// ITU-R BT.1700-1 Annex 1 Part B: Lines in PAL_M VFR field 1 (top spatial
// field). Identical to NTSC: ld-decode stores the odd-scan field (263 lines,
// first temporal) at even file indices → VFR field 1 (top).
constexpr int32_t kPalMField1Lines = 263;

// ITU-R BT.1700-1 Annex 1 Part B: First active picture line per field
// (0-based).  PAL_M has the same 525-line structure as NTSC.
constexpr int32_t kPalMFirstActiveLine = kNtscFirstActiveLine;

// PAL_M signal levels are identical to NTSC (same line count and blanking).
// Use kNtscSyncTip, kNtscBlanking, kNtscBlack, kNtscWhite, kNtscPeak.

// ---------------------------------------------------------------------------
// SECAM — ITU-R BT.1700-1 Annex 1 Part C
// ---------------------------------------------------------------------------
// CVBS File Format Specification v1.8.1, video-standard-presets.md, Preset
// SECAM: SECAM has the line and field timing and the signal levels of
// 625-line PAL and is stored on the PAL 4fsc lattice. These constants name the
// SECAM preset table; where they equal PAL's they are defined from PAL's so
// the two cannot drift.

// ITU-R BT.1700-1 Annex 1 Part C Table 4 item 2: line frequency fH.
constexpr double kSecamLineFrequency = 15'625.0;

// ITU-R BT.1700-1 Annex 1 Part C Table 4 items 5-6: undeviated D'R
// sub-carrier fOR = 282 fH = 4,406,250 Hz.
constexpr double kSecamFor = 282.0 * kSecamLineFrequency;

// ITU-R BT.1700-1 Annex 1 Part C Table 4 items 5-6: undeviated D'B
// sub-carrier fOB = 272 fH = 4,250,000 Hz.
constexpr double kSecamFob = 272.0 * kSecamLineFrequency;

// CVBS File Format Specification v1.8.1, Preset SECAM "Sampling rate": the
// PAL 4fsc rate, 17,734,475 Hz. SECAM has no single colour sub-carrier, so
// this is not 4 x a SECAM sub-carrier.
constexpr double kSecamSampleRate = kPalSampleRate;

// CVBS File Format Specification v1.8.1, Preset SECAM "Horizontal line
// structure": the PAL lattice, 1135.0064 samples a line.
constexpr double kSecamSamplesPerLine = kPalSamplesPerLine;
constexpr int32_t kSecamSamplesPerLineNominal = kPalSamplesPerLineNominal;

// CVBS File Format Specification v1.8.1, Preset SECAM "Exact frame size":
// 709,379 samples a frame.
constexpr int32_t kSecamFrameSamples = kPalFrameSamples;

// ITU-R BT.1700-1 Annex 1 Part C Table 4 item 1: 625 lines a frame.
constexpr int32_t kSecamFrameLines = kPalFrameLines;

// ITU-R BT.1700-1 Annex 1 Part C: the 625-line field split, 313 lines in
// field 1 of the flat frame buffer, as for PAL.
constexpr int32_t kSecamField1Lines = kPalField1Lines;

// CVBS File Format Specification v1.8.1, Preset SECAM sample level table
// (EBU Tech. 3280-E coding levels applied to ITU-R BT.1700-1 Annex 1 Part C
// Table 5): the PAL levels. Black = blanking (no set-up by default).
constexpr int32_t kSecamSyncTip = kPalSyncTip;
constexpr int32_t kSecamBlanking = kPalBlanking;
constexpr int32_t kSecamBlack = kPalBlack;
constexpr int32_t kSecamWhite = kPalWhite;
constexpr int32_t kSecamPeak = kPalPeak;

// The undeviated sub-carrier reference on the line-blanking back porch,
// which SECAM carries in place of a colour burst, in stored coordinates
// (0H half a sample before stored sample 0 on the first row; the window moves
// with 0H through the frame exactly as PAL's burst window does).
// ITU-R BT.1700-1 Annex 1 Part C Table 5 item i: the sub-carrier is blanked
// up to 5.6 us after 0H, 99.31 samples at 4fsc, so stored 98.81; item b: line
// blanking ends 10.5 us after 0H, 186.21 samples, so stored 185.71. The window
// is stored samples 99-185, including the 300 ns (5 sample) gate edges.
constexpr int32_t kSecamSubcarrierReferenceStart = 99;
constexpr int32_t kSecamSubcarrierReferenceEnd = 186;

// ---------------------------------------------------------------------------
// ld-decode 16-bit domain normative levels
// ---------------------------------------------------------------------------
// The ld-decode .tbc format stores samples as uint16_t using a ×64 scale
// factor applied to the CVBS_U10_4FSC signal levels.  These constants are
// ONLY for use in tbc_source and tbc_sink when converting CVBS_U10_4FSC ↔
// ld-decode 16-bit.  All other pipeline code must use the system-specific
// CVBS_U10_4FSC constants above (kPalBlanking/kPalWhite, kNtscBlanking/
// kNtscWhite) or the 10-bit values from SourceParameters.
//
// NTSC setup note: ld-decode JSON stores black16bIre at the 7.5 IRE picture
// black (setup) level = kNtscBlack × 64 = 282 × 64 = 18048, NOT the 0 IRE
// blanking level = kNtscBlanking × 64 = 240 × 64 = 15360.  The tbc_source
// reader must derive blanking from black using the formula:
//   blanking_16b = black16bIre − 7.5 × (white16bIre − black16bIre) / 92.5
// PAL has no setup pedestal so black == blanking; PAL_M follows the NTSC
// convention above instead (see "PAL_M signal levels are identical to NTSC"
// below, and tbc_level_derivation.h's derive_tbc_domain_levels()).

// PAL ld-decode 16-bit domain levels (CVBS_U10_4FSC × 64):
// kPalBlanking × 64 = 256 × 64 = 16384 (0 IRE blanking)
constexpr int32_t kTbcPalBlanking = 16384;
// kPalWhite × 64 = 844 × 64 = 54016 (100 IRE white)
constexpr int32_t kTbcPalWhite = 54016;

// NTSC ld-decode 16-bit domain levels (CVBS_U10_4FSC × 64):
// kNtscBlanking × 64 = 240 × 64 = 15360 (0 IRE blanking)
constexpr int32_t kTbcNtscBlanking = 15360;
// kNtscBlack × 64 = 282 × 64 = 18048 (7.5 IRE setup; stored as black16bIre in
// ld-decode JSON — NOT the blanking reference)
constexpr int32_t kTbcNtscBlack = 18048;
// kNtscWhite × 64 = 800 × 64 = 51200 (100 IRE white)
constexpr int32_t kTbcNtscWhite = 51200;

// ---------------------------------------------------------------------------
// Active video geometry constants
// ---------------------------------------------------------------------------
// Normative sample and line positions for the active picture area.
// BT.601-5 §2 / EBU Tech. 3280-E §1.2 / SMPTE 170M §6.4 / ITU-R BT.1700-1.
//
// PAL (625-line, EBU Tech. 3280-E §1.2):
constexpr int32_t kPalActiveVideoStart = 157;
constexpr int32_t kPalActiveVideoEnd = 157 + 948;  // = 1105
constexpr int32_t kPalFirstActiveFrameLine = 44;
// ITU-R BT.1700 Table 1 item 1a: 576 active lines for 625-line PAL.
// 44 + 576 = 620.
constexpr int32_t kPalLastActiveFrameLine = 620;

// NTSC (525-line): the SMPTE 244M digital active line, in stored coordinates.
// SMPTE 244M-2003 §4.1.1: the digital active line is digital samples 0-767.
// CVBS File Format Specification v1.8.1 video-standard-presets.md, NTSC
// "Stored line alignment": digital sample 0 is stored sample 125 (0H falls
// 57/90 of a sample before stored sample 0), so the 768 samples are stored
// samples 125-892.
constexpr int32_t kNtscActiveVideoStart = 125;
constexpr int32_t kNtscActiveVideoEnd = 125 + 768;  // = 893
constexpr int32_t kNtscFirstActiveFrameLine = 40;
// ITU-R BT.1700 Table 1 item 1a: 483 active lines for 525-line systems.
// 40 + 483 = 523.
constexpr int32_t kNtscLastActiveFrameLine = 523;

// PAL_M shares NTSC active video geometry (ITU-R BT.1700-1 Annex 1 Part B).

// ---------------------------------------------------------------------------
// Display pixel-aspect correction
// ---------------------------------------------------------------------------
//
// Horizontal scale factor that maps square rendered samples to the intended
// display so the *standard* active picture appears at 4:3.  This is the
// signal's pixel aspect ratio: a fixed property of the sampling geometry, so it
// is derived from the STANDARD active-area constants and depends only on the
// video system.
//
// IMPORTANT: this must NOT be recomputed from a source's (or a video_params
// override's) actual active-area values.  Doing so ties the display aspect to
// the chosen active window, so changing first/last active line or active video
// start/end rescales the whole preview instead of merely re-framing it.  With a
// fixed factor, changing the active window re-frames (crop/extend) while pixels
// keep their shape.
inline double standard_dar_correction(VideoSystem system) {
  int32_t active_w = 0;
  int32_t active_h = 0;
  switch (system) {
    case VideoSystem::PAL:
    case VideoSystem::SECAM:
      active_w = kPalActiveVideoEnd - kPalActiveVideoStart;
      active_h = kPalLastActiveFrameLine - kPalFirstActiveFrameLine;
      break;
    case VideoSystem::NTSC:
    case VideoSystem::PAL_M:
      active_w = kNtscActiveVideoEnd - kNtscActiveVideoStart;
      active_h = kNtscLastActiveFrameLine - kNtscFirstActiveFrameLine;
      break;
    case VideoSystem::Unknown:
      return 1.0;
  }
  if (active_w <= 0 || active_h <= 0) return 1.0;
  return (4.0 / 3.0) /
         (static_cast<double>(active_w) / static_cast<double>(active_h));
}

// ---------------------------------------------------------------------------
// Per-system total-sample and frame-line helpers
// ---------------------------------------------------------------------------

// Return the total number of samples in a complete frame for the given system.
// EBU Tech. 3280-E (PAL) / SMPTE 244M-2003 (NTSC) / ITU-R BT.1700-1 (PAL_M).
inline int32_t frame_samples_from_system(VideoSystem sys) {
  switch (sys) {
    case VideoSystem::PAL:
      return kPalFrameSamples;
    case VideoSystem::SECAM:
      return kSecamFrameSamples;
    case VideoSystem::NTSC:
      return kNtscFrameSamples;
    case VideoSystem::PAL_M:
      return kPalMFrameSamples;
    case VideoSystem::Unknown:
      break;
  }
  return 0;
}

// Return the total number of lines in a complete frame for the given system.
inline int32_t frame_lines_from_system(VideoSystem sys) {
  switch (sys) {
    case VideoSystem::PAL:
      return kPalFrameLines;
    case VideoSystem::SECAM:
      return kSecamFrameLines;
    case VideoSystem::NTSC:
      return kNtscFrameLines;
    case VideoSystem::PAL_M:
      return kPalMFrameLines;
    case VideoSystem::Unknown:
      break;
  }
  return 0;
}

// Return the nominal samples-per-line for the given system.
inline int32_t samples_per_line_from_system(VideoSystem sys) {
  switch (sys) {
    case VideoSystem::PAL:
      return kPalSamplesPerLineNominal;
    case VideoSystem::SECAM:
      return kSecamSamplesPerLineNominal;
    case VideoSystem::NTSC:
      return kNtscSamplesPerLine;
    case VideoSystem::PAL_M:
      return kPalMSamplesPerLine;
    case VideoSystem::Unknown:
      break;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Colour burst sample range constants
// ---------------------------------------------------------------------------
// Sample offsets within a source line that locate the colour burst window.
// These are nominal values fixed by the video standard; per-disc values may
// deviate by a few samples for damaged or non-standard tapes, but spec-defined
// ranges are sufficient for dropout-correction masking and observer windowing.

// EBU Tech. 3280-E Table 1: PAL colour burst sample range.
constexpr int32_t kPalColourBurstStart = 98;
constexpr int32_t kPalColourBurstEnd = 138;

// NTSC colour burst sample range, in stored coordinates (0H 57/90 of a
// sample before stored sample 0; CVBS File Format Specification v1.8.1
// video-standard-presets.md, NTSC "Stored line alignment").
// SMPTE 170M-2004 Table 2: burst start is 19 subcarrier cycles after 0H
// (76 samples at 4fsc, so stored 75.37) and the burst lasts 9 cycles (36
// samples, ending at stored 111.37).  The window is the 36 whole samples in
// between: stored samples 76-111.
constexpr int32_t kNtscColourBurstStart = 76;
constexpr int32_t kNtscColourBurstEnd = 112;

// PAL_M colour burst sample range, in stored coordinates (0H half a sample
// before stored sample 0 on every line).  ITU-R BT.470-6 Table 2 item 2.14
// (M/PAL): burst start 5.8 +/- 0.1 us after 0H, 82.95 samples at 4fsc, so
// stored 82.45; duration 9 +/- 1 cycles (36 samples, ending at stored
// 118.45).  The window is stored samples 83-118.  (The BT.1700 transcription
// in docs-tech gives 5.3 us for 525 PAL; BT.470-6 and videosynth use 5.8.)
constexpr int32_t kPalMColourBurstStart = 83;
constexpr int32_t kPalMColourBurstEnd = 119;

// Return the (start, end) colour burst sample range for the given video system.
// SECAM has no colour burst; its range is the undeviated sub-carrier reference
// on the back porch (kSecamSubcarrierReferenceStart/End), the part of the line
// that carries the colour reference in its place.
inline std::pair<int32_t, int32_t> colour_burst_range(VideoSystem sys) {
  switch (sys) {
    case VideoSystem::PAL:
      return {kPalColourBurstStart, kPalColourBurstEnd};
    case VideoSystem::PAL_M:
      return {kPalMColourBurstStart, kPalMColourBurstEnd};
    case VideoSystem::SECAM:
      return {kSecamSubcarrierReferenceStart, kSecamSubcarrierReferenceEnd};
    case VideoSystem::NTSC:
    case VideoSystem::Unknown:
      break;
  }
  return {kNtscColourBurstStart, kNtscColourBurstEnd};
}

// ---------------------------------------------------------------------------
// System-derived sample rate and subcarrier frequency helpers
// ---------------------------------------------------------------------------
// Both values are fully determined by the video system; reading them from
// per-disc metadata and re-storing in SourceParameters adds round-trip noise.

// Return the 4FSC sample rate (Hz) for the given video system.
// EBU Tech. 3280-E §1.1 (PAL) / SMPTE 244M-2003 §4.1 (NTSC) /
// ITU-R BT.1700-1 Annex 1 Part B (PAL_M).
inline double sample_rate_from_system(VideoSystem sys) {
  switch (sys) {
    case VideoSystem::PAL:
      return kPalSampleRate;
    case VideoSystem::SECAM:
      return kSecamSampleRate;
    case VideoSystem::PAL_M:
      return kPalMSampleRate;
    case VideoSystem::NTSC:
    case VideoSystem::Unknown:
      break;
  }
  return kNtscSampleRate;
}

// Return the colour subcarrier frequency (Hz) for the given video system.
// EBU Tech. 3280-E §1.1 (PAL) / SMPTE 244M-2003 §4.1 (NTSC) /
// ITU-R BT.1700-1 Annex 1 Part B (PAL_M).
// SECAM has no single colour sub-carrier (fOR and fOB alternate by line);
// it returns the frequency its 4fsc storage rate is a quarter of, the PAL
// fsc (CVBS File Format Specification v1.8.1, Preset SECAM "Sampling rate").
// Code that wants a SECAM carrier must use kSecamFor / kSecamFob.
inline double fsc_from_system(VideoSystem sys) {
  switch (sys) {
    case VideoSystem::PAL:
    case VideoSystem::SECAM:
      return kPalFsc;
    case VideoSystem::PAL_M:
      return kPalMFsc;
    case VideoSystem::NTSC:
    case VideoSystem::Unknown:
      break;
  }
  return kNtscFsc;
}

// Return the number of lines in field 1 (the larger/first field in the flat
// frame buffer) for the given video system.
inline size_t field1_lines(VideoSystem sys) {
  switch (sys) {
    case VideoSystem::PAL:
      return static_cast<size_t>(kPalField1Lines);
    case VideoSystem::SECAM:
      return static_cast<size_t>(kSecamField1Lines);
    case VideoSystem::PAL_M:
      return static_cast<size_t>(kPalMField1Lines);
    case VideoSystem::NTSC:
    case VideoSystem::Unknown:
      break;
  }
  return static_cast<size_t>(kNtscField1Lines);
}

}  // namespace orc
