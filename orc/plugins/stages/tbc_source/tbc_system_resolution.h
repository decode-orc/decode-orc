/*
 * File:        tbc_system_resolution.h
 * Module:      orc-stage-plugin-tbc_source
 * Purpose:     Decides the video system a TBC is read as, from its metadata
 *              label and the project's video format
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef TBC_SYSTEM_RESOLUTION_H
#define TBC_SYSTEM_RESOLUTION_H

#include <orc/stage/common_types.h>

#include <string>

namespace orc {

// The video system a TBC is read as.
//
// A TBC cannot say it carries SECAM: the ld-decode schema's `system` has no
// SECAM, and vhs-decode writes "PAL" for SECAM and MESECAM captures
// (parent_system() in vhsdecode/formats.py). So the project format decides:
//  - In a SECAM project, a 625-line TBC (labelled PAL, or SECAM) is read as
//    SECAM. SECAM shares PAL's raster and line-locked lattice (ITU-R BT.1700-1
//    Annex 1 Part C), so only the label changes.
//  - In any other project, a TBC labelled SECAM is refused: its chroma is FM
//    and no PAL, NTSC or PAL-M stage can use it.
//  - Otherwise, and with no project format (an empty string), the label
//    stands. A PAL/NTSC mismatch is the project's check, not this one.
//
// |project_system| is the kProjectVideoSystemParameter value. Returns
// VideoSystem::Unknown with |error_message| set when the TBC cannot be read
// in this project.
inline VideoSystem resolve_tbc_system(VideoSystem labelled,
                                      const std::string& project_system,
                                      std::string& error_message) {
  const VideoSystem project = video_system_from_string(project_system);
  if (project == VideoSystem::SECAM) {
    if (is_625_line(labelled)) {
      return VideoSystem::SECAM;
    }
    error_message = "a SECAM project needs a 625-line TBC, but this one is " +
                    video_system_to_string(labelled);
    return VideoSystem::Unknown;
  }
  if (labelled == VideoSystem::SECAM && project != VideoSystem::Unknown) {
    error_message = "the TBC is labelled SECAM, which a " +
                    video_system_to_string(project) +
                    " project cannot use; open it in a SECAM project";
    return VideoSystem::Unknown;
  }
  return labelled;
}

}  // namespace orc

#endif  // TBC_SYSTEM_RESOLUTION_H
