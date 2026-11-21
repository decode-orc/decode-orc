/*
 * File:        hints_view_models.h
 * Module:      orc-presenters
 * Purpose:     View-facing data models for GUI/CLI layers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>

namespace orc::presenters {

/**
 * @brief Video system/format enumeration for presenter layer
 */
enum class VideoSystem {
  PAL,    // 625-line PAL
  NTSC,   // 525-line NTSC
  PAL_M,  // 525-line PAL
  Unknown,
  SECAM  // 625-line SECAM (appended so the other values keep their numbers)
};

/**
 * @brief Video parameters view model for presenter layer
 *
 * Contains all video format and timing parameters needed by GUI.
 * Mirrors core VideoParameters but in presenter layer.
 */
struct VideoParametersView {
  // Format
  VideoSystem system = VideoSystem::Unknown;

  // Frame geometry — canonical field names matching SourceParameters.
  // frame_width_nominal: nominal samples per line (1135 PAL, 910 NTSC, 909
  // PAL_M).
  int frame_width_nominal = -1;

  // Sample ranges
  int color_burst_start = -1;
  int color_burst_end = -1;
  int active_video_start = -1;
  int active_video_end = -1;

  // CVBS_U10_4FSC 10-bit domain signal levels (from SourceParameters).
  // -1 means not populated (source has not been migrated to Phase 3+ pipeline).
  int32_t sync_tip_level = -1;
  int32_t blanking_level = -1;
  int32_t black_level = -1;
  int32_t white_level = -1;
  int32_t peak_level = -1;

  // CVBS_U10_4FSC DC level of the chroma signal for YC sources (-1 = N/A).
  int32_t chroma_dc_offset = -1;
};

}  // namespace orc::presenters

// Include public API types that are used
#include <orc/stage/orc_source_parameters.h>

namespace orc::presenters {

/**
 * @brief Convert core SourceParameters to presenter VideoParametersView
 *
 * This helper function encapsulates the conversion logic to avoid
 * duplication across the codebase.
 */
VideoParametersView toVideoParametersView(const orc::SourceParameters& params);

}  // namespace orc::presenters
