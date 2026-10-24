/*
 * File:        teletext_recovery_stats_test.cpp
 * Module:      orc-tests/core/unit/support
 * Purpose:     Unit tests for TeletextRecoveryStats (support tier)
 *
 * Line results are constructed in memory; the accumulator is pure and does no
 * I/O, so no filesystem, network or clock access is involved.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi-services/teletext_recovery_stats.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

#include "teletext_line_synthesizer.h"
#include "vbi-services/teletext_page_decoder.h"
#include "vbi-services/teletext_slicer.h"

namespace orc {
namespace tests {
namespace {

using Packet = std::array<uint8_t, kTeletextPacketBytes>;

// A parity-coded packet for |row| of magazine 1 whose data bytes are all
// valid odd parity (ETSI EN 300 706 §9.3.1).
Packet make_clean_packet(int row) {
  Packet packet{};
  const auto mrag = make_mrag(/*magazine=*/1, /*packet_number=*/row);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  for (size_t i = 2; i < packet.size(); ++i) {
    packet[i] = teletext_odd_parity_encode(static_cast<uint8_t>('A' + i % 26));
  }
  return packet;
}

// As above, with the data byte at |position| (0-39) made even parity.
Packet make_packet_with_parity_error(int row, size_t position) {
  Packet packet = make_clean_packet(row);
  packet[position + 2] ^= 0x80;  // §8.1: bit 8 is the parity bit
  return packet;
}

TeletextLineResult make_valid(
    const Packet& bytes,
    TeletextDetector detector = TeletextDetector::kThreshold) {
  TeletextLineResult result;
  result.valid = true;
  result.bytes = bytes;
  result.detector = detector;
  return result;
}

TeletextLineResult make_rejected(
    TeletextRejectReason reason,
    TeletextDetector detector = TeletextDetector::kThreshold) {
  TeletextLineResult result;
  result.valid = false;
  result.reject_reason = reason;
  result.detector = detector;
  return result;
}

TeletextLineResult make_mlse_rejection(TeletextRejectReason reason,
                                       double preamble_residual,
                                       double payload_residual) {
  TeletextLineResult result = make_rejected(reason, TeletextDetector::kMlse);
  result.preamble_residual = preamble_residual;
  result.payload_residual = payload_residual;
  return result;
}

// ---------------------------------------------------------------------------
// Accumulation
// ---------------------------------------------------------------------------

TEST(TeletextRecoveryStats, EmptyAccumulatorReportsNothing) {
  const TeletextRecoveryStats stats;
  EXPECT_EQ(stats.lines_seen(), 0u);
  EXPECT_EQ(stats.lines_with_burst(), 0u);
  EXPECT_EQ(stats.packets(), 0u);
  EXPECT_EQ(stats.parity_checked_packets(), 0u);
  EXPECT_EQ(stats.parity_failure_rate(0), 0.0);
  EXPECT_TRUE(stats.per_line().empty());
}

TEST(TeletextRecoveryStats, CountsPacketsPerDetector) {
  TeletextRecoveryStats stats;
  stats.add_line(7, make_valid(make_clean_packet(1)));
  stats.add_line(7, make_valid(make_clean_packet(2)));
  stats.add_line(8, make_valid(make_clean_packet(3), TeletextDetector::kMlse));

  EXPECT_EQ(stats.packets(), 3u);
  EXPECT_EQ(stats.packets_from(TeletextDetector::kThreshold), 2u);
  EXPECT_EQ(stats.packets_from(TeletextDetector::kMlse), 1u);
}

TEST(TeletextRecoveryStats, CountsRejectionsPerReason) {
  TeletextRecoveryStats stats;
  stats.add_line(7, make_rejected(TeletextRejectReason::kAmplitudeGate));
  stats.add_line(7, make_rejected(TeletextRejectReason::kAmplitudeGate));
  stats.add_line(7, make_rejected(TeletextRejectReason::kFramingCodeMiss));

  EXPECT_EQ(stats.rejections(TeletextRejectReason::kAmplitudeGate), 2u);
  EXPECT_EQ(stats.rejections(TeletextRejectReason::kFramingCodeMiss), 1u);
  EXPECT_EQ(stats.rejections(TeletextRejectReason::kParityFraction), 0u);
  EXPECT_EQ(stats.lines_seen(), 3u);
}

