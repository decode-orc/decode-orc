/*
 * File:        efm_confidence_stack.h
 * Module:      orc-core
 * Purpose:     Sync-anchored confidence-weighted combining of EFM t-values
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace orc {

// ============================================================================
// Sync-anchored confidence stacking of EFM t-value streams
// ============================================================================
//
// EFM t-values are run lengths, not samples on a shared time axis, so two
// captures of the same disc cannot be combined by sample index: one spurious
// or missed transition shifts every following index in that capture, after
// which an index-wise mean or median is averaging unrelated runs. A whole
// video frame carries around 36,600 t-values, so a single early insertion
// spoils the rest of the frame.
//
// What two captures do share is the channel bit axis. An inserted transition
// splits one run into two that sum to the same length, and a missed transition
// merges two runs into one of the same length, so neither moves the bit
// position of any later transition. Only a genuinely mis-measured run length
// shifts what follows, and IEC 60908 gives a grid that re-anchors that every
// 588 bits: the T11+T11 frame sync. Measured over 130,422 sync intervals of a
// Domesday side, 99.95% are exactly 588 bits and 99.96% an exact multiple.
//
// So this combiner works on channel frames, not on the whole stream:
//
//   1. Each source's t-values are cut on its own detected T11+T11 syncs into
//      588-bit channel frames, and each is given a slot number from its bit
//      distance to that source's first sync. Sources are then aligned to the
//      reference source by a single whole-slot offset, which absorbs a source
//      that missed its own first sync.
//   2. Within a slot, sources vote on where the transitions are. A source
//      votes for a bit offset it has a transition at, and against an offset
//      that falls strictly inside one of its runs, each with a weight taken
//      from the producer's doubt nibble. An offset is accepted when the votes
//      for it outweigh the votes against.
//   3. The accepted offsets are repaired to legal T3-T11 run lengths and
//      emitted as t-values, each carrying a doubt derived from how strongly
//      its bounding transitions won their votes and from how much the sources
//      that reported it doubted it.
//
// A stacked t-value is therefore always one that some source actually
// reported, never a synthesised average of two readings that disagree, and it
// arrives downstream with an honest doubt that the CIRC erasure machinery can
// use.
//
// Regions no slot covers - the partial channel frame before the first sync,
// the one after the last, and any stretch where the sync grid breaks down -
// are copied from the reference source unchanged, doubt included.

// Combine the packed EFM bytes of two or more captures of one video frame.
//
// |sources| holds one packed byte stream per contributing capture (t-value in
// the low nibble, producer doubt in the high nibble; see
// orc/stage/video_frame_representation.h). |reference| indexes the source to
// fall back on where voting cannot apply; it is clamped into range.
//
// Returns the combined packed byte stream. With fewer than two sources the
// single source is returned unchanged - nothing was stacked, so its doubt
// still stands.
//
// Complexity is O(T * S) in the total t-value count T and source count S.
// Thread-safe: holds no state.
std::vector<uint8_t> stack_efm_confidence(
    const std::vector<std::vector<uint8_t>>& sources, size_t reference);

}  // namespace orc
