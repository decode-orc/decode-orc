/*
 * File:        gif_decoder.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Decodes the first image of a GIF file into palette indices
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#ifndef ORC_NABTS_SINK_GIF_DECODER_H
#define ORC_NABTS_SINK_GIF_DECODER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace orc {

// The largest image decoded, on either side: well above any picture a
// datacast of the period carried, and small enough that a damaged size field
// cannot ask for gigabytes.
constexpr int kGifMaxSide = 4096;
// The largest logical screen decoded, by area: a VGA picture with room to
// spare. Each pixel costs a byte here and a CatalogueColour once drawn, so the
// side limit alone would let a 13-byte header ask for a 4096 by 4096 screen,
// 16 MB decoded and over 100 MB as a display list, with no data behind it.
constexpr size_t kGifMaxPixels = size_t{1} << 20;

/// The first image of a GIF file (GIF87a or GIF89a), as palette indices.
struct GifImage {
  /// The logical screen, which the image is drawn on.
  int width = 0;
  int height = 0;
  /// One index into @ref palette per pixel of the logical screen, row by row
  /// from the top. Pixels outside the image take the background index.
  std::vector<uint8_t> pixels;
  /// The colour table the image uses: its own where it has one, the file's
  /// otherwise.
  std::vector<std::array<uint8_t, 3>> palette;
  /// The index a graphic control extension names transparent, or -1.
  int transparent_index = -1;
  /// The image's coded data ran out, or did not end where it should, before
  /// every pixel was decoded: what was decoded is kept, the rest left at the
  /// background.
  bool truncated = false;
};

/**
 * @brief Decode the first image of the GIF file |data| holds
 *
 * Reads the logical screen, the global colour table, any extensions before the
 * first image (a graphic control extension's transparent colour is kept), the
 * image descriptor and its local colour table, and its LZW-coded data, with
 * interlaced rows put back in order. Nothing for bytes that are not a GIF file,
 * or whose sizes are out of bounds (kGifMaxSide, kGifMaxPixels); a file whose
 * coded data is damaged or cut short decodes as far as it goes
 * (GifImage::truncated).
 *
 * Pure: no I/O.
 */
std::optional<GifImage> decode_gif(const std::vector<uint8_t>& data);

}  // namespace orc

#endif  // ORC_NABTS_SINK_GIF_DECODER_H