TEST(TeletextRecoveryStats, EmptyLinesAreNotCountedAsBursts) {
  // Only lines that got past the amplitude gate held anything to recover; the
  // rest are the empty VBI lines every field is mostly made of.
  TeletextRecoveryStats stats;
  stats.add_line(7, make_rejected(TeletextRejectReason::kAmplitudeGate));
  stats.add_line(7, make_rejected(TeletextRejectReason::kInsufficientSamples));
  stats.add_line(7, make_rejected(TeletextRejectReason::kRunInAmplitude));
  stats.add_line(7, make_valid(make_clean_packet(1)));

  EXPECT_EQ(stats.lines_seen(), 4u);
  EXPECT_EQ(stats.lines_with_burst(), 2u);
}

TEST(TeletextRecoveryStats, TracksEachVbiLineSeparately) {
  TeletextRecoveryStats stats;
  stats.add_line(7, make_valid(make_clean_packet(1)));
  stats.add_line(7, make_rejected(TeletextRejectReason::kPayloadResidual,
                                  TeletextDetector::kMlse));
  stats.add_line(9, make_rejected(TeletextRejectReason::kAmplitudeGate));

  const auto& per_line = stats.per_line();
  ASSERT_EQ(per_line.size(), 2u);

  const auto& line7 = per_line.at(7);
  EXPECT_EQ(line7.lines, 2u);
  EXPECT_EQ(line7.bursts, 2u);
  EXPECT_EQ(line7.packets, 1u);
  EXPECT_EQ(line7.failures, 1u);

  const auto& line9 = per_line.at(9);
  EXPECT_EQ(line9.lines, 1u);
  EXPECT_EQ(line9.bursts, 0u);
  EXPECT_EQ(line9.packets, 0u);
  EXPECT_EQ(line9.failures, 0u);
}

// ---------------------------------------------------------------------------
// Per-position parity profile
// ---------------------------------------------------------------------------

TEST(TeletextRecoveryStats, ParityFailureRateIsPerBytePosition) {
  TeletextRecoveryStats stats;
  for (int copy = 0; copy < 4; ++copy) {
    stats.add_line(7, make_valid(make_packet_with_parity_error(1, 3)));
  }
  stats.add_line(7, make_valid(make_clean_packet(1)));

  EXPECT_EQ(stats.parity_checked_packets(), 5u);
  EXPECT_EQ(stats.parity_failures(3), 4u);
  EXPECT_DOUBLE_EQ(stats.parity_failure_rate(3), 4.0 / 5.0);
  EXPECT_EQ(stats.parity_failures(2), 0u);
  EXPECT_DOUBLE_EQ(stats.parity_failure_rate(2), 0.0);
}

TEST(TeletextRecoveryStats, PositionsOutsideThePacketReportZero) {
  TeletextRecoveryStats stats;
  stats.add_line(7, make_valid(make_packet_with_parity_error(1, 39)));
  EXPECT_EQ(stats.parity_failures(39), 1u);
  EXPECT_EQ(stats.parity_failures(TeletextRecoveryStats::kDataBytes), 0u);
  EXPECT_EQ(stats.parity_failure_rate(TeletextRecoveryStats::kDataBytes), 0.0);
}

TEST(TeletextRecoveryStats, RowsWithoutByteParityAreNotParityChecked) {
  // Rows above 25 carry Hamming 24/18 triplets (§9.6) or independent data
  // (§9.8), so their bytes are not odd parity and must not enter the profile.
  TeletextRecoveryStats stats;
  stats.add_line(7, make_valid(make_clean_packet(26)));
  stats.add_line(7, make_valid(make_clean_packet(31)));

  EXPECT_EQ(stats.packets(), 2u);
  EXPECT_EQ(stats.parity_checked_packets(), 0u);
}

