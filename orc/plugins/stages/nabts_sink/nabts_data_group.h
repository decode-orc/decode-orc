/*
 * File:        nabts_data_group.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Reassemble NABTS data groups from a packet stream (CEA-516 §4)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_DATA_GROUP_H
#define ORC_NABTS_DATA_GROUP_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "nabts_packet.h"

namespace orc {

// CEA-516 §4.2.1: the data group header is eight Hamming 8/4 bytes — GT, GC,
// GR, S1, S2, F1, F2, GN — at the head of the synchronizing packet's data
// block.
constexpr size_t kNabtsGroupHeaderBytes = 8;

// CEA-516 §4.2.2: zero is the group type broadcast teletext uses, and §8.4.2.2
// is the only type an FSS receiver accepts. Fifteen is reserved for the service
// provider's private use and will never be standardized.
constexpr uint8_t kNabtsBroadcastGroupType = 0;
constexpr uint8_t kNabtsPrivateGroupType = 15;

// The values of NabtsDataGroup::present, and of every mask the record layer
// derives from it. A byte a lost packet carried; a byte that arrived; a byte
// that arrived in a data block the suffix check vouches for — its longitudinal
// parity passed as received or the product code put its one wrong bit right
// (CEA-516 §3.4), and in a type-zero group every byte also has the odd parity
// §3.3 requires; and a byte that arrived as the detector's wide reading
// changed it (NabtsPacket::reread), which passes its parity because it was
// read to. Anything that only asks whether a byte arrived reads the mask as
// non-zero; the vote and the comparisons of contents ask more (see
// nabts_vote_record()), and ask it of each value by name: the four are kinds,
// not an order.
constexpr uint8_t kNabtsByteLost = 0;
constexpr uint8_t kNabtsByteArrived = 1;
constexpr uint8_t kNabtsByteVouched = 2;
constexpr uint8_t kNabtsByteReread = 3;

// CEA-516 §8.4.2.5: at most 67 further blocks, so 68 packets, so 1904 bytes
// when no suffix is used. Both are stated because both are useful: the block
// count is what S1,S2 is checked against, and the byte count is the allocation
// bound that follows from it.
constexpr uint16_t kNabtsMaxFurtherBlocks = 67;
constexpr size_t kNabtsMaxGroupBytes = 1904;

// Groups held open at once. A group stays open from its synchronizing packet
// until its last block arrives, and packets of different channels interleave
// freely, so one per channel may legitimately be in flight. §3.2.3 permits 4096
// channels; a real service uses a handful and a misread prefix invents the
// rest, so this is what stops noise from opening thousands of buffers. At
// kNabtsMaxGroupBytes each, the ceiling is about 61 kB.
constexpr size_t kNabtsMaxOpenGroups = 32;

// Pairs of unequal neighbouring bytes a group's data needs before it can be
// told for an encoder's idle fill, and the share of them that must step as the
// fill's pattern steps (nabts_serial_pattern_step()). Norpak BERT groups are
// 65 blocks long, some 1,700 pairs, 99.6 % of them steps; no address of any
// other recording measured reaches 7 %.
constexpr size_t kNabtsIdleFillMinPairs = 64;
constexpr size_t kNabtsIdleFillMinStepPercent = 50;

/// The eight bytes of CEA-516 §4.2, decoded.
struct NabtsGroupHeader {
  /// All eight bytes survived Hamming 8/4 correction.
  bool valid = false;
  /// All eight arrived as Hamming 8/4 codewords rather than being corrected
  /// into them — the same test NabtsPacket::address_attested applies to the
  /// packet address, and for the same reason: a byte three bits wrong corrects
  /// silently into a neighbouring value, so only an uncorrected header vouches
  /// for what it says.
  bool attested = false;
  /// Whether GT alone arrived as a codeword (see @ref attested). A type read
  /// from a corrected byte may be a neighbouring codeword's, which is what
  /// lets the assembler read one on a teletext channel as zero
  /// (NabtsGroupAssembler::begin_group()).
  bool type_attested = false;
  /// GT (§4.2.2).
  uint8_t type = 0;
  /// GC (§4.2.3). §8.4.2.3 lets a service leave this un-incremented, so it is
  /// reported rather than relied on.
  uint8_t continuity = 0;
  /// GR (§4.2.4): non-zero means the group is expected to repeat. Decoded,
  /// and not acted on.
  uint8_t repetition = 0;
  /// S1,S2 (§4.2.5): data blocks following the synchronizing packet's own.
  uint16_t further_blocks = 0;
  /// F1,F2 (§4.2.6): bytes of useful data in the final non-zero data block.
  uint16_t final_block_bytes = 0;
  /// GN (§4.2.7). §8.4.2.7 has FSS receivers ignore it, and so does this.
  uint8_t routing = 0;
};

/**
 * @brief Decode a data group header
 *
 * @param bytes  Head of the synchronizing packet's data block
 * @param length Bytes available; fewer than kNabtsGroupHeaderBytes yields an
 *               invalid header rather than a partial read
 */
