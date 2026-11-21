/*
 * File:        subtitle_embed_policy_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the video sink subtitle source and codec
 *              selection
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/plugins/stages/sinks/common/subtitle_embed_policy.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace orc_unit_test {

namespace {

using orc::select_subtitle_embed_source;
using orc::SubtitleCodec;
using orc::SubtitleEmbedRequest;
using orc::SubtitleEmbedSource;

// A viable closed caption request: MP4 container, observation context present.
SubtitleEmbedRequest cc_request() {
  SubtitleEmbedRequest request;
  request.embed_closed_captions = true;
  request.container_format = "mp4";
  request.has_observation_context = true;
  return request;
}

}  // namespace

// Regression for issue #236: closed caption embedding used to be gated on a
// VideoFrameRepresentation being handed to the backend, which only happened
// when audio embedding was enabled. The selection depends on no such input.
TEST(SubtitleEmbedPolicyTest,
     ClosedCaptions_AreSelected_WithoutAudioEmbedding) {
  const auto decision = select_subtitle_embed_source(cc_request());
  EXPECT_EQ(decision.source, SubtitleEmbedSource::kClosedCaptions);
  EXPECT_TRUE(decision.closed_caption_reason.empty());
}

TEST(SubtitleEmbedPolicyTest, NothingRequested_SelectsNoSource) {
  SubtitleEmbedRequest request;
  request.container_format = "mp4";
  request.has_observation_context = true;

  const auto decision = select_subtitle_embed_source(request);
  EXPECT_EQ(decision.source, SubtitleEmbedSource::kNone);
  EXPECT_TRUE(decision.closed_caption_reason.empty());
}

TEST(SubtitleEmbedPolicyTest, ClosedCaptions_AreMovText_InMp4Container) {
  const auto decision = select_subtitle_embed_source(cc_request());
  EXPECT_EQ(decision.codec, SubtitleCodec::kMovText);
}

TEST(SubtitleEmbedPolicyTest, ClosedCaptions_AreSelected_InMovContainer) {
  auto request = cc_request();
  request.container_format = "mov";

  const auto decision = select_subtitle_embed_source(request);
  EXPECT_EQ(decision.source, SubtitleEmbedSource::kClosedCaptions);
  EXPECT_EQ(decision.codec, SubtitleCodec::kMovText);
}

// Issue #373: Matroska cannot carry mov_text, so the lossy LaserDisc
// emulation format (mkv-h264) carries its captions as SubRip.
TEST(SubtitleEmbedPolicyTest, ClosedCaptions_AreSubRip_InMkvContainer) {
  auto request = cc_request();
  request.container_format = "mkv";

  const auto decision = select_subtitle_embed_source(request);
  EXPECT_EQ(decision.source, SubtitleEmbedSource::kClosedCaptions);
  EXPECT_EQ(decision.codec, SubtitleCodec::kSubRip);
  EXPECT_TRUE(decision.closed_caption_reason.empty());
}

TEST(SubtitleEmbedPolicyTest,
     ClosedCaptions_AreDropped_InContainerWithoutTextSubtitles) {
  for (const char* container : {"mxf", "avi"}) {
    auto request = cc_request();
    request.container_format = container;

    const auto decision = select_subtitle_embed_source(request);
    EXPECT_EQ(decision.source, SubtitleEmbedSource::kNone) << container;
    EXPECT_EQ(decision.codec, SubtitleCodec::kNone) << container;
    EXPECT_THAT(decision.closed_caption_reason,
                testing::HasSubstr("MP4/MOV/MKV"))
        << container;
  }
}

TEST(SubtitleEmbedPolicyTest, ClosedCaptions_AreDropped_WithoutObservations) {
  auto request = cc_request();
  request.has_observation_context = false;

  const auto decision = select_subtitle_embed_source(request);
  EXPECT_EQ(decision.source, SubtitleEmbedSource::kNone);
  EXPECT_THAT(decision.closed_caption_reason,
              testing::HasSubstr("observation context"));
}

// The selection has never depended on the video system; EIA-608 captions are
// carried on 525-line sources and the policy must not read anything into that.
TEST(SubtitleEmbedPolicyTest, ClosedCaptions_AreSelected_ForEverySystem) {
  EXPECT_EQ(select_subtitle_embed_source(cc_request()).source,
            SubtitleEmbedSource::kClosedCaptions);
}

}  // namespace orc_unit_test