TEST(TeletextRecoveryStats, PacketsWithUndecodableAddressAreNotParityChecked) {
  // Without the row number there is no way to know whether the payload is
  // parity coded at all.
  Packet packet = make_clean_packet(1);
  packet[0] ^= 0b00000011;  // double-bit error: uncorrectable (§8.2)
  packet[1] ^= 0b00011000;

  TeletextRecoveryStats stats;
  stats.add_line(7, make_valid(packet));
  EXPECT_EQ(stats.packets(), 1u);
  EXPECT_EQ(stats.parity_checked_packets(), 0u);
}

// ---------------------------------------------------------------------------
// MLSE residual histograms
// ---------------------------------------------------------------------------

TEST(TeletextRecoveryStats, BinsMlseResidualsIncludingRejectedLines) {
  TeletextRecoveryStats stats;
  // 0.12 → bin 2 (0.10-0.15); 0.44 → bin 8 (0.40-0.45).
  auto recovered = make_valid(make_clean_packet(1), TeletextDetector::kMlse);
  recovered.preamble_residual = 0.12;
  recovered.payload_residual = 0.44;
  stats.add_line(7, recovered);
  stats.add_line(7, make_mlse_rejection(TeletextRejectReason::kPayloadResidual,
                                        0.12, 0.44));

  const auto& preamble = stats.preamble_residual_histogram();
  const auto& payload = stats.payload_residual_histogram();
  EXPECT_EQ(preamble[2], 2u);
  EXPECT_EQ(payload[8], 2u);
  EXPECT_EQ(preamble[8], 0u);
}

TEST(TeletextRecoveryStats, ResidualsAtOrAboveTheLastBinSaturate) {
  TeletextRecoveryStats stats;
  stats.add_line(7, make_mlse_rejection(TeletextRejectReason::kPreambleResidual,
                                        4.0, 0.0));
  const auto& preamble = stats.preamble_residual_histogram();
  EXPECT_EQ(preamble[TeletextRecoveryStats::kResidualBins - 1], 1u);
}

TEST(TeletextRecoveryStats, ThresholdResultsContributeNoResiduals) {
  // The threshold detector fits no channel, so it has no residual to report.
  TeletextRecoveryStats stats;
  stats.add_line(7, make_valid(make_clean_packet(1)));
  stats.add_line(7, make_rejected(TeletextRejectReason::kFramingCodeMiss));

  for (const auto count : stats.preamble_residual_histogram()) {
    EXPECT_EQ(count, 0u);
  }
  for (const auto count : stats.payload_residual_histogram()) {
    EXPECT_EQ(count, 0u);
  }
}

// ---------------------------------------------------------------------------
// Per-bit reconstruction-error profile
// ---------------------------------------------------------------------------

// An MLSE packet whose per-bit reconstruction error rises linearly from
// |first| at bit 0 to |last| at the final payload bit — the shape a bit clock
// running at the wrong rate leaves behind.
TeletextLineResult make_mlse_packet_with_error_ramp(const Packet& bytes,
                                                    double first, double last) {
  TeletextLineResult result = make_valid(bytes, TeletextDetector::kMlse);
  const auto span = static_cast<double>(kTeletextPayloadBits - 1);
  for (size_t bit = 0; bit < kTeletextPayloadBits; ++bit) {
    result.payload_bit_errors[bit] = static_cast<float>(
        first + (last - first) * static_cast<double>(bit) / span);
  }
  return result;
}

TEST(TeletextRecoveryStats, AveragesTheBitErrorProfileOverPackets) {
  TeletextRecoveryStats stats;
  stats.add_line(
      7, make_mlse_packet_with_error_ramp(make_clean_packet(1), 0.10, 0.10));
  stats.add_line(
      7, make_mlse_packet_with_error_ramp(make_clean_packet(1), 0.20, 0.20));

  EXPECT_EQ(stats.bit_error_packets(), 2u);
  EXPECT_NEAR(stats.bit_error_mean(0), 0.15, 1e-6);
  EXPECT_NEAR(stats.bit_error_mean(kTeletextPayloadBits - 1), 0.15, 1e-6);
  // Out of range, and before anything was recorded.
  EXPECT_EQ(stats.bit_error_mean(kTeletextPayloadBits), 0.0);
  EXPECT_EQ(TeletextRecoveryStats().bit_error_mean(0), 0.0);
}