NabtsGroupHeader nabts_decode_group_header(const uint8_t* bytes, size_t length);

/// Why a group ended.
enum class NabtsGroupOutcome {
  /// Every block S1,S2 promised arrived.
  kComplete,
  /// A synchronizing packet for the same channel arrived first, so this group
  /// will never be finished. Its bytes are handed on anyway — a truncated
  /// record header still identifies its record — held at the length its header
  /// declared (see NabtsDataGroup::data).
  kSuperseded,
  /// The pass ended with the group still open.
  kUnfinished,
};

/// One reassembled data group.
struct NabtsDataGroup {
  /// Channel the group's packets were addressed to (§3.2.3).
  uint16_t channel = 0;
  /// Whether the synchronizing packet's address arrived as Hamming 8/4
  /// codewords rather than being corrected into them (see
  /// NabtsPacket::address_attested). The synchronizing packet's own address is
  /// what opened the group, so it is the one the group's channel rests on.
  bool channel_attested = false;
  NabtsGroupHeader header;
  NabtsGroupOutcome outcome = NabtsGroupOutcome::kComplete;

  /// Group data: everything after the eight header bytes, trimmed to the
  /// useful length F1,F2 states (§4.2.6, §8.4.2.6). For a type-zero group this
  /// is exactly one teletext record (§5.1).
  ///
  /// A group that ended before its block count S1,S2 was reached (§4.2.5) —
  /// superseded by the next synchronizing packet on its channel, or still open
  /// when the pass ended — is held at the length its header declared, the
  /// blocks it never received as a hole at its tail (see @ref present). The
  /// packets a group loses last are the commonest loss of all, and no later
  /// packet's continuity index is there to say they went missing; closed up,
  /// the group would come out a block or more short, and a linked record
  /// (§5.2.6) would have every link after it moved earlier by as much.
  std::vector<uint8_t> data;

  /// One entry per byte of @ref data: non-zero where that byte arrived, zero
  /// where it stands in for one a lost packet carried — kNabtsByteReread where
  /// the detector's wide reading changed it (NabtsPacket::reread),
  /// kNabtsByteVouched where its block passed the suffix check otherwise,
  /// kNabtsByteArrived where it did not or carried none, kNabtsByteLost in a
  /// hole. The assembler fills it for every group; an empty one, which only a
  /// group built by hand has, reads as every byte arrived.
  ///
  /// §3.2.4's continuity index says how many packets never arrived, so a hole
  /// can be held open at the width the lost blocks would have filled instead of
  /// letting the bytes after it close up. That keeps every later byte at the
  /// offset the record gave it, which is what lets two copies of one record be
  /// compared position for position however much each of them lost. The width
  /// is an estimate — the lost packets' own suffix codes went with them, so the
  /// group's nominal block length stands in — and where the continuity index
  /// disagrees with the block count the header declared it is not trusted at
  /// all, and the rest of the group is marked absent rather than misplaced.
  std::vector<uint8_t> present;

