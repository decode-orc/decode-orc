/*
 * File:        closed_caption_assembler.cpp
 * Module:      orc-gui
 * Purpose:     Trailing-frame-window cache and caption-screen history for the
 *              closed caption preview dialog
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "closed_caption_assembler.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <string>
#include <utility>

namespace {

// One field's caption data as the line 21 rules read it.
orc::EIA608FieldReading caption_reading(
    const ClosedCaptionAssembler::FieldData& field) {
  orc::EIA608FieldReading reading;
  reading.present = field.present;
  reading.data0 = static_cast<uint8_t>(field.data0 & 0x7F);
  reading.data1 = static_cast<uint8_t>(field.data1 & 0x7F);
  reading.parity0_valid = field.parity0_valid;
  reading.parity1_valid = field.parity1_valid;
  return reading;
}

// Trim trailing spaces and clip to the display grid. The decoder deliberately
// lets a row run past 32 characters (its other consumer writes timed text,
// which has no column limit); the caption display has 32 columns and that is
// what a viewer saw.
std::string clip_row(const std::string& row, int columns) {
  std::string clipped =
      row.substr(0, std::min(row.size(), static_cast<std::size_t>(columns)));
  while (!clipped.empty() && clipped.back() == ' ') {
    clipped.pop_back();
  }
  return clipped;
}

std::string trim(const std::string& text) {
  std::size_t first = text.find_first_not_of(' ');
  if (first == std::string::npos) {
    return {};
  }
  const std::size_t last = text.find_last_not_of(' ');
  return text.substr(first, last - first + 1);
}

}  // namespace

bool ClosedCaptionAssembler::CaptionScreen::blank() const {
  for (const auto& row : rows) {
    if (!trim(row).empty()) {
      return false;
    }
  }
  return true;
}

std::string ClosedCaptionAssembler::CaptionScreen::text() const {
  std::string result;
  for (const auto& row : rows) {
    const std::string line = trim(row);
    if (line.empty()) {
      continue;
    }
    if (!result.empty()) {
      result += ' ';
    }
    result += line;
  }
  return result;
}

ClosedCaptionAssembler::ClosedCaptionAssembler() { restartDecodeAt(0); }

void ClosedCaptionAssembler::setService(orc::EIA608Service service) {
  if (service == service_) {
    return;
  }
  service_ = service;
  // Nothing decoded under the old selection describes the new one, and the
  // bytes of frames already consumed have been released, so the window has to
  // be read again from the start of the run.
  discardAccumulated();
  restartDecodeAt(anchorFor(current_frame_));
}

ClosedCaptionAssembler::~ClosedCaptionAssembler() = default;

uint64_t ClosedCaptionAssembler::anchorFor(uint64_t frame_index) {
  return frame_index >= kTrailingWindowFrames - 1
             ? frame_index - (kTrailingWindowFrames - 1)
             : 0;
}

void ClosedCaptionAssembler::discardAccumulated() {
  frames_.clear();
  byte_log_.clear();
  const bool had_history = !history_.empty();
  history_.clear();
  if (had_history) {
    ++history_revision_;
  }
}

void ClosedCaptionAssembler::restartDecodeAt(uint64_t anchor_frame) {
  decode_anchor_ = anchor_frame;
  decode_frontier_ = anchor_frame;
  decoder_ = std::make_unique<orc::EIA608Decoder>();
  demux_ = std::make_unique<orc::EIA608ServiceDemux>(
      service_, /*suppress_repeated_controls=*/true);

  // History from the anchor on was produced by the run being replaced. The
  // re-read will produce it again, and keeping it in the meantime would show a
  // caption attributed to a decode that no longer exists.
  const auto stale = history_.lower_bound(anchor_frame);
  if (stale != history_.end()) {
    history_.erase(stale, history_.end());
    ++history_revision_;
  }

  last_screen_ = CaptionScreen{};
  last_mode_ = orc::CaptionMode::POP_ON;
  last_rollup_rows_ = 2;
}

