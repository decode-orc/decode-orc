/*
 * File:        pal_m_tbc_converter.h
 * Module:      orc-stage-plugin-tbc-stream-source
 * Purpose:     PAL_M TBC level mapping and frame assembly into CVBS_U10_4FSC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * Deliberately duplicated from
 * orc/plugins/stages/tbc_source/pal_m_tbc_converter.h rather than shared —
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
// PalMTBCConverter
// ---------------------------------------------------------------------------
// Stateless helper class for PAL_M TBC → CVBS_U10_4FSC conversion.
//
// ITU-R BT.1700-1 Annex 1 Part B: PAL_M is the Brazilian variant of PAL
// that uses NTSC line/field structure (525 lines, 60 Hz) with PAL colour
// encoding. The subcarrier frequency is ~3.575611 MHz giving 909 samples/line
// at 4FSC.
//
// Signal levels: PAL_M uses the same levels as NTSC (kNtscBlanking,
// kNtscWhite) per ITU-R BT.1700-1 Annex 1 Part B.
//
// TBC field ordering (ld-decode convention, identical to NTSC): both fields
// stored padded to 263 lines; the caller strips field 2's padding line
// before calling assemble_frame().
class PalMTBCConverter {
 public:
  // -------------------------------------------------------------------------
  // Level mapping
  // -------------------------------------------------------------------------

  // ITU-R BT.1700-1 Annex 1 Part B: the linear TBC → CVBS_U10_4FSC level map
  // for PAL_M. Level constants are identical to NTSC.
  static TbcLevelScale level_scale(int32_t tbc_blanking, int32_t tbc_white) {
    return make_tbc_level_scale(tbc_blanking, tbc_white, kNtscBlanking,
                                kNtscWhite);
  }

  // -------------------------------------------------------------------------
  // Frame assembly
  // -------------------------------------------------------------------------

  // Assemble a CVBS_U10_4FSC PAL_M frame from two TBC fields.
  //
  // tbc_field1: kPalMField1Lines = 263 lines × 909 = 239,067 samples.
  // tbc_field2: kPalMFrameLines - kPalMField1Lines = 262 lines × 909
  //   = 238,158 samples — the padding line already stripped by the caller.
  //
  // PAL_M is orthogonal: kPalMSamplesPerLine = 909 on every line.
  // Output: [VFR field 1 (top): 263 × 909][VFR field 2 (bottom): 262 × 909]
  //         = kPalMFrameSamples.
  static std::vector<int16_t> assemble_frame(
      const std::vector<uint16_t>& tbc_field1,  // 263 × 909 samples
      const std::vector<uint16_t>& tbc_field2,  // 262 × 909 samples
      int32_t tbc_blanking, int32_t tbc_white);
};

}  // namespace orc
