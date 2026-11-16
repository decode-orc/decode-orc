/*
 * File:        ntsc_tbc_converter.cpp
 * Module:      orc-stage-plugin-tbc-source
 * Purpose:     NTSC TBC level mapping and frame assembly into CVBS_U10_4FSC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "ntsc_tbc_converter.h"

#include <cmath>
#include <stdexcept>

#include "../common/tbc-format/ntsc_tbc_origin.h"

namespace orc {

// ---------------------------------------------------------------------------
// Level mapping
// ---------------------------------------------------------------------------

// tbc_to_cvbs lives in the header — see the note there on why.

// ---------------------------------------------------------------------------
// Frame assembly
// ---------------------------------------------------------------------------

std::vector<int16_t> NtscTBCConverter::assemble_frame(
    const std::vector<uint16_t>& tbc_field1,  // 263 × 910 samples
    const std::vector<uint16_t>& tbc_field2,  // 262 × 910 samples
    int32_t tbc_blanking, int32_t tbc_white) {
  // SMPTE 244M-2003 §4.1 / SMPTE 170M-2004 §11.3: NTSC frame assembly.
  // TBC field ordering (ld-decode convention for NTSC):
  //   TBC field 1 (even file index) = odd-scan/first temporal, 263 real lines
  //     → VFR field 1 (top spatial).  Starts at the very top of the picture.
  //   TBC field 2 (odd file index) = even-scan/second temporal, 262 real lines
  //     → VFR field 2 (bottom spatial).
  // VFR layout: [field 1 (top, 263 lines)][field 2 (bottom, 262 lines)]
  // NTSC is orthogonal: all lines have kNtscSamplesPerLine = 910 samples.
  constexpr int32_t kTBCF1Lines = kNtscField1Lines;                    // 263
  constexpr int32_t kTBCF2Lines = kNtscFrameLines - kNtscField1Lines;  // 262
  constexpr int32_t kLineW = kNtscSamplesPerLine;                      // 910

  const size_t exp_f1 =
      static_cast<size_t>(kTBCF1Lines) * static_cast<size_t>(kLineW);
  const size_t exp_f2 =
      static_cast<size_t>(kTBCF2Lines) * static_cast<size_t>(kLineW);

  if (tbc_field1.size() != exp_f1 || tbc_field2.size() != exp_f2) {
    throw std::invalid_argument(
        "NtscTBCConverter::assemble_frame: unexpected field sample counts");
  }

  // Sized up front and written through a cursor rather than push_back: the
  // output length is fixed, so the per-sample capacity check buys nothing.
  std::vector<int16_t> frame(static_cast<size_t>(kNtscFrameSamples));
  int16_t* dst = frame.data();

  // Level map built once for the whole frame — the source levels are
  // constant across it, so the division belongs here and not in the
  // sample loops.
  const TbcLevelScale levels = level_scale(tbc_blanking, tbc_white);

  // The TBC stores ld-decode's line origin, two samples before the
  // specification's (ntsc_tbc_origin.h): TBC frame sample j is written to
  // frame sample j - 2, and TBC samples 0-1 to the end of the frame.
  constexpr size_t kShift = ntsc_tbc_origin::kOriginShiftSamples;
  const size_t frame_samples = frame.size();
  for (size_t i = 0; i < kShift; ++i) {
    dst[frame_samples - kShift + i] = levels.map(tbc_field1[i]);
  }

  // VFR field 1 (top, 263 lines) ← TBC field 1 (odd-scan, first temporal)
  for (size_t i = kShift; i < exp_f1; ++i) {
    dst[i - kShift] = levels.map(tbc_field1[i]);
  }
  dst += exp_f1 - kShift;

  // VFR field 2 (bottom, 262 lines) ← TBC field 2 (even-scan, second temporal)
  for (size_t i = 0; i < exp_f2; ++i) {
    dst[i] = levels.map(tbc_field2[i]);
  }

  return frame;
}

}  // namespace orc