TEST(TeletextRecoveryStats, BitErrorProfileKeepsItsShapeAlongThePacket) {
  TeletextRecoveryStats stats;
  stats.add_line(
      7, make_mlse_packet_with_error_ramp(make_clean_packet(1), 0.05, 0.45));

  EXPECT_NEAR(stats.bit_error_mean(0), 0.05, 1e-6);
  EXPECT_NEAR(stats.bit_error_mean(kTeletextPayloadBits - 1), 0.45, 1e-6);
  EXPECT_GT(stats.bit_error_mean(kTeletextPayloadBits / 2),
            stats.bit_error_mean(0));
}

TEST(TeletextRecoveryStats, OnlyRecoveredMlsePacketsEnterTheBitErrorProfile) {
  // A threshold packet reconstructs nothing, and the shape of a failed lock
  // says only that it failed — neither belongs in a profile read for drift.
  TeletextRecoveryStats stats;
  stats.add_line(7, make_valid(make_clean_packet(1)));
  auto rejected =
      make_mlse_rejection(TeletextRejectReason::kPayloadResidual, 0.12, 0.62);
  rejected.payload_bit_errors.fill(0.9F);
  stats.add_line(7, rejected);

  EXPECT_EQ(stats.bit_error_packets(), 0u);
  EXPECT_EQ(stats.bit_error_mean(0), 0.0);
}

// ---------------------------------------------------------------------------
// Decision confidence and parity repair
// ---------------------------------------------------------------------------

// An MLSE packet whose every byte was decided with |confidence|, |repaired| of
// them by flipping their least-confident bit to restore parity.
TeletextLineResult make_mlse_packet_with_confidence(const Packet& bytes,
                                                    float confidence,
                                                    int repaired = 0) {
  auto result = make_valid(bytes, TeletextDetector::kMlse);
  result.has_byte_confidence = true;
  result.byte_confidence.fill(confidence);
  result.repaired_bytes = repaired;
  return result;
}

TEST(TeletextRecoveryStats, AveragesDecisionConfidenceOverPackets) {
  TeletextRecoveryStats stats;
  stats.add_line(7,
                 make_mlse_packet_with_confidence(make_clean_packet(1), 0.8F));
  stats.add_line(8,
                 make_mlse_packet_with_confidence(make_clean_packet(2), 0.4F));

  EXPECT_EQ(stats.confidence_packets(), 2u);
  EXPECT_NEAR(stats.mean_byte_confidence(), 0.6, 1e-6);
}

TEST(TeletextRecoveryStats, PacketsWithoutAConfidenceMeasurementAreExcluded) {
  // A threshold packet has no path metric to measure, and counting it as
  // either confident or unsure would misreport the detector that did measure.
  TeletextRecoveryStats stats;
  stats.add_line(7, make_valid(make_clean_packet(1)));

  EXPECT_EQ(stats.confidence_packets(), 0u);
  EXPECT_EQ(stats.mean_byte_confidence(), 0.0);
}

TEST(TeletextRecoveryStats, CountsRepairedBytesAndThePacketsTheyWereIn) {
  TeletextRecoveryStats stats;
  stats.add_line(7, make_mlse_packet_with_confidence(make_clean_packet(1), 0.5F,
                                                     /*repaired=*/3));
  stats.add_line(8, make_mlse_packet_with_confidence(make_clean_packet(2), 0.5F,
                                                     /*repaired=*/1));
  stats.add_line(9,
                 make_mlse_packet_with_confidence(make_clean_packet(3), 0.5F));

  EXPECT_EQ(stats.bytes_repaired(), 4u);
  EXPECT_EQ(stats.packets_repaired(), 2u);
}

