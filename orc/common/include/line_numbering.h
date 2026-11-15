/*
 * File:        line_numbering.h
 * Module:      orc-common
 * Purpose:     Line numbering mode conversion for CVBS frame-flat line indices
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>  // VideoSystem

#include <cstddef>
#include <stdexcept>
#include <string>

namespace orc {

// ============================================================================
// LineNumberingMode
// ============================================================================
// Controls how a 0-based frame-flat line index is displayed to the user.
//
// Internally, all line positions are stored as 0-based frame-flat indices
// (frame_line ∈ [0, frame_height-1]).  A LineNumberingMode converts that
// internal representation to a human-readable label.
enum class LineNumberingMode {
  // 0-based integer index into the flat frame buffer.  Same as the internal
  // representation; no conversion.
  kFrameFlat0Based,

  // 1-based sequential index across the entire frame.
  // frame_line 0 → 1, frame_line 624 (PAL) → 625.
  kFrameSequential1Based,

  // Field-relative: shows the 1-based field number and 1-based line within
  // that field.  Uses LineLabel::field and LineLabel::line_in_field.
  kFieldRelative,

  // Broadcast-interlaced: the conventional broadcast line number used in
  // analogue video standards (ITU-R BT.470-6 for PAL, SMPTE 170M for NTSC).
  // In this convention, odd-field lines and even-field lines are interleaved,
  // with field 1 starting on line 1 (NTSC) or line 1 (PAL).  Uses
  // LineLabel::broadcast_line.
  kBroadcastInterlaced,
};

// ============================================================================
// LineLabel
// ============================================================================
struct LineLabel {
  // Pre-formatted display string for the current mode.
  std::string display;

  // Field number (1 or 2).  Non-zero only for kFieldRelative.
  int field = 0;

  // 1-based line within the field.  Non-zero only for kFieldRelative.
  int line_in_field = 0;

  // Broadcast line number.  Non-zero only for kBroadcastInterlaced.
  int broadcast_line = 0;
};

// ============================================================================
// make_line_label
// ============================================================================
// Convert a 0-based frame-flat line index to a display label.
//
// PAL broadcast interlace convention (ITU-R BT.470-6 §1.1):
//   Frame-flat line 0 is the first line of field 1.
//   Broadcast lines alternate between fields:
//     field 1 lines → broadcast odd: 1, 3, 5, …, 625
//     field 2 lines → broadcast even: 2, 4, 6, …, 624
//   frame_line i ∈ [0, kPalField1Lines-1] (field 1):
//     broadcast_line = 1 + i * 2
//   frame_line i ∈ [kPalField1Lines, kPalFrameLines-1] (field 2):
//     broadcast_line = 2 + (i - kPalField1Lines) * 2
//
// NTSC broadcast interlace convention (SMPTE 170M-2004 §11.3):
//   VFR field 1 (top spatial, 263 lines) → broadcast odd: 1, 3, 5, …, 525
//   VFR field 2 (bottom spatial, 262 lines) → broadcast even: 2, 4, 6, …, 524
//   frame_line i ∈ [0, 262] (field 1, top):
//     broadcast_line = 1 + i * 2
//   frame_line i ∈ [263, 524] (field 2, bottom):
//     broadcast_line = 2 + (i - 263) * 2
//
// PAL_M follows the same field structure as NTSC (525 lines, 2 fields).
inline LineLabel make_line_label(size_t frame_line, VideoSystem sys,
                                 LineNumberingMode mode) {
  LineLabel label;

  int frame_height = 0;
  int field1_lines = 0;
  if (is_625_line(sys)) {
    // EBU Tech. 3280-E §1.1: 625-line PAL
    // ITU-R BT.1700-1 Annex 1 Part C: SECAM has 625-line PAL timing.
    frame_height = 625;
    field1_lines = 313;
  } else if (sys == VideoSystem::NTSC) {
    // SMPTE 170M-2004 §11.3: 525-line NTSC; VFR field 1 = top spatial (263
    // lines)
    frame_height = 525;
    field1_lines = 263;
  } else if (sys == VideoSystem::PAL_M) {
    // ITU-R BT.1700-1 Annex 1 Part B: 525-line PAL_M; VFR field 1 = top spatial
    // (263 lines)
    frame_height = 525;
    field1_lines = 263;
  } else {
    label.display = "?";
    return label;
  }

  const int iline = static_cast<int>(frame_line);

  switch (mode) {
    case LineNumberingMode::kFrameFlat0Based:
      label.display = std::to_string(iline);
      break;

    case LineNumberingMode::kFrameSequential1Based:
      label.display = std::to_string(iline + 1);
      break;

    case LineNumberingMode::kFieldRelative: {
      if (iline < field1_lines) {
        label.field = 1;
        label.line_in_field = iline + 1;
      } else {
        label.field = 2;
        label.line_in_field = iline - field1_lines + 1;
      }
      label.display = "F" + std::to_string(label.field) + "L" +
                      std::to_string(label.line_in_field);
      break;
    }

    case LineNumberingMode::kBroadcastInterlaced: {
      if (is_625_line(sys)) {
        // ITU-R BT.470-6 §1.1: PAL broadcast interlace
        // Field 1 (lines 0..312): broadcast = 1, 3, 5, …, 625 (odd)
        // Field 2 (lines 313..624): broadcast = 2, 4, 6, …, 624 (even)
        if (iline < field1_lines) {
          label.broadcast_line = 1 + iline * 2;
        } else {
          label.broadcast_line = 2 + (iline - field1_lines) * 2;
        }
      } else {
        // SMPTE 170M-2004 §11.3 / ITU-R BT.1700-1: NTSC/PAL_M broadcast
        // VFR field 1 (top, lines 0..262): broadcast = 1, 3, …, 525 (odd)
        // VFR field 2 (bottom, lines 263..524): broadcast = 2, 4, …, 524 (even)
        if (iline < field1_lines) {
          label.broadcast_line = 1 + iline * 2;
        } else {
          label.broadcast_line = 2 + (iline - field1_lines) * 2;
        }
      }
      label.display = std::to_string(label.broadcast_line);
      break;
    }
  }

  return label;
}

// ============================================================================
// broadcast_line_to_frame_line
// ============================================================================
// Convert a 1-based broadcast line number to the 0-based internal frame-flat
// index.
//
// ITU-R BT.470-6 (PAL) / SMPTE 170M-2004 (NTSC): the broadcast convention
// numbers lines starting at 1.  The internal representation starts at 0.
// This utility performs the straightforward offset; callers are responsible for
// validating that the broadcast_line value is in range for the given system.
inline size_t broadcast_line_to_frame_line(int broadcast_line) {
  return static_cast<size_t>(broadcast_line - 1);
}

}  // namespace orc
