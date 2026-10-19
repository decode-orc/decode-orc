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
#include <limits>
#include <optional>
#include <utility>

#include "nabts_service_census.h"
#include "vbi-services/teletext_page_decoder.h"
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

NabtsSerialEvidence nabts_group_serial_evidence(const NabtsDataGroup& group) {
  NabtsSerialEvidence evidence;
  for (size_t i = 0; i + 1 < group.data.size(); ++i) {
    // A hole is zeros standing for bytes that never arrived, and says nothing
    // of the pattern; nor does a byte repeated, which the pattern seldom does
    // and teletext often.
    const bool arrived =
        group.present.empty() || (group.present[i] != kNabtsByteLost &&
                                  group.present[i + 1] != kNabtsByteLost);
    if (!arrived || group.data[i] == group.data[i + 1]) {
      continue;
    }
    ++evidence.pairs;
    if (nabts_serial_pattern_step(group.data[i], group.data[i + 1])) {
      ++evidence.steps;
    }
  }
  return evidence;
}

bool nabts_group_is_idle_fill(const NabtsDataGroup& group) {
  return nabts_group_serial_evidence(group).is_pattern();
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
        "were lines for; after one, the rest of its group left unplaced\n",
        continuity_misreads);
  }
  if (continuity_wraps > 0) {
    out += fmt::format(
        "  Continuity:    {} gap(s) the index could have come round over (the "
        "empty lines account for 16 more packets lost than it reads); after "
        "one, the rest of its group left unplaced\n",
        continuity_wraps);
  }
  if (idle_fill_groups > 0) {
    out += fmt::format(
        "  Idle fill:     {} groups of an encoder's test pattern, not "
        "passed on as records\n",
        idle_fill_groups);
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
  // Where they are, the mask also carries what the suffix check made of their
  // block, which is what lets the vote believe a vouched byte over any number
  // of copies that merely agree.
  uint8_t level = kNabtsByteLost;
  if (group.placeable) {
    level =
        block_vouched(group, packet) ? kNabtsByteVouched : kNabtsByteArrived;
  }
  const size_t first = group.present.size();
  group.present.insert(group.present.end(), packet.data_length, level);
  // A byte the detector's wide reading changed passes its parity, and its
  // block its check, because it was read to: neither says it is right, and
  // the mask says so, for the vote and the comparisons of contents not to take
  // its parity for evidence (kNabtsByteReread).
  if (level != kNabtsByteLost && packet.reread != 0) {
    for (size_t i = 0; i < packet.data_length; ++i) {
      if (((packet.reread >> i) & 1U) != 0) {
        group.present[first + i] = kNabtsByteReread;
      }
    }
  }
  group.confidence.insert(
      group.confidence.end(), packet.confidence.begin(),
      packet.confidence.begin() + static_cast<ptrdiff_t>(packet.data_length));
}

