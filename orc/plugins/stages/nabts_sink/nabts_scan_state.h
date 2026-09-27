/*
 * File:        nabts_scan_state.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     What a pass over a recording learns about where its NABTS data
 *              is, so later frames cost less than the first ones
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_SCAN_STATE_H
#define ORC_NABTS_SCAN_STATE_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "vbi-services/teletext_bit_clock.h"
#include "vbi-services/teletext_slicer.h"

namespace orc {

// Widest 0-based field line the standard window can reach, plus headroom for a
// caller that widened it: the trackers are indexed by field line, so this
// sizes their tables. Lines past it are simply not tracked.
constexpr int32_t kNabtsMaxTrackedFieldLine = 31;

// Fields per frame, which is what the line mask is kept per.
constexpr size_t kNabtsFields = 2;

// Field lines the mask tables hold.
constexpr size_t kNabtsTrackedLines =
    static_cast<size_t>(kNabtsMaxTrackedFieldLine) + 1;

/// Whether each (field, 0-based field line) has ever carried a packet.
using NabtsLineMask =
    std::array<std::array<bool, kNabtsTrackedLines>, kNabtsFields>;

/// When the line mask stands aside and the full window is read anyway.
struct NabtsLineTrackerOptions {
  // Frames read in full before the mask engages. Long enough to cover a
  // recording whose first frames sit in a lead-in with no data on them.
  uint64_t learn_frames = 50;
  // Every Nth frame reads the full window regardless, so a line that starts
  // carrying data later is found. Zero disables the recheck, which pins the
  // mask to whatever the learning frames saw.
  uint64_t recheck_interval = 50;
};

/**
 * @brief What recovery knows about a recording while it slices a block of it
 *
 * Deliberately a value, and deliberately frozen. Slicing runs on several
 * threads at once, and what each of them may read has to be fixed before they
 * start — otherwise the packets a frame yields would depend on how far ahead
 * of it the other threads had got, and two runs of the same recording would
 * not agree. See NabtsScanState for how a pass advances from one of these
 * to the next.
 *
 * Every method is a pure function of this value and its arguments, so a worker
 * holds no scan state of its own.
 */
struct NabtsScanSnapshot {
  // Narrow the acquisition sweep to |hint|. False leaves |hint| unread.
  bool pin_phase = false;
  // Read only the lines |line_alive| marks. False reads the whole window,
  // which is what a caller that has learned nothing (or wants nothing learned)
  // gets by default.
  bool mask_lines = false;

  // Read at |bit_clock| (TeletextSlicerOptions::bit_clock), and while it is
  // still being learned score every packet at the trial clocks as well.
  double bit_clock = 1.0;
  bool calibrate_clock = false;

  TeletextPhaseHint hint;
  NabtsLineMask line_alive{};
  NabtsLineTrackerOptions line_options;

  /// Whether frame |frame_index| of the pass (0-based) reads every candidate
  /// line whatever the mask says.
  bool reads_full_window(uint64_t frame_index) const {
    if (!mask_lines) {
      return true;
    }
    return frame_index < line_options.learn_frames ||
           (line_options.recheck_interval > 0 &&
            frame_index % line_options.recheck_interval == 0);
  }

  /// Whether frame |frame_index| should slice |field_line| of |field_idx|.
  bool should_probe(uint64_t frame_index, size_t field_idx,
                    int32_t field_line) const {
    if (reads_full_window(frame_index)) {
      return true;
    }
    if (field_idx >= kNabtsFields || field_line < 0 ||
        field_line > kNabtsMaxTrackedFieldLine) {
      // Outside the tracked table: never masked, so a caller that widened the
      // window past what this can index keeps the behaviour it had before.
      return true;
    }
    return line_alive[field_idx][static_cast<size_t>(field_line)];
  }

  /// The hint to sweep against, which is none unless pinning is on.
  TeletextPhaseHint acquisition_hint() const {
    return pin_phase ? hint : TeletextPhaseHint{};
  }
};

/**
 * @brief Learns the acquisition window this recording's data lines sit in
 *
 * Every data line of a time-base-corrected recording begins at essentially the
 * same sample, because the position of the data burst in the line is fixed by
 * the standard and the TBC has already removed the timing variation the
 * transport added. The slicer nevertheless sweeps the whole CEA-516 §2.3
 * window on every line, because a slicer looking at one line in isolation has
 * no way to know that.
 *
 * This accumulates the lock positions of lines that did yield packets and,
 * once enough of them agree, offers them back as a TeletextPhaseHint. The hint
 * is advisory in the strongest sense: a hinted attempt that fails is retried
 * over the full window (see TeletextPhaseHint), so a tracker that has learned
 * the wrong thing costs time and nothing else.
 */
