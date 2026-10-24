/*
 * File:        nabts_frame_slicer.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Per-frame NABTS line recovery from a video frame representation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_FRAME_SLICER_H
#define ORC_NABTS_FRAME_SLICER_H

#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/video_frame_representation.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "nabts_scan_state.h"
#include "vbi-services/teletext_slicer.h"

namespace orc {

// Candidate VBI window as 0-based field lines, identical in both fields of a
// frame.
//
// ITU-R BT.653 §2 and CEA-516 §1.1.1 allow broadcast lines 10 to 21 (field 1)
// and 273 to 284 (field 2), 0-based field lines 9-20 of each field. The window
// stops a line short of that, at broadcast lines 20 and 283: line 21 of both
// fields is the caption line (47 CFR 73.682(a)(22), 15.119), and its EIA-608
// is not as safely rejected as its lack of a framing code suggested. 77 of 98
// recordings measured gave packets there, 64,139 of them; read alone, one
// recording's line 21 gave 18,628 packets of which none had a whole prefix
// (nabts_prefix_every_byte_whole()) and not one record — noise that went into
// group reassembly beside the service's packets. A service that did use line 21
// is read by setting last_vbi_line to 21.
constexpr int32_t kNabtsFirstFieldLine = 9;
constexpr int32_t kNabtsLastFieldLine = 19;

/// Slicer tuning the stage's parameters map onto.
struct NabtsFrameSlicerOptions {
  // Bit detector (TeletextSlicerOptions::detector).
  TeletextDetector detector = TeletextDetector::kAuto;
  // Candidate window override as 0-based field lines, identical in both
  // fields. Unset means the standard window above, which is what a caller with
  // no opinion wants.
  std::optional<int32_t> first_field_line;
  std::optional<int32_t> last_field_line;

  // There is deliberately no parity-repair option. CEA-516 §3.3 makes byte
  // parity conditional on the data group type, which a single packet cannot
  // establish, so a System C packet's data bytes are never repaired against a
  // parity rule that may not apply to them.
};

/// One candidate line of one field, sliced.
struct NabtsFrameLineResult {
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
struct NabtsFieldScan {
  /// One entry per candidate line that was read and yielded samples, in
  /// ascending line order — rejected lines included, so a caller accumulating
  /// recovery diagnostics sees every line that was looked at.
  std::vector<NabtsFrameLineResult> lines;
  /// Candidate lines the active-line mask skipped without reading.
  uint64_t lines_skipped = 0;

  void clear() {
    lines.clear();
    lines_skipped = 0;
  }

  /// Room for exactly |count| lines, given back where a field reused for
  /// another reads fewer than the last one did. A result is 1.6 kB, and once
  /// a pass has learnt its lines most fields read a quarter to a half of the
  /// window.
  void fit(size_t count) {
    if (lines.capacity() != count) {
      std::vector<NabtsFrameLineResult> sized;
      sized.reserve(count);
      lines.swap(sized);
    }
  }
};

// The television systems NABTS is defined on, in the order this builds its
// slicers. CEA-516 §1.1.1 specifies it on the 525-line signal only, so PAL is
// absent: ITU-R BT.653-3 Table 1a does describe a 625-line System C at 367 ×
// fH, but its Table 2 lists no country that used it, so there is no 625-line
// service for a row to describe. Every per-system fact lives in
// NabtsFrameSlicer::profile_for(); this is only the list of rows.
inline constexpr std::array<VideoSystem, 2> kNabtsVideoSystems = {
    VideoSystem::NTSC, VideoSystem::PAL_M};

/**
 * @brief Recovers the NABTS packets of one field of a video frame
 *
 * Owns one TeletextSlicer per 525-line television system — each carries its own
 * sample rate, and both are cheap enough to build once here rather than per
 * frame — and selects between them from the representation's video parameters.
 *
 * System C only (CEA-516). World System Teletext, the other ITU-R BT.653
 * service carried on the same VBI lines of the same 525-line systems, is
 * recovered by the teletext_sink stage, which has its own copy of this pass.
 * The two share the clock run-in, the bit rate and the data levels and differ
 * in the framing code and the packet length, so a slicer configured for one
 * reads the other's lines as noise and rejects them — see
 * docs-tech/nabts-support-design.md §2 for why the stages are separate and
 * what they do still share.
 *
 * Thread safety: slice_field() is const and the class holds no mutable state;
 * a single instance may be used concurrently from multiple threads.
 */
