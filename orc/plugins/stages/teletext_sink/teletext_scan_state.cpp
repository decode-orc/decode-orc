/*
 * File:        teletext_scan_state.cpp
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Phase and active-line learning for one pass over a recording
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_scan_state.h"

#include <algorithm>
#include <cstddef>

namespace orc {

namespace {

// Fraction of the recorded locks trimmed from each end before the spread is
// measured. A line that locked onto something that was not the data burst
// would otherwise widen the hint until it is worthless, and a source with
// real line-to-line jitter produces exactly such a lock now and again.
constexpr double kTrimFraction = 0.05;

// Whether (field_idx, field_line) is inside the mask tables.
bool is_tracked(size_t field_idx, int32_t field_line) {
  return field_idx < kTeletextFields && field_line >= 0 &&
         field_line <= kTeletextMaxTrackedFieldLine;
}

}  // namespace

void TeletextPhaseTracker::observe(const TeletextLineResult& result) {
  if (!result.valid || result.lock_sample < 0.0) {
    // Only lines that yielded a packet are evidence of where the data starts:
    // a lock the later gates rejected is as likely to be noise as signal.
    return;
  }

  locks_[next_] = result.lock_sample;
  next_ = (next_ + 1) % kLockWindow;
  count_ = std::min(count_ + 1, kLockWindow);
  ++locks_seen_;

  if (count_ < kLocksBeforeHinting) {
    return;
  }

  // Recomputed here rather than in hint() because hint() is read once per
  // block of frames and this runs once per recovered packet.
  std::array<double, kLockWindow> sorted{};
  std::copy_n(locks_.begin(), count_, sorted.begin());
  std::sort(sorted.begin(), sorted.begin() + static_cast<ptrdiff_t>(count_));

  const size_t trim =
      static_cast<size_t>(static_cast<double>(count_) * kTrimFraction);
  const double low = sorted[trim];
  const double high = sorted[count_ - 1 - trim];

  hint_.centre = 0.5 * (low + high);
  hint_.radius = std::max(kMinRadiusSamples, 0.5 * (high - low));
  // Locks that disagree by more than a pin's worth are not a pin. Withholding
  // the hint costs the full sweep, which is what an unpinned pass pays anyway.
  hint_.valid = (hint_.radius <= kMaxRadiusSamples);
}

void TeletextLineTracker::observe(size_t field_idx, int32_t field_line,
                                  bool valid) {
  if (!valid || !is_tracked(field_idx, field_line)) {
    return;
  }
  alive_[field_idx][static_cast<size_t>(field_line)] = true;
}

TeletextScanSnapshot TeletextScanState::snapshot() const {
  TeletextScanSnapshot snapshot;
  snapshot.pin_phase = true;
  snapshot.mask_lines = true;
  snapshot.bit_clock = clock_.bit_clock();
  snapshot.calibrate_clock = calibrate_clock_ && clock_.calibrating();
  snapshot.hint = phase_.hint();
  snapshot.line_alive = lines_.alive();
  snapshot.line_options = line_options_;
  return snapshot;
}

void TeletextScanState::observe(
    size_t field_idx, int32_t field_line, const TeletextLineResult& result,
    const TeletextClockCalibrator::Scores* clock_scores) {
  // Both trackers learn from every line sliced. What they know reaches the
  // workers only through the next snapshot, so nothing taken in here changes a
  // block of frames already being sliced.
  phase_.observe(result);
  lines_.observe(field_idx, field_line, result.valid);
  if (clock_scores != nullptr) {
    clock_.observe(result, *clock_scores);
  }
}

}  // namespace orc
