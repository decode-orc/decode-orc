/*
 * File:        closed_caption_assembler.h
 * Module:      orc-gui
 * Purpose:     Trailing-frame-window cache and caption-screen history for the
 *              closed caption preview dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef CLOSED_CAPTION_ASSEMBLER_H
#define CLOSED_CAPTION_ASSEMBLER_H

#include <orc/support/eia608_decoder.h>
#include <orc/support/eia608_line21_pairs.h>
#include <orc/support/eia608_service_demux.h>
#include <orc_closed_caption.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief Trailing-frame-window byte cache and caption-screen history
 *
 * The closed caption preview dialog follows the frame previewer through a
 * stream that carries only two bytes per frame: what is on screen at any one
 * frame is the result of every byte before it, not of that frame's own data.
 * So the assembler decodes a trailing window of frames ending at the current
 * frame and records the caption screen as it changes.
 *
 * The record is a *history* keyed by the frame the screen changed at, which is
 * both what the dialog renders (the screen at the previewer's frame is the
 * newest entry at or before it) and what it lists (each entry with a caption on
 * it is a caption the recording showed). Keeping the changes rather than a
 * per-frame snapshot costs one entry per caption instead of one per frame, and
 * makes stepping backwards inside the decoded span exact rather than a
 * re-decode.
 *
 * Frames are fed to a *persistent* decoder in ascending order and never decoded
 * twice: the decoder is a state machine that cannot be rewound, and re-running
 * the window on every frame change would repeat the same work for the whole
 * window at every step. The consequence is that a frame arriving after the
 * decoder has passed it cannot contribute — see storeFrame().
 *
 * The pairs that reach the decoder are those the Closed Caption Sink takes
 * (orc/support/eia608_line21_pairs.h): a pair whose first byte failed its
 * parity is dropped, one whose second byte failed keeps its first only when
 * that is a character, a byte with no function on its field is ignored, and
 * a pair that is no code EIA-608 defines for its field is dropped. Which stored
 * field is television field 1 is decided from the control codes of the frames
 * delivered so far (EIA608FieldOrder); when that decision changes, the decode
 * run is laid out again. The sink's last rule, that a pair needs line 21 data
 * around it, is not applied: it looks at the frames after the one shown, which
 * the preview has not read.
 *
 * Qt-free by design so caption assembly is unit-testable without a
 * QApplication. Thread safety: none; confine an instance to the GUI thread.
 */
class ClosedCaptionAssembler {
 public:
  /**
   * Trailing window length in frames (25 s of 525/60 video).
   *
   * How far back the previewer reaches when it arrives at a new position, and
   * so how much context a caption arrived at by seeking is decoded from. A
   * caption is built a byte pair at a time over a second or two and stays up
   * for a few seconds more, so this reaches back over several of them: landing
   * mid-caption still shows the caption, and the list arrives with the last
   * few already on it rather than empty.
   *
   * It is not the limit on how much of a recording is remembered: the history
   * keeps accumulating as the previewer moves forward (bounded by
   * kMaxHistoryEntries). Only *reaching backwards* is bounded, and only
   * because each frame of it costs one observation read before anything can be
   * shown.
   */
  static constexpr uint64_t kTrailingWindowFrames = 750;

  /**
   * Frames framesNeedingData() will name in one call.
   *
   * Arriving at a new position leaves a whole window unread, and every frame
   * costs one observation read. Handing the caller all of them at once would
   * queue hundreds of reads ahead of the previewer's own rendering; the cap
   * paces them instead, and the caller comes back for more as deliveries land.
   * Frames are always named in ascending order, so the decoder's contiguous
   * frontier keeps advancing.
   */
  static constexpr std::size_t kMaxFramesPerRequest = 120;

  /// Screen changes retained. A captioned programme changes the screen every
  /// couple of seconds, so this is around half an hour of captions; the oldest
  /// entries are dropped once it is full.
  static constexpr std::size_t kMaxHistoryEntries = 512;

  /// Frames the recovered byte pairs are kept for, so the dialog can show the
  /// data of a frame the previewer goes back to. Two bytes a frame, so the
  /// bound is about memory hygiene over a long run rather than about size.
  static constexpr std::size_t kMaxByteLogFrames = 2048;

  /// EIA-608 display grid (CTA-608-E §4.2: 15 rows of 32 columns)
  static constexpr int kScreenRows =
      static_cast<int>(orc::CaptionBuffer::MAX_ROWS);
  static constexpr int kScreenColumns =
      static_cast<int>(orc::CaptionBuffer::MAX_COLS);

