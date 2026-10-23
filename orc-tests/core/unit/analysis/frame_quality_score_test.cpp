/*
 * File:        frame_quality_score_test.cpp
 * Module:      analysis
 * Purpose:     Unit tests for disc mapper frame signal-quality scoring
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/core/analysis/disc_mapper/frame_quality_score.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <vector>

#include "../include/observation_context_interface_mock.h"

namespace {

using orc::compute_frame_quality_score;
using orc::dropout_fraction;
using orc::DropoutRun;
using orc::FieldID;
using orc::FrameID;
using orc::FrameQualityMetrics;
using orc::kNeutralFrameQualityScore;
using orc::nominal_burst_peak_10bit;
using orc::read_frame_quality_metrics;
using orc::VideoSystem;
using orc_unit_test::MockObservationContext;
using testing::_;
using testing::NiceMock;
using testing::Return;

// CVBS_U10_4FSC PAL level references: kPalBlanking = 256, kPalWhite = 844,
// so 100 IRE spans 588 sample units.
constexpr int32_t kPalBlanking = 256;
constexpr int32_t kPalWhite = 844;

// Nominal PAL burst peak in 10-bit units: 21.4286 IRE x 5.88 units/IRE.
constexpr double kPalNominalBurst = 126.0;

// ---------------------------------------------------------------------------
// nominal_burst_peak_10bit
// ---------------------------------------------------------------------------

TEST(FrameQualityNominalBurst, MatchesPalSpecAmplitude_ForPalLevels) {
  auto nominal =
      nominal_burst_peak_10bit(VideoSystem::PAL, kPalBlanking, kPalWhite);
  ASSERT_TRUE(nominal.has_value());
  EXPECT_NEAR(*nominal, kPalNominalBurst, 0.01);
}

TEST(FrameQualityNominalBurst, MatchesNtscSpecAmplitude_ForNtscLevels) {
  // SMPTE 170M-2004 Table 1: 40 IRE p-p → 20 IRE peak. Against a 100 IRE span
  // of 500 units that is 100 units.
  auto nominal = nominal_burst_peak_10bit(VideoSystem::NTSC, 240, 740);
  ASSERT_TRUE(nominal.has_value());
  EXPECT_NEAR(*nominal, 100.0, 0.01);
}

TEST(FrameQualityNominalBurst, ExceedsPalAmplitude_ForPalM) {
  auto pal =
      nominal_burst_peak_10bit(VideoSystem::PAL, kPalBlanking, kPalWhite);
  auto pal_m =
      nominal_burst_peak_10bit(VideoSystem::PAL_M, kPalBlanking, kPalWhite);
  ASSERT_TRUE(pal.has_value());
  ASSERT_TRUE(pal_m.has_value());
  // ITU-R BT.1700: 525 PAL burst is 316-317 mV p-p against 625 PAL's 300 mV.
  EXPECT_GT(*pal_m, *pal);
}

TEST(FrameQualityNominalBurst, ReturnsNothing_WhenLevelReferencesUnusable) {
  // Sources with no video parameters report -1 for every level.
  EXPECT_FALSE(nominal_burst_peak_10bit(VideoSystem::PAL, -1, -1).has_value());
  // Inverted references cannot define an IRE scale.
  EXPECT_FALSE(
      nominal_burst_peak_10bit(VideoSystem::PAL, 844, 256).has_value());
}

TEST(FrameQualityNominalBurst, ReturnsNothing_WhenSystemIsUnknown) {
  EXPECT_FALSE(
      nominal_burst_peak_10bit(VideoSystem::Unknown, kPalBlanking, kPalWhite)
          .has_value());
}

// ---------------------------------------------------------------------------
// compute_frame_quality_score
// ---------------------------------------------------------------------------

TEST(FrameQualityScore, ReturnsNeutralScore_WhenNoReadingsAvailable) {
  FrameQualityMetrics metrics;
  EXPECT_TRUE(metrics.empty());
  EXPECT_DOUBLE_EQ(compute_frame_quality_score(metrics, kPalNominalBurst),
                   kNeutralFrameQualityScore);
}

TEST(FrameQualityScore, ReturnsNeutralScore_WhenBurstHasNoNominalReference) {
  FrameQualityMetrics metrics;
  metrics.median_burst_10bit = kPalNominalBurst;
  // Without a reference amplitude the reading cannot be interpreted.
  EXPECT_DOUBLE_EQ(compute_frame_quality_score(metrics, std::nullopt),
                   kNeutralFrameQualityScore);
}

TEST(FrameQualityScore, ScoresFullMarks_WhenBurstIsAtNominal) {
  FrameQualityMetrics metrics;
  metrics.median_burst_10bit = kPalNominalBurst;
  EXPECT_DOUBLE_EQ(compute_frame_quality_score(metrics, kPalNominalBurst),
                   100.0);
}

TEST(FrameQualityScore, ScoresZero_WhenBurstHasCollapsed) {
  FrameQualityMetrics metrics;
  metrics.median_burst_10bit = 0.0;
  EXPECT_DOUBLE_EQ(compute_frame_quality_score(metrics, kPalNominalBurst), 0.0);
}

TEST(FrameQualityScore, PenalisesBurstSymmetrically_AboveAndBelowNominal) {
  FrameQualityMetrics under;
  under.median_burst_10bit = kPalNominalBurst * 0.8;
  FrameQualityMetrics over;
  over.median_burst_10bit = kPalNominalBurst * 1.2;

  EXPECT_NEAR(compute_frame_quality_score(under, kPalNominalBurst), 80.0, 1e-9);
  EXPECT_NEAR(compute_frame_quality_score(over, kPalNominalBurst), 80.0, 1e-9);
}

TEST(FrameQualityScore, PrefersStrongerBurst_WhenBothAreBelowNominal) {
  FrameQualityMetrics weak;
  weak.median_burst_10bit = kPalNominalBurst * 0.5;
  FrameQualityMetrics strong;
  strong.median_burst_10bit = kPalNominalBurst * 0.9;

  EXPECT_LT(compute_frame_quality_score(weak, kPalNominalBurst),
            compute_frame_quality_score(strong, kPalNominalBurst));
}

TEST(FrameQualityScore, ClampsSnrSubScore_WhenReadingIsOutsideUsefulWindow) {
  FrameQualityMetrics below_floor;
  below_floor.white_snr_db = 5.0;  // Well below the 20 dB floor
  EXPECT_DOUBLE_EQ(compute_frame_quality_score(below_floor, std::nullopt), 0.0);

  FrameQualityMetrics above_ceiling;
  above_ceiling.white_snr_db = 90.0;  // Well above the 48 dB ceiling
  EXPECT_DOUBLE_EQ(compute_frame_quality_score(above_ceiling, std::nullopt),
                   100.0);
}

TEST(FrameQualityScore, PrefersHigherWhiteSnr_WhenBurstReadingsMatch) {
  FrameQualityMetrics noisy;
  noisy.median_burst_10bit = kPalNominalBurst;
  noisy.white_snr_db = 26.0;

  FrameQualityMetrics clean;
  clean.median_burst_10bit = kPalNominalBurst;
  clean.white_snr_db = 42.0;

  EXPECT_LT(compute_frame_quality_score(noisy, kPalNominalBurst),
            compute_frame_quality_score(clean, kPalNominalBurst));
}

TEST(FrameQualityScore, PrefersHigherBlackPsnr_WhenOtherReadingsMatch) {
  FrameQualityMetrics noisy;
  noisy.median_burst_10bit = kPalNominalBurst;
  noisy.black_psnr_db = 24.0;

  FrameQualityMetrics clean;
  clean.median_burst_10bit = kPalNominalBurst;
  clean.black_psnr_db = 44.0;

  EXPECT_LT(compute_frame_quality_score(noisy, kPalNominalBurst),
            compute_frame_quality_score(clean, kPalNominalBurst));
}

TEST(FrameQualityScore, RenormalisesWeights_WhenSomeReadingsAreAbsent) {
  // A frame with only a perfect burst reading must still score 100, not 40
  // (its unrenormalised weight share).
  FrameQualityMetrics burst_only;
  burst_only.median_burst_10bit = kPalNominalBurst;
  EXPECT_DOUBLE_EQ(compute_frame_quality_score(burst_only, kPalNominalBurst),
                   100.0);

  // Likewise for a frame carrying only SNR readings at the top of the window.
  FrameQualityMetrics snr_only;
  snr_only.white_snr_db = 48.0;
  snr_only.black_psnr_db = 48.0;
  EXPECT_DOUBLE_EQ(compute_frame_quality_score(snr_only, kPalNominalBurst),
                   100.0);
}

TEST(FrameQualityScore, StaysWithinZeroToHundred_WhenAllReadingsPresent) {
  FrameQualityMetrics metrics;
  metrics.median_burst_10bit = kPalNominalBurst * 0.7;
  metrics.white_snr_db = 33.0;
  metrics.black_psnr_db = 38.0;

  const double score = compute_frame_quality_score(metrics, kPalNominalBurst);
  EXPECT_GE(score, 0.0);
  EXPECT_LE(score, 100.0);
}

// ---------------------------------------------------------------------------
// read_frame_quality_metrics
// ---------------------------------------------------------------------------

TEST(FrameQualityMetricsRead, ReadsFrameScopedKeys_WhenObservationsPresent) {
  NiceMock<MockObservationContext> context;
  const FrameID frame_id(7);
  // The quality observers key their frame-level results on the frame's first
  // FieldID (FieldID = FrameID * 2 + field_index).
  const FieldID expected_field_id(14);

  EXPECT_CALL(context,
              get(expected_field_id, "burst_level", "median_burst_10bit"))
      .WillOnce(Return(orc::ObservationValue(123.5)));
  EXPECT_CALL(context, get(expected_field_id, "white_snr", "snr_db"))
      .WillOnce(Return(orc::ObservationValue(41.25)));
  EXPECT_CALL(context, get(expected_field_id, "black_psnr", "psnr_db"))
      .WillOnce(Return(orc::ObservationValue(37.5)));

  auto metrics = read_frame_quality_metrics(context, frame_id);

  ASSERT_TRUE(metrics.median_burst_10bit.has_value());
  EXPECT_DOUBLE_EQ(*metrics.median_burst_10bit, 123.5);
  ASSERT_TRUE(metrics.white_snr_db.has_value());
  EXPECT_DOUBLE_EQ(*metrics.white_snr_db, 41.25);
  ASSERT_TRUE(metrics.black_psnr_db.has_value());
  EXPECT_DOUBLE_EQ(*metrics.black_psnr_db, 37.5);
  EXPECT_FALSE(metrics.empty());
}

TEST(FrameQualityMetricsRead, ReportsNoReadings_WhenObservationsAreMissing) {
  NiceMock<MockObservationContext> context;
  ON_CALL(context, get(_, _, _))
      .WillByDefault(Return(std::optional<orc::ObservationValue>{}));

  auto metrics = read_frame_quality_metrics(context, FrameID(0));

  EXPECT_TRUE(metrics.empty());
}

TEST(FrameQualityMetricsRead, IgnoresReading_WhenObservationIsNotADouble) {
  NiceMock<MockObservationContext> context;
  ON_CALL(context, get(_, _, _))
      .WillByDefault(Return(std::optional<orc::ObservationValue>{}));
  // An int32 in a double-valued slot must read as absent, never as zero — a
  // zero would look like a fully collapsed burst.
  ON_CALL(context, get(_, "burst_level", "median_burst_10bit"))
      .WillByDefault(Return(orc::ObservationValue(static_cast<int32_t>(0))));

  auto metrics = read_frame_quality_metrics(context, FrameID(3));

  EXPECT_FALSE(metrics.median_burst_10bit.has_value());
  EXPECT_TRUE(metrics.empty());
}

// ---------------------------------------------------------------------------
// Dropout share
// ---------------------------------------------------------------------------

TEST(FrameQualityDropout, SumsTheFlaggedSamples) {
  const std::vector<DropoutRun> runs = {{0, 100, 300, 50}, {0, 2000, 700, 50}};

  EXPECT_DOUBLE_EQ(*dropout_fraction(runs, 100000), 0.01);
  EXPECT_DOUBLE_EQ(*dropout_fraction({}, 100000), 0.0);
}

TEST(FrameQualityDropout, IsUnknown_WhenTheFrameSizeIsUnknown) {
  EXPECT_FALSE(dropout_fraction({{0, 0, 10, 50}}, 0).has_value());
}

TEST(FrameQualityDropout, ScoresFullMarksUnflaggedAndZeroAtTheCeiling) {
  FrameQualityMetrics clean;
  clean.dropout_fraction = 0.0;
  FrameQualityMetrics damaged;
  damaged.dropout_fraction = orc::kDropoutFractionCeiling;
  FrameQualityMetrics worse;
  worse.dropout_fraction = 0.5;

  EXPECT_DOUBLE_EQ(compute_frame_quality_score(clean, std::nullopt), 100.0);
  EXPECT_DOUBLE_EQ(compute_frame_quality_score(damaged, std::nullopt), 0.0);
  EXPECT_DOUBLE_EQ(compute_frame_quality_score(worse, std::nullopt), 0.0);
}

// Two copies whose burst and SNR read alike are told apart by their dropouts.
TEST(FrameQualityDropout, PrefersTheCleanerCopy_WhenOtherReadingsMatch) {
  FrameQualityMetrics clean;
  clean.median_burst_10bit = kPalNominalBurst;
  clean.white_snr_db = 40.0;
  clean.dropout_fraction = 0.0001;
  FrameQualityMetrics flagged = clean;
  flagged.dropout_fraction = 0.004;

  EXPECT_GT(compute_frame_quality_score(clean, kPalNominalBurst),
            compute_frame_quality_score(flagged, kPalNominalBurst));
}

}  // namespace
