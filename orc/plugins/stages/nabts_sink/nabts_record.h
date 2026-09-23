/*
 * File:        nabts_record.h
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Teletext record headers, linked messages and application
 *              function descriptors (CEA-516 §5, §7)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_NABTS_RECORD_H
#define ORC_NABTS_RECORD_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "nabts_data_group.h"

namespace orc {

// CEA-516 §5.2.1: RT, RD and the three record address bytes are always present;
// everything after them is optional and announced by RD.
constexpr size_t kNabtsRecordHeaderMinBytes = 5;

// §5.2.4 and §5.2.5: three address nibbles, or nine when the extension is
// transmitted.
constexpr size_t kNabtsShortAddressNibbles = 3;
constexpr size_t kNabtsLongAddressNibbles = 9;

// §5.2.6: seven information bits of link order, so 128 records in a series.
constexpr uint8_t kNabtsMaxLinkOrder = 127;

// Record types worth naming (§5.2.2). The rest are reserved, and reporting the
// number is more use than inventing a name for it.
constexpr uint8_t kNabtsRecordTypeCyclicPresentation = 0;     // §5.2.2.2
constexpr uint8_t kNabtsRecordTypeNoncyclicPresentation = 1;  // §5.2.2.3
constexpr uint8_t kNabtsRecordTypeApplication = 2;            // §5.2.2.4
constexpr uint8_t kNabtsRecordTypePriorityPresentation = 3;   // §5.2.2.5

/// Whether |type| is a presentation record, i.e. its data is NAPLPS (§6.1).
constexpr bool nabts_type_is_presentation(uint8_t type) {
  return type == kNabtsRecordTypeCyclicPresentation ||
         type == kNabtsRecordTypeNoncyclicPresentation ||
         type == kNabtsRecordTypePriorityPresentation;
}

/**
 * @brief A record address, held in the long form §5.2.5 makes equivalent
 *
 * §5.2.5 requires a receiver to treat short address PQR as long address
 * 0000PQR00, so both are stored that way and two records that name the same
 * address in different forms compare equal. |long_form| remembers which was
 * transmitted, because that is what a report should show.
 */
struct NabtsRecordAddress {
  /// Nine hexadecimal digits, A1 the most significant — 36 bits used.
  uint64_t value = 0;
  bool long_form = false;

  bool operator==(const NabtsRecordAddress& other) const {
    return value == other.value;
  }
  bool operator<(const NabtsRecordAddress& other) const {
    return value < other.value;
  }

  /// As transmitted: three hexadecimal digits for a short address, nine for a
  /// long one.
  std::string text() const;

  /// The three short-address digits, or 0x1000 when the long form carries
  /// something no short address could — which is what §7.1.5's reserved
  /// addresses are compared against.
  uint16_t short_value() const;
};

/**
 * @brief The classification sequence's flags (§5.2.7)
 *
 * §5.2.7.2 assigns meanings to the Y1N flag bytes only and reserves the rest,
 * and requires that an absent flag byte be read as though it were zero — which
 * is what the default state of this is.
 */
struct NabtsClassification {
  bool present = false;

  // Y13
  bool caption = false;
  bool delay = false;
  bool index = false;
  // Y14
  bool more = false;
  bool cyclic_marker = false;
  bool auto_acquire = false;
  bool support_needed = false;
  // Y15
  bool priority = false;
  bool alarm = false;
  bool update = false;
  bool support_record = false;
  // Y16
  bool version_present = false;
  uint8_t version = 0;

  /// Flag bytes beyond Y16, whose meanings §5.2.7.2 reserves. Counted so a
  /// report can say the service used them rather than silently dropping them.
  uint8_t reserved_flag_bytes = 0;
};

/// One header extension field (§5.2.8).
struct NabtsHeaderExtension {
  /// EI b6 b4 b2 (§5.2.8.2), b6 the most significant: 1 redefines the More
  /// record address, 2 Next, 3 Index, 4 a recommended precapture address; 0 and
  /// 5-7 are reserved.
  uint8_t meaning = 0;
  /// ES (§5.2.8.3): the data-byte count, which §5.2.8.4 also gives a meaning of
  /// its own for the address-redefinition introducers.
  uint8_t size = 0;
  /// The decoded data nibbles (§5.2.8.4).
  std::vector<uint8_t> data;
};