  /// One entry per byte of @ref data: how sure the detector was of it, on the
  /// 0-255 scale of NabtsPacket::confidence. Zero where the byte never arrived.
  /// Empty when nothing measured any of them, which is read as full confidence
  /// throughout.
  std::vector<uint8_t> confidence;

  /// Packets that contributed, including the synchronizing packet.
  uint32_t packets = 0;
  /// Data blocks whose suffix check the product code had to repair.
  uint32_t blocks_corrected = 0;
  /// Data blocks whose suffix check failed and could not be repaired. Their
  /// bytes are present as transmitted, so a caller may still get a usable
  /// record out of a group with a few of these. In a type-zero group a block
  /// whose longitudinal parity passed with a byte failing its own parity is
  /// one of these: the wrong bits fell in pairs down each column they reached,
  /// which the longitudinal check cannot see and §3.3's parity can.
  uint32_t blocks_damaged = 0;
  /// Data blocks that carried bytes but no suffix (PS b8,b6 = 0,0). CEA-516
  /// §3.4 makes the suffix the receiver's means of detecting errors in the
  /// block, so nothing checked these: they are not known to be damaged, and
  /// not known to be undamaged either.
  uint32_t blocks_unchecked = 0;
  /// Packets the continuity index says never arrived (§3.2.4). A group with any
  /// is missing bytes from somewhere in the middle of |data|.
  uint32_t packets_lost = 0;
  /// Whether every byte that arrived could be put where it belongs. False once
  /// a continuity index could not say how many packets went missing ahead of
  /// it (NabtsGroupAssembler::continuity_gap()): the rest of the group's bytes
  /// are then marked lost (see @ref present), and the group, whatever else it
  /// shows, did not arrive whole.
  bool placed = true;
  /// Data blocks that pass their suffix check only as the detector read them
  /// again under it (NabtsPacket::read_under_check), or that its wide reading
  /// changed (NabtsPacket::reread).
  uint32_t blocks_read_under_check = 0;

  /// Whether the group arrived whole with every data block vouched for by its
  /// suffix check. A block without a suffix is not known to be damaged, but
  /// nothing vouches for it either, so a group holding one is never intact:
  /// a service that sends no suffix at all has its copies combined rather than
  /// any one of them believed (see NabtsRecordCatalogue). Nor is a group
  /// holding a block read under its check: what the record catalogue takes an
  /// intact copy's length to prove — that no block went missing on the way —
  /// rested on its damage showing, and a copy that lost a packet without a gap
  /// in the continuity index shows none once its other blocks are read to pass.
  /// On one tape that turned eleven such copies, each one data block short,
  /// into contents of their own.
  bool intact() const {
    return outcome == NabtsGroupOutcome::kComplete && placed &&
           packets_lost == 0 && blocks_damaged == 0 && blocks_unchecked == 0 &&
           blocks_read_under_check == 0;
  }
};

