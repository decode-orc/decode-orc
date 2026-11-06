/*
 * File:        pal_tbc_converter.h
 * Module:      orc-stage-plugin-tbc-source
 * Purpose:     PAL TBC level mapping and frame assembly into CVBS_U10_4FSC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/frame_descriptor.h>

#include <cstdint>
#include <vector>

#include "tbc_level_scale.h"

namespace orc {

// ---------------------------------------------------------------------------
// PalTBCConverter
// ---------------------------------------------------------------------------
// Stateless helper class for PAL TBC → CVBS_U10_4FSC conversion.
//
// EBU Tech. 3280-E §1.1: PAL 4FSC sampling.
// EBU Tech. 3280-E §1.3: PAL non-orthogonal line structure.
//
// TBC field ordering note (EBU Tech. 3280-E §1.3 / ld-decode PAL convention):
//   PAL field 1 (odd scan, first temporal, isFirstField=true) has 313 stored
//   lines (lines 1, 3, 5, …, 625). Field 2 (even scan, second temporal) has
//   312 stored lines (lines 2, 4, …, 624). EBU/CVBS convention places field 1
//   first in the flat frame buffer. Therefore:
//     TBC field 1 (313 lines) → CVBS field 1 (top, odd scan);
//     TBC field 2 (312 lines) → CVBS field 2 (bottom, even scan).
class PalTBCConverter {
 public:
  // -------------------------------------------------------------------------
  // Level mapping
  // -------------------------------------------------------------------------

  // EBU Tech. 3280-E: the linear TBC → CVBS_U10_4FSC level map for PAL.
  //
  // tbc_blanking and tbc_white are the TBC-domain level values read from
  // `.tbc.json.db` (blanking_16b_ire, white_16b_ire).  Build this once per
  // frame and map samples through it; see tbc_level_scale.h for why the
  // division cannot stay in the per-sample path.
  static TbcLevelScale level_scale(int32_t tbc_blanking, int32_t tbc_white) {
    return make_tbc_level_scale(tbc_blanking, tbc_white, kPalBlanking,
                                kPalWhite);
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

  // Assemble a CVBS_U10_4FSC PAL frame from two TBC fields.
  //
  // tbc_field1: kPalField1Lines = 313 lines × 1135 samples from the TBC file
  //   (the odd/earlier temporal field — ld-decode TBC field 1, isFirstField).
  //
  // tbc_field2: kPalFrameLines - kPalField1Lines = 312 lines × 1135 samples
  //   from the TBC file (even/later temporal — ld-decode TBC field 2).
  //
  // tbc_blanking / tbc_white: TBC-domain level values.
  //
  // Output layout:
  //   [CVBS field 1: frame rows 0–312 (= TBC field 1, odd scan)] followed by
  //   [CVBS field 2: frame rows 313–624 (= TBC field 2, even scan)]
  //   Total output size = kPalFrameSamples = 709,379.
  //
  // The TBC fields are line-locked; each is resampled onto the 4fsc lattice
  // (pal_line_locked_resample.h), which makes it two samples longer — the
  // extra samples on rows 312 and 624 (EBU Tech. 3280-E §1.3.1).
  static std::vector<int16_t> assemble_frame(
      const std::vector<uint16_t>& tbc_field1,  // 313 × 1135 samples
      const std::vector<uint16_t>& tbc_field2,  // 312 × 1135 samples
      int32_t tbc_blanking, int32_t tbc_white);
};

}  // namespace orc
