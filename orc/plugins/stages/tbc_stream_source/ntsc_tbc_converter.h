/*
 * File:        ntsc_tbc_converter.h
 * Module:      orc-stage-plugin-tbc-stream-source
 * Purpose:     NTSC TBC level mapping and frame assembly into CVBS_U10_4FSC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * Deliberately duplicated from
 * orc/plugins/stages/tbc_source/ntsc_tbc_converter.h rather than shared —
 * see tbc_level_scale.h's own note on why. The two must stay in agreement
 * if this math ever changes.
 */

#pragma once

#include <orc/stage/cvbs_signal_constants.h>

#include <cstdint>
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
//   TBC field 1 (even file index, isFirstField=true) = odd-scan/first
//     temporal, 263 real lines → VFR field 1 (top spatial).
//   TBC field 2 (odd file index) = even-scan/second temporal,
//     262 real lines → VFR field 2 (bottom spatial). The file stores it
//     padded to 263 lines; the caller reads 263 and passes only the first
//     262 × 910 samples to assemble_frame().
class NtscTBCConverter {
 public:
  // -------------------------------------------------------------------------
  // Level mapping
  // -------------------------------------------------------------------------

  // SMPTE 244M-2003: the linear TBC → CVBS_U10_4FSC level map for NTSC.
  //
  // tbc_blanking / tbc_white are the TBC-domain level values (blanking_16b,
  // white_16b). Build this once per frame and map samples through it; see
  // tbc_level_scale.h for why the division cannot stay in the per-sample
  // path.
  static TbcLevelScale level_scale(int32_t tbc_blanking, int32_t tbc_white) {
    return make_tbc_level_scale(tbc_blanking, tbc_white, kNtscBlanking,
                                kNtscWhite);
  }

  // -------------------------------------------------------------------------
  // Frame assembly
  // -------------------------------------------------------------------------

  // Assemble a CVBS_U10_4FSC NTSC frame from two TBC fields.
  //
  // tbc_field1: kNtscField1Lines = 263 lines × 910 = 239,530 samples.
  // tbc_field2: kNtscFrameLines - kNtscField1Lines = 262 lines × 910
  //   = 238,220 samples — the padding line already stripped by the caller.
  //
  // SMPTE 244M-2003 §4.1: NTSC is orthogonal — all lines have exactly
  // kNtscSamplesPerLine = 910 samples.
  //
  // Output: [VFR field 1 (top): 263 × 910][VFR field 2 (bottom): 262 × 910]
  //         = kNtscFrameSamples.
  static std::vector<int16_t> assemble_frame(
      const std::vector<uint16_t>& tbc_field1,  // 263 × 910 samples
      const std::vector<uint16_t>& tbc_field2,  // 262 × 910 samples
      int32_t tbc_blanking, int32_t tbc_white);
};

}  // namespace orc
