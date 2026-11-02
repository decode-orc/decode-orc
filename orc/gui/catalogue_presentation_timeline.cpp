/*
 * File:        catalogue_presentation_timeline.cpp
 * Module:      orc-gui
 * Purpose:     Reads the timing a catalogue display list carries: which
 *              operations are drawn and what each palette entry holds at a
 *              moment of the presentation, and when that next changes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "catalogue_presentation_timeline.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numeric>

namespace {

/// Slack for counting the rows a sweep crosses, which arithmetic can leave a
/// hair under a whole number.
constexpr double kRowSlack = 1e-6;

/// When the step of @p track in force at @p time_ms was taken, counted round
/// its loop as many times as the loop has gone: 0 where no step is at or
/// before @p time_ms.
uint32_t stepAtOrBefore(const orc::CataloguePaletteTrack& track,
                        uint32_t time_ms) {
  uint32_t folded = time_ms;
  if (CataloguePresentationTimeline::loops(track) &&
      time_ms >= uint64_t{track.loop_start_ms} + track.loop_period_ms) {
    folded = track.loop_start_ms +
             (time_ms - track.loop_start_ms) % track.loop_period_ms;
  }
  const auto after =
      std::upper_bound(track.steps.begin(), track.steps.end(), folded,
                       [](uint32_t time, const orc::CatalogueColourStep& step) {
                         return time < step.time_ms;
                       });
  if (after == track.steps.begin()) {
    return 0;
  }
  const uint32_t taken = std::prev(after)->time_ms;
  // A step before the loop is taken once, at its own time. Every looping
  // track has a step where its loop starts, so a folded time always finds one
  // inside the loop; this only guards a track that breaks that promise.
  if (folded == time_ms || taken < track.loop_start_ms) {
    return taken;
  }
  return time_ms - (folded - taken);
}

}  // namespace

bool CataloguePresentationTimeline::loops(
    const orc::CataloguePaletteTrack& track) {
  if (track.loop_period_ms == 0) {
    return false;
  }
  return std::any_of(track.steps.begin(), track.steps.end(),
                     [&track](const orc::CatalogueColourStep& step) {
                       return step.time_ms >= track.loop_start_ms;
                     });
}

CataloguePresentationTimeline::CataloguePresentationTimeline(
    const orc::CatalogueDisplayList& list)
    : tracks_(list.palette_tracks), timed_(list.timed()) {
  if (list.nominal_height > 0 && list.aspect_height > 0.0) {
    row_pitch_ = list.aspect_height / list.nominal_height;
  }
  appear_times_.reserve(list.ops.size());
  // The operations a sweep reveals together are listed together, so a sweep
  // is read once for all of them.
  const orc::CatalogueDrawOp* last_swept = nullptr;
  for (const orc::CatalogueDrawOp& op : list.ops) {
    appear_times_.push_back(op.appear_ms);
    if (op.reveal_duration_ms == 0) {
      continue;
    }
    if (last_swept != nullptr && last_swept->appear_ms == op.appear_ms &&
        last_swept->reveal_duration_ms == op.reveal_duration_ms) {
      continue;
    }
    last_swept = &op;
    const std::vector<RevealStep> steps = revealSteps(op);
    // The first position, at the start, has passed nothing yet.
    for (size_t i = 1; i < steps.size(); ++i) {
      appear_times_.push_back(steps[i].time_ms);
    }
  }
  // Operations are listed in appear order, but the times are sorted here
  // rather than trusted to be: a lookup over them is a binary search.
  std::sort(appear_times_.begin(), appear_times_.end());
  appear_times_.erase(std::unique(appear_times_.begin(), appear_times_.end()),
                      appear_times_.end());
}

double CataloguePresentationTimeline::sweepLine(const orc::CatalogueDrawOp& op,
                                                uint64_t after_ms) const {
  const uint32_t length = op.appear_ms - revealStart(op);
  const double span = op.reveal_to_y - op.reveal_from_y;
  const double passed = static_cast<double>(after_ms) / length;
  if (row_pitch_ <= 0.0) {
    return op.reveal_from_y + passed * span;
  }
  // Rows of the grid the sweep crosses; it moves on by whole rows, the last
  // part row coming in with the end.
  const double rows = std::fabs(span) / row_pitch_ + kRowSlack;
  const double direction = span < 0.0 ? -1.0 : 1.0;
  return op.reveal_from_y + direction * std::floor(passed * rows) * row_pitch_;
}

std::vector<CataloguePresentationTimeline::RevealStep>
CataloguePresentationTimeline::revealSteps(
    const orc::CatalogueDrawOp& op) const {
  std::vector<RevealStep> steps;
  const uint32_t start = revealStart(op);
  const uint32_t length = op.appear_ms - start;
  for (uint64_t field = 0;; ++field) {
    const uint64_t after =
        field * kRevealFieldNumerator / kRevealFieldDenominator;
    if (after >= length) {
      break;
    }
    steps.push_back(
        RevealStep{start + static_cast<uint32_t>(after), sweepLine(op, after)});
  }
  return steps;
}

std::optional<double> CataloguePresentationTimeline::revealLineAt(
    const orc::CatalogueDrawOp& op, uint32_t time_ms) const {
  if (op.reveal_duration_ms == 0 || time_ms >= op.appear_ms ||
      time_ms < revealStart(op)) {
    return std::nullopt;
  }
  // The steps are a field apart, so the one in force is found by arithmetic
  // rather than by listing them: the last field that began at or before
  // |time_ms|.
  const uint64_t into = time_ms - revealStart(op);
  uint64_t field = into * kRevealFieldDenominator / kRevealFieldNumerator;
  while (field > 0 &&
         field * kRevealFieldNumerator / kRevealFieldDenominator > into) {
    --field;
  }
  while ((field + 1) * kRevealFieldNumerator / kRevealFieldDenominator <=
         into) {
    ++field;
  }
  return sweepLine(op, field * kRevealFieldNumerator / kRevealFieldDenominator);
}

orc::CatalogueColour CataloguePresentationTimeline::colourAt(
    const orc::CatalogueColour& colour, uint32_t time_ms) const {
  if (colour.palette_index < 0) {
    return colour;
  }
  const auto index = static_cast<size_t>(colour.palette_index);
  if (index >= tracks_.size()) {
    return colour;
  }
  // A track with no steps hands back the colour it is given, which is the
  // colour's own still RGB.
  return tracks_[index].colour_at(time_ms, colour);
}

std::optional<uint32_t> CataloguePresentationTimeline::nextChangeAfter(
    uint32_t time_ms) const {
  uint32_t next = orc::kCatalogueNever;
  const auto appear =
      std::upper_bound(appear_times_.begin(), appear_times_.end(), time_ms);
  if (appear != appear_times_.end()) {
    next = *appear;
  }
  for (const orc::CataloguePaletteTrack& track : tracks_) {
    const uint32_t change = track.next_change_after(time_ms);
    // A change that does not come after |time_ms| is the track's arithmetic
    // running off the end of the 32-bit range, which no presentation reaches.
    if (change > time_ms && change < next) {
      next = change;
    }
  }
  if (next == orc::kCatalogueNever) {
    return std::nullopt;
  }
  return next;
}

std::optional<uint32_t> CataloguePresentationTimeline::settledAt() const {
  uint32_t settled = appear_times_.empty() ? 0 : appear_times_.back();
  for (const orc::CataloguePaletteTrack& track : tracks_) {
    if (track.steps.empty()) {
      continue;
    }
    if (loops(track)) {
      return std::nullopt;
    }
    for (const orc::CatalogueColourStep& step : track.steps) {
      settled = std::max(settled, step.time_ms);
    }
  }
  return settled;
}

std::optional<CataloguePresentationTimeline::Cycle>
CataloguePresentationTimeline::cycle() const {
  if (settledAt()) {
    return std::nullopt;
  }
  // Each looping track repeats with its own period from its loop start, and
  // everything else on screen has stopped changing once the last stage is up
  // and the last held step taken; from the latest of those the whole screen
  // repeats with a period every loop's period divides.
  uint64_t start = appear_times_.empty() ? 0 : appear_times_.back();
  uint64_t common = 1;
  uint32_t longest = 0;
  bool exact = true;
  for (const orc::CataloguePaletteTrack& track : tracks_) {
    if (track.steps.empty()) {
      continue;
    }
    if (!loops(track)) {
      start = std::max<uint64_t>(start, track.steps.back().time_ms);
      continue;
    }
    start = std::max<uint64_t>(start, track.loop_start_ms);
    longest = std::max(longest, track.loop_period_ms);
    if (exact) {
      common = std::lcm(common, uint64_t{track.loop_period_ms});
      exact = common <= kLongestCycleMs;
    }
  }
  Cycle cycle;
  cycle.start_ms = static_cast<uint32_t>(start);
  cycle.period_ms = exact ? static_cast<uint32_t>(common) : longest;
  return cycle;
}

uint32_t CataloguePresentationTimeline::changeAtOrBefore(
    uint32_t time_ms) const {
  uint32_t latest = 0;
  const auto appear =
      std::upper_bound(appear_times_.begin(), appear_times_.end(), time_ms);
  if (appear != appear_times_.begin()) {
    latest = *std::prev(appear);
  }
  for (const orc::CataloguePaletteTrack& track : tracks_) {
    latest = std::max(latest, stepAtOrBefore(track, time_ms));
  }
  return latest;
}
