/*
 * File:        pal_tbc_converter.cpp
 * Module:      orc-stage-plugin-tbc-stream-source
 * Purpose:     PAL TBC level mapping and frame assembly into CVBS_U10_4FSC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "pal_tbc_converter.h"

#include <orc/support/pal_line_locked_resample.h>

#include <stdexcept>

namespace orc {

std::vector<int16_t> PalTBCConverter::assemble_frame(
    const std::vector<uint16_t>& tbc_field1,  // 313 lines × 1135 samples
    const std::vector<uint16_t>& tbc_field2,  // 312 lines × 1135 samples
    int32_t tbc_blanking, int32_t tbc_white) {
  // TBC field 1 (isFirstField, 313 lines) → CVBS field 1 (frame rows 0–312);
  // TBC field 2 (312 lines) → CVBS field 2 (frame rows 313–624).
  if (tbc_field1.size() != pal_line_locked::line_locked_field_samples(1) ||
      tbc_field2.size() != pal_line_locked::line_locked_field_samples(2)) {
    throw std::invalid_argument(
        "PalTBCConverter::assemble_frame: unexpected field sample counts");
  }

  // ld-decode's PAL fields are line-locked (exactly 1135 samples per line);
  // the CVBS_U10_4FSC frame is the subcarrier-locked 4fsc lattice (1135.0064
  // samples per line). Each field is resampled uniformly onto the lattice and
  // comes out two samples longer, filling the EBU Tech. 3280-E §1.3.1 extra
  // samples on frame rows 312 and 624 with real signal. See
  // pal_line_locked_resample.h for the geometry (issue #369).
  std::vector<int16_t> frame(static_cast<size_t>(kPalFrameSamples));

  // Level map built once for the whole frame — the source levels are constant
  // across it, so the division belongs here and not in the sample loops.
  const TbcLevelScale levels = level_scale(tbc_blanking, tbc_white);
  const auto map = [&levels](float v) { return levels.map_value(v); };

  pal_line_locked::to_lattice(
      1, tbc_field1.data(),
      frame.data() + pal_line_locked::lattice_field_start(1), map);
  pal_line_locked::to_lattice(
      2, tbc_field2.data(),
      frame.data() + pal_line_locked::lattice_field_start(2), map);

  return frame;
}

}  // namespace orc
