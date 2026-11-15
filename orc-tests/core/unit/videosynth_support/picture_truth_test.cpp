/*
 * File:        picture_truth_test.cpp
 * Module:      orc-core-unit-tests
 * Purpose:     Reading videosynth's picture truth, and placing a video
 *              sink's raw decode on the truth grid
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <picture_truth.h>

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace {

using orc_videosynth::DecodedFile;
using orc_videosynth::TruthFile;
using orc_videosynth::TruthMeta;

constexpr size_t kSamples = 4;
constexpr size_t kRows = 4;

// The .truth.meta row of a one-frame PAL truth of 4 x 4 samples from
// sample 177: black 0 mV, white 700 mV. Field 2 (the lower field) opens on
// line 622, so the half-picture line 623 is its second row, frame row 3,
// and its picture ends at sample 179, two samples in.
TruthMeta meta_row() {
  TruthMeta meta;
  meta.video_standard = "PAL";
  auto& g = meta.geometry;
  g.sample_rate_hz = 17734475.0;
  g.frame_count = 1;
  g.samples_per_line = kSamples;
  g.rows = kRows;
  g.first_sample = 177;
  g.first_line_field1 = 22;
  g.first_line_field2 = 622;
  g.upper_field = 1;
  g.half_picture_line = 623;
  g.half_picture_end_sample = 179;
  g.black_mv = 0.0;
  g.white_mv = 700.0;
  meta.plane_order = "YUV";
  meta.sample_format = "float32le";
  meta.units = "mV";
  meta.y_reference = "blanking";
  meta.u_axis = "sin";
  meta.v_axis = "cos";
  meta.v_switch = "removed";
  return meta;
}

// The .truth file's bytes for that frame: Y half way, U 70 mV, V -35 mV.
std::vector<uint8_t> truth_bytes() {
  std::vector<uint8_t> raw;
  for (size_t plane = 0; plane < 3; ++plane) {
    const float value = plane == 0 ? 350.0F : (plane == 1 ? 70.0F : -35.0F);
    uint32_t word = 0;
    std::memcpy(&word, &value, sizeof(word));
    for (size_t n = 0; n < kSamples * kRows; ++n) {
      for (int shift = 0; shift < 32; shift += 8) {
        raw.push_back(static_cast<uint8_t>((word >> shift) & 0xFF));
      }
    }
  }
  return raw;
}

TEST(VideosynthPictureTruth, readsGeometryAndScalesToExcursion) {
  std::string error;
  const auto g = orc_videosynth::truth_geometry(meta_row(), &error);
  ASSERT_TRUE(g.has_value()) << error;
  EXPECT_EQ(g->system, orc::VideoSystem::PAL);
  EXPECT_EQ(g->samples_per_line, kSamples);
  EXPECT_EQ(g->rows, kRows);
  const auto truth = TruthFile::from_bytes(*g, truth_bytes(), &error);
  ASSERT_TRUE(truth.has_value()) << error;
  const auto frame = truth->frame(0);
  EXPECT_DOUBLE_EQ(frame.y[5], 0.5);
  EXPECT_DOUBLE_EQ(frame.u[5], 0.1);
  EXPECT_DOUBLE_EQ(frame.v[5], -0.05);
}

TEST(VideosynthPictureTruth, masksTheHalfPictureLineTail) {
  const auto g = orc_videosynth::truth_geometry(meta_row());
  ASSERT_TRUE(g.has_value());
  const auto truth = TruthFile::from_bytes(*g, truth_bytes());
  ASSERT_TRUE(truth.has_value());
  const auto mask = truth->picture_mask();
  for (size_t n = 0; n < mask.size(); ++n) {
    const bool tail = n >= (3 * kSamples) + 2;
    EXPECT_EQ(mask[n], tail ? 0 : 1) << "sample " << n;
  }
}

TEST(VideosynthPictureTruth, rejectsAnotherSampleFormat) {
  auto meta = meta_row();
  meta.sample_format = "int16le";
  std::string error;
  EXPECT_FALSE(orc_videosynth::truth_geometry(meta, &error).has_value());
  EXPECT_NE(error.find("format"), std::string::npos);
}

TEST(VideosynthPictureTruth, rejectsAnUnknownStandard) {
  auto meta = meta_row();
  meta.video_standard = "PAL_N";
  EXPECT_FALSE(orc_videosynth::truth_geometry(meta).has_value());
}

// A SECAM .truth.meta row: the same geometry as meta_row(), planes Y, D'R
// and D'B (videosynth docs/reference/output.md, "SECAM planes"), and no
// carrier axes.
TruthMeta secam_meta_row() {
  TruthMeta meta = meta_row();
  meta.video_standard = "SECAM";
  meta.plane_order = "YDRDB";
  meta.u_axis.clear();
  meta.v_axis.clear();
  meta.v_switch.clear();
  meta.colour_difference_units = "nominal_deviation";
  meta.geometry.dr_per_r_minus_y = -1.902;
  meta.geometry.db_per_b_minus_y = 1.505;
  return meta;
}

// D'R and D'B become V and U: V = 0.877 (R'-Y') = 0.877 D'R / -1.902 and
// U = 0.493 (B'-Y') = 0.493 D'B / 1.505. truth_bytes() gives D'R 70 and
// D'B -35.
TEST(VideosynthPictureTruth, readsSecamColourDifferenceAsUAndV) {
  std::string error;
  const auto g = orc_videosynth::truth_geometry(secam_meta_row(), &error);
  ASSERT_TRUE(g.has_value()) << error;
  EXPECT_EQ(g->system, orc::VideoSystem::SECAM);
  const auto parsed = TruthFile::from_bytes(*g, truth_bytes(), &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  const auto frame = parsed->frame(0);
  EXPECT_DOUBLE_EQ(frame.y[5], 0.5);
  EXPECT_NEAR(frame.v[5], 0.87728322 * 70.0 / -1.902, 1e-6);
  EXPECT_NEAR(frame.u[5], 0.49211104 * -35.0 / 1.505, 1e-6);
}

TEST(VideosynthPictureTruth, rejectsSecamWithoutItsScale) {
  auto meta = secam_meta_row();
  meta.geometry.dr_per_r_minus_y = 0.0;
  std::string error;
  EXPECT_FALSE(orc_videosynth::truth_geometry(meta, &error).has_value());
  EXPECT_NE(error.find("scale"), std::string::npos);
}

TEST(VideosynthPictureTruth, rejectsSecamWithCarrierPlanes) {
  auto meta = secam_meta_row();
  meta.plane_order = "YUV";
  EXPECT_FALSE(orc_videosynth::truth_geometry(meta).has_value());
}

// dr_line_parity 1: the odd-numbered frame lines carry D'R. Field 1 opens
// on line 22 (rows 0 and 2 are lines 22 and 23), field 2 on line 622 (rows
// 1 and 3 are 622 and 623). Frame 1 has parity 0, so every row swaps.
TEST(VideosynthPictureTruth, givesEachSecamRowItsTransmittedComponent) {
  const auto g = orc_videosynth::truth_geometry(secam_meta_row());
  ASSERT_TRUE(g.has_value());
  const TruthFile truth(*g, std::vector<float>(2 * 3 * kSamples * kRows),
                        {1, 0});
  EXPECT_EQ(orc_videosynth::truth_row_frame_line(*g, 0), 22);
  EXPECT_EQ(orc_videosynth::truth_row_frame_line(*g, 1), 622);
  EXPECT_EQ(orc_videosynth::truth_row_frame_line(*g, 2), 23);
  EXPECT_EQ(orc_videosynth::truth_row_frame_line(*g, 3), 623);
  EXPECT_EQ(truth.row_transmits_dr(0, 0), std::optional<bool>(false));
  EXPECT_EQ(truth.row_transmits_dr(0, 2), std::optional<bool>(true));
  EXPECT_EQ(truth.row_transmits_dr(0, 3), std::optional<bool>(true));
  EXPECT_EQ(truth.row_transmits_dr(1, 0), std::optional<bool>(true));
  EXPECT_EQ(truth.row_transmits_dr(1, 3), std::optional<bool>(false));
  EXPECT_EQ(truth.secam_sequence_frame(0), std::optional<int>(1));
  EXPECT_EQ(truth.secam_sequence_frame(1), std::optional<int>(2));
  EXPECT_FALSE(truth.row_transmits_dr(2, 0).has_value());
}

TEST(VideosynthPictureTruth, givesNoSecamRowsForPal) {
  const auto g = orc_videosynth::truth_geometry(meta_row());
  ASSERT_TRUE(g.has_value());
  const auto truth = TruthFile::from_bytes(*g, truth_bytes());
  ASSERT_TRUE(truth.has_value());
  EXPECT_FALSE(truth->row_transmits_dr(0, 0).has_value());
  EXPECT_FALSE(truth->secam_sequence_frame(0).has_value());
}

TEST(VideosynthPictureTruth, rejectsATruthOfTheWrongSize) {
  const auto g = orc_videosynth::truth_geometry(meta_row());
  ASSERT_TRUE(g.has_value());
  auto raw = truth_bytes();
  raw.pop_back();
  std::string error;
  EXPECT_FALSE(TruthFile::from_bytes(*g, raw, &error).has_value());
  EXPECT_NE(error.find("bytes"), std::string::npos);
}

TEST(VideosynthPictureTruth, placesTheDecodeOnTheTruthGrid) {
  orc_videosynth::TruthGeometry g;
  g.samples_per_line = 3;
  g.rows = 2;
  g.first_sample = 10;
  // A 4 x 2 decode starting at sample 9: truth column x is decode column
  // x + 1. Y' code 16 + 219 * 0.5, Cb/Cr at zero except one sample.
  const uint16_t half = static_cast<uint16_t>((16.0 + (219.0 * 0.5)) * 256.0);
  const uint16_t zero = 128 * 256;
  std::vector<uint16_t> samples(3 * 8, zero);
  for (size_t n = 0; n < 8; ++n) samples[n] = half;
  samples[8 + 1] = zero + (112 * 256);  // Cb at decode (1, 0): full scale
  const DecodedFile decoded(samples, 1);
  std::vector<uint8_t> covered;
  const auto picture =
      orc_videosynth::decoded_picture(decoded, 0, 4, {9, 0}, g, &covered);
  EXPECT_DOUBLE_EQ(picture.y[0], 0.5);
  // Cb full scale is (B'-Y') = 1 - Kb, U = kB (1 - Kb).
  EXPECT_NEAR(picture.u[0], 0.49211104 * 0.886, 1e-6);
  EXPECT_EQ(picture.u[1], 0.0);
  for (const uint8_t c : covered) EXPECT_EQ(c, 1);

  // Starting at sample 11 the decode misses truth column 0.
  const auto shifted =
      orc_videosynth::decoded_picture(decoded, 0, 4, {11, 0}, g, &covered);
  EXPECT_EQ(covered[0], 0);
  EXPECT_EQ(covered[1], 1);
  EXPECT_EQ(shifted.y[0], 0.0);
}

}  // namespace
