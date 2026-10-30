/*
 * File:        source_alignment_offsets_test.cpp
 * Module:      analysis
 * Purpose:     Unit tests for pad-mode Source Alignment offsets
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/core/analysis/source_alignment/source_alignment_offsets.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace {

using orc::compute_pad_alignment;
using orc::format_alignment_map;
using orc::PictureNumberReading;

// A clean mapped source: frame index k carries picture first + k, from
// frame |from| to frame |to| inclusive.
std::vector<PictureNumberReading> pictures(int32_t first, uint64_t from,
                                           uint64_t to) {
  std::vector<PictureNumberReading> readings;
  for (uint64_t k = from; k <= to; ++k) {
    readings.push_back({k, first + static_cast<int32_t>(k)});
  }
  return readings;
}

// Sources whose frame 0 carries a number are placed by it.
TEST(SourceAlignmentOffsets, PadsEachSourceToTheEarliestPicture) {
  const auto alignment = compute_pad_alignment(
      {pictures(1000, 0, 40), pictures(1003, 0, 40), pictures(1001, 0, 40)});

  ASSERT_TRUE(alignment.first_picture.has_value());
  EXPECT_EQ(*alignment.first_picture, 1000);
  ASSERT_EQ(alignment.pads.size(), 3u);
  EXPECT_EQ(alignment.pads[0], 0u);
  EXPECT_EQ(alignment.pads[1], 3u);
  EXPECT_EQ(alignment.pads[2], 1u);
  EXPECT_EQ(format_alignment_map(alignment.pads), "1+0, 2+3, 3+1");
}

// The Disc Mapper keeps a lead-in frame at the head by default, and it
// carries no picture number. The source's frame 1 holds picture 1003, so its
// frame 0 holds the picture before it; the first number found (1003) would
// have placed it one frame late.
TEST(SourceAlignmentOffsets, SourceOpeningOnALeadInFrameIsPlacedByItsFrames) {
  const auto alignment =
      compute_pad_alignment({pictures(1000, 0, 40), pictures(1002, 1, 40)});

  EXPECT_EQ(alignment.pads[0], 0u);
  EXPECT_EQ(alignment.pads[1], 2u);
}

// One corrupt early number must not move the source: its other readings
// outvote it.
TEST(SourceAlignmentOffsets, OneCorruptEarlyNumberDoesNotMoveTheSource) {
  auto corrupt = pictures(1003, 0, 40);
  corrupt[0].picture_number = 1203;

  const auto alignment =
      compute_pad_alignment({pictures(1000, 0, 40), corrupt});

  EXPECT_EQ(*alignment.first_picture, 1000);
  EXPECT_EQ(alignment.pads[1], 3u);
}

// A source with no picture numbers cannot be placed. It is left out of the
// map, so Source Align excludes it, rather than being stacked at +0.
TEST(SourceAlignmentOffsets, SourceWithNoPictureNumbersIsExcluded) {
  const auto alignment =
      compute_pad_alignment({pictures(1000, 0, 40), {}, pictures(1004, 0, 40)});

  EXPECT_EQ(alignment.pads[0], 0u);
  EXPECT_FALSE(alignment.pads[1].has_value());
  EXPECT_EQ(alignment.pads[2], 4u);
  EXPECT_EQ(format_alignment_map(alignment.pads), "1+0, 3+4");
}

// Film with 3:2 pulldown: four pictures in every five frames, the fifth
// frame carrying none, from frame |from| of a disc whose frame 0 holds
// picture 1, to frame |to| inclusive, indexed from frame |from|.
std::vector<PictureNumberReading> film(uint64_t from, uint64_t to) {
  std::vector<PictureNumberReading> readings;
  for (uint64_t frame = from; frame <= to; ++frame) {
    if (frame % 5 == 4) continue;
    readings.push_back(
        {frame - from, static_cast<int32_t>(frame - frame / 5) + 1});
  }
  return readings;
}

// A capture of film starting ten frames into the disc starts eight pictures
// in. It is placed ten frames later, by the frames at which both read the
// same pictures, not eight by the numbers.
TEST(SourceAlignmentOffsets, FilmIsPlacedInFramesNotPictures) {
  const auto alignment = compute_pad_alignment({film(0, 399), film(10, 399)});

  ASSERT_TRUE(alignment.first_picture.has_value());
  EXPECT_EQ(*alignment.first_picture, 1);
  EXPECT_EQ(alignment.pads[0], 0u);
  EXPECT_EQ(alignment.pads[1], 10u);
}

// Captures that share no pictures can only be placed by their numbers, a
// picture per frame.
TEST(SourceAlignmentOffsets, SourcesSharingNoPicturesArePlacedByNumber) {
  const auto alignment =
      compute_pad_alignment({pictures(1000, 0, 40), pictures(1100, 0, 40)});

  EXPECT_EQ(alignment.pads[0], 0u);
  EXPECT_EQ(alignment.pads[1], 100u);
}

TEST(SourceAlignmentOffsets, NoPictureNumbersAnywhereGivesNoAlignment) {
  const auto alignment = compute_pad_alignment({{}, {}});

  EXPECT_FALSE(alignment.first_picture.has_value());
  EXPECT_FALSE(alignment.pads[0].has_value());
  EXPECT_FALSE(alignment.pads[1].has_value());
  EXPECT_EQ(format_alignment_map(alignment.pads), "");
}

}  // namespace

// A source labelled one higher moves one frame later, one labelled lower
// earlier; a source that cannot move earlier is met by padding the rest.
TEST(SourceAlignmentOffsets, ShiftsPadsByEachSourcesContentOffset) {
  const std::vector<std::optional<size_t>> pads = {0, 3, std::nullopt, 2};

  const auto later = orc::shift_pads(pads, {0, 1, 0, 0});
  EXPECT_EQ(orc::format_alignment_map(later), "1+0, 2+2, 4+2");

  const auto earlier = orc::shift_pads(pads, {-1, 0, 0, 0});
  EXPECT_EQ(orc::format_alignment_map(earlier), "1+1, 2+3, 4+2");

  const auto both = orc::shift_pads(pads, {1, 0, 5, 0});
  EXPECT_EQ(orc::format_alignment_map(both), "1+0, 2+4, 4+3");
}
