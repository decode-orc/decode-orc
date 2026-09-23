/*
 * File:        palcolour_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit test(s) for PalColour decoder core paths
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/sinks/common/decoders/palcolour.h"

#include <gtest/gtest.h>

#include <cmath>

namespace orc_unit_test {
namespace {

orc::SourceParameters make_pal_video_params() {
  orc::SourceParameters p;
  p.system = orc::VideoSystem::PAL;
  p.frame_width_nominal = 64;
  p.active_video_start = 16;
  p.active_video_end = 32;
  p.first_active_frame_line = 0;
  p.last_active_frame_line = 6;
  return p;
}

// Helper owning wrapper: holds sample buffers and the non-owning SourceField.
struct OwnedField {
  std::vector<int16_t> composite_buf;
  std::vector<int16_t> luma_buf;
  std::vector<int16_t> chroma_buf;
  SourceField field;

  static OwnedField makeYC(bool is_first_field, int16_t luma_base,
                           int16_t chroma_base, int width, int height) {
    OwnedField of;
    of.field.is_yc = true;
    of.field.is_first_field = is_first_field;
    of.field.field_phase_id = is_first_field ? 1 : 2;
    of.field.line_count = static_cast<size_t>(height);
    of.field.samples_per_line = static_cast<size_t>(width);

    of.luma_buf.reserve(static_cast<size_t>(width * height));
    of.chroma_buf.reserve(static_cast<size_t>(width * height));
    for (int line = 0; line < height; ++line) {
      for (int x = 0; x < width; ++x) {
        of.luma_buf.push_back(static_cast<int16_t>(luma_base + line * 8 + x));
        of.chroma_buf.push_back(
            static_cast<int16_t>(chroma_base + ((x % 4) * 12)));
      }
    }
    of.field.luma_data = of.luma_buf.data();
    of.field.chroma_data = of.chroma_buf.data();
    return of;
  }

  static OwnedField makeComposite(bool is_first_field, int16_t base, int width,
                                  int height) {
    OwnedField of;
    of.field.is_yc = false;
    of.field.is_first_field = is_first_field;
    of.field.field_phase_id = is_first_field ? 1 : 2;
    of.field.line_count = static_cast<size_t>(height);
    of.field.samples_per_line = static_cast<size_t>(width);

    of.composite_buf.reserve(static_cast<size_t>(width * height));
    for (int line = 0; line < height; ++line) {
      for (int x = 0; x < width; ++x) {
        of.composite_buf.push_back(static_cast<int16_t>(base + line * 5 + x));
      }
    }
    of.field.data = of.composite_buf.data();
    return of;
  }
};

}  // namespace

TEST(PalColourTest, ConfigurationLookAround_MatchesFilterMode) {
  PalColour::Configuration pal2d;
  pal2d.chromaFilter = PalColour::palColourFilter;

  PalColour::Configuration transform3d;
  transform3d.chromaFilter = PalColour::transform3DFilter;

  EXPECT_EQ(pal2d.getLookBehind(), 0);
  EXPECT_EQ(pal2d.getLookAhead(), 0);
  EXPECT_GT(transform3d.getLookBehind(), 0);
  EXPECT_GT(transform3d.getLookAhead(), 0);
}

TEST(PalColourTest, DecodeFramesYcPath_PreservesLumaInActiveRegion) {
  const auto params = make_pal_video_params();

  PalColour::Configuration config;
  config.chromaFilter = PalColour::palColourFilter;
  config.yNRLevel = 0.0;

  PalColour decoder;
  decoder.updateConfiguration(params, config);

  auto first_owned = OwnedField::makeYC(true, 1200, 2200, 64, 4);
  auto second_owned = OwnedField::makeYC(false, 3200, 4200, 64, 4);

  std::vector<SourceField> fields = {first_owned.field, second_owned.field};
  std::vector<ComponentFrame> output(1);

  decoder.decodeFrames(fields, 0, 2, output);

  EXPECT_EQ(output[0].getWidth(), 64);
  EXPECT_EQ(output[0].getHeight(), 625);  // PAL: 313*2-1

  const double* line0 = output[0].y(0);
  const double* line1 = output[0].y(1);

  EXPECT_DOUBLE_EQ(line0[16],
                   static_cast<double>(first_owned.field.luma_data[16]));
  EXPECT_DOUBLE_EQ(line0[24],
                   static_cast<double>(first_owned.field.luma_data[24]));
  EXPECT_DOUBLE_EQ(line1[16],
                   static_cast<double>(second_owned.field.luma_data[16]));
  EXPECT_DOUBLE_EQ(line1[24],
                   static_cast<double>(second_owned.field.luma_data[24]));
}

TEST(PalColourTest, DecodeFramesCompositePath_MatchesGoldenVector) {
  const auto params = make_pal_video_params();

  PalColour::Configuration config;
  config.chromaFilter = PalColour::palColourFilter;
  config.yNRLevel = 0.0;

  PalColour decoder;
  decoder.updateConfiguration(params, config);

  auto first_owned = OwnedField::makeComposite(true, 3000, 64, 4);
  auto second_owned = OwnedField::makeComposite(false, 3100, 64, 4);

  std::vector<SourceField> fields = {first_owned.field, second_owned.field};
  std::vector<ComponentFrame> output(1);

  decoder.decodeFrames(fields, 0, 2, output);

  EXPECT_EQ(output[0].getWidth(), 64);
  EXPECT_EQ(output[0].getHeight(), 625);  // PAL: 313*2-1

  const double* y0 = output[0].y(0);
  const double* u0 = output[0].u(0);
  const double* v0 = output[0].v(0);

  EXPECT_TRUE(std::isfinite(y0[16]));
  EXPECT_TRUE(std::isfinite(y0[24]));
  EXPECT_TRUE(std::isfinite(u0[16]));
  EXPECT_TRUE(std::isfinite(u0[24]));
  EXPECT_TRUE(std::isfinite(v0[16]));
  EXPECT_TRUE(std::isfinite(v0[24]));
}

TEST(PalColourTest, InvalidConfiguration_DoesNotAttemptDecode) {
  auto params = make_pal_video_params();
  params.frame_width_nominal = 8;

  PalColour::Configuration config;
  config.chromaFilter = PalColour::palColourFilter;

  PalColour decoder;
  std::vector<SourceField> fields(2);
  std::vector<ComponentFrame> output(1);

  EXPECT_NO_FATAL_FAILURE({
    decoder.updateConfiguration(params, config);
    decoder.decodeFrames(fields, 0, 2, output);
  });

  EXPECT_EQ(output[0].getWidth(), -1);
  EXPECT_EQ(output[0].getHeight(), -1);
}
}  // namespace orc_unit_test
