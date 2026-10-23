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

PadAlignment compute_pad_alignment(
    const std::vector<std::vector<PictureNumberReading>>& readings) {
  PadAlignment alignment;
  std::vector<std::optional<int32_t>> firsts;
  firsts.reserve(readings.size());
  int32_t earliest = 0;
  for (const auto& source : readings) {
    const std::optional<int32_t> first = picture_number_at_first_frame(source);
    firsts.push_back(first);
    if (first && (!alignment.first_picture || *first < earliest)) {
      earliest = *first;
      alignment.first_picture = earliest;
    }
  }

  alignment.pads.resize(readings.size());
  if (!alignment.first_picture) return alignment;
  for (size_t i = 0; i < firsts.size(); ++i) {
    const std::optional<int32_t> first = firsts[i];
    if (first) alignment.pads[i] = static_cast<size_t>(*first - earliest);
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

}  // namespace orc