/**
 * @brief A record header, decoded (§5.2)
 *
 * Every byte of a record header is Hamming 8/4 (§5.2.1). |valid| means the
 * fixed five bytes and every optional byte RD announced all decoded, and that
 * |header_bytes| therefore really is where the record data starts. A header
 * that ran out of bytes part way through, or hit an uncorrectable one, reports
 * invalid: the alternative is a record whose data begins at a guess.
 */
struct NabtsRecordHeader {
  bool valid = false;
  /// RT (§5.2.2).
  uint8_t type = 0;
  NabtsRecordAddress address;

  /// Whether every Hamming 8/4 byte the record's identity rests on — RT, RD,
  /// the address nibbles and the classification sequence carrying Y16 — arrived
  /// as a codeword rather than being corrected into one (see
  /// teletext_hamming84_clean()).
  ///
  /// §8.2's minimum distance of 4 leaves a three-bit burst resolving silently
  /// to a neighbouring codeword. In an address that does not damage a record:
  /// it copies it to an address the service never used, and a catalogue keyed
  /// on §5.2.1's identity has no way to tell the copy from the original. The
  /// header extension is excluded, since it bears on where the record's data
  /// starts rather than on which record this is.
  bool identity_attested = false;

  /// RD b4 (§5.2.3): the record link was transmitted.
  bool linked = false;
  /// L1 b8 (§5.2.6): further linked records follow this one.
  bool more_links = false;
  /// L1/L2 order within the linked series (§5.2.6), 0 for the first. Zero also
  /// when unlinked, which §5.2.6 makes a message of one record.
  uint8_t link_order = 0;

  NabtsClassification classification;
  std::vector<NabtsHeaderExtension> extensions;

  /// Header length, so record data is data[header_bytes ...] (§5.3).
  size_t header_bytes = 0;
};

/**
 * @brief Decode a record header from the head of a data group's data
 *
 * @param bytes  Group data, record header first (§5.1)
 * @param length Bytes available
 *
 * Reads only as far as RD and the sub-group announcers say to. Never reads past
 * |length|: a header that would need more bytes than the group carries is
 * reported invalid, which is the honest answer for a group that lost packets.
 */
NabtsRecordHeader nabts_decode_record_header(const uint8_t* bytes,
                                             size_t length);

/// A reserved channel/address pairing (§7.1.5), or an empty string.
std::string nabts_reserved_purpose(uint16_t channel,
                                   const NabtsRecordAddress& address);

/**
 * @brief One function descriptor of an application record (§7.2.2)
 *
 * A descriptor is a function code, its arguments and the delimiter 0/13. Codes
 * 2/0-2/15 control the receiver and 3/0-3/15 inform it; the rest are reserved.
 * Parity is stripped, so these are the seven-bit code-table values §7.2.2
 * names.
 */
struct NabtsFunctionDescriptor {
  uint8_t code = 0;
  std::vector<uint8_t> arguments;

  /// Code column 2 — control data (§7.2.2).
  bool is_control() const { return (code & 0x70) == 0x20; }
  /// Code column 3 — information (§7.2.2).
  bool is_information() const { return (code & 0x70) == 0x30; }
  /// A descriptor with no arguments, which §7.2.3.1 makes a request to restore
  /// that function's initial state.
  bool resets_state() const { return arguments.empty(); }
  /// "2/0" style code-table notation, which is how the standard names these.
  std::string code_text() const;
};

/**
 * @brief Split an application record's data into its function descriptors
 *
 * §7.2.2: descriptors are delimited by 0/13, nulls (0/0) are ignored, and a
 * function code sits in the range 2/0 to 7/15. A leading delimiter is
 * recommended practice, so an empty descriptor is dropped rather than reported.
 * Bytes outside the legal ranges end the descriptor they appear in — a damaged
 * record yields the descriptors it can rather than nothing.
 */
