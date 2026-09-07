/*
 * File:        outputwriter.cpp
 * Module:      orc-core
 * Purpose:     Output format writer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2026 Simon Inns
 * SPDX-FileCopyrightText: 2019-2021 Adam Sampson
 */

#include "outputwriter.h"

#include <orc/stage/cvbs_signal_constants.h>
#include <orc/support/logging.h>

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <sstream>

#include "componentframe.h"

// Limits, zero points and scaling factors (from 0-1) for Y'CbCr colour
// representations [Poynton ch25 p305] [BT.601-7 sec 2.5.3]
static constexpr double Y_MIN = 1.0 * 256.0;
static constexpr double Y_ZERO = 16.0 * 256.0;
static constexpr double Y_SCALE = 219.0 * 256.0;
static constexpr double Y_MAX = 254.75 * 256.0;
static constexpr double C_MIN = 1.0 * 256.0;
static constexpr double C_ZERO = 128.0 * 256.0;
static constexpr double C_SCALE = 112.0 * 256.0;
static constexpr double C_MAX = 254.75 * 256.0;

// ITU-R BT.601-7
// [Poynton eq 25.1 p303 and eq 25.5 p307]
static constexpr double ONE_MINUS_Kb = 1.0 - 0.114;
static constexpr double ONE_MINUS_Kr = 1.0 - 0.299;

// kB = sqrt(209556997.0 / 96146491.0) / 3.0
// kR = sqrt(221990474.0 / 288439473.0)
// [Poynton eq 28.1 p336]
static constexpr double kB = 0.49211104112248356308804691718185;
static constexpr double kR = 0.87728321993817866838972487283129;

void OutputWriter::updateConfiguration(
    ::orc::SourceParameters& _videoParameters,
    const OutputWriter::Configuration& _config) {
  config = _config;
  videoParameters = _videoParameters;
  topPadLines = 0;
  bottomPadLines = 0;

  activeWidth =
      videoParameters.active_video_end - videoParameters.active_video_start;
  activeHeight = videoParameters.last_active_frame_line -
                 videoParameters.first_active_frame_line;
  outputHeight = activeHeight;

  if (config.paddingAmount > 1) {
    // Some video codecs require the width and height of a video to be divisible
    // by a given number of samples on each axis.

    // Expand horizontal active region so the width is divisible by the
    // specified padding factor.
    while (true) {
      activeWidth =
          videoParameters.active_video_end - videoParameters.active_video_start;
      if ((activeWidth % config.paddingAmount) == 0) {
        break;
      }

      // Add pixels to the right and left sides in turn, to keep the active area
      // centred
      if ((activeWidth % 2) == 0) {
        videoParameters.active_video_end++;
      } else {
        videoParameters.active_video_start--;
      }
    }

    // Insert empty padding lines so the height is divisible by by the specified
    // padding factor.
    while (true) {
      outputHeight = topPadLines + activeHeight + bottomPadLines;
      if ((outputHeight % config.paddingAmount) == 0) {
        break;
      }

      // Add lines to the bottom and top in turn, to keep the active area
      // centred
      if ((outputHeight % 2) == 0) {
        bottomPadLines++;
      } else {
        topPadLines++;
      }
    }

    // Update the caller's copy, now we've adjusted the active area
    _videoParameters = videoParameters;
  }
}

const char* OutputWriter::getPixelName() const {
  switch (config.pixelFormat) {
    case RGB48:
      return "RGB48";
    case YUV444P16:
      return "YUV444P16";
    case GRAY16:
      return "GRAY16";
    default:
      return "unknown";
  }
}

void OutputWriter::printOutputInfo() const {
  // Show output information to the user
  const int32_t frameHeight =
      static_cast<int32_t>(
          orc::calculate_padded_field_height(videoParameters.system)) *
          2 -
      1;
  ORC_LOG_DEBUG(
      "Input video of {}x{} will be colourised and trimmed to {}x{} {} frames",
      videoParameters.frame_width_nominal, frameHeight, activeWidth,
      outputHeight, getPixelName());
}

