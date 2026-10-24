/*
 * File:        gif_decoder_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the GIF decoder the records viewer draws a
 *              datacast's pictures with
 *
 * Covers: a small image decoded to its palette indices and colour table; an
 * interlaced image's rows put back in order; bytes that are not a GIF file
 * refused; a screen too large to hold refused; and a file cut short decoded
 * as far as it goes. The images are
 * built here with the simplest LZW coding a decoder must accept: a clear code
 * before every pixel. Deterministic, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "gif_decoder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <utility>
#include <vector>

namespace orc_unit_test {
namespace {

/// A GIF89a of |width| x |height| with a four-colour local table, its
/// |pixels| (row by row from the top) coded with a clear code before each
/// one, interlaced where asked.
std::vector<uint8_t> gif_of(int width, int height,
                            const std::vector<uint8_t>& pixels,
                            bool interlaced) {
  std::vector<uint8_t> out = {'G', 'I', 'F', '8', '9', 'a'};
  const auto word = [&out](int value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
  };
  word(width);
  word(height);
  out.push_back(0x00);  // no global table
  out.push_back(0x00);  // background index
  out.push_back(0x00);  // aspect
  out.push_back(0x2C);  // image
  word(0);
  word(0);
  word(width);
  word(height);
  out.push_back(static_cast<uint8_t>(0x80 | (interlaced ? 0x40 : 0) | 0x01));
  const uint8_t table[4][3] = {
      {0, 0, 0}, {255, 0, 0}, {0, 255, 0}, {0, 0, 255}};
  for (const auto& entry : table) {
    out.insert(out.end(), entry, entry + 3);
  }
  out.push_back(2);  // minimum code size: codes start 3 bits wide

  std::vector<int> rows;
  if (interlaced) {
    for (const auto [first, step] :
         {std::pair{0, 8}, std::pair{4, 8}, std::pair{2, 4}, std::pair{1, 2}}) {
      for (int y = first; y < height; y += step) {
        rows.push_back(y);
      }
    }
  } else {
    for (int y = 0; y < height; ++y) {
      rows.push_back(y);
    }
  }
  std::vector<uint8_t> coded;
  uint32_t acc = 0;
  int bits = 0;
  const auto put = [&](int code) {
    acc |= static_cast<uint32_t>(code) << bits;
    bits += 3;
    while (bits >= 8) {
      coded.push_back(static_cast<uint8_t>(acc & 0xFF));
      acc >>= 8;
      bits -= 8;
    }
  };
  for (const int y : rows) {
    for (int x = 0; x < width; ++x) {
      put(4);  // clear
      put(pixels[static_cast<size_t>(y * width + x)]);
    }
  }
  put(5);  // end
  if (bits > 0) {
    coded.push_back(static_cast<uint8_t>(acc & 0xFF));
  }
  for (size_t at = 0; at < coded.size(); at += 255) {
    const size_t n = std::min<size_t>(255, coded.size() - at);
    out.push_back(static_cast<uint8_t>(n));
    out.insert(out.end(), coded.begin() + static_cast<ptrdiff_t>(at),
               coded.begin() + static_cast<ptrdiff_t>(at + n));
  }
  out.push_back(0x00);
  out.push_back(0x3B);
  return out;
}

TEST(GifDecoder, DecodesAnImageToItsPaletteIndices) {
  const std::vector<uint8_t> pixels = {0, 1, 2, 3, 3, 2};
  const auto image = orc::decode_gif(gif_of(3, 2, pixels, false));
  ASSERT_TRUE(image.has_value());
  EXPECT_EQ(image->width, 3);
  EXPECT_EQ(image->height, 2);
  EXPECT_EQ(image->pixels, pixels);
  ASSERT_EQ(image->palette.size(), 4u);
  EXPECT_EQ(image->palette[1][0], 255);
  EXPECT_EQ(image->palette[3][2], 255);
  EXPECT_FALSE(image->truncated);
}

// GIF89a Appendix E: an interlaced image sends every eighth row from 0, then
// from 4, every fourth from 2, every second from 1.
TEST(GifDecoder, PutsAnInterlacedImagesRowsBackInOrder) {
  const std::vector<uint8_t> pixels = {0, 1, 2, 3, 0, 1, 2, 3};
  const auto image = orc::decode_gif(gif_of(1, 8, pixels, true));
  ASSERT_TRUE(image.has_value());
  EXPECT_EQ(image->pixels, pixels);
}

TEST(GifDecoder, RefusesBytesThatAreNotAGifFile) {
  EXPECT_FALSE(orc::decode_gif({}).has_value());
  const std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A,
                                    0x0A, 0,   0,   0,   0,    0};
  EXPECT_FALSE(orc::decode_gif(png).has_value());
}

// A misread size field must not cost the memory it names: a header alone can
// claim a screen within kGifMaxSide on each side whose area is far beyond
// any picture a datacast sent.
TEST(GifDecoder, RefusesAScreenLargerThanTheAreaLimit) {
  const std::vector<uint8_t> header = {'G',  'I',  'F',  '8',  '9',  'a', 0x00,
                                       0x10, 0x00, 0x10, 0x00, 0x00, 0x00};
  EXPECT_FALSE(orc::decode_gif(header).has_value());
}

// A file a datacast lost the end of still shows what arrived.
TEST(GifDecoder, DecodesAFileCutShortAsFarAsItGoes) {
  std::vector<uint8_t> pixels(64, 2);
  std::vector<uint8_t> file = gif_of(8, 8, pixels, false);
  file.resize(file.size() - 12);
  const auto image = orc::decode_gif(file);
  ASSERT_TRUE(image.has_value());
  EXPECT_TRUE(image->truncated);
  EXPECT_EQ(image->pixels.front(), 2);
}

}  // namespace
}  // namespace orc_unit_test
