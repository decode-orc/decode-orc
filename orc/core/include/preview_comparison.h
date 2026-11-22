/*
 * File:        preview_comparison.h
 * Module:      orc-core
 * Purpose:     A/B comparison imaging on preview images (stretch,
 *              difference, automatic crop, export frame assembly)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>
#include <orc/stage/node_id.h>
#include <orc/stage/preview/orc_rendering.h>

#include <cstdint>
#include <string>

namespace orc {

/// Side length of the square window the local median is taken over.
inline constexpr uint32_t kDefaultStretchWindow = 15;
/// How many times a sample's distance from the local median is amplified.
inline constexpr double kDefaultStretchGain = 4.0;
/// How many times B − A is amplified about mid grey.
inline constexpr double kDefaultDifferenceGain = 4.0;
/// Size of the automatic crop window, in native samples.
inline constexpr uint32_t kDefaultAutoCropWidth = 320;
inline constexpr uint32_t kDefaultAutoCropHeight = 240;
/// Largest nearest-neighbour enlargement factor.
inline constexpr uint32_t kMaxEnlargeFactor = 8;
/// Grey level of the gap between the halves of an export frame.
inline constexpr uint8_t kComparisonGapGrey = 128;
/// Width of that gap, in output pixels (not scaled by the enlarge factor).
inline constexpr uint32_t kDefaultComparisonGap = 8;

/**
 * @brief The two renders a comparison is made from.
 *
 * Both nodes are rendered with the same output type, option and index. The
 * index is each node's own: no mapping between the two paths is attempted.
 */
struct PreviewComparisonRequest {
  NodeID node_a;
  NodeID node_b;
  PreviewOutputType output_type = PreviewOutputType::Frame_Field1_First;
  std::string option_id;
  uint64_t index = 0;
};

/// Result of checking that two images can be compared sample for sample.
struct PreviewComparisonCheck {
  bool compatible = false;
  std::string message;  ///< Why not, when compatible is false
};

/// A rectangle on an image's sample grid.
struct PreviewCropRect {
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;

  bool operator==(const PreviewCropRect& other) const {
    return x == other.x && y == other.y && width == other.width &&
           height == other.height;
  }
};

/// Which label is burned into a half of an export frame.
enum class ComparisonLabel { A, B };

/**
 * @brief Check that two images can be compared sample for sample.
 *
 * Refuses an invalid (empty or mis-sized) image on either side and images
 * whose width or height differ; the message gives both sizes.
 */
PreviewComparisonCheck check_comparison_compatible(const PreviewImage& a,
                                                   const PreviewImage& b);

/**
 * @brief Contrast stretch about the reference's local median.
 *
 * Per RGB channel, out = clamp(m + gain · (x − m)), rounded to nearest, where
 * m is the median of @p reference over a window × window square centred on
 * the sample. Samples outside the image take the nearest edge sample.
 *
 * Taking m from a reference (A) and applying it to both A and B keeps a level
 * shift between them visible instead of normalising it away.
 *
 * The median uses a sliding 256-bin histogram per channel that moves one
 * column at a time along each row, so the cost is O(window) per sample
 * (plus O(window²) once per row), not O(window²) per sample.
 *
 * @param window Odd, at least 1. A window of 1 is the identity.
 * @throws std::invalid_argument if the images are not compatible or the
 *         window is even or zero.
 */
PreviewImage stretch_about_local_median(const PreviewImage& reference,
                                        const PreviewImage& image,
                                        uint32_t window = kDefaultStretchWindow,
                                        double gain = kDefaultStretchGain);

/**
 * @brief Scaled difference about mid grey.
 *
 * Per channel, out = clamp(128 + gain · (b − a)), rounded to nearest.
 *
 * @throws std::invalid_argument if the images are not compatible.
 */
PreviewImage difference_image(const PreviewImage& a, const PreviewImage& b,
                              double gain = kDefaultDifferenceGain);