// ---------------------------------------------------------------------------
// Reset and summary
// ---------------------------------------------------------------------------

TEST(TeletextRecoveryStats, ResetClearsEverything) {
  TeletextRecoveryStats stats;
  auto packet = make_mlse_packet_with_error_ramp(
      make_packet_with_parity_error(1, 5), 0.05, 0.45);
  packet.has_byte_confidence = true;
  packet.byte_confidence.fill(0.7F);
  packet.repaired_bytes = 2;
  stats.add_line(7, packet);
  stats.add_line(8, make_rejected(TeletextRejectReason::kAmplitudeGate));
  stats.reset();

  EXPECT_EQ(stats.bit_error_packets(), 0u);
  EXPECT_EQ(stats.bit_error_mean(0), 0.0);
  EXPECT_EQ(stats.confidence_packets(), 0u);
  EXPECT_EQ(stats.mean_byte_confidence(), 0.0);
  EXPECT_EQ(stats.bytes_repaired(), 0u);
  EXPECT_EQ(stats.packets_repaired(), 0u);

  EXPECT_EQ(stats.lines_seen(), 0u);
  EXPECT_EQ(stats.lines_with_burst(), 0u);
  EXPECT_EQ(stats.packets(), 0u);
  EXPECT_EQ(stats.packets_from(TeletextDetector::kMlse), 0u);
  EXPECT_EQ(stats.rejections(TeletextRejectReason::kAmplitudeGate), 0u);
  EXPECT_EQ(stats.parity_checked_packets(), 0u);
  EXPECT_EQ(stats.parity_failures(5), 0u);
  EXPECT_TRUE(stats.per_line().empty());
  for (const auto count : stats.payload_residual_histogram()) {
    EXPECT_EQ(count, 0u);
  }
}

TEST(TeletextRecoveryStats, SummaryOfAnEmptyRunIsASingleLine) {
  const std::string summary = TeletextRecoveryStats().summary();
  EXPECT_NE(summary.find("0 candidate lines"), std::string::npos) << summary;
  EXPECT_EQ(summary.find('\n'), std::string::npos) << summary;
}

TEST(TeletextRecoveryStats, SummaryReportsEverySection) {
  TeletextRecoveryStats stats;
  auto packet = make_mlse_packet_with_error_ramp(
      make_packet_with_parity_error(1, 0), 0.05, 0.45);
  packet.has_byte_confidence = true;
  packet.byte_confidence.fill(0.75F);
  packet.repaired_bytes = 2;
  stats.add_line(7, packet);
  stats.add_line(7, make_mlse_rejection(TeletextRejectReason::kPayloadResidual,
                                        0.12, 0.62));
  stats.add_line(9, make_rejected(TeletextRejectReason::kAmplitudeGate));

  const std::string summary = stats.summary();
  EXPECT_NE(summary.find("3 candidate lines"), std::string::npos) << summary;
  EXPECT_NE(summary.find("2 with a data burst"), std::string::npos) << summary;
  EXPECT_NE(summary.find("1 packets"), std::string::npos) << summary;
  // Rejection reasons are named, not numbered.
  EXPECT_NE(summary.find("payload residual 1"), std::string::npos) << summary;
  EXPECT_NE(summary.find("amplitude gate 1"), std::string::npos) << summary;
  EXPECT_NE(summary.find("Odd-parity failures per data byte"),
            std::string::npos)
      << summary;
  EXPECT_NE(summary.find("MLSE preamble-fit residual"), std::string::npos)
      << summary;
  EXPECT_NE(summary.find("MLSE payload residual"), std::string::npos)
      << summary;
  EXPECT_NE(summary.find("MLSE reconstruction error per packet byte"),
            std::string::npos)
      << summary;
  EXPECT_NE(summary.find("Decision confidence: mean 0.75 over 1 packets"),
            std::string::npos)
      << summary;
  EXPECT_NE(summary.find("parity repaired 2 bytes in 1 packets"),
            std::string::npos)
      << summary;
  // The drift reading is the point of that section: the ramp fed in must show
  // as a last quarter well above the first.
  EXPECT_NE(summary.find("first quarter 10.0, last quarter 40.0"),
            std::string::npos)
      << summary;
  EXPECT_NE(summary.find("Per VBI line"), std::string::npos) << summary;
}

