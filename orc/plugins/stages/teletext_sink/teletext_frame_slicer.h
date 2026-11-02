/*
 * File:        teletext_frame_slicer.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Per-frame World System Teletext line recovery from a video
 *              frame representation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_TELETEXT_FRAME_SLICER_H
#define ORC_TELETEXT_FRAME_SLICER_H

#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/video_frame_representation.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "teletext_scan_state.h"
#include "vbi-services/teletext_slicer.h"

namespace orc {

// Candidate VBI window as 0-based field lines, identical in both fields of a
// frame.
//
// 625 lines — ETSI EN 300 706 §4.1: broadcast lines 6 to 22 (field 1) and 318
// to 335 (field 2) may carry teletext; in the frame-flat buffer those are
// 0-based field lines 5-21 of each field (field 2 offset by the field-1 line
// count).
constexpr int32_t kTeletextFirstFieldLine625 = 5;
constexpr int32_t kTeletextLastFieldLine625 = 21;

// 525 lines — ITU-R BT.653 §2 allows broadcast lines 10 to 21 (field 1) and
// 273 to 284 (field 2), 0-based field lines 9-20 of each field. The window
// stops a line short of that, at broadcast lines 20 and 283: line 21 of both
// fields is the caption line (47 CFR 73.682(a)(22), 15.119), and its EIA-608
// is not as safely rejected as its lack of a framing code suggested. Six of
// twelve 525-line teletext recordings measured gave packets there; read alone,
// one recording's line 21 gave 360 of them at a mean decision confidence
// of 0.12, and not one page. A service that did use line 21 is read by setting
// last_vbi_line to 21.
constexpr int32_t kTeletextFirstFieldLine525 = 9;
constexpr int32_t kTeletextLastFieldLine525 = 19;

/// Slicer tuning the stage's parameters map onto.
struct TeletextFrameSlicerOptions {
  // Bit detector (TeletextSlicerOptions::detector).
  TeletextDetector detector = TeletextDetector::kAuto;
  // Restore odd parity on damaged display bytes by flipping the bit the MLSE
  // detector was least sure of (TeletextSlicerOptions::parity_repair).
  bool parity_repair = true;
  // Candidate window override as 0-based field lines, identical in both
  // fields. Unset means the television system's own window (above), which is
  // what a caller with no opinion wants.
  std::optional<int32_t> first_field_line;
  std::optional<int32_t> last_field_line;
};

/// One candidate line of one field, sliced.
struct TeletextFrameLineResult {
  /// 0-based field line the result came from
  int32_t field_line = 0;
  /// 0-based line in the frame-flat buffer the samples were read from
  size_t flat_line = 0;
  TeletextLineResult sliced;
  /// The line read again at each trial bit clock, while the pass is still
  /// learning the clock and this line yielded a packet.
  std::optional<TeletextClockCalibrator::Scores> clock_scores;
};

/// What slicing one field of one frame produced.
struct TeletextFieldScan {
  /// One entry per candidate line that was read and yielded samples, in
  /// ascending line order — rejected lines included, so a caller accumulating
  /// recovery diagnostics sees every line that was looked at.
  std::vector<TeletextFrameLineResult> lines;
  /// Candidate lines the active-line mask skipped without reading.
  uint64_t lines_skipped = 0;

  void clear() {
    lines.clear();
    lines_skipped = 0;
  }
};

// The television systems ITU-R BT.653 System B is defined on, in the order
// this builds its slicers. Every per-system fact lives in
// TeletextFrameSlicer::profile_for(); this is only the list of rows.
inline constexpr std::array<VideoSystem, 3> kTeletextVideoSystems = {
    VideoSystem::PAL, VideoSystem::NTSC, VideoSystem::PAL_M};

/**
 * @brief Recovers the World System Teletext packets of one field of a frame
 *
 * Owns one TeletextSlicer per television system — each carries its own sample
 * rate, bit rate, packet length and data-timing window, and all are cheap
 * enough to build once here rather than per frame — and selects between them
 * from the representation's video parameters.
 *
 * System B only: 625 lines per ETSI EN 300 706 (PAL) and 525 per ITU-R BT.653
 * Table 1b (NTSC and PAL_M). NABTS, the other BT.653 service carried on the
 * same 525-line VBI lines, is recovered by the nabts_sink stage, which has its
 * own copy of this pass — see docs-tech/nabts-support-design.md §2 for why the
 * two are not one.
 *
 * Thread safety: slice_field() is const and the class holds no mutable state;
 * a single instance may be used concurrently from multiple threads.
 */
