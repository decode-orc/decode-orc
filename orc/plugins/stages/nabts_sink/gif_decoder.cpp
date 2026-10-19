/*
 * File:        gif_decoder.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Decodes the first image of a GIF file into palette indices
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "gif_decoder.h"

#include <algorithm>

namespace orc {

namespace {

// GIF89a: the largest LZW code is 12 bits wide.
constexpr int kMaxCodeBits = 12;
constexpr int kMaxCodes = 1 << kMaxCodeBits;

constexpr uint8_t kExtensionIntroducer = 0x21;
constexpr uint8_t kImageSeparator = 0x2C;
constexpr uint8_t kTrailer = 0x3B;
constexpr uint8_t kGraphicControlLabel = 0xF9;

/// Bytes read from a GIF file, with the position kept and no read past its end.
class Reader {
 public:
  explicit Reader(const std::vector<uint8_t>& data) : data_(data) {}

  bool has(size_t count) const { return pos_ + count <= data_.size(); }
  uint8_t byte() { return has(1) ? data_[pos_++] : 0; }
  int word() {
    const int low = byte();
    return low | (byte() << 8);
  }
  void skip(size_t count) { pos_ = std::min(data_.size(), pos_ + count); }
  size_t pos() const { return pos_; }

  /// The data sub-blocks from here to their terminator, joined.
  std::vector<uint8_t> sub_blocks() {
    std::vector<uint8_t> out;
    while (has(1)) {
      const uint8_t size = byte();
      if (size == 0) {
        break;
      }
      const size_t take = std::min<size_t>(size, data_.size() - pos_);
      out.insert(out.end(), data_.begin() + static_cast<ptrdiff_t>(pos_),
                 data_.begin() + static_cast<ptrdiff_t>(pos_ + take));
      pos_ += take;
    }
    return out;
  }

 private:
  const std::vector<uint8_t>& data_;
  size_t pos_ = 0;
};

std::vector<std::array<uint8_t, 3>> colour_table(Reader& in, int entries) {
  std::vector<std::array<uint8_t, 3>> table(static_cast<size_t>(entries));
  for (auto& entry : table) {
    entry[0] = in.byte();
    entry[1] = in.byte();
    entry[2] = in.byte();
  }
  return table;
}

/// The row of the image the |n|th row of interlaced data is (GIF89a
/// Appendix E): every eighth row from 0, every eighth from 4, every fourth
/// from 2, every second from 1.
int interlaced_row(int n, int height) {
  const int pass1 = (height + 7) / 8;
  if (n < pass1) {
    return n * 8;
  }
  n -= pass1;
  const int pass2 = (height + 3) / 8;
  if (n < pass2) {
    return 4 + n * 8;
  }
  n -= pass2;
  const int pass3 = (height + 1) / 4;
  if (n < pass3) {
    return 2 + n * 4;
  }
  n -= pass3;
  return 1 + n * 2;
}

}  // namespace

std::optional<GifImage> decode_gif(const std::vector<uint8_t>& data) {
  Reader in(data);
  if (!in.has(13) || !(std::equal(data.begin(), data.begin() + 6, "GIF87a") ||
                       std::equal(data.begin(), data.begin() + 6, "GIF89a"))) {
    return std::nullopt;
  }
  in.skip(6);
  GifImage image;
  image.width = in.word();
  image.height = in.word();
  const uint8_t screen_flags = in.byte();
  const uint8_t background = in.byte();
  in.byte();  // pixel aspect ratio
  if (image.width <= 0 || image.height <= 0 || image.width > kGifMaxSide ||
      image.height > kGifMaxSide) {
    return std::nullopt;
  }
  std::vector<std::array<uint8_t, 3>> global;
  if ((screen_flags & 0x80U) != 0) {
    global = colour_table(in, 2 << (screen_flags & 0x07U));
  }
  image.pixels.assign(
      static_cast<size_t>(image.width) * static_cast<size_t>(image.height),
      background);

  // Extensions up to the first image; only the graphic control extension's
  // transparent colour is of any use to a still picture.
  while (in.has(1)) {
    const uint8_t introducer = in.byte();
    if (introducer == kTrailer) {
      return std::nullopt;  // no image at all
    }
    if (introducer == kExtensionIntroducer) {
      const uint8_t label = in.byte();
      const std::vector<uint8_t> body = in.sub_blocks();
      if (label == kGraphicControlLabel && body.size() >= 4 &&
          (body[0] & 0x01U) != 0) {
        image.transparent_index = body[3];
      }
      continue;
    }
    if (introducer != kImageSeparator) {
      return std::nullopt;
    }
    break;
  }
  if (!in.has(9)) {
    return std::nullopt;
  }
  const int left = in.word();
  const int top = in.word();
  const int width = in.word();
  const int height = in.word();
  const uint8_t image_flags = in.byte();
  if (width <= 0 || height <= 0 || width > kGifMaxSide ||
      height > kGifMaxSide) {
    return std::nullopt;
  }
  image.palette = global;
  if ((image_flags & 0x80U) != 0) {
    image.palette = colour_table(in, 2 << (image_flags & 0x07U));
  }
  if (image.palette.empty()) {
    return std::nullopt;
  }
  const bool interlaced = (image_flags & 0x40U) != 0;
  const int min_code_bits = in.byte();
  if (min_code_bits < 2 || min_code_bits > 8) {
    return std::nullopt;
  }
  const std::vector<uint8_t> coded = in.sub_blocks();

  // LZW (GIF89a Appendix F): codes least significant bit first, a clear code
  // and an end code after the roots, the code width growing as the table does.
  const int clear = 1 << min_code_bits;
  const int end = clear + 1;
  std::vector<int16_t> prefix(kMaxCodes, -1);
  std::vector<uint8_t> suffix(kMaxCodes, 0);
  std::vector<uint8_t> first(kMaxCodes, 0);
  std::vector<uint16_t> length(kMaxCodes, 0);
  for (int c = 0; c < clear; ++c) {
    suffix[static_cast<size_t>(c)] = static_cast<uint8_t>(c);
    first[static_cast<size_t>(c)] = static_cast<uint8_t>(c);
    length[static_cast<size_t>(c)] = 1;
  }
  int code_bits = min_code_bits + 1;
  int next = clear + 2;
  int previous = -1;
  size_t bit = 0;
  const size_t total_bits = coded.size() * 8;
  const size_t pixel_count =
      static_cast<size_t>(width) * static_cast<size_t>(height);
  size_t produced = 0;
  std::vector<uint8_t> string(kMaxCodes);

  const auto emit = [&](int code) {
    // The code's string, walked back from its last byte.
    const size_t n = length[static_cast<size_t>(code)];
    size_t k = n;
    for (int c = code; c >= 0 && k > 0; c = prefix[static_cast<size_t>(c)]) {
      string[--k] = suffix[static_cast<size_t>(c)];
    }
    for (size_t i = 0; i < n && produced < pixel_count; ++i, ++produced) {
      const int x = static_cast<int>(produced % static_cast<size_t>(width));
      int y = static_cast<int>(produced / static_cast<size_t>(width));
      if (interlaced) {
        y = interlaced_row(y, height);
      }
      const int sx = left + x;
      const int sy = top + y;
      if (sx < image.width && sy < image.height) {
        image
            .pixels[static_cast<size_t>(sy) * static_cast<size_t>(image.width) +
                    static_cast<size_t>(sx)] = string[i];
      }
    }
  };

  while (produced < pixel_count &&
         bit + static_cast<size_t>(code_bits) <= total_bits) {
    int code = 0;
    for (int b = 0; b < code_bits; ++b, ++bit) {
      code |= ((coded[bit / 8] >> (bit % 8)) & 1) << b;
    }
    if (code == clear) {
      code_bits = min_code_bits + 1;
      next = clear + 2;
      previous = -1;
      continue;
    }
    if (code == end) {
      break;
    }
    if (previous < 0) {
      if (code >= clear) {
        break;  // the first code after a clear must be a root
      }
      emit(code);
      previous = code;
      continue;
    }
    uint8_t lead = 0;
    if (code < next) {
      lead = first[static_cast<size_t>(code)];
    } else if (code == next) {
      lead = first[static_cast<size_t>(previous)];
    } else {
      break;  // a code the table cannot have yet: the data is damaged
    }
    if (next < kMaxCodes) {
      prefix[static_cast<size_t>(next)] = static_cast<int16_t>(previous);
      suffix[static_cast<size_t>(next)] = lead;
      first[static_cast<size_t>(next)] = first[static_cast<size_t>(previous)];
      length[static_cast<size_t>(next)] = static_cast<uint16_t>(
          std::min<int>(length[static_cast<size_t>(previous)] + 1, kMaxCodes));
      ++next;
      if (next == (1 << code_bits) && code_bits < kMaxCodeBits) {
        ++code_bits;
      }
    }
    emit(code);
    previous = code;
  }
  // The end code may be missing from a stream that holds every pixel; one
  // that stops short of them, ended or not, did not hold the whole image.
  image.truncated = produced < pixel_count;
  return image;
}

}  // namespace orc