std::string OutputWriter::getStreamHeader() const {
  // Only yuv4mpeg output needs a header
  if (!config.outputY4m) {
    return std::string();
  }

  std::ostringstream str;

  str << "YUV4MPEG2";

  // Frame size
  str << " W" << activeWidth;
  str << " H" << outputHeight;

  // Frame rate
  if (videoParameters.system == orc::VideoSystem::PAL) {
    str << " F25:1";
  } else {
    str << " F30000:1001";
  }

  // Field order
  if (videoParameters.first_active_frame_line % 2 ^ topPadLines % 2) {
    str << " Ib";
  } else {
    str << " It";
  }

  // Pixel aspect ratio
  // Follows EBU R92 and SMPTE RP 187 except that values are scaled from
  // BT.601 sampling (13.5 MHz) to 4fSC
  if (videoParameters.system == orc::VideoSystem::PAL) {
    if (videoParameters.is_widescreen) {
      str << " A865:779";  // (16 / 9) * (576 / (702 * 4*fSC / 13.5))
    } else {
      str << " A259:311";  // (4 / 3) * (576 / (702 * 4*fSC / 13.5))
    }
  } else {
    if (videoParameters.is_widescreen) {
      str << " A25:22";  // (16 / 9) * (480 / (708 * 4*fSC / 13.5))
    } else {
      str << " A352:413";  // (4 / 3) * (480 / (708 * 4*fSC / 13.5))
    }
  }

  // Pixel format
  switch (config.pixelFormat) {
    case YUV444P16:
      str << " C444p16 XCOLORRANGE=LIMITED";
      break;
    case GRAY16:
      str << " Cmono16 XCOLORRANGE=LIMITED";
      break;
    default:
      ORC_LOG_CRITICAL("pixel format not supported in yuv4mpeg header");
      std::abort();
      break;
  }

  str << "\n";
  return str.str();
}

std::string OutputWriter::getFrameHeader() const {
  // Only yuv4mpeg output needs a header
  if (!config.outputY4m) {
    return std::string();
  }

  return "FRAME\n";
}

void OutputWriter::convert(const ComponentFrame& componentFrame,
                           OutputFrame& outputFrame) const {
  // Work out the number of output values, and resize the vector accordingly
  int32_t totalSize = activeWidth * outputHeight;
  switch (config.pixelFormat) {
    case RGB48:
    case YUV444P16:
      totalSize *= 3;
      break;
    case GRAY16:
      break;
  }
  outputFrame.resize(totalSize);

  // Clear padding
  clearPadLines(0, topPadLines, outputFrame);
  clearPadLines(outputHeight - bottomPadLines, bottomPadLines, outputFrame);

  // Convert active lines
  for (int32_t y = 0; y < activeHeight; y++) {
    convertLine(y, componentFrame, outputFrame);
  }
}

void OutputWriter::clearPadLines(int32_t firstLine, int32_t numLines,
                                 OutputFrame& outputFrame) const {
  switch (config.pixelFormat) {
    case RGB48: {
      // Fill with RGB black
      uint16_t* out = outputFrame.data() +
                      (static_cast<ptrdiff_t>(activeWidth * firstLine * 3));

      for (int32_t i = 0; i < numLines * activeWidth * 3; i++) {
        out[i] = 0;
      }

      break;
    }
    case YUV444P16: {
      // Fill Y with black, no chroma
      uint16_t* outY = outputFrame.data() +
                       (static_cast<ptrdiff_t>(activeWidth * firstLine));
      uint16_t* outCB =
          outY + (static_cast<ptrdiff_t>(activeWidth * outputHeight));
      uint16_t* outCR =
          outCB + (static_cast<ptrdiff_t>(activeWidth * outputHeight));

      for (int32_t i = 0; i < numLines * activeWidth; i++) {
        outY[i] = static_cast<uint16_t>(Y_ZERO);
        outCB[i] = static_cast<uint16_t>(C_ZERO);
        outCR[i] = static_cast<uint16_t>(C_ZERO);
      }

      break;
    }
    case GRAY16: {
      // Fill with black
      uint16_t* out = outputFrame.data() +
                      (static_cast<ptrdiff_t>(activeWidth * firstLine));

      for (int32_t i = 0; i < numLines * activeWidth; i++) {
        out[i] = static_cast<uint16_t>(Y_ZERO);
      }

      break;
    }
  }
}

