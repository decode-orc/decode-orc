/*
 * File:        capture_comparison_test.cpp
 * Module:      analysis
 * Purpose:     Unit tests for comparing aligned captures: pressing groups,
 *              noise, sample grid and high-frequency response
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/core/analysis/source_alignment/capture_comparison.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/support/frame_line_util.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace {

using orc::CaptureComparison;
using orc::FrameID;
using orc::VideoSystem;
using sample_type = orc::VideoFrameRepresentation::sample_type;

constexpr size_t kPalWidth = 1135;
constexpr size_t kPalHeight = 625;
constexpr size_t kFrames = 20;

uint64_t mix(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ULL;
  h ^= h >> 33;
  return h;
}

// Uniform in [0, 1) from a hash of the arguments.
double uniform(uint64_t seed, uint64_t frame, size_t line, int64_t n) {
  const uint64_t h =
      mix(seed * 0x9e3779b97f4a7c15ULL + frame * 0xc2b2ae3d27d4eb4fULL +
          static_cast<uint64_t>(line) * 0x165667b19e3779f9ULL +
          static_cast<uint64_t>(n) * 0x27d4eb2f165667c5ULL);
  return static_cast<double>(h >> 11) / 9007199254740992.0;
}

// Near-Gaussian noise of |sigma|: the sum of four uniforms has variance 1/3.
double noise(double sigma, uint64_t seed, uint64_t frame, size_t line,
             int64_t n) {
  double sum = -2.0;
  for (uint64_t k = 0; k < 4; ++k) sum += uniform(seed * 4 + k, frame, line, n);
  return sigma * std::sqrt(3.0) * sum;
}

// The picture every capture carries: broadband, so it has detail at every
// frequency and correlates at a single lag.
double picture(uint64_t frame, size_t line, int64_t n) {
  return 256.0 + 512.0 * uniform(7, frame, line, n);
}

// One capture: |value| gives each sample of a frame.
using Value = std::function<double(uint64_t frame, size_t line, int64_t n)>;

class FakeCapture : public orc::VideoFrameRepresentation {
 public:
  explicit FakeCapture(Value value) : value_(std::move(value)) {
    total_ =
        orc::frame_line_sample_offset(VideoSystem::PAL, kPalWidth, kPalHeight);
  }

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
    d.system = VideoSystem::PAL;
    d.height = kPalHeight;
    d.samples_total = total_;
    d.samples_per_line_nominal = kPalWidth;
    return d;
  }

  const sample_type* get_frame(FrameID id) const override {
    return has_frame(id) ? samples_for(id).data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(FrameID id) const override {
    return has_frame(id) ? samples_for(id) : std::vector<sample_type>{};
  }

 private:
  const std::vector<sample_type>& samples_for(FrameID id) const {
    auto it = frames_.find(id);
    if (it != frames_.end()) return it->second;
    std::vector<sample_type> samples(total_, 0);
    for (size_t line = 0; line < kPalHeight; ++line) {
      const size_t start =
          orc::frame_line_sample_offset(VideoSystem::PAL, kPalWidth, line);
      const size_t length =
          orc::frame_line_sample_count(VideoSystem::PAL, kPalWidth, line);
      for (size_t n = 0; n < length; ++n) {
        const double v = value_(id, line, static_cast<int64_t>(n));
        samples[start + n] =
            static_cast<sample_type>(std::clamp(std::lround(v), 0L, 1023L));
      }
    }
    return frames_.emplace(id, std::move(samples)).first->second;
  }

  Value value_;
  size_t total_ = 0;
  mutable std::map<FrameID, std::vector<sample_type>> frames_;
};

// A capture of the picture carrying noise |pressing_sigma| shared with every
// capture of the same |pressing| and noise |own_sigma| of its own.
std::shared_ptr<FakeCapture> pressed(double pressing_sigma, uint64_t pressing,
                                     double own_sigma, uint64_t own) {
  return std::make_shared<FakeCapture>(
      [=](uint64_t frame, size_t line, int64_t n) {
        return picture(frame, line, n) +
               noise(pressing_sigma, 1000 + pressing, frame, line, n) +
               noise(own_sigma, own, frame, line, n);
      });
}

std::shared_ptr<FakeCapture> independent(double sigma, uint64_t seed) {
  return pressed(0.0, 0, sigma, seed);
}

// A capture through a soft chain: the picture low-passed by a [1 2 1]/4
// filter, which passes about a third of it at 5–6 MHz.
std::shared_ptr<FakeCapture> soft(double sigma, uint64_t seed) {
  return std::make_shared<FakeCapture>([=](uint64_t frame, size_t line,
                                           int64_t n) {
    return 0.25 * picture(frame, line, n - 1) + 0.5 * picture(frame, line, n) +
           0.25 * picture(frame, line, n + 1) +
           noise(sigma, seed, frame, line, n);
  });
}

// A capture whose line starts |shift| samples earlier, so its sample n holds
// the picture's sample n - shift.
std::shared_ptr<FakeCapture> shifted(double sigma, uint64_t seed,
                                     int32_t shift) {
  return std::make_shared<FakeCapture>([=](uint64_t frame, size_t line,
                                           int64_t n) {
    return picture(frame, line, n - shift) + noise(sigma, seed, frame, line, n);
  });
}

CaptureComparison compare(
    const std::vector<std::shared_ptr<FakeCapture>>& captures) {
  std::vector<const orc::VideoFrameRepresentation*> sources;
  std::vector<std::optional<int64_t>> starts;
  for (const auto& capture : captures) {
    sources.push_back(capture.get());
    starts.emplace_back(0);
  }
  return orc::compare_captures(sources, starts);
}

}  // namespace

// Three discs of one pressing and two of another, whose stamper noise
// outweighs each disc's own, as on the study's NatB set.
TEST(CaptureComparison, ReportsTwoPressings) {
  const auto comparison =
      compare({pressed(12.0, 1, 4.0, 1), pressed(12.0, 1, 4.0, 2),
               pressed(12.0, 1, 4.0, 3), pressed(12.0, 2, 4.0, 4),
               pressed(12.0, 2, 4.0, 5)});

  ASSERT_GT(comparison.frames, 0u);
  EXPECT_EQ(comparison.pressings, 2u);
  for (const auto& noise : comparison.noise) ASSERT_TRUE(noise.has_value());
  EXPECT_EQ(comparison.noise[0]->group, comparison.noise[1]->group);
  EXPECT_EQ(comparison.noise[0]->group, comparison.noise[2]->group);
  EXPECT_EQ(comparison.noise[3]->group, comparison.noise[4]->group);
  EXPECT_NE(comparison.noise[0]->group, comparison.noise[3]->group);

  const auto report = orc::describe_capture_comparison(comparison);
  EXPECT_THAT(report.summary, testing::HasSubstr("A {1, 2, 3}, B {4, 5}"));
  EXPECT_TRUE(report.warnings.empty());
}

// Captures whose noise is all their own share no pressing noise.
TEST(CaptureComparison, IndependentCapturesFormOnePressing) {
  const auto comparison = compare({independent(3.0, 1), independent(5.0, 2),
                                   independent(7.0, 3), independent(4.0, 4)});

  EXPECT_EQ(comparison.pressings, 1u);
  ASSERT_TRUE(comparison.noise[2].has_value());
  EXPECT_NEAR(comparison.noise[2]->sigma, 7.0, 0.7);

  const auto report = orc::describe_capture_comparison(comparison);
  EXPECT_THAT(report.summary, testing::HasSubstr("from one pressing"));
}

// The study's soft captures passed 0.41-0.58 of the others' detail at
// 5-6 MHz; a low-passed capture must stand out the same way.
TEST(CaptureComparison, FlagsALowPassedCapture) {
  const auto comparison = compare({independent(4.0, 1), independent(4.0, 2),
                                   soft(4.0, 3), independent(4.0, 4)});

  for (size_t i : {0u, 1u, 3u}) {
    ASSERT_TRUE(comparison.response[i].has_value()) << i;
    EXPECT_NEAR(*comparison.response[i], 1.0, 0.1) << i;
  }
  ASSERT_TRUE(comparison.response[2].has_value());
  EXPECT_LT(*comparison.response[2], 0.5);

  const auto report = orc::describe_capture_comparison(comparison);
  ASSERT_EQ(report.warnings.size(), 1u);
  EXPECT_THAT(report.warnings[0], testing::HasSubstr("Source 3 is soft"));
}

// A capture on a different sample grid is shifted onto the first one's
// before its noise is measured; otherwise the picture would read as noise.
TEST(CaptureComparison, AlignsASampleGridBeforeMeasuringNoise) {
  const auto comparison =
      compare({independent(4.0, 1), shifted(4.0, 2, 3), independent(4.0, 3)});

  ASSERT_TRUE(comparison.grid_offsets[1].has_value());
  EXPECT_EQ(*comparison.grid_offsets[1], 3);
  ASSERT_TRUE(comparison.noise[1].has_value());
  EXPECT_NEAR(comparison.noise[1]->sigma, 4.0, 0.6);
  ASSERT_TRUE(comparison.response[1].has_value());
  EXPECT_NEAR(*comparison.response[1], 1.0, 0.1);
}

TEST(CaptureComparison, CannotTellPressingsApartWithThreeCaptures) {
  const auto comparison =
      compare({independent(4.0, 1), independent(4.0, 2), independent(4.0, 3)});

  const auto report = orc::describe_capture_comparison(comparison);
  EXPECT_THAT(report.summary, testing::HasSubstr("cannot be told apart"));
}

// Source 2 holds output frame k at its frame k + 30, beyond its last frame,
// so no stretch is common to both.
TEST(CaptureComparison, ReportsCapturesWithNothingInCommon) {
  const auto a = independent(4.0, 1);
  const auto b = independent(4.0, 2);
  const auto comparison = orc::compare_captures({a.get(), b.get()}, {0, 30});

  EXPECT_EQ(comparison.frames, 0u);
  const auto report = orc::describe_capture_comparison(comparison);
  EXPECT_THAT(report.summary, testing::HasSubstr("could not be compared"));
}
