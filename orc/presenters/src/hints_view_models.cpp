/*
 * File:        hints_view_models.cpp
 * Module:      orc-presenters
 * Purpose:     Implementation of hint view model conversion functions
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../include/hints_view_models.h"

#include <orc/stage/common_types.h>  // For VideoSystem enum
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/orc_source_parameters.h>

namespace orc::presenters {

VideoParametersView toVideoParametersView(const orc::SourceParameters& params) {
  VideoParametersView v{};

  // Convert VideoSystem enum
  switch (params.system) {
    case orc::VideoSystem::PAL:
      v.system = VideoSystem::PAL;
      break;
    case orc::VideoSystem::NTSC:
      v.system = VideoSystem::NTSC;
      break;
    case orc::VideoSystem::PAL_M:
      v.system = VideoSystem::PAL_M;
      break;
    case orc::VideoSystem::SECAM:
      v.system = VideoSystem::SECAM;
      break;
    case orc::VideoSystem::Unknown:
      v.system = VideoSystem::Unknown;
      break;
  }

  // Canonical CVBS_U10_4FSC fields (not deprecated).
  v.active_video_start = params.active_video_start;
  v.active_video_end = params.active_video_end;

  // Presenter view model fields — derived from the video system.
  // EBU Tech. 3280-E §1.1 (PAL) / SMPTE 244M-2003 §4.1 (NTSC) /
  // ITU-R BT.1700-1 Annex 1 Part B (PAL_M).
  v.frame_width_nominal = params.frame_width_nominal;
  const auto [cb_start, cb_end] = colour_burst_range(params.system);
  v.color_burst_start = cb_start;
  v.color_burst_end = cb_end;

  // CVBS_U10_4FSC 10-bit domain signal levels
  v.sync_tip_level = params.sync_tip_level;
  v.blanking_level = params.blanking_level;
  v.black_level = params.black_level;
  v.white_level = params.white_level;
  v.peak_level = params.peak_level;
  v.chroma_dc_offset = params.chroma_dc_offset;

  return v;
}

}  // namespace orc::presenters
