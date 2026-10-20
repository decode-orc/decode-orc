/*
 * File:        pal_tbc_converter.cpp
 * Module:      orc-stage-plugin-tbc-stream-source
 * Purpose:     PAL TBC level mapping and frame assembly into CVBS_U10_4FSC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "pal_tbc_converter.h"

#include <stdexcept>

namespace orc {

// Write 2 linearly-interpolated bridge samples at t=1/3 and t=2/3 to |dst|.
// EBU Tech. 3280-E §1.3.1: the two extra samples on lines 312 and 624 bridge
// the signal from the last nominal sample to the first sample of the next
// line.
static void write_two_extra_samples(int16_t* dst, int16_t last,
                                    int16_t first_next) {
  const int32_t a = static_cast<int32_t>(last);
  const int32_t b = static_cast<int32_t>(first_next);
  dst[0] = static_cast<int16_t>((2 * a + b) / 3);
  dst[1] = static_cast<int16_t>((a + 2 * b) / 3);
}

std::vector<int16_t> PalTBCConverter::assemble_frame(
    const std::vector<uint16_t>& tbc_field1,  // 313 lines × 1135 samples
    const std::vector<uint16_t>& tbc_field2,  // 312 lines × 1135 samples
    int32_t tbc_blanking, int32_t tbc_white) {
  constexpr int32_t kField1Lines = kPalField1Lines;                   // 313
  constexpr int32_t kField2Lines = kPalFrameLines - kPalField1Lines;  // 312
  constexpr int32_t kLineWidth = kPalSamplesPerLineNominal;           // 1135

  const size_t expected_field1 =
      static_cast<size_t>(kField1Lines) * static_cast<size_t>(kLineWidth);
  const size_t expected_field2 =
      static_cast<size_t>(kField2Lines) * static_cast<size_t>(kLineWidth);

  if (tbc_field1.size() != expected_field1 ||
      tbc_field2.size() != expected_field2) {
    throw std::invalid_argument(
        "PalTBCConverter::assemble_frame: unexpected field sample counts");
  }

  // Flat frame buffer: [CVBS field 1 (313 lines)] [CVBS field 2 (312 lines)]
  // with 2 extra interpolated samples appended to the last line of each
  // field (frame-flat lines 312 and 624) per EBU Tech. 3280-E §1.3.1.
  std::vector<int16_t> frame(static_cast<size_t>(kPalFrameSamples));
  int16_t* dst = frame.data();

  const TbcLevelScale levels = level_scale(tbc_blanking, tbc_white);

  // ---- CVBS field 1: sourced from TBC field 1 (313 lines, odd-scan) ----
  for (size_t i = 0; i < expected_field1; ++i) {
    dst[i] = levels.map(tbc_field1[i]);
  }
  dst += expected_field1;

  // Frame-flat line 312 (last of field 1) gets 2 bridge samples toward the
  // first sample of field 2.
  write_two_extra_samples(dst, dst[-1], levels.map(tbc_field2[0]));
  dst += 2;

  // ---- CVBS field 2: sourced from TBC field 2 (312 lines, even-scan) ----
  for (size_t i = 0; i < expected_field2; ++i) {
    dst[i] = levels.map(tbc_field2[i]);
  }
  dst += expected_field2;

  // Frame-flat line 624 (last of field 2) gets 2 extra bridge samples. No
  // following line in this frame; bridge toward the last sample itself.
  write_two_extra_samples(dst, dst[-1], dst[-1]);

  return frame;
}

}  // namespace orc
