/*
 * File:        domesday2d_calibration.h
 * Module:      analysis
 * Purpose:     Measures a PAL source's line-alternating chroma matrix, the
 *              per-source correction the Domesday2D decoder applies
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_ANALYSIS_DOMESDAY2D_CALIBRATION_H
#define ORC_CORE_ANALYSIS_DOMESDAY2D_CALIBRATION_H

#include <orc/stage/video_frame_representation.h>
#include <orc/support/domesday2d.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace orc {

// A row of V-switch s decodes as W_s = (I + s M) W (orc/support/domesday2d.h).
// M is fitted from flat colour on field-adjacent rows of frames spread over
// the source, pooled: one frame's fit is fooled by fine colour stripes one
// field line high. Each frame's rows are demodulated straight from the
// composite: on flat colour, where the pairs are taken, the luma has nothing
// at the subcarrier, and the pooled matrix agrees with the one taken after
// the decoder's separation to 0.002 on the Domesday2D bench.
// tools/domesday2d-bench/hanover.py (flat_pairs, fit_matrix) is the
// prototype.

struct Domesday2DCalibrationOptions {
  // Frames read, spread evenly over the source.
  size_t frames = 200;
  // Flat-colour pairs the fit needs.
  size_t minimum_pairs = 1000;
};

struct Domesday2DCalibration {
  size_t frames_read = 0;
  // Pairs found, and those the fit kept.
  size_t pairs = 0;
  size_t pairs_used = 0;
  // std::nullopt when the source has too little flat colour.
  std::optional<domesday2d::Matrix> matrix;
};

// Reports progress (0-100) and a status; returns false to cancel, after
// which the result is empty.
using Domesday2DCalibrationProgress =
    std::function<bool(int percent, const std::string& status)>;

Domesday2DCalibration calibrate_domesday2d(
    const VideoFrameRepresentation& source,
    const Domesday2DCalibrationOptions& options,
    const Domesday2DCalibrationProgress& progress);

}  // namespace orc

#endif  // ORC_CORE_ANALYSIS_DOMESDAY2D_CALIBRATION_H