// A consumer reading back the host observer's stored observations gets the
// packet but not the slice that produced it, so the parity profile and the
// per-line yield are what it can contribute.
TEST(TeletextRecoveryStats, ObservedLinesBuildTheParityProfile) {
  TeletextRecoveryStats stats;
  const Packet clean = make_clean_packet(1);
  const Packet damaged = make_packet_with_parity_error(1, /*position=*/3);

  stats.add_observed_line(7, &clean, nullptr);
  stats.add_observed_line(7, &damaged, nullptr);
  stats.add_observed_line(8, nullptr, nullptr);

  EXPECT_EQ(stats.lines_seen(), 3u);
  EXPECT_EQ(stats.observed_lines(), 3u);
  EXPECT_EQ(stats.packets(), 2u);
  EXPECT_EQ(stats.parity_checked_packets(), 2u);
  EXPECT_EQ(stats.parity_failures(3), 1u);
  EXPECT_DOUBLE_EQ(stats.parity_failure_rate(3), 0.5);

  // A line with no packet is not evidence of a burst or of a rejection: a
  // stored observation cannot tell a failed recovery from a blank VBI line.
  EXPECT_EQ(stats.lines_with_burst(), 0u);
  EXPECT_EQ(stats.rejections(TeletextRejectReason::kAmplitudeGate), 0u);

  const auto& per_line = stats.per_line();
  ASSERT_EQ(per_line.count(7), 1u);
  EXPECT_EQ(per_line.at(7).lines, 2u);
  EXPECT_EQ(per_line.at(7).packets, 2u);
  EXPECT_EQ(per_line.at(8).packets, 0u);
}

TEST(TeletextRecoveryStats, ObservedConfidenceFeedsTheConfidenceMean) {
  TeletextRecoveryStats stats;
  const Packet packet = make_clean_packet(1);
  TeletextPacketConfidence high{};
  high.fill(0.8F);

  stats.add_observed_line(7, &packet, &high);
  stats.add_observed_line(7, &packet, nullptr);

  EXPECT_EQ(stats.confidence_packets(), 1u);
  EXPECT_NEAR(stats.mean_byte_confidence(), 0.8, 1e-6);
}

// Figures the observed path cannot support must not be printed as zeroes: a
// reader would take "0 with a data burst" for a finding.
TEST(TeletextRecoveryStats, SummaryOmitsSliceOnlyFiguresForObservedLines) {
  TeletextRecoveryStats stats;
  const Packet packet = make_clean_packet(1);
  stats.add_observed_line(7, &packet, nullptr);

  const std::string summary = stats.summary();
  EXPECT_NE(summary.find("1 candidate lines"), std::string::npos) << summary;
  EXPECT_NE(summary.find("1 packets recovered from stored observations"),
            std::string::npos)
      << summary;
  EXPECT_EQ(summary.find("with a data burst"), std::string::npos) << summary;
  EXPECT_EQ(summary.find("threshold"), std::string::npos) << summary;
  EXPECT_NE(summary.find("Per VBI line (lines/packets):"), std::string::npos)
      << summary;
}

TEST(TeletextRecoveryStats, SummaryKeepsSliceFiguresWhenLinesWereSliced) {
  TeletextRecoveryStats stats;
  stats.add_line(7, make_valid(make_clean_packet(1)));

  const std::string summary = stats.summary();
  EXPECT_NE(summary.find("with a data burst"), std::string::npos) << summary;
  EXPECT_NE(summary.find("threshold 1"), std::string::npos) << summary;
  EXPECT_NE(summary.find("Per VBI line (lines/bursts/packets/failures):"),
            std::string::npos)
      << summary;
}

// ---------------------------------------------------------------------------
// brief(): the per-field headline
// ---------------------------------------------------------------------------