  /// What the caption decoder had on screen, row 0 at the top. Rows are
  /// trimmed of trailing spaces, clipped to the 32-column display grid and
  /// spelled in UTF-8.
  struct CaptionScreen {
    std::array<std::string, kScreenRows> rows{};

    /// True when no row carries any text (nothing is on screen)
    bool blank() const;
    /// Rows with text on them, joined by single spaces — the caption as a
    /// reader would read it aloud
    std::string text() const;

    bool operator==(const CaptionScreen& other) const {
      return rows == other.rows;
    }
    bool operator!=(const CaptionScreen& other) const {
      return !(*this == other);
    }
  };

  /// One point at which the caption display changed
  struct ScreenChange {
    /// Frame the change took effect at
    uint64_t frame = 0;
    CaptionScreen screen;
    /// Caption mode in force at that point (Pop-On, Roll-Up, Paint-On)
    orc::CaptionMode mode = orc::CaptionMode::POP_ON;
    /// Roll-Up window height (2-4); meaningful only in Roll-Up mode
    int rollup_rows = 2;
  };

  /// The caption bytes recovered from one field
  using FieldData = orc::presenters::ClosedCaptionFieldDataView;

  /// Both fields of one frame, as delivered
  struct FrameData {
    FieldData field1;
    FieldData field2;
  };

  ClosedCaptionAssembler();
  ~ClosedCaptionAssembler();

  /**
   * @brief Choose which of line 21's services to decode
   *
   * Line 21 carries CC1, CC2, TEXT1 and TEXT2 multiplexed into one byte-pair
   * stream; decoded together they interleave, so the preview follows one of
   * them. Changing it discards everything decoded so far — the decode run
   * would otherwise be half of one service and half of another — and the
   * window is read again from the observations.
   */
  void setService(orc::EIA608Service service);

  orc::EIA608Service service() const { return service_; }

  /**
   * @brief Advance the window so it ends at @p frame_index
   *
   * A move backwards past the start of the decode run lays the run out again
   * from a window before the new position — the decoder only goes forwards.
   * Screen history at or after the new anchor is dropped with it, because it
   * will be produced again by the re-read and would otherwise be an answer
   * from a decode that no longer exists.
   *
   * Only a *seek* discards everything: a move more than a whole window forward
   * of the previewer, or so far back that a fresh window here would not reach
   * where the run began. There nothing decoded so far describes anywhere near
   * the new position.
   */
  void setCurrentFrame(uint64_t frame_index);

  uint64_t currentFrame() const { return current_frame_; }

  /// First frame of the current decode run
  uint64_t windowStartFrame() const { return decode_anchor_; }

  /**
   * @brief Highest frame whose data is still worth holding or delivering
   *
   * Frames ahead of the previewer are kept after a backward step, as far as a
   * continuous move could come back to them; each one held is an observation
   * read saved when it does. Callers tracking in-flight reads should use this
   * rather than currentFrame() as the upper bound on what is still worth
   * waiting for.
   */
  uint64_t retainedFrameLimit() const {
    return current_frame_ + kTrailingWindowFrames;
  }

  /**
   * @brief Frames inside the current window with no cached data yet
   *
   * Ascending, and at most kMaxFramesPerRequest of them; call again once the
   * named frames have been delivered to get the next batch.
   */
  std::vector<uint64_t> framesNeedingData() const;

  /// Total frames in the window still without data (not capped by the batch
  /// size), for reporting progress
  std::size_t framesNeedingDataCount() const;

  /**
   * @brief Store the delivered per-field caption data for a frame
   *
   * Frames outside the run's reach are ignored (stale deliveries; see
   * retainedFrameLimit()), as are frames the decoder has already passed: it is
   * fed strictly forwards, so a late delivery for an earlier frame can no
   * longer contribute.
   */
  void storeFrame(uint64_t frame_index, FieldData field1, FieldData field2);

  /**
   * @brief Record that a frame could not be observed
   *
   * Cached as a frame carrying no caption data so the window converges: an
   * unobservable frame stays unobservable for this view node, and
   * re-requesting it on every frame change would issue a whole window of
   * requests per step. It also unblocks the decoder, which advances only over
   * frames it has.
   */
  void markFrameUnavailable(uint64_t frame_index);