/// What a pass over a packet stream made of it.
struct NabtsGroupStats {
  uint64_t packets_seen = 0;
  /// Packets whose prefix did not survive Hamming 8/4 (§3.2.2).
  uint64_t prefix_failures = 0;
  /// Synchronizing packets whose group header did not decode (§4.2.1).
  uint64_t header_failures = 0;
  /// Synchronizing packets claiming more than §8.4.2.5 permits.
  uint64_t oversized_groups = 0;
  /// Synchronizing packets refused because kNabtsMaxOpenGroups were already
  /// open.
  uint64_t refused_groups = 0;
  /// Standard packets arriving on a channel with no group open — the normal
  /// state of affairs when a recording starts part way through a group.
  uint64_t orphan_packets = 0;
  /// Bundle packets (§3.4 PS 1,1): counted, continuity consumed, contents
  /// skipped.
  uint64_t bundle_packets = 0;
  /// Continuity indices that claimed more packets lost than there were lines
  /// for them to have been lost on, and so were misread rather than believed:
  /// read as the next packet where no line went by, and otherwise leaving the
  /// rest of their group unplaced.
  uint64_t continuity_misreads = 0;
  /// Continuity gaps the index could have come round over: the lines that went
  /// by without a packet account for sixteen more lost than it reads
  /// (§3.2.4), so how many were lost cannot be told and the rest of the group
  /// was left unplaced.
  uint64_t continuity_wraps = 0;
  uint64_t groups_completed = 0;
  uint64_t groups_superseded = 0;
  uint64_t groups_unfinished = 0;
  /// Groups of an encoder's idle fill (nabts_group_is_idle_fill()), counted
  /// above as any group is and withheld from the callback.
  uint64_t idle_fill_groups = 0;
  /// Packets flagged synchronizing whose group header did not decode, taken
  /// as the next block of the group open on their channel: a data packet
  /// whose PS byte was misread (see NabtsGroupAssembler::begin_group()).
  /// Counted here and not under header_failures.
  uint64_t synchronizing_misreads = 0;
  /// Groups on a teletext channel whose type byte was corrected into a value
  /// other than zero, read as type zero (NabtsGroupAssembler::begin_group()).
  uint64_t type_misreads = 0;
  /// Runs of packets that arrived with no group open on their channel and
  /// were handed on as loose runs (NabtsLooseRun), and the packets in them.
  uint64_t loose_runs = 0;
  uint64_t loose_packets = 0;

  /// Human-readable summary for the stage report.
  std::string summary() const;
};

/**
 * @brief Whether a group's data is an encoder's idle fill
 *
 * An encoder with nothing to send fills its lines with a bit-error-rate test
 * pattern, and the Ross Video (Norpak) encoders send it as data groups of type
 * zero on address F00 (TES3/TES5 Software User Manual Iss. 02 p. 10-3): to the
 * record layer, a broadcast teletext group like any other, whose bytes would
 * become a record no service ever sent — hundreds of them on one recording. Its
 * bytes give it away: each is the one before it shifted a place
 * (nabts_serial_pattern_step()), where teletext's neighbours relate that way by
 * chance, one pair in sixty-four.
 *
 * Judged over the pairs of unequal neighbouring bytes that both arrived; a
 * group with fewer than kNabtsIdleFillMinPairs of them is too short to say.
 */
bool nabts_group_is_idle_fill(const NabtsDataGroup& group);

/**
 * @brief What a run of bytes says of the byte-serial test pattern
 *
 * The pairs of unequal neighbouring bytes judged, and how many of them are
 * steps of the pattern (nabts_serial_pattern_step()).
 */
struct NabtsSerialEvidence {
  uint64_t pairs = 0;
  uint64_t steps = 0;

  /// Enough pairs to judge by (kNabtsIdleFillMinPairs), and enough of them
  /// steps (kNabtsIdleFillMinStepPercent), to make the bytes the pattern.
  bool is_pattern() const {
    return pairs >= kNabtsIdleFillMinPairs &&
           steps * 100 >= pairs * kNabtsIdleFillMinStepPercent;
  }
};

/// @p group's evidence, over the pairs of unequal neighbouring bytes of its
/// data that both arrived. nabts_group_is_idle_fill() is its is_pattern().
NabtsSerialEvidence nabts_group_serial_evidence(const NabtsDataGroup& group);

// Loose runs held open at once, one per channel, as groups are
// (kNabtsMaxOpenGroups): a misread packet address files a packet under a
// channel no group is open on, and a run is only ever started on a channel
// that has carried teletext, so in practice one or two are open.
constexpr size_t kNabtsMaxOpenLooseRuns = 8;

// Packets a loose run may hold a hole for. A group's own losses are bounded by
// its header; a loose run has none, and a longer gap in its continuity index is
// as likely the next group's packets — whose synchronizing packet went too, on
// a recording that loses one line in every field — as more of this one's:
// read as one run, two pages' packets match neither page. A recording losing
// one line of three leaves gaps of one packet.
constexpr uint8_t kNabtsLooseRunMaxGap = 2;

