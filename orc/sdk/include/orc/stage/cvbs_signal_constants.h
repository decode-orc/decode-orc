/*
 * File:        cvbs_signal_constants.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Normative CVBS_U10_4FSC signal constants for PAL, NTSC, and
 * PAL_M
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

// NTSC (525-line, SMPTE 170M §6.4):
constexpr int32_t kNtscActiveVideoStart = 126;
constexpr int32_t kNtscActiveVideoEnd = 126 + 768;  // = 894
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
      active_w = kPalActiveVideoEnd - kPalActiveVideoStart;
      active_h = kPalLastActiveFrameLine - kPalFirstActiveFrameLine;
      break;
    case VideoSystem::NTSC:
    case VideoSystem::PAL_M:
      active_w = kNtscActiveVideoEnd - kNtscActiveVideoStart;
      active_h = kNtscLastActiveFrameLine - kNtscFirstActiveFrameLine;
      break;
    default:
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
    case VideoSystem::NTSC:
      return kNtscFrameSamples;
    case VideoSystem::PAL_M:
      return kPalMFrameSamples;
    default:
      return 0;
  }
}

// Return the total number of lines in a complete frame for the given system.
inline int32_t frame_lines_from_system(VideoSystem sys) {
  switch (sys) {
    case VideoSystem::PAL:
      return kPalFrameLines;
    case VideoSystem::NTSC:
      return kNtscFrameLines;
    case VideoSystem::PAL_M:
      return kPalMFrameLines;
    default:
      return 0;
  }
}

// Return the nominal samples-per-line for the given system.
inline int32_t samples_per_line_from_system(VideoSystem sys) {
  switch (sys) {
    case VideoSystem::PAL:
      return kPalSamplesPerLineNominal;
    case VideoSystem::NTSC:
      return kNtscSamplesPerLine;
    case VideoSystem::PAL_M:
      return kPalMSamplesPerLine;
    default:
      return 0;
  }
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

// NTSC colour burst sample range in the ld-decode CVBS_U10_4FSC line format.
// SMPTE 170M-2004 Table 2: burst duration = 9 ± 1 subcarrier cycles →
// 36 ± 4 samples at 4fsc. Window here is exactly 36 samples (9 cycles). ✓
// SMPTE 170M-2004 §8.2: burst centre is nominally 19 subcarrier cycles after
// the H-sync leading edge (50 % point). The ld-decode TBC places sample 0 at
// the start of horizontal blanking (before sync), so absolute sample numbers
// are ld-decode-format-specific rather than directly spec-derived. The window
// is verified empirically to enclose the burst for all standard NTSC sources.
// PAL_M uses the same values as NTSC.
constexpr int32_t kNtscColourBurstStart = 72;
constexpr int32_t kNtscColourBurstEnd = 108;

// Return the (start, end) colour burst sample range for the given video system.
inline std::pair<int32_t, int32_t> colour_burst_range(VideoSystem sys) {
  if (sys == VideoSystem::PAL) {
    return {kPalColourBurstStart, kPalColourBurstEnd};
  }
  // NTSC and PAL_M share the same burst window.
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
    case VideoSystem::PAL_M:
      return kPalMSampleRate;
    default:
      return kNtscSampleRate;
  }
}

// Return the colour subcarrier frequency (Hz) for the given video system.
// EBU Tech. 3280-E §1.1 (PAL) / SMPTE 244M-2003 §4.1 (NTSC) /
// ITU-R BT.1700-1 Annex 1 Part B (PAL_M).
inline double fsc_from_system(VideoSystem sys) {
  switch (sys) {
    case VideoSystem::PAL:
      return kPalFsc;
    case VideoSystem::PAL_M:
      return kPalMFsc;
    default:
      return kNtscFsc;
  }
}

// Return the number of lines in field 1 (the larger/first field in the flat
// frame buffer) for the given video system.
inline size_t field1_lines(VideoSystem sys) {
  switch (sys) {
    case VideoSystem::PAL:
      return static_cast<size_t>(kPalField1Lines);
    case VideoSystem::PAL_M:
      return static_cast<size_t>(kPalMField1Lines);
    default:
      return static_cast<size_t>(kNtscField1Lines);
  }
}

}  // namespace orc
