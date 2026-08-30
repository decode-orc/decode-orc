/*
 * File:        preview_frame_layout_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for preview row <-> field line mapping
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <preview_frame_layout.h>

namespace orc_unit_test {

namespace {

// EBU Tech. 3280-E Section 1.2: 625-line PAL frame, field 1 carries 313 lines
// and field 2 carries 312.
constexpr orc::PreviewFieldGeometry kPalGeometry{313, 312};

// SMPTE 170M-2004 Section 11.3: 525-line frame split 263/262.
constexpr orc::PreviewFieldGeometry kNtscGeometry{263, 262};

constexpr uint64_t kFrameIndex = 7;
constexpr uint64_t kField1 = kFrameIndex * 2;
constexpr uint64_t kField2 = kFrameIndex * 2 + 1;

}  // namespace

// ---------------------------------------------------------------------------
// Layout selection from the preview option id
// ---------------------------------------------------------------------------

TEST(PreviewFrameLayoutTest,
     LayoutForOption_ReportsSequential_ForSignalDomainOptions) {
  EXPECT_EQ(orc::preview_frame_layout_for_option("sequential_clamped"),
            orc::PreviewFrameLayout::FieldSequential);
  EXPECT_EQ(orc::preview_frame_layout_for_option("sequential_raw"),
            orc::PreviewFrameLayout::FieldSequential);
  // The GUI appends a channel suffix for Y/C sources.
  EXPECT_EQ(orc::preview_frame_layout_for_option("sequential_clamped_y"),
            orc::PreviewFrameLayout::FieldSequential);
  EXPECT_EQ(orc::preview_frame_layout_for_option("sequential_raw_yc"),
            orc::PreviewFrameLayout::FieldSequential);
}

TEST(PreviewFrameLayoutTest,
     LayoutForOption_ReportsSequential_ForColourCarrierOption) {
  EXPECT_EQ(
      orc::preview_frame_layout_for_option("phase2_colour_carrier_sequential"),
      orc::PreviewFrameLayout::FieldSequential);
}

TEST(PreviewFrameLayoutTest,
     LayoutForOption_ReportsWeaved_ForInterlacedAndUnknownOptions) {
  EXPECT_EQ(orc::preview_frame_layout_for_option("interlaced_clamped"),
            orc::PreviewFrameLayout::Weaved);
  EXPECT_EQ(orc::preview_frame_layout_for_option("phase2_colour_carrier"),
            orc::PreviewFrameLayout::Weaved);
  EXPECT_EQ(orc::preview_frame_layout_for_option(""),
            orc::PreviewFrameLayout::Weaved);
}

// ---------------------------------------------------------------------------
// Weaved frame rows alternate between the fields
// ---------------------------------------------------------------------------

TEST(PreviewFrameLayoutTest, RowToField_AlternatesFields_ForWeavedFrame) {
  const auto even = orc::map_preview_row_to_field(
      orc::PreviewOutputType::Frame_Field1_First,
      orc::PreviewFrameLayout::Weaved, kFrameIndex, 100, kPalGeometry);
  ASSERT_TRUE(even.is_valid);
  EXPECT_EQ(even.field_index, kField1);
  EXPECT_EQ(even.field_line, 50);

  const auto odd = orc::map_preview_row_to_field(
      orc::PreviewOutputType::Frame_Field1_First,
      orc::PreviewFrameLayout::Weaved, kFrameIndex, 101, kPalGeometry);
  ASSERT_TRUE(odd.is_valid);
  EXPECT_EQ(odd.field_index, kField2);
  EXPECT_EQ(odd.field_line, 50);
}

// ---------------------------------------------------------------------------
// Field-sequential frames stack field 1 above field 2
// ---------------------------------------------------------------------------

TEST(PreviewFrameLayoutTest,
     RowToField_MapsTopBlockToField1_ForSequentialFrame) {
  const auto top = orc::map_preview_row_to_field(
      orc::PreviewOutputType::Frame_Field1_First,
      orc::PreviewFrameLayout::FieldSequential, kFrameIndex, 100, kPalGeometry);
  ASSERT_TRUE(top.is_valid);
  EXPECT_EQ(top.field_index, kField1);
  EXPECT_EQ(top.field_line, 100);
}

TEST(PreviewFrameLayoutTest,
     RowToField_MapsBottomBlockToField2_ForSequentialFrame) {
  // First row of the bottom block is field 2 line 0.
  const auto first = orc::map_preview_row_to_field(
      orc::PreviewOutputType::Frame_Field1_First,
      orc::PreviewFrameLayout::FieldSequential, kFrameIndex, 313, kPalGeometry);
  ASSERT_TRUE(first.is_valid);
  EXPECT_EQ(first.field_index, kField2);
  EXPECT_EQ(first.field_line, 0);

  const auto later = orc::map_preview_row_to_field(
      orc::PreviewOutputType::Frame_Field1_First,
      orc::PreviewFrameLayout::FieldSequential, kFrameIndex, 400, kPalGeometry);
  ASSERT_TRUE(later.is_valid);
  EXPECT_EQ(later.field_index, kField2);
  EXPECT_EQ(later.field_line, 87);
}

TEST(PreviewFrameLayoutTest,
     RowToField_RejectsRowsPastFrame_ForSequentialFrame) {
  const auto below = orc::map_preview_row_to_field(
      orc::PreviewOutputType::Frame_Field1_First,
      orc::PreviewFrameLayout::FieldSequential, kFrameIndex, 625, kPalGeometry);
  EXPECT_FALSE(below.is_valid);
}

TEST(PreviewFrameLayoutTest,
     RowToField_MapsBottomBlockToField2_ForNtscSequentialFrame) {
  const auto first =
      orc::map_preview_row_to_field(orc::PreviewOutputType::Frame_Field1_First,
                                    orc::PreviewFrameLayout::FieldSequential,
                                    kFrameIndex, 263, kNtscGeometry);
  ASSERT_TRUE(first.is_valid);
  EXPECT_EQ(first.field_index, kField2);
  EXPECT_EQ(first.field_line, 0);
}

// ---------------------------------------------------------------------------
// Round trips: the cross-hair row must be the row the samples came from
// ---------------------------------------------------------------------------

TEST(PreviewFrameLayoutTest, RoundTrip_PreservesEveryRow_ForSequentialFrame) {
  const int frame_lines =
      static_cast<int>(kPalGeometry.field1_lines + kPalGeometry.field2_lines);
  for (int row = 0; row < frame_lines; ++row) {
    const auto to_field = orc::map_preview_row_to_field(
        orc::PreviewOutputType::Frame_Field1_First,
        orc::PreviewFrameLayout::FieldSequential, kFrameIndex, row,
        kPalGeometry);
    ASSERT_TRUE(to_field.is_valid) << "row " << row;

    const auto back = orc::map_field_to_preview_row(
        orc::PreviewOutputType::Frame_Field1_First,
        orc::PreviewFrameLayout::FieldSequential, kFrameIndex,
        to_field.field_index, to_field.field_line, kPalGeometry);
    ASSERT_TRUE(back.is_valid) << "row " << row;
    EXPECT_EQ(back.image_y, row);
  }
}

TEST(PreviewFrameLayoutTest, RoundTrip_PreservesEveryRow_ForWeavedFrame) {
  const int frame_lines =
      static_cast<int>(kPalGeometry.field1_lines + kPalGeometry.field2_lines);
  for (int row = 0; row < frame_lines; ++row) {
    const auto to_field = orc::map_preview_row_to_field(
        orc::PreviewOutputType::Frame_Field1_First,
        orc::PreviewFrameLayout::Weaved, kFrameIndex, row, kPalGeometry);
    ASSERT_TRUE(to_field.is_valid) << "row " << row;

    const auto back = orc::map_field_to_preview_row(
        orc::PreviewOutputType::Frame_Field1_First,
        orc::PreviewFrameLayout::Weaved, kFrameIndex, to_field.field_index,
        to_field.field_line, kPalGeometry);
    ASSERT_TRUE(back.is_valid) << "row " << row;
    EXPECT_EQ(back.image_y, row);
  }
}

// ---------------------------------------------------------------------------
// Layouts must not be interchangeable: the sequential layout is what the
// line scope was previously getting wrong.
// ---------------------------------------------------------------------------

TEST(PreviewFrameLayoutTest, RowToField_DiffersFromWeaved_ForSequentialFrame) {
  const auto weaved = orc::map_preview_row_to_field(
      orc::PreviewOutputType::Frame_Field1_First,
      orc::PreviewFrameLayout::Weaved, kFrameIndex, 400, kPalGeometry);
  const auto sequential = orc::map_preview_row_to_field(
      orc::PreviewOutputType::Frame_Field1_First,
      orc::PreviewFrameLayout::FieldSequential, kFrameIndex, 400, kPalGeometry);
  ASSERT_TRUE(weaved.is_valid);
  ASSERT_TRUE(sequential.is_valid);
  EXPECT_NE(weaved.field_index, sequential.field_index);
  EXPECT_NE(weaved.field_line, sequential.field_line);
}

// ---------------------------------------------------------------------------
// Non-frame output types ignore the frame layout
// ---------------------------------------------------------------------------

TEST(PreviewFrameLayoutTest,
     RowToField_KeepsStackedBlocks_ForSplitOutputRegardlessOfLayout) {
  for (auto layout : {orc::PreviewFrameLayout::Weaved,
                      orc::PreviewFrameLayout::FieldSequential}) {
    const auto mapping = orc::map_preview_row_to_field(
        orc::PreviewOutputType::Split, layout, kFrameIndex, 400, kPalGeometry);
    ASSERT_TRUE(mapping.is_valid);
    EXPECT_EQ(mapping.field_index, kField2);
    EXPECT_EQ(mapping.field_line, 87);
  }
}

// ---------------------------------------------------------------------------
// Flat single-field views navigate by sequential field number
// ---------------------------------------------------------------------------
// The GUI converts a frame position into a field position when switching into
// these views, and get_line_samples() resolves a field's frame as index / 2,
// so output_index is the field itself - not the frame that contains it.

TEST(PreviewFrameLayoutTest,
     RowToField_MapsRowsDirectly_ForSingleFieldOutputs) {
  const auto field1 = orc::map_preview_row_to_field(
      orc::PreviewOutputType::Frame_Field1,
      orc::PreviewFrameLayout::FieldSequential, kField1, 200, kPalGeometry);
  ASSERT_TRUE(field1.is_valid);
  EXPECT_EQ(field1.field_index, kField1);
  EXPECT_EQ(field1.field_line, 200);

  const auto field2 = orc::map_preview_row_to_field(
      orc::PreviewOutputType::Frame_Field2,
      orc::PreviewFrameLayout::FieldSequential, kField2, 200, kPalGeometry);
  ASSERT_TRUE(field2.is_valid);
  EXPECT_EQ(field2.field_index, kField2);
  EXPECT_EQ(field2.field_line, 200);
}

TEST(PreviewFrameLayoutTest,
     RowToField_BoundsRowsByTheDisplayedFieldsHeight_ForSingleFieldOutputs) {
  // PAL field 2 holds 312 lines, so row 312 is past its end...
  const auto past_field2_end = orc::map_preview_row_to_field(
      orc::PreviewOutputType::Frame_Field2,
      orc::PreviewFrameLayout::FieldSequential, kField2, 312, kPalGeometry);
  EXPECT_FALSE(past_field2_end.is_valid);

  // ...while field 1 holds 313, so the same row is its last line.
  const auto last_field1_line = orc::map_preview_row_to_field(
      orc::PreviewOutputType::Frame_Field1,
      orc::PreviewFrameLayout::FieldSequential, kField1, 312, kPalGeometry);
  ASSERT_TRUE(last_field1_line.is_valid);
  EXPECT_EQ(last_field1_line.field_index, kField1);
  EXPECT_EQ(last_field1_line.field_line, 312);
}

TEST(PreviewFrameLayoutTest, FieldToRow_RoundTripsRows_ForSingleFieldOutputs) {
  for (const uint64_t field : {kField1, kField2}) {
    const auto row =
        orc::map_field_to_preview_row(orc::PreviewOutputType::Frame_Field1,
                                      orc::PreviewFrameLayout::FieldSequential,
                                      field, field, 200, kPalGeometry);
    ASSERT_TRUE(row.is_valid);
    EXPECT_EQ(row.image_y, 200);

    const auto back =
        orc::map_preview_row_to_field(orc::PreviewOutputType::Frame_Field1,
                                      orc::PreviewFrameLayout::FieldSequential,
                                      field, row.image_y, kPalGeometry);
    ASSERT_TRUE(back.is_valid);
    EXPECT_EQ(back.field_index, field);
    EXPECT_EQ(back.field_line, 200);
  }
}

TEST(PreviewFrameLayoutTest,
     FieldToRow_RejectsFieldsThatAreNotOnScreen_ForSingleFieldOutputs) {
  // Only one field is displayed, so the other field of the same frame - and
  // any field of another frame - has no row.
  const auto other_field =
      orc::map_field_to_preview_row(orc::PreviewOutputType::Frame_Field1,
                                    orc::PreviewFrameLayout::FieldSequential,
                                    kField1, kField2, 10, kPalGeometry);
  EXPECT_FALSE(other_field.is_valid);

  const auto past_end =
      orc::map_field_to_preview_row(orc::PreviewOutputType::Frame_Field2,
                                    orc::PreviewFrameLayout::FieldSequential,
                                    kField2, kField2, 312, kPalGeometry);
  EXPECT_FALSE(past_end.is_valid);
}

// ---------------------------------------------------------------------------
// Reverse mapping rejects fields that are not on screen
// ---------------------------------------------------------------------------

TEST(PreviewFrameLayoutTest,
     FieldToRow_RejectsFieldsOutsideDisplayedFrame_ForSequentialFrame) {
  const auto other_frame =
      orc::map_field_to_preview_row(orc::PreviewOutputType::Frame_Field1_First,
                                    orc::PreviewFrameLayout::FieldSequential,
                                    kFrameIndex, kField1 + 2, 10, kPalGeometry);
  EXPECT_FALSE(other_frame.is_valid);
}

}  // namespace orc_unit_test