class NabtsPhaseTracker {
 public:
  // Locks accumulated before a hint is offered. Enough that a couple of
  // spurious locks cannot move the centre far, few enough that the saving
  // starts within the first frames of a recording that carries data on several
  // lines per field.
  static constexpr size_t kLocksBeforeHinting = 24;

  // Locks kept. The window is a running one so a source whose data start
  // shifts part way through — a recording spliced from two transfers — is
  // followed rather than averaged with its own past.
  static constexpr size_t kLockWindow = 64;

  // Smallest half-width a hint may claim, in samples. Roughly a bit period —
  // 5,727272 Mbit/s at the 14,31818 MHz 4FSC sample rate is 2,5 samples — so a
  // lock that has drifted by a bit is still inside the pinned window.
  static constexpr double kMinRadiusSamples = 3.0;

  // Half-width above which the locks are too scattered to be a pin: the hint
  // is withheld and the full window swept. The CEA-516 §2.3 search window is
  // about 53 samples wide, so this still narrows it fourfold.
  static constexpr double kMaxRadiusSamples = 7.0;

  /// Record the phase a line locked at. Ignores lines that never locked.
  void observe(const TeletextLineResult& result);

  /// The window to pin the sweep to, or an invalid hint while the tracker is
  /// still learning or the locks it has seen disagree too much.
  TeletextPhaseHint hint() const { return hint_; }

  /// Locks recorded over the whole pass (diagnostics).
  uint64_t locks_seen() const { return locks_seen_; }

 private:
  std::array<double, kLockWindow> locks_{};
  size_t count_ = 0;  // Entries of locks_ in use, up to kLockWindow
  size_t next_ = 0;   // Where the next lock goes (ring buffer)
  uint64_t locks_seen_ = 0;
  // Maintained by observe() so hint() is a read rather than a sort.
  TeletextPhaseHint hint_;
};

/**
 * @brief Learns which field lines this recording actually carries data on
 *
 * A service uses a handful of the lines its standard permits — the window is
 * what may be used, not what is. Every frame of a pass otherwise pays to slice
 * every line of the window, and on a line that carries picture content, VITS,
 * VITC or EIA-608 that cost is spent reaching a rejection already reached on
 * the frame before.
 *
 * A line is kept once it has produced a packet. When the mask engages, and how
 * often it stands aside so a line that comes alive later is found, is
 * NabtsScanSnapshot's half of the arrangement.
 */
class NabtsLineTracker {
 public:
  /// Record what slicing (|field_idx|, |field_line|) produced.
  void observe(size_t field_idx, int32_t field_line, bool valid);

  /// The lines seen to carry data so far.
  const NabtsLineMask& alive() const { return alive_; }

  /// Add |count| to the tally of candidate lines the mask skipped.
  void add_skipped(uint64_t count) { lines_skipped_ += count; }

  /// Candidate lines the mask skipped over the whole pass (diagnostics).
  uint64_t lines_skipped() const { return lines_skipped_; }

 private:
  NabtsLineMask alive_{};
  uint64_t lines_skipped_ = 0;
};

/**
 * @brief The learned state of one pass, and what the stage asked for of it
 *
 * Held by the frame loop, which advances it in frame order from the results
 * the workers produced, and freezes it into a NabtsScanSnapshot for each
 * block of frames they slice. Not thread safe, and not read by the workers:
 * they see only the snapshot.
 */
class NabtsScanState {
 public:
  NabtsScanState(bool pin_phase, bool learn_lines, bool calibrate_clock,
                 NabtsLineTrackerOptions line_options = {})
      : pin_phase_(pin_phase),
        learn_lines_(learn_lines),
        calibrate_clock_(calibrate_clock),
        line_options_(line_options) {}

  /// Freeze what is known now, for the next block of frames to slice against.
  NabtsScanSnapshot snapshot() const;

  /// Take in one sliced line, in the pass's own order, with its trial-clock
  /// scores when it was scored.
  void observe(size_t field_idx, int32_t field_line,
               const TeletextLineResult& result,
               const TeletextClockCalibrator::Scores* clock_scores = nullptr);

  const NabtsPhaseTracker& phase() const { return phase_; }
  const TeletextClockCalibrator& clock() const { return clock_; }
  NabtsLineTracker& lines() { return lines_; }
  const NabtsLineTracker& lines() const { return lines_; }

 private:
  bool pin_phase_ = true;
  bool learn_lines_ = true;
  bool calibrate_clock_ = true;
  NabtsLineTrackerOptions line_options_;
  NabtsPhaseTracker phase_;
  NabtsLineTracker lines_;
  TeletextClockCalibrator clock_;
};

}  // namespace orc

#endif  // ORC_NABTS_SCAN_STATE_H
