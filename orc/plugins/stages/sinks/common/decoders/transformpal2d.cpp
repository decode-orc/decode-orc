/*
 * File:        transformpal2d.cpp
 * Module:      orc-core
 * Purpose:     Transform PAL 2D decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2026 Simon Inns
 */

#include "transformpal2d.h"

#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstddef>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*!
    \class TransformPal2D

    2D Transform PAL filter, based on Jim Easterbrook's implementation in
    pyctools-pal. Given a composite signal, this extracts a chroma signal from
    it using frequency-domain processing.

    For a description of the algorithm with examples, see the Transform PAL web
    site (http://www.jim-easterbrook.me.uk/pal/).
 */

// Compute one value of the window function, applied to the data blocks before
// the FFT to reduce edge effects. This is a symmetrical raised-cosine
// function, which means that the overlapping inverse-FFT blocks can be summed
// directly without needing an inverse window function.
static double computeWindow(int32_t element, int32_t limit) {
  return 0.5 - (0.5 * cos((2 * M_PI * (element + 0.5)) / limit));
}

TransformPal2D::TransformPal2D() : TransformPal(XCOMPLEX, YCOMPLEX, 1) {
  // Compute the window function.
  for (int32_t y = 0; y < YTILE; y++) {
    const double windowY = computeWindow(y, YTILE);
    for (int32_t x = 0; x < XTILE; x++) {
      const double windowX = computeWindow(x, XTILE);
      windowFunction[y][x] = windowY * windowX;
    }
  }

  // Allocate buffers for FFTW. These must be allocated using FFTW's own
  // functions so they're properly aligned for SIMD operations.
  fftReal = fftw_alloc_real(static_cast<size_t>(YTILE) * XTILE);
  fftComplexIn = fftw_alloc_complex(static_cast<size_t>(YCOMPLEX) * XCOMPLEX);
  fftComplexOut = fftw_alloc_complex(static_cast<size_t>(YCOMPLEX) * XCOMPLEX);

  // Plan FFTW operations
  std::lock_guard<std::mutex> lock(fftwPlannerMutex());
  forwardPlan =
      fftw_plan_dft_r2c_2d(YTILE, XTILE, fftReal, fftComplexIn, FFTW_MEASURE);
  inversePlan =
      fftw_plan_dft_c2r_2d(YTILE, XTILE, fftComplexOut, fftReal, FFTW_MEASURE);
}

TransformPal2D::~TransformPal2D() {
  // Free FFTW plans and buffers
  {
    std::lock_guard<std::mutex> lock(fftwPlannerMutex());
    fftw_destroy_plan(forwardPlan);
    fftw_destroy_plan(inversePlan);
  }
  fftw_free(fftReal);
  fftw_free(fftComplexIn);
  fftw_free(fftComplexOut);
}

int32_t TransformPal2D::getThresholdsSize() {
  // On the X axis, include only the bins we actually use in applyFilter
  return YCOMPLEX * ((XCOMPLEX / 4) + 1);
}

void TransformPal2D::filterFields(const std::vector<SourceField>& inputFields,
                                  int32_t startIndex, int32_t endIndex,
                                  std::vector<const double*>& outputFields) {
  assert(configurationSet);

  // Check for YC sources - not supported by Transform PAL
  if (!inputFields.empty() && inputFields[0].is_yc) {
    ORC_LOG_ERROR(
        "TransformPal2D: YC sources are not supported. Use NTSC/Comb decoder "
        "instead.");
    // Return empty output to avoid crashes
    for (int32_t i = 0; i < static_cast<int32_t>(outputFields.size()); i++) {
      outputFields[i] = nullptr;
    }
    return;
  }

  // Check we have a valid vector of input fields, and a matching output vector
  assert((inputFields.size() % 2) == 0);
  for (int32_t i = 0; i < static_cast<int32_t>(inputFields.size()); i++) {
    assert(inputFields[i].data != nullptr);
  }
  assert(outputFields.size() == (endIndex - startIndex));

  // Allocate and clear output buffers
  chromaBuf.resize(endIndex - startIndex);

  for (int32_t i = 0; i < static_cast<int32_t>(chromaBuf.size()); i++) {
    chromaBuf[i].resize(
        static_cast<size_t>(videoParameters.frame_width_nominal) *
        static_cast<int32_t>(
            calculate_padded_field_height(videoParameters.system)));
    std::fill(chromaBuf[i].begin(), chromaBuf[i].end(), 0.0);

    outputFields[i] = chromaBuf[i].data();
  }

  for (int32_t i = startIndex, j = 0; i < endIndex; i++, j++) {
    filterField(inputFields[i], j);
  }
}

// Process one field, writing the result into chromaBuf[outputIndex]
void TransformPal2D::filterField(const SourceField& inputField,
                                 int32_t outputIndex) {
  // Convert frame-based active area limits to field-based coordinates
  // This ensures proper indexing when active area cropping is applied
  const int32_t firstFieldLine =
      (videoParameters.first_active_frame_line + 1 - inputField.getOffset()) /
      2;
  const int32_t lastFieldLine =
      (videoParameters.last_active_frame_line + 1 - inputField.getOffset()) / 2;

  // Iterate through the overlapping tile positions, covering the active area.
  // (See TransformPal2D member variable documentation for how the tiling
  // works.)
  for (int32_t tileY = firstFieldLine - HALFYTILE; tileY < lastFieldLine;
       tileY += HALFYTILE) {
    // Work out which lines of these tiles are within the active region
    const int32_t startY = std::max(firstFieldLine - tileY, 0);
    const int32_t endY = std::min(lastFieldLine - tileY, YTILE);

    for (int32_t tileX = videoParameters.active_video_start - HALFXTILE;
         tileX < videoParameters.active_video_end; tileX += HALFXTILE) {
      // Compute the forward FFT
      forwardFFTTile(tileX, tileY, startY, endY, inputField);

      // Apply the frequency-domain filter
      applyFilter();

      // Compute the inverse FFT
      inverseFFTTile(tileX, tileY, startY, endY, outputIndex);
    }
  }
}

// Apply the forward FFT to an input tile, populating fftComplexIn
void TransformPal2D::forwardFFTTile(int32_t tileX, int32_t tileY,
                                    int32_t startY, int32_t endY,
                                    const SourceField& inputField) {
  // Copy the input signal into fftReal, applying the window function.
  // Use inputField.getLine() to correctly handle PAL non-uniform lines.
  for (int32_t y = 0; y < YTILE; y++) {
    // If this frame line is above/below the active region, fill it with
    // blanking level instead.
    if (y < startY || y >= endY) {
      for (int32_t x = 0; x < XTILE; x++) {
        // EBU Tech. 3280-E: blanking level = kPalBlanking in 10-bit domain.
        fftReal[(y * XTILE) + x] = orc::kPalBlanking * windowFunction[y][x];
      }
      continue;
    }

    const int32_t field_line = tileY + y;
    const int16_t* b = inputField.getLine(static_cast<size_t>(field_line));
    for (int32_t x = 0; x < XTILE; x++) {
      fftReal[(y * XTILE) + x] = b[tileX + x] * windowFunction[y][x];
    }
  }

  // Convert time domain in fftReal to frequency domain in fftComplexIn
  fftw_execute(forwardPlan);
}

// Apply the inverse FFT to fftComplexOut, overlaying the result into
// chromaBuf[outputIndex]
void TransformPal2D::inverseFFTTile(int32_t tileX, int32_t tileY,
                                    int32_t startY, int32_t endY,
                                    int32_t outputIndex) {
  // Work out what X range of this tile is inside the active area
  const int32_t startX =
      std::max(videoParameters.active_video_start - tileX, 0);
  const int32_t endX =
      std::min(videoParameters.active_video_end - tileX, XTILE);

  // Convert frequency domain in fftComplexOut back to time domain in fftReal
  fftw_execute(inversePlan);

  // Overlay the result, normalising the FFTW output, into chromaBuf
  double* outputPtr = chromaBuf[outputIndex].data();
  for (int32_t y = startY; y < endY; y++) {
    double* b =
        outputPtr + (static_cast<ptrdiff_t>(
                        (tileY + y) * videoParameters.frame_width_nominal));
    for (int32_t x = startX; x < endX; x++) {
      b[tileX + x] += fftReal[(y * XTILE) + x] / (YTILE * XTILE);
    }
  }
}

// Return the absolute value squared of an fftw_complex
static inline double fftwAbsSq(const fftw_complex& value) {
  return (value[0] * value[0]) + (value[1] * value[1]);
}

// Apply the frequency-domain filter.
void TransformPal2D::applyFilter() {
  // Get pointer to squared threshold values
  const double* thresholdsPtr = thresholds.data();

  // Clear fftComplexOut. We discard values by default; the filter only
  // copies values that look like chroma.
  for (int32_t i = 0; i < XCOMPLEX * YCOMPLEX; i++) {
    fftComplexOut[i][0] = 0.0;
    fftComplexOut[i][1] = 0.0;
  }

  // This is a direct translation of transform_filter from pyctools-pal.
  // The main simplification is that we don't need to worry about
  // conjugates, because FFTW only returns half the result in the first
  // place.
  //
  // The general idea is that a real modulated chroma signal will be
  // symmetrical around the U carrier, which is at fSC Hz and 72 c/aph -- and
  // because we're sampling at 4fSC, this is handily equivalent to being
  // symmetrical around the V carrier owing to wraparound. We look at every
  // bin that might be a chroma signal, and only keep it if it's
  // sufficiently symmetrical with its reflection.
  //
  // The Y axis covers 0 to 288 c/aph;  72 c/aph is 1/4 * YTILE.
  // The X axis covers 0 to 4fSC Hz;    fSC HZ   is 1/4 * XTILE.

  for (int32_t y = 0; y < YTILE; y++) {
    // Reflect around 72 c/aph vertically.
    const int32_t y_ref = ((YTILE / 2) + YTILE - y) % YTILE;

    // Input data for this line and its reflection
    const fftw_complex* bi =
        fftComplexIn + (static_cast<ptrdiff_t>(y * XCOMPLEX));
    const fftw_complex* bi_ref =
        fftComplexIn + (static_cast<ptrdiff_t>(y_ref * XCOMPLEX));

    // Output data for this line and its reflection
    fftw_complex* bo = fftComplexOut + (static_cast<ptrdiff_t>(y * XCOMPLEX));
    fftw_complex* bo_ref =
        fftComplexOut + (static_cast<ptrdiff_t>(y_ref * XCOMPLEX));

    // We only need to look at horizontal frequencies that might be chroma
    // (0.5fSC to 1.5fSC).
    for (int32_t x = XTILE / 8; x <= XTILE / 4; x++) {
      // Reflect around fSC horizontally
      const int32_t x_ref = (XTILE / 2) - x;

      // Get the threshold for this bin
      const double threshold_sq = *thresholdsPtr++;

      const fftw_complex& in_val = bi[x];
      const fftw_complex& ref_val = bi_ref[x_ref];

      if (x == x_ref && y == y_ref) {
        // This bin is its own reflection (i.e. it's a carrier). Keep it!
        bo[x][0] = in_val[0];
        bo[x][1] = in_val[1];
        continue;
      }

      // Get the squares of the magnitudes (to minimise the number of sqrts)
      const double m_in_sq = fftwAbsSq(in_val);
      const double m_ref_sq = fftwAbsSq(ref_val);

      // Compare the magnitudes of the two values, and discard both
      // if they are more different than the threshold for this
      // bin.
      if (m_in_sq < m_ref_sq * threshold_sq ||
          m_ref_sq < m_in_sq * threshold_sq) {
        // Probably not a chroma signal; throw it away.
      } else {
        // They're similar. Keep it!
        bo[x][0] = in_val[0];
        bo[x][1] = in_val[1];
        bo_ref[x_ref][0] = ref_val[0];
        bo_ref[x_ref][1] = ref_val[1];
      }
    }
  }

  assert(thresholdsPtr == thresholds.data() + thresholds.size());
}

void TransformPal2D::overlayFFTFrame(
    int32_t positionX, int32_t positionY,
    const std::vector<SourceField>& inputFields, int32_t fieldIndex,
    ComponentFrame& componentFrame) {
  // Do nothing if the tile isn't within the frame
  if (positionX < 0 ||
      positionX + XTILE > videoParameters.frame_width_nominal ||
      positionY < 0 ||
      positionY + YTILE >
          (2 * static_cast<int32_t>(
                   calculate_padded_field_height(videoParameters.system))) +
              1) {
    return;
  }

  // Work out which field lines to use (as the input is in frame lines)
  const SourceField& inputField = inputFields[fieldIndex];
  // Convert frame-based active area limits to field-based coordinates
  const int32_t firstFieldLine =
      (videoParameters.first_active_frame_line + 1 - inputField.getOffset()) /
      2;
  const int32_t lastFieldLine =
      (videoParameters.last_active_frame_line + 1 - inputField.getOffset()) / 2;
  const int32_t tileY = positionY / 2;
  const int32_t startY = std::max(firstFieldLine - tileY, 0);
  const int32_t endY = std::min(lastFieldLine - tileY, YTILE);

  // Compute the forward FFT
  forwardFFTTile(positionX, tileY, startY, endY, inputField);

  // Apply the frequency-domain filter
  applyFilter();

  // Create a canvas
  FrameCanvas canvas(componentFrame, videoParameters);

  // Outline the selected tile
  const auto green = canvas.rgb(0, 0xFFFF, 0);
  canvas.drawRectangle(positionX - 1, positionY + inputField.getOffset() - 1,
                       XTILE + 1, (YTILE * 2) + 1, green);

  // Draw the arrays
  overlayFFTArrays(fftComplexIn, fftComplexOut, canvas);
}
