/*
 * File:        output_backend.cpp
 * Module:      orc-core
 * Purpose:     Output backend factory implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "output_backend.h"

#include "raw_output_backend.h"

#ifdef HAVE_FFMPEG
#include "ffmpeg_output_backend.h"
#endif

namespace orc {

std::unique_ptr<OutputBackend> OutputBackendFactory::create(
    const std::string& format) {
  // Raw formats
  if (format == "rgb" || format == "yuv" || format == "y4m") {
    return std::make_unique<RawOutputBackend>();
  }

#ifdef HAVE_FFMPEG
  // Encoded formats (require FFmpeg)
  if (format.find("mp4-") == 0 || format.find("mkv-") == 0 ||
      format.find("mov-") == 0 || format.find("mxf-") == 0) {
    return std::make_unique<FFmpegOutputBackend>();
  }
#endif

  // Unknown format
  return nullptr;
}

std::vector<std::string> OutputBackendFactory::getSupportedFormats() {
  std::vector<std::string> formats = {"rgb", "yuv", "y4m"};

#ifdef HAVE_FFMPEG
  // Lossless/Archive formats
  formats.push_back("mkv-ffv1");
  // Same codec on the ITU-R BT.601 13.5 MHz grid ("FFV1 for VP415e")
  formats.push_back("mkv-ffv1-bt601");

  // ProRes formats (variant selected by prores_profile parameter)
  formats.push_back("mov-prores");

  // Uncompressed formats
  formats.push_back("mov-v210");
  formats.push_back("mov-v410");

  // D10 (Sony IMX/XDCAM)
  formats.push_back("mxf-mpeg2video");

  // H.264 formats (hardware variant selected by hardware_encoder parameter)
  formats.push_back("mp4-h264");
  formats.push_back("mov-h264");

  // H.265 formats (hardware variant selected by hardware_encoder parameter)
  formats.push_back("mp4-hevc");
  formats.push_back("mov-hevc");

  // AV1 format
  formats.push_back("mp4-av1");
#endif

  return formats;
}

}  // namespace orc
