/*
 * File:        tbc_sink_levels.h
 * Module:      orc-stage-plugin-tbc-sink
 * Purpose:     CVBS_U10_4FSC → ld-decode 16-bit TBC domain level mapping
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/orc_source_parameters.h>

#include <algorithm>
#include <cstdint>

namespace orc {

// The inverse of the tbc_source level map (see tbc_level_scale.h): it takes
// CVBS_U10_4FSC samples back to the ld-decode 16-bit .tbc domain.
//
// The anchors on both sides are the normative constants for the video system,
// and the 16-bit domain is defined as CVBS_U10_4FSC × 64
// (cvbs_signal_constants.h "ld-decode 16-bit domain normative levels"), so the
// map always reduces to scale = 64, offset = 0 — a lossless widening.  Signal
// levels that differ from the spec (NTSC-J picture black, a video_params
// override) are NOT normalised away here; they stay in the samples and are
// described by the capture-row levels from tbc_capture_levels() below.
struct TbcSinkLevelScale {
  double scale = 64.0;
  double offset = 0.0;

  // Map one CVBS_U10_4FSC sample to the ld-decode 16-bit domain, clamped to
  // the uint16_t storage range.
  uint16_t to_tbc(int16_t cvbs) const {
    const int32_t v = to_tbc_level(static_cast<int32_t>(cvbs));
    return static_cast<uint16_t>(std::clamp(v, 0, 65535));
  }

  // The same map for a CVBS value that is not on the integer grid (an
  // interpolated sample from the PAL line-locked resample).
  uint16_t to_tbc_value(float cvbs) const {
    const double v = static_cast<double>(cvbs) * scale + offset;
    const double r = v < 0.0 ? v - 0.5 : v + 0.5;
    if (r <= 0.0) return 0;
    if (r >= 65535.0) return 65535;
    return static_cast<uint16_t>(r);
  }

  // Same map applied to a level rather than a sample: no clamping, so it can
  // also carry the out-of-picture reference levels recorded in the metadata.
  int32_t to_tbc_level(int32_t cvbs_level) const {
    // Round to nearest, ties away from zero, without a libm call.
    const double v = static_cast<double>(cvbs_level) * scale + offset;
    return static_cast<int32_t>(v < 0.0 ? v - 0.5 : v + 0.5);
  }
};

// Build the map for |system| from the normative CVBS_U10_4FSC and ld-decode
// 16-bit level pairs.  EBU Tech. 3280-E §1.1.1 (PAL) / SMPTE 244M-2003 §4.2.1
// (NTSC) / ITU-R BT.1700-1 Annex 1 Part B (PAL_M shares the NTSC levels).
inline TbcSinkLevelScale make_tbc_sink_level_scale(VideoSystem system) {
  const bool is_ntsc_like =
      (system == VideoSystem::NTSC || system == VideoSystem::PAL_M);
  const int32_t cvbs_blanking = is_ntsc_like ? kNtscBlanking : kPalBlanking;
  const int32_t cvbs_white = is_ntsc_like ? kNtscWhite : kPalWhite;
  const int32_t tbc_blanking =
      is_ntsc_like ? kTbcNtscBlanking : kTbcPalBlanking;
  const int32_t tbc_white = is_ntsc_like ? kTbcNtscWhite : kTbcPalWhite;

  TbcSinkLevelScale levels;
  levels.scale = static_cast<double>(tbc_white - tbc_blanking) /
                 static_cast<double>(cvbs_white - cvbs_blanking);
  levels.offset =
      static_cast<double>(tbc_blanking) - cvbs_blanking * levels.scale;
  return levels;
}

// The white_16b_ire / black_16b_ire / blanking_16b_ire trio recorded in the
// `.tbc.db` capture row.
struct TbcCaptureLevels {
  int32_t blanking_16b = 0;
  int32_t black_16b = 0;
  int32_t white_16b = 0;
};

// Derive the capture-row levels from the levels the pipeline reports for the
// signal being written, so a non-spec black level survives the export.
//
// This is what carries NTSC-J through the sink: an NTSC-J capture has picture
// black at the 0 IRE blanking level rather than on the SMPTE 170M-2004 §4.4
// 7.5 IRE setup pedestal, and tbc_source reports that as
// SourceParameters::black_level.  Writing the spec pedestal unconditionally
// instead would re-label the capture as standard NTSC and shift its black
// level on the next read (see tbc_level_derivation.h, which classifies a
// re-imported capture from exactly these three values).
//
// Levels the source left unset (-1) fall back to the spec constants for the
// video system.
inline TbcCaptureLevels tbc_capture_levels(const SourceParameters& params) {
  const bool is_ntsc_like = (params.system == VideoSystem::NTSC ||
                             params.system == VideoSystem::PAL_M);
  const TbcSinkLevelScale scale = make_tbc_sink_level_scale(params.system);

  TbcCaptureLevels levels;
  levels.blanking_16b =
      params.blanking_level >= 0
          ? scale.to_tbc_level(params.blanking_level)
          : (is_ntsc_like ? kTbcNtscBlanking : kTbcPalBlanking);
  levels.white_16b = params.white_level > 0
                         ? scale.to_tbc_level(params.white_level)
                         : (is_ntsc_like ? kTbcNtscWhite : kTbcPalWhite);
  // PAL and PAL_M carry no setup pedestal, so black == blanking there.
  levels.black_16b = params.black_level >= 0
                         ? scale.to_tbc_level(params.black_level)
                         : (is_ntsc_like ? kTbcNtscBlack : kTbcPalBlanking);
  return levels;
}

}  // namespace orc
