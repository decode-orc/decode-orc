/*
 * File:        field_frame_presentation.cpp
 * Module:      orc-gui
 * Purpose:     GUI presentation helpers for field/frame numbering
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "field_frame_presentation.h"

#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <cstddef>

namespace orc::gui {

QString formatFieldNumber(uint64_t field_id) {
  // Convert 0-indexed to 1-indexed
  return QString("Field %1").arg(field_id + 1);
}

QString formatFieldLine(uint64_t field_id, int field_line_index) {
  Q_UNUSED(field_id);
  // Convert 0-indexed to 1-indexed
  return QString("line %1").arg(field_line_index + 1);
}

QString formatFieldWithInternal(uint64_t field_id, int field_line_index) {
  // Field F line L [id – fieldLineIndex]
  return QString("Field %1 line %2 [%3 – %4]")
      .arg(field_id + 1)          // Presentation field number (1-indexed)
      .arg(field_line_index + 1)  // Presentation field line (1-indexed)
      .arg(field_id)              // Internal fieldID (0-indexed)
      .arg(field_line_index);     // Internal fieldLineIndex (0-indexed)
}

QString formatFrameNumber(uint64_t frame_index) {
  // Convert 0-indexed to 1-indexed
  return QString("Frame %1").arg(frame_index + 1);
}

uint64_t getFrameNumberFromFieldID(uint64_t field_id) {
  // Frame number (1-based) from fieldID (0-based)
  // Frame N consists of fields (2×(N-1)) and (2×(N-1)+1)
  // So: field_id → frame_number = (field_id / 2) + 1
  return (field_id / 2) + 1;
}

int getFieldWithinFrame(uint64_t field_id) {
  // Return 1-indexed field number for display
  // fieldID 0 → Field 1, fieldID 1 → Field 2, etc.
  return static_cast<int>(field_id + 1);
}

int getPresentationFieldLine(uint64_t field_id, int field_line_index,
                             bool is_pal) {
  bool is_first_field = (field_id % 2 == 0);
  // ITU-R BT.1700 / EBU Tech. 3280-E: PAL field 1 (odd-scan) has 313 lines.
  int first_field_height = is_pal ? 313 : 262;

  if (is_first_field) {
    // First field: lines 1..313 (PAL) or 1..262 (NTSC)
    return field_line_index + 1;
  } else {
    // Second field: lines 314..625 (PAL) or 263..525 (NTSC)
    return first_field_height + field_line_index + 1;
  }
}

int getInterlacedFrameLine(uint64_t field_id, int field_line_index,
                           bool is_pal) {
  // Convert field representation to actual interlaced frame line number
  // In interlaced video, frame lines alternate between fields:
  // Frame line 1 = Field 0, line 0
  // Frame line 2 = Field 1, line 0
  // Frame line 3 = Field 0, line 1
  // Frame line 4 = Field 1, line 1
  // ...
  // Formula: frame_line = field_line_index * 2 + (field_id % 2) + 1
  int frame_line = static_cast<int>(field_line_index * 2) +
                   static_cast<int>(field_id % 2) + 1;

  // Cap at total frame height (PAL: 625, NTSC: 525)
  int total_lines = is_pal ? 625 : 525;
  return std::min(frame_line, total_lines);
}

QString formatFrameViewWithInternal(uint64_t field_id, int field_line_index,
                                    bool is_pal) {
  uint64_t frame_number = getFrameNumberFromFieldID(field_id);
  int field_number = getFieldWithinFrame(field_id);
  int interlaced_frame_line =
      getInterlacedFrameLine(field_id, field_line_index, is_pal);
  int presentation_field_line =
      getPresentationFieldLine(field_id, field_line_index, is_pal);

  // Frame <N> line <L> (Field <F> line <FL_PRESENTATION>) [<id> –
  // <fieldLineIndex>]
  return QString("Frame %1 line %2 (Field %3 line %4) [%5 – %6]")
      .arg(frame_number)             // Frame number (1-indexed)
      .arg(interlaced_frame_line)    // Interlaced frame line (1-625)
      .arg(field_number)             // Field number (1-indexed fieldID)
      .arg(presentation_field_line)  // Presentation field line (continuous
                                     // 1-312 or 313-625)
      .arg(field_id)                 // Internal fieldID (0-indexed)
      .arg(field_line_index);        // Internal fieldLineIndex (0-indexed)
}

size_t frameFlatLineIndex(uint64_t field_id, int field_line_index,
                          orc::VideoSystem system) {
  const size_t line = static_cast<size_t>(std::max(0, field_line_index));
  const bool is_first_field = (field_id % 2) == 0;
  return is_first_field ? line : orc::field1_lines(system) + line;
}

QString formatFrameFieldRange(uint64_t frame_index) {
  // Frame at index I consists of fields (I*2) and (I*2+1) in 0-indexed terms
  // Convert to 1-indexed for display
  uint64_t first_field_id = frame_index * 2;      // 0-indexed fieldID
  uint64_t second_field_id = first_field_id + 1;  // 0-indexed fieldID

  // Convert to 1-indexed presentation
  uint64_t first_field = first_field_id + 1;
  uint64_t second_field = second_field_id + 1;

  return QString("Field %1 - %2").arg(first_field).arg(second_field);
}

}  // namespace orc::gui
