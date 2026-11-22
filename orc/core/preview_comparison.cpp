/*
 * File:        preview_comparison.cpp
 * Module:      orc-core
 * Purpose:     A/B comparison imaging on preview images (stretch,
 *              difference, automatic crop, export frame assembly)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview_comparison.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace orc {

namespace {

constexpr size_t kChannels = 3;

std::string size_text(const PreviewImage& image) {
  return std::to_string(image.width) + "x" + std::to_string(image.height);
}

void require_compatible(const PreviewImage& a, const PreviewImage& b) {
  const PreviewComparisonCheck check = check_comparison_compatible(a, b);
  if (!check.compatible) {
    throw std::invalid_argument(check.message);
  }
}

void require_valid(const PreviewImage& image, const char* what) {
  if (!image.is_valid()) {
    throw std::invalid_argument(std::string(what) + ": invalid image");
  }
}

PreviewImage blank_like(uint32_t width, uint32_t height) {
  PreviewImage out;
  out.width = width;
  out.height = height;
  out.rgb_data.assign(static_cast<size_t>(width) * height * kChannels, 0);
  return out;
}

uint8_t clamp_round(double value) {
  return static_cast<uint8_t>(std::clamp(std::lround(value), 0L, 255L));
}

uint32_t clamp_index(int64_t value, uint32_t size) {
  return static_cast<uint32_t>(
      std::clamp<int64_t>(value, 0, static_cast<int64_t>(size) - 1));
}

/**
 * Running median of one channel over a square window, kept as a 256-bin
 * histogram with the current median and the count of values below it
 * (Huang, Yang and Tang's method). Moving the median after an update costs a
 * few bin steps because the window changes by one column at a time.
 */
class SlidingMedian {
 public:
  explicit SlidingMedian(uint32_t count) : half_(count / 2) {}

  void reset() {
    histogram_.fill(0);
    median_ = 0;
    below_ = 0;
  }

  void add(uint8_t value) {
    ++histogram_[value];
    if (value < median_) {
      ++below_;
    }
  }

  void remove(uint8_t value) {
    --histogram_[value];
    if (value < median_) {
      --below_;
    }
  }

  // The median is the value m with below(m) <= half < below(m) + hist[m].
  uint8_t median() {
    while (below_ > half_) {
      --median_;
      below_ -= histogram_[median_];
    }
    while (below_ + histogram_[median_] <= half_) {
      below_ += histogram_[median_];
      ++median_;
    }
    return static_cast<uint8_t>(median_);
  }

 private:
  std::array<uint32_t, 256> histogram_{};
  uint32_t half_;
  uint32_t median_ = 0;
  uint32_t below_ = 0;
};

// ITU-R BT.601-7, Section 2.5.1: E'Y = 0.299 E'R + 0.587 E'G + 0.114 E'B.
// Kept in thousandths so the summed-area table stays in integers.
int64_t luma_thousandths(const uint8_t* rgb) {
  return 299 * int64_t{rgb[0]} + 587 * int64_t{rgb[1]} + 114 * int64_t{rgb[2]};
}

// 5 x 7 bitmaps, one row per byte, most significant of the low five bits on
// the left.
constexpr uint32_t kGlyphWidth = 5;
constexpr uint32_t kGlyphHeight = 7;
constexpr std::array<uint8_t, kGlyphHeight> kGlyphA = {
    0b01110, 0b10001, 0b10001, 0b11111, 0b10001, 0b10001, 0b10001};
constexpr std::array<uint8_t, kGlyphHeight> kGlyphB = {
    0b11110, 0b10001, 0b10001, 0b11110, 0b10001, 0b10001, 0b11110};

}  // namespace

PreviewComparisonCheck check_comparison_compatible(const PreviewImage& a,
                                                   const PreviewImage& b) {
  if (!a.is_valid()) {
    return {false, "Image A is empty or invalid"};
  }
  if (!b.is_valid()) {
    return {false, "Image B is empty or invalid"};
  }
  if (a.width != b.width || a.height != b.height) {
    return {false, "Image sizes differ: A is " + size_text(a) + ", B is " +
                       size_text(b)};
  }
  return {true, {}};
}

