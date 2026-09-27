/*
 * File:        teletext_bit_clock_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the bit-clock calibrator and the verified-byte
 *              count it scores by
 *
 * Lines are synthesized in memory; no filesystem, network, or clock access.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "vbi-services/teletext_bit_clock.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <cstddef>
#include <iterator>

#include "teletext_line_synthesizer.h"

namespace orc {
namespace tests {
namespace {

constexpr size_t kTrials = TeletextClockCalibrator::kTrialClocks.size();

TeletextLineResult clean_packet() {
  TeletextLineResult result;
  result.valid = true;
  result.system = TeletextSystem::kWst625;
  result.packet_bytes = kTeletextPacketBytes;
  result.bytes = make_parity_coded_payload();
  return result;
}

size_t trial_index(double clock) {
  const auto& trials = TeletextClockCalibrator::kTrialClocks;
  return static_cast<size_t>(std::distance(
      trials.begin(), std::find(trials.begin(), trials.end(), clock)));
}

TEST(TeletextVerifiedBytes, CountBytesThatCheckThemselves) {
  TeletextLineResult result = clean_packet();
  EXPECT_EQ(teletext_verified_bytes(result), kTeletextPacketBytes);

  result.bytes[10] ^= 0x01;  // breaks odd parity
  EXPECT_EQ(teletext_verified_bytes(result), kTeletextPacketBytes - 1);

  result.bytes[0] ^= 0x01;  // a corrected MRAG byte is not a checked one
  EXPECT_EQ(teletext_verified_bytes(result), kTeletextPacketBytes - 2);

  result.valid = false;
  EXPECT_EQ(teletext_verified_bytes(result), 0u);
}

TEST(TeletextClockCalibrator, AdoptTheClockThatReadsMostPacketsWhole) {
  TeletextClockCalibrator calibrator;
  TeletextClockCalibrator::Scores scores{};
  scores[trial_index(0.9991)] = true;
  TeletextLineResult read = clean_packet();
  read.bytes[40] ^= 0x01;  // the nominal clock loses the packet's last byte

  for (uint32_t i = 0; i + 1 < TeletextClockCalibrator::kPackets; ++i) {
    calibrator.observe(read, scores);
    ASSERT_TRUE(calibrator.calibrating());
    EXPECT_EQ(calibrator.bit_clock(), 1.0);
  }
  calibrator.observe(read, scores);
  EXPECT_FALSE(calibrator.calibrating());
  EXPECT_EQ(calibrator.bit_clock(), 0.9991);
  EXPECT_NE(calibrator.summary().find("0.09% slower"), std::string::npos)
      << calibrator.summary();
}

// On a corrected capture every clock scores within noise of the nominal one,
// and chasing that noise would read it at a wrong clock.
TEST(TeletextClockCalibrator, KeepTheNominalClockWithinChance) {
  TeletextClockCalibrator calibrator;
  TeletextClockCalibrator::Scores scores{};
  TeletextLineResult whole = clean_packet();
  TeletextLineResult damaged = clean_packet();
  damaged.bytes[40] ^= 0x01;
  // Of the packets the two clocks disagree on, the trial clock wins 10 and
  // loses 8: well inside three standard deviations of 18.
  for (uint32_t i = 0; i < TeletextClockCalibrator::kPackets; ++i) {
    const bool nominal_whole = i < 200;
    scores[trial_index(1.0003)] = i < 192 || (i >= 200 && i < 210);
    calibrator.observe(nominal_whole ? whole : damaged, scores);
  }
  EXPECT_FALSE(calibrator.calibrating());
  EXPECT_EQ(calibrator.bit_clock(), 1.0);
}

// Both clocks read the same packets, so a consistent gain stands out even when
// it is a small share of them.
TEST(TeletextClockCalibrator, AdoptAConsistentGainOverTheSamePackets) {
  TeletextClockCalibrator calibrator;
  TeletextClockCalibrator::Scores scores{};
  TeletextLineResult whole = clean_packet();
  TeletextLineResult damaged = clean_packet();
  damaged.bytes[40] ^= 0x01;
  for (uint32_t i = 0; i < TeletextClockCalibrator::kPackets; ++i) {
    scores[trial_index(0.9994)] = i < 230;
    calibrator.observe(i < 200 ? whole : damaged, scores);
  }
  EXPECT_EQ(calibrator.bit_clock(), 0.9994);
}

// A line that yielded no packet says nothing about the clock.
TEST(TeletextClockCalibrator, CountOnlyPackets) {
  TeletextClockCalibrator calibrator;
  TeletextLineResult rejected = clean_packet();
  rejected.valid = false;
  calibrator.observe(rejected, TeletextClockCalibrator::Scores{});
  EXPECT_EQ(calibrator.packets(), 0u);
}

// A tape played 0,24 % slow: read at the nominal clock the last bits of the
// packet slip by most of a bit, and at the trial clock it was sent at every
// byte checks.
TEST(TeletextClockCalibrator, ScoreALineBestAtTheClockItWasSentAt) {
  constexpr double kSent = 0.9976;
  auto synth = TeletextLineSynthOptions{};
  synth.low_pass_cutoff_hz = 2.8e6;
  synth.bit_rate = kTeletextBitRate * kSent;
  const auto line =
      synthesize_teletext_line(make_parity_coded_payload(), synth);

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const TeletextSlicer nominal(kPalSampleRate, TeletextSystem::kWst625,
                               options);
  const auto read =
      nominal.slice(line.data(), line.size(), static_cast<int16_t>(kPalBlack),
                    static_cast<int16_t>(kPalWhite));
  ASSERT_GE(read.lock_sample, 0.0);

  const auto scores = TeletextClockCalibrator::score(
      line.data(), line.size(), static_cast<int16_t>(kPalBlack),
      static_cast<int16_t>(kPalWhite), kPalSampleRate, TeletextSystem::kWst625,
      options, read);
  EXPECT_TRUE(scores[trial_index(kSent)]);
  EXPECT_LT(teletext_verified_bytes(read), kTeletextPacketBytes);
  EXPECT_FALSE(scores[trial_index(1.0030)]);
  static_assert(kTrials > 1, "the calibrator needs clocks to choose between");
}

}  // namespace
}  // namespace tests
}  // namespace orc
