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

// A row of a page in Zenith's VirText layout, as VirText recordings send it
// on 525 lines: the address, then 0x55, the 30 characters of the row
// and 0x55 again, the frame bytes failing parity as alternating bits do.
TeletextLineResult framed_packet() {
  TeletextLineResult result;
  result.valid = true;
  result.system = TeletextSystem::kWst525;
  result.packet_bytes = kTeletext525PacketBytes;
  const auto payload = make_525_test_payload();
  std::copy(payload.begin(), payload.begin() + kTeletext525PacketBytes,
            result.bytes.begin());
  const auto mrag = make_mrag(/*magazine=*/1, /*packet_number=*/1);
  result.bytes[0] = mrag[0];
  result.bytes[1] = mrag[1];
  result.bytes[2] = 0x55;
  result.bytes[kTeletext525PacketBytes - 1] = 0x55;
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

// A detector that reads most packets whole at the nominal clock leaves few for
// the clocks to disagree on, and a real gain can stand between one and three
// standard deviations after kPackets: scoring continues, and the clock is
// adopted once the lead is clear. Here the trial clock wins three packets of
// every fifty and loses one — 2.8 standard deviations after 400, 4 after 800.
TEST(TeletextClockCalibrator, KeepScoringWhileTheLeadIsOpen) {
  TeletextClockCalibrator calibrator;
  TeletextClockCalibrator::Scores scores{};
  TeletextLineResult whole = clean_packet();
  TeletextLineResult damaged = clean_packet();
  damaged.bytes[40] ^= 0x01;
  for (uint32_t i = 0; i < 2 * TeletextClockCalibrator::kPackets; ++i) {
    const bool trial_wins = i % 50 < 3;
    const bool trial_loses = i % 50 == 3;
    scores[trial_index(0.9994)] = !trial_loses;
    calibrator.observe(trial_wins ? damaged : whole, scores);
    if (i + 1 == TeletextClockCalibrator::kPackets) {
      EXPECT_TRUE(calibrator.calibrating());
      EXPECT_EQ(calibrator.bit_clock(), 1.0);
    }
  }
  EXPECT_FALSE(calibrator.calibrating());
  EXPECT_EQ(calibrator.bit_clock(), 0.9994);
  EXPECT_EQ(calibrator.packets(), 2 * TeletextClockCalibrator::kPackets);
}

// A lead that stays short of three standard deviations is not followed for
// ever: after kMaxPackets the nominal clock is kept. Here it stands at 2.8,
// 2.8 and 2.6 standard deviations after 400, 800 and 1,600 packets.
TEST(TeletextClockCalibrator, KeepTheNominalClockWhenTheLeadStaysOpen) {
  TeletextClockCalibrator calibrator;
  TeletextClockCalibrator::Scores scores{};
  TeletextLineResult whole = clean_packet();
  TeletextLineResult damaged = clean_packet();
  damaged.bytes[40] ^= 0x01;
  constexpr uint32_t kFirst = TeletextClockCalibrator::kPackets;
  for (uint32_t i = 0; i < TeletextClockCalibrator::kMaxPackets; ++i) {
    // 24 won and 8 lost of the first 400; 6 and 4 of the next 400; 10 and 8
    // of the last 800.
    bool trial_wins = false;
    bool trial_loses = false;
    if (i < kFirst) {
      trial_wins = i % 50 < 3;
      trial_loses = i % 50 == 3;
    } else if (i < 2 * kFirst) {
      trial_wins = i - kFirst < 6;
      trial_loses = i - kFirst >= 6 && i - kFirst < 10;
    } else {
      trial_wins = i - 2 * kFirst < 10;
      trial_loses = i - 2 * kFirst >= 10 && i - 2 * kFirst < 18;
    }
    scores[trial_index(1.0003)] = !trial_loses;
    calibrator.observe(trial_wins ? damaged : whole, scores);
    if (i + 1 < TeletextClockCalibrator::kMaxPackets) {
      ASSERT_TRUE(calibrator.calibrating()) << "packet " << i;
    }
  }
  EXPECT_FALSE(calibrator.calibrating());
  EXPECT_EQ(calibrator.bit_clock(), 1.0);
}

// A lead that fades is given up as soon as, grown with the square root of the
// packets as a real one grows, it could no longer pass by kMaxPackets: 2.8
// standard deviations after 400 packets, 1.3 after 800.
TEST(TeletextClockCalibrator, StopScoringWhenTheLeadFades) {
  TeletextClockCalibrator calibrator;
  TeletextClockCalibrator::Scores scores{};
  TeletextLineResult whole = clean_packet();
  TeletextLineResult damaged = clean_packet();
  damaged.bytes[40] ^= 0x01;
  constexpr uint32_t kFirst = TeletextClockCalibrator::kPackets;
  for (uint32_t i = 0; i < 2 * kFirst; ++i) {
    const bool trial_wins = i < kFirst && i % 50 < 3;
    const bool trial_loses = i < kFirst ? i % 50 == 3 : i - kFirst < 8;
    scores[trial_index(0.9994)] = !trial_loses;
    calibrator.observe(trial_wins ? damaged : whole, scores);
  }
  EXPECT_FALSE(calibrator.calibrating());
  EXPECT_EQ(calibrator.bit_clock(), 1.0);
  EXPECT_EQ(calibrator.packets(), 2 * kFirst);
}

// A byte of alternating bits never passes parity, so a System B row carries
// one only as a frame (Zenith's VirText layout) — and it is the byte a slightly
// wrong clock misreads first, into one that passes. Judged, it made every
// framed packet whole at a wrong clock only, and VirText recordings were read
// up to 0.24 % slow; left out, a trial clock gains nothing by misreading it,
// and the nominal clock is kept.
TEST(TeletextClockCalibrator, LeaveAlternatingBytesOfASystemBRowUnjudged) {
  TeletextClockCalibrator calibrator;
  TeletextClockCalibrator::Scores scores{};
  scores[trial_index(0.9976)] = true;  // reads the frames as 0x54, say
  const TeletextLineResult read = framed_packet();
  EXPECT_LT(teletext_verified_bytes(read), read.packet_bytes);
  for (uint32_t i = 0; i < TeletextClockCalibrator::kPackets; ++i) {
    calibrator.observe(read, scores);
  }
  EXPECT_FALSE(calibrator.calibrating());
  EXPECT_EQ(calibrator.bit_clock(), 1.0);
}

// A framed row damaged elsewhere is still evidence: a trial clock that reads
// it whole wins it.
TEST(TeletextClockCalibrator, JudgeAFramedRowOnItsOtherBytes) {
  TeletextClockCalibrator calibrator;
  TeletextClockCalibrator::Scores scores{};
  scores[trial_index(0.9991)] = true;
  TeletextLineResult read = framed_packet();
  read.bytes[20] ^= 0x01;  // the nominal clock loses a character
  for (uint32_t i = 0; i < TeletextClockCalibrator::kPackets; ++i) {
    calibrator.observe(read, scores);
  }
  EXPECT_EQ(calibrator.bit_clock(), 0.9991);
}

// System C codes its data bytes for parity in some data groups only (CEA-516
// §3.3), and no service is known to frame them: every byte is judged, so a
// trial clock that reads an alternating byte as one that passes wins the
// packet, as before.
TEST(TeletextClockCalibrator, JudgeEveryByteOfASystemCPacket) {
  TeletextClockCalibrator calibrator;
  TeletextClockCalibrator::Scores scores{};
  scores[trial_index(0.9991)] = true;
  TeletextLineResult read;
  read.valid = true;
  read.system = TeletextSystem::kNabts525;
  read.packet_bytes = kNabtsPacketBytes;
  const auto payload = make_nabts_test_payload();
  std::copy(payload.begin(), payload.begin() + kNabtsPacketBytes,
            read.bytes.begin());
  for (size_t i = 5; i < kNabtsPacketBytes; ++i) {
    read.bytes[i] = teletext_odd_parity_encode(static_cast<uint8_t>(i));
  }
  read.bytes[kNabtsPacketBytes - 1] = 0x55;
  for (uint32_t i = 0; i < TeletextClockCalibrator::kPackets; ++i) {
    calibrator.observe(read, scores);
  }
  EXPECT_EQ(calibrator.bit_clock(), 0.9991);
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

// The same on a 525-line row framed in Zenith's layout, sent 0,24 % slow: at
// the clock it was sent at, every byte but the frames checks, and the frames
// are not judged.
TEST(TeletextClockCalibrator, ScoreAFramedLineAtTheClockItWasSentAt) {
  constexpr double kSent = 0.9976;
  auto synth = ntsc_wst_synth_options();
  synth.low_pass_cutoff_hz = 2.8e6;
  synth.bit_rate = kTeletext525BitRate * kSent;
  const TeletextLineResult framed = framed_packet();
  const auto line =
      synthesize_teletext_line(framed.bytes, synth, kTeletext525PacketBytes);

  TeletextSlicerOptions options;
  options.detector = TeletextDetector::kMlse;
  const TeletextSlicer nominal(kNtscSampleRate, TeletextSystem::kWst525,
                               options);
  const auto read =
      nominal.slice(line.data(), line.size(), static_cast<int16_t>(kNtscBlack),
                    static_cast<int16_t>(kNtscWhite));
  ASSERT_GE(read.lock_sample, 0.0);

  const auto scores = TeletextClockCalibrator::score(
      line.data(), line.size(), static_cast<int16_t>(kNtscBlack),
      static_cast<int16_t>(kNtscWhite), kNtscSampleRate,
      TeletextSystem::kWst525, options, read);
  EXPECT_TRUE(scores[trial_index(kSent)]);
}

}  // namespace
}  // namespace tests
}  // namespace orc
