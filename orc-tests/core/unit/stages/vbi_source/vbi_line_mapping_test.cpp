/*
 * File:        vbi_line_mapping_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for stored record to CVBS frame line placement
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi_line_mapping.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace orc {
namespace {

VBITeletextLineMap pal_wst_line_map() {
  VBITeletextLineMap line_map;
  std::string error;
  EXPECT_TRUE(make_vbi_teletext_line_map(
      VBITVSystem::kPAL, VBITeletextSystem::kWST, line_map, error))
      << error;
  return line_map;
}

VBISourceFormat bt8x8_pal_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset("bt8x8 card dump, 8-bit (WST)", format, error))
      << error;
  return format;
}

// WST occupies broadcast frame lines 7-22 and 320-335, which are stored frame
// lines 6-21 and 319-334.
TEST(VBILineMapping, PALWSTLineTableMatchesTheStandardForBothFields) {
  const VBITeletextLineMap line_map = pal_wst_line_map();

  ASSERT_EQ(line_map.field1.size(), 16u);
  ASSERT_EQ(line_map.field2.size(), 16u);

  EXPECT_EQ(line_map.field1,
            (std::vector<uint32_t>{6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17,
                                   18, 19, 20, 21}));
  EXPECT_EQ(line_map.field2,
            (std::vector<uint32_t>{319, 320, 321, 322, 323, 324, 325, 326, 327,
                                   328, 329, 330, 331, 332, 333, 334}));

  EXPECT_EQ(&line_map.for_tv_field(1), &line_map.field1);
  EXPECT_EQ(&line_map.for_tv_field(2), &line_map.field2);
  EXPECT_TRUE(line_map.for_tv_field(3).empty());
}

// The two fields' ranges are not related by a constant offset, which is why
// the mapping is a table rather than an offset and a stride.
TEST(VBILineMapping, FieldRangesAreNotSeparatedByAConstantOffset) {
  const VBITeletextLineMap line_map = pal_wst_line_map();

  EXPECT_EQ(line_map.field2.front() - line_map.field1.front(), 313u);
  EXPECT_NE(line_map.field2.front() - line_map.field1.front(),
            line_map.field1.size() * 2u);
}

TEST(VBILineMapping, EveryBt8x8RecordResolvesToItsStandardFrameLine) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextLineMap line_map = pal_wst_line_map();

  for (uint32_t record = 0; record < 16; ++record) {
    uint32_t frame_line = 0;
    std::string error;

    ASSERT_TRUE(map_vbi_record_to_frame_line(format, line_map, 0, record,
                                             frame_line, error))
        << error;
    EXPECT_EQ(frame_line, 6u + record);

    ASSERT_TRUE(map_vbi_record_to_frame_line(format, line_map, 1, record,
                                             frame_line, error))
        << error;
    EXPECT_EQ(frame_line, 319u + record);
  }
}

// Which television field a stored field carries is a driver convention, so it
// is configuration; getting it backwards swaps the line numbering between the
// two fields.
TEST(VBILineMapping, FirstFieldConfigurationSwapsTheTelevisionFieldOrder) {
  VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextLineMap line_map = pal_wst_line_map();

  EXPECT_EQ(vbi_tv_field_for_stored_field(format, 0), 1u);
  EXPECT_EQ(vbi_tv_field_for_stored_field(format, 1), 2u);

  format.first_field = 2;
  EXPECT_EQ(vbi_tv_field_for_stored_field(format, 0), 2u);
  EXPECT_EQ(vbi_tv_field_for_stored_field(format, 1), 1u);

  uint32_t frame_line = 0;
  std::string error;
  ASSERT_TRUE(
      map_vbi_record_to_frame_line(format, line_map, 0, 0, frame_line, error))
      << error;
  EXPECT_EQ(frame_line, 319u);
}

// A source carrying fewer lines than the standard maps contiguously from the
// head of the field's line list.
TEST(VBILineMapping, ShorterSourcesMapContiguouslyFromTheFieldRangeStart) {
  VBISourceFormat format = bt8x8_pal_format();
  format.field_range = VBIFieldRange{2, 9};
  const VBITeletextLineMap line_map = pal_wst_line_map();

  uint32_t frame_line = 0;
  std::string error;

  ASSERT_TRUE(
      map_vbi_record_to_frame_line(format, line_map, 0, 2, frame_line, error))
      << error;
  EXPECT_EQ(frame_line, 6u);

  ASSERT_TRUE(
      map_vbi_record_to_frame_line(format, line_map, 0, 9, frame_line, error))
      << error;
  EXPECT_EQ(frame_line, 13u);

  ASSERT_TRUE(
      map_vbi_record_to_frame_line(format, line_map, 1, 2, frame_line, error))
      << error;
  EXPECT_EQ(frame_line, 319u);
}

TEST(VBILineMapping, RecordsOutsideTheFieldRangeCarryNoDataService) {
  VBISourceFormat format = bt8x8_pal_format();
  format.field_range = VBIFieldRange{1, 8};
  const VBITeletextLineMap line_map = pal_wst_line_map();

  uint32_t frame_line = 0;
  std::string error;

  EXPECT_FALSE(
      map_vbi_record_to_frame_line(format, line_map, 0, 0, frame_line, error));
  EXPECT_NE(error.find("field_range"), std::string::npos);

  error.clear();
  EXPECT_FALSE(
      map_vbi_record_to_frame_line(format, line_map, 0, 9, frame_line, error));
  EXPECT_NE(error.find("field_range"), std::string::npos);
}

// Excess records have nowhere to go; that is a configuration error and never
// a silent truncation.
TEST(VBILineMapping, RecordsBeyondTheStandardsLineListAreRejected) {
  VBISourceFormat format = bt8x8_pal_format();
  format.field_lines = 18;
  format.field_range = VBIFieldRange{0, 17};
  const VBITeletextLineMap line_map = pal_wst_line_map();

  uint32_t frame_line = 0;
  std::string error;

  ASSERT_TRUE(
      map_vbi_record_to_frame_line(format, line_map, 0, 15, frame_line, error))
      << error;
  EXPECT_EQ(frame_line, 21u);

  EXPECT_FALSE(
      map_vbi_record_to_frame_line(format, line_map, 0, 16, frame_line, error));
  EXPECT_NE(error.find("16"), std::string::npos);
  EXPECT_NE(error.find("WST"), std::string::npos);
}

TEST(VBILineMapping, OnlyTwoStoredFieldsExistPerFrame) {
  const VBISourceFormat format = bt8x8_pal_format();
  const VBITeletextLineMap line_map = pal_wst_line_map();

  uint32_t frame_line = 0;
  std::string error;
  EXPECT_FALSE(
      map_vbi_record_to_frame_line(format, line_map, 2, 0, frame_line, error));
  EXPECT_FALSE(error.empty());
}

// NABTS is a 525-line service, so a 625-line project has no line list to place
// it on at all.
TEST(VBILineMapping, UndefinedSystemPairingsAreRefused) {
  VBITeletextLineMap line_map;
  std::string error;

  EXPECT_FALSE(make_vbi_teletext_line_map(
      VBITVSystem::kPAL, VBITeletextSystem::kNABTS, line_map, error));
  EXPECT_NE(error.find("PAL"), std::string::npos);
  EXPECT_TRUE(line_map.field1.empty());
}

// ---------------------------------------------------------------------------
// 525-line systems
// ---------------------------------------------------------------------------

VBITeletextLineMap ntsc_wst_line_map() {
  VBITeletextLineMap line_map;
  std::string error;
  EXPECT_TRUE(make_vbi_teletext_line_map(
      VBITVSystem::kNTSC, VBITeletextSystem::kWST, line_map, error))
      << error;
  return line_map;
}

VBISourceFormat tbc_vbi_ntsc_format() {
  VBISourceFormat format;
  std::string error;
  EXPECT_TRUE(
      expand_vbi_source_preset(".tbc VBI crop, 16-bit (WST)", format, error))
      << error;
  return format;
}

// Both 525-line teletext services occupy broadcast frame lines 10-21 and
// 273-284, which are stored frame lines 9-20 and 272-283.  Field 2 begins at
// broadcast frame line 264, so its line 10 is frame line 273.  A last entry,
// line 22 of each field, is for a source's caption records only.
TEST(VBILineMapping, NTSCLineTableMatchesTheStandardForBothFields) {
  const VBITeletextLineMap line_map = ntsc_wst_line_map();

  ASSERT_EQ(line_map.field1.size(), 13u);
  ASSERT_EQ(line_map.field2.size(), 13u);

  EXPECT_EQ(line_map.field1, (std::vector<uint32_t>{9, 10, 11, 12, 13, 14, 15,
                                                    16, 17, 18, 19, 20, 21}));
  EXPECT_EQ(line_map.field2,
            (std::vector<uint32_t>{272, 273, 274, 275, 276, 277, 278, 279, 280,
                                   281, 282, 283, 284}));

  // The two fields are 263 lines apart on a 525-line frame, which a constant
  // offset from the PAL table would not give.
  for (size_t index = 0; index < line_map.field1.size(); ++index) {
    EXPECT_EQ(line_map.field2[index] - line_map.field1[index], 263u);
  }
}

// The circulating VBI-only .tbc crops start at broadcast field line 9, so the
// twelve teletext lines are records 1 to 12 and the mapping starts one record
// in rather than at record 0.
TEST(VBILineMapping, TBCVBINTSCRecordsMapOntoTheStandardLines) {
  const VBISourceFormat format = tbc_vbi_ntsc_format();
  const VBITeletextLineMap line_map = ntsc_wst_line_map();

  ASSERT_EQ(format.field_range.start, 1u);
  ASSERT_EQ(format.field_range.end, 12u);
  // Record 13, line 22, for the caption observer only.
  ASSERT_EQ(format.caption_records, 1u);
  {
    std::string caption_error;
    uint32_t caption_line = 0;
    ASSERT_TRUE(map_vbi_record_to_frame_line(format, ntsc_wst_line_map(), 0,
                                             13u, caption_line, caption_error))
        << caption_error;
    EXPECT_EQ(caption_line, 21u);
    ASSERT_TRUE(map_vbi_record_to_frame_line(format, ntsc_wst_line_map(), 1,
                                             13u, caption_line, caption_error))
        << caption_error;
    EXPECT_EQ(caption_line, 284u);
    EXPECT_FALSE(map_vbi_record_to_frame_line(
        format, ntsc_wst_line_map(), 0, 14u, caption_line, caption_error));
  }

  std::string error;
  uint32_t frame_line = 0;
  for (uint32_t record = 1; record <= 12u; ++record) {
    ASSERT_TRUE(map_vbi_record_to_frame_line(format, line_map, 0, record,
                                             frame_line, error))
        << error;
    EXPECT_EQ(frame_line, 9u + (record - 1u)) << "record " << record;

    ASSERT_TRUE(map_vbi_record_to_frame_line(format, line_map, 1, record,
                                             frame_line, error))
        << error;
    EXPECT_EQ(frame_line, 272u + (record - 1u)) << "record " << record;
  }

  // Record 0 is the last post-equalising line and records 14-15 are active
  // picture; neither carries a data service, so neither maps.  Record 13,
  // line 22, maps for the caption observer alone (caption_records).
  EXPECT_FALSE(
      map_vbi_record_to_frame_line(format, line_map, 0, 0, frame_line, error));
  EXPECT_FALSE(
      map_vbi_record_to_frame_line(format, line_map, 0, 14, frame_line, error));
}

// Line 22 is in the 525-line list for caption records only: a data record has
// the standard's twelve lines and no more, so a field range one record too
// long is refused rather than given line 22.
TEST(VBILineMapping, ADataRecordIsNeverPlacedOnTheCaptionLine) {
  VBISourceFormat format = tbc_vbi_ntsc_format();
  format.field_range.end = 13u;
  format.caption_records = 0;
  std::string error;
  uint32_t frame_line = 0;
  EXPECT_FALSE(map_vbi_record_to_frame_line(format, ntsc_wst_line_map(), 0, 13u,
                                            frame_line, error));
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace orc
