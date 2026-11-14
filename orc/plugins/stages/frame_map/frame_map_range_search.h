/*
 * File:        frame_map_range_search.h
 * Module:      orc-stage-plugin-frame-map
 * Purpose:     Binary-chop frame search over monotonic VBI picture numbers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#ifndef ORC_STAGE_FRAME_MAP_FRAME_MAP_RANGE_SEARCH_H
#define ORC_STAGE_FRAME_MAP_FRAME_MAP_RANGE_SEARCH_H

#include <cstdint>
#include <functional>
#include <optional>

namespace orc::frame_map_range {

// Reads the VBI picture number for a frame; returns nullopt when the frame
// has no decodable VBI (dropout, lead-in/lead-out, damaged field). Probing a
// frame is expensive (full biphase decode), so searches minimise probe count.
using PictureNumberProbe = std::function<std::optional<int32_t>(int64_t frame)>;

// Polled between probes; return true to abort the search.
using CancelCheck = std::function<bool()>;

enum class SearchStatus { Found, NotFound, Cancelled };

struct SearchOutcome {
  SearchStatus status = SearchStatus::NotFound;
  int64_t frame = 0;
};

// Both searches assume picture numbers are monotonically non-decreasing
// across the frame range (true for CAV picture numbers and CLV timecodes on
// a normal capture) and complete in O(log n) probes plus a short linear
// refinement. Unreadable frames between probe points are skipped. Callers
// should fall back to a sequential scan on NotFound when the source may be
// non-monotonic (e.g. the player skipped backwards during capture).

// First frame in [first_frame, last_frame] whose picture number equals
// target. Thread-safety: stateless; safe if probe/cancelled are safe.
SearchOutcome find_first_frame_with_picture(int64_t first_frame,
                                            int64_t last_frame, int32_t target,
                                            const PictureNumberProbe& probe,
                                            const CancelCheck& cancelled);

// Last frame in [first_frame, last_frame] whose picture number equals
// target. Thread-safety: stateless; safe if probe/cancelled are safe.
SearchOutcome find_last_frame_with_picture(int64_t first_frame,
                                           int64_t last_frame, int32_t target,
                                           const PictureNumberProbe& probe,
                                           const CancelCheck& cancelled);

}  // namespace orc::frame_map_range

#endif  // ORC_STAGE_FRAME_MAP_FRAME_MAP_RANGE_SEARCH_H