/**
 * @brief Packets of a teletext channel that arrived with no group open
 *
 * The body of a group whose synchronizing packet never arrived — lost on a line
 * the recording does not keep, or read with a header that did not decode — in
 * the order it was sent, with a hole wherever the continuity index says
 * packets went missing, as a group holds its own losses
 * (NabtsDataGroup::present). Without the group header nothing says how long the
 * group was, and without the record header nothing says which record the bytes
 * are or where in it they start: whoever takes the run has to find that out
 * from the bytes themselves (see NabtsSalvage).
 *
 * A record's first block carries its group header and record header, so a run
 * whose synchronizing packet was lost is a record with its head missing; one
 * whose synchronizing packet arrived with a header that did not decode starts
 * with that packet, its eight header bytes included.
 */
struct NabtsLooseRun {
  uint16_t channel = 0;
  /// Data-block bytes, holes and all, at the run's first block width.
  std::vector<uint8_t> data;
  /// As NabtsDataGroup::present and NabtsDataGroup::confidence, one entry per
  /// byte of @ref data.
  std::vector<uint8_t> present;
  std::vector<uint8_t> confidence;
  /// Packets that arrived in the run.
  uint32_t packets = 0;
  /// The continuity index of the run's first packet (§3.2.4).
  uint8_t first_continuity = 0;
  /// Whether the run's first packet was flagged synchronizing, so that its
  /// first kNabtsGroupHeaderBytes bytes are a group header that did not
  /// decode rather than data.
  bool starts_synchronizing = false;
  /// Data-block bytes per packet: the first packet's, which a lost packet's
  /// hole is held open at.
  size_t block_bytes = 0;
};

/**
 * @brief Turns a temporally ordered packet stream into data groups
 *
 * Fed packets in the order they were broadcast — frame, then field, then
 * ascending line, which is the order nabts_sink emits them — and calls back
 * once per group as it completes.
 *
 * Groups of different channels interleave, so one is held open per channel
 * (§4.1: a group is the packets of one packet address). Loss is detected from
 * the continuity index, which §3.2.4 increments per packet of a channel; a gap
 * is recorded and the group carries on, because the bytes that did arrive are
 * still worth having.
 *
 * A gap is only believed where it could have happened. A packet travels on a
 * line, so no more packets can have been lost since a group's last one than
 * there have been lines since — which the caller makes known by reporting the
 * lines that yielded no packet (add_empty_line()) as well as the ones that
 * did. A continuity index claiming more than that was misread: the packet is
 * the next where no line went by, and otherwise the rest of the group is left
 * unplaced (continuity_gap()).
 *
 * Not thread safe, and deliberately so: the assembler's whole value is that it
 * sees the stream in transmission order, which one thread walking the emitted
 * packets is what guarantees.
 */
class NabtsGroupAssembler {
 public:
  using GroupCallback = std::function<void(const NabtsDataGroup&)>;

  /// Called once per group, as it ends. A group whose type is not zero is
  /// delivered too: §4.2.2 reserves other types rather than forbidding them,
  /// and what to do with one is the caller's decision to make. A group of an
  /// encoder's idle fill is not (see nabts_group_is_idle_fill()): nothing was
  /// sent in it, and the record layer would read a record out of it all the
  /// same.
  void set_group_callback(GroupCallback callback) {
    callback_ = std::move(callback);
  }

  using LooseRunCallback = std::function<void(const NabtsLooseRun&)>;

  /// Called once per loose run (NabtsLooseRun), as it ends: at the next
  /// synchronizing packet on its channel, at a gap its continuity index cannot
  /// place, at a run as long as a group can be (§8.4.2.5), or at flush().
  /// Runs are only collected while a callback is set, and only on a channel
  /// that has carried a group of type zero whose type arrived as a codeword:
  /// a channel known to carry teletext.
  void set_loose_run_callback(LooseRunCallback callback) {
    loose_callback_ = std::move(callback);
  }

  /// Take in one packet. An invalid packet is counted and dropped: without a
  /// channel there is no group to file it under.
  void add_packet(const NabtsPacket& packet);

