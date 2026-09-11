/*
 * File:        preview_image_qt.cpp
 * Module:      orc-gui
 * Purpose:     Conversion from core PreviewImage RGB888 data to QImage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview_image_qt.h"

#include <orc/stage/preview/orc_rendering.h>

#include <cstring>

namespace orc::gui {

QImage previewImageToQImage(const orc::PreviewImage& image, QImage reuse) {
  if (image.rgb_data.empty() || image.width == 0 || image.height == 0) {
    return QImage();
  }

  QImage result = std::move(reuse);
  if (result.width() != static_cast<int>(image.width) ||
      result.height() != static_cast<int>(image.height) ||
      result.format() != QImage::Format_RGB888) {
    result = QImage(static_cast<int>(image.width),
                    static_cast<int>(image.height), QImage::Format_RGB888);
  }

  // QImage aligns scanlines to 4-byte boundaries, so check the stride.
  const size_t source_bytes_per_line = static_cast<size_t>(image.width) * 3;
  const size_t qimage_bytes_per_line =
      static_cast<size_t>(result.bytesPerLine());

  if (source_bytes_per_line == qimage_bytes_per_line) {
    std::memcpy(result.bits(), image.rgb_data.data(), image.rgb_data.size());
  } else {
    for (size_t y = 0; y < image.height; ++y) {
      auto* scan_line = result.scanLine(static_cast<int>(y));
      const uint8_t* src = &image.rgb_data[y * source_bytes_per_line];
      std::memcpy(scan_line, src, source_bytes_per_line);
    }
  }

  return result;
}

QImage previewImageToRgbaQImage(const orc::PreviewImage& image, QImage reuse) {
  if (image.rgb_data.empty() || image.width == 0 || image.height == 0) {
    return QImage();
  }

  QImage result = std::move(reuse);
  if (result.width() != static_cast<int>(image.width) ||
      result.height() != static_cast<int>(image.height) ||
      result.format() != QImage::Format_RGBA8888) {
    result = QImage(static_cast<int>(image.width),
                    static_cast<int>(image.height), QImage::Format_RGBA8888);
  }

  const size_t source_bytes_per_line = static_cast<size_t>(image.width) * 3;
  for (size_t y = 0; y < image.height; ++y) {
    auto* scan_line = result.scanLine(static_cast<int>(y));
    const uint8_t* src = &image.rgb_data[y * source_bytes_per_line];
    for (size_t x = 0; x < image.width; ++x) {
      scan_line[x * 4 + 0] = src[x * 3 + 0];
      scan_line[x * 4 + 1] = src[x * 3 + 1];
      scan_line[x * 4 + 2] = src[x * 3 + 2];
      scan_line[x * 4 + 3] = 0xFF;
    }
  }

  return result;
}

}  // namespace orc::gui