void ClosedCaptionAssembler::setCurrentFrame(uint64_t frame_index) {
  // Decode what has already arrived before anything is discarded below: the
  // history must not depend on whether a reader happened to look between the
  // delivery of a frame and its eviction.
  refresh();

  // A seek is a move to somewhere no part of this decode run describes: more
  // than a whole window forward of the previewer, or so far back that a fresh
  // window here would not reach where the run began. The forward measure is
  // taken from where the previewer was, not from how far decoding has got:
  // reads can lag a long way behind a user holding down a step key, and a
  // backlog is no reason to throw away the history.
  const bool seek = frame_index >= current_frame_ + kTrailingWindowFrames ||
                    (frame_index < decode_anchor_ &&
                     decode_anchor_ - frame_index > kTrailingWindowFrames);

  // Anything else that lands before the anchor is a rewind: the decoder only
  // moves forwards, so reaching frames before the anchor means laying the run
  // out again from further back.
  const bool rewind = !seek && frame_index < decode_anchor_;

  if (seek) {
    discardAccumulated();
  }
  if (seek || rewind) {
    restartDecodeAt(anchorFor(frame_index));
  }

  current_frame_ = frame_index;
  // Frames behind the previewer are kept whatever the trailing length, because
  // the decoder still has to consume them in order — dropping one it has not
  // reached would stall the frontier on a frame that could never arrive.
  // Frames ahead of it are kept as far as a continuous move could come back to
  // them; past that reach only a seek could revisit them, and a seek discards
  // everything.
  frames_.erase(frames_.upper_bound(retainedFrameLimit()), frames_.end());
  refresh_pending_ = true;
}

std::vector<uint64_t> ClosedCaptionAssembler::framesNeedingData() const {
  std::vector<uint64_t> needed;
  // From the frontier rather than the window start: everything before it has
  // been decoded, and a frame arriving late can no longer be used, so
  // re-reading it would achieve nothing.
  const uint64_t start = decode_frontier_;
  auto cached = frames_.lower_bound(start);
  for (uint64_t frame = start;
       frame <= current_frame_ && needed.size() < kMaxFramesPerRequest;
       ++frame) {
    while (cached != frames_.end() && cached->first < frame) {
      ++cached;
    }
    if (cached != frames_.end() && cached->first == frame) {
      continue;
    }
    needed.push_back(frame);
  }
  return needed;
}

std::size_t ClosedCaptionAssembler::framesNeedingDataCount() const {
  const uint64_t start = decode_frontier_;
  if (current_frame_ < start) {
    return 0;
  }
  const uint64_t span = current_frame_ - start + 1;
  const auto cached = static_cast<uint64_t>(std::distance(
      frames_.lower_bound(start), frames_.upper_bound(current_frame_)));
  return static_cast<std::size_t>(span > cached ? span - cached : 0);
}

void ClosedCaptionAssembler::storeFrame(uint64_t frame_index, FieldData field1,
                                        FieldData field2) {
  if (frame_index < windowStartFrame() || frame_index > retainedFrameLimit()) {
    return;  // stale delivery from a superseded window
  }
  if (frame_index < decode_frontier_) {
    return;  // the decoder has already moved past this frame
  }
  frames_[frame_index] = FrameData{field1, field2};
  refresh_pending_ = true;
  countFieldOrder(frame_index, frames_[frame_index]);
}

void ClosedCaptionAssembler::countFieldOrder(uint64_t frame_index,
                                             const FrameData& data) {
  if (frame_index < field_order_frontier_) {
    return;  // counted when it was first delivered
  }
  field_order_frontier_ = frame_index + 1;
  int stored_field = 0;
  for (const FieldData* field : {&data.field1, &data.field2}) {
    const auto pair = orc::eia608_pair_by_parity(caption_reading(*field));
    if (pair.has_value() && !pair->first_only) {
      field_order_.add(stored_field, pair->data0, pair->data1);
    }
    ++stored_field;
  }
  if (field_order_.reversed() == fields_reversed_) {
    return;
  }

  // What this run decoded was decoded with the fields the other way round:
  // the run is laid out again from its anchor, and the frames it has already
  // read go back to be decoded again rather than being read again.
  fields_reversed_ = !fields_reversed_;
  const auto first = byte_log_.lower_bound(decode_anchor_);
  const auto last = byte_log_.upper_bound(retainedFrameLimit());
  for (auto it = first; it != last; ++it) {
    frames_.emplace(it->first, it->second);
  }
  restartDecodeAt(decode_anchor_);
}

void ClosedCaptionAssembler::markFrameUnavailable(uint64_t frame_index) {
  storeFrame(frame_index, FieldData{}, FieldData{});
}

bool ClosedCaptionAssembler::hasFrame(uint64_t frame_index) const {
  // Consumed frames are dropped from the cache, so "already decoded" counts as
  // fetched just as much as "still held". Frames before the anchor were never
  // fetched at all — the current decode run does not reach them.
  if (frame_index >= decode_anchor_ && frame_index < decode_frontier_) {
    return true;
  }
  return frames_.find(frame_index) != frames_.end();
}

void ClosedCaptionAssembler::clear() {
  discardAccumulated();
  field_order_ = orc::EIA608FieldOrder{};
  field_order_frontier_ = 0;
  fields_reversed_ = false;
  restartDecodeAt(anchorFor(current_frame_));
  refresh_pending_ = false;
}

