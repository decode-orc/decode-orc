/*
 * File:        content_offsets_test.cpp
 * Module:      analysis
 * Purpose:     Unit tests for checking aligned captures' picture numbers
 *              against their content
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/core/analysis/source_alignment/content_offsets.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/support/frame_line_util.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

using orc::FrameID;
using orc::VideoSystem;
using sample_type = orc::VideoFrameRepresentation::sample_type;

// A small NTSC frame: blanking and the active picture the check reads, at a
// fraction of a real line's cost.
constexpr size_t kWidth = 64;
constexpr size_t kHeight = 525;
constexpr size_t kFrames = 1200;

uint64_t mix(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

double uniform(uint64_t seed, uint64_t a, uint64_t b, uint64_t c) {
  const uint64_t h =
      mix(seed * 0x9e3779b97f4a7c15ULL + a * 0xc2b2ae3d27d4eb4fULL +
          b * 0x165667b19e3779f9ULL + c * 0x27d4eb2f165667c5ULL);
  return static_cast<double>(h >> 11) / 9007199254740992.0;
}

// Which picture each frame holds.
using PictureOf = std::function<int64_t(int64_t frame)>;

// A capture whose frame j holds picture |picture_of(j)|: a pattern of blocks
// of its own for each picture, under noise of the capture's own.
class FakeCapture : public orc::VideoFrameRepresentation {
 public:
  FakeCapture(PictureOf picture_of, uint64_t seed)
      : picture_of_(std::move(picture_of)), seed_(seed) {}

  orc::FrameIDRange frame_range() const override {
    return {FrameID{0}, FrameID{kFrames - 1}};
  }
  size_t frame_count() const override { return kFrames; }
  bool has_frame(FrameID id) const override { return id < kFrames; }

  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    orc::FrameDescriptor d;
    d.frame_id = id;
    d.system = VideoSystem::NTSC;
    d.height = kHeight;
    d.samples_total = kWidth * kHeight;
    d.samples_per_line_nominal = kWidth;
    return d;
  }

  const sample_type* get_frame(FrameID) const override { return nullptr; }
  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    if (!has_frame(id)) return {};
    const auto picture =
        static_cast<uint64_t>(picture_of_(static_cast<int64_t>(id)));
    std::vector<sample_type> samples(kWidth * kHeight);
    for (size_t line = 0; line < kHeight; ++line) {
      for (size_t x = 0; x < kWidth; ++x) {
        const double content = 400.0 * uniform(1, picture, line / 4, x / 6);
        const double noise = 8.0 * (uniform(seed_, id, line, x) - 0.5);
        samples[line * kWidth + x] =
            static_cast<sample_type>(std::lround(300.0 + content + noise));
      }
    }
    return samples;
  }

 private:
  PictureOf picture_of_;
  uint64_t seed_;
};

std::shared_ptr<FakeCapture> capture(PictureOf picture_of, uint64_t seed) {
  return std::make_shared<FakeCapture>(std::move(picture_of), seed);
}

int64_t same(int64_t frame) { return frame; }

orc::ContentOffsets check(
    const std::vector<std::shared_ptr<FakeCapture>>& captures) {
  std::vector<const orc::VideoFrameRepresentation*> sources;
  std::vector<std::optional<int64_t>> starts;
  for (const auto& c : captures) {
    sources.push_back(c.get());
    starts.emplace_back(0);
  }
  return orc::check_content_offsets(sources, starts);
}

}  // namespace

TEST(ContentOffsets, ReportsNothingWhenEveryCaptureAgrees) {
  const auto offsets = check(
      {capture(same, 1), capture(same, 2), capture(same, 3), capture(same, 4)});

  EXPECT_EQ(offsets.windows, orc::kContentWindows);
  EXPECT_EQ(offsets.windows_with_motion, orc::kContentWindows);
  for (const auto& spans : offsets.spans) EXPECT_TRUE(spans.empty());
  const auto report = orc::describe_content_offsets(offsets);
  EXPECT_TRUE(report.warnings.empty());
  EXPECT_THAT(report.summary, testing::HasSubstr("matches its picture"));
  EXPECT_EQ(std::get<int>(report.sources[1].at("content_offset")), 0);
}

// As Countryside side 1 DS3: every picture labelled one lower than the other
// captures label it, so its frame k holds their picture k + 1.
TEST(ContentOffsets, FindsACaptureLabelledOneLowerThroughout) {
  const auto offsets =
      check({capture(same, 1), capture(same, 2),
             capture([](int64_t j) { return j + 1; }, 3), capture(same, 4)});

  ASSERT_EQ(offsets.spans[2].size(), 1u);
  EXPECT_EQ(offsets.spans[2][0].first, 0);
  EXPECT_EQ(offsets.spans[2][0].last, static_cast<int64_t>(kFrames - 1));
  ASSERT_TRUE(offsets.spans[2][0].offset.has_value());
  EXPECT_EQ(*offsets.spans[2][0].offset, -1);
  EXPECT_EQ(orc::whole_content_offset(offsets, 2), -1);
  for (size_t i : {0u, 1u, 3u}) EXPECT_TRUE(offsets.spans[i].empty()) << i;

  const auto report = orc::describe_content_offsets(offsets);
  ASSERT_EQ(report.warnings.size(), 1u);
  EXPECT_THAT(report.warnings[0],
              testing::HasSubstr("labels its pictures 1 lower"));
  EXPECT_THAT(report.warnings[0],
              testing::HasSubstr("Moving its alignment by -1 frame"));
  EXPECT_EQ(std::get<int>(report.sources[2].at("content_offset")), -1);
}

// Labelled correctly up to output frame 500 and one lower from there on, so
// picture 500 is missing from it: the change is found to the frame either
// side of the picture it lacks.
TEST(ContentOffsets, FindsWhereAnOffsetStarts) {
  const auto offsets =
      check({capture(same, 1), capture(same, 2),
             capture([](int64_t j) { return j < 500 ? j : j + 1; }, 3),
             capture(same, 4)});

  ASSERT_EQ(offsets.spans[2].size(), 1u);
  const auto& span = offsets.spans[2][0];
  EXPECT_EQ(span.earliest_first, 500);
  EXPECT_EQ(span.first, 501);
  EXPECT_EQ(span.last, static_cast<int64_t>(kFrames - 1));
  ASSERT_TRUE(span.offset.has_value());
  EXPECT_EQ(*span.offset, -1);
  EXPECT_FALSE(orc::whole_content_offset(offsets, 2).has_value());

  const auto report = orc::describe_content_offsets(offsets);
  ASSERT_EQ(report.warnings.size(), 1u);
  EXPECT_THAT(report.warnings[0],
              testing::HasSubstr("output frames 502–1200 (from somewhere "
                                 "after frame 500)"));
  EXPECT_THAT(report.warnings[0],
              testing::HasSubstr("need their picture numbers corrected"));
}

// The first source, which the others are compared with, is the odd one out:
// the consensus says so, not the comparison.
TEST(ContentOffsets, BlamesAReferenceTheOthersOutvote) {
  const auto offsets =
      check({capture([](int64_t j) { return j + 1; }, 1), capture(same, 2),
             capture(same, 3), capture(same, 4)});

  ASSERT_TRUE(offsets.reference.has_value());
  EXPECT_EQ(*offsets.reference, 0u);
  ASSERT_EQ(offsets.spans[0].size(), 1u);
  ASSERT_TRUE(offsets.spans[0][0].offset.has_value());
  EXPECT_EQ(*offsets.spans[0][0].offset, -1);
  for (size_t i : {1u, 2u, 3u}) EXPECT_TRUE(offsets.spans[i].empty()) << i;
}

// On a still every offset matches, so nothing can be said.
TEST(ContentOffsets, CannotCheckAStill) {
  const auto still = [](int64_t) { return int64_t{7}; };
  const auto offsets = check({capture(still, 1), capture(still, 2),
                              capture([](int64_t) { return int64_t{7}; }, 3)});

  EXPECT_EQ(offsets.windows_with_motion, 0u);
  for (const auto& spans : offsets.spans) EXPECT_TRUE(spans.empty());
  EXPECT_THAT(orc::describe_content_offsets(offsets).summary,
              testing::HasSubstr("not checked"));
}

// A capture of other pictures altogether matches at no offset.
TEST(ContentOffsets, ReportsACaptureOfOtherPictures) {
  const auto offsets =
      check({capture(same, 1), capture(same, 2),
             capture([](int64_t j) { return j + 100000; }, 3)});

  ASSERT_EQ(offsets.spans[2].size(), 1u);
  EXPECT_FALSE(offsets.spans[2][0].offset.has_value());
  const auto report = orc::describe_content_offsets(offsets);
  ASSERT_EQ(report.warnings.size(), 1u);
  EXPECT_THAT(report.warnings[0], testing::HasSubstr("hold none of"));
}