class NabtsFrameSlicer {
 public:
  explicit NabtsFrameSlicer(NabtsFrameSlicerOptions options = {});

  /**
   * @brief Everything about a television system that NABTS recovery needs
   *
   * The single place a video system is turned into the facts that follow from
   * it. Recovery reads nothing per-system anywhere else, so a system added
   * here is a system added: no caller has to be found and updated to match.
   */
  struct SystemProfile {
    /// False for a system carrying no System C service, in which case nothing
    /// below is meaningful.
    bool carries_nabts = false;
    int32_t first_field_line = kNabtsFirstFieldLine;
    int32_t last_field_line = kNabtsLastFieldLine;
    /// 4FSC sample rate (SMPTE 244M-2003 §4.1, ITU-R BT.1700-1 Annex 1 Part B).
    double sample_rate = kNtscSampleRate;
    /// Levels used when the source states none of its own. The transmitted '0'
    /// sits at blanking and '1' at 70 IRE (CEA-516 §2.2), below the white these
    /// name; the amplitude gate derived from them is therefore stricter than
    /// the standard requires rather than looser — measured against a real
    /// recording it still clears by a factor of two.
    int16_t default_black = static_cast<int16_t>(kNtscBlack);
    int16_t default_white = static_cast<int16_t>(kNtscWhite);
    /// Index into kNabtsVideoSystems, and so into the slicers built from it.
    size_t slicer_index = 0;
  };

  /// Profile of |system|, before any option override is applied. A 625-line
  /// system reports carries_nabts false: CEA-516 §1.1.1 does not define the
  /// service there.
  static SystemProfile profile_for(VideoSystem system);

  /// Whether |system| carries a NABTS service this can recover.
  static bool applies_to(VideoSystem system) {
    return profile_for(system).carries_nabts;
  }

  /// Profile actually probed, i.e. profile_for() with this instance's window
  /// override applied.
  SystemProfile effective_profile(VideoSystem system) const;

  /**
   * @brief Candidate lines slice_field() reads of one field
   *
   * Those of the window that |snapshot|'s line mask leaves for the frame at
   * |frame_index| of the pass, before any of them turns out to hold no
   * samples: the room a field's NabtsFieldScan needs. Zero where the
   * representation carries no System C service.
   */
  size_t lines_to_read(const VideoFrameRepresentation& representation,
                       size_t field_idx, uint64_t frame_index,
                       const NabtsScanSnapshot& snapshot) const;

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
   *                       Cleared first; reused across calls, so a pass that
   *                       sizes it beforehand (NabtsFieldScan::fit(),
   *                       lines_to_read()) allocates nothing here.
   *
   * Does nothing when the representation has no video parameters, carries a
   * system with no System C service, or holds no samples for the lines in
   * question.
   *
   * Const and free of shared mutable state: several threads may slice
   * different frames of one representation through one NabtsFrameSlicer at the
   * same time, each with its own |out|. That is why what the pass has learned
   * arrives as a frozen |snapshot| rather than as a live tracker — see
   * NabtsScanSnapshot for why it has to.
   */
  void slice_field(const VideoFrameRepresentation& representation,
                   FrameID frame_id, size_t field_idx, uint64_t frame_index,
                   const NabtsScanSnapshot& snapshot,
                   NabtsFieldScan& out) const;

  /// As above for a caller with no pass behind it — one frame, everything
  /// read, full sweep.
  void slice_field(const VideoFrameRepresentation& representation,
                   FrameID frame_id, size_t field_idx,
                   NabtsFieldScan& out) const {
    slice_field(representation, frame_id, field_idx, /*frame_index=*/0,
                NabtsScanSnapshot{}, out);
  }

 private:
  NabtsFrameSlicerOptions options_;

  // One slicer per entry of kNabtsVideoSystems, selected by
  // SystemProfile::slicer_index.
  std::array<TeletextSlicer, kNabtsVideoSystems.size()> slicers_;
};

}  // namespace orc

#endif  // ORC_NABTS_FRAME_SLICER_H
