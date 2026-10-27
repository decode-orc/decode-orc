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
#include <orc_analysis.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
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
  // compared.  When the pressing model does not explain the sources'
  // differences (fit.trusted false), σ is the source's spread against the
  // others instead, as the Stacker then uses.
  std::vector<std::optional<PressingNoise>> noise;
  // How well the pressing model explains the pair noise.
  PressingFitCheck fit;
  // Each source's luma gain and level against the picture common to every
  // source; nullopt where the frames carried too little picture.
  std::vector<std::optional<LevelMatch>> levels;
  // Each source's response at 5–6 MHz against the typical source's; nullopt
  // with fewer than 3 sources compared, or when the pressing model is not
  // trusted, as the response is measured between sources whose noise does
  // not correlate and only the pressings and families say which those are.
  std::vector<std::optional<double>> response;
  // Number of pressing groups among the compared sources, and of the stamper
  // families those fall into (as many as the pressings when no two share a
  // mother).
  size_t pressings = 0;
  size_t families = 0;
  // The output frames each source covers, first and last, numbered from 0;
  // nullopt for a source with no start or no frames.
  std::vector<std::optional<std::pair<int64_t, int64_t>>> coverage;
  // The padding frames within each source's coverage: pictures the capture
  // lacks, which its frame map put placeholders in for.
  std::vector<size_t> padding;
};

// Compare |sources| on up to 8 frames of the same picture.  Source i holds
// output frame k at frame |starts[i]| + k; a source with no start takes no
// part.  Frames are probed across the stretch every taking part covers.
CaptureComparison compare_captures(
    const std::vector<const VideoFrameRepresentation*>& sources,
    const std::vector<std::optional<int64_t>>& starts);

// Padding frames within which two sources covering the same frames count as
// equally padded when a reference is suggested.  A frame or two of padding
// says nothing about a capture; its noise does.
inline constexpr size_t kReferencePaddingTolerance = 10;

// Stack σ, in codes, within which two sources count as adding the same: the
// summary gives σ to two places, and what it cannot tell apart should not
// decide.
inline constexpr double kReferenceNoiseTolerance = 0.01;

// The sources that could go first among a Stacker's inputs, whose sample grid
// the stack is made on, best first; sources covering no frame are left out.
// Each place goes to a source covering the most output frames of those left;
// of those, one with no more than kReferencePaddingTolerance padding frames
// over the fewest; of those, one within kReferenceNoiseTolerance of adding
// the most to the stack (whose absence would leave it noisiest; a source not
// compared adds nothing measured); then the one whose sample grid is nearest
// the median of the sources'; then the fewest padding frames; then the one
// adding the most.
std::vector<size_t> rank_references(const CaptureComparison& comparison);

// The first of rank_references(), or nullopt when no source covers a frame.
std::optional<size_t> suggest_reference(const CaptureComparison& comparison);

// Gain or level against the common picture beyond which a source's level is
// reported: 1 % and 2 codes.
inline constexpr double kReportedGainDifference = 0.01;
inline constexpr double kReportedLevelDifference = 2.0;

// One source as the report describes it: its line of the summary, without
// the indent, and the same figures as data for a script (the keys are listed
// in docs/cli-user-guide/multi-disc-stacking.md).
struct CaptureSourceReport {
  std::string line;
  std::map<std::string, StatisticValue> data;
};

// The report text for |comparison|, sources numbered from 1, the warnings it
// raises (soft sources), one per line, each source's line and data, and
// statistics of the whole comparison.
struct CaptureComparisonReport {
  std::string summary;
  std::vector<std::string> warnings;
  std::vector<CaptureSourceReport> sources;
  std::map<std::string, StatisticValue> statistics;
};
CaptureComparisonReport describe_capture_comparison(
    const CaptureComparison& comparison);

}  // namespace orc
