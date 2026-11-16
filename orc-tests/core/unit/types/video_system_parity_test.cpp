/*
 * File:        video_system_parity_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Per-system raster parity: SECAM has PAL's geometry, and the
 *              video-system string round trip
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/frame_line_util.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

using namespace orc;

namespace {

constexpr std::array<VideoSystem, 5> kAllSystems = {
    VideoSystem::PAL, VideoSystem::NTSC, VideoSystem::PAL_M,
    VideoSystem::Unknown, VideoSystem::SECAM};

// Everything a system's raster is: frame size, lines, field split, per-line
// offsets and counts, sample rate (and so frame rate) and audio cadence.
struct Raster {
  int32_t frame_samples = 0;
  int32_t frame_lines = 0;
  int32_t samples_per_line = 0;
  size_t field1 = 0;
  size_t padded_field = 0;
  double sample_rate = 0.0;
  double frame_rate = 0.0;
  double dar_correction = 0.0;
  std::vector<size_t> line_offsets;
  std::vector<size_t> line_counts;
  std::vector<uint64_t> audio_offsets;
};

Raster raster_of(VideoSystem system) {
  Raster r;
  r.frame_samples = frame_samples_from_system(system);
  r.frame_lines = frame_lines_from_system(system);
  r.samples_per_line = samples_per_line_from_system(system);
  r.field1 = field1_lines(system);
  r.padded_field = calculate_padded_field_height(system);
  r.sample_rate = sample_rate_from_system(system);
  r.frame_rate = r.frame_samples > 0
                     ? r.sample_rate / static_cast<double>(r.frame_samples)
                     : 0.0;
  r.dar_correction = standard_dar_correction(system);
  const auto spl = static_cast<size_t>(r.samples_per_line);
  for (int32_t line = 0; line < r.frame_lines; ++line) {
    const auto l = static_cast<size_t>(line);
    r.line_offsets.push_back(frame_line_sample_offset(system, spl, l));
    r.line_counts.push_back(frame_line_sample_count(system, spl, l));
  }
  for (uint64_t frame = 0; frame < 10; ++frame) {
    r.audio_offsets.push_back(audio_pair_offset(frame, system));
  }
  return r;
}

void expect_same_raster(const Raster& a, const Raster& b) {
  EXPECT_EQ(a.frame_samples, b.frame_samples);
  EXPECT_EQ(a.frame_lines, b.frame_lines);
  EXPECT_EQ(a.samples_per_line, b.samples_per_line);
  EXPECT_EQ(a.field1, b.field1);
  EXPECT_EQ(a.padded_field, b.padded_field);
  EXPECT_DOUBLE_EQ(a.sample_rate, b.sample_rate);
  EXPECT_DOUBLE_EQ(a.frame_rate, b.frame_rate);
  EXPECT_DOUBLE_EQ(a.dar_correction, b.dar_correction);
  EXPECT_EQ(a.line_offsets, b.line_offsets);
  EXPECT_EQ(a.line_counts, b.line_counts);
  EXPECT_EQ(a.audio_offsets, b.audio_offsets);
}

}  // namespace

// CVBS File Format Specification v1.8.1, Preset SECAM: the SECAM storage
// lattice is identical to the PAL 4fsc lattice, with PAL's line and field
// timing (ITU-R BT.1700-1 Annex 1 Part C).
TEST(VideoSystemParity, SecamRasterEqualsPal) {
  expect_same_raster(raster_of(VideoSystem::SECAM),
                     raster_of(VideoSystem::PAL));
}

TEST(VideoSystemParity, RasterOfEachSystem) {
  const auto pal = raster_of(VideoSystem::PAL);
  EXPECT_EQ(pal.frame_samples, 709'379);
  EXPECT_EQ(pal.frame_lines, 625);
  EXPECT_EQ(pal.field1, 313u);
  EXPECT_NEAR(pal.frame_rate, 25.0, 1e-9);
  EXPECT_EQ(pal.line_offsets.back() + pal.line_counts.back(), 709'379u);
  EXPECT_EQ(pal.audio_offsets[1], 1920u);

  for (const auto system : {VideoSystem::NTSC, VideoSystem::PAL_M}) {
    const auto r = raster_of(system);
    EXPECT_EQ(r.frame_lines, 525);
    EXPECT_EQ(r.field1, 263u);
    EXPECT_NEAR(r.frame_rate, 30000.0 / 1001.0, 1e-9);
    EXPECT_EQ(r.line_offsets.back() + r.line_counts.back(),
              static_cast<size_t>(r.frame_samples));
    EXPECT_EQ(r.audio_offsets[1], 1602u);
  }

  const auto unknown = raster_of(VideoSystem::Unknown);
  EXPECT_EQ(unknown.frame_samples, 0);
  EXPECT_EQ(unknown.frame_lines, 0);
  EXPECT_EQ(unknown.audio_offsets[1], 0u);
}

TEST(VideoSystemParity, FlatOffsetToLineSampleUsesTheLattice) {
  // Line 313 (frame-flat) follows the 1137-sample line 312.
  const size_t start = frame_line_sample_offset(VideoSystem::SECAM, 1135, 313);
  EXPECT_EQ(start, 313u * 1135u + 2u);
  const auto [line, sample] =
      frame_flat_offset_to_line_sample(VideoSystem::SECAM, 1135, start + 7);
  EXPECT_EQ(line, 313u);
  EXPECT_EQ(sample, 7u);
}

TEST(VideoSystemParity, Is625LineAndRasterSystem) {
  EXPECT_TRUE(is_625_line(VideoSystem::PAL));
  EXPECT_TRUE(is_625_line(VideoSystem::SECAM));
  EXPECT_FALSE(is_625_line(VideoSystem::NTSC));
  EXPECT_FALSE(is_625_line(VideoSystem::PAL_M));
  EXPECT_FALSE(is_625_line(VideoSystem::Unknown));

  EXPECT_EQ(raster_system(VideoSystem::SECAM), VideoSystem::PAL);
  for (const auto system : {VideoSystem::PAL, VideoSystem::NTSC,
                            VideoSystem::PAL_M, VideoSystem::Unknown}) {
    EXPECT_EQ(raster_system(system), system);
  }
}

// Appended after Unknown in ABI 21, so the released values keep their numbers.
TEST(VideoSystemParity, EnumValuesAreStable) {
  EXPECT_EQ(static_cast<int>(VideoSystem::PAL), 0);
  EXPECT_EQ(static_cast<int>(VideoSystem::NTSC), 1);
  EXPECT_EQ(static_cast<int>(VideoSystem::PAL_M), 2);
  EXPECT_EQ(static_cast<int>(VideoSystem::Unknown), 3);
  EXPECT_EQ(static_cast<int>(VideoSystem::SECAM), 4);
}

TEST(VideoSystemParity, StringRoundTrip) {
  for (const auto system : kAllSystems) {
    EXPECT_EQ(video_system_from_string(video_system_to_string(system)), system)
        << video_system_to_string(system);
  }
  EXPECT_EQ(video_system_to_string(VideoSystem::SECAM), "SECAM");
  EXPECT_EQ(video_system_from_string("SECAM"), VideoSystem::SECAM);
  EXPECT_EQ(video_system_from_string("MESECAM"), VideoSystem::Unknown);
}

TEST(VideoSystemParity, SecamLevelsAndCarriers) {
  // CVBS File Format Specification v1.8.1, Preset SECAM sample level table.
  EXPECT_EQ(kSecamBlanking, 256);
  EXPECT_EQ(kSecamWhite, 844);
  EXPECT_EQ(kSecamSyncTip, 4);
  EXPECT_EQ(kSecamPeak, 1019);
  // ITU-R BT.1700-1 Annex 1 Part C Table 4 items 5-6.
  EXPECT_DOUBLE_EQ(kSecamFor, 4'406'250.0);
  EXPECT_DOUBLE_EQ(kSecamFob, 4'250'000.0);
  EXPECT_EQ(default_amplitude_unit(VideoSystem::SECAM),
            AmplitudeDisplayUnit::Millivolts);
  EXPECT_EQ(video_format_from_system(VideoSystem::SECAM), VideoFormat::PAL);
  const auto [start, end] = colour_burst_range(VideoSystem::SECAM);
  EXPECT_EQ(start, kSecamSubcarrierReferenceStart);
  EXPECT_EQ(end, kSecamSubcarrierReferenceEnd);
}
