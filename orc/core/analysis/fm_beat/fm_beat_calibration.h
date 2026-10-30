/*
 * File:        fm_beat_calibration.h
 * Module:      analysis
 * Purpose:     Measures a PAL LaserDisc master's FM carrier at black from
 *              the beat FM Beat Cancel removes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_ANALYSIS_FM_BEAT_CALIBRATION_H
#define ORC_CORE_ANALYSIS_FM_BEAT_CALIBRATION_H

#include <orc/stage/video_frame_representation.h>

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace orc {

struct FmBeatCalibrationOptions {
  // Frames read, spread evenly over the source, to find flat colour.
  size_t probe_frames = 300;
  // The fields with the most flat colour that the carrier is searched on.
  size_t max_fields = 24;
  // The search: a coarse sweep over the stage's range, then a fine one
  // around its best, with the beat predicted at that best.
  double min_carrier_hz = 6.9e6;
  double max_carrier_hz = 7.3e6;
  double coarse_step_hz = 2.0e3;
  double fine_span_hz = 3.0e3;
  double fine_step_hz = 250.0;
};

struct FmBeatCalibration {
  // Whether the source carries a beat: the best carrier corrects lines.
  bool found = false;
  double carrier_black_hz = 0.0;
  size_t probed_frames = 0;
  // Fields searched (those with enough flat colour, up to max_fields).
  size_t fields = 0;
  // Over the fields searched, at the best carrier: picture lines, those
  // corrected, and the RMS of what is removed over the corrected lines.
  size_t picture_lines = 0;
  size_t lines_corrected = 0;
  double removed_rms_ire = 0.0;
  // The coarse sweep: carrier and energy removed, IRE^2.
  std::vector<std::pair<double, double>> sweep;
};

// Reports progress (0-100) and a status; returns false to cancel, after
// which the result is empty.
using FmBeatCalibrationProgress =
    std::function<bool(int percent, const std::string& status)>;

FmBeatCalibration calibrate_fm_beat(const VideoFrameRepresentation& source,
                                    const FmBeatCalibrationOptions& options,
                                    const FmBeatCalibrationProgress& progress);

}  // namespace orc

#endif  // ORC_CORE_ANALYSIS_FM_BEAT_CALIBRATION_H
