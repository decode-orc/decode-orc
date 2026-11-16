/*
 * File:        orc_source_parameters.h
 * Module:      decode-orc Plugin SDK (stage contract)
 * Purpose:     Source metadata types
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>  // For VideoSystem enum

#include <array>
#include <cstdint>
#include <string>

namespace orc {

// ============================================================================
// SourceParameters — CVBS_U10_4FSC signal geometry and level domain
// ============================================================================
// This struct carries the signal parameters that stages need to interpret
// frame sample buffers correctly.  All level fields are in the
// CVBS_U10_4FSC 10-bit domain (0–1023).
struct SourceParameters {
  // ---------------------------------------------------------------------------
  // Identity
  // ---------------------------------------------------------------------------
  VideoSystem system = VideoSystem::Unknown;
  bool is_widescreen = false;

  // The phase-locked axis of the CVBS Signal State Preset (CVBS file format
  // spec v1.8.1, signal-state-presets.md, "Subcarrier phase analysis"): true
  // when each sample's subcarrier phase follows from its position in the
  // frame, so the colour-sequence phase can be computed from the burst at
  // fixed sample positions.  When false, colour_frame_phase reports every id
  // as unknown (-1).  Sources set it from what the input declares
  // (CVBS Source: signal_state_preset; TBC Source: see its instructions).
  //
  // Sits in the padding after is_widescreen, so the struct's size and every
  // other member's offset are unchanged (plugins released against ABI 21
  // read SourceParameters and never see this byte).
  bool is_subcarrier_locked = true;

  // ---------------------------------------------------------------------------
  // Frame geometry (CVBS_U10_4FSC domain)
  // ---------------------------------------------------------------------------

  // Nominal samples per line (1135 PAL, 910 NTSC, 909 PAL_M).
  // PAL frames may have up to 4 lines with one extra sample.
  int32_t frame_width_nominal = -1;

  // Total lines per frame (625 PAL, 525 NTSC/PAL_M).
  int32_t frame_height = -1;

  // Total number of frames available from this source.
  int32_t number_of_sequential_frames = -1;

  // ---------------------------------------------------------------------------
  // Signal levels (CVBS_U10_4FSC 10-bit domain)
  // ---------------------------------------------------------------------------
  // Spec-defined normative values are in cvbs_signal_constants.h.
  // These fields hold the values actually used by this source; for standard
  // signals they equal the spec constants.  has_nonstandard_values is set when
  // they differ.

  int32_t sync_tip_level = -1;  // Sync tip (e.g., kPalSyncTip = 4)
  int32_t blanking_level = -1;  // Blanking / 0 IRE (e.g., kPalBlanking = 256)
  int32_t black_level = -1;     // Black level (e.g., kPalBlack = 282)
  int32_t white_level = -1;     // White / 100 IRE (e.g., kPalWhite = 844)
  int32_t peak_level = -1;      // Peak white (e.g., kPalPeak = 1019)

  // CVBS_U10_4FSC DC level of the chroma signal for YC sources.
  // Raw .tbcc chroma is centred at 32768 (uint16 midpoint); this is that value
  // converted to CVBS domain via tbc_to_cvbs(32768, blanking_16b, white_16b).
  // -1 when not applicable (composite sources).
  int32_t chroma_dc_offset = -1;

  // True when black_level or white_level differs from the spec-defined
  // constant for this system (e.g., NTSC-J, or a video_params override).
  bool has_nonstandard_values = false;

  // ---------------------------------------------------------------------------
  // Active picture geometry
  // ---------------------------------------------------------------------------
  // Sample offsets within each line for the active video region.
  int32_t active_video_start = -1;
  int32_t active_video_end = -1;

  // 0-based frame-flat line indices for the active picture area.
  int32_t first_active_frame_line = -1;
  int32_t last_active_frame_line = -1;

  // ---------------------------------------------------------------------------
  // Source metadata
  // ---------------------------------------------------------------------------
  bool is_mapped = false;
  std::string tape_format;
  std::string decoder;  // e.g., "ld-decode", "vhs-decode"
  std::string git_branch;
  std::string git_commit;

  // True when active_video_start/end and first/last_active_frame_line have
  // been applied as a crop.  Decoder output stages write to 0-based
  // ComponentFrame coordinates when this is set.
  bool active_area_cropping_applied = false;

  bool is_valid() const {
    return system != VideoSystem::Unknown && frame_width_nominal > 0;
  }
};

}  // namespace orc
