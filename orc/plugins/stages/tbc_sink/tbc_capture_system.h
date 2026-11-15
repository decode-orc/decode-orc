/*
 * File:        tbc_capture_system.h
 * Module:      orc-stage-plugin-tbc-sink
 * Purpose:     ld-decode spelling of the video system for capture.system
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>

namespace orc {

// The value written to the .tbc.db capture.system column. ld-decode's schema
// admits only 'NTSC', 'PAL' and 'PAL_M' (CHECK constraint), so this keeps its
// own spelling rather than video_system_to_string(), whose display name for
// PAL_M is "PAL-M". Returns nullptr for a system ld-decode cannot describe.
inline const char* tbc_capture_system_name(VideoSystem system) {
  switch (system) {
    case VideoSystem::PAL:
      return "PAL";
    case VideoSystem::NTSC:
      return "NTSC";
    case VideoSystem::PAL_M:
      return "PAL_M";
    default:
      return nullptr;
  }
}

}  // namespace orc
