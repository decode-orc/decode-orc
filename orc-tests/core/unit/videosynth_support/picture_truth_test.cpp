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
#include <sqlite3.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

using orc_videosynth::DecodedFile;
using orc_videosynth::TruthFile;

constexpr size_t kSamples = 4;
constexpr size_t kRows = 4;

// A one-frame PAL truth of 4 x 4 samples from sample 177: black 0 mV, white
// 700 mV. Field 2 (the lower field) opens on line 622, so the half-picture
// line 623 is its second row, frame row 3, and its picture ends at sample
// 179, two samples in.
fs::path write_truth(const std::string& name, const std::string& format) {
  const fs::path dir = fs::temp_directory_path() / "orc_picture_truth_test";
  fs::create_directories(dir);
  const fs::path base = dir / name;
  std::vector<float> samples(3 * kSamples * kRows);
  for (size_t n = 0; n < kSamples * kRows; ++n) {
    samples[n] = 350.0F;                           // Y: half way
    samples[(kSamples * kRows) + n] = 70.0F;       // U
    samples[(2 * kSamples * kRows) + n] = -35.0F;  // V
  }
  std::ofstream out(base.string() + ".truth", std::ios::binary);
  for (const float s : samples) {
    uint32_t word = 0;
    std::memcpy(&word, &s, sizeof(word));
    const char bytes[4] = {static_cast<char>(word & 0xFF),
                           static_cast<char>((word >> 8) & 0xFF),
                           static_cast<char>((word >> 16) & 0xFF),
                           static_cast<char>((word >> 24) & 0xFF)};
    out.write(bytes, 4);
  }
  out.close();

  const std::string meta = base.string() + ".truth.meta";
  fs::remove(meta);
  sqlite3* db = nullptr;
  EXPECT_EQ(sqlite3_open(meta.c_str(), &db), SQLITE_OK);
  const std::string sql =
      "CREATE TABLE truth_file (truth_file_id INTEGER, video_standard TEXT, "
      "sample_rate_hz REAL, frame_count INTEGER, samples_per_line INTEGER, "
      "rows INTEGER, first_sample INTEGER, first_line_field1 INTEGER, "
      "first_line_field2 INTEGER, upper_field INTEGER, half_picture_line "
      "INTEGER, half_picture_end_sample INTEGER, plane_order TEXT, "
      "sample_format TEXT, units TEXT, y_reference TEXT, black_mv REAL, "
      "white_mv REAL, u_axis TEXT, v_axis TEXT, v_switch TEXT);"
      "INSERT INTO truth_file VALUES (1, 'PAL', 17734475.0, 1, 4, 4, 177, "
      "22, 622, 1, 623, 179, 'YUV', '" +
      format + "', 'mV', 'blanking', 0.0, 700.0, 'sin', 'cos', 'removed');";
  EXPECT_EQ(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, nullptr),
            SQLITE_OK);
  sqlite3_close(db);
  return base;
}

TEST(VideosynthPictureTruth, readsGeometryAndScalesToExcursion) {
  std::string error;
  const auto truth = TruthFile::read(write_truth("good", "float32le"), &error);
  ASSERT_TRUE(truth.has_value()) << error;
  const auto& g = truth->geometry();
  EXPECT_EQ(g.system, orc::VideoSystem::PAL);
  EXPECT_EQ(g.samples_per_line, kSamples);
  EXPECT_EQ(g.rows, kRows);
  const auto frame = truth->frame(0);
  EXPECT_DOUBLE_EQ(frame.y[5], 0.5);
  EXPECT_DOUBLE_EQ(frame.u[5], 0.1);
  EXPECT_DOUBLE_EQ(frame.v[5], -0.05);
}

TEST(VideosynthPictureTruth, masksTheHalfPictureLineTail) {
  const auto truth = TruthFile::read(write_truth("half", "float32le"));
  ASSERT_TRUE(truth.has_value());
  const auto mask = truth->picture_mask();
  for (size_t n = 0; n < mask.size(); ++n) {
    const bool tail = n >= (3 * kSamples) + 2;
    EXPECT_EQ(mask[n], tail ? 0 : 1) << "sample " << n;
  }
}

TEST(VideosynthPictureTruth, rejectsAnotherSampleFormat) {
  std::string error;
  EXPECT_FALSE(
      TruthFile::read(write_truth("int16", "int16le"), &error).has_value());
  EXPECT_NE(error.find("format"), std::string::npos);
}

TEST(VideosynthPictureTruth, rejectsAMissingFile) {
  EXPECT_FALSE(TruthFile::read(fs::temp_directory_path() / "orc_no_such_truth")
                   .has_value());
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
