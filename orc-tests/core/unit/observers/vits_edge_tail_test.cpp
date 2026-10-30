/*
 * File:        vits_edge_tail_test.cpp
 * Module:      orc-tests/core/unit/observers
 * Purpose:     Unit tests for VitsEdgeTailMeter
 *
 * Tests: a known tail on the white bar's edges is measured back, PAL and
 * NTSC; a clean edge reads no tail; frames without a bar are passed over.
 * Frame data is synthesised in memory; no I/O is performed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/frame_line_util.h>
#include <vits_edge_tail.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace orc {
namespace tests {

namespace {

constexpr double kBlanking = 256.0;
constexpr double kWhite = 844.0;
constexpr double kPi = 3.14159265358979323846;
// As many as the capture comparison reads the bar lines of.
constexpr size_t kFrames = 32;

struct System {
  VideoSystem system;
  size_t width;
  size_t height;
  double line_us;
  size_t bar_line;  // 1-based field line
  double fall_us;
  double pulse_us;
};

const System kPal{VideoSystem::PAL, 1135, 625, 64.0, 19, 22.0, 26.0};
const System kNtsc{VideoSystem::NTSC, 910, 525, 63.5, 20, 30.0, 34.0};

// A raised-cosine step from 0 to 1, |width_us| wide, centred on 0.
double fast_step(double t, double width_us) {
  if (t <= -0.5 * width_us) return 0.0;
  if (t >= 0.5 * width_us) return 1.0;
  return 0.5 - 0.5 * std::cos(kPi * (t / width_us + 0.5));
}

// An edge at |at_us| whose last |fraction| settles over |tau_us|.
double edge(double t, double at_us, double fraction, double tau_us) {
  const double s = fast_step(t - at_us, 0.15);
  const double lag = t > at_us ? std::exp(-(t - at_us) / tau_us) : 1.0;
  return s * (1.0 - fraction * lag);
}

// Frames whose bar lines rise at 12.2 µs and fall at |fall_us| + 0.3, each
// edge with the tail given, a 2T-like pulse after, and noise of σ 3 codes,
// about a decode's at white.
std::vector<std::vector<int16_t>> frames(const System& s, size_t count,
                                         double fraction, double tau_us,
                                         bool with_bar = true) {
  std::mt19937 rng(7);
  std::normal_distribution<double> noise(0.0, 3.0);
  const double spu = static_cast<double>(s.width) / s.line_us;
  const size_t f1 = field1_lines(s.system);
  std::vector<std::vector<int16_t>> out;
  for (size_t f = 0; f < count; ++f) {
    std::vector<int16_t> plane(
        frame_line_sample_offset(s.system, s.width, s.height),
        static_cast<int16_t>(kBlanking));
    for (size_t field = 0; field < 2; ++field) {
      const size_t line = (field == 0 ? 0 : f1) + s.bar_line - 1;
      const size_t start = frame_line_sample_offset(s.system, s.width, line);
      const size_t length = frame_line_sample_count(s.system, s.width, line);
      for (size_t k = 0; k < length; ++k) {
        const double t = static_cast<double>(k) / spu;
        double v = kBlanking;
        if (with_bar) {
          v += (kWhite - kBlanking) *
               (edge(t, 12.2, fraction, tau_us) -
                edge(t, s.fall_us + 0.3, fraction, tau_us));
          v += 400.0 * std::exp(-std::pow((t - s.pulse_us) / 0.15, 2.0));
        }
        plane[start + k] = static_cast<int16_t>(std::lround(v + noise(rng)));
      }
    }
    out.push_back(std::move(plane));
  }
  return out;
}

EdgeTailMeasurement measure(const System& s, double fraction, double tau_us,
                            bool with_bar = true) {
  VitsEdgeTailMeter meter(s.system, s.width, kBlanking, kWhite);
  for (const auto& plane : frames(s, kFrames, fraction, tau_us, with_bar)) {
    meter.add_frame(plane, s.height);
  }
  return meter.measure();
}

void expect_tail(const std::optional<EdgeTail>& tail, double fraction,
                 double tau_us) {
  ASSERT_TRUE(tail.has_value());
  EXPECT_EQ(tail->lines, 2 * kFrames);
  EXPECT_NEAR(tail->fraction, fraction, 0.015);
  ASSERT_TRUE(tail->time_constant_us.has_value());
  EXPECT_NEAR(*tail->time_constant_us, tau_us, 0.1 * tau_us);
}

}  // namespace

TEST(VitsEdgeTailMeterTest, MeasuresAKnownTail_PAL) {
  const EdgeTailMeasurement m = measure(kPal, 0.12, 0.4);
  expect_tail(m.rising, 0.12, 0.4);
  expect_tail(m.falling, 0.12, 0.4);
}

TEST(VitsEdgeTailMeterTest, MeasuresAKnownTail_NTSC) {
  const EdgeTailMeasurement m = measure(kNtsc, 0.08, 0.5);
  expect_tail(m.rising, 0.08, 0.5);
  expect_tail(m.falling, 0.08, 0.5);
}

TEST(VitsEdgeTailMeterTest, ReadsNoTailOnACleanEdge) {
  const EdgeTailMeasurement m = measure(kPal, 0.0, 0.4);
  ASSERT_TRUE(m.rising.has_value());
  ASSERT_TRUE(m.falling.has_value());
  EXPECT_LT(std::abs(m.rising->fraction), kMinReportedEdgeTail);
  EXPECT_LT(std::abs(m.falling->fraction), kMinReportedEdgeTail);
  EXPECT_FALSE(m.rising->time_constant_us.has_value());
  EXPECT_FALSE(m.falling->time_constant_us.has_value());
}

TEST(VitsEdgeTailMeterTest, PassesOverFramesWithoutABar) {
  const EdgeTailMeasurement m = measure(kPal, 0.12, 0.4, false);
  EXPECT_FALSE(m.rising.has_value());
  EXPECT_FALSE(m.falling.has_value());
}

}  // namespace tests
}  // namespace orc
