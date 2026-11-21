/*
 * File:        ntsc_tbc_converter.h
 * Module:      orc-stage-plugin-tbc-source
 * Purpose:     NTSC TBC level mapping and frame assembly into CVBS_U10_4FSC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/cvbs_signal_constants.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "tbc_level_scale.h"

namespace orc {

// ---------------------------------------------------------------------------
// NtscTBCConverter
// ---------------------------------------------------------------------------
// Stateless helper class for NTSC TBC → CVBS_U10_4FSC conversion.
//
// SMPTE 244M-2003 §4.1: NTSC 4FSC sampling at 910 samples/line.
// SMPTE 244M-2003 §3.1: 525 lines/frame, 2:1 interlace.
//
// TBC field ordering (SMPTE 244M-2003 §3.2 / SMPTE 170M-2004 §11.3):
//   ld-decode stores both fields padded to 263 lines in the TBC file.
//   TBC field 1 (even file index, isFirstField=true) = odd-scan/first temporal,
//     263 real lines → VFR field 1 (top spatial).
//   TBC field 2 (odd file index) = even-scan/second temporal,
//     262 real lines → VFR field 2 (bottom spatial).
//   VFR frame layout: [VFR field 1 = TBC field 1 (263 lines, top)]
//                     [VFR field 2 = TBC field 2 (262 lines, bottom)]
//   NTSC Field 1 (odd-scan, 263 lines) starts at the very top of the picture,
//   unlike PAL where field ordering is reversed.
class NtscTBCConverter {
 public:
  // -------------------------------------------------------------------------
  // Level mapping
  // -------------------------------------------------------------------------

  // SMPTE 244M-2003: the linear TBC → CVBS_U10_4FSC level map for NTSC.
  //
  // tbc_blanking / tbc_white are the 16-bit TBC-domain level values from
  // `.tbc.json.db` (blanking_16b_ire, white_16b_ire).  Build this once per
  // frame and map samples through it; see tbc_level_scale.h for why the
  // division cannot stay in the per-sample path.
  static TbcLevelScale level_scale(int32_t tbc_blanking, int32_t tbc_white) {
    return make_tbc_level_scale(tbc_blanking, tbc_white, kNtscBlanking,
                                kNtscWhite);
  }

  // Map one TBC 16-bit unsigned sample to CVBS_U10_4FSC.  No output clamping:
  // headroom below sync tip and above peak white is preserved.
  //
  // Convenience overload for single samples — it rebuilds the level map on
  // every call, so never use it in a loop.
  //
  // Defined here, not in the .cpp: the plugins build with default symbol
  // visibility, so an out-of-line definition is interposable and the compiler
  // must emit a PLT call per sample — even from its own translation unit.
  static int16_t tbc_to_cvbs(uint16_t tbc_sample, int32_t tbc_blanking,
                             int32_t tbc_white) {
    return level_scale(tbc_blanking, tbc_white).map(tbc_sample);
  }

  // -------------------------------------------------------------------------
  // Frame assembly
  // -------------------------------------------------------------------------

  // Assemble a CVBS_U10_4FSC NTSC frame from two TBC fields.
  //
  // tbc_field1: kNtscField1Lines = 263 lines × 910 = 239,530 samples.
  //   Odd-scan/first temporal field (ld-decode even file index); becomes
  //   VFR field 1 (top spatial).
  //
  // tbc_field2: kNtscFrameLines - kNtscField1Lines = 262 lines × 910
  //   = 238,220 samples.  Even-scan/second temporal field (ld-decode odd file
  //   index); becomes VFR field 2 (bottom spatial).  The TBC file stores both
  //   fields padded to 263 lines; caller strips the padding line and passes
  //   exactly 262 × 910 samples.
  //
  // SMPTE 244M-2003 §4.1: NTSC is orthogonal — all lines have exactly
  // kNtscSamplesPerLine = 910 samples; no extra samples are inserted.
  //
  // Output: [VFR field 1 (top): 263 × 910][VFR field 2 (bottom): 262 × 910]
  //         = kNtscFrameSamples, on the CVBS specification's line origin:
  //         every sample sits two places earlier than in the TBC, and TBC
  //         field 1's first two samples end the frame
  //         (common/tbc-format/ntsc_tbc_origin.h).
  static std::vector<int16_t> assemble_frame(
      const std::vector<uint16_t>& tbc_field1,  // 263 × 910 samples
      const std::vector<uint16_t>& tbc_field2,  // 262 × 910 samples
      int32_t tbc_blanking, int32_t tbc_white);
};

}  // namespace orc
