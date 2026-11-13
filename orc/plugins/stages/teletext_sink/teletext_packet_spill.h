/*
 * File:        teletext_packet_spill.h
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Holds the recovered packet stream between the squash vote and
 *              the rewrite pass, on disk rather than in memory
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/support/scratch_file.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "vbi-services/teletext_slicer.h"

namespace orc {

// One emitted packet as the rewrite pass needs it again.
struct SpilledTeletextPacket {
  std::array<uint8_t, kTeletextPacketBytes> bytes{};
  int64_t field_index = 0;
  // A keep_empty_packets placeholder: no bytes are stored for it.
  bool empty = false;
  // Per-byte confidence quantised to 8 bits (value × 255), when the recovery
  // chain could say. The rewrite re-feeds every copy with the weight it was
  // voted with, so the confidence travels with the packet.
  bool has_confidence = false;
  std::array<uint8_t, kTeletextPacketBytes> confidence{};
};

// The squash pass needs every copy of every row before it can rewrite any of
// them, so the whole recovered stream is kept until the end of the run: tens
// of millions of packets on a long recording. They are appended here in
// emission order and read back once, in the same order.
//
// Records are variable length (an empty placeholder is 9 bytes, a packet 51,
// a packet with confidence 93), buffered in 1 MiB blocks.
//
// Thread-safety: none; one instance belongs to one run.
class TeletextPacketSpill {
 public:
  // Backed by |file|; held in memory when |file| is null (unit tests, or a
  // run whose scratch file could not be created).
  explicit TeletextPacketSpill(std::unique_ptr<ScratchFile> file);
  ~TeletextPacketSpill();

  TeletextPacketSpill(const TeletextPacketSpill&) = delete;
  TeletextPacketSpill& operator=(const TeletextPacketSpill&) = delete;

  bool on_disk() const { return file_ != nullptr; }

  // Append one packet. False when the scratch file cannot be written (the
  // disk is full); the spill is then unusable.
  bool append(const SpilledTeletextPacket& packet);

  // Packets appended so far.
  uint64_t count() const { return count_; }

  // End the writing phase and start reading from the first packet.
  bool rewind();

  // Read the next packet; false at the end or on a read failure.
  bool next(SpilledTeletextPacket& packet);

  // True when a write or read failed.
  bool failed() const { return failed_; }

 private:
  bool flush_write_buffer();
  bool fill_read_buffer(size_t need);

  std::unique_ptr<ScratchFile> file_;
  std::vector<uint8_t> memory_;  // the store when there is no file
  std::vector<uint8_t> buffer_;
  uint64_t stored_bytes_ = 0;  // bytes flushed to the store
  uint64_t count_ = 0;
  bool failed_ = false;

  // Reading.
  bool reading_ = false;
  uint64_t read_offset_ = 0;  // next store byte to load into buffer_
  size_t buffer_pos_ = 0;
};

// A spill in a scratch file under scratch_directory("teletext"), falling back
// to memory (with a warning) when the file cannot be created.
std::unique_ptr<TeletextPacketSpill> make_teletext_packet_spill();

}  // namespace orc