/**
 * @brief The window where A and B differ most.
 *
 * Scores each window by the sum of |Y(B) − Y(A)|, with Y the BT.601 luma of
 * each sample. Uses a summed-area table, so the cost is O(samples) whatever
 * the window size. Ties go to the topmost, then leftmost, window; identical
 * images therefore give the window at (0, 0). A window larger than the image
 * is clamped to the image.
 *
 * @throws std::invalid_argument if the images are not compatible or a window
 *         dimension is zero.
 */
PreviewCropRect largest_difference_window(
    const PreviewImage& a, const PreviewImage& b,
    uint32_t width = kDefaultAutoCropWidth,
    uint32_t height = kDefaultAutoCropHeight);

/**
 * @brief Copy a rectangle out of an image.
 *
 * @throws std::invalid_argument if the image is invalid, the rectangle is
 *         empty or it does not lie inside the image.
 */
PreviewImage crop(const PreviewImage& image, const PreviewCropRect& rect);

/**
 * @brief Enlarge by an integer factor, each sample becoming a factor × factor
 *        block (nearest neighbour; no smoothing).
 *
 * @throws std::invalid_argument if the image is invalid or the factor is
 *         outside 1 – kMaxEnlargeFactor.
 */
PreviewImage enlarge_nearest(const PreviewImage& image, uint32_t factor);

/**
 * @brief Place two images side by side with a neutral grey gap between them.
 *
 * @throws std::invalid_argument if either image is invalid or their heights
 *         differ.
 */
PreviewImage compose_side_by_side(const PreviewImage& left,
                                  const PreviewImage& right,
                                  uint32_t gap = kDefaultComparisonGap);

/**
 * @brief Burn an "A" or "B" label into the image's top-left corner.
 *
 * The glyph is a built-in 5 × 7 bitmap drawn white on a black box with a
 * one-cell border, each cell @p scale × @p scale pixels. The label is clipped
 * to the image if the image is smaller than the box.
 *
 * @throws std::invalid_argument if the image is invalid or scale is zero.
 */
void draw_comparison_label(PreviewImage& image, ComparisonLabel label,
                           uint32_t scale);

/// How the export crop is chosen.
enum class ComparisonCropMode {
  None,      ///< The whole image
  Explicit,  ///< PreviewComparisonSettings::crop_rect
  Auto       ///< largest_difference_window at the auto crop size
};

/// Settings for build_comparison_frames.
struct PreviewComparisonSettings {
  ComparisonCropMode crop_mode = ComparisonCropMode::None;
  PreviewCropRect crop_rect;  ///< Used when crop_mode is Explicit
  uint32_t auto_crop_width = kDefaultAutoCropWidth;
  uint32_t auto_crop_height = kDefaultAutoCropHeight;

  bool stretch = true;  ///< Add a stretched half beside the normal one
  uint32_t stretch_window = kDefaultStretchWindow;
  double stretch_gain = kDefaultStretchGain;

  uint32_t enlarge = 1;  ///< Nearest-neighbour factor, 1 – kMaxEnlargeFactor
  uint32_t gap = kDefaultComparisonGap;
};

/// The two animation frames of an export, and the crop they were cut from.
struct PreviewComparisonFrames {
  PreviewImage frame_a;
  PreviewImage frame_b;
  PreviewCropRect crop;  ///< In native samples, so an auto crop can be repeated
};

/**
 * @brief Assemble the two frames of an A/B export.
 *
 * Each frame is the cropped image, optionally with its stretched copy beside
 * it, each half enlarged and labelled. The stretch is computed on the whole
 * image and then cropped, so the crop edges show the same values as an
 * uncropped stretch would. A and B always share one crop.
 *
 * @throws std::invalid_argument if the images are not compatible or a setting
 *         is out of range.
 */
PreviewComparisonFrames build_comparison_frames(
    const PreviewImage& a, const PreviewImage& b,
    const PreviewComparisonSettings& settings);

}  // namespace orc