void OutputWriter::convertLine(int32_t lineNumber,
                               const ComponentFrame& componentFrame,
                               OutputFrame& outputFrame) const {
  // When cropping is applied, componentFrame is indexed from 0
  // Otherwise, it's indexed from first_active_frame_line
  const int32_t inputLine =
      videoParameters.active_area_cropping_applied
          ? lineNumber
          : (videoParameters.first_active_frame_line + lineNumber);
  const int32_t xOffset = videoParameters.active_area_cropping_applied
                              ? 0
                              : videoParameters.active_video_start;

  // Get pointers to the component data for the active region
  const double* inY = componentFrame.y(inputLine) + xOffset;
  // Not used if output is GRAY16
  const double* inU = (config.pixelFormat != GRAY16)
                          ? componentFrame.u(inputLine) + xOffset
                          : nullptr;
  const double* inV = (config.pixelFormat != GRAY16)
                          ? componentFrame.v(inputLine) + xOffset
                          : nullptr;

  const int32_t outputLine = topPadLines + lineNumber;

  const double yOffset = static_cast<double>(videoParameters.black_level);
  const double yRange = static_cast<double>(videoParameters.white_level -
                                            videoParameters.black_level);
  // U/V share the luma excursion.  The setup pedestal does not offset chroma,
  // but it does shorten the picture excursion that defines the volts per unit
  // of colour difference, so the subcarrier amplitude shrinks with it: on a
  // 7.5 IRE setup system one unit of Y'/U/V spans 92.5 IRE, not 100.  Measured
  // against the EIA-189-A 75% bars, whose chroma peak-to-peak amplitudes
  // (62 / 88 / 82 IRE for yellow / cyan / green) only come out right on the
  // black-to-white excursion.  Deriving the range from black rather than
  // blanking also covers the systems without a pedestal — PAL, and NTSC-J,
  // whose picture black tbc_source reports at the blanking level — where the
  // two ranges coincide.  FFmpegOutputBackend::convertAndEncode() scales
  // chroma the same way, so the two export paths agree.
  const double uvRange = yRange;

  switch (config.pixelFormat) {
    case RGB48: {
      // Convert Y'UV to full-range R'G'B' [Poynton eq 28.6 p337]
      uint16_t* out = outputFrame.data() +
                      (static_cast<ptrdiff_t>(activeWidth * outputLine * 3));

      const double yScale = 65535.0 / yRange;
      const double uvScale = 65535.0 / uvRange;

      for (int32_t x = 0; x < activeWidth; x++) {
        // Scale Y'UV to 0-65535
        const double rY = std::clamp((inY[x] - yOffset) * yScale, 0.0, 65535.0);
        const double rU = inU[x] * uvScale;
        const double rV = inV[x] * uvScale;

        // Convert Y'UV to R'G'B'
        const int32_t pos = x * 3;
        out[pos] = static_cast<uint16_t>(
            std::clamp(rY + (1.139883 * rV), 0.0, 65535.0));
        out[pos + 1] = static_cast<uint16_t>(
            std::clamp(rY + (-0.394642 * rU) + (-0.580622 * rV), 0.0, 65535.0));
        out[pos + 2] = static_cast<uint16_t>(
            std::clamp(rY + (2.032062 * rU), 0.0, 65535.0));
      }

      break;
    }
    case YUV444P16: {
      // Convert Y'UV to Y'CbCr [Poynton eq 25.5 p307]
      uint16_t* outY = outputFrame.data() +
                       (static_cast<ptrdiff_t>(activeWidth * outputLine));
      uint16_t* outCB =
          outY + (static_cast<ptrdiff_t>(activeWidth * outputHeight));
      uint16_t* outCR =
          outCB + (static_cast<ptrdiff_t>(activeWidth * outputHeight));

      const double yScale = Y_SCALE / yRange;
      const double cbScale = (C_SCALE / (ONE_MINUS_Kb * kB)) / uvRange;
      const double crScale = (C_SCALE / (ONE_MINUS_Kr * kR)) / uvRange;

      for (int32_t x = 0; x < activeWidth; x++) {
        outY[x] = static_cast<uint16_t>(
            std::clamp(((inY[x] - yOffset) * yScale) + Y_ZERO, Y_MIN, Y_MAX));
        outCB[x] = static_cast<uint16_t>(
            std::clamp((inU[x] * cbScale) + C_ZERO, C_MIN, C_MAX));
        outCR[x] = static_cast<uint16_t>(
            std::clamp((inV[x] * crScale) + C_ZERO, C_MIN, C_MAX));
      }

      break;
    }
    case GRAY16: {
      // Throw away UV and just convert Y' to the same scale as Y'CbCr
      uint16_t* out = outputFrame.data() +
                      (static_cast<ptrdiff_t>(activeWidth * outputLine));

      const double yScale = Y_SCALE / yRange;

      for (int32_t x = 0; x < activeWidth; x++) {
        out[x] = static_cast<uint16_t>(
            std::clamp(((inY[x] - yOffset) * yScale) + Y_ZERO, Y_MIN, Y_MAX));
      }

      break;
    }
  }
}
