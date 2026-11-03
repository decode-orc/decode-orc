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

// The leak's mark on Domesday2D's decode, each decode against the decode of
// the same pressing played without the leak (the variant keeps every noise
// seed, so only the leak differs): uncorrected (hanover_per_frame off, no
// matrix), with the measured matrix, with each frame's own rotation (the
// default), and with both. The correction mostly shows in the luma, whose
// residual reassignment re-encodes the chroma through the matrix; part of
// the mark is the transform's keep-or-drop decisions, which the leak moves
// and no correction after them undoes. With the matrix's sign reversed the
// mark roughly doubles. The per-frame fit keeps only the rotation (here
// 0.04 of the leak's 0.06 / -0.02), so on its own it takes most of the
// luma's mark out but leaves the leak's symmetric part, which costs V a
// little (0.059 to 0.063 %); with the measured matrix it does at least as
// well as the matrix alone.
TEST(Domesday2DTruth, PAL_Hanover) {
  const auto leak = calibrate(kHanover);
  ASSERT_TRUE(leak && leak->matrix.has_value());
  const auto& m = *leak->matrix;
  Parameters matrix;
  matrix["hanover_uu"] = m[0];
  matrix["hanover_uv"] = m[1];
  matrix["hanover_vu"] = m[2];
  matrix["hanover_vv"] = m[3];
  Parameters off;
  off["hanover_per_frame"] = false;
  Parameters fixed_only = matrix;
  fixed_only["hanover_per_frame"] = false;
  const auto reference =
      decode_case(kChannel, "domesday2d", off, "domesday2d_no_leak", kFrames);
  const auto plain =
      decode_case(kHanover, "domesday2d", off, "domesday2d_leak", kFrames);
  const auto fixed = decode_case(kHanover, "domesday2d", fixed_only,
                                 "domesday2d_leak_corrected", kFrames);
  const auto frame =
      decode_case(kHanover, "domesday2d", {}, "domesday2d_leak_frame", kFrames);
  const auto both = decode_case(kHanover, "domesday2d", matrix,
                                "domesday2d_leak_frame_corrected", kFrames);
  ASSERT_TRUE(reference && plain && fixed && frame && both);
  const auto before = orc_videosynth::plane_rms(*plain, *reference, 0, kFrames);
  ASSERT_TRUE(before);
  std::vector<Metric> metrics;
  const auto score = [&](const char* prefix, const auto& decoded,
                         bool all_planes) {
    const auto after =
        orc_videosynth::plane_rms(*decoded, *reference, 0, kFrames);
    ASSERT_TRUE(after);
    EXPECT_LT(after->y, before->y) << prefix;
    if (all_planes) {
      EXPECT_LT(after->u, before->u) << prefix;
      EXPECT_LT(after->v, before->v) << prefix;
    }
    for (const auto& [plane, value] :
         {std::pair<const char*, double>{"_y_rms_pct", after->y},
          {"_u_rms_pct", after->u},
          {"_v_rms_pct", after->v}}) {
      metrics.push_back({std::string(prefix) + plane, value, Better::Lower,
                         orc_videosynth::new_tolerance(value, 0.05, 0.002)});
    }
  };
  score("corrected", fixed, true);
  score("frame", frame, false);
  score("frame_corrected", both, true);
  const auto fixed_rms =
      orc_videosynth::plane_rms(*fixed, *reference, 0, kFrames);
  const auto both_rms =
      orc_videosynth::plane_rms(*both, *reference, 0, kFrames);
  ASSERT_TRUE(fixed_rms && both_rms);
  constexpr double kNoWorse = 0.002;  // % of full scale
  EXPECT_LE(both_rms->y, fixed_rms->y + kNoWorse);
  EXPECT_LE(both_rms->u, fixed_rms->u + kNoWorse);
  EXPECT_LE(both_rms->v, fixed_rms->v + kNoWorse);
  for (const auto& [name, value] :
       {std::pair<const char*, double>{"leak_y_rms_pct", before->y},
        {"leak_u_rms_pct", before->u},
        {"leak_v_rms_pct", before->v}}) {
    metrics.push_back({name, value, Better::Lower,
                       orc_videosynth::new_tolerance(value, 0.05, 0.002)});
  }
  orc_videosynth::expect_metrics(metrics);
}

}  // namespace orc_videosynth_test
