/*
 * File:        ntsc_tbc_converter.cpp
 * Module:      orc-stage-plugin-tbc-stream-source
 * Purpose:     NTSC TBC level mapping and frame assembly into CVBS_U10_4FSC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "ntsc_tbc_converter.h"

#include <stdexcept>

namespace orc {

std::vector<int16_t> NtscTBCConverter::assemble_frame(
    const std::vector<uint16_t>& tbc_field1,  // 263 × 910 samples
    const std::vector<uint16_t>& tbc_field2,  // 262 × 910 samples
    int32_t tbc_blanking, int32_t tbc_white) {
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

  std::vector<int16_t> frame(static_cast<size_t>(kNtscFrameSamples));
  int16_t* dst = frame.data();

  const TbcLevelScale levels = level_scale(tbc_blanking, tbc_white);

  // VFR field 1 (top, 263 lines) ← TBC field 1 (odd-scan, first temporal)
  for (size_t i = 0; i < exp_f1; ++i) {
    dst[i] = levels.map(tbc_field1[i]);
  }
  dst += exp_f1;

  // VFR field 2 (bottom, 262 lines) ← TBC field 2 (even-scan, second temporal)
  for (size_t i = 0; i < exp_f2; ++i) {
    dst[i] = levels.map(tbc_field2[i]);
  }

  return frame;
}

}  // namespace orc