const ClosedCaptionAssembler::ScreenChange* ClosedCaptionAssembler::screenAt(
    uint64_t frame_index) const {
  refresh();
  auto it = history_.upper_bound(frame_index);
  if (it == history_.begin()) {
    return nullptr;  // nothing decoded at or before this frame
  }
  --it;
  return &it->second;
}

std::vector<ClosedCaptionAssembler::ScreenChange>
ClosedCaptionAssembler::captions() const {
  refresh();
  std::vector<ScreenChange> captions;
  captions.reserve(history_.size());
  for (const auto& [frame, change] : history_) {
    // A change that cleared the screen is the end of a caption, not one of its
    // own; listing it would put a blank row between every pair of captions.
    if (!change.screen.blank()) {
      captions.push_back(change);
    }
  }
  return captions;
}

const ClosedCaptionAssembler::FrameData* ClosedCaptionAssembler::frameData(
    uint64_t frame_index) const {
  refresh();
  const auto it = byte_log_.find(frame_index);
  return it == byte_log_.end() ? nullptr : &it->second;
}

uint64_t ClosedCaptionAssembler::historyRevision() const {
  refresh();
  return history_revision_;
}

ClosedCaptionAssembler::CaptionScreen ClosedCaptionAssembler::snapshotScreen()
    const {
  CaptionScreen screen;
  const auto& rows = decoder_->displayed().rows();
  for (std::size_t row = 0; row < rows.size(); ++row) {
    screen.rows[row] =
        orc::eia608_cells_to_utf8(clip_row(rows[row], kScreenColumns));
  }
  return screen;
}

void ClosedCaptionAssembler::recordChange(uint64_t frame,
                                          CaptionScreen screen) const {
  ScreenChange change;
  change.frame = frame;
  change.screen = std::move(screen);
  change.mode = last_mode_;
  change.rollup_rows = last_rollup_rows_;
  history_[frame] = std::move(change);
  while (history_.size() > kMaxHistoryEntries) {
    history_.erase(history_.begin());
  }
  ++history_revision_;
}

void ClosedCaptionAssembler::refresh() const {
  if (!refresh_pending_) {
    return;
  }
  refresh_pending_ = false;

  // Feed every frame from the frontier that has arrived, stopping at the first
  // gap: the decoder is a state machine fed in transmission order, so a frame
  // still in flight has to be waited for rather than skipped.
  for (auto it = frames_.find(decode_frontier_);
       it != frames_.end() && it->first == decode_frontier_;
       it = frames_.find(decode_frontier_)) {
    const uint64_t frame = it->first;
    FrameData& data = it->second;

    int stored_field = 0;
    for (const FieldData* field : {&data.field1, &data.field2}) {
      const int field_in_frame =
          fields_reversed_ ? 1 - stored_field : stored_field;
      ++stored_field;
      // The pairs the caption sink takes, so the preview shows what the
      // export produces rather than a more optimistic reading of it: both
      // parities (or a character whose partner's failed, alone), the bytes
      // with no function on the field ignored, and a code EIA-608 defines for
      // the field.
      const auto read = orc::eia608_pair_by_parity(caption_reading(*field));
      if (!read.has_value()) {
        continue;
      }
      const orc::EIA608Pair pair =
          orc::eia608_pair_for_field(field_in_frame, *read);
      if (!orc::eia608_pair_is_defined(field_in_frame, pair.data0,
                                       pair.data1)) {
        continue;
      }

      // Line 21 carries four services in one byte-pair stream. Only the
      // selected one reaches the decoder; the rest would land another
      // service's text in the middle of this one's screen.
      if (!demux_->accept(field_in_frame, pair.data0, pair.data1)) {
        continue;
      }

      // Timestamps are frame indices rather than seconds: the dialog reports
      // frames, and the decoder only compares them.
      decoder_->process_bytes(static_cast<double>(frame), pair.data0,
                              pair.data1);
    }

    last_mode_ = decoder_->mode();
    last_rollup_rows_ = decoder_->rollup_rows();
    CaptionScreen screen = snapshotScreen();
    if (screen != last_screen_) {
      last_screen_ = screen;
      recordChange(frame, std::move(screen));
    }

    byte_log_[frame] = std::move(data);
    ++decode_frontier_;
    // The frame has been decoded and can never contribute again; the cache is
    // for frames still waiting their turn, so dropping it here is what keeps
    // memory flat over an unbounded forward run.
    frames_.erase(it);
  }

  while (byte_log_.size() > kMaxByteLogFrames) {
    byte_log_.erase(byte_log_.begin());
  }
}
