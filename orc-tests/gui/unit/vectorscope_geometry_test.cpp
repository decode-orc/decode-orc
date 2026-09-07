/*
 * File:        vectorscope_geometry_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Regression tests for shared vectorscope geometry helpers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview/vectorscope_geometry.h"

#include <gtest/gtest.h>

#include <cmath>

namespace gui_unit_test {

TEST(VectorscopeGeometryTest, PlotGeometry_MatchesVectorscopeRasterMapping) {
  const orc::gui::VectorscopePlotGeometry geometry;

  EXPECT_EQ(geometry.canvas_size, 1024);
  EXPECT_EQ(geometry.plot_padding, orc::gui::kVectorscopePlotPadding);
  EXPECT_DOUBLE_EQ(geometry.centre_point.x(), 512.0);
  EXPECT_DOUBLE_EQ(geometry.centre_point.y(), 512.0);
  EXPECT_DOUBLE_EQ(geometry.plot_area.left(), 16.0);
  EXPECT_DOUBLE_EQ(geometry.plot_area.top(), 16.0);
  EXPECT_DOUBLE_EQ(geometry.plot_area.right(), 1008.0);
  EXPECT_DOUBLE_EQ(geometry.plot_area.bottom(), 1008.0);

  const QPointF origin = geometry.mapUV(0.0, 0.0);
  EXPECT_DOUBLE_EQ(origin.x(), 512.0);
  EXPECT_DOUBLE_EQ(origin.y(), 512.0);

  const QPointF top_left =
      geometry.mapUV(-orc::gui::kVectorscopeSignedFullScale,
                     orc::gui::kVectorscopeSignedFullScale);
  EXPECT_DOUBLE_EQ(top_left.x(), 16.0);
  EXPECT_DOUBLE_EQ(top_left.y(), 16.0);

  const QPointF bottom_right =
      geometry.mapUV(orc::gui::kVectorscopeSignedFullScale,
                     -orc::gui::kVectorscopeSignedFullScale);
  EXPECT_DOUBLE_EQ(bottom_right.x(), 1008.0);
  EXPECT_DOUBLE_EQ(bottom_right.y(), 1008.0);
}

TEST(VectorscopeGeometryTest, DecodedTargets_AreTheSameForEverySystem) {
  // A decoded-component acquisition normalises U/V over the picture excursion
  // (black → white), which the setup pedestal shortens, so the 0.925 of the
  // SMPTE 170M-2004 §10 encoding equation is already divided out of the
  // samples.  One set of targets therefore serves every system — including
  // NTSC-J, which has no pedestal and so no 0.925 to account for.
  constexpr double kIreRange = 50000.0;

  for (int rgb = 1; rgb <= 6;
       ++rgb) {  // all six standard primaries/secondaries
    const orc::UVSample target =
        orc::gui::vectorscopeTargetUv(rgb, 0.75, kIreRange);

    for (const orc::VideoSystem system :
         {orc::VideoSystem::PAL, orc::VideoSystem::PAL_M,
          orc::VideoSystem::NTSC}) {
      const orc::UVSample display =
          orc::gui::vectorscopeDisplayTargetUv(rgb, 0.75, kIreRange, system);
      EXPECT_NEAR(display.u, target.u, 1e-9) << "rgb=" << rgb;
      EXPECT_NEAR(display.v, target.v, 1e-9) << "rgb=" << rgb;
    }
  }
}

TEST(VectorscopeGeometryTest, MeasurementTargets_CarryTheSmpteChromaScale) {
  // A composite acquisition measures the signal against blanking, so there the
  // encoding equation's 0.925 is visible and the NTSC / PAL-M targets must sit
  // 0.925× inside the PAL ones.
  constexpr double kIreRange = 50000.0;
  constexpr double kSetupFraction = 42.0 / 560.0;  // = 1 − 0.925

  for (int rgb = 1; rgb <= 6; ++rgb) {
    const orc::UVSample pal = orc::gui::measurementTargetUv(
        rgb, 0.75, kIreRange, orc::VideoSystem::PAL,
        orc::VectorscopeLinePhase::VPositive);
    const orc::UVSample ntsc = orc::gui::measurementTargetUv(
        rgb, 0.75, kIreRange, orc::VideoSystem::NTSC,
        orc::VectorscopeLinePhase::VPositive);

    EXPECT_NEAR(ntsc.u, pal.u * (1.0 - kSetupFraction), 1e-9) << "rgb=" << rgb;
    EXPECT_NEAR(ntsc.v, pal.v * (1.0 - kSetupFraction), 1e-9) << "rgb=" << rgb;
  }
}

TEST(VectorscopeGeometryTest, Ntsc_DisplayTargetsEqualRawTargets) {
  // calibrateVectorscopeDisplayUv is identity for NTSC: the comb decoder
  // already converts I/Q → U/V, so no coordinate-space rotation is required.
  // Display targets must equal the raw setup-adjusted targets without further
  // modification.
  constexpr double kIreRange = 50000.0;

  const orc::UVSample raw_target =
      orc::gui::vectorscopeTargetUv(4, 0.75, kIreRange);
  const orc::UVSample display_target = orc::gui::vectorscopeDisplayTargetUv(
      4, 0.75, kIreRange, orc::VideoSystem::NTSC);

  EXPECT_DOUBLE_EQ(display_target.u, raw_target.u);
  EXPECT_DOUBLE_EQ(display_target.v, raw_target.v);
}

TEST(VectorscopeGeometryTest, Pal_DisplayTargetsRemainUnchanged) {
  constexpr double kIreRange = 50000.0;

  const orc::UVSample raw_target =
      orc::gui::vectorscopeTargetUv(4, 0.75, kIreRange);
  const orc::UVSample display_target = orc::gui::vectorscopeDisplayTargetUv(
      4, 0.75, kIreRange, orc::VideoSystem::PAL);

  EXPECT_DOUBLE_EQ(display_target.u, raw_target.u);
  EXPECT_DOUBLE_EQ(display_target.v, raw_target.v);
}

TEST(VectorscopeGeometryTest,
     Seventy_FivePercentTargetScalesSampleSpaceMagnitude) {
  constexpr double kIreRange = 65535.0;

  const orc::UVSample full_target =
      orc::gui::vectorscopeTargetUv(6, 1.0, kIreRange);
  const orc::UVSample partial_target =
      orc::gui::vectorscopeTargetUv(6, 0.75, kIreRange);

  const double full_magnitude = std::hypot(full_target.u, full_target.v);
  const double partial_magnitude =
      std::hypot(partial_target.u, partial_target.v);

  EXPECT_NEAR(partial_magnitude, full_magnitude * 0.75, 1e-9);
}

TEST(VectorscopeGeometryTest, StandardDegrees_MapToExpectedScreenQuadrants) {
  const orc::gui::VectorscopePlotGeometry geometry;

  const QPointF right = geometry.pointFromStandardDegrees(
      0.0, orc::gui::kVectorscopeSignedFullScale);
  const QPointF up = geometry.pointFromStandardDegrees(
      90.0, orc::gui::kVectorscopeSignedFullScale);
  const QPointF left = geometry.pointFromStandardDegrees(
      180.0, orc::gui::kVectorscopeSignedFullScale);
  const QPointF down = geometry.pointFromStandardDegrees(
      270.0, orc::gui::kVectorscopeSignedFullScale);

  EXPECT_GT(right.x(), geometry.centre_point.x());
  EXPECT_NEAR(right.y(), geometry.centre_point.y(), 1e-6);

  EXPECT_LT(up.y(), geometry.centre_point.y());
  EXPECT_NEAR(up.x(), geometry.centre_point.x(), 1e-6);

  EXPECT_LT(left.x(), geometry.centre_point.x());
  EXPECT_NEAR(left.y(), geometry.centre_point.y(), 1e-6);

  EXPECT_GT(down.y(), geometry.centre_point.y());
  EXPECT_NEAR(down.x(), geometry.centre_point.x(), 1e-6);
}

TEST(VectorscopeGeometryTest, Ntsc_IAndQAxesAreAt123And33Degrees) {
  // SMPTE 170M-2004 §7.3: NTSC I and Q are rotated 33° from the BT.601 V and
  // U axes respectively.  In (U, V) space:
  //   Positive I maps to (-sin33°, cos33°) → atan2(cos33°, -sin33°) = 123°
  //   Positive Q maps to ( cos33°, sin33°) → atan2(sin33°,  cos33°) =  33°
  // These are the angles that must appear on the NTSC vectorscope graticule.
  constexpr double kDeg33 = 33.0 * M_PI / 180.0;
  const double sin33 = std::sin(kDeg33);
  const double cos33 = std::cos(kDeg33);

  // From U = -sin33°·I + cos33°·Q; V = cos33°·I + sin33°·Q (pure I: Q=0):
  const double i_u = -sin33;
  const double i_v = cos33;
  const double i_angle_deg = std::atan2(i_v, i_u) * 180.0 / M_PI;
  EXPECT_NEAR(i_angle_deg, 123.0, 0.5);

  // From U and V equations (pure Q: I=0):
  const double q_u = cos33;
  const double q_v = sin33;
  const double q_angle_deg = std::atan2(q_v, q_u) * 180.0 / M_PI;
  EXPECT_NEAR(q_angle_deg, 33.0, 0.5);
}

TEST(VectorscopeGeometryTest, MeasurementBurstMagnitude_MatchesSpecLevels) {
  // EBU Tech. 3280-E §1.2: PAL burst is 300 mV peak-to-peak on a 700 mV
  // luminance range, so its peak is 150/700 of the active video range.
  EXPECT_DOUBLE_EQ(
      orc::gui::nominalBurstMagnitudeUv(orc::VideoSystem::PAL, 1.0),
      150.0 / 700.0);

  // SMPTE 170M-2004 §8.4: NTSC burst is 40 IRE peak-to-peak → 20 IRE peak,
  // against a 100 IRE blanking→white range.  PAL-M follows the 525-line
  // levels (ITU-R BT.1700-1 Annex 1 Part B).
  EXPECT_DOUBLE_EQ(
      orc::gui::nominalBurstMagnitudeUv(orc::VideoSystem::NTSC, 1.0), 0.20);
  EXPECT_DOUBLE_EQ(
      orc::gui::nominalBurstMagnitudeUv(orc::VideoSystem::PAL_M, 1.0), 0.20);

  // The magnitude scales with the display's full scale.
  EXPECT_DOUBLE_EQ(
      orc::gui::nominalBurstMagnitudeUv(orc::VideoSystem::NTSC,
                                        orc::gui::kVectorscopeSignedFullScale),
      0.20 * orc::gui::kVectorscopeSignedFullScale);
}

TEST(VectorscopeGeometryTest, OnlyTheVSwitchedSystemsNeedTwoTargetSets) {
  // ITU-R BT.470-6 Table 2 item 2.16: PAL swings V line by line, so a PAL
  // measurement graticule carries mirrored target sets.
  EXPECT_TRUE(orc::gui::hasSwitchedVAxis(orc::VideoSystem::PAL));
  // ITU-R BT.1700-1 Annex 1 Part B: PAL-M is PAL colour encoding on the
  // 525-line raster, V-switch included — only its signal levels come from
  // NTSC — so it needs the mirrored sets too.
  EXPECT_TRUE(orc::gui::hasSwitchedVAxis(orc::VideoSystem::PAL_M));
  // SMPTE 170M-2004 §8.4: NTSC does not swing.
  EXPECT_FALSE(orc::gui::hasSwitchedVAxis(orc::VideoSystem::NTSC));
}

TEST(VectorscopeGeometryTest, MeasurementTargets_MirrorAboutTheUAxis) {
  constexpr double kIreRange = orc::gui::kVectorscopeSignedFullScale;

  for (const orc::VideoSystem system :
       {orc::VideoSystem::PAL, orc::VideoSystem::PAL_M}) {
    for (int rgb = 1; rgb <= 6; ++rgb) {
      const orc::UVSample positive = orc::gui::measurementTargetUv(
          rgb, 0.75, kIreRange, system, orc::VectorscopeLinePhase::VPositive);
      const orc::UVSample negative = orc::gui::measurementTargetUv(
          rgb, 0.75, kIreRange, system, orc::VectorscopeLinePhase::VNegative);
      // A −V line inverts V only, which is exactly what an undelayed composite
      // display shows.
      EXPECT_DOUBLE_EQ(negative.u, positive.u);
      EXPECT_DOUBLE_EQ(negative.v, -positive.v);
    }
  }
}

TEST(VectorscopeGeometryTest, BurstAnglesFollowTheSwingingBurstConvention) {
  // ITU-R BT.470-6 Table 2 item 2.16: PAL burst swings ±45° about the −U axis.
  EXPECT_DOUBLE_EQ(orc::gui::kPalBurstVPositiveDegrees, 135.0);
  EXPECT_DOUBLE_EQ(orc::gui::kPalBurstVNegativeDegrees, 225.0);
  // SMPTE 170M-2004 §8.4: the NTSC burst sits on the −U axis.
  EXPECT_DOUBLE_EQ(orc::gui::kNtscBurstDegrees, 180.0);

  const orc::gui::VectorscopePlotGeometry geometry;
  const double magnitude = orc::gui::nominalBurstMagnitudeUv(
      orc::VideoSystem::PAL, orc::gui::kVectorscopeSignedFullScale);

  // The two PAL burst vectors are mirror images about the U axis.
  const QPointF positive = geometry.pointFromStandardDegrees(
      orc::gui::kPalBurstVPositiveDegrees, magnitude);
  const QPointF negative = geometry.pointFromStandardDegrees(
      orc::gui::kPalBurstVNegativeDegrees, magnitude);
  EXPECT_NEAR(positive.x(), negative.x(), 1e-9);
  EXPECT_NEAR(positive.y() - geometry.centre_point.y(),
              geometry.centre_point.y() - negative.y(), 1e-9);
  EXPECT_LT(positive.x(), geometry.centre_point.x());
  EXPECT_LT(positive.y(), geometry.centre_point.y());
}

}  // namespace gui_unit_test