TEST(TeletextRecoveryStats, BriefIsOneLineCarryingEveryHeadlineFigure) {
  TeletextRecoveryStats stats;
  // Two parity-coded packets, each carrying one damaged data byte, so the
  // parity headline is 2 failures in 80 data bytes: 2,5%.
  auto packet =
      make_valid(make_packet_with_parity_error(1, 0), TeletextDetector::kMlse);
  packet.has_byte_confidence = true;
  packet.byte_confidence.fill(0.75F);
  packet.repaired_bytes = 2;
  stats.add_line(7, packet);
  stats.add_line(8, make_valid(make_packet_with_parity_error(2, 5),
                               TeletextDetector::kMlse));
  stats.add_line(9, make_rejected(TeletextRejectReason::kFramingCodeMiss));
  stats.add_line(10, make_rejected(TeletextRejectReason::kAmplitudeGate));

  const std::string brief = stats.brief();
  EXPECT_EQ(brief.find('\n'), std::string::npos) << brief;
  EXPECT_NE(brief.find("2 packets from 3 bursts on 4 candidate lines"),
            std::string::npos)
      << brief;
  EXPECT_NE(brief.find("(all MLSE)"), std::string::npos) << brief;
  // Rejection reasons are named, as in the full summary.
  EXPECT_NE(brief.find("rejected: amplitude gate 1, framing code miss 1"),
            std::string::npos)
      << brief;
  EXPECT_NE(brief.find("odd parity failed on 2.5% of data bytes in 2 packets"),
            std::string::npos)
      << brief;
  EXPECT_NE(brief.find("confidence 0.75, 2 bytes repaired in 1 packets"),
            std::string::npos)
      << brief;

  // The tables the full profile prints are what brief() leaves behind.
  EXPECT_EQ(brief.find("byte "), std::string::npos) << brief;
  EXPECT_EQ(brief.find("Per VBI line"), std::string::npos) << brief;
  EXPECT_EQ(brief.find("residual"), std::string::npos) << brief;
}

TEST(TeletextRecoveryStats, BriefSpellsOutAMixedDetectorSplit) {
  TeletextRecoveryStats stats;
  stats.add_line(7, make_valid(make_clean_packet(1)));
  stats.add_line(8, make_valid(make_clean_packet(2), TeletextDetector::kMlse));

  EXPECT_NE(stats.brief().find("(threshold 1, MLSE 1)"), std::string::npos)
      << stats.brief();

  TeletextRecoveryStats threshold_only;
  threshold_only.add_line(7, make_valid(make_clean_packet(1)));
  EXPECT_NE(threshold_only.brief().find("(all threshold)"), std::string::npos)
      << threshold_only.brief();
}

TEST(TeletextRecoveryStats, BriefOmitsSliceOnlyFiguresForObservedLines) {
  TeletextRecoveryStats stats;
  const Packet packet = make_clean_packet(1);
  stats.add_observed_line(7, &packet, nullptr);

  const std::string brief = stats.brief();
  EXPECT_NE(brief.find("1 packets from 1 stored observations"),
            std::string::npos)
      << brief;
  EXPECT_EQ(brief.find("burst"), std::string::npos) << brief;
  EXPECT_EQ(brief.find("threshold"), std::string::npos) << brief;
  EXPECT_EQ(brief.find("MLSE"), std::string::npos) << brief;
}

TEST(TeletextRecoveryStats, BriefOfAnEmptyRunSaysSo) {
  const std::string brief = TeletextRecoveryStats().brief();
  EXPECT_EQ(brief, "0 packets");
}

TEST(TeletextRecoveryStats, SummaryIsStableForTheSameInput) {
  TeletextRecoveryStats first;
  TeletextRecoveryStats second;
  for (auto* stats : {&first, &second}) {
    stats->add_line(9, make_valid(make_clean_packet(2)));
    stats->add_line(7, make_rejected(TeletextRejectReason::kRunInAmplitude));
  }
  EXPECT_EQ(first.summary(), second.summary());
}

}  // namespace
}  // namespace tests
}  // namespace orc