std::vector<NabtsFunctionDescriptor> nabts_decode_application_record(
    const std::vector<uint8_t>& data);

/// One record as it arrived, before linking.
struct NabtsRecord {
  uint16_t channel = 0;
  NabtsRecordHeader header;
  /// Record data (§5.3): the group's data past the header.
  std::vector<uint8_t> data;
  /// One entry per byte of @ref data: whether that byte arrived, or stands in
  /// for one a lost packet carried. Empty when none was lost (see
  /// NabtsDataGroup::present).
  std::vector<uint8_t> present;
  /// One entry per byte of @ref data: detector confidence, 0-255 (see
  /// NabtsDataGroup::confidence). Empty when nothing measured it.
  std::vector<uint8_t> confidence;
  /// Whether the group this came from arrived whole and undamaged.
  bool intact = true;
  /// Whether this arrival named its record as transmitted: the header's own
  /// NabtsRecordHeader::identity_attested and the channel the synchronizing
  /// packet was addressed to (NabtsDataGroup::channel_attested), which together
  /// are the whole of §5.2.1's identity.
  bool identity_attested = false;
};

/**
 * @brief A message: one unlinked record, or a linked series joined (§5.2.6)
 *
 * Identified by channel, record address and version, which §5.2.1 makes the
 * unique identity of a record. The data is the series' record data in link
 * order; §5.2.6 has every record after the first defer to the first record's
 * classification sequence and header extension, so those are the first
 * record's.
 */
struct NabtsMessage {
  uint16_t channel = 0;
  NabtsRecordAddress address;
  uint8_t type = 0;
  uint8_t version = 0;
  NabtsClassification classification;
  std::vector<NabtsHeaderExtension> extensions;

  /// Record data of the series, concatenated in link order.
  std::vector<uint8_t> data;
  /// One entry per byte of @ref data: whether that byte arrived, or stands in
  /// for one a lost packet carried (see NabtsDataGroup::present). This is what
  /// lets a damaged copy vote in the record catalogue over the bytes it did
  /// receive without its holes displacing everything after them.
  std::vector<uint8_t> present;
  /// One entry per byte of @ref data: detector confidence, 0-255, which weights
  /// this copy's say in that vote (see NabtsDataGroup::confidence). Empty when
  /// nothing measured it, which is read as full confidence throughout.
  std::vector<uint8_t> confidence;

  /// Records that contributed.
  uint32_t records = 0;
  /// Links the series declared, when its last record has been seen.
  uint32_t links_expected = 0;
  /// Every link from 0 to the last arrived, and the last said no more follow.
  bool complete = false;
  /// Every contributing group arrived whole and undamaged.
  bool intact = true;
  /// At least one contributing record named the message's identity as
  /// transmitted (NabtsRecord::identity_attested). One arrival that did is
  /// enough: the identity is either one the service used or one it did not, and
  /// a single uncorrected reading of it settles that.
  bool identity_attested = false;
  /// No link of the series is missing, so @ref data starts where the record
  /// starts and every byte of it is at the offset the record gave it. A missing
  /// link takes a whole record out of the middle of the concatenation, and
  /// unlike a lost packet its width is unknown, so there is no hole to hold
  /// open and the copy cannot be compared with another position for position.
  /// This is what admits a copy to the vote in the record catalogue.
  bool aligned = true;
  /// The reserved purpose of this address, if it has one (§7.1.5).
  std::string reserved_purpose;

  /// Function descriptors, for an application record (§7.2.2). Empty for a
  /// presentation record, whose data is NAPLPS and belongs to the interpreter.
  std::vector<NabtsFunctionDescriptor> functions;
};

