/*
 * File:        domesday2d_calibration_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     Domesday2D's line-alternating chroma matrix: the one Domesday2D
 *              Calibration reads on a player that leaks U and V into each
 *              other alternately with the V-switch, and what the decoder
 *              makes of it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <decoded_comparison.h>
#include <domesday2d_calibration.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sink_decode.h"
#include "videosynth_cases.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace {

using orc_videosynth::Better;
using orc_videosynth::CaseFile;
using orc_videosynth::Metric;

const CaseFile kChannel{"pal_channel", "videosynth_pal_channel",
                        orc::VideoSystem::PAL};
const CaseFile kHanover{"pal_channel_hanover", "videosynth_pal_channel_hanover",
                        orc::VideoSystem::PAL};

// projects/variants.json's channel-hanover crosstalk: M_UV = v_to_u,
// M_VU = u_to_v.
constexpr orc::domesday2d::Matrix kLeak{0.0, 0.06, -0.02, 0.0};
constexpr double kMatrixTolerance = 0.005;

// projects/channel/pal_channel.yaml's frames.
constexpr size_t kFrames = 12;

std::optional<orc::Domesday2DCalibration> calibrate(const CaseFile& file) {
  const auto artifact = orc_videosynth::open_case(file);
  const auto source = orc_videosynth::as_vfr(artifact);
  if (!source) return std::nullopt;
  return orc::calibrate_domesday2d(*source, orc::Domesday2DCalibrationOptions{},
                                   nullptr);
}

}  // namespace

// The tool reads the leak the player put in, and none where there is none.
TEST(Domesday2DTruth, PAL_Calibration) {
  const auto leak = calibrate(kHanover);
  const auto none = calibrate(kChannel);
  ASSERT_TRUE(leak && none);
  ASSERT_TRUE(leak->matrix.has_value());
  ASSERT_TRUE(none->matrix.has_value());
  std::vector<Metric> metrics;
  constexpr std::array<const char*, 4> kNames = {"uu", "uv", "vu", "vv"};
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_NEAR((*leak->matrix)[i], kLeak[i], kMatrixTolerance) << kNames[i];
    EXPECT_NEAR((*none->matrix)[i], 0.0, kMatrixTolerance) << kNames[i];
    const double error = std::abs((*leak->matrix)[i] - kLeak[i]);
    metrics.push_back({std::string("leak_") + kNames[i] + "_error", error,
                       Better::Lower,
                       orc_videosynth::new_tolerance(0.0, 0.0, 0.001)});
  }
  orc_videosynth::expect_metrics(metrics);
}

// The leak's mark on Domesday2D's decode, with the measured matrix and
// without it: each decode against the decode of the same pressing played
// without the leak (the variant keeps every noise seed, so only the leak
// differs). The correction mostly shows in the luma, whose residual
// reassignment re-encodes the chroma through the matrix; part of the mark
// is the transform's keep-or-drop decisions, which the leak moves and no
// correction after them undoes. With the matrix's sign reversed the mark
// roughly doubles.
TEST(Domesday2DTruth, PAL_Hanover) {
  const auto leak = calibrate(kHanover);
  ASSERT_TRUE(leak && leak->matrix.has_value());
  const auto& m = *leak->matrix;
  Parameters corrected;
  corrected["hanover_uu"] = m[0];
  corrected["hanover_uv"] = m[1];
  corrected["hanover_vu"] = m[2];
  corrected["hanover_vv"] = m[3];
  const auto reference =
      decode_case(kChannel, "domesday2d", {}, "domesday2d_no_leak", kFrames);
  const auto plain =
      decode_case(kHanover, "domesday2d", {}, "domesday2d_leak", kFrames);
  const auto fixed = decode_case(kHanover, "domesday2d", corrected,
                                 "domesday2d_leak_corrected", kFrames);
  ASSERT_TRUE(reference && plain && fixed);
  const auto before = orc_videosynth::plane_rms(*plain, *reference, 0, kFrames);
  const auto after = orc_videosynth::plane_rms(*fixed, *reference, 0, kFrames);
  ASSERT_TRUE(before && after);
  EXPECT_LT(after->y, before->y);
  EXPECT_LT(after->u, before->u);
  EXPECT_LT(after->v, before->v);
  std::vector<Metric> metrics;
  for (const auto& [name, value] :
       {std::pair<const char*, double>{"leak_y_rms_pct", before->y},
        {"leak_u_rms_pct", before->u},
        {"leak_v_rms_pct", before->v},
        {"corrected_u_rms_pct", after->u},
        {"corrected_v_rms_pct", after->v},
        {"corrected_y_rms_pct", after->y}}) {
    metrics.push_back({name, value, Better::Lower,
                       orc_videosynth::new_tolerance(value, 0.05, 0.002)});
  }
  orc_videosynth::expect_metrics(metrics);
}

}  // namespace orc_videosynth_test
