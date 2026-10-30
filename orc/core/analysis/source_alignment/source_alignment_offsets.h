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
  // The picture number output frame 0 holds, for the report: the earliest
  // first picture of the sources that start there, taken back from their
  // first readings a picture per frame. nullopt when no source has picture
  // numbers.
  std::optional<int32_t> first_picture;
  // Leading padding per source; nullopt for a source with no picture numbers,
  // which cannot be placed and is left out of the alignment.
  std::vector<std::optional<size_t>> pads;
};

// Leading padding that places every source's first frame at its picture.
// Sources are placed in frames, by the frame indices at which they read the
// same pictures, because a picture is not always a frame (film with 3:2
// pulldown carries four pictures in five frames). A source that shares no
// pictures with any other is placed by its picture numbers, a picture per
// frame.
PadAlignment compute_pad_alignment(
    const std::vector<std::vector<PictureNumberReading>>& readings);

// The Source Align alignmentMap for |pads|: "1+0, 2+3", one-based, with
// excluded sources omitted so the stage leaves them out.
std::string format_alignment_map(
    const std::vector<std::optional<size_t>>& pads);

// The pads that move each source by |shifts| (one per source, frames): its
// frame that sat at output frame k + shift then sits at k, so a source whose
// pictures are labelled |shift| higher than the others' lines up with them.
// When a pad would fall below zero every source is padded that much more.
std::vector<std::optional<size_t>> shift_pads(
    const std::vector<std::optional<size_t>>& pads,
    const std::vector<int32_t>& shifts);

}  // namespace orc
