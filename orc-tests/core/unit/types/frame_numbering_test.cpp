/*
 * File:        frame_numbering_test.cpp
 * Module:      orc-tests/core/unit
 * Purpose:     Unit tests for frame/line range spec presentation conversion
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <frame_numbering.h>
#include <gtest/gtest.h>

namespace {

// ============================================================================
// range_spec_to_presentation (stored 0-based → displayed 1-based)
// ============================================================================

TEST(FrameNumbering, RangeSpecToPresentation_ShiftsSingleIndex) {
  EXPECT_EQ(orc::range_spec_to_presentation("0"), "1");
  EXPECT_EQ(orc::range_spec_to_presentation("42"), "43");
}

TEST(FrameNumbering, RangeSpecToPresentation_ShiftsRanges) {
  EXPECT_EQ(orc::range_spec_to_presentation("0-10"), "1-11");
  EXPECT_EQ(orc::range_spec_to_presentation("0-10,20-30,11-19"),
            "1-11,21-31,12-20");
}

TEST(FrameNumbering, RangeSpecToPresentation_PreservesPadTokens) {
  EXPECT_EQ(orc::range_spec_to_presentation("0-10,PAD_5,11-20"),
            "1-11,PAD_5,12-21");
}

TEST(FrameNumbering, RangeSpecToPresentation_EmptySpecStaysEmpty) {
  EXPECT_EQ(orc::range_spec_to_presentation(""), "");
}

TEST(FrameNumbering, RangeSpecToPresentation_NormalisesWhitespace) {
  EXPECT_EQ(orc::range_spec_to_presentation(" 0-10 , 20 "), "1-11,21");
}

TEST(FrameNumbering, RangeSpecToPresentation_MalformedSpecReturnedVerbatim) {
  EXPECT_EQ(orc::range_spec_to_presentation("F:20"), "F:20");
  EXPECT_EQ(orc::range_spec_to_presentation("abc"), "abc");
  EXPECT_EQ(orc::range_spec_to_presentation("1-"), "1-");
}

// ============================================================================
// range_spec_from_presentation (displayed 1-based → stored 0-based)
// ============================================================================

TEST(FrameNumbering, RangeSpecFromPresentation_ShiftsSingleIndex) {
  auto result = orc::range_spec_from_presentation("1");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "0");
}

TEST(FrameNumbering, RangeSpecFromPresentation_ShiftsRanges) {
  auto result = orc::range_spec_from_presentation("1-11,21-31,12-20");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "0-10,20-30,11-19");
}

TEST(FrameNumbering, RangeSpecFromPresentation_PreservesPadTokens) {
  auto result = orc::range_spec_from_presentation("1-11,PAD_5,12-21");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "0-10,PAD_5,11-20");
}

TEST(FrameNumbering, RangeSpecFromPresentation_RejectsIndexZero) {
  EXPECT_FALSE(orc::range_spec_from_presentation("0").has_value());
  EXPECT_FALSE(orc::range_spec_from_presentation("0-10").has_value());
  EXPECT_FALSE(orc::range_spec_from_presentation("1-11,0-5").has_value());
}

TEST(FrameNumbering, RangeSpecFromPresentation_RejectsMalformedTokens) {
  EXPECT_FALSE(orc::range_spec_from_presentation("abc").has_value());
  EXPECT_FALSE(orc::range_spec_from_presentation("F:20").has_value());
  EXPECT_FALSE(orc::range_spec_from_presentation("1-").has_value());
  EXPECT_FALSE(orc::range_spec_from_presentation("-5").has_value());
}

TEST(FrameNumbering, RangeSpecFromPresentation_EmptySpecStaysEmpty) {
  auto result = orc::range_spec_from_presentation("");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "");
}

TEST(FrameNumbering, RangeSpec_RoundTripIsStable) {
  const std::string stored = "0-10,PAD_3,20,30-40";
  auto round_trip = orc::range_spec_from_presentation(
      orc::range_spec_to_presentation(stored));
  ASSERT_TRUE(round_trip.has_value());
  EXPECT_EQ(*round_trip, stored);
}

// ============================================================================
// dropout_map_spec conversion (only "frame" values shift)
// ============================================================================

TEST(FrameNumbering, DropoutMapToPresentation_ShiftsOnlyFrameValues) {
  const std::string stored =
      "[{frame:0,add:[{line:10,start:100,end:200}]},"
      "{frame:41,remove:[{line:15,start:50,end:75}]}]";
  const std::string expected =
      "[{frame:1,add:[{line:10,start:100,end:200}]},"
      "{frame:42,remove:[{line:15,start:50,end:75}]}]";
  EXPECT_EQ(orc::dropout_map_spec_to_presentation(stored), expected);
}

TEST(FrameNumbering, DropoutMapToPresentation_EmptyMapUnchanged) {
  EXPECT_EQ(orc::dropout_map_spec_to_presentation("[]"), "[]");
  EXPECT_EQ(orc::dropout_map_spec_to_presentation(""), "");
}

TEST(FrameNumbering, DropoutMapFromPresentation_ShiftsOnlyFrameValues) {
  const std::string display = "[{frame:1,add:[{line:10,start:100,end:200}]}]";
  auto result = orc::dropout_map_spec_from_presentation(display);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "[{frame:0,add:[{line:10,start:100,end:200}]}]");
}

TEST(FrameNumbering, DropoutMapFromPresentation_RejectsFrameZero) {
  EXPECT_FALSE(orc::dropout_map_spec_from_presentation("[{frame:0,add:[]}]")
                   .has_value());
}

TEST(FrameNumbering, DropoutMapFromPresentation_RejectsMissingFrameValue) {
  EXPECT_FALSE(
      orc::dropout_map_spec_from_presentation("[{frame:,add:[]}]").has_value());
}

TEST(FrameNumbering, DropoutMap_RoundTripIsStable) {
  const std::string stored =
      "[{frame:0,add:[{line:10,start:100,end:200}]},{frame:99,remove:[]}]";
  auto round_trip = orc::dropout_map_spec_from_presentation(
      orc::dropout_map_spec_to_presentation(stored));
  ASSERT_TRUE(round_trip.has_value());
  EXPECT_EQ(*round_trip, stored);
}

// ============================================================================
// Decoder schedule conversion
// ============================================================================

TEST(FrameNumbering, DecoderScheduleToPresentation_ShiftsOnlyRanges) {
  EXPECT_EQ(orc::decoder_schedule_to_presentation(
                "transform3d,1000-1499:transform2d,1599:mono"),
            "transform3d,1001-1500:transform2d,1600:mono");
}

TEST(FrameNumbering, DecoderScheduleFromPresentation_NormalisesWhitespace) {
  auto result = orc::decoder_schedule_from_presentation(
      " transform3d , 1001 - 1500 : transform2d ");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "transform3d,1000-1499:transform2d");
}

TEST(FrameNumbering, DecoderScheduleFromPresentation_RejectsBadRanges) {
  for (const auto* spec :
       {"0-10:pal2d", "1-x:pal2d", ":pal2d", "PAD_5:pal2d", "-5:pal2d"}) {
    EXPECT_FALSE(orc::decoder_schedule_from_presentation(spec).has_value())
        << spec;
  }
}

TEST(FrameNumbering, DecoderSchedule_RoundTripIsStable) {
  const std::string stored = "pal2d,0:mono,10-19:transform3d";
  auto round_trip = orc::decoder_schedule_from_presentation(
      orc::decoder_schedule_to_presentation(stored));
  ASSERT_TRUE(round_trip.has_value());
  EXPECT_EQ(*round_trip, stored);
}

// ============================================================================
// indexed_spec_kind / generic wrappers
// ============================================================================

TEST(FrameNumbering, IndexedSpecKind_IdentifiesSpecParameters) {
  EXPECT_EQ(orc::indexed_spec_kind("frame_map", "ranges"),
            orc::IndexedSpecKind::kRangeSpec);
  EXPECT_EQ(orc::indexed_spec_kind("mask_line", "lineSpec"),
            orc::IndexedSpecKind::kRangeSpec);
  EXPECT_EQ(orc::indexed_spec_kind("dropout_map", "dropout_map"),
            orc::IndexedSpecKind::kDropoutMapSpec);
  EXPECT_EQ(orc::indexed_spec_kind("video_sink", "decoder_schedule"),
            orc::IndexedSpecKind::kDecoderScheduleSpec);
  EXPECT_EQ(orc::indexed_spec_kind("video_sink", "decoder_type"),
            orc::IndexedSpecKind::kNone);
  EXPECT_EQ(orc::indexed_spec_kind("frame_map", "pad_strategy"),
            orc::IndexedSpecKind::kNone);
  EXPECT_EQ(orc::indexed_spec_kind("stacker", "ranges"),
            orc::IndexedSpecKind::kNone);
}

TEST(FrameNumbering, IndexedSpecWrappers_PassThroughForKindNone) {
  EXPECT_EQ(
      orc::indexed_spec_to_presentation(orc::IndexedSpecKind::kNone, "0-10"),
      "0-10");
  auto result = orc::indexed_spec_from_presentation(orc::IndexedSpecKind::kNone,
                                                    "anything");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, "anything");
}

TEST(FrameNumbering, IndexedSpecWrappers_DispatchByKind) {
  EXPECT_EQ(orc::indexed_spec_to_presentation(orc::IndexedSpecKind::kRangeSpec,
                                              "0-10"),
            "1-11");
  EXPECT_EQ(orc::indexed_spec_to_presentation(
                orc::IndexedSpecKind::kDropoutMapSpec, "[{frame:0}]"),
            "[{frame:1}]");
}

}  // namespace
