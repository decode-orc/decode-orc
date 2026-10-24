/*
 * File:        nabts_scan_state.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Phase and active-line learning for one pass over a recording
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_scan_state.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstddef>

namespace orc {

namespace {

// Fraction of the recorded locks trimmed from each end before the spread is
// measured. A line that locked onto something that was not the data burst
// would otherwise widen the hint until it is worthless, and a source with
// real line-to-line jitter produces exactly such a lock now and again.
constexpr double kTrimFraction = 0.05;

}  // namespace

void NabtsPhaseTracker::observe(const TeletextLineResult& result) {
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

void NabtsLineTracker::observe(size_t field_idx, int32_t field_line,
                               bool valid) {
  if (!valid || !nabts_line_tracked(field_idx, field_line)) {
    return;
  }
  alive_[field_idx][static_cast<size_t>(field_line)] = true;
}

void NabtsRoutineLosses::observe(Position from, Position to, uint8_t step) {
  // §3.2.4: the index counts the channel's packets modulo 16, so the packets
  // sent between the two are the step less one — and a step of zero, the same
  // index again, is fifteen lost or a misread, which the histogram simply
  // records as the former.
  const size_t lost = (static_cast<size_t>(step) + 15U) % 16U;
  ++steps_[{from, to}][lost];
}

uint8_t NabtsRoutineLosses::routine_loss(Position from, Position to) const {
  const auto it = steps_.find({from, to});
  if (it == steps_.end()) {
    return 0;
  }
  const std::array<uint32_t, 16>& counts = it->second;
  uint32_t total = 0;
  size_t most = 0;
  for (size_t lost = 0; lost < counts.size(); ++lost) {
    total += counts[lost];
    if (counts[lost] > counts[most]) {
      most = lost;
    }
  }
  // Fifteen lost is the index repeating itself: a line whose test signal
  // decodes as the same packet every frame, or a misread, never a routine.
  if (most == 15 || counts[most] < kLossesToTrust ||
      2 * counts[most] <= total) {
    return 0;
  }
  return static_cast<uint8_t>(most);
}

std::vector<NabtsRoutineLosses::Routine> NabtsRoutineLosses::told() const {
  std::vector<Routine> out;
  for (const auto& [from, to] : told_) {
    out.push_back(Routine{from, to, routine_loss(from, to)});
  }
  return out;
}

NabtsScanSnapshot NabtsScanState::snapshot() const {
  NabtsScanSnapshot snapshot;
  snapshot.pin_phase = true;
  snapshot.mask_lines = true;
  snapshot.bit_clock = clock_.bit_clock();
  snapshot.calibrate_clock = calibrate_clock_ && clock_.calibrating();
  snapshot.hint = phase_.hint();
  snapshot.line_alive = lines_.alive();
  snapshot.line_options = line_options_;
  return snapshot;
}

void NabtsScanState::observe(
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

bool NabtsServiceLines::not_nabts(const Counts& counts) {
  if (counts.packets < kNabtsOtherServiceMinPackets) {
    return false;
  }
  const auto packets = static_cast<uint64_t>(counts.packets);
  return static_cast<uint64_t>(counts.prefer_other) * 100 >=
             packets * kNabtsOtherServiceMinPercent ||
         static_cast<uint64_t>(counts.whole) * 100 <
             packets * kNabtsLineMinWholePercent;
}

bool NabtsServiceLines::refuse(size_t field_idx, int32_t field_line,
                               bool prefers_other, bool whole) {
  Counts& counts = counts_[{field_idx, field_line}];
  ++counts.packets;
  counts.prefer_other += prefers_other ? 1 : 0;
  counts.whole += whole ? 1 : 0;
  if (counts.packets < kNabtsOtherServiceMinPackets) {
    return prefers_other;
  }
  return not_nabts(counts);
}

std::vector<std::pair<size_t, int32_t>> NabtsServiceLines::other_service_lines()
    const {
  std::vector<std::pair<size_t, int32_t>> lines;
  for (const auto& [line, counts] : counts_) {
    if (not_nabts(counts)) {
      lines.push_back(line);
    }
  }
  return lines;
}

std::string NabtsServiceLines::summary() const {
  std::string out;
  for (const auto& [line, counts] : counts_) {
    out += fmt::format(
        "{}field {} line {}: {} of {} prefer the other code, {} prefixes whole",
        out.empty() ? "" : "; ", line.first + 1, line.second + 1,
        counts.prefer_other, counts.packets, counts.whole);
  }
  return out;
}

}  // namespace orc