NabtsPacket NabtsGroupAssembler::at_group_width(const OpenGroup& group,
                                                const NabtsPacket& packet) {
  // A bundle packet's block is empty by its PS byte and counted as one all the
  // same (§4.2.5); a group of bundles and data blocks is how a service sends
  // forward error correction, so its width is no evidence of a misread.
  if (packet.data_length == 0 || group.nominal_block_bytes == 0 ||
      packet.data_length == group.nominal_block_bytes) {
    return packet;
  }
  // §3.4 makes the suffix a property of the packet, but a group's packets share
  // one in practice (see OpenGroup::nominal_block_bytes), and a PS byte is a
  // Hamming 8/4 byte a burst can mis-correct like any other. A block read at
  // the width its misread PS byte claims moves every byte after it in the group
  // by one or two, and the copy agrees with the record's other copies up to
  // there and with nothing after; read at the group's width it stays in place.
  // Across the recordings measured 4,166 blocks arrived with a width their
  // group's synchronizing packet contradicts, nearly all on recordings sent
  // with the check, and reading them at the group's width took the bytes the
  // vote got wrong on those from 140,929 to 131,413, measured against the
  // other recordings of the same day. The check the misread PS byte made over
  // the block is no evidence either way, so the block is not vouched for.
  NabtsPacket out = packet;
  out.data_length = group.nominal_block_bytes;
  out.integrity = NabtsBlockIntegrity::kUncorrectable;
  return out;
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

void NabtsGroupAssembler::tally_integrity(OpenGroup& group,
                                          const NabtsPacket& packet) {
  if (packet.read_under_check || packet.reread != 0) {
    ++group.blocks_read_under_check;
  }
  switch (packet.integrity) {
    case NabtsBlockIntegrity::kCorrected:
      ++group.blocks_corrected;
      break;
    case NabtsBlockIntegrity::kUncorrectable:
      ++group.blocks_damaged;
      break;
    case NabtsBlockIntegrity::kUnchecked:
      // A bundle packet is unchecked too, but it carries no block to vouch
      // for (§3.4), so only a block with bytes in it counts.
      if (packet.data_length > 0) {
        ++group.blocks_unchecked;
      }
      break;
    case NabtsBlockIntegrity::kClean:
      if (!block_vouched(group, packet)) {
        ++group.blocks_damaged;
      }
      break;
  }
}

bool NabtsGroupAssembler::block_vouched(const OpenGroup& group,
                                        const NabtsPacket& packet) {
  switch (packet.integrity) {
    case NabtsBlockIntegrity::kCorrected:
      // Exactly one byte failed its parity, and the correction was what put
      // it right, so every byte now has it.
      return true;
    case NabtsBlockIntegrity::kClean:
      break;
    default:
      return false;
  }
  if (group.header.type != kNabtsBroadcastGroupType) {
    return true;  // §3.3 asks no parity of this group's bytes
  }
  // A longitudinal parity that passes only says every bit position is wrong an
  // even number of times, which two wrong bits in one column are. §3.3's odd
  // parity finds each byte they fell in.
  return std::all_of(
      packet.data.begin(),
      packet.data.begin() + static_cast<ptrdiff_t>(packet.data_length),
      [](uint8_t byte) { return teletext_odd_parity_valid(byte); });
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
  out.blocks_unchecked = group.blocks_unchecked;
  out.packets_lost = group.packets_lost;
  out.placed = group.placeable;
  out.blocks_read_under_check = group.blocks_read_under_check;

  // A group that ended short of its block count (§4.2.5) lost the packets it
  // was still owed, and its header says how many: the next synchronizing packet
  // on the channel ends it before any packet of its own could carry a
  // continuity index past the loss. The blocks are held open as a hole at the
  // tail, as a loss inside the group is (append_hole()), so the group keeps the
  // length its header declared and the final block keeps its place for F1,F2
  // below. Closed up instead, every copy that lost its last packet came out a
  // block short and a linked record moved every later link with it. Measured
  // against the other recordings of the same day, holding the tail open took
  // the bytes the vote got wrong on recordings sent with the check from
  // 140,929 to 134,003 and put 17,336 more right; on recordings that lose
  // fewer packets at a group's end, it changes little either way.
  if (outcome != NabtsGroupOutcome::kComplete &&
      group.header.further_blocks > group.further_blocks_seen &&
      group.nominal_block_bytes > 0) {
    append_hole(group, static_cast<uint32_t>(group.header.further_blocks -
                                             group.further_blocks_seen));
    group.last_nonzero_offset = group.stream.size() - group.nominal_block_bytes;
    group.last_nonzero_length = group.nominal_block_bytes;
  }

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
      break;
    case NabtsGroupOutcome::kSuperseded:
      ++stats_.groups_superseded;
      break;
    case NabtsGroupOutcome::kUnfinished:
      ++stats_.groups_unfinished;
      break;
  }

  if (withhold_as_idle_fill(channel, out)) {
    ++stats_.idle_fill_groups;
    return;
  }
  if (callback_) {
    callback_(out);
  }
}

