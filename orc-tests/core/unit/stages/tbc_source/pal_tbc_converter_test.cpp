/*
 * File:        pal_tbc_converter_test.cpp
 * Module:      orc-tests/core/unit/stages/tbc_source
 * Purpose:     Unit tests for PalTBCConverter level mapping and frame assembly.
 *
 * Tests:
 *   - tbc_to_cvbs level mapping at blanking, white, and sync-tip reference
 *     points (EBU Tech. 3280-E §1.3.1)
 *   - assemble_frame sample count equals kPalFrameSamples
 *   - CVBS field 1 (first 313 lines in output) is sourced from TBC field 1
 *   - Extra samples are inserted at frame-flat lines 312 and 624 (EBU3280)
 *   - map_field_phase_to_colour_frame_index covers all 1–8 phase values, -1
 *     for absent/invalid
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/tbc_source/pal_tbc_converter.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace orc_unit_test {

// ============================================================================
// Helpers
// ============================================================================

// Build a synthetic PAL TBC field filled with a constant uint16_t value.
static std::vector<uint16_t> make_field(int32_t lines, int32_t width,
                                        uint16_t fill) {
  return std::vector<uint16_t>(
      static_cast<size_t>(lines) * static_cast<size_t>(width), fill);
}

// Reference TBC levels used throughout these tests (ld-decode defaults for
// PAL).
constexpr int32_t kRefBlanking = 16384;
constexpr int32_t kRefWhite = 54400;

// TBC field line counts (matching EBU/ld-decode PAL convention):
//   TBC field 1 (odd-scan, isFirstField) = 313 lines → CVBS field 1
//   TBC field 2 (even-scan) = 312 lines → CVBS field 2
constexpr int32_t kTBCField1Lines = orc::kPalField1Lines;  // 313
constexpr int32_t kTBCField2Lines =
    orc::kPalFrameLines - orc::kPalField1Lines;                 // 312
constexpr int32_t kLineWidth = orc::kPalSamplesPerLineNominal;  // 1135

// ============================================================================
// tbc_to_cvbs — level mapping
// ============================================================================

TEST(PalTBCConverterTest, LevelMapping_BlankingInputYieldsCVBSBlanking) {
  // A sample at TBC blanking → CVBS blanking (256). EBU Tech. 3280-E §1.3.1.
  const int16_t result = orc::PalTBCConverter::tbc_to_cvbs(
      static_cast<uint16_t>(kRefBlanking), kRefBlanking, kRefWhite);
  EXPECT_EQ(result, static_cast<int16_t>(orc::kPalBlanking));
}

TEST(PalTBCConverterTest, LevelMapping_WhiteInputYieldsCVBSWhite) {
  const int16_t result = orc::PalTBCConverter::tbc_to_cvbs(
      static_cast<uint16_t>(kRefWhite), kRefBlanking, kRefWhite);
  EXPECT_EQ(result, static_cast<int16_t>(orc::kPalWhite));
}

TEST(PalTBCConverterTest, LevelMapping_LinearInterpolationMidpoint) {
  // Midpoint of [kRefBlanking, kRefWhite] → midpoint of [kPalBlanking,
  // kPalWhite].
  const int32_t tbc_mid = (kRefBlanking + kRefWhite) / 2;
  const int16_t result = orc::PalTBCConverter::tbc_to_cvbs(
      static_cast<uint16_t>(tbc_mid), kRefBlanking, kRefWhite);
  const int32_t expected = (orc::kPalBlanking + orc::kPalWhite) / 2;
  // Allow ±1 for rounding.
  EXPECT_NEAR(result, expected, 1);
}

TEST(PalTBCConverterTest,
     LevelMapping_BelowBlankingProducesValueBelowCVBSBlanking) {
  // Samples below TBC blanking (e.g. sync tip) map below CVBS blanking.
  const int16_t result = orc::PalTBCConverter::tbc_to_cvbs(
      static_cast<uint16_t>(kRefBlanking - 1000), kRefBlanking, kRefWhite);
  EXPECT_LT(result, static_cast<int16_t>(orc::kPalBlanking));
}

// The level map folds the frame-constant division into a single scale factor
// so the per-sample path is one multiply-add (see tbc_level_scale.h). That is
// deliberately not bit-identical to dividing per sample, so the contract the
// implementation owes callers is an accuracy bound, not an exact value: every
// sample must land within 1 LSB of the exact rational mapping.
TEST(PalTBCConverterTest, LevelMapping_WithinOneLsbOfExactRational) {
  // Nominal ld-decode levels plus off-nominal pairs, since these come from
  // per-capture metadata and vary.
  const std::pair<int32_t, int32_t> level_pairs[] = {{kRefBlanking, kRefWhite},
                                                     {16384, 54016},
                                                     {16000, 53000},
                                                     {17200, 55000},
                                                     {15000, 60000},
                                                     {20000, 50000}};

  for (const auto& [blanking, white] : level_pairs) {
    const orc::TbcLevelScale levels =
        orc::PalTBCConverter::level_scale(blanking, white);
    const int32_t span = orc::kPalWhite - orc::kPalBlanking;
    const int32_t den = white - blanking;

    // Whole 16-bit input domain, so no sample value can hide a larger error.
    for (int32_t sample = 0; sample <= 0xFFFF; ++sample) {
      // Exact rational mapping, rounded half away from zero, in integers.
      const int64_t num = static_cast<int64_t>(sample - blanking) * span;
      const int64_t exact = (num >= 0 ? (2 * num + den) / (2 * den)
                                      : -((-2 * num + den) / (2 * den))) +
                            orc::kPalBlanking;
      const int32_t mapped = levels.map(static_cast<uint16_t>(sample));
      ASSERT_LE(std::abs(mapped - static_cast<int32_t>(exact)), 1)
          << "sample " << sample << " with blanking=" << blanking
          << " white=" << white << ": mapped " << mapped << ", exact " << exact;
    }
  }
}

// The single-sample convenience overload and the map used for whole frames
// must agree, or a spot check would disagree with the frame it came from.
TEST(PalTBCConverterTest, LevelMapping_SingleSampleOverloadMatchesLevelScale) {
  const orc::TbcLevelScale levels =
      orc::PalTBCConverter::level_scale(kRefBlanking, kRefWhite);
  for (int32_t sample = 0; sample <= 0xFFFF; sample += 7) {
    EXPECT_EQ(orc::PalTBCConverter::tbc_to_cvbs(static_cast<uint16_t>(sample),
                                                kRefBlanking, kRefWhite),
              levels.map(static_cast<uint16_t>(sample)))
        << "sample " << sample;
  }
}

// ============================================================================
// assemble_frame — sample count and structure
// ============================================================================

TEST(PalTBCConverterTest,
     AssembleFrame_OutputSampleCountEqualskPalFrameSamples) {
  // Build minimal synthetic TBC fields at blanking level.
  auto f1 = make_field(kTBCField1Lines, kLineWidth,
                       static_cast<uint16_t>(kRefBlanking));  // 313 × 1135
  auto f2 = make_field(kTBCField2Lines, kLineWidth,
                       static_cast<uint16_t>(kRefBlanking));  // 312 × 1135

  const auto frame =
      orc::PalTBCConverter::assemble_frame(f1, f2, kRefBlanking, kRefWhite);

  EXPECT_EQ(static_cast<int32_t>(frame.size()), orc::kPalFrameSamples);
}

TEST(PalTBCConverterTest, AssembleFrame_ThrowsOnWrongField1Size) {
  // field1 must be 313 × 1135 samples.
  auto bad_f1 = make_field(kTBCField2Lines, kLineWidth,
                           static_cast<uint16_t>(kRefBlanking));  // 312, wrong
  auto f2 = make_field(kTBCField2Lines, kLineWidth,
                       static_cast<uint16_t>(kRefBlanking));
  EXPECT_THROW(
      orc::PalTBCConverter::assemble_frame(bad_f1, f2, kRefBlanking, kRefWhite),
      std::invalid_argument);
}

TEST(PalTBCConverterTest, AssembleFrame_ThrowsOnWrongField2Size) {
  auto f1 = make_field(kTBCField1Lines, kLineWidth,
                       static_cast<uint16_t>(kRefBlanking));
  auto bad_f2 = make_field(kTBCField1Lines, kLineWidth,
                           static_cast<uint16_t>(kRefBlanking));  // 313, wrong
  EXPECT_THROW(
      orc::PalTBCConverter::assemble_frame(f1, bad_f2, kRefBlanking, kRefWhite),
      std::invalid_argument);
}

TEST(PalTBCConverterTest, AssembleFrame_CVBSField1IsFirst313Lines) {
  // Fill TBC field 1 (313 lines, odd-scan) with white, TBC field 2 (312 lines)
  // with blanking.  The first 313 × 1135 samples in the output must be white
  // (with small deviations at the 4 extra-sample positions).
  // EBU Tech. 3280-E §1.3: field 1 (odd-scan) is placed first in the buffer.
  const uint16_t tbc_white = static_cast<uint16_t>(kRefWhite);
  const uint16_t tbc_blank = static_cast<uint16_t>(kRefBlanking);

  auto f1 = make_field(kTBCField1Lines, kLineWidth, tbc_white);  // TBC field 1
  auto f2 = make_field(kTBCField2Lines, kLineWidth, tbc_blank);  // TBC field 2

  const auto frame =
      orc::PalTBCConverter::assemble_frame(f1, f2, kRefBlanking, kRefWhite);

  // First sample of the frame must be kPalWhite (sourced from TBC field 1).
  EXPECT_EQ(frame[0], static_cast<int16_t>(orc::kPalWhite));

  // First sample of CVBS field 2 (after 313 lines) must be kPalBlanking.
  // EBU Tech. 3280-E §1.3.1: CVBS field 1 ends at frame-flat line 312 (last
  // of field 1), which carries 2 extra bridge samples.
  // Field 1 occupies samples 0 .. (313×1135 + 2) - 1 = 355,257 - 1.
  const size_t cvbs_field2_start =
      static_cast<size_t>(kTBCField1Lines) * static_cast<size_t>(kLineWidth) +
      2;  // +2 for the extra samples on line 312 (last of field 1)
  ASSERT_LT(cvbs_field2_start, frame.size());
  EXPECT_EQ(frame[cvbs_field2_start], static_cast<int16_t>(orc::kPalBlanking));
}

TEST(PalTBCConverterTest, AssembleFrame_ExtraSamplesInsertedAtLines312And624) {
  // EBU Tech. 3280-E §1.3.1: frame-flat lines 312 and 624 each carry 2 extra
  // samples.  Frame size baseline: 625 × 1135 = 709,375; + 4 extras →
  // 709,379 = kPalFrameSamples.
  //
  // The TBC fields are resampled onto the 4fsc lattice, which makes each one
  // two samples longer, so the extra samples are real signal from the end of
  // the field. Fill TBC field 1 line 312 with a distinctive value and confirm
  // it carries on through both extras, with field 2 starting after them.

  auto f1 = make_field(kTBCField1Lines, kLineWidth,
                       static_cast<uint16_t>(kRefBlanking));
  auto f2 = make_field(kTBCField2Lines, kLineWidth,
                       static_cast<uint16_t>(kRefBlanking));

  constexpr int32_t kLineWidth32 = kLineWidth;
  constexpr uint16_t kDistinctive = 40000;
  for (int i = 0; i < kLineWidth32; ++i) {
    f1[static_cast<size_t>(312 * kLineWidth32 + i)] = kDistinctive;
  }

  const auto frame =
      orc::PalTBCConverter::assemble_frame(f1, f2, kRefBlanking, kRefWhite);

  EXPECT_EQ(static_cast<int32_t>(frame.size()), orc::kPalFrameSamples);

  // The 2 extra samples on line 312 sit at offsets 354,120 + 1135 and + 1136.
  const size_t line312_start =
      static_cast<size_t>(312) * static_cast<size_t>(kLineWidth);
  const size_t extra0 = line312_start + static_cast<size_t>(kLineWidth);
  const size_t extra1 = extra0 + 1;
  ASSERT_LT(extra1 + 1, frame.size());

  const int16_t cvbs_distinctive =
      orc::PalTBCConverter::tbc_to_cvbs(kDistinctive, kRefBlanking, kRefWhite);
  EXPECT_NEAR(frame[extra0], cvbs_distinctive, 1);
  EXPECT_NEAR(frame[extra1], cvbs_distinctive, 1);
  // Line 313 (first of field 2) must be at blanking.
  EXPECT_EQ(frame[extra1 + 1], static_cast<int16_t>(orc::kPalBlanking));
}

// ============================================================================
// assemble_frame — line-locked → lattice conversion (issue #369)
// ============================================================================

namespace {

constexpr double kPi = 3.14159265358979323846;

// Burst-like carrier on every sample of a line-locked TBC field, as ld-decode
// stores it: sample m is one line-locked period (709379/709375 lattice
// samples) after sample m − 1, starting at 0H of the field's first frame row
// (row k's 0H sits at k × 709379 / 625 on the lattice).
std::vector<uint16_t> make_carrier_field(int32_t lines, int32_t first_row,
                                         double phase0) {
  const size_t n = static_cast<size_t>(lines) * static_cast<size_t>(kLineWidth);
  const double t0 = static_cast<double>(first_row) * 709379.0 / 625.0;
  std::vector<uint16_t> v(n);
  for (size_t m = 0; m < n; ++m) {
    const double t = t0 + static_cast<double>(m) * 709379.0 / 709375.0;
    v[m] = static_cast<uint16_t>(
        std::lround(30000.0 + 6000.0 * std::cos(kPi / 2.0 * t + phase0)));
  }
  return v;
}

// Carrier phase on frame row |row| (samples 100–1099 of the row) against the
// lattice phase π/2 × frame sample index, in degrees.
double row_phase_error_deg(const std::vector<int16_t>& frame, int32_t row,
                           double phase0) {
  const size_t start =
      static_cast<size_t>(row) * static_cast<size_t>(kLineWidth) +
      (row >= kTBCField1Lines ? 2u : 0u);
  double re = 0.0;
  double im = 0.0;
  double mean = 0.0;
  for (size_t n = start + 100; n < start + 1100; ++n) mean += frame[n];
  mean /= 1000.0;
  for (size_t n = start + 100; n < start + 1100; ++n) {
    const double a = kPi / 2.0 * static_cast<double>(n) + phase0;
    re += (frame[n] - mean) * std::cos(a);
    im -= (frame[n] - mean) * std::sin(a);
  }
  return std::atan2(im, re) * 180.0 / kPi;
}

}  // namespace

// ld-decode's PAL .tbc is line-locked, so its subcarrier drifts +0.576° per
// line against a 1135-sample grid. The frame must come out on the 4fsc
// lattice: residual drift under 0.05°/line in both fields, and the two fields
// on one lattice (issue #369 acceptance).
TEST(PalTBCConverterTest, AssembleFrame_RemovesTheLineLockedSubcarrierDrift) {
  const double phase0 = 1.1;
  const auto f1 = make_carrier_field(kTBCField1Lines, 0, phase0);
  const auto f2 = make_carrier_field(kTBCField2Lines, kTBCField1Lines, phase0);

  const auto frame =
      orc::PalTBCConverter::assemble_frame(f1, f2, kRefBlanking, kRefWhite);
  ASSERT_EQ(static_cast<int32_t>(frame.size()), orc::kPalFrameSamples);

  for (const auto& [top, bottom] :
       {std::pair<int32_t, int32_t>{10, 300}, {323, 613}}) {
    const double e_top = row_phase_error_deg(frame, top, phase0);
    const double e_bottom = row_phase_error_deg(frame, bottom, phase0);
    EXPECT_NEAR(e_top, 0.0, 0.2) << "row " << top;
    EXPECT_NEAR(e_bottom, 0.0, 0.2) << "row " << bottom;
    EXPECT_LT(std::abs(e_bottom - e_top) / (bottom - top), 0.05)
        << "rows " << top << "–" << bottom;
  }
}

}  // namespace orc_unit_test
