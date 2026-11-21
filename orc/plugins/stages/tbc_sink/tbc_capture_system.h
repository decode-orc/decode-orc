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
// PAL_M is "PAL-M". SECAM is written as 'PAL', as vhs-decode writes it
// (parent_system() in vhsdecode/formats.py): the two share the 625-line
// raster and the 4fsc lattice, and tbc_capture_notes() records that the
// content is SECAM. Returns nullptr for a system ld-decode cannot describe.
inline const char* tbc_capture_system_name(VideoSystem system) {
  switch (system) {
    case VideoSystem::NTSC:
      return "NTSC";
    case VideoSystem::PAL_M:
      return "PAL_M";
    case VideoSystem::PAL:
    case VideoSystem::SECAM:
      return "PAL";
    case VideoSystem::Unknown:
      break;
  }
  return nullptr;
}

// The capture.capture_notes text for |system|: for SECAM, the one place the
// TBC can say that its 'PAL' capture holds SECAM. A SECAM project's TBC
// Source reads such a TBC as SECAM (tbc_system_resolution.h). Empty for the
// systems the schema names.
inline const char* tbc_capture_notes(VideoSystem system) {
  return system == VideoSystem::SECAM
             ? "SECAM: the content is SECAM (ITU-R BT.1700 Part C). The "
               "ld-decode TBC format has no SECAM system, so capture.system "
               "reads PAL; open this TBC in a SECAM project."
             : "";
}

}  // namespace orc
