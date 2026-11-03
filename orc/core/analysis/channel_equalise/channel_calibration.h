/*
 * File:        channel_calibration.h
 * Module:      analysis
 * Purpose:     Measures a PAL LaserVision source's edge smear from the VITS
 *              on field 1 line 19, for Channel Equalise
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_CORE_ANALYSIS_CHANNEL_CALIBRATION_H
#define ORC_CORE_ANALYSIS_CHANNEL_CALIBRATION_H

#include <orc/stage/video_frame_representation.h>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace orc {

// The smear is fitted to the long tails of the CCIR 17 white bar and 2T
// pulse on field 1 line 19, averaged over many frames with each frame's bar
// edge aligned. Only samples 0.4 us or more from an edge or the pulse are
// fitted, so the generator's own edge shaping (100-200 ns) and any group
// delay confined to the chroma band drop out, and a tail lasting
// microseconds can only be a smear. One fit per window of frames, at the
// inner, middle and outer radius; the source's smear is the median of the
// windows' accepted fits. tools/domesday2d-bench/equalise.py (fit_smear,
// accept) and phase5_vits.py are the prototype.

// One window's fit of field 1 line 19, averaged over its frames.
struct SmearFit {
  double fraction = 0.0;
  double tau_samples = 0.0;
  // RMS residual over the fitted samples, IRE: with the smear, and the best
  // fit with no smear at all.
  double residual_ire = 0.0;
  double residual_no_smear_ire = 0.0;
};

// What a fit says about the source.
enum class SmearVerdict {
  Smear,       // the fit's smear is real
  NoSmear,     // the tails are explained without one
  CannotTell,  // no usable bar, or a fit that is neither
  // A smear whose time constant is beyond what Channel Equalise accepts
  // (channel_equalise::kMaxTauSamples): the fit is reported, never applied.
  OutOfRange,
};

SmearVerdict judge_smear_fit(const std::optional<SmearFit>& fit);

// Fits the smear to one averaged segment of field 1 line 19, in IRE, that
// starts |start_us| before the bar's rising edge at 4fsc PAL. std::nullopt
// when the segment has no bar or pulse to fit.
std::optional<SmearFit> fit_smear_segment(const std::vector<double>& segment,
                                          double start_us);

// Field 1 line 19 of |frames| averaged with their bar edges aligned, from 3
// us before the edge to 17 us after it, in IRE; empty when no frame has a
// bar edge. |frames_used| counts the frames that had one.
std::vector<double> aligned_line19_average(
    const VideoFrameRepresentation& source, const std::vector<FrameID>& frames,
    size_t* frames_used);

struct ChannelCalibrationOptions {
  // Frames averaged for each fit.
  size_t window_frames = 48;
  // The inner and outer windows start this far from the source's ends, as
  // far as its length allows (a tenth of it at most).
  size_t edge_margin_frames = 1000;
};

struct ChannelWindow {
  FrameID first = 0;
  size_t frames = 0;
  size_t frames_used = 0;
  std::optional<SmearFit> fit;
  SmearVerdict verdict = SmearVerdict::CannotTell;
};

struct ChannelCalibration {
  // Smear: the median of the accepted windows; OutOfRange: none accepted and
  // at least one a smear beyond the stage's limits; NoSmear: none of either
  // and at least one read as no smear; CannotTell: no window could be read.
  SmearVerdict verdict = SmearVerdict::CannotTell;
  double fraction = 0.0;
  double tau_samples = 0.0;
  std::vector<ChannelWindow> windows;
};

// The source's verdict and smear from its windows' verdicts and fits.
ChannelCalibration combine_channel_windows(std::vector<ChannelWindow> windows);

// Reports progress (0-100) and a status; returns false to cancel, after
// which the result is empty.
using ChannelCalibrationProgress =
    std::function<bool(int percent, const std::string& status)>;

ChannelCalibration calibrate_channel(
    const VideoFrameRepresentation& source,
    const ChannelCalibrationOptions& options,
    const ChannelCalibrationProgress& progress);

}  // namespace orc

#endif  // ORC_CORE_ANALYSIS_CHANNEL_CALIBRATION_H
