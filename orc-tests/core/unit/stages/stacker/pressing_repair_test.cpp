/*
 * File:        pressing_repair_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the Stacker's pressing-defect repair: a streak
 *              every disc of one pressing carries, replaced from the others
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/support/frame_line_util.h>

#include <cmath>
#include <memory>
#include <optional>
#include <random>
#include <vector>

#include "../../../../orc/plugins/stages/stacker/stacker_stage.h"

namespace orc_unit_test {
namespace {

using orc::FrameID;
using orc::VideoSystem;
using sample_type = orc::VideoFrameRepresentation::sample_type;

constexpr size_t kWidth = 1135;
constexpr size_t kHeight = 625;
constexpr size_t kFrames = 20;
constexpr int32_t kBlack = 256;

// The defect: field 2, field line 208, samples 540-579, at code 4, as on
// Community North picture 3001.
constexpr size_t kDefectLine = 313 + 208;
constexpr size_t kDefectFirst = 540;
constexpr size_t kDefectEnd = 580;

double picture_value(size_t line, size_t n) {
  const auto x = static_cast<double>(n);
  const auto y = static_cast<double>(line);
  return 500.0 + 80.0 * std::sin(x / 41.0) * std::cos(y / 17.0);
}

// One capture of one pressing: a smooth picture, the pressing's noise (the
// same on every capture of it) and the capture's own, and on the pressing's
// discs, when |defect|, the streak. |dark_line| draws a dark line across the
// picture on line kDefectLine on every capture: picture, not a defect.
class FakePressingSource : public orc::VideoFrameRepresentation {
 public:
  FakePressingSource(uint32_t pressing, uint32_t capture, bool defect,
                     bool dark_line) {
    const size_t total =
        orc::frame_line_sample_offset(VideoSystem::PAL, kWidth, kHeight);
    for (size_t f = 0; f < kFrames; ++f) {
      std::mt19937 shared(pressing * 7919U + static_cast<uint32_t>(f));
      std::mt19937 own(capture * 104729U + static_cast<uint32_t>(f));
      std::normal_distribution<double> pressing_noise(0.0, 6.0);
      std::normal_distribution<double> capture_noise(0.0, 3.0);
      std::vector<sample_type> frame(total, static_cast<sample_type>(kBlack));
      for (size_t line = 0; line < kHeight; ++line) {
        const size_t start =
            orc::frame_line_sample_offset(VideoSystem::PAL, kWidth, line);
        const size_t length =
            orc::frame_line_sample_count(VideoSystem::PAL, kWidth, line);
        for (size_t n = 0; n < length; ++n) {
          double value = picture_value(line, n);
          if (dark_line && line == kDefectLine && n >= 300 && n < 900) {
            value = 300.0;
          }
          value += pressing_noise(shared) + capture_noise(own);
          if (defect && line == kDefectLine && n >= kDefectFirst &&
              n < kDefectEnd) {
            value = 4.0;
          }
          frame[start + n] = static_cast<sample_type>(std::lround(value));
        }
      }
      frames_.push_back(std::move(frame));
    }
  }

  orc::FrameIDRange frame_range() const override {
    return {FrameID{0}, FrameID{kFrames - 1}};
  }
  size_t frame_count() const override { return kFrames; }
  bool has_frame(FrameID id) const override { return id < kFrames; }
  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    orc::FrameDescriptor desc;
    desc.frame_id = id;
    desc.system = VideoSystem::PAL;
    desc.height = kHeight;
    desc.samples_total = frames_[id].size();
    desc.samples_per_line_nominal = kWidth;
    return desc;
  }
  const sample_type* get_frame(FrameID id) const override {
    return has_frame(id) ? frames_[id].data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    return has_frame(id) ? frames_[id] : std::vector<sample_type>{};
  }
  std::optional<orc::SourceParameters> get_video_parameters() const override {
    orc::SourceParameters params;
    params.system = VideoSystem::PAL;
    params.frame_width_nominal = static_cast<int32_t>(kWidth);
    params.frame_height = static_cast<int32_t>(kHeight);
    params.black_level = kBlack;
    params.number_of_sequential_frames = static_cast<int32_t>(kFrames);
    return params;
  }

 private:
  std::vector<std::vector<sample_type>> frames_;
};

using Sources =
    std::vector<std::shared_ptr<const orc::VideoFrameRepresentation>>;

// Four captures of pressing A and three of pressing B; |a_defect| puts the
// streak on all of A's.
Sources two_pressings(bool a_defect, bool dark_line = false) {
  Sources sources;
  for (uint32_t c = 0; c < 4; ++c) {
    sources.push_back(
        std::make_shared<FakePressingSource>(1, 10 + c, a_defect, dark_line));
  }
  for (uint32_t c = 0; c < 3; ++c) {
    sources.push_back(
        std::make_shared<FakePressingSource>(2, 20 + c, false, dark_line));
  }
  return sources;
}

std::vector<sample_type> stacked_frame(const Sources& sources, bool repair,
                                       FrameID id) {
  orc::StackerStage stage;
  EXPECT_TRUE(stage.set_parameters({{"mode", std::string("Weighted")},
                                    {"pressing_repair", repair},
                                    {"sample_align", false},
                                    {"luma_level_match", false},
                                    {"chroma_gain_match", false}}));
  const auto output = stage.process(sources);
  EXPECT_NE(output, nullptr);
  return output ? output->get_frame_copy(id) : std::vector<sample_type>{};
}

// The mean of the defect line's samples where the streak is.
double streak_mean(const std::vector<sample_type>& frame) {
  const size_t start =
      orc::frame_line_sample_offset(VideoSystem::PAL, kWidth, kDefectLine);
  double sum = 0.0;
  for (size_t n = kDefectFirst; n < kDefectEnd; ++n) sum += frame[start + n];
  return sum / static_cast<double>(kDefectEnd - kDefectFirst);
}

double clean_streak_mean() {
  double sum = 0.0;
  for (size_t n = kDefectFirst; n < kDefectEnd; ++n) {
    sum += picture_value(kDefectLine, n);
  }
  return sum / static_cast<double>(kDefectEnd - kDefectFirst);
}

}  // namespace

TEST(PressingRepairTest, IsOffByDefaultAndOnlyInWeightedMode) {
  orc::StackerStage stage;
  EXPECT_FALSE(std::get<bool>(stage.get_parameters().at("pressing_repair")));
  for (const auto& descriptor : stage.get_parameter_descriptors()) {
    if (descriptor.name != "pressing_repair") continue;
    ASSERT_TRUE(descriptor.constraints.default_value.has_value());
    EXPECT_FALSE(std::get<bool>(*descriptor.constraints.default_value));
    ASSERT_TRUE(descriptor.constraints.depends_on.has_value());
    EXPECT_EQ(descriptor.constraints.depends_on->parameter_name, "mode");
  }
  EXPECT_FALSE(stage.set_parameters({{"pressing_repair", int32_t{1}}}));
}

TEST(PressingRepairTest, TheStackKeepsAStreakEveryDiscOfAPressingCarries) {
  const auto frame = stacked_frame(two_pressings(true), false, 10);
  ASSERT_FALSE(frame.empty());
  EXPECT_LT(streak_mean(frame), 50.0);
}

TEST(PressingRepairTest, RepairReplacesItFromTheOtherPressing) {
  const auto frame = stacked_frame(two_pressings(true), true, 10);
  ASSERT_FALSE(frame.empty());
  EXPECT_NEAR(streak_mean(frame), clean_streak_mean(), 10.0);
}

TEST(PressingRepairTest, WithoutADefectRepairChangesNothing) {
  const auto sources = two_pressings(false);
  EXPECT_EQ(stacked_frame(sources, true, 10),
            stacked_frame(sources, false, 10));
}

TEST(PressingRepairTest, PictureEveryPressingCarriesIsLeftAlone) {
  // A dark line across the picture departs from its neighbours as far as
  // the streak does, but on every pressing.
  const auto sources = two_pressings(false, true);
  EXPECT_EQ(stacked_frame(sources, true, 10),
            stacked_frame(sources, false, 10));
}

}  // namespace orc_unit_test
