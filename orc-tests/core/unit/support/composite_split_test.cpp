/*
 * File:        composite_split_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the composite luma/chroma split: the round
 *              trip, a known colour recovered, and chroma moved to another
 *              line's subcarrier phase on PAL, NTSC and PAL-M
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/composite_split.h>

#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

namespace orc_unit_test {
namespace {

using orc::VideoSystem;
namespace split = orc::composite_split;

constexpr double kTwoPi = 2.0 * split::kPi;

struct System {
  VideoSystem system;
  int32_t line_length;
  // Subcarrier cycles a line advances, less whole cycles.
  double phase_step_cycles;
};

// PAL: 283.7516 cycles a line (1135.0064 samples at 4FSC); NTSC 227.5;
// PAL-M 227.25.
constexpr System kPal{VideoSystem::PAL, 1135, 0.7516};
constexpr System kNtsc{VideoSystem::NTSC, 910, 0.5};
constexpr System kPalM{VideoSystem::PAL_M, 909, 0.25};

constexpr int32_t kActiveStart = 200;
constexpr int32_t kActiveEnd = 860;

// One encoded line: luma |y| plus U and V on the subcarrier at the line's
// phase, V inverted on odd lines of PAL and PAL-M, and a burst (-U, plus V on
// PAL's) over the system's burst samples.
std::vector<int16_t> encode_line(const System& s, int32_t line, double y,
                                 double u, double v) {
  const double phase = kTwoPi * s.phase_step_cycles * line;
  const bool pal_colour = s.system != VideoSystem::NTSC;
  const double v_switch = (pal_colour && (line % 2) != 0) ? -1.0 : 1.0;
  const auto [burst_start, burst_end] = orc::colour_burst_range(s.system);
  std::vector<int16_t> out(static_cast<size_t>(s.line_length));
  for (int32_t n = 0; n < s.line_length; ++n) {
    const double theta = phase + (n * split::kPi / 2.0);
    double sample = 256.0;
    if (n >= burst_start && n < burst_end) {
      const double burst_v = pal_colour ? 60.0 : 0.0;
      sample +=
          (-60.0 * std::sin(theta)) + (v_switch * burst_v * std::cos(theta));
    } else if (n >= kActiveStart - 40 && n < kActiveEnd + 40) {
      sample = y + (u * std::sin(theta)) + (v_switch * v * std::cos(theta));
    }
    out[static_cast<size_t>(n)] = static_cast<int16_t>(std::lround(sample));
  }
  return out;
}

split::SplitSpan split_active(const System& s,
                              const std::vector<int16_t>& line) {
  const split::ChromaSplitter splitter(s.system);
  return splitter.split(line.data(), s.line_length, kActiveStart, kActiveEnd);
}

std::complex<double> burst_of(const System& s,
                              const std::vector<int16_t>& line) {
  const auto [start, end] = orc::colour_burst_range(s.system);
  const auto burst = split::burst_reference(line.data(), start, end, 10.0);
  EXPECT_TRUE(burst.has_value());
  return burst.value_or(std::complex<double>{1.0, 0.0});
}

class CompositeSplitTest : public ::testing::TestWithParam<System> {};

// A split and its re-modulation give the line back exactly.
TEST_P(CompositeSplitTest, RoundTripIsExact) {
  const System s = GetParam();
  auto line = encode_line(s, 7, 500.0, 80.0, -50.0);
  // Picture detail in every band: a ramp and a fine pattern.
  for (int32_t n = kActiveStart; n < kActiveEnd; ++n) {
    line[static_cast<size_t>(n)] +=
        static_cast<int16_t>(((n % 13) * 7) + ((n * 3) % 11) + (n / 10));
  }
  const auto span = split_active(s, line);
  ASSERT_EQ(span.luma.size(), static_cast<size_t>(kActiveEnd - kActiveStart));
  for (int32_t n = kActiveStart; n < kActiveEnd; ++n) {
    const auto i = static_cast<size_t>(n - kActiveStart);
    EXPECT_NEAR(span.luma[i] + split::remodulate(span.chroma[i], n),
                line[static_cast<size_t>(n)], 1e-9)
        << "sample " << n;
  }
}

// Flat colour splits into its luma and its chroma, away from the filter's
// reach of the colour's ends.
TEST_P(CompositeSplitTest, RecoversFlatColour) {
  const System s = GetParam();
  constexpr double kY = 420.0;
  constexpr double kU = 70.0;
  constexpr double kV = -45.0;
  const auto line = encode_line(s, 10, kY, kU, kV);
  const auto span = split_active(s, line);
  const double phase = kTwoPi * s.phase_step_cycles * 10;
  for (int32_t n = kActiveStart; n < kActiveEnd; ++n) {
    const auto i = static_cast<size_t>(n - kActiveStart);
    const double theta = phase + (n * split::kPi / 2.0);
    const double chroma = (kU * std::sin(theta)) + (kV * std::cos(theta));
    // Within rounding of the encoded line, and the filter's DC gain.
    EXPECT_NEAR(span.luma[i], kY, 1.0) << "sample " << n;
    EXPECT_NEAR(split::remodulate(span.chroma[i], n), chroma, 1.0)
        << "sample " << n;
  }
}

// The same colour on a line 1-4 lines away in the field: the first line's
// chroma, made relative to its burst, moved across V-switch where the
// system has one and re-modulated at the other line's burst, is the other
// line's chroma.
TEST_P(CompositeSplitTest, MovesChromaToAnotherLine) {
  const System s = GetParam();
  constexpr int32_t kFrom = 20;
  constexpr double kU = -55.0;
  constexpr double kV = 75.0;
  const auto from = encode_line(s, kFrom, 300.0, kU, kV);
  const auto from_span = split_active(s, from);
  const auto from_burst = burst_of(s, from);
  for (int32_t distance = -4; distance <= 4; ++distance) {
    if (distance == 0) continue;
    const auto to = encode_line(s, kFrom + distance, 300.0, kU, kV);
    const auto to_span = split_active(s, to);
    const auto to_burst = burst_of(s, to);
    const bool differs = split::v_switch_differs(s.system, distance);
    for (int32_t n = kActiveStart + 40; n < kActiveEnd - 40; ++n) {
      const auto i = static_cast<size_t>(n - kActiveStart);
      const auto relative =
          split::to_burst_relative(from_span.chroma[i], from_burst);
      const auto moved = split::from_burst_relative(
          split::across_v_switch(relative, differs), to_burst);
      EXPECT_NEAR(split::remodulate(moved, n),
                  split::remodulate(to_span.chroma[i], n), 1.0)
          << "distance " << distance << " sample " << n;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Systems, CompositeSplitTest,
                         ::testing::Values(kPal, kNtsc, kPalM),
                         [](const ::testing::TestParamInfo<System>& info) {
                           switch (info.param.system) {
                             case VideoSystem::PAL:
                               return std::string("PAL");
                             case VideoSystem::NTSC:
                               return std::string("NTSC");
                             default:
                               return std::string("PALM");
                           }
                         });

// On PAL the move across V-switch is what makes adjacent lines agree:
// without it the moved chroma is a different colour (29 codes off here,
// against under 1 with it).
TEST(CompositeSplit, PalAdjacentLineNeedsTheVSwitchMove) {
  const auto from = encode_line(kPal, 30, 300.0, -55.0, 75.0);
  const auto to = encode_line(kPal, 31, 300.0, -55.0, 75.0);
  const auto from_span = split_active(kPal, from);
  const auto to_span = split_active(kPal, to);
  const size_t i = (kActiveEnd - kActiveStart) / 2;
  const auto moved = split::from_burst_relative(
      split::to_burst_relative(from_span.chroma[i], burst_of(kPal, from)),
      burst_of(kPal, to));
  EXPECT_GT(std::abs(moved - to_span.chroma[i]), 20.0);
}

TEST(CompositeSplit, NoBurstGivesNoReference) {
  std::vector<int16_t> line(1135, 256);
  EXPECT_FALSE(split::burst_reference(line.data(), orc::kPalColourBurstStart,
                                      orc::kPalColourBurstEnd, 10.0)
                   .has_value());
}

TEST(CompositeSplit, VSwitchDiffersOnOddDistancesOfPalOnly) {
  EXPECT_TRUE(split::v_switch_differs(VideoSystem::PAL, 1));
  EXPECT_TRUE(split::v_switch_differs(VideoSystem::PAL_M, -3));
  EXPECT_FALSE(split::v_switch_differs(VideoSystem::PAL, 2));
  EXPECT_FALSE(split::v_switch_differs(VideoSystem::NTSC, 1));
}

}  // namespace
}  // namespace orc_unit_test
