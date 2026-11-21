/*
 * File:        frame_map_range_search.cpp
 * Module:      orc-stage-plugin-frame-map
 * Purpose:     Binary-chop frame search over monotonic VBI picture numbers
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "frame_map_range_search.h"

#include <algorithm>

namespace orc::frame_map_range {
namespace {

bool is_cancelled(const CancelCheck& cancelled) {
  return cancelled && cancelled();
}

struct ProbeHit {
  int64_t frame = 0;
  int64_t picture_number = 0;
};

// Probe outward from position (position, position+1, position-1, ...) within
// [lo, hi] until a readable picture number is found. Returns nullopt when the
// whole window is unreadable or the search was cancelled (cancelled_out set).
std::optional<ProbeHit> probe_outward(int64_t position, int64_t lo, int64_t hi,
                                      const PictureNumberProbe& probe,
                                      const CancelCheck& cancelled,
                                      bool& cancelled_out) {
  position = std::clamp(position, lo, hi);
  for (int64_t offset = 0;; ++offset) {
    const int64_t above = position + offset;
    const int64_t below = position - offset;
    if (above > hi && below < lo) {
      return std::nullopt;
    }
    if (above <= hi) {
      if (is_cancelled(cancelled)) {
        cancelled_out = true;
        return std::nullopt;
      }
      if (auto pn = probe(above)) {
        return ProbeHit{above, *pn};
      }
    }
    if (offset > 0 && below >= lo) {
      if (is_cancelled(cancelled)) {
        cancelled_out = true;
        return std::nullopt;
      }
      if (auto pn = probe(below)) {
        return ProbeHit{below, *pn};
      }
    }
  }
}

// First frame in [first, last] with a readable picture number >= threshold,
// assuming monotonic non-decreasing numbering. Threshold is int64_t so
// callers can pass target + 1 without int32_t overflow.
SearchOutcome lower_bound_frame(int64_t first, int64_t last, int64_t threshold,
                                const PictureNumberProbe& probe,
                                const CancelCheck& cancelled) {
  if (first > last) {
    return {SearchStatus::NotFound, 0};
  }

  // Right anchor: last readable frame; its number must reach the threshold.
  std::optional<ProbeHit> right;
  for (int64_t f = last; f >= first; --f) {
    if (is_cancelled(cancelled)) {
      return {SearchStatus::Cancelled, 0};
    }
    if (auto pn = probe(f)) {
      right = ProbeHit{f, *pn};
      break;
    }
  }
  if (!right || right->picture_number < threshold) {
    return {SearchStatus::NotFound, 0};
  }

  // Left anchor: first readable frame (exists; right is readable).
  ProbeHit left{};
  for (int64_t f = first; f <= right->frame; ++f) {
    if (is_cancelled(cancelled)) {
      return {SearchStatus::Cancelled, 0};
    }
    if (auto pn = probe(f)) {
      left = ProbeHit{f, *pn};
      break;
    }
  }
  if (left.picture_number >= threshold) {
    return {SearchStatus::Found, left.frame};
  }

  // Invariant: picture(left) < threshold <= picture(right). Chop until the
  // anchors are adjacent; unreadable frames between them are skipped by
  // probing outward from each midpoint.
  while (right->frame - left.frame > 1) {
    if (is_cancelled(cancelled)) {
      return {SearchStatus::Cancelled, 0};
    }
    const int64_t mid = left.frame + (right->frame - left.frame) / 2;
    bool aborted = false;
    auto hit = probe_outward(mid, left.frame + 1, right->frame - 1, probe,
                             cancelled, aborted);
    if (aborted) {
      return {SearchStatus::Cancelled, 0};
    }
    if (!hit) {
      break;  // nothing readable between the anchors
    }
    if (hit->picture_number < threshold) {
      left = *hit;
    } else {
      right = hit;
    }
  }
  return {SearchStatus::Found, right->frame};
}

}  // namespace

SearchOutcome find_first_frame_with_picture(int64_t first_frame,
                                            int64_t last_frame, int32_t target,
                                            const PictureNumberProbe& probe,
                                            const CancelCheck& cancelled) {
  SearchOutcome bound =
      lower_bound_frame(first_frame, last_frame, target, probe, cancelled);
  if (bound.status != SearchStatus::Found) {
    return bound;
  }
  auto pn = probe(bound.frame);
  if (pn && *pn == target) {
    return bound;
  }
  // Lower bound landed past the target: the number is absent (skipped gap).
  return {SearchStatus::NotFound, 0};
}

SearchOutcome find_last_frame_with_picture(int64_t first_frame,
                                           int64_t last_frame, int32_t target,
                                           const PictureNumberProbe& probe,
                                           const CancelCheck& cancelled) {
  SearchOutcome start = find_first_frame_with_picture(first_frame, last_frame,
                                                      target, probe, cancelled);
  if (start.status != SearchStatus::Found) {
    return start;
  }

  // Upper bound: first frame past the run carrying the target number.
  SearchOutcome next =
      lower_bound_frame(start.frame, last_frame,
                        static_cast<int64_t>(target) + 1, probe, cancelled);
  if (next.status == SearchStatus::Cancelled) {
    return next;
  }

  const int64_t scan_end =
      next.status == SearchStatus::Found ? next.frame - 1 : last_frame;
  // Scan back over the (usually short) run of frames carrying the target
  // number; unreadable frames within the run are skipped.
  for (int64_t f = scan_end; f > start.frame; --f) {
    if (is_cancelled(cancelled)) {
      return {SearchStatus::Cancelled, 0};
    }
    auto pn = probe(f);
    if (pn && *pn == target) {
      return {SearchStatus::Found, f};
    }
  }
  return start;
}

}  // namespace orc::frame_map_range