PreviewImage stretch_about_local_median(const PreviewImage& reference,
                                        const PreviewImage& image,
                                        uint32_t window, double gain) {
  require_compatible(reference, image);
  if (window == 0 || window % 2 == 0) {
    throw std::invalid_argument("Stretch window must be odd, got " +
                                std::to_string(window));
  }

  const uint32_t width = image.width;
  const uint32_t height = image.height;
  const int64_t radius = window / 2;
  PreviewImage out = blank_like(width, height);

  const auto ref_at = [&](uint32_t x, uint32_t y, size_t c) {
    return reference
        .rgb_data[(static_cast<size_t>(y) * width + x) * kChannels + c];
  };

  std::array<SlidingMedian, kChannels> medians = {
      SlidingMedian(window * window), SlidingMedian(window * window),
      SlidingMedian(window * window)};
  std::vector<uint32_t> rows(window);

  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t k = 0; k < window; ++k) {
      rows[k] = clamp_index(static_cast<int64_t>(y) - radius + k, height);
    }

    // Fill the window centred on x = 0, edge samples repeated.
    for (size_t c = 0; c < kChannels; ++c) {
      medians[c].reset();
      for (int64_t dx = -radius; dx <= radius; ++dx) {
        const uint32_t col = clamp_index(dx, width);
        for (uint32_t row : rows) {
          medians[c].add(ref_at(col, row, c));
        }
      }
    }

    for (uint32_t x = 0; x < width; ++x) {
      if (x > 0) {
        const uint32_t leaving =
            clamp_index(static_cast<int64_t>(x) - radius - 1, width);
        const uint32_t entering =
            clamp_index(static_cast<int64_t>(x) + radius, width);
        for (size_t c = 0; c < kChannels; ++c) {
          for (uint32_t row : rows) {
            medians[c].remove(ref_at(leaving, row, c));
            medians[c].add(ref_at(entering, row, c));
          }
        }
      }

      const size_t offset = (static_cast<size_t>(y) * width + x) * kChannels;
      for (size_t c = 0; c < kChannels; ++c) {
        const double median = medians[c].median();
        out.rgb_data[offset + c] =
            clamp_round(median + gain * (image.rgb_data[offset + c] - median));
      }
    }
  }
  return out;
}

PreviewImage difference_image(const PreviewImage& a, const PreviewImage& b,
                              double gain) {
  require_compatible(a, b);
  PreviewImage out = blank_like(a.width, a.height);
  for (size_t i = 0; i < a.rgb_data.size(); ++i) {
    const double delta = static_cast<double>(b.rgb_data[i]) - a.rgb_data[i];
    out.rgb_data[i] = clamp_round(128.0 + gain * delta);
  }
  return out;
}

PreviewCropRect largest_difference_window(const PreviewImage& a,
                                          const PreviewImage& b, uint32_t width,
                                          uint32_t height) {
  require_compatible(a, b);
  if (width == 0 || height == 0) {
    throw std::invalid_argument("Crop window must not be empty");
  }

  const uint32_t image_width = a.width;
  const uint32_t image_height = a.height;
  const uint32_t win_w = std::min(width, image_width);
  const uint32_t win_h = std::min(height, image_height);

  // sat[(y + 1) * stride + (x + 1)] = sum of |dY| over [0, x] x [0, y].
  const size_t stride = static_cast<size_t>(image_width) + 1;
  std::vector<uint64_t> sat(stride * (static_cast<size_t>(image_height) + 1),
                            0);
  for (uint32_t y = 0; y < image_height; ++y) {
    uint64_t row_sum = 0;
    for (uint32_t x = 0; x < image_width; ++x) {
      const size_t offset =
          (static_cast<size_t>(y) * image_width + x) * kChannels;
      const int64_t delta = luma_thousandths(&b.rgb_data[offset]) -
                            luma_thousandths(&a.rgb_data[offset]);
      row_sum += static_cast<uint64_t>(delta < 0 ? -delta : delta);
      sat[(y + 1) * stride + (x + 1)] = sat[y * stride + (x + 1)] + row_sum;
    }
  }

  PreviewCropRect best{0, 0, win_w, win_h};
  uint64_t best_score = 0;
  bool have_best = false;
  for (uint32_t y = 0; y + win_h <= image_height; ++y) {
    for (uint32_t x = 0; x + win_w <= image_width; ++x) {
      const size_t top = static_cast<size_t>(y) * stride;
      const size_t bottom = static_cast<size_t>(y + win_h) * stride;
      const uint64_t score = sat[bottom + x + win_w] - sat[top + x + win_w] -
                             sat[bottom + x] + sat[top + x];
      // Strictly greater keeps the first window met in row-major order,
      // which is the topmost, then leftmost.
      if (!have_best || score > best_score) {
        best_score = score;
        best.x = x;
        best.y = y;
        have_best = true;
      }
    }
  }
  return best;
}

