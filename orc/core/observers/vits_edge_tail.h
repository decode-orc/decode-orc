/*
 * File:        vits_edge_tail.h
 * Module:      orc-core
 * Purpose:     Measure the slow tail of the VITS white bar's edges
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace orc {

// The slow part of an edge: most of a step arrives at once and the rest,
// |fraction| of it, settles exponentially with |time_constant_us|.  dp111's
// domesday-aiv-restore measured about 12 % over 0.4 µs on every edge of its
// Community South captures, and corrected it with fixed constants; how much
// it varies between players and decodes decides whether a correction belongs
// in decode-orc at all.
struct EdgeTail {
  // Of the step, at its 50 % crossing.
  double fraction = 0.0;
  // nullopt when the tail is too small for its time constant to mean
  // anything.
  std::optional<double> time_constant_us;
  // Lines averaged before the fit.
  size_t lines = 0;
};

struct EdgeTailMeasurement {
  std::optional<EdgeTail> rising;
  std::optional<EdgeTail> falling;
};

// A fraction below which an edge's time constant is not reported.
inline constexpr double kMinReportedEdgeTail = 0.01;

// Averages the white bar's rising and falling edges over a capture's frames,
// then fits the tail on the average: one frame's edge is too noisy to fit.
// The bar is the one WhiteSNRObserver measures: PAL line 19 (UK ITS, 12–22
// µs, then black to the 2T pulse at 26 µs) and NTSC or PAL-M line 20 (NTC-7
// composite, 12–30 µs, then black to the 2T pulse at 34 µs).  A line whose
// levels are not a bar's (no VITS, or a dropout across it) is passed over.
class VitsEdgeTailMeter {
 public:
  // |blanking| and |white| are the source's levels, in codes.
  VitsEdgeTailMeter(VideoSystem system, size_t nominal_width, double blanking,
                    double white);

  // A frame as stored, lines at frame_line_sample_offset(); both fields'
  // bar lines are read.
  void add_frame(const std::vector<int16_t>& plane, size_t height);

  // The frame line, from 0, that carries the bar in |field| (0 or 1), and one
  // such line on its own, from 0H, for a caller reading lines rather than
  // frames.
  size_t bar_line(size_t field) const;
  void add_line(const int16_t* line, size_t length);

  EdgeTailMeasurement measure() const;

 private:
  struct Edge {
    double nominal_us = 0.0;  // nominal 50 % point
    double level_from = 0.0;  // start of the window the level before it is
    double level_to = 0.0;    // measured in, and its end
    double fit_to = 0.0;      // end of the tail fit
    bool rising = true;
    std::vector<double> sum;  // samples from level_from, summed
    size_t lines = 0;
  };

  std::optional<EdgeTail> fit(const Edge& edge) const;
  size_t sample_at(double us) const;

  VideoSystem system_;
  size_t nominal_width_;
  double samples_per_us_;
  double blanking_;
  double white_;
  size_t field_line_;  // 1-based, as WhiteSNRObserver counts
  Edge rising_;
  Edge falling_;
};

}  // namespace orc
