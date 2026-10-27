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
#include <string>
#include <variant>
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

// As pressed(), with the pressing's stamper made from mother |family|, whose
// noise |family_sigma| every capture of its pressings shares.
std::shared_ptr<FakeCapture> mothered(double family_sigma, uint64_t family,
                                      double pressing_sigma, uint64_t pressing,
                                      double own_sigma, uint64_t own) {
  return std::make_shared<FakeCapture>(
      [=](uint64_t frame, size_t line, int64_t n) {
        return picture(frame, line, n) +
               noise(family_sigma, 2000 + family, frame, line, n) +
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

// A capture of a faint picture (σ about 15 codes, as fine detail is at the
// top of the band) under stamper noise of the same size, so pairs within a
// pressing cross-correlate on their shared noise as much as on the picture.
std::shared_ptr<FakeCapture> faint(double pressing_sigma, uint64_t pressing,
                                   double own_sigma, uint64_t own) {
  return std::make_shared<FakeCapture>(
      [=](uint64_t frame, size_t line, int64_t n) {
        return 461.0 + 0.1 * picture(frame, line, n) +
               noise(pressing_sigma, 1000 + pressing, frame, line, n) +
               noise(own_sigma, own, frame, line, n);
      });
}

// A capture of a picture of broad shapes, with a little fine detail to set
// the grid by, at |gain| about mid-grey and |level| codes up.
std::shared_ptr<FakeCapture> levelled(double gain, double level, double sigma,
                                      uint64_t seed) {
  return std::make_shared<FakeCapture>(
      [=](uint64_t frame, size_t line, int64_t n) {
        const double shapes = 200.0 * std::sin(0.02 * static_cast<double>(n)) *
                              std::cos(0.05 * static_cast<double>(line));
        const double detail = 0.1 * (picture(frame, line, n) - 512.0);
        return 512.0 + gain * (shapes + detail) + level +
               noise(sigma, seed, frame, line, n);
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

// Two pressings of one mother, a third pressing and a lone disc, as on
// Community South: the pressings are found by the pressing test, the family
// by fit, and the report names it.
TEST(CaptureComparison, ReportsAFamilyOfPressings) {
  const auto comparison = compare(
      {mothered(12.0, 1, 5.0, 1, 4.0, 1), mothered(12.0, 1, 5.0, 1, 4.0, 2),
       mothered(12.0, 1, 5.0, 1, 4.0, 3), mothered(12.0, 1, 5.0, 2, 4.0, 4),
       mothered(12.0, 1, 5.0, 2, 4.0, 5), pressed(12.0, 3, 4.0, 6),
       pressed(12.0, 3, 4.0, 7), pressed(12.0, 4, 4.0, 8)});

  ASSERT_GT(comparison.frames, 0u);
  EXPECT_EQ(comparison.pressings, 4u);
  EXPECT_EQ(comparison.families, 3u);
  EXPECT_TRUE(comparison.fit.trusted) << comparison.fit.worst_misfit;
  for (const auto& noise : comparison.noise) ASSERT_TRUE(noise.has_value());
  EXPECT_NE(comparison.noise[0]->group, comparison.noise[3]->group);
  EXPECT_EQ(comparison.noise[0]->family, comparison.noise[3]->family);
  EXPECT_NE(comparison.noise[0]->family, comparison.noise[5]->family);
  EXPECT_NE(comparison.noise[5]->family, comparison.noise[7]->family);

  const auto report = orc::describe_capture_comparison(comparison);
  EXPECT_THAT(report.summary,
              testing::HasSubstr("A {1, 2, 3}, B {4, 5}, C {6, 7}, D {8}"));
  EXPECT_THAT(report.summary,
              testing::HasSubstr("Families: pressings A and B"));
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

// Four discs of one pressing, one of its own and two of a third, as on
// Community South.  Within the large pressing the stamper's noise
// cross-correlates as strongly as the picture, so fitting every pair read
// the other three's response at 0.66-0.77 and called one of them soft, though
// every capture passes the same detail.  Only pairs across pressings carry
// no shared noise, and they alone are fitted.
TEST(CaptureComparison, SharedStamperNoiseDoesNotReadAsResponse) {
  const auto comparison = compare(
      {faint(15.0, 1, 4.0, 1), faint(15.0, 1, 4.0, 2), faint(15.0, 1, 4.0, 3),
       faint(15.0, 1, 4.0, 4), faint(0.0, 0, 8.0, 5), faint(15.0, 3, 4.0, 6),
       faint(15.0, 3, 4.0, 7)});

  ASSERT_TRUE(comparison.fit.trusted) << comparison.fit.worst_misfit;
  EXPECT_EQ(comparison.pressings, 3u);
  for (size_t i = 0; i < 7; ++i) {
    ASSERT_TRUE(comparison.response[i].has_value()) << i;
    EXPECT_NEAR(*comparison.response[i], 1.0, 0.15) << i;
  }
  EXPECT_TRUE(orc::describe_capture_comparison(comparison).warnings.empty());
}

// A capture 5 % up in gain and 8 codes up in level is reported, and its
// noise is measured as its own.
TEST(CaptureComparison, ReportsALevelMismatch) {
  const auto comparison =
      compare({levelled(1.0, 0.0, 4.0, 1), levelled(1.0, 0.0, 4.0, 2),
               levelled(1.0, 0.0, 4.0, 3), levelled(1.05, 8.0, 4.0, 4)});

  ASSERT_TRUE(comparison.levels[3].has_value());
  EXPECT_NEAR(comparison.levels[3]->gain, 1.05, 0.01);
  EXPECT_NEAR(comparison.levels[3]->level, 8.0, 1.0);
  ASSERT_TRUE(comparison.noise[3].has_value());
  EXPECT_NEAR(comparison.noise[3]->sigma, 4.0, 0.6);

  const auto report = orc::describe_capture_comparison(comparison);
  EXPECT_THAT(report.summary, testing::HasSubstr("source 4 gain +5.0 %"));
  EXPECT_THAT(report.summary,
              testing::Not(testing::HasSubstr("source 1 gain")));
}

// Every input gets its line and data, the summary's figures for a script;
// one that took no part says so and carries nothing else.  The lone disc of
// a pressing adds the most to the stack.
TEST(CaptureComparison, ReportsEachSourceAsData) {
  const auto a = pressed(12.0, 1, 4.0, 1);
  const auto b = pressed(12.0, 1, 4.0, 2);
  const auto c = pressed(12.0, 1, 4.0, 3);
  const auto d = pressed(12.0, 2, 4.0, 4);
  const auto e = pressed(12.0, 2, 4.0, 5);
  const auto lone = independent(12.5, 6);
  const auto absent = independent(4.0, 7);
  const auto comparison = orc::compare_captures(
      {a.get(), b.get(), c.get(), d.get(), e.get(), lone.get(), absent.get()},
      {0, 0, 0, 0, 0, 0, std::nullopt});
  ASSERT_TRUE(comparison.fit.trusted) << comparison.fit.worst_misfit;
  const auto report = orc::describe_capture_comparison(comparison);

  ASSERT_EQ(report.sources.size(), 7u);
  for (size_t i = 0; i < 6; ++i) {
    const auto& data = report.sources[i].data;
    EXPECT_EQ(std::get<int>(data.at("source")), static_cast<int>(i + 1));
    EXPECT_TRUE(std::get<bool>(data.at("compared"))) << i;
    for (const char* key :
         {"noise_sigma", "own_sigma", "pressing_sigma", "family_sigma",
          "stamper_group", "family", "gain_percent", "level_codes",
          "response_5_6mhz", "soft", "sample_grid", "weight_share",
          "stack_sigma_without", "first_frame", "last_frame",
          "padding_frames"}) {
      EXPECT_TRUE(data.count(key)) << i << " " << key;
    }
    EXPECT_EQ(std::get<int>(data.at("first_frame")), 1);
    EXPECT_EQ(std::get<int>(data.at("last_frame")), static_cast<int>(kFrames));
    EXPECT_NEAR(std::get<double>(data.at("noise_sigma")),
                comparison.noise[i]->sigma, 1e-12);
    // The line is the summary's, without its indent.
    EXPECT_THAT(report.summary,
                testing::HasSubstr("  " + report.sources[i].line + "\n"));
  }
  EXPECT_EQ(std::get<std::string>(report.sources[0].data.at("stamper_group")),
            "A");
  EXPECT_EQ(std::get<std::string>(report.sources[3].data.at("stamper_group")),
            "B");

  const auto& missing = report.sources[6];
  EXPECT_EQ(missing.line, "Source 7: not compared");
  EXPECT_FALSE(std::get<bool>(missing.data.at("compared")));
  EXPECT_EQ(missing.data.size(), 2u);

  auto without = [&](size_t i) {
    return std::get<double>(report.sources[i].data.at("stack_sigma_without"));
  };
  const double stack = std::get<double>(report.statistics.at("stackSigma"));
  EXPECT_NEAR(stack, comparison.fit.weighted_sigma, 1e-9);
  EXPECT_LT(stack, std::get<double>(report.statistics.at("bestSingleSigma")));
  for (size_t i = 0; i < 5; ++i) {
    EXPECT_GT(without(5), without(i)) << i;
    EXPECT_GT(without(i), stack) << i;
  }
  double shares = 0.0;
  for (size_t i = 0; i < 6; ++i) {
    shares += std::get<double>(report.sources[i].data.at("weight_share"));
  }
  EXPECT_NEAR(shares, 100.0, 1e-9);
  EXPECT_TRUE(std::get<bool>(report.statistics.at("fitTrusted")));
  EXPECT_THAT(report.summary, testing::HasSubstr("What each source adds"));
  EXPECT_THAT(report.summary, testing::HasSubstr("most first:\n  without "
                                                 "source 6: "));
  // Every compared source covers every frame, unpadded, on one grid, so the
  // one that adds the most is suggested, and the next choice is named.
  EXPECT_EQ(std::get<int>(report.statistics.at("suggestedReference")), 6);
  EXPECT_THAT(report.summary,
              testing::HasSubstr("Suggested reference: source 6 (covers "));
  EXPECT_THAT(report.summary, testing::HasSubstr(". Next choice: source "));
  const int runner_up =
      std::get<int>(report.statistics.at("referenceRunnerUp"));
  for (size_t i = 0; i < 5; ++i) {
    EXPECT_GE(without(static_cast<size_t>(runner_up - 1)), without(i)) << i;
  }
}

namespace {

// A comparison of sources covering |coverage| (first and last frame), with
// |padding| padding frames each and sample grids |grids|.
orc::CaptureComparison covering(
    std::vector<std::optional<std::pair<int64_t, int64_t>>> coverage,
    std::vector<size_t> padding, std::vector<std::optional<int32_t>> grids) {
  orc::CaptureComparison comparison;
  comparison.coverage = std::move(coverage);
  comparison.padding = std::move(padding);
  comparison.grid_offsets = std::move(grids);
  return comparison;
}

}  // namespace

// The suggested reference covers the most output frames, however much
// padding it carries.
TEST(CaptureComparison, SuggestsTheSourceCoveringTheMostFrames) {
  const auto comparison =
      covering({{{0, 899}}, {{0, 999}}, {{20, 999}}}, {0, 40, 0}, {0, 3, 0});
  EXPECT_EQ(orc::suggest_reference(comparison), 1u);
}

// Of the sources covering the most frames, the one with the fewest padding
// frames.
TEST(CaptureComparison, SuggestsTheLeastPaddedOfTheLongest) {
  const auto comparison =
      covering({{{0, 999}}, {{0, 999}}, {{0, 999}}}, {12, 3, 7}, {0, 0, 0});
  EXPECT_EQ(orc::suggest_reference(comparison), 1u);
}

// Padding within kReferencePaddingTolerance frames of the fewest counts as
// equal, and the source that adds the most to the stack is taken first: a
// frame of padding does not make a capture the better reference.
TEST(CaptureComparison, SuggestsTheSourceAddingTheMostOnANearTie) {
  auto comparison =
      compare({independent(3.0, 1), independent(5.0, 2), independent(7.0, 3)});
  ASSERT_TRUE(comparison.noise[0] && comparison.noise[1] &&
              comparison.noise[2]);
  comparison.coverage = {{{0, 999}}, {{0, 999}}, {{0, 999}}};
  comparison.grid_offsets = {0, 0, 0};
  comparison.padding = {15, 14, 40};
  EXPECT_EQ(orc::rank_references(comparison), (std::vector<size_t>{0, 1, 2}));

  // Beyond the tolerance padding still decides.
  comparison.padding = {14 + orc::kReferencePaddingTolerance + 1, 14, 14};
  EXPECT_EQ(orc::rank_references(comparison), (std::vector<size_t>{1, 2, 0}));
}

// Then the one whose sample grid is nearest the median of the sources'; one
// whose grid was not measured comes last.
TEST(CaptureComparison, SuggestsTheGridNearestTheMedianOnATie) {
  const auto comparison =
      covering({{{0, 999}}, {{0, 999}}, {{0, 999}}, {{0, 999}}, {{0, 999}}},
               {0, 0, 0, 0, 0}, {4, std::nullopt, -1, 1, 2});
  // Median of 4, -1, 1 and 2 is 1.5: sources 4 and 5 are 0.5 away, and the
  // earlier is taken.
  EXPECT_EQ(orc::suggest_reference(comparison), 3u);
}

// With no source covering a frame there is nothing to suggest.
TEST(CaptureComparison, SuggestsNothingWithoutCoverage) {
  EXPECT_FALSE(orc::suggest_reference(
                   covering({std::nullopt, std::nullopt}, {0, 0}, {0, 0}))
                   .has_value());
}

// With fewer than 3 sources no response is measured, and the data says why.
TEST(CaptureComparison, SaysWhyAResponseIsMissing) {
  const auto comparison = compare({independent(4.0, 1), independent(4.0, 2)});
  const auto report = orc::describe_capture_comparison(comparison);
  ASSERT_EQ(report.sources.size(), 2u);
  EXPECT_FALSE(report.sources[0].data.count("response_5_6mhz"));
  EXPECT_EQ(std::get<std::string>(report.sources[0].data.at("response_note")),
            "fewer than 3 sources");
  EXPECT_FALSE(std::get<bool>(report.sources[0].data.at("soft")));
}