PreviewImage crop(const PreviewImage& image, const PreviewCropRect& rect) {
  require_valid(image, "crop");
  if (rect.width == 0 || rect.height == 0) {
    throw std::invalid_argument("Crop rectangle must not be empty");
  }
  if (static_cast<uint64_t>(rect.x) + rect.width > image.width ||
      static_cast<uint64_t>(rect.y) + rect.height > image.height) {
    throw std::invalid_argument("Crop rectangle " + std::to_string(rect.width) +
                                "x" + std::to_string(rect.height) + " at " +
                                std::to_string(rect.x) + "," +
                                std::to_string(rect.y) + " does not fit a " +
                                size_text(image) + " image");
  }

  PreviewImage out = blank_like(rect.width, rect.height);
  const size_t row_bytes = static_cast<size_t>(rect.width) * kChannels;
  for (uint32_t y = 0; y < rect.height; ++y) {
    const size_t src =
        (static_cast<size_t>(rect.y + y) * image.width + rect.x) * kChannels;
    std::copy_n(
        image.rgb_data.begin() + static_cast<std::ptrdiff_t>(src), row_bytes,
        out.rgb_data.begin() + static_cast<std::ptrdiff_t>(y * row_bytes));
  }
  return out;
}

PreviewImage enlarge_nearest(const PreviewImage& image, uint32_t factor) {
  require_valid(image, "enlarge");
  if (factor < 1 || factor > kMaxEnlargeFactor) {
    throw std::invalid_argument("Enlarge factor must be 1 to " +
                                std::to_string(kMaxEnlargeFactor) + ", got " +
                                std::to_string(factor));
  }
  if (factor == 1) {
    PreviewImage out = blank_like(image.width, image.height);
    out.rgb_data = image.rgb_data;
    return out;
  }

  PreviewImage out = blank_like(image.width * factor, image.height * factor);
  for (uint32_t y = 0; y < out.height; ++y) {
    const uint32_t src_y = y / factor;
    for (uint32_t x = 0; x < out.width; ++x) {
      const size_t src =
          (static_cast<size_t>(src_y) * image.width + x / factor) * kChannels;
      const size_t dst = (static_cast<size_t>(y) * out.width + x) * kChannels;
      for (size_t c = 0; c < kChannels; ++c) {
        out.rgb_data[dst + c] = image.rgb_data[src + c];
      }
    }
  }
  return out;
}