class TeletextFrameSlicer {
 public:
  explicit TeletextFrameSlicer(TeletextFrameSlicerOptions options = {});

  /**
   * @brief Everything about a television system that teletext recovery needs
   *
   * The single place a video system is turned into the facts that follow from
   * it. Recovery reads nothing per-system anywhere else, so a system added
   * here is a system added: no caller has to be found and updated to match.
   */
  struct SystemProfile {
    /// False for a system carrying no service this can recover, in which case
    /// nothing below is meaningful.
    bool carries_teletext = false;
    TeletextSystem teletext_system = TeletextSystem::kWst625;
    int32_t first_field_line = kTeletextFirstFieldLine625;
    int32_t last_field_line = kTeletextLastFieldLine625;
    /// 4FSC sample rate (EBU Tech. 3280-E §1.1.1 Table 1, SMPTE 244M-2003
    /// §4.1, ITU-R BT.1700-1 Annex 1 Part B).
    double sample_rate = kPalSampleRate;
    /// Levels used when the source states none of its own. The data '0'
    /// reference is black in both systems (ETSI EN 300 706 §5.2, ITU-R BT.653
    /// Table 1b); on a 525-line system the transmitted '0' actually sits at
    /// blanking, below the 7,5 IRE setup black, which makes the amplitude gate
    /// derived from these stricter than the standard requires rather than
    /// looser — measured against a real burst it still clears by a factor of
    /// two.
    int16_t default_black = static_cast<int16_t>(kPalBlack);
    int16_t default_white = static_cast<int16_t>(kPalWhite);
    /// Index into kTeletextVideoSystems, and so into the slicers built from it.
    size_t slicer_index = 0;
  };

  /// Profile of |system|, before any option override is applied.
  static SystemProfile profile_for(VideoSystem system);

  /// Whether |system| carries a teletext service this can recover.
  static bool applies_to(VideoSystem system) {
    return profile_for(system).carries_teletext;
  }

  /// Profile actually probed, i.e. profile_for() with this instance's window
  /// override applied.
  SystemProfile effective_profile(VideoSystem system) const;

  /**
   * @brief Slice every candidate line of one field
   *
   * @param representation Source of the video parameters and the line samples
   * @param frame_id       Frame to read
   * @param field_idx      0 for field 1, 1 for field 2
   * @param frame_index    Position of this frame in the pass, 0-based, which
   *                       is what decides whether |snapshot|'s line mask
   *                       applies to it or stands aside
   * @param snapshot       What the pass knows about this recording. The
   *                       default is a pass that knows nothing: every line of
   *                       the window is sliced and every acquisition sweeps
   *                       the full timing window, which is what a caller
   *                       slicing a single frame wants. A line the mask skips
   *                       yields no entry in |out|.lines, exactly as a line
   *                       with no samples does.
   * @param out            Receives the sliced lines and the mask's tally.
   *                       Cleared first; reused across calls so a full-range
   *                       pass allocates once.
   *
   * Does nothing when the representation has no video parameters, carries a
   * system with no WST service, or holds no samples for the lines in question.
   *
   * Const and free of shared mutable state: several threads may slice
   * different frames of one representation through one TeletextFrameSlicer at
   * the same time, each with its own |out|. That is why what the pass has
   * learned arrives as a frozen |snapshot| rather than as a live tracker —
   * see TeletextScanSnapshot for why it has to.
   */
  void slice_field(const VideoFrameRepresentation& representation,
                   FrameID frame_id, size_t field_idx, uint64_t frame_index,
                   const TeletextScanSnapshot& snapshot,
                   TeletextFieldScan& out) const;

  /// As above for a caller with no pass behind it — one frame, everything
  /// read, full sweep.
  void slice_field(const VideoFrameRepresentation& representation,
                   FrameID frame_id, size_t field_idx,
                   TeletextFieldScan& out) const {
    slice_field(representation, frame_id, field_idx, /*frame_index=*/0,
                TeletextScanSnapshot{}, out);
  }

 private:
  TeletextFrameSlicerOptions options_;

  // One slicer per entry of kTeletextVideoSystems, selected by
  // SystemProfile::slicer_index. Each carries its own sample rate, bit rate,
  // packet length and data-timing window, and all are cheap enough to build
  // once here rather than per frame.
  std::array<TeletextSlicer, kTeletextVideoSystems.size()> slicers_;
};

}  // namespace orc

#endif  // ORC_TELETEXT_FRAME_SLICER_H
