/*
 * File:        field_frame_presentation_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Unit tests for field and frame numbering presentation helpers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QString>

// Include headers from their installed locations
#include "field_frame_presentation.h"

using namespace orc::gui;

namespace gui_unit_test {

// =============================================================================
// Field Number Formatting Tests
// =============================================================================

TEST(FieldFramePresentationTest, FormatFieldNumberZero_ReturnsField1) {
  // 0-indexed field ID 0 should format as "Field 1"
  QString result = formatFieldNumber(0);
  EXPECT_EQ(result.toStdString(), "Field 1");
}

TEST(FieldFramePresentationTest, FormatFieldNumberOne_ReturnsField2) {
  // 0-indexed field ID 1 should format as "Field 2"
  QString result = formatFieldNumber(1);
  EXPECT_EQ(result.toStdString(), "Field 2");
}

TEST(FieldFramePresentationTest, Format_FieldNumberLargeValues) {
  // Test with larger values to ensure correct conversion
  QString result = formatFieldNumber(99);
  EXPECT_EQ(result.toStdString(), "Field 100");

  result = formatFieldNumber(1000);
  EXPECT_EQ(result.toStdString(), "Field 1001");
}

TEST(FieldFramePresentationTest, FormatFieldNumber_IsOneIndexed) {
  // Verify consistent 1-indexed output for multiple values
  for (uint64_t i = 0; i < 10; ++i) {
    QString result = formatFieldNumber(i);
    uint64_t expected_number = i + 1;
    QString expected = QString("Field %1").arg(expected_number);
    EXPECT_EQ(result, expected);
  }
}

// =============================================================================
// Field Line Formatting Tests
// =============================================================================

TEST(FieldFramePresentationTest, FormatFieldLineZero_ReturnsLine1) {
  // 0-indexed field line 0 should format as "line 1"
  QString result = formatFieldLine(0, 0);
  EXPECT_EQ(result.toStdString(), "line 1");
}

TEST(FieldFramePresentationTest, FormatFieldLine_IgnoresFieldId) {
  // Field line numbering should not depend on field ID (per function contract)
  QString result1 = formatFieldLine(0, 100);
  QString result2 = formatFieldLine(1, 100);
  EXPECT_EQ(result1, result2);  // Both should produce "line 101"
}

TEST(FieldFramePresentationTest, Format_FieldLineLargeValues) {
  // Test with larger line indices
  QString result = formatFieldLine(0, 624);  // Last line in PAL field
  EXPECT_EQ(result.toStdString(), "line 625");
}

TEST(FieldFramePresentationTest, FormatFieldLine_IsOneIndexed) {
  // Verify consistent 1-indexed output for multiple values
  for (int i = 0; i < 20; ++i) {
    QString result = formatFieldLine(0, i);
    int expected_line = i + 1;
    QString expected = QString("line %1").arg(expected_line);
    EXPECT_EQ(result, expected);
  }
}

// =============================================================================
// Field With Internal Representation Formatting Tests
// =============================================================================

TEST(FieldFramePresentationTest, Format_FieldWithInternalZeroZero) {
  // Field ID 0, line 0 should show Field 1 line 1 with internal [0 – 0]
  QString result = formatFieldWithInternal(0, 0);
  EXPECT_EQ(result.toStdString(), "Field 1 line 1 [0 – 0]");
}

TEST(FieldFramePresentationTest, Format_FieldWithInternalOneZero) {
  // Field ID 1 (second field), line 0 should show Field 2 line 1 with internal
  // [1 – 0]
  QString result = formatFieldWithInternal(1, 0);
  EXPECT_EQ(result.toStdString(), "Field 2 line 1 [1 – 0]");
}

TEST(FieldFramePresentationTest, Format_FieldWithInternalZeroLarge) {
  // Field ID 0, line 624 should show internal representation correctly
  QString result = formatFieldWithInternal(0, 624);
  EXPECT_EQ(result.toStdString(), "Field 1 line 625 [0 – 624]");
}

TEST(FieldFramePresentationTest,
     Format_FieldWithInternalIncludesInternalRepresentation) {
  // Verify that internal representation is included in the output
  QString result = formatFieldWithInternal(5, 123);
  EXPECT_TRUE(result.contains("[5 – 123]"));  // Check internal values
  EXPECT_TRUE(result.contains("Field 6"));    // Check presentation field number
  EXPECT_TRUE(result.contains("line 124"));   // Check presentation line number
}

// =============================================================================
// Frame Number Formatting Tests
// =============================================================================

TEST(FramePresentationTest, FormatFrameNumberZero_ReturnsFrame1) {
  // 0-indexed frame 0 should format as "Frame 1"
  QString result = formatFrameNumber(0);
  EXPECT_EQ(result.toStdString(), "Frame 1");
}

TEST(FramePresentationTest, FormatFrameNumberOne_ReturnsFrame2) {
  // 0-indexed frame 1 should format as "Frame 2"
  QString result = formatFrameNumber(1);
  EXPECT_EQ(result.toStdString(), "Frame 2");
}

TEST(FramePresentationTest, Format_FrameNumberLargeValues) {
  // Test with larger values to ensure correct conversion
  QString result = formatFrameNumber(99);
  EXPECT_EQ(result.toStdString(), "Frame 100");

  result = formatFrameNumber(1000);
  EXPECT_EQ(result.toStdString(), "Frame 1001");
}

TEST(FramePresentationTest, FormatFrameNumber_IsOneIndexed) {
  // Verify consistent 1-indexed output for multiple values
  for (uint64_t i = 0; i < 10; ++i) {
    QString result = formatFrameNumber(i);
    uint64_t expected_number = i + 1;
    QString expected = QString("Frame %1").arg(expected_number);
    EXPECT_EQ(result, expected);
  }
}

// =============================================================================
// Frame Number From Field ID Tests
// =============================================================================

TEST(FrameFieldConversionTest, Get_FrameNumberFromFieldIDEvenFields) {
  // Field ID 0 → Frame 1, Field 2 → Frame 2, Field 4 → Frame 3, etc.
  EXPECT_EQ(getFrameNumberFromFieldID(0), 1UL);     // fields 0,1 → frame 1
  EXPECT_EQ(getFrameNumberFromFieldID(2), 2UL);     // fields 2,3 → frame 2
  EXPECT_EQ(getFrameNumberFromFieldID(4), 3UL);     // fields 4,5 → frame 3
  EXPECT_EQ(getFrameNumberFromFieldID(100), 51UL);  // fields 100,101 → frame 51
}

TEST(FrameFieldConversionTest, Get_FrameNumberFromFieldIDOddFields) {
  // Odd field IDs should produce the same frame as the preceding even field
  EXPECT_EQ(getFrameNumberFromFieldID(1),
            1UL);  // field 1 (with field 0) → frame 1
  EXPECT_EQ(getFrameNumberFromFieldID(3),
            2UL);  // field 3 (with field 2) → frame 2
  EXPECT_EQ(getFrameNumberFromFieldID(5),
            3UL);  // field 5 (with field 4) → frame 3
  EXPECT_EQ(getFrameNumberFromFieldID(101),
            51UL);  // field 101 (with field 100) → frame 51
}

TEST(FrameFieldConversionTest, Get_FrameNumberFromFieldIDFrameConsistency) {
  // Both fields in a frame should return the same frame number
  for (uint64_t frame_idx = 0; frame_idx < 100; ++frame_idx) {
    uint64_t even_field_id = frame_idx * 2;
    uint64_t odd_field_id = frame_idx * 2 + 1;

    uint64_t frame_from_even = getFrameNumberFromFieldID(even_field_id);
    uint64_t frame_from_odd = getFrameNumberFromFieldID(odd_field_id);

    EXPECT_EQ(frame_from_even, frame_from_odd)
        << "Fields " << even_field_id << " and " << odd_field_id
        << " should produce the same frame number";
  }
}

// =============================================================================
// Field Within Frame Tests
// =============================================================================

TEST(FrameFieldConversionTest, GetFieldWithinFrameZero_ReturnsOne) {
  // Field ID 0 is the first field (1-indexed output)
  int result = getFieldWithinFrame(0);
  EXPECT_EQ(result, 1);
}

TEST(FrameFieldConversionTest, GetFieldWithinFrameOne_ReturnsTwo) {
  // Field ID 1 is the second field (1-indexed output)
  int result = getFieldWithinFrame(1);
  EXPECT_EQ(result, 2);
}

TEST(FrameFieldConversionTest, Get_FieldWithinFrameAlternates) {
  // Field within frame returns 1-indexed field number, not alternating pattern
  // Field ID 0 → 1, Field ID 1 → 2, Field ID 2 → 3, etc.
  for (int i = 0; i < 10; ++i) {
    int result = getFieldWithinFrame(i);
    int expected = i + 1;  // Just add 1 to field ID
    EXPECT_EQ(result, expected) << "Field ID " << i;
  }
}

// =============================================================================
// Presentation Field Line Tests
// =============================================================================

TEST(PresentationLineTest, GetPresentationFieldLinePALFirstField_StartsAt1) {
  // PAL field 1 (odd-scan, isFirstField, even fieldID): 313 lines, 1..313.
  // EBU Tech. 3280-E §1.1: lines 1, 3, 5, …, 625 → 313 stored lines.
  int line = getPresentationFieldLine(0, 0, true);  // fieldID 0, line 0
  EXPECT_EQ(line, 1);

  line = getPresentationFieldLine(0, 312, true);  // fieldID 0, line 312
  EXPECT_EQ(line, 313);
}

TEST(PresentationLineTest, GetPresentationFieldLinePALSecondField_StartsAt314) {
  // PAL field 2 (even-scan, odd fieldID): 312 lines, 314..625.
  // EBU Tech. 3280-E §1.1: lines 2, 4, 6, …, 624 → 312 stored lines.
  int line = getPresentationFieldLine(1, 0, true);  // fieldID 1, line 0
  EXPECT_EQ(line, 314);

  line = getPresentationFieldLine(1, 311, true);  // fieldID 1, line 311
  EXPECT_EQ(line, 625);
}

TEST(PresentationLineTest, GetPresentationFieldLineNTSCFirstField_StartsAt1) {
  // NTSC first field (even fieldID): lines 1..262
  int line = getPresentationFieldLine(0, 0, false);  // fieldID 0, line 0
  EXPECT_EQ(line, 1);

  line = getPresentationFieldLine(0, 261, false);  // fieldID 0, line 261
  EXPECT_EQ(line, 262);
}

TEST(PresentationLineTest,
     GetPresentationFieldLineNTSCSecondField_StartsAt263) {
  // NTSC second field (odd fieldID): lines 263..525
  int line = getPresentationFieldLine(1, 0, false);  // fieldID 1, line 0
  EXPECT_EQ(line, 263);

  line = getPresentationFieldLine(1, 262, false);  // fieldID 1, line 262
  EXPECT_EQ(line, 525);
}

// ---- frameFlatLineIndex() --------------------------------------------------
// The line scope addresses lines per field; orc::make_line_label() numbers them
// frame-flat (field 2's block follows field 1's).

TEST(FrameFlatLineIndexTest, FirstFieldLinesAreUnchanged) {
  EXPECT_EQ(frameFlatLineIndex(0, 0, orc::VideoSystem::PAL), 0u);
  EXPECT_EQ(frameFlatLineIndex(0, 312, orc::VideoSystem::PAL), 312u);
  // Field parity, not the frame, selects the block.
  EXPECT_EQ(frameFlatLineIndex(4, 17, orc::VideoSystem::PAL), 17u);
}

TEST(FrameFlatLineIndexTest, SecondFieldLinesFollowTheFirstFieldBlock) {
  // EBU Tech. 3280-E §1.1: PAL field 1 holds 313 lines.
  EXPECT_EQ(frameFlatLineIndex(1, 0, orc::VideoSystem::PAL), 313u);
  EXPECT_EQ(frameFlatLineIndex(1, 311, orc::VideoSystem::PAL), 624u);
  EXPECT_EQ(frameFlatLineIndex(5, 17, orc::VideoSystem::PAL), 330u);
}

TEST(FrameFlatLineIndexTest, SecondFieldUsesTheSystemsFieldOneHeight) {
  // SMPTE 170M-2004 §11.3: NTSC VFR field 1 holds 263 lines.
  EXPECT_EQ(frameFlatLineIndex(1, 0, orc::VideoSystem::NTSC), 263u);
  EXPECT_EQ(frameFlatLineIndex(1, 0, orc::VideoSystem::PAL_M), 263u);
}

TEST(FrameFlatLineIndexTest, BothFieldsOfAFrameGetDistinctIndices) {
  // The defect this guards: without the block offset, the two interleaved
  // lines either side of a weaved preview row report the same label.
  EXPECT_NE(frameFlatLineIndex(2, 120, orc::VideoSystem::PAL),
            frameFlatLineIndex(3, 120, orc::VideoSystem::PAL));
}

}  // namespace gui_unit_test
