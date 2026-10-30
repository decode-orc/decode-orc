/*
 * File:        content_offsets.h
 * Module:      orc-core/analysis
 * Purpose:     Check aligned captures' picture numbers against their content
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/video_frame_representation.h>
#include <orc_analysis.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace orc {

// The largest content offset looked for, in frames either way.
inline constexpr int32_t kMaxContentOffset = 2;

// Windows of output frames the check probes, spread across the reference's
// coverage.
inline constexpr size_t kContentWindows = 24;

// A stretch of output frames over which one source's content sits off the
// others' picture numbers.  Source Align places frames by the picture
// numbers their VBI carries, so a capture whose numbers are off by one holds,
// at each output frame, the neighbouring picture: stacked, it blends that
// picture in, with its chroma in another phase (domesday-aiv-restore found
// Countryside side 1 DS3 so).
struct ContentOffsetSpan {
  // Output frames, from 0, inclusive.
  int64_t first = 0;
  int64_t last = 0;
  // Where the change from the stretch before may already have happened: the
  // last frame known to hold the earlier offset, plus one.  Equal to |first|
  // when the change was found to the frame.
  int64_t earliest_first = 0;
  // The source labels its pictures this many frames higher than the others
  // (its frame at output frame k + offset holds the picture they hold at
  // k); nullopt when none of its frames within kMaxContentOffset holds the
  // others' picture.
  std::optional<int32_t> offset;
};

struct ContentOffsets {
  // Per source, the stretches where it differs from the others; empty for a
  // source that agrees, or that could not be checked.
  std::vector<std::vector<ContentOffsetSpan>> spans;
  // Per source, the output frames it was checked over, first and last;
  // nullopt where it was not.
  std::vector<std::optional<std::pair<int64_t, int64_t>>> checked;
  // The source the others were compared with: the one covering the most
  // output frames.
  std::optional<size_t> reference;
  // Windows probed, and those holding a frame whose picture differs from its
  // neighbours', which alone can show an offset.
  size_t windows = 0;
  size_t windows_with_motion = 0;
  bool cancelled = false;
};

// Check |sources| (source i holds output frame k at frame |starts[i]| + k; a
// source with no start takes no part) for content that sits off their
// picture numbers.  |cancelled|, when given, is polled between probes.
ContentOffsets check_content_offsets(
    const std::vector<const VideoFrameRepresentation*>& sources,
    const std::vector<std::optional<int64_t>>& starts,
    const std::function<bool()>& cancelled = {});

// The source's offset when one span covers every frame it was checked over;
// a correction to its alignment then fixes it.
std::optional<int32_t> whole_content_offset(const ContentOffsets& offsets,
                                            size_t source);

// The report text, one warning per source that differs, and each source's
// figures as data (keys "content_offset" and "content_offset_spans").
struct ContentOffsetReport {
  std::string summary;
  std::vector<std::string> warnings;
  std::vector<std::map<std::string, StatisticValue>> sources;
};
ContentOffsetReport describe_content_offsets(const ContentOffsets& offsets);

}  // namespace orc
