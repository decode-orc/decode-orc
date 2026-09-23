/*
 * File:        nabts_data_group.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     NABTS data group reassembly implementation (CEA-516 §4)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_data_group.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <bitset>
#include <limits>
#include <utility>

#include "vbi-services/teletext_slicer.h"

namespace orc {

namespace {

/// Two Hamming nibbles as one byte, the first the more significant — the
/// concatenation §4.2.5 and §4.2.6 specify for S1,S2 and F1,F2.
constexpr uint16_t concat_nibbles(int high, int low) {
  return static_cast<uint16_t>((high << 4) | low);
}

}  // namespace

NabtsGroupHeader nabts_decode_group_header(const uint8_t* bytes,
                                           size_t length) {
  NabtsGroupHeader header;
  if (bytes == nullptr || length < kNabtsGroupHeaderBytes) {
    return header;
  }

  // §4.2.1: all eight are Hamming 8/4. A group whose size did not decode has no
  // known extent, and one whose type did not decode cannot be routed, so the
  // header is all-or-nothing rather than partially trusted.
  std::array<int, kNabtsGroupHeaderBytes> nibbles{};
  for (size_t i = 0; i < kNabtsGroupHeaderBytes; ++i) {
    nibbles[i] = teletext_hamming84_decode(bytes[i]);
    if (nibbles[i] < 0) {
      return header;
    }
  }

  header.valid = true;
  header.attested = std::all_of(bytes, bytes + kNabtsGroupHeaderBytes,
                                teletext_hamming84_clean);
  header.type = static_cast<uint8_t>(nibbles[0]);
  header.continuity = static_cast<uint8_t>(nibbles[1]);
  header.repetition = static_cast<uint8_t>(nibbles[2]);
  header.further_blocks = concat_nibbles(nibbles[3], nibbles[4]);
  header.final_block_bytes = concat_nibbles(nibbles[5], nibbles[6]);
  header.routing = static_cast<uint8_t>(nibbles[7]);
  return header;
}

std::string NabtsGroupStats::summary() const {
  std::string out = "Data group reassembly\n";
  out += fmt::format(
      "  Packets:       {} seen, {} prefix rejected, {} orphaned, {} bundle\n"
      "  Groups:        {} completed, {} superseded, {} unfinished\n",
      packets_seen, prefix_failures, orphan_packets, bundle_packets,
      groups_completed, groups_superseded, groups_unfinished);
  if (header_failures > 0 || oversized_groups > 0 || refused_groups > 0) {
    out += fmt::format(
        "  Refused:       {} bad header, {} oversized, {} over the open-group "
        "limit\n",
        header_failures, oversized_groups, refused_groups);
  }
  if (continuity_misreads > 0) {
    out += fmt::format(
        "  Continuity:    {} index(es) claiming more packets lost than there "
        "were lines for, read as the nearest possible index\n",
        continuity_misreads);
  }
  if (non_teletext_groups > 0) {
    out += fmt::format(
        "  Not teletext:  {} completed groups of a type other than zero\n",
        non_teletext_groups);
  }
  return out;
}

void NabtsGroupAssembler::append_block(OpenGroup& group,
                                       const NabtsPacket& packet) {
  if (packet.data_length == 0) {
    return;  // A bundle packet: counted by the caller, contributes no bytes.
  }
  group.last_nonzero_offset = group.stream.size();
  group.last_nonzero_length = packet.data_length;
  group.stream.insert(
      group.stream.end(), packet.data.begin(),
      packet.data.begin() + static_cast<ptrdiff_t>(packet.data_length));
  // Bytes that arrived are present only while the group can still say where
  // they belong; past a continuity index the header contradicts, nothing can.
  group.present.insert(group.present.end(), packet.data_length,
                       group.placeable ? 1 : 0);
  group.confidence.insert(
      group.confidence.end(), packet.confidence.begin(),
      packet.confidence.begin() + static_cast<ptrdiff_t>(packet.data_length));
}

void NabtsGroupAssembler::append_hole(OpenGroup& group, uint32_t blocks) {
  const size_t bytes = static_cast<size_t>(blocks) * group.nominal_block_bytes;
  if (bytes == 0) {
    return;
  }
  // Zero rather than anything meaningful. The mask keeps these out of the
  // record catalogue's vote, and for anything that reads the bytes themselves
  // NUL is X3.110 §6.1.4's transparent control — no presentation effect — so a
  // hole in NAPLPS code costs the drawing nothing beyond what was actually
  // lost.
  group.stream.insert(group.stream.end(), bytes, 0);
  group.present.insert(group.present.end(), bytes, 0);
  group.confidence.insert(group.confidence.end(), bytes, 0);
}

void NabtsGroupAssembler::emit(uint16_t channel, OpenGroup& group,
                               NabtsGroupOutcome outcome) {
  NabtsDataGroup out;
  out.channel = channel;
  out.channel_attested = group.channel_attested;
  out.header = group.header;
  out.outcome = outcome;
  out.packets = group.packets;
  out.blocks_corrected = group.blocks_corrected;
  out.blocks_damaged = group.blocks_damaged;
  out.packets_lost = group.packets_lost;

  // §4.2.6 and §8.4.2.6: F1,F2 is the useful length of the final non-zero data
  // block, and everything before that block is full. Greater than a block is
  // read as full, zero discards the block entirely.
  size_t useful_end = group.last_nonzero_offset + group.last_nonzero_length;
  if (group.header.final_block_bytes == 0) {
    useful_end = group.last_nonzero_offset;
  } else if (group.header.final_block_bytes < group.last_nonzero_length) {
    useful_end = group.last_nonzero_offset + group.header.final_block_bytes;
  }
  useful_end = std::min(useful_end, group.stream.size());

  // The header bytes are part of the block they arrived in but not part of the
  // group's data (§4.2.1, §5.1): the record starts where they end.
  if (useful_end > kNabtsGroupHeaderBytes) {
    out.data.assign(
        group.stream.begin() + static_cast<ptrdiff_t>(kNabtsGroupHeaderBytes),
        group.stream.begin() + static_cast<ptrdiff_t>(useful_end));
    // Trimmed identically, so an index into one is an index into the other.
    out.present.assign(
        group.present.begin() + static_cast<ptrdiff_t>(kNabtsGroupHeaderBytes),
        group.present.begin() + static_cast<ptrdiff_t>(useful_end));
    out.confidence.assign(
        group.confidence.begin() +
            static_cast<ptrdiff_t>(kNabtsGroupHeaderBytes),
        group.confidence.begin() + static_cast<ptrdiff_t>(useful_end));
  }

  switch (outcome) {
    case NabtsGroupOutcome::kComplete:
      ++stats_.groups_completed;
      if (group.header.type != kNabtsBroadcastGroupType) {
        ++stats_.non_teletext_groups;
      }
      break;
    case NabtsGroupOutcome::kSuperseded:
      ++stats_.groups_superseded;
      break;
    case NabtsGroupOutcome::kUnfinished:
      ++stats_.groups_unfinished;
      break;
  }

  if (callback_) {
    callback_(out);
  }
}

void NabtsGroupAssembler::begin_group(const NabtsPacket& packet) {
  const NabtsGroupHeader header =
      nabts_decode_group_header(packet.data.data(), packet.data_length);
  if (!header.valid) {
    ++stats_.header_failures;
    return;
  }
  if (header.further_blocks > kNabtsMaxFurtherBlocks) {
    // §8.4.2.5 caps the group at 68 packets. A larger claim is a misread header
    // or a service this cannot follow; either way it is refused before it can
    // reserve the memory it asked for.
    ++stats_.oversized_groups;
    return;
  }

  // A synchronizing packet for a channel that already has one open ends that
  // group (§4.1: the next group's start is the previous one's end).
  const auto existing = open_.find(packet.channel);
  if (existing != open_.end()) {
    emit(packet.channel, existing->second, NabtsGroupOutcome::kSuperseded);
    open_.erase(existing);
  } else if (open_.size() >= kNabtsMaxOpenGroups) {
    ++stats_.refused_groups;
    return;
  }

  OpenGroup group;
  group.header = header;
  group.channel_attested = packet.address_attested;
  group.last_continuity = packet.continuity;
  group.last_line = line_clock_;
  group.packets = 1;
  // Reserved from the header's own claim rather than the standard's ceiling, so
  // a two-packet group costs two packets' worth.
  group.stream.reserve(std::min(kNabtsMaxGroupBytes,
                                static_cast<size_t>(header.further_blocks + 1) *
                                    kNabtsMaxDataBlockBytes));
  // The width a lost block of this group is assumed to have had: §4.2 has a
  // group's packets share a suffix code, and the synchronizing packet is the
  // one whose code is certainly this group's.
  group.nominal_block_bytes = packet.data_length;
  if (packet.integrity == NabtsBlockIntegrity::kCorrected) {
    ++group.blocks_corrected;
  } else if (packet.integrity == NabtsBlockIntegrity::kUncorrectable) {
    ++group.blocks_damaged;
  }
  append_block(group, packet);

  // S1,S2 = 0 is a whole group in one packet, so it is complete on arrival.
  if (header.further_blocks == 0) {
    emit(packet.channel, group, NabtsGroupOutcome::kComplete);
    return;
  }
  open_.emplace(packet.channel, std::move(group));
}

void NabtsGroupAssembler::extend_group(const NabtsPacket& packet) {
  const auto it = open_.find(packet.channel);
  if (it == open_.end()) {
    // No group open on this channel. Normal at the head of a recording, which
    // starts part way through whatever was being transmitted.
    ++stats_.orphan_packets;
    return;
  }
  OpenGroup& group = it->second;

  // §3.2.4: the continuity index increments once per packet of the channel, so
  // the gap is how many never arrived. It wraps at 16, which bounds what can be
  // detected: a loss of exactly 16 packets reads as none.
  const uint64_t lines = line_clock_ - group.last_line - 1;
  const uint8_t gap = continuity_gap(group, packet, lines);
  if (gap != 0) {
    group.packets_lost += gap;
    // The lost packets carried blocks this group was promised, so they count
    // towards its size — otherwise a group missing packets would never reach
    // its block count and would only ever end superseded.
    const uint32_t room = static_cast<uint32_t>(group.header.further_blocks) -
                          group.further_blocks_seen;
    group.further_blocks_seen = static_cast<uint16_t>(std::min<uint32_t>(
        group.further_blocks_seen + gap, group.header.further_blocks));
    // A gap wider than the group has blocks left is not a loss the header can
    // account for: §3.2.4's index wraps at 16, so a damaged continuity nibble
    // reads as a gap just as a real loss does, and only the header says which
    // is possible. Where it says the gap cannot be real, nothing after it can
    // be placed.
    if (gap > room) {
      group.placeable = false;
    } else {
      append_hole(group, gap);
    }
  }
  // The index this packet had, which is the one it carried unless that one was
  // misread; the next packet is judged against it either way.
  group.last_continuity = static_cast<uint8_t>(
      (group.last_continuity + 1 + gap) % kNabtsContinuityModulus);
  group.last_line = line_clock_;
  ++group.packets;

  if (packet.integrity == NabtsBlockIntegrity::kCorrected) {
    ++group.blocks_corrected;
  } else if (packet.integrity == NabtsBlockIntegrity::kUncorrectable) {
    ++group.blocks_damaged;
  }

  append_block(group, packet);
  // §4.2.5 counts every block towards the size, "including any Data Blocks of
  // zero length that occur with a 28-byte Suffix", so a bundle packet advances
  // this even though it contributed no bytes.
  ++group.further_blocks_seen;

  if (group.further_blocks_seen >= group.header.further_blocks) {
    emit(packet.channel, group, NabtsGroupOutcome::kComplete);
    open_.erase(it);
  }
}

uint8_t NabtsGroupAssembler::continuity_gap(const OpenGroup& group,
                                            const NabtsPacket& packet,
                                            uint64_t lines) {
  const uint8_t expected = static_cast<uint8_t>((group.last_continuity + 1) %
                                                kNabtsContinuityModulus);
  const uint8_t read = static_cast<uint8_t>(
      (packet.continuity + kNabtsContinuityModulus - expected) %
      kNabtsContinuityModulus);
  if (read <= lines) {
    return read;
  }

  // More packets lost than there were lines to lose them on: the index was
  // misread, which Hamming 8/4's distance of 4 lets a three-bit burst do
  // without a trace. On a band-limited recording that is common enough that
  // believing it would displace the rest of the group by up to fifteen packets,
  // and every copy of the record so displaced votes against the ones that were
  // not. The losses that could have happened are the gaps up to |lines| — and
  // the group's remaining blocks, beyond which none can be placed anyway — so
  // the reading taken is whichever of those the byte received lies nearest to,
  // and the smaller loss where two are equally near, since a packet arriving
  // is the likelier event than one going missing.
  ++stats_.continuity_misreads;
  const uint32_t room = static_cast<uint32_t>(group.header.further_blocks) -
                        group.further_blocks_seen;
  const uint64_t limit =
      std::min<uint64_t>({lines, room, kNabtsContinuityModulus - 1U});
  uint8_t best = 0;
  int best_distance = std::numeric_limits<int>::max();
  for (uint64_t candidate = 0; candidate <= limit; ++candidate) {
    const uint8_t index =
        static_cast<uint8_t>((expected + candidate) % kNabtsContinuityModulus);
    const std::bitset<8> differing(static_cast<unsigned>(
        packet.continuity_byte ^ teletext_hamming84_encode(index)));
    const int distance = static_cast<int>(differing.count());
    if (distance < best_distance) {
      best = static_cast<uint8_t>(candidate);
      best_distance = distance;
    }
  }
  return best;
}

void NabtsGroupAssembler::add_packet(const NabtsPacket& packet) {
  ++stats_.packets_seen;
  ++line_clock_;  // A line carried this, whether or not it can be used.
  if (!packet.valid) {
    ++stats_.prefix_failures;
    return;
  }
  if (packet.suffix == NabtsSuffixKind::kBundle) {
    ++stats_.bundle_packets;
  }

  if (packet.synchronizing) {
    begin_group(packet);
  } else {
    extend_group(packet);
  }
}

void NabtsGroupAssembler::flush() {
  for (auto& entry : open_) {
    emit(entry.first, entry.second, NabtsGroupOutcome::kUnfinished);
  }
  open_.clear();
}

}  // namespace orc
