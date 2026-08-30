/*
 * File:        field_frame_presentation.h
 * Module:      orc-gui
 * Purpose:     GUI presentation helpers for field/frame numbering
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef FIELD_FRAME_PRESENTATION_H
#define FIELD_FRAME_PRESENTATION_H

#include <orc/stage/common_types.h>

#include <QString>
#include <cstddef>
#include <cstdint>

namespace orc::gui {

/**
 * @brief GUI Presentation Helpers for Field and Frame Numbering
 *
 * All field and frame information presented to the user must follow a single,
 * consistent numbering convention as defined in:
 * tech-notes/Frame-and-field-number-presentation.md
 *
 * INTERNAL REPRESENTATION (used throughout core):
 * - fieldID: 0-indexed (0, 1, 2, 3, ...)
 * - fieldLineIndex: 0-indexed line number within a field
 *
 * PRESENTATION (user-visible, 1-indexed):
 * - Frames start at 1
 * - Frame lines: 1..625 (PAL) or 1..525 (NTSC)
 * - Presentation field lines within a frame:
 *   - First field (even fieldID): 1..312 (PAL) or 1..262 (NTSC)
 *   - Second field (odd fieldID): 313..625 (PAL) or 263..525 (NTSC)
 */

/**
 * @brief Format a field ID for display in field-only view
 *
 * Converts 0-indexed fieldID to 1-indexed presentation.
 *
 * @param field_id 0-indexed internal field ID
 * @return Formatted string: "Field N" where N is 1-indexed
 *
 * Example: fieldID 0 → "Field 1"
 *          fieldID 1 → "Field 2"
 */
QString formatFieldNumber(uint64_t field_id);

/**
 * @brief Format a field line for display in field-only view
 *
 * Converts 0-indexed fieldLineIndex to 1-indexed presentation.
 *
 * @param field_id 0-indexed internal field ID (for determining field parity)
 * @param field_line_index 0-indexed line within the field
 * @return Formatted string: "line L" where L is 1-indexed
 *
 * Example: fieldLineIndex 0 → "line 1"
 *          fieldLineIndex 311 → "line 312"
 */
QString formatFieldLine(uint64_t field_id, int field_line_index);

/**
 * @brief Format field information with internal representation for debugging
 *
 * Format: "Field F line L [id – fieldLineIndex]"
 *
 * @param field_id 0-indexed internal field ID
 * @param field_line_index 0-indexed line within the field
 * @return Formatted string with both presentation and internal representation
 *
 * Example: fieldID 0, fieldLineIndex 0 → "Field 1 line 1 [0 – 0]"
 *          fieldID 1, fieldLineIndex 0 → "Field 2 line 1 [1 – 0]"
 */
QString formatFieldWithInternal(uint64_t field_id, int field_line_index);

/**
 * @brief Format a frame number for display
 *
 * Converts 0-indexed frame index to 1-indexed frame number.
 *
 * @param frame_index 0-indexed frame index
 * @return Formatted string: "Frame N" where N is 1-indexed
 *
 * Example: frame_index 0 → "Frame 1"
 *          frame_index 61 → "Frame 62"
 */
QString formatFrameNumber(uint64_t frame_index);

/**
 * @brief Get frame number from field ID
 *
 * A frame N consists of:
 * - Even field: fieldID = 2 × (N − 1)
 * - Odd field:  fieldID = 2 × (N − 1) + 1
 *
 * @param field_id 0-indexed internal field ID
 * @return 1-indexed frame number
 *
 * Example: fieldID 0 → Frame 1 (first field)
 *          fieldID 1 → Frame 1 (second field)
 *          fieldID 2 → Frame 2 (first field)
 */
uint64_t getFrameNumberFromFieldID(uint64_t field_id);

/**
 * @brief Get 1-indexed field number for display
 *
 * @param field_id 0-indexed internal field ID
 * @return 1-indexed field number (fieldID + 1)
 *
 * Example: fieldID 0 → 1 (Field 1)
 *          fieldID 1 → 2 (Field 2)
 *          fieldID 2 → 3 (Field 3)
 */
int getFieldWithinFrame(uint64_t field_id);

/**
 * @brief Get presentation field line number within a frame
 *
 * Presentation field lines are continuous across the two fields that make up a
 * frame:
 * - First field (even):  1..312 (PAL) or 1..262 (NTSC)
 * - Second field (odd):  313..625 (PAL) or 263..525 (NTSC)
 *
 * @param field_id 0-indexed internal field ID
 * @param field_line_index 0-indexed line within the field
 * @param is_pal True for PAL (625 lines), false for NTSC (525 lines)
 * @return 1-indexed presentation field line number (1..625 for PAL, 1..525 for
 * NTSC)
 *
 * Example (PAL):
 *   fieldID 0, fieldLineIndex 0 → 1
 *   fieldID 0, fieldLineIndex 311 → 312
 *   fieldID 1, fieldLineIndex 0 → 313
 *   fieldID 1, fieldLineIndex 312 → 625
 */
