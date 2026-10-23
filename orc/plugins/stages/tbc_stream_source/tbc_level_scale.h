/*
 * File:        tbc_level_scale.h
 * Module:      orc-stage-plugin-tbc-stream-source
 * Purpose:     Frame-constant linear map from the TBC 16-bit domain to
 *              CVBS_U10_4FSC
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * Deliberately duplicated from orc/plugins/stages/tbc_source/tbc_level_scale.h
 * rather than shared: a plugin cannot include another plugin's private
 * headers, and this stage's whole point is staying self-contained so it
 * never has to touch the existing file-based TBC source's code. The two
 * must stay in agreement if this math ever changes.
 */

#pragma once

#include <cstdint>

namespace orc {

// The TBC → CVBS_U10_4FSC level mapping with the frame-constant division
// already applied, so converting a frame is one multiply-add per sample.
//
// Built once per assemble_frame() call. Computing it per sample would cost a
// double division and a libm round on every sample in a frame, neither of
// which the vectoriser can touch.
struct TbcLevelScale {
  double scale = 1.0;
  double offset = 0.0;

  // Map one TBC 16-bit unsigned sample to CVBS_U10_4FSC. No output clamping:
  // headroom below sync tip and above peak white is preserved in the int16_t
  // result.
  int16_t map(uint16_t tbc_sample) const {
    // Round to nearest, ties away from zero, without a libm call.
    const double v = static_cast<double>(tbc_sample) * scale + offset;
    return static_cast<int16_t>(v < 0.0 ? v - 0.5 : v + 0.5);
  }
};

// Build the map from the TBC-domain levels (blanking_16b, white_16b) and the
// target CVBS_U10_4FSC levels for the video system.
inline TbcLevelScale make_tbc_level_scale(int32_t tbc_blanking,
                                          int32_t tbc_white,
                                          int32_t cvbs_blanking,
                                          int32_t cvbs_white) {
  TbcLevelScale levels;
  levels.scale = static_cast<double>(cvbs_white - cvbs_blanking) /
                 static_cast<double>(tbc_white - tbc_blanking);
  levels.offset =
      static_cast<double>(cvbs_blanking) - tbc_blanking * levels.scale;
  return levels;
}

}  // namespace orc
