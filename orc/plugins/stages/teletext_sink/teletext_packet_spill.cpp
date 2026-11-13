/*
 * File:        teletext_packet_spill.cpp
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Holds the recovered packet stream between the squash vote and
 *              the rewrite pass, on disk rather than in memory
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "teletext_packet_spill.h"

#include <orc/support/logging.h>
#include <orc/support/scratch_file.h>

#include <algorithm>
#include <cstring>

namespace orc {

namespace {

constexpr size_t kBlockBytes = size_t{1} << 20;

constexpr uint8_t kFlagEmpty = 0x01;
constexpr uint8_t kFlagConfidence = 0x02;

// Flags byte + 64-bit field index.
constexpr size_t kRecordHeaderBytes = 1 + sizeof(int64_t);

size_t record_bytes(uint8_t flags) {
  size_t bytes = kRecordHeaderBytes;
  if ((flags & kFlagEmpty) == 0) bytes += kTeletextPacketBytes;
  if ((flags & kFlagConfidence) != 0) bytes += kTeletextPacketBytes;
  return bytes;
}

}  // namespace

TeletextPacketSpill::TeletextPacketSpill(std::unique_ptr<ScratchFile> file)
    : file_(std::move(file)) {
  buffer_.reserve(kBlockBytes);
}

TeletextPacketSpill::~TeletextPacketSpill() = default;

bool TeletextPacketSpill::flush_write_buffer() {
  if (buffer_.empty()) return true;
  if (file_) {
    if (!file_->write_at(stored_bytes_, buffer_.data(), buffer_.size())) {
      failed_ = true;
      return false;
    }
  } else {
    memory_.insert(memory_.end(), buffer_.begin(), buffer_.end());
  }
  stored_bytes_ += buffer_.size();
  buffer_.clear();
  return true;
}

bool TeletextPacketSpill::append(const SpilledTeletextPacket& packet) {
  if (failed_ || reading_) return false;
  uint8_t flags = 0;
  if (packet.empty) flags |= kFlagEmpty;
  if (!packet.empty && packet.has_confidence) flags |= kFlagConfidence;
  if (buffer_.size() + record_bytes(flags) > kBlockBytes &&
      !flush_write_buffer()) {
    return false;
  }
  buffer_.push_back(flags);
  const auto field = static_cast<uint64_t>(packet.field_index);
  for (int shift = 0; shift < 64; shift += 8) {
    buffer_.push_back(static_cast<uint8_t>((field >> shift) & 0xFF));
  }
  if ((flags & kFlagEmpty) == 0) {
    buffer_.insert(buffer_.end(), packet.bytes.begin(), packet.bytes.end());
  }
  if ((flags & kFlagConfidence) != 0) {
    buffer_.insert(buffer_.end(), packet.confidence.begin(),
                   packet.confidence.end());
  }
  ++count_;
  return true;
}

bool TeletextPacketSpill::rewind() {
  if (failed_) return false;
  if (!reading_ && !flush_write_buffer()) return false;
  reading_ = true;
  read_offset_ = 0;
  buffer_.clear();
  buffer_pos_ = 0;
  return true;
}

bool TeletextPacketSpill::fill_read_buffer(size_t need) {
  if (buffer_.size() - buffer_pos_ >= need) return true;
  // Keep the unread tail, then top up with the next block from the store.
  buffer_.erase(buffer_.begin(),
                buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_pos_));
  buffer_pos_ = 0;
  const uint64_t left = stored_bytes_ - read_offset_;
  const size_t take = static_cast<size_t>(
      std::min<uint64_t>(left, kBlockBytes - buffer_.size()));
  if (take > 0) {
    const size_t at = buffer_.size();
    buffer_.resize(at + take);
    if (file_) {
      if (!file_->read_at(read_offset_, buffer_.data() + at, take)) {
        failed_ = true;
        return false;
      }
    } else {
      std::memcpy(buffer_.data() + at,
                  memory_.data() + static_cast<size_t>(read_offset_), take);
    }
    read_offset_ += take;
  }
  return buffer_.size() >= need;
}

bool TeletextPacketSpill::next(SpilledTeletextPacket& packet) {
  if (!reading_ || failed_) return false;
  if (!fill_read_buffer(1)) return false;
  const uint8_t flags = buffer_[buffer_pos_];
  const size_t bytes = record_bytes(flags);
  if (!fill_read_buffer(bytes)) {
    // A record cut short means the store is damaged, not merely at its end.
    if (buffer_.size() > buffer_pos_) failed_ = true;
    return false;
  }
  const uint8_t* p = buffer_.data() + buffer_pos_ + 1;
  uint64_t field = 0;
  for (int i = 0; i < 8; ++i) {
    field |= static_cast<uint64_t>(p[i]) << (8 * i);
  }
  p += 8;
  packet.field_index = static_cast<int64_t>(field);
  packet.empty = (flags & kFlagEmpty) != 0;
  packet.has_confidence = (flags & kFlagConfidence) != 0;
  if (!packet.empty) {
    std::memcpy(packet.bytes.data(), p, kTeletextPacketBytes);
    p += kTeletextPacketBytes;
  } else {
    packet.bytes.fill(0);
  }
  if (packet.has_confidence) {
    std::memcpy(packet.confidence.data(), p, kTeletextPacketBytes);
  } else {
    packet.confidence.fill(0);
  }
  buffer_pos_ += bytes;
  return true;
}

std::unique_ptr<TeletextPacketSpill> make_teletext_packet_spill() {
  std::string error;
  auto file = ScratchFile::create(scratch_directory("teletext"),
                                  "orc-teletext-", ".spill", error);
  if (!file) {
    ORC_LOG_WARN(
        "TeletextSink: {}; holding the packet stream in memory for squashing "
        "instead",
        error);
  }
  return std::make_unique<TeletextPacketSpill>(std::move(file));
}

}  // namespace orc
