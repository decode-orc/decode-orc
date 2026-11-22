/*
 * File:        preview_comparison_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for A/B comparison imaging on preview images
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <preview_comparison.h>

#include <array>
#include <cstdint>
#include <stdexcept>

namespace orc_unit_test {

namespace {

using orc::PreviewCropRect;
using orc::PreviewImage;

PreviewImage flat_image(uint32_t width, uint32_t height, uint8_t value) {
  PreviewImage image;
  image.width = width;
  image.height = height;
  image.rgb_data.assign(static_cast<size_t>(width) * height * 3, value);
  return image;
}

void set_pixel(PreviewImage& image, uint32_t x, uint32_t y, uint8_t value) {
  const size_t offset = (static_cast<size_t>(y) * image.width + x) * 3;
  image.rgb_data[offset] = value;
  image.rgb_data[offset + 1] = value;
  image.rgb_data[offset + 2] = value;
}

void fill_rect(PreviewImage& image, const PreviewCropRect& rect,
               uint8_t value) {
  for (uint32_t y = rect.y; y < rect.y + rect.height; ++y) {
    for (uint32_t x = rect.x; x < rect.x + rect.width; ++x) {
      set_pixel(image, x, y, value);
    }
  }
}

std::array<uint8_t, 3> pixel(const PreviewImage& image, uint32_t x,
                             uint32_t y) {
  const size_t offset = (static_cast<size_t>(y) * image.width + x) * 3;
  return {image.rgb_data[offset], image.rgb_data[offset + 1],
          image.rgb_data[offset + 2]};
}

uint8_t grey(const PreviewImage& image, uint32_t x, uint32_t y) {
  return pixel(image, x, y)[0];
}

}  // namespace

// ---------------------------------------------------------------------------
// Compatibility check
// ---------------------------------------------------------------------------

TEST(PreviewComparisonTest, Compatible_AcceptsEqualSizes) {
  const auto check = orc::check_comparison_compatible(flat_image(8, 6, 0),
                                                      flat_image(8, 6, 255));
  EXPECT_TRUE(check.compatible);
  EXPECT_TRUE(check.message.empty());
}

TEST(PreviewComparisonTest, Compatible_RefusesUnequalWidth_NamingBothSizes) {
  const auto check = orc::check_comparison_compatible(flat_image(8, 6, 0),
                                                      flat_image(9, 6, 0));
  EXPECT_FALSE(check.compatible);
  EXPECT_NE(check.message.find("8x6"), std::string::npos);
  EXPECT_NE(check.message.find("9x6"), std::string::npos);
}

TEST(PreviewComparisonTest, Compatible_RefusesUnequalHeight_NamingBothSizes) {
  const auto check = orc::check_comparison_compatible(flat_image(8, 6, 0),
                                                      flat_image(8, 5, 0));
  EXPECT_FALSE(check.compatible);
  EXPECT_NE(check.message.find("8x6"), std::string::npos);
  EXPECT_NE(check.message.find("8x5"), std::string::npos);
}

TEST(PreviewComparisonTest, Compatible_RefusesInvalidImageOnEitherSide) {
  const PreviewImage empty{};
  EXPECT_FALSE(
      orc::check_comparison_compatible(empty, flat_image(8, 6, 0)).compatible);
  EXPECT_FALSE(
      orc::check_comparison_compatible(flat_image(8, 6, 0), empty).compatible);

  PreviewImage short_data = flat_image(8, 6, 0);
  short_data.rgb_data.pop_back();
  EXPECT_FALSE(orc::check_comparison_compatible(flat_image(8, 6, 0), short_data)
                   .compatible);
}

// ---------------------------------------------------------------------------
// Local-median contrast stretch
// ---------------------------------------------------------------------------

TEST(PreviewComparisonTest, Stretch_LeavesFlatFieldUnchanged) {
  const PreviewImage image = flat_image(40, 30, 77);
  const PreviewImage out = orc::stretch_about_local_median(image, image);
  EXPECT_EQ(out.rgb_data, image.rgb_data);
}

TEST(PreviewComparisonTest, Stretch_AmplifiesHaloBesideStepEdgeByGain) {
  // A 64 -> 192 step at x = 30 with a one-column halo two samples before it,
  // 5 levels (2 % of full scale) above the dark side. Over the 15-sample
  // window centred on the halo, 135 of 225 samples are 64 or 69 and 120 of
  // those are 64, so the median there is 64.
  PreviewImage image = flat_image(60, 20, 64);
  fill_rect(image, {30, 0, 30, 20}, 192);
  fill_rect(image, {28, 0, 1, 20}, 69);

  const PreviewImage out = orc::stretch_about_local_median(image, image);
  for (uint32_t y = 0; y < 20; ++y) {
    EXPECT_EQ(grey(out, 28, y), 64 + 4 * 5) << "row " << y;
    EXPECT_EQ(grey(out, 5, y), 64) << "row " << y;
    EXPECT_EQ(grey(out, 50, y), 192) << "row " << y;
  }
}

TEST(PreviewComparisonTest, Stretch_ClampsAtBlackAndWhite) {
  PreviewImage image = flat_image(40, 40, 128);
  set_pixel(image, 10, 10, 200);  // 128 + 4 * 72 = 416
  set_pixel(image, 30, 30, 60);   // 128 - 4 * 68 = -144

  const PreviewImage out = orc::stretch_about_local_median(image, image);
  EXPECT_EQ(grey(out, 10, 10), 255);
  EXPECT_EQ(grey(out, 30, 30), 0);
  EXPECT_EQ(grey(out, 20, 20), 128);
}

TEST(PreviewComparisonTest, Stretch_TakesMedianFromReference) {
  // A flat at 100, B flat at 103: with the median taken from A, B's level
  // shift is shown amplified rather than normalised away.
  const PreviewImage a = flat_image(32, 24, 100);
  const PreviewImage b = flat_image(32, 24, 103);

  const PreviewImage out_a = orc::stretch_about_local_median(a, a);
  const PreviewImage out_b = orc::stretch_about_local_median(a, b);
  EXPECT_EQ(grey(out_a, 16, 12), 100);
  EXPECT_EQ(grey(out_b, 16, 12), 100 + 4 * 3);
  EXPECT_EQ(grey(out_b, 0, 0), 112);
  EXPECT_EQ(grey(out_b, 31, 23), 112);
}

TEST(PreviewComparisonTest, Stretch_WindowOneIsIdentity) {
  PreviewImage image = flat_image(16, 12, 40);
  set_pixel(image, 3, 4, 250);
  set_pixel(image, 9, 1, 0);
  fill_rect(image, {10, 6, 4, 4}, 180);

  const PreviewImage out = orc::stretch_about_local_median(image, image, 1);
  EXPECT_EQ(out.rgb_data, image.rgb_data);
}

TEST(PreviewComparisonTest, Stretch_StretchesEachChannelOnItsOwn) {
  PreviewImage image = flat_image(30, 30, 0);
  for (size_t i = 0; i < image.rgb_data.size(); i += 3) {
    image.rgb_data[i] = 50;
    image.rgb_data[i + 1] = 100;
    image.rgb_data[i + 2] = 150;
  }
  const size_t centre = (15 * 30 + 15) * 3;
  image.rgb_data[centre + 1] = 102;  // green only

  const PreviewImage out = orc::stretch_about_local_median(image, image);
  const auto rgb = pixel(out, 15, 15);
  EXPECT_EQ(rgb[0], 50);
  EXPECT_EQ(rgb[1], 108);
  EXPECT_EQ(rgb[2], 150);
}

TEST(PreviewComparisonTest, Stretch_RefusesEvenOrZeroWindow) {
  const PreviewImage image = flat_image(8, 8, 0);
  EXPECT_THROW(orc::stretch_about_local_median(image, image, 0),
               std::invalid_argument);
  EXPECT_THROW(orc::stretch_about_local_median(image, image, 4),
               std::invalid_argument);
}

TEST(PreviewComparisonTest, Stretch_RefusesIncompatibleImages) {
  EXPECT_THROW(
      orc::stretch_about_local_median(flat_image(8, 8, 0), flat_image(8, 9, 0)),
      std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Scaled difference
// ---------------------------------------------------------------------------

TEST(PreviewComparisonTest, Difference_IdenticalInputsGiveFlatMidGrey) {
  PreviewImage image = flat_image(10, 10, 0);
  set_pixel(image, 4, 4, 200);
  const PreviewImage out = orc::difference_image(image, image);
  EXPECT_EQ(out.rgb_data, flat_image(10, 10, 128).rgb_data);
}

TEST(PreviewComparisonTest, Difference_ShowsPositiveAndNegativeAboutMidGrey) {
  const PreviewImage a = flat_image(4, 4, 100);
  PreviewImage b = flat_image(4, 4, 100);
  set_pixel(b, 1, 1, 110);  // +10 -> 128 + 40
  set_pixel(b, 2, 2, 95);   // -5  -> 128 - 20

  const PreviewImage out = orc::difference_image(a, b);
  EXPECT_EQ(grey(out, 1, 1), 168);
  EXPECT_EQ(grey(out, 2, 2), 108);
  EXPECT_EQ(grey(out, 0, 0), 128);
}

TEST(PreviewComparisonTest, Difference_Saturates) {
  const PreviewImage a = flat_image(4, 4, 100);
  PreviewImage b = flat_image(4, 4, 100);
  set_pixel(b, 0, 0, 200);
  set_pixel(b, 3, 3, 0);

  const PreviewImage out = orc::difference_image(a, b);
  EXPECT_EQ(grey(out, 0, 0), 255);
  EXPECT_EQ(grey(out, 3, 3), 0);
}

// ---------------------------------------------------------------------------
// Automatic crop
// ---------------------------------------------------------------------------

TEST(PreviewComparisonTest, AutoCrop_WindowContainsPlantedDifference) {
  const PreviewImage a = flat_image(200, 100, 80);
  PreviewImage b = a;
  fill_rect(b, {150, 60, 5, 5}, 120);

  const PreviewCropRect window = orc::largest_difference_window(a, b, 20, 20);
  EXPECT_EQ(window.width, 20u);
  EXPECT_EQ(window.height, 20u);
  EXPECT_LE(window.x, 150u);
  EXPECT_GE(window.x + window.width, 155u);
  EXPECT_LE(window.y, 60u);
  EXPECT_GE(window.y + window.height, 65u);
}

TEST(PreviewComparisonTest, AutoCrop_TiesGoToTopmostThenLeftmost) {
  const PreviewImage a = flat_image(100, 50, 80);

  // Same row: the leftmost window that holds either difference wins.
  PreviewImage same_row = a;
  set_pixel(same_row, 20, 5, 120);
  set_pixel(same_row, 70, 5, 120);
  EXPECT_EQ(orc::largest_difference_window(a, same_row, 10, 10),
            (PreviewCropRect{11, 0, 10, 10}));

  // Different rows: the higher difference wins although it is further right.
  PreviewImage different_rows = a;
  set_pixel(different_rows, 70, 5, 120);
  set_pixel(different_rows, 20, 30, 120);
  EXPECT_EQ(orc::largest_difference_window(a, different_rows, 10, 10),
            (PreviewCropRect{61, 0, 10, 10}));
}

TEST(PreviewComparisonTest, AutoCrop_IdenticalImagesGiveOrigin) {
  const PreviewImage a = flat_image(64, 48, 10);
  EXPECT_EQ(orc::largest_difference_window(a, a, 16, 12),
            (PreviewCropRect{0, 0, 16, 12}));
}

TEST(PreviewComparisonTest, AutoCrop_ClampsOversizeWindowToImage) {
  const PreviewImage a = flat_image(64, 48, 10);
  PreviewImage b = a;
  set_pixel(b, 60, 40, 200);
  EXPECT_EQ(orc::largest_difference_window(a, b),
            (PreviewCropRect{0, 0, 64, 48}));
  EXPECT_EQ(orc::largest_difference_window(a, b, 100, 10),
            (PreviewCropRect{0, 31, 64, 10}));
}

TEST(PreviewComparisonTest, AutoCrop_WeighsChannelsByLuma) {
  // A blue-only change of 100 counts 11.4 luma levels; a green-only change of
  // 30 counts 17.6, so the green one wins although its raw step is smaller.
  const PreviewImage a = flat_image(100, 20, 100);
  PreviewImage b = a;
  b.rgb_data[(5 * 100 + 10) * 3 + 2] = 200;
  b.rgb_data[(5 * 100 + 80) * 3 + 1] = 130;
  const PreviewCropRect window = orc::largest_difference_window(a, b, 10, 10);
  EXPECT_EQ(window.x, 71u);
}

TEST(PreviewComparisonTest, AutoCrop_RefusesEmptyWindow) {
  const PreviewImage a = flat_image(8, 8, 0);
  EXPECT_THROW(orc::largest_difference_window(a, a, 0, 4),
               std::invalid_argument);
  EXPECT_THROW(orc::largest_difference_window(a, a, 4, 0),
               std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Crop, enlarge and side-by-side layout
// ---------------------------------------------------------------------------

TEST(PreviewComparisonTest, Crop_CopiesExactSamples) {
  PreviewImage image = flat_image(6, 5, 0);
  for (uint32_t y = 0; y < 5; ++y) {
    for (uint32_t x = 0; x < 6; ++x) {
      set_pixel(image, x, y, static_cast<uint8_t>(y * 10 + x));
    }
  }
  const PreviewImage out = orc::crop(image, {2, 1, 3, 2});
  ASSERT_EQ(out.width, 3u);
  ASSERT_EQ(out.height, 2u);
  ASSERT_TRUE(out.is_valid());
  EXPECT_EQ(grey(out, 0, 0), 12);
  EXPECT_EQ(grey(out, 2, 0), 14);
  EXPECT_EQ(grey(out, 0, 1), 22);
  EXPECT_EQ(grey(out, 2, 1), 24);
}

TEST(PreviewComparisonTest, Crop_WholeImageIsIdentity) {
  PreviewImage image = flat_image(6, 5, 3);
  set_pixel(image, 5, 4, 9);
  EXPECT_EQ(orc::crop(image, {0, 0, 6, 5}).rgb_data, image.rgb_data);
}

TEST(PreviewComparisonTest, Crop_RefusesOutOfRangeOrEmptyRectangle) {
  const PreviewImage image = flat_image(6, 5, 0);
  EXPECT_THROW(orc::crop(image, {4, 0, 3, 1}), std::invalid_argument);
  EXPECT_THROW(orc::crop(image, {0, 4, 1, 2}), std::invalid_argument);
  EXPECT_THROW(orc::crop(image, {0, 0, 0, 1}), std::invalid_argument);
  EXPECT_THROW(orc::crop(PreviewImage{}, {0, 0, 1, 1}), std::invalid_argument);
}

TEST(PreviewComparisonTest, Enlarge_FactorOneIsIdentity) {
  PreviewImage image = flat_image(3, 2, 5);
  set_pixel(image, 1, 1, 200);
  const PreviewImage out = orc::enlarge_nearest(image, 1);
  EXPECT_EQ(out.width, 3u);
  EXPECT_EQ(out.height, 2u);
  EXPECT_EQ(out.rgb_data, image.rgb_data);
}

TEST(PreviewComparisonTest, Enlarge_RepeatsEachSampleAsABlock) {
  PreviewImage image = flat_image(2, 2, 0);
  set_pixel(image, 0, 0, 10);
  set_pixel(image, 1, 0, 20);
  set_pixel(image, 0, 1, 30);
  set_pixel(image, 1, 1, 40);

  const PreviewImage out = orc::enlarge_nearest(image, 3);
  ASSERT_EQ(out.width, 6u);
  ASSERT_EQ(out.height, 6u);
  for (uint32_t y = 0; y < 6; ++y) {
    for (uint32_t x = 0; x < 6; ++x) {
      const uint8_t expected =
          static_cast<uint8_t>(10 + 10 * (x / 3) + 20 * (y / 3));
      EXPECT_EQ(grey(out, x, y), expected) << x << "," << y;
    }
  }
}

TEST(PreviewComparisonTest, Enlarge_RefusesOutOfRangeFactor) {
  const PreviewImage image = flat_image(2, 2, 0);
  EXPECT_THROW(orc::enlarge_nearest(image, 0), std::invalid_argument);
  EXPECT_THROW(orc::enlarge_nearest(image, orc::kMaxEnlargeFactor + 1),
               std::invalid_argument);
  EXPECT_NO_THROW(orc::enlarge_nearest(image, orc::kMaxEnlargeFactor));
}

TEST(PreviewComparisonTest, Compose_PlacesHalvesEitherSideOfGreyGap) {
  const PreviewImage left = flat_image(2, 3, 10);
  const PreviewImage right = flat_image(4, 3, 200);

  const PreviewImage out = orc::compose_side_by_side(left, right, 3);
  ASSERT_EQ(out.width, 9u);
  ASSERT_EQ(out.height, 3u);
  for (uint32_t y = 0; y < 3; ++y) {
    for (uint32_t x = 0; x < 9; ++x) {
      const uint8_t expected =
          x < 2 ? 10 : (x < 5 ? orc::kComparisonGapGrey : 200);
      EXPECT_EQ(pixel(out, x, y),
                (std::array<uint8_t, 3>{expected, expected, expected}))
          << x << "," << y;
    }
  }
}

TEST(PreviewComparisonTest, Compose_ZeroGapAbutsHalves) {
  const PreviewImage out =
      orc::compose_side_by_side(flat_image(2, 1, 1), flat_image(1, 1, 2), 0);
  ASSERT_EQ(out.width, 3u);
  EXPECT_EQ(grey(out, 1, 0), 1);
  EXPECT_EQ(grey(out, 2, 0), 2);
}

TEST(PreviewComparisonTest, Compose_RefusesDifferentHeights) {
  EXPECT_THROW(
      orc::compose_side_by_side(flat_image(2, 3, 0), flat_image(2, 4, 0)),
      std::invalid_argument);
}

// ---------------------------------------------------------------------------
// A/B labels
// ---------------------------------------------------------------------------

TEST(PreviewComparisonTest, Label_DrawsGlyphOnBoxAndLeavesRestUnchanged) {
  const PreviewImage original = flat_image(40, 40, 50);
  PreviewImage image = original;
  orc::draw_comparison_label(image, orc::ComparisonLabel::A, 2);

  // The box is (5 + 2) x (7 + 2) cells of 2 x 2 pixels.
  constexpr uint32_t kBoxW = 14;
  constexpr uint32_t kBoxH = 18;
  for (uint32_t y = 0; y < 40; ++y) {
    for (uint32_t x = 0; x < 40; ++x) {
      if (x >= kBoxW || y >= kBoxH) {
        EXPECT_EQ(pixel(image, x, y), pixel(original, x, y)) << x << "," << y;
      } else {
        const uint8_t value = grey(image, x, y);
        EXPECT_TRUE(value == 0 || value == 255) << x << "," << y;
      }
    }
  }

  // Border cells are black.
  EXPECT_EQ(grey(image, 0, 0), 0);
  EXPECT_EQ(grey(image, 13, 17), 0);
  // "A" row 0 is .XXX. : glyph cell (0, 0) dark, (1, 0) lit.
  EXPECT_EQ(grey(image, 2, 2), 0);
  EXPECT_EQ(grey(image, 4, 2), 255);
  EXPECT_EQ(grey(image, 5, 3), 255);
  // "A" row 3 is the crossbar, all lit.
  for (uint32_t cell = 0; cell < 5; ++cell) {
    EXPECT_EQ(grey(image, 2 + cell * 2, 8), 255) << "cell " << cell;
  }
}

TEST(PreviewComparisonTest, Label_BDiffersFromA) {
  PreviewImage a = flat_image(20, 20, 50);
  PreviewImage b = a;
  orc::draw_comparison_label(a, orc::ComparisonLabel::A, 1);
  orc::draw_comparison_label(b, orc::ComparisonLabel::B, 1);
  EXPECT_NE(a.rgb_data, b.rgb_data);
  // "B" row 0 is XXXX. : glyph cell (0, 0) lit, where "A" has it dark.
  EXPECT_EQ(grey(a, 1, 1), 0);
  EXPECT_EQ(grey(b, 1, 1), 255);
}

TEST(PreviewComparisonTest, Label_ClipsToSmallImage) {
  PreviewImage image = flat_image(4, 3, 50);
  EXPECT_NO_THROW(
      orc::draw_comparison_label(image, orc::ComparisonLabel::B, 4));
  EXPECT_TRUE(image.is_valid());
}

// ---------------------------------------------------------------------------
// Export frame assembly
// ---------------------------------------------------------------------------

namespace {

struct ExportPair {
  PreviewImage a;
  PreviewImage b;
};

ExportPair export_pair() {
  ExportPair pair{flat_image(64, 48, 100), flat_image(64, 48, 100)};
  fill_rect(pair.b, {40, 30, 3, 3}, 106);
  return pair;
}

}  // namespace

TEST(PreviewComparisonTest, Frames_HaveEqualSizeWithStretchedHalf) {
  const ExportPair pair = export_pair();
  orc::PreviewComparisonSettings settings;
  settings.crop_mode = orc::ComparisonCropMode::Auto;
  settings.auto_crop_width = 16;
  settings.auto_crop_height = 12;
  settings.enlarge = 2;
  settings.gap = 8;

  const auto frames = orc::build_comparison_frames(pair.a, pair.b, settings);
  EXPECT_EQ(frames.frame_a.width, 16u * 2 * 2 + 8);
  EXPECT_EQ(frames.frame_a.height, 12u * 2);
  EXPECT_EQ(frames.frame_b.width, frames.frame_a.width);
  EXPECT_EQ(frames.frame_b.height, frames.frame_a.height);
  EXPECT_TRUE(frames.frame_a.is_valid());
  EXPECT_TRUE(frames.frame_b.is_valid());
}

TEST(PreviewComparisonTest, Frames_OmitStretchedHalfWhenStretchIsOff) {
  const ExportPair pair = export_pair();
  orc::PreviewComparisonSettings settings;
  settings.stretch = false;
  settings.enlarge = 3;

  const auto frames = orc::build_comparison_frames(pair.a, pair.b, settings);
  EXPECT_EQ(frames.frame_a.width, 64u * 3);
  EXPECT_EQ(frames.frame_a.height, 48u * 3);
  EXPECT_EQ(frames.crop, (PreviewCropRect{0, 0, 64, 48}));
}

TEST(PreviewComparisonTest, Frames_AutoCropIsSharedAndReported) {
  const ExportPair pair = export_pair();
  orc::PreviewComparisonSettings settings;
  settings.crop_mode = orc::ComparisonCropMode::Auto;
  settings.auto_crop_width = 16;
  settings.auto_crop_height = 12;
  settings.stretch = false;

  const auto frames = orc::build_comparison_frames(pair.a, pair.b, settings);
  EXPECT_EQ(frames.crop,
            orc::largest_difference_window(pair.a, pair.b, 16, 12));

  // Both frames are cut from that one window: the planted block sits at the
  // same place in B's frame, and A's frame is flat there.
  const uint32_t bx = 40 - frames.crop.x;
  const uint32_t by = 30 - frames.crop.y;
  EXPECT_EQ(grey(frames.frame_b, bx, by), 106);
  EXPECT_EQ(grey(frames.frame_a, bx, by), 100);
}

TEST(PreviewComparisonTest, Frames_StretchedHalfShowsDifferenceAmplified) {
  const ExportPair pair = export_pair();
  orc::PreviewComparisonSettings settings;
  settings.crop_mode = orc::ComparisonCropMode::Explicit;
  settings.crop_rect = {32, 24, 16, 12};
  settings.gap = 4;

  const auto frames = orc::build_comparison_frames(pair.a, pair.b, settings);
  // The block (40, 30) is at (8, 6) in the crop; the stretched half starts
  // after 16 normal samples and the gap. A's median there is 100, so B's
  // 6-level step reads 24 levels.
  const uint32_t stretched_x = 16 + 4 + 8;
  EXPECT_EQ(grey(frames.frame_a, stretched_x, 6), 100);
  EXPECT_EQ(grey(frames.frame_b, stretched_x, 6), 124);
  EXPECT_EQ(grey(frames.frame_b, 8, 6), 106);
}

TEST(PreviewComparisonTest, Frames_RefuseOutOfRangeSettings) {
  const ExportPair pair = export_pair();
  orc::PreviewComparisonSettings settings;
  settings.enlarge = 0;
  EXPECT_THROW(orc::build_comparison_frames(pair.a, pair.b, settings),
               std::invalid_argument);

  settings.enlarge = 1;
  settings.crop_mode = orc::ComparisonCropMode::Explicit;
  settings.crop_rect = {60, 0, 10, 10};
  EXPECT_THROW(orc::build_comparison_frames(pair.a, pair.b, settings),
               std::invalid_argument);

  EXPECT_THROW(orc::build_comparison_frames(pair.a, flat_image(64, 47, 0),
                                            orc::PreviewComparisonSettings{}),
               std::invalid_argument);
}

}  // namespace orc_unit_test
