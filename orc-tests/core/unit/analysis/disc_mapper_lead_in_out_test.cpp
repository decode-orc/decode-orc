/*
 * File:        disc_mapper_lead_in_out_test.cpp
 * Module:      analysis
 * Purpose:     Unit tests for disc mapper lead-in/lead-out retention
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

// IEC 60857-1986 - 10.1.1 Lead-in / 10.1.2 Lead-out codes, carried on VBI
// lines 17 and 18.
constexpr int32_t kLeadInCode = 0x88FFFF;
constexpr int32_t kLeadOutCode = 0x80EEEE;

// IEC 60857-1986 - 10.1.9 Users code: 0x8X_D_XXX with X1 in 0..7. This one
// decodes to user code "1234".
constexpr int32_t kUserCode = 0x81D234;

// IEC 60857-1986 - 10.1.3 Picture numbers (CAV discs): 0xF00000 marker plus
// the picture number in BCD.
int32_t cav_picture_word(int32_t picture_number) {
  int32_t bcd = 0;
  int32_t shift = 0;
  for (int32_t remaining = picture_number; remaining > 0; remaining /= 10) {
    bcd |= (remaining % 10) << shift;
    shift += 4;
  }
  return 0xF00000 | bcd;
}

// A source whose only job is to describe a PAL frame range; the analyzer reads
// picture numbers from the observation context, never from the samples.
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

  // Publishes the same VBI words on both fields of a frame, as the biphase
  // observer would for a frame whose lines decoded cleanly.
  void set_frame_vbi(FrameID frame_id, int32_t vbi16, int32_t vbi17,
                     int32_t vbi18) {
    for (uint64_t field = 0; field < 2; ++field) {
      const FieldID fid(frame_id * 2 + field);
      obs_.set(fid, "biphase", "vbi_line_16", vbi16);
      obs_.set(fid, "biphase", "vbi_line_17", vbi17);
      obs_.set(fid, "biphase", "vbi_line_18", vbi18);
    }
  }

  void set_lead_in(FrameID frame_id, bool with_user_code) {
    set_frame_vbi(frame_id, with_user_code ? kUserCode : 0, kLeadInCode,
                  kLeadInCode);
  }

  void set_lead_out(FrameID frame_id) {
    set_frame_vbi(frame_id, 0, kLeadOutCode, kLeadOutCode);
  }

  void set_picture(FrameID frame_id, int32_t picture_number) {
    const int32_t word = cav_picture_word(picture_number);
    set_frame_vbi(frame_id, 0, word, word);
  }

  const MockVideoFrameRepresentationArtifact& vfr() const { return vfr_; }
  const ObservationContext& observations() const { return obs_; }

 private:
  NiceMock<MockVideoFrameRepresentationArtifact> vfr_;
  ObservationContext obs_;
};

// Source frames 0 and 1 are lead-in (only frame 1 carries the user's code),
// frames 2-4 are pictures 1-3, and frame 5 is lead-out. The source is filled
// in place because the mock it holds is not copyable.
constexpr size_t kDiscWithLeadFramesCount = 6;

void fill_disc_with_lead_frames(TestSource& source) {
  source.set_lead_in(0, /*with_user_code=*/false);
  source.set_lead_in(1, /*with_user_code=*/true);
  source.set_picture(2, 1);
  source.set_picture(3, 2);
  source.set_picture(4, 3);
  source.set_lead_out(5);
}

DiscMapperAnalyzer::Options options_with_lead_frames(bool include) {
  DiscMapperAnalyzer::Options options;
  options.include_lead_in_out = include;
  return options;
}

TEST(DiscMapperLeadInOut, DropsAllLeadFrames_WhenOptionDisabled) {
  TestSource source(kDiscWithLeadFramesCount);
  fill_disc_with_lead_frames(source);
  DiscMapperAnalyzer analyzer;

  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   options_with_lead_frames(false));

  ASSERT_TRUE(decision.success);
  EXPECT_EQ(decision.mapping_spec, "2-4");
  EXPECT_EQ(decision.stats.removed_lead_in_out, 3u);
  EXPECT_FALSE(decision.stats.lead_in_included);
  EXPECT_FALSE(decision.stats.lead_out_included);
}

