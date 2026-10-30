/*
 * File:        sources_test.cpp
 * Module:      orc-videosynth-tests
 * Purpose:     cvbs_source and the observers on videosynth cases: CLV time
 *              codes and VITS, composite and Y/C, and the noise observers
 *              against the project's configured noise
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <biphase_observer.h>
#include <black_psnr_observer.h>
#include <gtest/gtest.h>
#include <orc/stage/common_types.h>
#include <orc/stage/field_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>
#include <white_snr_observer.h>

#include <cmath>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "videosynth_cases.h"
#include "videosynth_harness.h"

namespace orc_videosynth_test {

namespace {

using orc_videosynth::Better;
using orc_videosynth::CaseFile;
using orc_videosynth::Metric;

template <typename T>
std::optional<T> read(const orc::ObservationContext& ctx, orc::FrameID frame,
                      const std::string& ns, const std::string& key) {
  const auto value = ctx.get(orc::FieldID(frame * 2), ns, key);
  if (!value || !std::holds_alternative<T>(*value)) return std::nullopt;
  return std::get<T>(*value);
}

// ---------------------------------------------------------------------------
// CLV time codes and VITS (projects/sources/*_clv.yaml)
// ---------------------------------------------------------------------------

struct ClvCase {
  CaseFile file;
  size_t frames;  // duration_frames of the project
  int32_t frames_per_second;
};

void run_clv_case(const ClvCase& c) {
  const auto vfr = orc_videosynth::as_vfr(orc_videosynth::open_case(c.file));
  ASSERT_NE(vfr, nullptr);
  EXPECT_EQ(vfr->has_separate_channels(), c.file.yc);
  ASSERT_EQ(vfr->frame_count(), c.frames);

  orc::BiphaseObserver biphase;
  orc::WhiteSNRObserver white_snr;
  orc::BlackPSNRObserver black_psnr;
  orc::ObservationContext ctx;
  for (orc::FrameID f = 0; f < c.frames; ++f) {
    biphase.process_frame(*vfr, f, ctx);
    white_snr.process_frame(*vfr, f, ctx);
    black_psnr.process_frame(*vfr, f, ctx);

    // The time code starts at 0:00 and the CLV picture number counts the
    // frames of each second (the colour time correction of NTSC and PAL-M
    // first skips far beyond this case).
    const auto frame = static_cast<int32_t>(f);
    EXPECT_EQ(read<int32_t>(ctx, f, "vbi", "clv_timecode_hours"), 0)
        << "frame " << f;
    EXPECT_EQ(read<int32_t>(ctx, f, "vbi", "clv_timecode_minutes"), 0)
        << "frame " << f;
    EXPECT_EQ(read<int32_t>(ctx, f, "vbi", "clv_timecode_seconds"),
              frame / c.frames_per_second)
        << "frame " << f;
    EXPECT_EQ(read<int32_t>(ctx, f, "vbi", "clv_timecode_picture"),
              frame % c.frames_per_second)
        << "frame " << f;
    EXPECT_FALSE(read<int32_t>(ctx, f, "vbi", "picture_number").has_value())
        << "frame " << f << ": a CAV picture number on a CLV disc";

    // The VITS white bar is where White SNR looks for it, and the case has
    // no noise, so both measurements are far above any real capture's.
    const auto white = read<double>(ctx, f, "white_snr", "snr_db");
    const auto black = read<double>(ctx, f, "black_psnr", "psnr_db");
    ASSERT_TRUE(white.has_value()) << "frame " << f << ": no VITS white bar";
    ASSERT_TRUE(black.has_value()) << "frame " << f;
    EXPECT_GT(*white, 60.0) << "frame " << f;
    EXPECT_GT(*black, 60.0) << "frame " << f;
  }
}

ClvCase clv_case(const std::string& name, orc::VideoSystem system, bool yc,
                 size_t frames, int32_t fps) {
  return {{yc ? name + "_clv_yc" : name + "_clv", "videosynth_" + name + "_clv",
           system, yc},
          frames,
          fps};
}

// ---------------------------------------------------------------------------
// Noise observers (projects/noise/*_noise.yaml)
// ---------------------------------------------------------------------------

// Each section is four frames at one noise_db, with noise_spread_db 3.
struct NoiseSection {
  double noise_db;
  orc::FrameID first_frame;
};
constexpr NoiseSection kNoiseSections[] = {{38.0, 0}, {44.0, 4}, {50.0, 8}};
constexpr size_t kNoiseSectionFrames = 4;
constexpr double kNoiseSpreadDb = 3.0;

// How far a measurement may stand from the configured level. videosynth sets
// the level of the noise it adds; the observers measure that and the 10-bit
// quantisation besides, which is about 0.1 dB at 50 dB.
constexpr double kNoiseAcceptanceDb = 1.0;

void run_noise_case(const CaseFile& file) {
  const auto vfr = orc_videosynth::as_vfr(orc_videosynth::open_case(file));
  ASSERT_NE(vfr, nullptr);
  ASSERT_EQ(vfr->frame_count(),
            std::size(kNoiseSections) * kNoiseSectionFrames);

  orc::WhiteSNRObserver white_snr;
  orc::BlackPSNRObserver black_psnr;
  orc::ObservationContext ctx;
  std::vector<Metric> metrics;
  for (const auto& section : kNoiseSections) {
    double white_sum = 0.0;
    double black_sum = 0.0;
    for (size_t i = 0; i < kNoiseSectionFrames; ++i) {
      const orc::FrameID f = section.first_frame + i;
      white_snr.process_frame(*vfr, f, ctx);
      black_psnr.process_frame(*vfr, f, ctx);
      const auto white = read<double>(ctx, f, "white_snr", "snr_db");
      const auto black = read<double>(ctx, f, "black_psnr", "psnr_db");
      ASSERT_TRUE(white.has_value()) << "frame " << f << ": no VITS white bar";
      ASSERT_TRUE(black.has_value()) << "frame " << f;
      white_sum += *white;
      black_sum += *black;
    }
    const double white_db = white_sum / kNoiseSectionFrames;
    const double black_db = black_sum / kNoiseSectionFrames;
    const double white_error = white_db - (section.noise_db - kNoiseSpreadDb);
    const double black_error = black_db - section.noise_db;
    const std::string level =
        std::to_string(static_cast<int>(section.noise_db));
    EXPECT_LE(std::abs(black_error), kNoiseAcceptanceDb)
        << "Black PSNR " << black_db << " dB at noise_db " << section.noise_db;
    EXPECT_LE(std::abs(white_error), kNoiseAcceptanceDb)
        << "White SNR " << white_db << " dB at noise_db " << section.noise_db
        << " with a " << kNoiseSpreadDb << " dB spread";
    metrics.push_back({"black_psnr_" + level + "_error_db",
                       std::abs(black_error), Better::Lower, 0.25});
    metrics.push_back({"white_snr_" + level + "_error_db",
                       std::abs(white_error), Better::Lower, 0.25});
  }
  orc_videosynth::expect_metrics(metrics);
}

CaseFile noise_case(const std::string& name, orc::VideoSystem system, bool yc) {
  return {yc ? name + "_noise_yc" : name + "_noise",
          "videosynth_" + name + "_noise", system, yc};
}

}  // namespace

TEST(VideosynthSources, PAL_CLV_Composite) {
  run_clv_case(clv_case("pal", orc::VideoSystem::PAL, false, 27, 25));
}

TEST(VideosynthSources, PAL_CLV_YC) {
  run_clv_case(clv_case("pal", orc::VideoSystem::PAL, true, 27, 25));
}

TEST(VideosynthSources, NTSC_CLV_Composite) {
  run_clv_case(clv_case("ntsc", orc::VideoSystem::NTSC, false, 32, 30));
}

TEST(VideosynthSources, NTSC_CLV_YC) {
  run_clv_case(clv_case("ntsc", orc::VideoSystem::NTSC, true, 32, 30));
}

TEST(VideosynthSources, PALM_CLV_Composite) {
  run_clv_case(clv_case("pal_m", orc::VideoSystem::PAL_M, false, 32, 30));
}

TEST(VideosynthSources, PALM_CLV_YC) {
  run_clv_case(clv_case("pal_m", orc::VideoSystem::PAL_M, true, 32, 30));
}

TEST(NoiseObservers, PAL_Composite) {
  run_noise_case(noise_case("pal", orc::VideoSystem::PAL, false));
}

TEST(NoiseObservers, PAL_YC) {
  run_noise_case(noise_case("pal", orc::VideoSystem::PAL, true));
}

TEST(NoiseObservers, NTSC_Composite) {
  run_noise_case(noise_case("ntsc", orc::VideoSystem::NTSC, false));
}

TEST(NoiseObservers, NTSC_YC) {
  run_noise_case(noise_case("ntsc", orc::VideoSystem::NTSC, true));
}

TEST(NoiseObservers, PALM_Composite) {
  run_noise_case(noise_case("pal_m", orc::VideoSystem::PAL_M, false));
}

TEST(NoiseObservers, PALM_YC) {
  run_noise_case(noise_case("pal_m", orc::VideoSystem::PAL_M, true));
}

}  // namespace orc_videosynth_test