bool NabtsGroupAssembler::withhold_as_idle_fill(uint16_t channel,
                                                const NabtsDataGroup& group) {
  const NabtsSerialEvidence evidence = nabts_group_serial_evidence(group);
  if (evidence.is_pattern()) {
    idle_fill_channels_.insert(channel);
    return true;
  }
  // A group that lost most of its packets, or was superseded early, may have
  // too few bytes to judge. On a channel already found carrying fill it is
  // taken for more of it; a group judged on its own bytes not to be fill is
  // delivered wherever it was sent.
  return evidence.pairs < kNabtsIdleFillMinPairs &&
         idle_fill_channels_.count(channel) > 0;
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
  group.last_silent = silent_clock_;
  group.first_data_line = data_line_clock_;
  group.last_data_line = data_line_clock_;
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
  tally_integrity(group, packet);
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
  const uint64_t lines = lines_unknown_ ? std::numeric_limits<uint64_t>::max()
                                        : line_clock_ - group.last_line - 1;
  // A stream that keeps no record of its empty lines says nothing of where
  // losses fell either, so no gap is taken for one the index came round over.
  const uint64_t silent =
      lines_unknown_ ? 0 : silent_clock_ - group.last_silent;
  // Once nothing can be placed, what the index says of losses no longer
  // matters: the group's bytes are all counted as lost from here on.
  const std::optional<uint8_t> read =
      group.placeable ? continuity_gap(group, packet, lines, silent)
                      : std::optional<uint8_t>{0};
  if (!read) {
    // A misread index some loss could have hidden behind: how many packets
    // went missing cannot be told, so nothing from here on can be placed.
    group.placeable = false;
  }
  const uint8_t gap = read.value_or(0);
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
  group.last_silent = silent_clock_;
  group.last_data_line = data_line_clock_;
  ++group.packets;

  const NabtsPacket block = at_group_width(group, packet);
  tally_integrity(group, block);

  append_block(group, block);
  // §4.2.5 counts every block towards the size, "including any Data Blocks of
  // zero length that occur with a 28-byte Suffix", so a bundle packet advances
  // this even though it contributed no bytes.
  ++group.further_blocks_seen;

  if (group.further_blocks_seen >= group.header.further_blocks) {
    emit(packet.channel, group, NabtsGroupOutcome::kComplete);
    open_.erase(it);
  }
}

std::optional<uint8_t> NabtsGroupAssembler::continuity_gap(
    const OpenGroup& group, const NabtsPacket& packet, uint64_t lines,
    uint64_t silent) {
  const uint8_t expected = static_cast<uint8_t>((group.last_continuity + 1) %
                                                kNabtsContinuityModulus);
  const uint8_t read = static_cast<uint8_t>(
      (packet.continuity + kNabtsContinuityModulus - expected) %
      kNabtsContinuityModulus);
  if (read <= lines) {
    // §3.2.4's index counts modulo 16, so it reads a loss of sixteen more
    // packets exactly as it reads this one, and a tape that loses a field in
    // every frame, or a run of frames with no data at all, can hide a whole
    // index's worth: the rest of this group and the start of the next, whose
    // synchronizing packet went with them. Taken at the smaller reading, the
    // next record's packets are placed as this one's. How many of this
    // group's packets the empty lines could have carried is estimated from
    // the share of the lines that carry data its blocks have taken since it
    // began — all of them on a channel that fills its lines, as a page
    // service's does, a few on one whose packets come seldom, as an encoder's
    // idle fill does. Where that accounts for the larger reading, how many
    // were lost cannot be told, and the rest of the group is left unplaced, as
    // after a misread index below. On one card dump seven pages were each
    // listed a second time as a copy that ran on into the page sent after it:
    // one read index 8 then B across 23 empty lines — two packets lost as
    // read, eighteen as sent — and went on to take another page's packets.
    const uint64_t data_lines = group.last_data_line - group.first_data_line;
    if (group.further_blocks_seen > 0 && data_lines > 0 &&
        static_cast<double>(silent) * group.further_blocks_seen >=
            static_cast<double>(read + kNabtsContinuityModulus) *
                static_cast<double>(data_lines)) {
      ++stats_.continuity_wraps;
      return std::nullopt;
    }
    return read;
  }

  // More packets lost than there were lines to lose them on: the index was
  // misread, which Hamming 8/4's distance of 4 lets a three-bit burst do
  // without a trace. Believing it would displace the rest of the group by up
  // to fifteen packets. Where no line went by since the group's last packet,
  // none of its packets can have been lost there, and this one is the next
  // whatever its index says. Otherwise some loss was possible, and which one
  // the damaged byte stood for is a guess — one that, taken, puts every byte
  // after it in the wrong place and lets the copy vote there. The caller
  // leaves the rest of the group unplaced instead: bytes known to be missing
  // can be filled from another recording, bytes in the wrong place cannot be
  // told from right ones.
  ++stats_.continuity_misreads;
  if (lines == 0) {
    return 0;
  }
  return std::nullopt;
}

void NabtsGroupAssembler::add_packet(const NabtsPacket& packet) {
  ++stats_.packets_seen;
  ++line_clock_;  // A line carried this, whether or not it can be used.
  ++data_line_clock_;
  if (!packet.valid) {
    // Whose packet it was cannot be told, so any group's could have been lost
    // here.
    ++silent_clock_;
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
