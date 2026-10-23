/*
 * File:        componentframe.h
 * Module:      orc-core
 * Purpose:     Component frame buffer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2021 Adam Sampson
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef COMPONENTFRAME_H
#define COMPONENTFRAME_H

#include <orc/stage/orc_source_parameters.h>
#include <orc/support/logging.h>

#include <cassert>
#include <cstdint>
#include <vector>

// Two complete, interlaced fields' worth of decoded luma and chroma
// information.
//
// The luma and chroma samples have the same scaling as in the original
// composite signal (i.e. they're not in Y'CbCr form yet). You can recover the
// chroma signal by subtracting Y from the composite signal.
class ComponentFrame {
 public:
  ComponentFrame();

  // Set the frame's size and clear it to black
  // If mono is true, only Y set to black, while U and V are cleared.
  void init(const ::orc::SourceParameters& videoParameters, bool mono = false);

  // Get a pointer to a line of samples. Line numbers are 0-based within the
  // frame. Lines are stored in a contiguous array, so it's safe to get a
  // pointer to line 0 and use it to refer to later lines.
  double* y(int32_t line) {
    if (line < 0) {
      ORC_LOG_ERROR("ComponentFrame::y() called with negative line: {}", line);
    }
    return yData.data() + getLineOffset(line);
  }
  double* u(int32_t line) { return uData.data() + getLineOffsetUV(line); }
  double* v(int32_t line) { return vData.data() + getLineOffsetUV(line); }
  const double* y(int32_t line) const {
    return yData.data() + getLineOffset(line);
  }
  const double* u(int32_t line) const {
    return uData.data() + getLineOffsetUV(line);
  }
  const double* v(int32_t line) const {
    return vData.data() + getLineOffsetUV(line);
  }

  std::vector<double>* getY() { return &yData; }
  const std::vector<double>* getY() const { return &yData; }

  std::vector<double>* getU() { return &uData; }
  const std::vector<double>* getU() const { return &uData; }

  std::vector<double>* getV() { return &vData; }
  const std::vector<double>* getV() const { return &vData; }

  void setY(std::vector<double>& _yData) { yData = _yData; }

  void setU(std::vector<double>& _uData) { uData = _uData; }

  void setV(std::vector<double>& _vData) { vData = _vData; }

  // Replace this frame's Y plane with the Y plane from luma_source.
  // U and V planes are untouched. Both frames must have identical dimensions.
  // This is the in-process equivalent of the FFmpeg extractplanes/mergeplanes
  // filter used by tbc-video-export to combine the mono Y and colour UV
  // outputs for Y/C (colour-under) sources.
  void merge_luma_from(const ComponentFrame& luma_source);

  int32_t getWidth() const { return width; }
  int32_t getHeight() const { return height; }

 private:
  int32_t getLineOffset(int32_t line) const {
    if (line < 0) {
      ORC_LOG_ERROR(
          "ComponentFrame::getLineOffset called with negative line: {}, "
          "yData.size={}, height={}",
          line, yData.size(), height);
    }
    assert(line >= 0);
    assert(line < static_cast<int32_t>(yData.size()));
    return line * width;
  }

  int32_t getLineOffsetUV(int32_t line) const {
    assert(line >= 0);
    assert(line < static_cast<int32_t>(uData.size()));
    return line * width;
  }

  // Size of the frame
  int32_t width;
  int32_t height;

  // Samples for Y, U and V
  std::vector<double> yData;
  std::vector<double> uData;
  std::vector<double> vData;
};

#endif  // COMPONENTFRAME_H