  /// True once @p frame_index has been fetched, whether or not its bytes are
  /// still held (they are released as soon as the decoder consumes them)
  bool hasFrame(uint64_t frame_index) const;

  /// Drop all cached frames, the byte log, the screen history and what the
  /// field order was decided from
  void clear();

  /// True when the frames delivered show the recording's first stored field
  /// to be television field 2, so CC1 is read from each frame's second field
  bool fieldsReversed() const { return fields_reversed_; }

  /**
   * @brief The caption display as of @p frame_index
   *
   * The newest change at or before the frame, or nullptr when nothing has been
   * decoded before it. Invalidated by any mutating call.
   */
  const ScreenChange* screenAt(uint64_t frame_index) const;

  /// Screen changes that put a caption up, oldest first — the transcript of
  /// what the recording displayed. Changes that only cleared the screen are
  /// not captions and are left out.
  std::vector<ScreenChange> captions() const;

  /// The caption bytes recovered for @p frame_index, or nullptr when the frame
  /// has not been decoded (or has aged out of the byte log). Invalidated by
  /// any mutating call.
  const FrameData* frameData(uint64_t frame_index) const;

  /**
   * @brief Revision counter of the screen history
   *
   * Incremented whenever a change is recorded or dropped, so views can skip
   * rebuilding their list when nothing happened.
   */
  uint64_t historyRevision() const;

 private:
  /// Feed newly available frames to the decoder and record what the screen
  /// does (no-op unless the cache advanced since the last refresh).
  void refresh() const;

  /// Take the decoder's display state as a screen snapshot.
  CaptionScreen snapshotScreen() const;

  /// Record a screen change at @p frame, enforcing the history bound.
  void recordChange(uint64_t frame, CaptionScreen screen) const;

  /// Lay the decode run out again from @p anchor_frame, discarding the
  /// decoder's state and the history the old run produced from there on.
  void restartDecodeAt(uint64_t anchor_frame);

  /// Throw away everything learned by this decode run.
  void discardAccumulated();

  /// Count the field-order signs of a frame delivered for the first time, and
  /// lay the decode run out again from its anchor when they change which
  /// stored field is television field 1.
  void countFieldOrder(uint64_t frame_index, const FrameData& data);

  /// Frame a decode run arriving at @p frame_index should start from — one
  /// whole window back, clamped to the start of the recording.
  static uint64_t anchorFor(uint64_t frame_index);

  uint64_t current_frame_ = 0;

  // Keyed by frame index; std::map keeps temporal order for decoder feeding.
  // An entry is erased once the decoder has consumed it (the byte log is what
  // keeps the data afterwards), so this holds only frames waiting their turn.
  mutable std::map<uint64_t, FrameData> frames_;

  // First frame this decode run covers, and the first frame not yet fed to the
  // decoder. Everything in [decode_anchor_, decode_frontier_) has been decoded
  // exactly once, in ascending order.
  uint64_t decode_anchor_ = 0;
  mutable uint64_t decode_frontier_ = 0;

  // Fed strictly forwards and never rewound; recreated on a discontinuity.
  mutable std::unique_ptr<orc::EIA608Decoder> decoder_;

  // Routes the multiplexed stream to service_ before the decoder sees it, and
  // drops the duplicate copy of each control pair. Recreated with the decoder,
  // whose state it has to stay in step with.
  orc::EIA608Service service_ = orc::EIA608Service::CC1;
  mutable std::unique_ptr<orc::EIA608ServiceDemux> demux_;

  // Screen state as of the frontier, so a frame that changes nothing records
  // nothing.
  mutable CaptionScreen last_screen_;
  mutable orc::CaptionMode last_mode_ = orc::CaptionMode::POP_ON;
  mutable int last_rollup_rows_ = 2;

  // Screen changes keyed by the frame they took effect at.
  mutable std::map<uint64_t, ScreenChange> history_;
  mutable uint64_t history_revision_ = 0;
  mutable bool refresh_pending_ = false;

  // Recovered bytes of decoded frames, kept for the dialog's data readout.
  mutable std::map<uint64_t, FrameData> byte_log_;

  // Which stored field is television field 1, from the frames delivered so
  // far. Each frame is counted once: frames before field_order_frontier_ are
  // not counted again when a seek or a rewind delivers them a second time.
  orc::EIA608FieldOrder field_order_;
  uint64_t field_order_frontier_ = 0;
  bool fields_reversed_ = false;
};

#endif  // CLOSED_CAPTION_ASSEMBLER_H
