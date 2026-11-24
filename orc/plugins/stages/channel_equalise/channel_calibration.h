/*
 * File:        channel_calibration.h
 * Module:      orc-stage-plugin-channel-equalise
 * Purpose:     Measures a PAL LaserVision source's edge smear from the VITS
 *              on field 1 line 19, for Channel Equalise
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_CHANNEL_EQUALISE_CHANNEL_CALIBRATION_H
#define ORC_STAGE_CHANNEL_EQUALISE_CHANNEL_CALIBRATION_H

#include <orc/stage/video_frame_representation.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "picture_edges.h"
#include "smear_track.h"

namespace orc {

// The smear is fitted to the long tails of the CCIR 17 white bar and 2T
// pulse on field 1 line 19, averaged over many frames with each frame's bar
// edge aligned. Only samples 0.4 us or more from an edge or the pulse are
// fitted, so the generator's own edge shaping (100-200 ns) and any group
// delay confined to the chroma band drop out, and a tail lasting
// microseconds can only be a smear. One fit per window of frames, at the
// inner, middle and outer radius; the source's smear is the median of the
// windows' accepted fits.

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

// ── Tracking the smear along a source ──────────────────────────────────────
//
// The smear is measured on line 19 of every field, the same segment the
// calibration averages, and followed along the source in windows. One
// field's bar reads its smear's tail to about 0.5 % of the bar, and the
// smear drifts with radius by about 1 % over a side, so each window averages
// the fields within |half_window_frames| of its centre: 128 frames either
// side average that noise down to under 0.05 % while the drift across them
// stays as small. A field whose segment lies far from the window's median (a
// dropout on the bar) is left out. Each window's correction must be
// certified no worse than Channel Calibration's (window_select), and so must
// the track the stage applies, interpolated between the windows' knots.

// One field's segment of line 19, as aligned_line19_average() averages
// them: from 3 us before the bar edge to 17 us after it, in IRE. std::nullopt
// when the line has no bar edge or too little line around it.
std::optional<std::vector<double>> aligned_bar_segment(
    const std::vector<double>& line_ire);

// The share of the bar that a segment's tail still lacks 3 to 8 samples
// after its rising edge. NaN when the segment has no bar (its top not above
// the level before it), and the tracking leaves that field out.
double segment_tail(const std::vector<double>& segment);

// ── The two-component fit the tracking uses ────────────────────────────────
//
// One-pole fits of the long tails leave the tail 3-8 samples after an edge
// 0.5-1 % of the bar off on every Domesday side measured, and see nothing of
// tails confined there. The tracking therefore fits both components of
// channel_equalise::EdgeSmear, so the corrected bar is flat from 3 samples
// after each of its edges: the slow component the calibration measures and
// a signed fast one for the tail near the edge.

// The corrected-bar deviations a fit minimises: at 3 to 30 samples after the
// rising edge from the bar top, then after the falling edge from the black
// after it, as fractions of the bar. Empty when the segment has no bar.
std::vector<double> edge_flatness(const std::vector<double>& segment);

struct TwoComponentFit {
  channel_equalise::EdgeSmear smear;
  // RMS of edge_flatness() after the fit's inverse, and without it.
  double residual = 0.0;
  double residual_identity = 0.0;
  // The raw tails 3 to 8 samples after the rising and the falling edge, and
  // 9 to 30 samples after them.
  double rise_tail = 0.0;
  double fall_tail = 0.0;
  double rise_long_tail = 0.0;
  double fall_long_tail = 0.0;
};

// Fits both components to an averaged segment (as aligned_bar_segment()):
// Nelder-Mead on (fast fraction, log fast tau, fraction, log tau) from five
// fixed starts, within the ranges of channel_equalise::EdgeSmear, the slow
// time constant at least 1.5 times the fast one and the inverse invertible.
std::optional<TwoComponentFit> fit_two_component(
    const std::vector<double>& segment);

// The smear's predicted share of the step still missing |first| to |last|
// samples after an ideal edge.
double predicted_tail(const channel_equalise::EdgeSmear& smear, int first,
                      int last);

// The guards. A fit is a smear when both edges of the bar carry at least
// 0.5 % of it 9 to 30 samples after them (the long smear Channel Calibration
// measures) and the fit explains the deviations clearly (residual under 0.8
// of the identity's). With |near_edge|, a tail confined near the edge is one
// too, when both edges carry at least 0.6 % of the bar 3 to 8 samples after
// them (a linear tail, not a difference between the edges) and the fit
// brings the residual under 0.7 of the identity's.
SmearVerdict judge_two_component(const std::optional<TwoComponentFit>& fit,
                                 bool near_edge);

struct ChannelTrackOptions {
  // Frames either side of a window's centre whose fields it measures.
  size_t half_window_frames = 128;
  // Frames between window centres; the track's knots are at the centres.
  size_t block_frames = 256;
  // The fewest fields a fit may average in each parity.
  size_t min_fields = 8;
  // Every this many frames, the picture's edges are found (both fields); on
  // a source of fewer than 256 times this many frames, every (frames / 256)th
  // frame, and every frame below 512, so that a short source's certifying
  // third has picture edges too.
  size_t picture_stride_frames = 32;
  // Frames either side of a window's centre whose picture edges it uses.
  size_t picture_half_window_frames = 1024;
  // The most edges kept from one field.
  size_t max_edges_per_field = 64;
  // Tolerances within which the knots of a slowly drifting smear are merged
  // (SmearTrack::simplified()); the merged track is then certified as it is
  // applied (track_channel_fields()).
  double fraction_tolerance = 0.002;
  double tau_tolerance = 0.05;
  // Also offer the tracked fit for tails confined near the edge
  // (judge_two_component()); it is still applied only where certified.
  bool near_edge = false;
};

// One field's measurement.
struct FieldSegment {
  FrameID frame = 0;
  // 0 for the first field of the frame, 1 for the second.
  unsigned field = 0;
  std::vector<float> segment;
};

// One window: the frames whose knot it sets, and what was chosen there.
struct ChannelTrackBlock {
  FrameID first = 0;
  size_t frames = 0;
  // Fields the window's fit split holds, both parities.
  size_t fields_used = 0;
  // Picture edges the window's selection split holds.
  size_t edges_used = 0;
  // "constrained", "tracked" or "upstream" (window_select::choose()).
  std::string choice;
  // The window's own correction was certified, but the track interpolated
  // through it was not, so it takes upstream's (choice "upstream").
  bool reverted = false;
  // Smear when the chosen correction corrects anything.
  SmearVerdict verdict = SmearVerdict::CannotTell;
};

// One frame's picture edges, both fields.
struct PictureSample {
  FrameID frame = 0;
  picture_edges::Edges edges;
};

// Each field parity is corrected on its own: the two fields' smears differ
// by up to 0.5 % of the bar on some captures.
struct ChannelTrack {
  std::vector<ChannelTrackBlock> blocks;
  // The first field's track, then the second's.
  SmearTrack track;
  SmearTrack track_field2;
  // Channel Calibration's smear for the source: what every window is
  // certified against, and what it falls back to.
  channel_equalise::EdgeSmear upstream;
  size_t fields_read = 0;
  size_t fields_measured = 0;
  // Picture edges found in the first field and in the second.
  std::array<size_t, 2> picture_edges_found{};
};

// The tracks from fields and picture edges already measured, in frame
// order, over the frames from |first| for |frame_count|, against
// Channel Calibration's |upstream| smear. Each window's frames are split
// three ways by frame number: those = 0 (mod 3) fit the candidates, those
// = 1 certify them (window_select::choose()); those = 2 are left for
// checking. The tracks are then certified as the stage applies them
// (simplified, through their text form and interpolated per frame) on each
// window's certifying split: against upstream's correction, and against
// the identity where the window chose a correction of its own. A window
// that fails keeps the knots around it unsimplified, and if it still fails,
// returns to upstream's correction (ChannelTrackBlock::reverted).
ChannelTrack track_channel_fields(const std::vector<FieldSegment>& fields,
                                  const std::vector<PictureSample>& pictures,
                                  FrameID first, size_t frame_count,
                                  const channel_equalise::EdgeSmear& upstream,
                                  const ChannelTrackOptions& options,
                                  const ChannelCalibrationProgress& progress);

// Measures line 19 of both fields of every frame of |source|, and the
// picture edges of every picture_stride_frames-th frame, calibrates the
// upstream smear (calibrate_channel()), and tracks the smear along the
// source. An empty result when the source is not PAL composite, or when
// cancelled.
ChannelTrack track_channel(const VideoFrameRepresentation& source,
                           const ChannelTrackOptions& options,
                           const ChannelCalibrationProgress& progress);

}  // namespace orc

#endif  // ORC_STAGE_CHANNEL_EQUALISE_CHANNEL_CALIBRATION_H
