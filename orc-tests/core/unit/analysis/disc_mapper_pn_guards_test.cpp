/*
 * File:        disc_mapper_pn_guards_test.cpp
 * Module:      analysis
 * Purpose:     Unit tests for disc mapper picture-number plausibility and
 *              gap-padding guards
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <cstdint>
#include <string>

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

// A VBI line that failed to decode is published as -1 by the biphase
// observer, so the field carries the picture number on one line only.
constexpr int32_t kUnreadableLine = -1;

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

// A source whose only job is to describe a PAL frame range; the analyzer
// reads picture numbers from the observation context, never from the samples.
class TestSource {
 public:
  explicit TestSource(size_t frame_count) {
    ON_CALL(vfr_, frame_range())
        .WillByDefault(
            Return(FrameIDRange{0, static_cast<FrameID>(frame_count - 1)}));
    ON_CALL(vfr_, frame_count()).WillByDefault(Return(frame_count));
    ON_CALL(vfr_, get_video_parameters()).WillByDefault(Return(std::nullopt));
    ON_CALL(vfr_, get_frame_descriptor(_))
        .WillByDefault([](FrameID id) -> std::optional<FrameDescriptor> {
          FrameDescriptor desc;
          desc.frame_id = id;
          desc.system = VideoSystem::PAL;
          desc.height = 625;
          desc.samples_per_line_nominal = 1135;
          return desc;
        });
  }

  void set_frame_vbi(FrameID frame_id, int32_t vbi17, int32_t vbi18) {
    for (uint64_t field = 0; field < 2; ++field) {
      const FieldID fid(frame_id * 2 + field);
      obs_.set(fid, "biphase", "vbi_line_16", 0);
      obs_.set(fid, "biphase", "vbi_line_17", vbi17);
      obs_.set(fid, "biphase", "vbi_line_18", vbi18);
    }
  }

  // Both VBI lines carry the number, as they do on an undamaged frame.
  void set_picture(FrameID frame_id, int32_t picture_number) {
    const int32_t word = cav_picture_word(picture_number);
    set_frame_vbi(frame_id, word, word);
  }

  // Line 17 was lost to a dropout, so only line 18 delivers the number and
  // there is no redundancy to check it against.
  void set_picture_from_line_18_only(FrameID frame_id, int32_t picture_number) {
    set_frame_vbi(frame_id, kUnreadableLine, cav_picture_word(picture_number));
  }

  const MockVideoFrameRepresentationArtifact& vfr() const { return vfr_; }
  const ObservationContext& observations() const { return obs_; }

 private:
  NiceMock<MockVideoFrameRepresentationArtifact> vfr_;
  ObservationContext obs_;
};

// The failure this guards against, taken from a real Domesday capture: a
// frame between pictures 48911 and 48913 read 68912 from line 18 alone (a
// `4` arriving as `6` — one flipped bit — adds 20000), which extended the
// timeline by nearly 15000 placeholder frames.
TEST(DiscMapperPictureNumberGuards, DropsSingleLineNumberOutsideItsNeighbours) {
  TestSource source(5);
  source.set_picture(0, 48910);
  source.set_picture(1, 48911);
  source.set_picture_from_line_18_only(2, 68912);
  source.set_picture(3, 48913);
  source.set_picture(4, 48914);

  DiscMapperAnalyzer analyzer;
  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   DiscMapperAnalyzer::Options{});

  ASSERT_TRUE(decision.success);
  EXPECT_EQ(decision.stats.rejected_implausible_pn, 1u);
  // The corrupt number is gone, so picture 48912 is a one-frame hole that the
  // gap padding fills — rather than a 20000-frame excursion at the end of the
  // timeline. The frame that carried it has no picture number left to place
  // it by, so it drops out like any other unmappable frame.
  EXPECT_EQ(decision.mapping_spec, "0,1,PAD_1,3,4");
  EXPECT_EQ(decision.stats.padding_frames, 1u);
  EXPECT_THAT(decision.warnings,
              testing::Contains(testing::HasSubstr("single VBI line")));
}

// A single-line read that fits the sequence is exactly what the redundancy
// would have confirmed, so it must be kept.
TEST(DiscMapperPictureNumberGuards, KeepsSingleLineNumberThatFitsTheSequence) {
  TestSource source(5);
  source.set_picture(0, 48910);
  source.set_picture(1, 48911);
  source.set_picture_from_line_18_only(2, 48912);
  source.set_picture(3, 48913);
  source.set_picture(4, 48914);

  DiscMapperAnalyzer analyzer;
  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   DiscMapperAnalyzer::Options{});

  ASSERT_TRUE(decision.success);
  EXPECT_EQ(decision.stats.rejected_implausible_pn, 0u);
  EXPECT_EQ(decision.mapping_spec, "0-4");
  EXPECT_EQ(decision.stats.padding_frames, 0u);
}

// A genuine short gap in the capture must not be mistaken for corruption:
// the bracketing neighbours widen with the gap, so the number still fits.
TEST(DiscMapperPictureNumberGuards, KeepsSingleLineNumberInsideAGenuineSkip) {
  TestSource source(4);
  source.set_picture(0, 100);
  source.set_picture(1, 101);
  source.set_picture_from_line_18_only(2, 140);
  source.set_picture(3, 141);

  DiscMapperAnalyzer analyzer;
  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   DiscMapperAnalyzer::Options{});

  ASSERT_TRUE(decision.success);
  EXPECT_EQ(decision.stats.rejected_implausible_pn, 0u);
  EXPECT_EQ(decision.stats.gaps_padded, 1u);
}

// With no confirmed number after it, the trailing frame is judged against
// what the sequence predicts from the last anchor instead.
TEST(DiscMapperPictureNumberGuards, DropsUnbracketedSingleLineNumberFarOut) {
  TestSource source(4);
  source.set_picture(0, 100);
  source.set_picture(1, 101);
  source.set_picture(2, 102);
  source.set_picture_from_line_18_only(3, 20103);

  DiscMapperAnalyzer analyzer;
  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   DiscMapperAnalyzer::Options{});

  ASSERT_TRUE(decision.success);
  EXPECT_EQ(decision.stats.rejected_implausible_pn, 1u);
  EXPECT_EQ(decision.mapping_spec, "0-2");
  EXPECT_EQ(decision.stats.padding_frames, 0u);
}

// A number both VBI lines confirm is trusted whatever the sequence says: the
// guard only judges values that had no redundancy behind them.
TEST(DiscMapperPictureNumberGuards, KeepsCrossValidatedNumberOutOfSequence) {
  TestSource source(4);
  source.set_picture(0, 100);
  source.set_picture(1, 101);
  source.set_picture(2, 102);
  source.set_picture(3, 150);

  DiscMapperAnalyzer analyzer;
  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   DiscMapperAnalyzer::Options{});

  ASSERT_TRUE(decision.success);
  EXPECT_EQ(decision.stats.rejected_implausible_pn, 0u);
  EXPECT_EQ(decision.stats.gaps_padded, 1u);
}

// The backstop for a corrupt number that survives stage 1b: padding a gap
// this wide would bury the captured frames under placeholders, so the gap is
// reported and left open, and every captured frame is still mapped.
TEST(DiscMapperPictureNumberGuards, RefusesToPadAGapWiderThanTheCapture) {
  TestSource source(4);
  source.set_picture(0, 1);
  source.set_picture(1, 2);
  source.set_picture(2, 3);
  source.set_picture(3, 50000);

  DiscMapperAnalyzer analyzer;
  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   DiscMapperAnalyzer::Options{});

  ASSERT_TRUE(decision.success);
  EXPECT_EQ(decision.stats.gaps_too_wide_to_pad, 1u);
  EXPECT_EQ(decision.stats.padding_frames, 0u);
  EXPECT_EQ(decision.mapping_spec, "0-3");
  EXPECT_THAT(decision.warnings,
              testing::Contains(testing::HasSubstr("too wide to pad")));
}

// A frame whose VBI never yielded a picture number cannot be placed on a
// picture-number-indexed timeline, so it is dropped and the disc picture it
// would have filled is padded. Guessing a position for it would put the wrong
// picture at that index and misalign every source stacked against this one.
TEST(DiscMapperPictureNumberGuards, DropsFrameWithNoPictureNumberAndPadsIt) {
  TestSource source(4);
  source.set_picture(0, 10);
  source.set_picture(1, 11);
  source.set_frame_vbi(2, kUnreadableLine, kUnreadableLine);
  source.set_picture(3, 13);

  DiscMapperAnalyzer analyzer;
  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   DiscMapperAnalyzer::Options{});

  ASSERT_TRUE(decision.success);
  EXPECT_EQ(decision.stats.removed_unmappable, 1u);
  EXPECT_EQ(decision.mapping_spec, "0,1,PAD_1,3");
  EXPECT_EQ(decision.stats.padding_frames, 1u);
  // The reported output count is the mapping that was actually emitted, not
  // one recomputed from the removal tallies.
  EXPECT_EQ(decision.stats.final_frames, 4u);
}

// The reported frame count must match the mapping even when frames are
// removed for several different reasons at once.
TEST(DiscMapperPictureNumberGuards, ReportsTheFrameCountItActuallyEmitted) {
  TestSource source(6);
  source.set_picture(0, 10);
  source.set_picture(1, 11);
  source.set_frame_vbi(2, kUnreadableLine, kUnreadableLine);  // unmappable
  source.set_picture(3, 11);                                  // duplicate
  source.set_picture_from_line_18_only(4, 40011);             // implausible
  source.set_picture(5, 13);

  DiscMapperAnalyzer analyzer;
  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   DiscMapperAnalyzer::Options{});

  ASSERT_TRUE(decision.success);
  EXPECT_EQ(decision.stats.removed_duplicates, 1u);
  EXPECT_EQ(decision.stats.rejected_implausible_pn, 1u);
  // Both the frame that never had a number and the one whose number was
  // rejected are unmappable by the time stage 4 groups by picture number.
  EXPECT_EQ(decision.stats.removed_unmappable, 2u);
  EXPECT_EQ(decision.stats.final_frames, 4u);  // pictures 10, 11, PAD 12, 13
  EXPECT_EQ(decision.mapping_spec, "0,1,PAD_1,5");
}

// Short captures must still pad normally: the limit has an absolute floor so
// that a preview range of a few frames is not treated as a corrupt timeline.
TEST(DiscMapperPictureNumberGuards, PadsSmallGapsInAShortCapture) {
  TestSource source(3);
  source.set_picture(0, 1);
  source.set_picture(1, 2);
  source.set_picture(2, 60);

  DiscMapperAnalyzer analyzer;
  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   DiscMapperAnalyzer::Options{});

  ASSERT_TRUE(decision.success);
  EXPECT_EQ(decision.stats.gaps_too_wide_to_pad, 0u);
  EXPECT_EQ(decision.stats.padding_frames, 57u);
  EXPECT_EQ(decision.mapping_spec, "0,1,PAD_57,2");
}

}  // namespace