/// What a pass over the group stream made of the records in it.
struct NabtsRecordStats {
  uint64_t groups_seen = 0;
  /// Groups whose type was not zero, and so carry no teletext record (§4.2.2).
  uint64_t non_teletext_groups = 0;
  /// The ones of those whose channel and header both arrived as codewords,
  /// counted by {channel, type}. Noise corrects into a non-zero type often
  /// enough that the bare count above says nothing about whether the recording
  /// carries some other service; a type the service really sent arrives clean,
  /// and on one channel, over and over.
  std::map<std::pair<uint16_t, uint8_t>, uint64_t> attested_foreign_groups;
  /// Groups whose record header did not decode (§5.2.1).
  uint64_t header_failures = 0;
  uint64_t records_seen = 0;
  uint64_t unlinked_records = 0;
  uint64_t linked_records = 0;
  /// Messages delivered complete, and messages delivered with links missing.
  uint64_t messages_complete = 0;
  uint64_t messages_partial = 0;
  /// Partial messages dropped because kNabtsMaxOpenMessages were already open.
  uint64_t messages_evicted = 0;

  std::string summary() const;
};

// Linked series held open at once. A series arrives over many groups and may
// interleave with others, so several are legitimately in flight; the bound is
// what stops a recording full of damaged headers from accumulating them
// without limit. The oldest is evicted, and evicting one still delivers it —
// partial, which is what it is.
constexpr size_t kNabtsMaxOpenMessages = 64;

/**
 * @brief Turns reassembled data groups into messages
 *
 * Fed groups in the order the assembler completed them, which is transmission
 * order. An unlinked record is a message on its own and is delivered at once
 * (§5.2.6). A linked series is held until its last record arrives, which
 * §5.2.6 marks with L1 b8 = 0, and the records of a series may arrive in any
 * order — a recording that starts part way through a carousel sees the middle
 * of a series before its beginning.
 *
 * Not thread safe: like the group assembler, its value is that it sees the
 * stream in one order on one thread.
 */
class NabtsRecordAssembler {
 public:
  using MessageCallback = std::function<void(const NabtsMessage&)>;

  void set_message_callback(MessageCallback callback) {
    callback_ = std::move(callback);
  }

  /// Take in one reassembled group. A group whose type is not zero carries no
  /// teletext record (§4.3) and is counted and dropped.
  void add_group(const NabtsDataGroup& group);

  /// End the pass: every series still open is delivered partial. Idempotent.
  void flush();

  const NabtsRecordStats& stats() const { return stats_; }

 private:
  /// Identity of a message (§5.2.1): channel, record address and version.
  struct MessageKey {
    uint16_t channel = 0;
    uint64_t address = 0;
    uint8_t version = 0;

    bool operator<(const MessageKey& other) const {
      if (channel != other.channel) return channel < other.channel;
      if (address != other.address) return address < other.address;
      return version < other.version;
    }
  };

  struct OpenMessage {
    NabtsMessage message;
    /// Record data per link order, so out-of-order arrival still concatenates
    /// in order. An absent entry is a link not yet seen.
    std::map<uint8_t, std::vector<uint8_t>> parts;
    /// Which bytes of each part arrived, keyed and concatenated alike, so the
    /// two stay index for index (see NabtsMessage::present).
    std::map<uint8_t, std::vector<uint8_t>> part_present;
    /// Detector confidence per part, keyed and concatenated alike.
    std::map<uint8_t, std::vector<uint8_t>> part_confidence;
    /// The last link's order, once a record with more_links = false has been
    /// seen; kNabtsMaxLinkOrder + 1 means not yet.
    uint16_t final_order = kNabtsMaxLinkOrder + 1;
    /// Arrival order, for eviction.
    uint64_t sequence = 0;
  };

  /// Concatenate |open|'s parts into its message and hand it to the callback.
  void emit(OpenMessage& open, bool complete);

  /// Deliver a record that is a message on its own.
  void emit_unlinked(const NabtsRecord& record);

  /// Fold a linked record into its series, delivering the series if that
  /// completed it.
  void add_linked(const NabtsRecord& record);

  /// Fill in the derived fields every message carries, whatever its shape.
  static void finalise(NabtsMessage& message);

  MessageCallback callback_;
  std::map<MessageKey, OpenMessage> open_;
  uint64_t next_sequence_ = 0;
  NabtsRecordStats stats_;
};

}  // namespace orc

#endif  // ORC_NABTS_RECORD_H
