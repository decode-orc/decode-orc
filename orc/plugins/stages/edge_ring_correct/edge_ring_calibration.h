/*
 * File:        edge_ring_calibration.h
 * Module:      orc-stage-plugin-edge-ring-correct
 * Purpose:     Edge Ring Calibration: fits the edge-ring templates window by
 *              window along a source and keeps only the windows whose
 *              correction is certified better on frames the fit did not see
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_EDGE_RING_CORRECT_EDGE_RING_CALIBRATION_H
#define ORC_STAGE_EDGE_RING_CORRECT_EDGE_RING_CALIBRATION_H

#include <orc/stage/video_frame_representation.h>

#include <functional>
#include <string>
#include <vector>

#include "edge_ring_certify.h"
#include "edge_ring_track.h"

namespace orc {

struct EdgeRingCalibrationOptions {
  // Windows tile the source, about this many frames each.
  size_t window_frames = 1024;
  // Each window's frames are split by frame number mod 3 into a fit third, a
  // certifying third and a reporting third. Of every probe_stride-th frame
  // of a third, the frames_per_third with the most measurable features, at
  // least min_spacing frames apart, are read.
  size_t frames_per_third = 18;
  // the fit third reads this many (rising edges are sparse on maps)
  size_t fit_frames = 24;
  size_t probe_stride = 8;
  size_t min_spacing = 6;
};

struct EdgeRingWindowReport {
  int64_t first_frame = 0;
  int64_t end_frame = 0;
  bool fitted = false;
  bool accepted = false;
  edge_ring::WindowCorrection chosen;
  // Why the candidates tried were refused (the first one's, when none was
  // certified).
  std::vector<std::string> reasons;
  // On the reporting third, which neither the fit nor the certificate saw:
  // the chosen correction (identity when none) against identity.
  edge_ring::Comparison held_out;
};

struct EdgeRingCalibration {
  edge_ring::RingTrack track;
  std::vector<EdgeRingWindowReport> windows;
};

using EdgeRingCalibrationProgress =
    std::function<bool(int percent, const std::string& status)>;

EdgeRingCalibration calibrate_edge_ring(
    const VideoFrameRepresentation& source,
    const EdgeRingCalibrationOptions& options,
    const EdgeRingCalibrationProgress& progress);

// A window's fit third, measured: over all its frames, over its line-art
// frames, and how many of those there are.
struct EdgeRingFitSums {
  edge_ring::ProfileSums all;
  edge_ring::ProfileSums line_art;
  int line_art_frames = 0;
};

// |fields| in frame pairs (field 1, field 2).
EdgeRingFitSums fit_sums(const std::vector<edge_ring::FieldImage>& fields);

// One window of frames [first, end): fitted on |fit| with the source's
// pooled rising |levels|, certified on |certify_fields| and reported on
// |report_fields| (each a list of frame pairs).
EdgeRingWindowReport calibrate_window(
    int64_t first, int64_t end, const EdgeRingFitSums& fit,
    std::shared_ptr<const edge_ring::LevelTables> levels,
    std::vector<edge_ring::CertifyField> certify_fields,
    std::vector<edge_ring::CertifyField> report_fields);

// The same, fitted (level templates included) on |fit_fields| alone.
EdgeRingWindowReport calibrate_window(
    int64_t first, int64_t end,
    const std::vector<edge_ring::FieldImage>& fit_fields,
    std::vector<edge_ring::CertifyField> certify_fields,
    std::vector<edge_ring::CertifyField> report_fields);

}  // namespace orc

#endif  // ORC_STAGE_EDGE_RING_CORRECT_EDGE_RING_CALIBRATION_H