  /// Note a line that could have carried a packet and yielded none, in its
  /// place in the stream. Every such line is one on which a packet may have
  /// been lost; see the class comment for why that bound matters.
  ///
  /// |carries_data| is false for a line the stream has never been seen to
  /// carry data on, read only to find out whether it has started to — a pass's
  /// periodic look at the whole window. Such a line still bounds a gap as any
  /// line does, but no group's packet was lost on it, so it does not count
  /// towards a gap the index could have come round over (see
  /// continuity_gap()).
  void add_empty_line(bool carries_data = true) {
    ++line_clock_;
    if (carries_data) {
      ++silent_clock_;
      ++data_line_clock_;
    }
  }

  /// Declare that the stream carries no record of the lines that yielded no
  /// packet — a packet stream read back from a file, which holds only the
  /// packets — so a continuity gap cannot be bounded by them and is believed as
  /// read, bounded only by the blocks the group has left.
  void set_lines_unknown() { lines_unknown_ = true; }

  /// End the pass: every group still open is reported as unfinished.
  /// Idempotent.
  void flush();

  const NabtsGroupStats& stats() const { return stats_; }

 private:
  struct OpenGroup {
    NabtsGroupHeader header;
    /// The synchronizing packet's NabtsPacket::address_attested.
    bool channel_attested = false;
    /// Every data-block byte the group has yielded, the synchronizing packet's
    /// eight header bytes included, so an offset into this is an offset into
    /// the group as §4.2.6 counts it.
    std::vector<uint8_t> stream;
    /// Parallel to @ref stream: whether each byte arrived or stands in for one
    /// a lost packet carried (see NabtsDataGroup::present).
    std::vector<uint8_t> present;
    /// Parallel to @ref stream: detector confidence per byte, zero in a hole.
    std::vector<uint8_t> confidence;
    /// Data-block bytes the synchronizing packet carried, which is the width a
    /// lost block is assumed to have had. §4.2 has a group's packets share a
    /// suffix code in practice, and a lost packet's own is lost with it.
    size_t nominal_block_bytes = 0;
    /// False once the continuity index has said something the group header
    /// contradicts, after which no byte of this group can be placed.
    bool placeable = true;
    /// Blocks after the synchronizing packet's own.
    uint16_t further_blocks_seen = 0;
    /// Where the last data block with any bytes in it started, and how long it
    /// was — which is what F1,F2 is a length within (§4.2.6). The final
    /// non-zero block need not be the last packet: zero-length blocks with
    /// 28-byte suffixes may follow it.
    size_t last_nonzero_offset = 0;
    size_t last_nonzero_length = 0;
    uint8_t last_continuity = 0;
    /// The line clock (see line_clock_) at the group's last packet.
    uint64_t last_line = 0;
    /// The silent-line clock (see silent_clock_) at the group's last packet.
    uint64_t last_silent = 0;
    /// The data-line clock (see data_line_clock_) at the synchronizing packet
    /// and at the group's last packet: over that span the group's blocks took
    /// further_blocks_seen of the lines that carry data.
    uint64_t first_data_line = 0;
    uint64_t last_data_line = 0;
    uint32_t packets = 0;
    uint32_t blocks_corrected = 0;
    uint32_t blocks_damaged = 0;
    uint32_t blocks_unchecked = 0;
    uint32_t packets_lost = 0;
    uint32_t blocks_read_under_check = 0;
  };

  /// Hand |group| to the callback with |outcome| and forget it.
  void emit(uint16_t channel, OpenGroup& group, NabtsGroupOutcome outcome);

  /// Whether |group|, ending on |channel|, is idle fill and kept from the
  /// callback: judged on its own bytes where enough arrived, and otherwise by
  /// whether its channel has carried fill already.
  bool withhold_as_idle_fill(uint16_t channel, const NabtsDataGroup& group);

  /// Start a group from a synchronizing packet, superseding any already open on
  /// that channel.
  void begin_group(const NabtsPacket& packet);

