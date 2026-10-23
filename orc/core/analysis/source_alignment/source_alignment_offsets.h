/*
 * File:        source_alignment_offsets.h
 * Module:      orc-core/analysis
 * Purpose:     Pad-mode alignment offsets from per-source picture numbers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace orc {

// One picture number read from a source: the frame's index from the start of
// the source, and the number its VBI carried.
struct PictureNumberReading {
  uint64_t frame_index = 0;
  int32_t picture_number = 0;
};

// The picture number the source's first frame holds, taken as the median of
// (picture number - frame index) over |readings|. A lead-in frame or a frame
// with no readable number at the head of the source therefore does not move
// it, and nor does one corrupt reading among many. nullopt when there are no
// readings.
std::optional<int32_t> picture_number_at_first_frame(
    const std::vector<PictureNumberReading>& readings);

struct PadAlignment {
  // The picture number output frame 0 holds: the earliest first picture of
  // any source. nullopt when no source has picture numbers.
  std::optional<int32_t> first_picture;
  // Leading padding per source; nullopt for a source with no picture numbers,
  // which cannot be placed and is left out of the alignment.
  std::vector<std::optional<size_t>> pads;
};

// Leading padding that places every source's first frame at its picture.
PadAlignment compute_pad_alignment(
    const std::vector<std::vector<PictureNumberReading>>& readings);

// The Source Align alignmentMap for |pads|: "1+0, 2+3", one-based, with
// excluded sources omitted so the stage leaves them out.
std::string format_alignment_map(
    const std::vector<std::optional<size_t>>& pads);

}  // namespace orc
