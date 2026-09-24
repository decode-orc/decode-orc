/*
 * File:        transformpal.h
 * Module:      orc-core
 * Purpose:     Transform PAL base decoder
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2018 William Andrew Steer
 * SPDX-FileCopyrightText: 2018-2026 Simon Inns
 * SPDX-FileCopyrightText: 2019-2021 Adam Sampson
 */

#ifndef TRANSFORMPAL_H
#define TRANSFORMPAL_H

#include <fftw3.h>
#include <orc/stage/orc_source_parameters.h>

#include <mutex>

#include "componentframe.h"
#include "framecanvas.h"
#include "outputwriter.h"
#include "sourcefield.h"

// Abstract base class for Transform PAL filters.
class TransformPal {
 public:
  TransformPal(int32_t xComplex, int32_t yComplex, int32_t zComplex);
  virtual ~TransformPal();

  // Configure TransformPal.
  //
  // threshold is the similarity threshold for the filter. Values from 0-1
  // are meaningful, with higher values requiring signals to be more similar
  // to be considered chroma.
  void updateConfiguration(const ::orc::SourceParameters& videoParameters,
                           double threshold,
                           const std::vector<double>& thresholds);

  // Filter input fields.
  //
  // For each input frame between startFieldIndex and endFieldIndex, a
  // pointer will be placed in outputFields to an array of the same size
  // (owned by this object) containing the chroma signal.
  virtual void filterFields(const std::vector<SourceField>& inputFields,
                            int32_t startIndex, int32_t endIndex,
                            std::vector<const double*>& outputFields) = 0;

  // Draw a visualisation of the FFT over component frames.
  //
  // The FFT is computed for each field, so this visualises only the first
  // field in each frame. positionX/Y specify the location to visualise in
  // frame coordinates.
  void overlayFFT(int32_t positionX, int32_t positionY,
                  const std::vector<SourceField>& inputFields,
                  int32_t startIndex, int32_t endIndex,
                  std::vector<ComponentFrame>& componentFrames);

 protected:
  // FFTW's planner is not thread-safe: fftw_plan_* and fftw_destroy_plan()
  // must never run concurrently anywhere in the process (only fftw_execute()
  // is safe). Every subclass creates and destroys its plans under this lock,
  // so decoders built by different sinks running side by side stay safe.
  static std::mutex& fftwPlannerMutex();

  // Overlay a visualisation of one field's FFT.
  // Calls back to overlayFFTArrays to draw the arrays.
  virtual void overlayFFTFrame(int32_t positionX, int32_t positionY,
                               const std::vector<SourceField>& inputFields,
                               int32_t fieldIndex,
                               ComponentFrame& componentFrame) = 0;

  void overlayFFTArrays(const fftw_complex* fftIn, const fftw_complex* fftOut,
                        FrameCanvas& canvas);

  // FFT size
  int32_t xComplex;
  int32_t yComplex;
  int32_t zComplex;

  // Configuration parameters
  bool configurationSet;
  ::orc::SourceParameters videoParameters;
  std::vector<double> thresholds;
};

#endif
