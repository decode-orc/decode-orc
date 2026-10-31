/*
 * File:        disc_mapper_pulldown_test.cpp
 * Module:      analysis
 * Purpose:     Unit tests for the disc mapper on film with 3:2 pulldown: the
 *              frames that carry no picture number by design are kept in
 *              place, and padded where the capture lost one
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <cstdint>
#include <optional>
#include <vector>

#include "../../../../orc/core/analysis/disc_mapper/disc_mapper_analyzer.h"
#include "../include/video_frame_representation_artifact_mock.h"

namespace {

using orc::DiscMapperAnalyzer;
using orc::FieldID;
using orc::FrameDescriptor;
using orc::FrameID;
using orc::FrameIDRange;
using orc::ObservationContext;
using orc::VideoSystem;
using orc_unit_test::MockVideoFrameRepresentationArtifact;
using testing::_;
using testing::NiceMock;
using testing::Return;

// The biphase observer publishes a line with no code on it as 0 and one that
// failed to decode as -1.
constexpr int32_t kBlankLine = 0;
constexpr int32_t kUnreadableLine = -1;

// Line 16's programme status code, as Dragon's Lair carries it on every
// field, pulldown frames included.
constexpr int32_t kStatusCode = 0x8BA01E;

// IEC 60857-1986 - 10.1.3 Picture numbers (CAV discs).
int32_t cav_picture_word(int32_t picture_number) {
  int32_t bcd = 0;
  int32_t shift = 0;
  for (int32_t remaining = picture_number; remaining > 0; remaining /= 10) {
    bcd |= (remaining % 10) << shift;
    shift += 4;
  }
  return 0xF00000 | bcd;
}

// The picture each field of disc frame |frame| starts, on a CAV film disc
// that opens on picture 1 at phase A of the cadence: the fields run
// A A B B B C C D D D, and each film frame's number goes on the field that
// starts it, so the frames of a cycle carry pictures p and p + 1 on field 1,
// p + 2 and p + 3 on field 2, and the fifth none (as videosynth and the
// Dragon's Lair captures place them).
struct FieldPictures {
  std::optional<int32_t> first;
  std::optional<int32_t> second;
};

FieldPictures film_frame(int64_t frame) {
  const auto p = static_cast<int32_t>(4 * (frame / 5)) + 1;
  switch (frame % 5) {
    case 0:
      return {p, std::nullopt};
    case 1:
      return {p + 1, std::nullopt};
    case 2:
      return {std::nullopt, p + 2};
    case 3:
      return {std::nullopt, p + 3};
    default:
      return {std::nullopt, std::nullopt};
  }
}

// A capture: the disc frames it holds, in order. nullopt is a frame whose
// VBI was lost to a dropout.
class FilmCapture {
 public:
  FilmCapture(const std::vector<std::optional<int64_t>>& disc_frames,
              VideoSystem system = VideoSystem::NTSC)
      : frame_count_(disc_frames.size()) {
    ON_CALL(vfr_, frame_range())
        .WillByDefault(
            Return(FrameIDRange{0, static_cast<FrameID>(frame_count_ - 1)}));
    ON_CALL(vfr_, frame_count()).WillByDefault(Return(frame_count_));
    ON_CALL(vfr_, get_video_parameters()).WillByDefault(Return(std::nullopt));
    ON_CALL(vfr_, get_frame_descriptor(_))
        .WillByDefault([system](FrameID id) -> std::optional<FrameDescriptor> {
          FrameDescriptor desc;
          desc.frame_id = id;
          desc.system = system;
          desc.height = system == VideoSystem::PAL ? 625 : 525;
          desc.samples_per_line_nominal =
              system == VideoSystem::PAL ? 1135 : 910;
          return desc;
        });
    for (size_t i = 0; i < disc_frames.size(); ++i) {
      const FrameID id = i;
      if (!disc_frames[i]) {
        set_field(id, 0, kUnreadableLine, kUnreadableLine, kUnreadableLine);
        set_field(id, 1, kUnreadableLine, kUnreadableLine, kUnreadableLine);
        continue;
      }
      const FieldPictures pictures = film_frame(*disc_frames[i]);
      set_picture_field(id, 0, pictures.first);
      set_picture_field(id, 1, pictures.second);
    }
  }

  orc::FieldMappingDecision map() const {
    return DiscMapperAnalyzer().analyze(vfr_, obs_,
                                        DiscMapperAnalyzer::Options{});
  }

  // Line 17 of field 1 of capture frame |id| carries |word|, while line 18
  // keeps the number the disc frame gave it.
  void set_line_17_on_first_field(FrameID id, int32_t word) {
    obs_.set(FieldID(id * 2), "biphase", "vbi_line_17", word);
  }

 private:
  void set_picture_field(FrameID id, uint64_t field,
                         std::optional<int32_t> picture) {
    const int32_t word = picture ? cav_picture_word(*picture) : kBlankLine;
    set_field(id, field, kStatusCode, word, word);
  }

  void set_field(FrameID id, uint64_t field, int32_t vbi16, int32_t vbi17,
                 int32_t vbi18) {
    const FieldID fid(id * 2 + field);
    obs_.set(fid, "biphase", "vbi_line_16", vbi16);
    obs_.set(fid, "biphase", "vbi_line_17", vbi17);
    obs_.set(fid, "biphase", "vbi_line_18", vbi18);
  }

  size_t frame_count_;
  NiceMock<MockVideoFrameRepresentationArtifact> vfr_;
  ObservationContext obs_;
};

std::vector<std::optional<int64_t>> disc_frames(int64_t first, int64_t last) {
  std::vector<std::optional<int64_t>> frames;
  for (int64_t f = first; f <= last; ++f) frames.emplace_back(f);
  return frames;
}

std::vector<std::optional<int64_t>> concat(
    std::vector<std::optional<int64_t>> a,
    const std::vector<std::optional<int64_t>>& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

}  // namespace

// The fifth frame of every cycle carries no picture number because the disc
// never gave it one. It stays where it is: dropping it would take a frame in
// five out of the film, and the sound with it.
TEST(DiscMapperPulldown, FilmKeepsItsPulldownFrames) {
  // Disc frames 0-20: pictures 1-17, pulldown frames 4, 9, 14 and 19.
  const auto decision = FilmCapture(disc_frames(0, 20)).map();

  ASSERT_TRUE(decision.success) << decision.rationale;
  EXPECT_EQ(decision.mapping_spec, "0-20");
  EXPECT_EQ(decision.stats.pulldown_frames, 4u);
  EXPECT_EQ(decision.stats.pulldown_frames_padded, 0u);
  EXPECT_EQ(decision.stats.removed_unmappable, 0u);
  EXPECT_EQ(decision.stats.padding_frames, 0u);
  ASSERT_TRUE(decision.stats.first_picture.has_value());
  EXPECT_EQ(*decision.stats.first_picture, 1);
  EXPECT_EQ(*decision.stats.last_picture, 17);
}

// A frame whose number exists but could not be read sits between pictures n
// and n + 2, not n and n + 1, so it is not taken for a pulldown frame: it is
// dropped and its picture padded, as on any disc.
TEST(DiscMapperPulldown, UnreadNumberIsStillPadded) {
  auto frames = disc_frames(0, 20);
  frames[6].reset();  // picture 6

  const auto decision = FilmCapture(frames).map();

  ASSERT_TRUE(decision.success) << decision.rationale;
  EXPECT_EQ(decision.mapping_spec, "0-5,PAD_1,7-20");
  EXPECT_EQ(decision.stats.pulldown_frames, 4u);
  EXPECT_EQ(decision.stats.removed_unmappable, 1u);
  EXPECT_EQ(decision.stats.padding_frames, 1u);
}

// Picture 8's number is lost, so neither it nor the pulldown frame after it
// can be placed. Both places are padded: the pulldown frames on either side
// say the cadence runs on across them, and without the second pad the
// capture would sit a frame early against every other capture from here on.
TEST(DiscMapperPulldown, LostPulldownFrameIsPadded) {
  auto frames = disc_frames(0, 20);
  frames[8].reset();  // picture 8, before pulldown frame 9

  const auto decision = FilmCapture(frames).map();

  ASSERT_TRUE(decision.success) << decision.rationale;
  EXPECT_EQ(decision.mapping_spec, "0-7,PAD_2,10-20");
  EXPECT_EQ(decision.stats.pulldown_frames, 3u);
  EXPECT_EQ(decision.stats.pulldown_frames_padded, 1u);
  EXPECT_EQ(decision.stats.padding_frames, 2u);
}

// Taken from Dragon's Lair side 1, where every capture reads picture 18989's
// line 17 as 58989 (one flipped bit, so on the disc) and line 18 correctly.
// Pictures 8 and 10 on either side leave room for picture 9 alone, so the
// copy that names it is kept. Padding it instead would lose the picture from
// every capture, and the pulldown frame before it with it, since that frame
// is placed by the numbers on either side.
TEST(DiscMapperPulldown, DisagreeingCopiesWithOnlyOnePlaceAreResolved) {
  FilmCapture capture(disc_frames(0, 20));
  capture.set_line_17_on_first_field(10, cav_picture_word(40009));

  const auto decision = capture.map();

  ASSERT_TRUE(decision.success) << decision.rationale;
  EXPECT_EQ(decision.mapping_spec, "0-20");
  EXPECT_EQ(decision.stats.corrected_vbi_errors, 1u);
  EXPECT_EQ(decision.stats.rejected_disagreeing_pn, 0u);
  EXPECT_EQ(decision.stats.pulldown_frames, 4u);
  EXPECT_EQ(decision.stats.removed_unmappable, 0u);
  EXPECT_EQ(decision.stats.padding_frames, 0u);
}

// A capture that skipped pictures 9-12 also skipped the pulldown frame after
// picture 12; the padding holds both, so the capture keeps the disc's frames.
// The pulldown frame before the skip has the skip beside it, not picture 9,
// so nothing places it: it is dropped and its place padded too.
TEST(DiscMapperPulldown, GapIsPaddedWithItsPulldownFrames) {
  const auto decision =
      FilmCapture(concat(disc_frames(0, 9), disc_frames(15, 25))).map();

  ASSERT_TRUE(decision.success) << decision.rationale;
  EXPECT_EQ(decision.mapping_spec, "0-8,PAD_6,10-20");
  EXPECT_EQ(decision.stats.padding_frames, 6u);
  EXPECT_EQ(decision.stats.pulldown_frames_padded, 2u);
  EXPECT_EQ(decision.stats.final_frames, 26u);
}

// Taken from Dragon's Lair DS5: a player catching up skips again and again,
// landing for a frame or two between jumps, so no kept pulldown frame sits
// near most of the gaps. The ones before and after the whole stretch fall on
// the same cycle and nothing captured between contradicts it, so every gap
// is padded with its pulldown frames and the capture ends with as many frames
// as the disc has.
TEST(DiscMapperPulldown, SkipsInARowArePaddedWithTheirPulldownFrames) {
  auto frames = disc_frames(0, 20);
  frames = concat(frames, disc_frames(60, 61));
  frames = concat(frames, disc_frames(122, 123));
  frames = concat(frames, disc_frames(180, 200));

  const auto decision = FilmCapture(frames).map();

  ASSERT_TRUE(decision.success) << decision.rationale;
  EXPECT_EQ(decision.stats.gaps_too_wide_to_pad, 0u);
  EXPECT_EQ(decision.stats.final_frames, 201u);
  EXPECT_EQ(decision.stats.padding_frames, 201u - frames.size());
}

// A replayed stretch replays its pulldown frames too; one copy of each is
// kept, so the replay leaves the disc's frames once each. The copy the player
// jumped back from has the jump beside it rather than picture 13, so it is
// dropped, and the copy from the second pass is the one kept.
TEST(DiscMapperPulldown, ReplayedFilmKeepsOneCopyOfEachFrame) {
  const auto decision =
      FilmCapture(concat(disc_frames(0, 14), disc_frames(5, 20))).map();

  ASSERT_TRUE(decision.success) << decision.rationale;
  EXPECT_EQ(decision.mapping_spec, "0-13,24-30");
  EXPECT_EQ(decision.stats.pulldown_frames, 4u);
  EXPECT_EQ(decision.stats.removed_duplicates, 9u);
  EXPECT_EQ(decision.stats.removed_unmappable, 1u);
  EXPECT_EQ(decision.stats.padding_frames, 0u);
  EXPECT_EQ(decision.stats.final_frames, 21u);
}

// PAL has no pulldown. A numberless frame between consecutive pictures is
// unmappable there, as before.
TEST(DiscMapperPulldown, PalHasNoPulldownFrames) {
  const auto decision = FilmCapture(disc_frames(0, 20), VideoSystem::PAL).map();

  ASSERT_TRUE(decision.success) << decision.rationale;
  EXPECT_EQ(decision.stats.pulldown_frames, 0u);
  EXPECT_EQ(decision.stats.removed_unmappable, 4u);
}
