/*
 * File:        transformpal3d.cpp
 * Module:      orc-core
 * Purpose:     Transform PAL 3D decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018-2026 Simon Inns
 * SPDX-FileCopyrightText: 2019-2020 Adam Sampson
 */

#include "transformpal3d.h"

#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <cassert>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include <cstddef>
#include <cstring>

#include "framecanvas.h"

/*!
    \class TransformPal3D

    3D Transform PAL filter, based on Jim Easterbrook's implementation in
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

TransformPal3D::TransformPal3D() : TransformPal(XCOMPLEX, YCOMPLEX, ZCOMPLEX) {
  // Compute the window function.
  for (int32_t z = 0; z < ZTILE; z++) {
    const double windowZ = computeWindow(z, ZTILE);
    for (int32_t y = 0; y < YTILE; y++) {
      const double windowY = computeWindow(y, YTILE);
      for (int32_t x = 0; x < XTILE; x++) {
        const double windowX = computeWindow(x, XTILE);
        windowFunction[z][y][x] = windowZ * windowY * windowX;
      }
    }
  }

  // Allocate buffers for FFTW. These must be allocated using FFTW's own
  // functions so they're properly aligned for SIMD operations.
  fftReal = fftw_alloc_real(static_cast<size_t>(ZTILE) * YTILE * XTILE);
  fftComplexIn =
      fftw_alloc_complex(static_cast<size_t>(ZCOMPLEX) * YCOMPLEX * XCOMPLEX);
  fftComplexOut =
      fftw_alloc_complex(static_cast<size_t>(ZCOMPLEX) * YCOMPLEX * XCOMPLEX);

  // Plan FFTW operations
  std::lock_guard<std::mutex> lock(fftwPlannerMutex());
  forwardPlan = fftw_plan_dft_r2c_3d(ZTILE, YTILE, XTILE, fftReal, fftComplexIn,
                                     FFTW_MEASURE);
  inversePlan = fftw_plan_dft_c2r_3d(ZTILE, YTILE, XTILE, fftComplexOut,
                                     fftReal, FFTW_MEASURE);
}

TransformPal3D::~TransformPal3D() {
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

int32_t TransformPal3D::getThresholdsSize() {
  // On the X axis, include only the bins we actually use in applyFilter
  return ZCOMPLEX * YCOMPLEX * ((XCOMPLEX / 4) + 1);
}

int32_t TransformPal3D::getLookBehind() {
  // Design §8.7: In the VFrameR frame-based architecture, this decoder
  // requires one frame of look-behind context.  Internally the 3D FFT tile
  // extends up to HALFZTILE fields back; fields outside the provided range
  // are filled with blanking in forwardFFTTile().
  return 1;
}

int32_t TransformPal3D::getLookAhead() {
  // ... and at most a tile minus one bin into the future.
  return (ZTILE - 1 + 1) / 2;
}

void TransformPal3D::filterFields(const std::vector<SourceField>& inputFields,
                                  int32_t startIndex, int32_t endIndex,
                                  std::vector<const double*>& outputFields) {
  assert(configurationSet);

  // Check for YC sources - not supported by Transform PAL
  if (!inputFields.empty() && inputFields[0].is_yc) {
    ORC_LOG_ERROR(
        "TransformPal3D: YC sources are not supported. Use NTSC/Comb decoder "
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

  // Check that we've been given at least getLookBehind() frames (= 2 fields)
  // of surrounding context.  The 3D FFT tile may reach further than this;
  // forwardFFTTile() fills missing fields with blanking.
  assert(startIndex >= getLookBehind() * 2);
  assert((static_cast<int32_t>(inputFields.size()) - endIndex) >=
         getLookAhead() * 2);

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

  // Iterate through the overlapping tile positions, covering the active area.
  // (See TransformPal3D member variable documentation for how the tiling works;
  // if you change the Z tiling here, also review getLookBehind/getLookAhead
  // above.)
  for (int32_t tileZ = startIndex - HALFZTILE; tileZ < endIndex;
       tileZ += HALFZTILE) {
    for (int32_t tileY = videoParameters.first_active_frame_line - HALFYTILE;
         tileY < videoParameters.last_active_frame_line; tileY += HALFYTILE) {
      for (int32_t tileX = videoParameters.active_video_start - HALFXTILE;
           tileX < videoParameters.active_video_end; tileX += HALFXTILE) {
        // Compute the forward FFT
        forwardFFTTile(tileX, tileY, tileZ, inputFields);

        // Apply the frequency-domain filter
        applyFilter();

        // Compute the inverse FFT
        inverseFFTTile(tileX, tileY, tileZ, startIndex, endIndex);
      }
    }
  }
}

// Apply the forward FFT to an input tile, populating fftComplexIn
void TransformPal3D::forwardFFTTile(
    int32_t tileX, int32_t tileY, int32_t tileZ,
    const std::vector<SourceField>& inputFields) {
  // Work out which lines of this tile are within the active region
  const int32_t startY =
      std::max(videoParameters.first_active_frame_line - tileY, 0);
  const int32_t endY =
      std::min(videoParameters.last_active_frame_line - tileY, YTILE);

  // Copy the input signal into fftReal, applying the window function
  for (int32_t z = 0; z < ZTILE; z++) {
    const int32_t fieldIndex = tileZ + z;

    // Bounds check to prevent out-of-bounds access
    if (fieldIndex < 0 ||
        fieldIndex >= static_cast<int32_t>(inputFields.size())) {
      // Fill entire z-slice with blanking if field is out of bounds.
      // EBU Tech. 3280-E: blanking level = kPalBlanking in 10-bit domain.
      for (int32_t y = 0; y < YTILE; y++) {
        for (int32_t x = 0; x < XTILE; x++) {
          fftReal[(((z * YTILE) + y) * XTILE) + x] =
              orc::kPalBlanking * windowFunction[z][y][x];
        }
      }
      continue;
    }

    for (int32_t y = 0; y < YTILE; y++) {
      // If this frame line is not available in the field we're reading from
      // (either outside the active region, or in the other field), fill with
      // blanking instead.
      if (y < startY || y >= endY || ((tileY + y) % 2) != (fieldIndex % 2)) {
        for (int32_t x = 0; x < XTILE; x++) {
          fftReal[(((z * YTILE) + y) * XTILE) + x] =
              orc::kPalBlanking * windowFunction[z][y][x];
        }
        continue;
      }

      const int32_t fieldLine = (tileY + y) / 2;
      // Use getLine() to handle PAL non-uniform line lengths correctly.
      const int16_t* b =
          inputFields[fieldIndex].getLine(static_cast<size_t>(fieldLine));
      for (int32_t x = 0; x < XTILE; x++) {
        fftReal[(((z * YTILE) + y) * XTILE) + x] =
            b[tileX + x] * windowFunction[z][y][x];
      }
    }
  }

  // Convert time domain in fftReal to frequency domain in fftComplexIn
  fftw_execute(forwardPlan);
}

// Apply the inverse FFT to fftComplexOut, overlaying the result into chromaBuf
void TransformPal3D::inverseFFTTile(int32_t tileX, int32_t tileY, int32_t tileZ,
                                    int32_t startIndex, int32_t endIndex) {
  // Work out what portion of this tile is inside the active area
  const int32_t startX =
      std::max(videoParameters.active_video_start - tileX, 0);
  const int32_t endX =
      std::min(videoParameters.active_video_end - tileX, XTILE);
  const int32_t startY =
      std::max(videoParameters.first_active_frame_line - tileY, 0);
  const int32_t endY =
      std::min(videoParameters.last_active_frame_line - tileY, YTILE);
  const int32_t startZ = std::max(startIndex - tileZ, 0);
  const int32_t endZ = std::min(endIndex - tileZ, ZTILE);

  // Convert frequency domain in fftComplexOut back to time domain in fftReal
  fftw_execute(inversePlan);

  // Overlay the result, normalising the FFTW output, into the chroma buffers
  for (int32_t z = startZ; z < endZ; z++) {
    const int32_t outputIndex = tileZ + z - startIndex;
    double* outputPtr = chromaBuf[outputIndex].data();

    for (int32_t y = startY; y < endY; y++) {
      // If this frame line is not part of this field, ignore it.
      if (((tileY + y) % 2) != (outputIndex % 2)) {
        continue;
      }

      const int32_t outputLine = (tileY + y) / 2;
      double* b =
          outputPtr + (static_cast<ptrdiff_t>(
                          outputLine * videoParameters.frame_width_nominal));
      for (int32_t x = startX; x < endX; x++) {
        b[tileX + x] +=
            fftReal[(((z * YTILE) + y) * XTILE) + x] / (ZTILE * YTILE * XTILE);
      }
    }
  }
}

// Return the absolute value squared of an fftw_complex
static inline double fftwAbsSq(const fftw_complex& value) {
  return (value[0] * value[0]) + (value[1] * value[1]);
}

// Apply the frequency-domain filter.
void TransformPal3D::applyFilter() {
  // Get pointer to squared threshold values
  const double* thresholdsPtr = thresholds.data();

  // Clear fftComplexOut. We discard values by default; the filter only
  // copies values that look like chroma.
  for (int32_t i = 0; i < ZCOMPLEX * YCOMPLEX * XCOMPLEX; i++) {
    fftComplexOut[i][0] = 0.0;
    fftComplexOut[i][1] = 0.0;
  }

  // This is a direct translation of transform_filter from pyctools-pal, with
  // an extra loop added to extend it to 3D. The main simplification is that
  // we don't need to worry about conjugates, because FFTW only returns half
  // the result in the first place.
  //
  // The general idea is that a real modulated chroma signal will be
  // symmetrical around the U carrier, which is at fSC Hz, 72 c/aph, 18.75 Hz
  // -- and because we're sampling at 4fSC, this is handily equivalent to
  // being symmetrical around the V carrier owing to wraparound. We look at
  // every bin that might be a chroma signal, and only keep it if it's
  // sufficiently symmetrical with its reflection.
  //
  // The Z axis covers 0 to 50 Hz;      18.75 Hz is 3/8 * ZTILE.
  // The Y axis covers 0 to 576 c/aph;  72 c/aph is 1/8 * YTILE.
  // The X axis covers 0 to 4fSC Hz;    fSC HZ   is 1/4 * XTILE.

  for (int32_t z = 0; z < ZTILE; z++) {
    // Reflect around 18.75 Hz temporally.
    // XXX Why ZTILE / 4? It should be (6 * ZTILE) / 8...
    const int32_t z_ref = ((ZTILE / 4) + ZTILE - z) % ZTILE;

    for (int32_t y = 0; y < YTILE; y++) {
      // Reflect around 72 c/aph vertically.
      const int32_t y_ref = ((YTILE / 4) + YTILE - y) % YTILE;

      // Input data for this line and its reflection
      const fftw_complex* bi =
          fftComplexIn +
          (static_cast<ptrdiff_t>(((z * YCOMPLEX) + y) * XCOMPLEX));
      const fftw_complex* bi_ref =
          fftComplexIn +
          (static_cast<ptrdiff_t>(((z_ref * YCOMPLEX) + y_ref) * XCOMPLEX));

      // Output data for this line and its reflection
      fftw_complex* bo =
          fftComplexOut +
          (static_cast<ptrdiff_t>(((z * YCOMPLEX) + y) * XCOMPLEX));
      fftw_complex* bo_ref =
          fftComplexOut +
          (static_cast<ptrdiff_t>(((z_ref * YCOMPLEX) + y_ref) * XCOMPLEX));

      // We only need to look at horizontal frequencies that might be chroma
      // (0.5fSC to 1.5fSC).
      for (int32_t x = XTILE / 8; x <= XTILE / 4; x++) {
        // Reflect around fSC horizontally
        const int32_t x_ref = (XTILE / 2) - x;

        // Get the threshold for this bin
        const double threshold_sq = *thresholdsPtr++;

        const fftw_complex& in_val = bi[x];
        const fftw_complex& ref_val = bi_ref[x_ref];

        if (x == x_ref && y == y_ref && z == z_ref) {
          // This bin is its own reflection (i.e. it's a carrier). Keep it!
          bo[x][0] = in_val[0];
          bo[x][1] = in_val[1];
          continue;
        }

        // Get the squares of the magnitudes (to minimise the number of sqrts)
        const double m_in_sq = fftwAbsSq(in_val);
        const double m_ref_sq = fftwAbsSq(ref_val);

        // Compare the magnitudes of the two values, and discard
        // both if they are more different than the threshold for
        // this bin.
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
  }

  assert(thresholdsPtr == thresholds.data() + thresholds.size());
}

void TransformPal3D::overlayFFTFrame(
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

  // Compute the forward FFT
  forwardFFTTile(positionX, positionY, fieldIndex, inputFields);

  // Apply the frequency-domain filter
  applyFilter();

  // Create a canvas
  FrameCanvas canvas(componentFrame, videoParameters);

  // Outline the selected tile
  const auto green = canvas.rgb(0, 0xFFFF, 0);
  canvas.drawRectangle(positionX - 1, positionY - 1, XTILE + 1, YTILE + 1,
                       green);

  // Draw the arrays
  overlayFFTArrays(fftComplexIn, fftComplexOut, canvas);
}
