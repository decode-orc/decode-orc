/*
 * File:        source_alignment_offsets.cpp
 * Module:      orc-core/analysis
 * Purpose:     Pad-mode alignment offsets from per-source picture numbers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "source_alignment_offsets.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <sstream>

namespace orc {

std::optional<int32_t> picture_number_at_first_frame(
    const std::vector<PictureNumberReading>& readings) {
  if (readings.empty()) return std::nullopt;

  std::vector<int64_t> anchors;
  anchors.reserve(readings.size());
  for (const auto& r : readings) {
    anchors.push_back(static_cast<int64_t>(r.picture_number) -
                      static_cast<int64_t>(r.frame_index));
  }
  // The lower median: an actual reading's anchor, never a value between two.
  const auto mid =
      anchors.begin() + static_cast<std::ptrdiff_t>((anchors.size() - 1) / 2);
  std::nth_element(anchors.begin(), mid, anchors.end());
  return static_cast<int32_t>(*mid);
}

namespace {

// Two sources are placed against each other by the pictures both read only
// when they share at least this many, so that one misread number cannot
// place a source on its own.
constexpr size_t kMinSharedPictures = 3;

// Each picture's frame index in |readings|, the first where it repeats.
std::map<int32_t, int64_t> frames_by_picture(
    const std::vector<PictureNumberReading>& readings) {
  std::map<int32_t, int64_t> frames;
  for (const auto& r : readings) {
    frames.emplace(r.picture_number, static_cast<int64_t>(r.frame_index));
  }
  return frames;
}

struct SharedOffset {
  int64_t frames = 0;
  size_t pictures = 0;
};

// Where source b's frame 0 sits against source a's: the lower median, over
// the pictures both read, of a's frame index less b's.
std::optional<SharedOffset> shared_picture_offset(
    const std::map<int32_t, int64_t>& a, const std::map<int32_t, int64_t>& b) {
  std::vector<int64_t> offsets;
  for (const auto& [picture, frame] : b) {
    const auto it = a.find(picture);
    if (it != a.end()) offsets.push_back(it->second - frame);
  }
  if (offsets.size() < kMinSharedPictures) return std::nullopt;
  const auto mid =
      offsets.begin() + static_cast<std::ptrdiff_t>((offsets.size() - 1) / 2);
  std::nth_element(offsets.begin(), mid, offsets.end());
  return SharedOffset{*mid, offsets.size()};
}

}  // namespace

PadAlignment compute_pad_alignment(
    const std::vector<std::vector<PictureNumberReading>>& readings) {
  // Sources are placed in frames, by the frames at which they read the same
  // pictures. A picture is not always a frame: film with 3:2 pulldown carries
  // four pictures in five frames, so a source starting eight pictures later
  // starts ten frames later. Only a source that shares no pictures with any
  // placed one is placed by its picture numbers alone, which takes a picture
  // per frame.
  PadAlignment alignment;
  const size_t count = readings.size();
  alignment.pads.resize(count);

  std::vector<std::map<int32_t, int64_t>> frames;
  std::vector<std::optional<int32_t>> firsts;
  std::optional<size_t> reference;
  for (size_t i = 0; i < count; ++i) {
    frames.push_back(frames_by_picture(readings[i]));
    firsts.push_back(picture_number_at_first_frame(readings[i]));
    if (firsts[i] &&
        (!reference || readings[i].size() > readings[*reference].size())) {
      reference = i;
    }
  }
  if (!reference) return alignment;

  // Where each source's frame 0 sits, against the reference's.
  std::vector<std::optional<int64_t>> positions(count);
  positions[*reference] = 0;
  for (;;) {
    std::optional<size_t> next;
    SharedOffset best;
    for (size_t b = 0; b < count; ++b) {
      if (positions[b] || !firsts[b]) continue;
      for (size_t a = 0; a < count; ++a) {
        if (!positions[a]) continue;
        const auto offset = shared_picture_offset(frames[a], frames[b]);
        if (offset && offset->pictures > best.pictures) {
          best = SharedOffset{*positions[a] + offset->frames, offset->pictures};
          next = b;
        }
      }
    }
    if (next) {
      positions[*next] = best.frames;
      continue;
    }
    for (size_t b = 0; b < count; ++b) {
      if (positions[b] || !firsts[b]) continue;
      next = b;
      positions[b] = *firsts[b] - *firsts[*reference];
      break;
    }
    if (!next) break;
  }

  int64_t earliest = 0;
  bool any = false;
  for (const auto& position : positions) {
    if (position && (!any || *position < earliest)) earliest = *position;
    any = any || position.has_value();
  }

  // Output frame 0's picture, for the report: from the first three readings
  // of each source that starts there, taken back to its frame 0 a picture per
  // frame. Three readings span one pulldown frame at most, and one misread
  // number among them is outvoted.
  constexpr size_t kLabelReadings = 3;
  for (size_t i = 0; i < count; ++i) {
    if (!positions[i]) continue;
    const size_t pad = static_cast<size_t>(*positions[i] - earliest);
    alignment.pads[i] = pad;
    if (pad != 0) continue;
    const std::vector<PictureNumberReading> head(
        readings[i].begin(),
        readings[i].begin() + static_cast<std::ptrdiff_t>(std::min(
                                  kLabelReadings, readings[i].size())));
    const auto first = picture_number_at_first_frame(head);
    if (first &&
        (!alignment.first_picture || *first < *alignment.first_picture)) {
      alignment.first_picture = first;
    }
  }
  return alignment;
}

std::string format_alignment_map(
    const std::vector<std::optional<size_t>>& pads) {
  std::ostringstream map;
  bool first = true;
  for (size_t i = 0; i < pads.size(); ++i) {
    const std::optional<size_t> pad = pads[i];
    if (!pad) continue;
    if (!first) map << ", ";
    first = false;
    map << (i + 1) << "+" << *pad;
  }
  return map.str();
}

std::vector<std::optional<size_t>> shift_pads(
    const std::vector<std::optional<size_t>>& pads,
    const std::vector<int32_t>& shifts) {
  std::vector<int64_t> moved(pads.size(), 0);
  int64_t lowest = 0;
  for (size_t i = 0; i < pads.size(); ++i) {
    const std::optional<size_t> pad = pads[i];
    if (!pad) continue;
    const int32_t shift = i < shifts.size() ? shifts[i] : 0;
    moved[i] = static_cast<int64_t>(*pad) - shift;
    lowest = std::min(lowest, moved[i]);
  }
  std::vector<std::optional<size_t>> shifted(pads.size());
  for (size_t i = 0; i < pads.size(); ++i) {
    if (pads[i]) shifted[i] = static_cast<size_t>(moved[i] - lowest);
  }
  return shifted;
}

}  // namespace orc
