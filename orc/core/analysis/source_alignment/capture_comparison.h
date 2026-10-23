/*
 * File:        capture_comparison.h
 * Module:      orc-core/analysis
 * Purpose:     Compare aligned captures of one disc: pressing groups, noise,
 *              and high-frequency response
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/frame_id.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/stack_measurement.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace orc {

// Response at 5–6 MHz, as a fraction of the typical source's, below which a
// source is reported as soft.  The study's soft captures measured 0.41 and
// 0.58 against about 1.0 for the rest.
inline constexpr double kSoftResponse = 0.75;

// What comparing the captures found.  Every vector has one entry per source.
struct CaptureComparison {
  // Frames of the same picture the comparison was made on.
  size_t frames = 0;
  // Each source's sample grid against the first compared source.
  std::vector<std::optional<int32_t>> grid_offsets;
  // Each source's noise and pressing group; nullopt when it could not be
  // compared.
  std::vector<std::optional<PressingNoise>> noise;
  // Each source's response at 5–6 MHz against the typical source's; nullopt
  // with fewer than 3 sources compared.
  std::vector<std::optional<double>> response;
  // Number of pressing groups among the compared sources.
  size_t pressings = 0;
};

// Compare |sources| on up to 8 frames of the same picture.  Source i holds
// output frame k at frame |starts[i]| + k; a source with no start takes no
// part.  Frames are probed across the stretch every taking part covers.
CaptureComparison compare_captures(
    const std::vector<const VideoFrameRepresentation*>& sources,
    const std::vector<std::optional<int64_t>>& starts);

// The report text for |comparison|, sources numbered from 1, and the warnings
// it raises (soft sources), one per line.
struct CaptureComparisonReport {
  std::string summary;
  std::vector<std::string> warnings;
};
CaptureComparisonReport describe_capture_comparison(
    const CaptureComparison& comparison);

}  // namespace orc