TEST(DiscMapperLeadInOut, KeepsOneLeadInAndOneLeadOut_WhenOptionEnabled) {
  TestSource source(kDiscWithLeadFramesCount);
  fill_disc_with_lead_frames(source);
  DiscMapperAnalyzer analyzer;

  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   options_with_lead_frames(true));

  ASSERT_TRUE(decision.success);
  // Lead-in frame 1 heads the mapping and lead-out frame 5 tails it, so the
  // whole run collapses to a single range.
  EXPECT_EQ(decision.mapping_spec, "1-5");
  // Only the lead-in frame without the user's code is discarded.
  EXPECT_EQ(decision.stats.removed_lead_in_out, 1u);
  EXPECT_TRUE(decision.stats.lead_in_included);
  EXPECT_TRUE(decision.stats.lead_out_included);
}

TEST(DiscMapperLeadInOut, PrefersLeadInFrameCarryingUserCode) {
  // The user's code is on the first of three lead-in frames, so the "closest
  // to the programme" preference must yield to it.
  TestSource source(5);
  source.set_lead_in(0, /*with_user_code=*/true);
  source.set_lead_in(1, /*with_user_code=*/false);
  source.set_lead_in(2, /*with_user_code=*/false);
  source.set_picture(3, 1);
  source.set_picture(4, 2);

  DiscMapperAnalyzer analyzer;
  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   options_with_lead_frames(true));

  ASSERT_TRUE(decision.success);
  EXPECT_EQ(decision.mapping_spec, "0,3,4");
  EXPECT_EQ(decision.stats.removed_lead_in_out, 2u);
  EXPECT_TRUE(decision.stats.lead_in_included);
  EXPECT_FALSE(decision.stats.lead_out_included);
}

TEST(DiscMapperLeadInOut, KeepsLastLeadInFrame_WhenNoUserCodeIsPresent) {
  TestSource source(5);
  source.set_lead_in(0, /*with_user_code=*/false);
  source.set_lead_in(1, /*with_user_code=*/false);
  source.set_picture(2, 1);
  source.set_picture(3, 2);
  source.set_picture(4, 3);

  DiscMapperAnalyzer analyzer;
  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   options_with_lead_frames(true));

  ASSERT_TRUE(decision.success);
  EXPECT_EQ(decision.mapping_spec, "1-4");
  EXPECT_EQ(decision.stats.removed_lead_in_out, 1u);
}

TEST(DiscMapperLeadInOut, ReportsNothingIncluded_WhenCaptureHasNoLeadFrames) {
  TestSource source(3);
  source.set_picture(0, 1);
  source.set_picture(1, 2);
  source.set_picture(2, 3);

  DiscMapperAnalyzer analyzer;
  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   options_with_lead_frames(true));

  ASSERT_TRUE(decision.success);
  EXPECT_EQ(decision.mapping_spec, "0-2");
  EXPECT_EQ(decision.stats.removed_lead_in_out, 0u);
  EXPECT_FALSE(decision.stats.lead_in_included);
  EXPECT_FALSE(decision.stats.lead_out_included);
}

// A CAV field whose picture number decodes as zero is a lead-in field that
// lost its lead-in code to a dropout: it must not be mapped as picture zero.
TEST(DiscMapperLeadInOut, TreatsZeroPictureNumberAsLeadIn) {
  TestSource source(4);
  source.set_frame_vbi(0, kUserCode, cav_picture_word(0), cav_picture_word(0));
  source.set_picture(1, 1);
  source.set_picture(2, 2);
  source.set_picture(3, 3);

  DiscMapperAnalyzer analyzer;
  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   options_with_lead_frames(true));

  ASSERT_TRUE(decision.success);
  EXPECT_EQ(decision.mapping_spec, "0-3");
  EXPECT_TRUE(decision.stats.lead_in_included);
}

// Gap padding runs over the programme picture numbers only; a retained
// lead-in frame carries no picture number and must not extend the gap search.
TEST(DiscMapperLeadInOut, LeadFramesDoNotDisturbGapPadding) {
  TestSource source(5);
  source.set_lead_in(0, /*with_user_code=*/true);
  source.set_picture(1, 1);
  source.set_picture(2, 3);  // picture 2 is missing
  source.set_picture(3, 4);
  source.set_lead_out(4);

  DiscMapperAnalyzer analyzer;
  auto decision = analyzer.analyze(source.vfr(), source.observations(),
                                   options_with_lead_frames(true));

  ASSERT_TRUE(decision.success);
  EXPECT_EQ(decision.mapping_spec, "0,1,PAD_1,2-4");
  EXPECT_EQ(decision.stats.padding_frames, 1u);
  EXPECT_EQ(decision.stats.gaps_padded, 1u);
}

}  // namespace
