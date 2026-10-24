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
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "vbi-services/teletext_bit_clock.h"
#include "vbi-services/teletext_slicer.h"

namespace orc {

// Widest 0-based field line the standard window can reach, plus headroom for a
// caller that widened it: the trackers are indexed by field line, so this
// sizes their tables. Lines past it are simply not tracked.
constexpr int32_t kNabtsMaxTrackedFieldLine = 31;

// Fields per frame: the line mask is kept per field, and it is the stride of
// a block of sliced fields (NabtsBlockSlicer) as well as its loop bound.
constexpr size_t kNabtsFieldsPerFrame = 2;

// Field lines the mask tables hold.
constexpr size_t kNabtsTrackedLines =
    static_cast<size_t>(kNabtsMaxTrackedFieldLine) + 1;

/// Whether each (field, 0-based field line) has ever carried a packet.
using NabtsLineMask =
    std::array<std::array<bool, kNabtsTrackedLines>, kNabtsFieldsPerFrame>;

/// Whether (field_idx, field_line) is inside the tables a NabtsLineMask holds.
constexpr bool nabts_line_tracked(size_t field_idx, int32_t field_line) {
  return field_idx < kNabtsFieldsPerFrame && field_line >= 0 &&
         field_line <= kNabtsMaxTrackedFieldLine;
}

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
  // which is what a caller with no pass behind it gets by default. A pass
  // sets both (see NabtsScanState).
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
    if (!nabts_line_tracked(field_idx, field_line)) {
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

// Packets a line must have yielded before it is judged, and the share of them
// that must prefer the other 525-line service's framing code
// (TeletextLineResult::other_framing_fits_better) for the line to be that
// service's. A line of NABTS prefers the other code on well under one packet
// in a hundred, where the recording damaged the two bits the codes differ in;
// a line of WST read inside the framing margin, on a band-limited tape whose
// smearing leaves the two codes fitting it about equally, on about half.
constexpr uint32_t kNabtsOtherServiceMinPackets = 64;
constexpr uint32_t kNabtsOtherServiceMinPercent = 20;

// The share of a line's packets whose five prefix bytes must arrive whole for
// the line to carry NABTS at all (nabts_prefix_every_byte_whole()). Every
// NABTS recording measured has at least 72 % whole over its lines; a line of
// some other signal behind NABTS's run-in and framing code, well under 1 %.
constexpr uint32_t kNabtsLineMinWholePercent = 10;

/**
 * @brief Which lines carry the other 525-line teletext service, or no NABTS
 *
 * System B (WST) and System C (NABTS) share the bit rate, the clock run-in,
 * the lines and the levels on a 525-line recording, and differ in two bits of
 * the framing code. The MLSE detector accepts a line whose samples fit NABTS's
 * code within a margin of WST's, so that a genuine line damaged in those two
 * bits is not lost — and on a band-limited recording a WST line passes inside
 * the same margin often enough to be listed as NABTS: half the packets of two
 * recordings of a WST service were WST rows read as NABTS "channels". One
 * packet's preference says nothing; a line's run of packets says which service
 * the line carries, since a line carries one. So does the share of its packets
 * whose prefix arrived whole, which on a line of some other signal is nearly
 * none.
 *
 * Fed every packet the emission accepts, in order. Until a line has yielded
 * kNabtsOtherServiceMinPackets of them, a packet preferring the other code is
 * refused on its own account — on a NABTS line that is the rare one the margin
 * was there to keep. From then on a line enough of whose packets preferred the
 * other code, or too few of whose prefixes arrived whole, is not NABTS, and
 * every packet it yields is refused; any other line's are all kept, as the
 * margin intends.
 */
class NabtsServiceLines {
 public:
  /// Whether to refuse a packet read from |field_line| of field |field_idx|,
  /// which |prefers_other| or not and whose prefix arrived |whole| or not: see
  /// the class comment.
  bool refuse(size_t field_idx, int32_t field_line, bool prefers_other,
              bool whole);

  /// Lines judged not NABTS, by field and line.
  std::vector<std::pair<size_t, int32_t>> other_service_lines() const;

  /// Per field and line, the packets seen and those preferring the other
  /// code, for the log.
  std::string summary() const;

 private:
  struct Counts {
    uint32_t packets = 0;
    uint32_t prefer_other = 0;
    uint32_t whole = 0;
  };
  /// Whether |counts| judge their line not NABTS.
  static bool not_nabts(const Counts& counts);
  std::map<std::pair<size_t, int32_t>, Counts> counts_;
};

/**
 * @brief Learns where this recording routinely loses packets on lines the
 *        pass cannot see
 *
 * NabtsGroupAssembler believes a continuity gap only as far as there have been
 * lines to lose the packets on, counting the lines that were read. A line the
 * pass has learnt never yields a packet is not read, and usually rightly not
 * counted: the service never used it, and a gap claiming a packet lost there
 * is a misread index. But a tape can lay a test signal over lines the service
 * did transmit on. A service sending four packets a field on lines 14 to 17
 * can reach a tape that loses line 16 of every field and line 17 of many (one
 * keeps line 17 of the second field and not of the first), so fields lose one
 * or two packets on lines that are never read, and
 * those losses read as misread indices.
 *
 * The continuity index itself (CEA-516 §3.2.4) says where that happens. Two
 * consecutive packets of a channel step it by one more than the packets sent
 * between them, so each pair of positions — the line and field of one packet
 * and of the next — collects the losses its steps have shown. A loss seen at
 * least kLossesToTrust times and on more than half of the steps between the
 * same pair of positions is routine there, and the group is told of that many
 * lines before the packet arrives. Where the service never sent on the unread
 * lines the steps show no loss, and nothing changes.
 *
 * Memory is one small histogram per pair of positions seen, O(lines^2) at
 * most. Not thread safe, and deliberately so: the frame loop advances it in
 * frame order, so what it knows at any point is a function of the recording
 * and not of the scheduling.
 */
class NabtsRoutineLosses {
 public:
  /// Steps a loss must show before it is trusted: enough that a misread
  /// index or two at the start of a pass decides nothing.
  static constexpr uint32_t kLossesToTrust = 3;

  /// A packet position: the field (0 or 1) and the field line.
  struct Position {
    size_t field = 0;
    int32_t line = 0;
    bool operator<(const Position& other) const {
      return field != other.field ? field < other.field : line < other.line;
    }
  };

  /// Record two consecutive packets of one channel, at |from| and |to|, whose
  /// continuity indices differ by |step| modulo 16. O(log positions).
  void observe(Position from, Position to, uint8_t step);

  /// The packets routinely lost between a packet at |from| and the channel's
  /// next at |to|: the loss the steps between them have shown most often, when
  /// it is at least kLossesToTrust times and more than half of the steps seen
  /// there, and is not fifteen (the index repeating itself); zero otherwise.
  /// O(log positions).
  uint8_t routine_loss(Position from, Position to) const;

  /// Note that a group was told of the routine loss between |from| and |to|
  /// because the lines it had counted fell short of it. O(log positions).
  void note_told(Position from, Position to) { told_.insert({from, to}); }

  /// Every pair of positions whose routine loss a group was told of, with
  /// that loss as it stands now, for the report.
  struct Routine {
    Position from;
    Position to;
    uint8_t loss = 0;
  };
  std::vector<Routine> told() const;

 private:
  std::map<std::pair<Position, Position>, std::array<uint32_t, 16>> steps_;
  std::set<std::pair<Position, Position>> told_;
};

/**
 * @brief The learned state of one pass, and what the stage asked for of it
 *
 * Held by the frame loop, which advances it in frame order from the results
 * the workers produced, and freezes it into a NabtsScanSnapshot for each
 * block of frames they slice. Not thread safe, and not read by the workers:
 * they see only the snapshot.
 *
 * A pass always acts on what it has learned: every snapshot pins the
 * acquisition sweep to the phase hint and reads only the lines the mask
 * marks. Neither acts before there is something to act on — the hint stays
 * invalid until enough locks agree, and the mask stands aside for the
 * learning frames and every recheck frame — so the bit clock is the only
 * thing a stage chooses whether to learn.
 */
class NabtsScanState {
 public:
  explicit NabtsScanState(bool calibrate_clock,
                          NabtsLineTrackerOptions line_options = {})
      : calibrate_clock_(calibrate_clock), line_options_(line_options) {}

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

  /// Where the recording routinely loses packets on lines the pass cannot
  /// see. Kept by the emission side of the pass, which sees the packets in
  /// order; the workers never read it.
  NabtsRoutineLosses& routine_losses() { return losses_; }
  const NabtsRoutineLosses& routine_losses() const { return losses_; }

 private:
  bool calibrate_clock_ = true;
  NabtsLineTrackerOptions line_options_;
  NabtsPhaseTracker phase_;
  NabtsLineTracker lines_;
  NabtsRoutineLosses losses_;
  TeletextClockCalibrator clock_;
};

}  // namespace orc

#endif  // ORC_NABTS_SCAN_STATE_H