PreviewImage compose_side_by_side(const PreviewImage& left,
                                  const PreviewImage& right, uint32_t gap) {
  require_valid(left, "compose (left)");
  require_valid(right, "compose (right)");
  if (left.height != right.height) {
    throw std::invalid_argument("Side-by-side heights differ: left is " +
                                size_text(left) + ", right is " +
                                size_text(right));
  }

  PreviewImage out = blank_like(left.width + gap + right.width, left.height);
  std::fill(out.rgb_data.begin(), out.rgb_data.end(), kComparisonGapGrey);
  const size_t out_row = static_cast<size_t>(out.width) * kChannels;
  const size_t left_row = static_cast<size_t>(left.width) * kChannels;
  const size_t right_row = static_cast<size_t>(right.width) * kChannels;
  const size_t right_start = static_cast<size_t>(left.width + gap) * kChannels;
  for (uint32_t y = 0; y < out.height; ++y) {
    auto row = out.rgb_data.begin() + static_cast<std::ptrdiff_t>(y * out_row);
    std::copy_n(
        left.rgb_data.begin() + static_cast<std::ptrdiff_t>(y * left_row),
        left_row, row);
    std::copy_n(
        right.rgb_data.begin() + static_cast<std::ptrdiff_t>(y * right_row),
        right_row, row + static_cast<std::ptrdiff_t>(right_start));
  }
  return out;
}

void draw_comparison_label(PreviewImage& image, ComparisonLabel label,
                           uint32_t scale) {
  require_valid(image, "label");
  if (scale == 0) {
    throw std::invalid_argument("Label scale must be at least 1");
  }

  const auto& glyph = label == ComparisonLabel::A ? kGlyphA : kGlyphB;
  // One cell of black border on every side of the glyph.
  const uint32_t box_cells_w = kGlyphWidth + 2;
  const uint32_t box_cells_h = kGlyphHeight + 2;
  const uint32_t box_w = std::min(box_cells_w * scale, image.width);
  const uint32_t box_h = std::min(box_cells_h * scale, image.height);

  for (uint32_t y = 0; y < box_h; ++y) {
    const uint32_t cell_y = y / scale;
    for (uint32_t x = 0; x < box_w; ++x) {
      const uint32_t cell_x = x / scale;
      bool lit = false;
      if (cell_y >= 1 && cell_y <= kGlyphHeight && cell_x >= 1 &&
          cell_x <= kGlyphWidth) {
        const uint8_t bits = glyph[cell_y - 1];
        lit = (bits >> (kGlyphWidth - cell_x)) & 1U;
      }
      const uint8_t value = lit ? 255 : 0;
      const size_t dst = (static_cast<size_t>(y) * image.width + x) * kChannels;
      for (size_t c = 0; c < kChannels; ++c) {
        image.rgb_data[dst + c] = value;
      }
    }
  }
}

PreviewComparisonFrames build_comparison_frames(
    const PreviewImage& a, const PreviewImage& b,
    const PreviewComparisonSettings& settings) {
  require_compatible(a, b);
  if (settings.enlarge < 1 || settings.enlarge > kMaxEnlargeFactor) {
    throw std::invalid_argument("Enlarge factor must be 1 to " +
                                std::to_string(kMaxEnlargeFactor) + ", got " +
                                std::to_string(settings.enlarge));
  }

  PreviewComparisonFrames frames;
  switch (settings.crop_mode) {
    case ComparisonCropMode::None:
      frames.crop = {0, 0, a.width, a.height};
      break;
    case ComparisonCropMode::Explicit:
      frames.crop = settings.crop_rect;
      break;
    case ComparisonCropMode::Auto:
      frames.crop = largest_difference_window(a, b, settings.auto_crop_width,
                                              settings.auto_crop_height);
      break;
  }

  const auto build = [&](const PreviewImage& image, ComparisonLabel label) {
    PreviewImage normal =
        enlarge_nearest(crop(image, frames.crop), settings.enlarge);
    draw_comparison_label(normal, label, settings.enlarge);
    if (!settings.stretch) {
      return normal;
    }
    // The median always comes from A, whichever frame is being built.
    PreviewImage stretched = enlarge_nearest(
        crop(stretch_about_local_median(a, image, settings.stretch_window,
                                        settings.stretch_gain),
             frames.crop),
        settings.enlarge);
    draw_comparison_label(stretched, label, settings.enlarge);
    return compose_side_by_side(normal, stretched, settings.gap);
  };

  frames.frame_a = build(a, ComparisonLabel::A);
  frames.frame_b = build(b, ComparisonLabel::B);
  return frames;
}

}  // namespace orc
