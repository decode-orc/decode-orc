/*
 * File:        transformpal.cpp
 * Module:      orc-core
 * Purpose:     Transform PAL base decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018 William Andrew Steer
 * SPDX-FileCopyrightText: 2018-2026 Simon Inns
 * SPDX-FileCopyrightText: 2019-2021 Adam Sampson
 */

#include "transformpal.h"

#include <algorithm>
#include <cassert>
#include <cmath>

TransformPal::TransformPal(int32_t _xComplex, int32_t _yComplex,
                           int32_t _zComplex)
    : xComplex(_xComplex),
      yComplex(_yComplex),
      zComplex(_zComplex),
      configurationSet(false) {}

TransformPal::~TransformPal() {}

std::mutex& TransformPal::fftwPlannerMutex() {
  static std::mutex mutex;
  return mutex;
}

void TransformPal::updateConfiguration(
    const ::orc::SourceParameters& _videoParameters, double threshold,
    const std::vector<double>& _thresholds) {
  videoParameters = _videoParameters;

  // Resize thresholds to match the number of FFT bins we will consider in
  // applyFilter. The x loop there doesn't need to look at every bin.
  const int32_t thresholdsSize = ((xComplex / 4) + 1) * yComplex * zComplex;

  if (_thresholds.size() == 0) {
    // Use the same (squared) threshold value for all bins
    thresholds.resize(thresholdsSize);
    std::fill(thresholds.begin(), thresholds.end(), threshold * threshold);
  } else {
    // Square the provided thresholds
    assert(static_cast<int32_t>(_thresholds.size()) == thresholdsSize);
    thresholds.resize(thresholdsSize);
    for (int i = 0; i < thresholdsSize; i++) {
      thresholds[i] = _thresholds[i] * _thresholds[i];
    }
  }

  configurationSet = true;
}

void TransformPal::overlayFFT(int32_t positionX, int32_t positionY,
                              const std::vector<SourceField>& inputFields,
                              int32_t startIndex, int32_t endIndex,
                              std::vector<ComponentFrame>& componentFrames) {
  // Visualise the first field for each frame
  for (int fieldIndex = startIndex, outputIndex = 0; fieldIndex < endIndex;
       fieldIndex += 2, outputIndex++) {
    overlayFFTFrame(positionX, positionY, inputFields, fieldIndex,
                    componentFrames[outputIndex]);
  }
}

// Overlay the input and output FFT arrays, in either 2D or 3D
void TransformPal::overlayFFTArrays(const fftw_complex* fftIn,
                                    const fftw_complex* fftOut,
                                    FrameCanvas& canvas) {
  // Colours
  const auto green = canvas.rgb(0, 0xFFFF, 0);

  // How many pixels to draw for each bin
  const int32_t xScale = 2;
  const int32_t yScale = 2;

  // Each block shows the absolute value of the real component of an FFT bin
  // using a log scale. Work out a scaling factor to make all values visible.
  double maxValue = 0;
  for (int32_t i = 0; i < xComplex * yComplex * zComplex; i++) {
    maxValue = std::max(maxValue, fabs(fftIn[i][0]));
    maxValue = std::max(maxValue, fabs(fftOut[i][0]));
  }
  const double valueScale = 65535.0 / log2(maxValue);

  // Draw each 2D plane of the array
  for (int32_t z = 0; z < zComplex; z++) {
    for (int32_t column = 0; column < 2; column++) {
      const fftw_complex* fftData = column == 0 ? fftIn : fftOut;

      // Work out where this 2D array starts
      const int32_t yStart = canvas.top() + (z * ((yScale * yComplex) + 1));
      const int32_t xStart =
          canvas.right() - ((2 - column) * ((xScale * xComplex) + 1)) - 1;

      // Outline the array
      canvas.drawRectangle(xStart, yStart, (xScale * xComplex) + 2,
                           (yScale * yComplex) + 2, green);

      // Draw the bins in the array
      for (int32_t y = 0; y < yComplex; y++) {
        for (int32_t x = 0; x < xComplex; x++) {
          const double value =
              fabs(fftData[(((z * yComplex) + y) * xComplex) + x][0]);
          const double shade = value <= 0 ? 0 : log2(value) * valueScale;
          const uint16_t shade16 =
              static_cast<uint16_t>(std::clamp(shade, 0.0, 65535.0));
          canvas.fillRectangle(xStart + (x * xScale) + 1,
                               yStart + (y * yScale) + 1, xScale, yScale,
                               canvas.grey(shade16));
        }
      }
    }
  }
}