int getPresentationFieldLine(uint64_t field_id, int field_line_index,
                             bool is_pal);

/**
 * @brief Get interlaced frame line number for frame view display
 *
 * Converts field representation to actual interlaced frame line number.
 * In interlaced video, frame lines alternate between fields:
 * - Frame line 1 = Field 0, line 0
 * - Frame line 2 = Field 1, line 0
 * - Frame line 3 = Field 0, line 1
 * - Frame line 4 = Field 1, line 1
 * - etc.
 *
 * @param field_id 0-indexed internal field ID
 * @param field_line_index 0-indexed line within the field
 * @param is_pal True for PAL (625 lines), false for NTSC (525 lines)
 * @return Interlaced frame line number (1-625 for PAL, 1-525 for NTSC)
 *
 * Example:
 *   fieldID 0, fieldLineIndex 0 → 1
 *   fieldID 1, fieldLineIndex 0 → 2
 *   fieldID 0, fieldLineIndex 1 → 3
 *   fieldID 1, fieldLineIndex 1 → 4
 */
int getInterlacedFrameLine(uint64_t field_id, int field_line_index,
                           bool is_pal);

/**
 * @brief Format complete frame view information
 *
 * Format: "Frame <N> line <L> (Field <F> line <FL>) [<id> – <fieldLineIndex>]"
 * where <F> is the 1-indexed fieldID
 *
 * @param field_id 0-indexed internal field ID
 * @param field_line_index 0-indexed line within the field
 * @param is_pal True for PAL (625 lines), false for NTSC (525 lines)
 * @return Formatted string with frame, field, and internal representation
 *
 * Examples (Frame 1, PAL):
 *   fieldID 0, fieldLineIndex 0 → "Frame 1 line 1 (Field 1 line 1) [0 – 0]"
 *   fieldID 1, fieldLineIndex 0 → "Frame 1 line 313 (Field 2 line 313) [1 – 0]"
 *
 * Examples (Frame 2, PAL):
 *   fieldID 2, fieldLineIndex 0 → "Frame 2 line 1 (Field 3 line 1) [2 – 0]"
 *   fieldID 3, fieldLineIndex 0 → "Frame 2 line 313 (Field 4 line 313) [3 – 0]"
 */
QString formatFrameViewWithInternal(uint64_t field_id, int field_line_index,
                                    bool is_pal);

/**
 * @brief Convert a field-relative line to its frame-flat index
 *
 * The line scope addresses lines as (field ID, 0-based line within that
 * field), but orc::make_line_label() - and the frame-flat convention it
 * documents - numbers a frame's lines as one block per field: field 1 occupies
 * indices 0..field1_lines-1 and field 2 follows it.
 *
 * Without this conversion both fields report the same label, so the two
 * interleaved lines either side of a frame row are indistinguishable.
 *
 * @param field_id 0-indexed internal field ID (its parity selects the field)
 * @param field_line_index 0-indexed line within the field
 * @param system Video system, which fixes the field 1 line count
 * @return 0-based frame-flat line index
 *
 * Example (PAL):
 *   fieldID 0, fieldLineIndex 0 → 0
 *   fieldID 1, fieldLineIndex 0 → 313
 *   fieldID 2, fieldLineIndex 5 → 5
 *   fieldID 3, fieldLineIndex 5 → 318
 */
size_t frameFlatLineIndex(uint64_t field_id, int field_line_index,
                          orc::VideoSystem system);

/**
 * @brief Format field range for a frame (for VBI dialog showing both fields)
 *
 * Shows the 1-indexed field IDs that comprise a frame.
 *
 * @param frame_index 0-indexed frame index
 * @return Formatted string: "Field F1 - F2" where both are 1-indexed fieldIDs
 *
 * Example: frame_index 0 → "Field 1 - 2"   (fieldIDs 0 and 1)
 *          frame_index 1 → "Field 3 - 4"   (fieldIDs 2 and 3)
 *          frame_index 2 → "Field 5 - 6"   (fieldIDs 4 and 5)
 */
QString formatFrameFieldRange(uint64_t frame_index);

}  // namespace orc::gui

#endif  // FIELD_FRAME_PRESENTATION_H