  /// Add a standard packet's block to its channel's open group.
  void extend_group(const NabtsPacket& packet);

  /// Whether |packet| carries the continuity index the group open on its
  /// channel expects next.
  bool continues_open_group(const NabtsPacket& packet) const;

  /// A loose run (NabtsLooseRun) still being collected on its channel.
  struct LooseRun {
    NabtsLooseRun run;
    uint8_t last_continuity = 0;
    /// The line clock (line_clock_) at the run's last packet.
    uint64_t last_line = 0;
  };

  /// Add |packet|, which arrived with no group open on its channel, to that
  /// channel's loose run: |starts| where it begins one of its own.
  void extend_loose(const NabtsPacket& packet, bool starts);

  /// Hand the loose run open on |channel|, if any, to the callback and forget
  /// it.
  void end_loose(uint16_t channel);

  /// Count |packet|'s block as corrected, damaged or unchecked by its suffix.
  static void tally_integrity(OpenGroup& group, const NabtsPacket& packet);

  /// Whether the suffix check vouches for |packet|'s data block, in |group|:
  /// see kNabtsByteVouched.
  static bool block_vouched(const OpenGroup& group, const NabtsPacket& packet);

  /// Append |packet|'s data block, maintaining the final-non-zero bookkeeping.
  static void append_block(OpenGroup& group, const NabtsPacket& packet);

  /// |packet| with its data block read at |group|'s block width, which is the
  /// synchronizing packet's: a block whose own PS byte names another suffix
  /// (§3.4) is read at the group's width instead, and its suffix check — made
  /// over a layout the PS byte misnamed — vouches for nothing.
  static NabtsPacket at_group_width(const OpenGroup& group,
                                    const NabtsPacket& packet);

  /// Hold open a hole |blocks| blocks wide where packets never arrived, so the
  /// bytes after it keep their offsets.
  static void append_hole(OpenGroup& group, uint32_t blocks);

  /// Packets lost ahead of |packet|, from its continuity index where that is
  /// possible, none where no line went by for them to be lost on, and nothing
  /// (the index misread over a loss that cannot be sized, or a loss it could
  /// have come round over) otherwise. |lines| is how many lines have gone by
  /// since the group's last packet with none of its packets on them, and
  /// |silent| how many of those, on lines that carry data, yielded no packet
  /// at all.
  std::optional<uint8_t> continuity_gap(const OpenGroup& group,
                                        const NabtsPacket& packet,
                                        uint64_t lines, uint64_t silent);

  GroupCallback callback_;
  // Ordered rather than hashed: the map is tiny and bounded, and iterating it
  // in channel order makes flush() deterministic.
  std::map<uint16_t, OpenGroup> open_;
  NabtsGroupStats stats_;
  /// Lines seen so far, with or without a packet on them — what bounds how many
  /// packets a gap in the continuity index can stand for.
  uint64_t line_clock_ = 0;
  /// Lines on which a packet of any group could have been lost: lines that
  /// carry data and yielded no packet, and packets whose prefix did not
  /// decode.
  uint64_t silent_clock_ = 0;
  /// Lines that carry data, with a packet on them or without: what a group's
  /// share of the lines is measured against (see continuity_gap()).
  uint64_t data_line_clock_ = 0;
  /// Set by set_lines_unknown().
  bool lines_unknown_ = false;
  /// Channels a group of idle fill has been judged on, so that a group of
  /// theirs too short to judge is taken for the same. At most 4096 entries
  /// (§3.2.3), and in practice one: F00.
  std::set<uint16_t> idle_fill_channels_;
  /// Channels that have carried a group whose type, zero, arrived as a
  /// codeword: teletext channels (see begin_group()). In practice one or two.
  std::set<uint16_t> teletext_channels_;
  LooseRunCallback loose_callback_;
  /// The loose run open on each channel, at most kNabtsMaxOpenLooseRuns.
  std::map<uint16_t, LooseRun> loose_;
};

}  // namespace orc

#endif  // ORC_NABTS_DATA_GROUP_H
