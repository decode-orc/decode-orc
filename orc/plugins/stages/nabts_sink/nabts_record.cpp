/*
 * File:        nabts_record.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Record header, message linking and application function
 *              descriptor implementation (CEA-516 §5, §7)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_record.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>

#include "vbi-services/teletext_slicer.h"

namespace orc {

namespace {

// The information nibble's bits, most significant first, are b8 b6 b4 b2 — see
// nabts_packet.cpp for why, and CEA-516 §5.2.8.3 for the standard saying so.
constexpr bool b2_of(int nibble) { return (nibble & 0x1) != 0; }
constexpr bool b4_of(int nibble) { return ((nibble >> 1) & 0x1) != 0; }
constexpr bool b6_of(int nibble) { return ((nibble >> 2) & 0x1) != 0; }
constexpr bool b8_of(int nibble) { return ((nibble >> 3) & 0x1) != 0; }

// §7.2.2 names codes in "column/row" notation; a byte is (column << 4) | row.
constexpr uint8_t kCodeNull = 0x00;               // 0/0
constexpr uint8_t kCodeFunctionDelimiter = 0x0D;  // 0/13, the APR of the C0 set
constexpr uint8_t kCodeFirstFunction = 0x20;      // 2/0
constexpr uint8_t kCodeLastFunction = 0x7F;       // 7/15

/**
 * @brief A cursor over Hamming 8/4 bytes that refuses to run off the end
 *
 * Every byte of a record header is Hamming coded (§5.2.1) and the header's
 * length is not stated anywhere — it is a consequence of the format (§5.2.9).
 * So decoding is a walk that can fail two ways at every step: out of bytes, or
 * a byte that would not correct. Both are terminal, and folding them into the
 * cursor keeps the walk itself readable.
 */
class HammingCursor {
 public:
  HammingCursor(const uint8_t* bytes, size_t length)
      : bytes_(bytes), length_(length) {}

  /// Next nibble, or -1 once the cursor has failed. Once failed, stays failed.
  int next() {
    if (failed_ || position_ >= length_) {
      failed_ = true;
      return -1;
    }
    const int value = teletext_hamming84_decode(bytes_[position_]);
    if (value < 0) {
      failed_ = true;
      return -1;
    }
    if (!teletext_hamming84_clean(bytes_[position_])) {
      clean_ = false;
    }
    ++position_;
    return value;
  }

  bool failed() const { return failed_; }
  size_t position() const { return position_; }

  /// Whether every byte read so far arrived as a codeword rather than being
  /// corrected into one. Monotone: once a byte has needed correcting, nothing
  /// read afterwards can restore the guarantee, which is what makes a single
  /// reading of this at any point in the walk cover everything before it.
  bool clean() const { return clean_; }

 private:
  const uint8_t* bytes_;
  size_t length_;
  size_t position_ = 0;
  bool failed_ = false;
  bool clean_ = true;
};

/// Read the nine, or three, address nibbles into the long form §5.2.5 defines.
NabtsRecordAddress read_address(HammingCursor& cursor, bool extended) {
  NabtsRecordAddress address;
  address.long_form = extended;

  std::array<int, kNabtsLongAddressNibbles> digits{};
  const size_t count =
      extended ? kNabtsLongAddressNibbles : kNabtsShortAddressNibbles;
  for (size_t i = 0; i < count; ++i) {
    digits[i] = cursor.next();
    if (cursor.failed()) {
      return address;
    }
  }

  if (!extended) {
    // §5.2.5: short address PQR is the long address 0000PQR00. Storing it that
    // way is what makes the two compare equal, which the standard requires.
    digits[4] = digits[0];
    digits[5] = digits[1];
    digits[6] = digits[2];
    digits[0] = 0;
    digits[1] = 0;
    digits[2] = 0;
    digits[3] = 0;
    digits[7] = 0;
    digits[8] = 0;
  }

  for (size_t i = 0; i < kNabtsLongAddressNibbles; ++i) {
    address.value = (address.value << 4) | static_cast<uint64_t>(digits[i]);
  }
  return address;
}

/// Read the classification sequence (§5.2.7): pointer bytes, each followed by
/// the flag bytes it announces.
NabtsClassification read_classification(HammingCursor& cursor) {
  NabtsClassification out;
  out.present = true;

  // §5.2.7.1: pointer byte Y0N announces flag pairs YN1/YN2 (b2), YN3/YN4 (b4)
  // and YN5/YN6 (b6), and another pointer byte follows if b8 is set. Only the
  // first group's flag bytes have meanings (§5.2.7.2), so |group| decides
  // whether a flag byte is read or merely counted.
  for (size_t group = 1;; ++group) {
    const int pointer = cursor.next();
    if (cursor.failed()) {
      return out;
    }

    // Flag bytes arrive in pairs, Y_1/Y_2 then Y_3/Y_4 then Y_5/Y_6, so the
    // pair's index gives each byte its position in the group.
    const bool pairs[3] = {b2_of(pointer), b4_of(pointer), b6_of(pointer)};
    for (size_t pair = 0; pair < 3; ++pair) {
      if (!pairs[pair]) {
        continue;
      }
      for (size_t half = 0; half < 2; ++half) {
        const int flags = cursor.next();
        if (cursor.failed()) {
          return out;
        }
        const size_t index = pair * 2 + half + 1;  // 1 … 6, i.e. YN1 … YN6
        if (group != 1) {
          ++out.reserved_flag_bytes;
          continue;
        }
        switch (index) {
          case 3:  // Y13
            out.caption = b8_of(flags);
            out.delay = b6_of(flags);
            out.index = b4_of(flags);
            break;
          case 4:  // Y14
            out.more = b8_of(flags);
            out.cyclic_marker = b6_of(flags);
            out.auto_acquire = b4_of(flags);
            out.support_needed = b2_of(flags);
            break;
          case 5:  // Y15
            out.priority = b8_of(flags);
            out.alarm = b6_of(flags);
            out.update = b4_of(flags);
            out.support_record = b2_of(flags);
            break;
          case 6:  // Y16 — the whole nibble is the version number
            out.version_present = true;
            out.version = static_cast<uint8_t>(flags);
            break;
          default:
            // Y11 and Y12 are reserved for future standardization (§5.2.7.2).
            ++out.reserved_flag_bytes;
            break;
        }
      }
    }

    if (!b8_of(pointer)) {
      return out;  // No further pointer byte, so the sequence ends here.
    }
  }
}

/// Read one or more header extension fields (§5.2.8).
std::vector<NabtsHeaderExtension> read_extensions(HammingCursor& cursor) {
  std::vector<NabtsHeaderExtension> out;
  for (;;) {
    const int introducer = cursor.next();
    if (cursor.failed()) {
      return out;
    }
    const int size = cursor.next();
    if (cursor.failed()) {
      return out;
    }

    NabtsHeaderExtension field;
    // §5.2.8.2: b8 says whether another field follows and b6 b4 b2 carry the
    // meaning, b6 the most significant. Those are the nibble's low three bits —
    // b8 is its top bit, not part of the meaning.
    field.meaning = static_cast<uint8_t>(introducer & 0x7);
    field.size = static_cast<uint8_t>(size);
    field.data.reserve(field.size);
    for (uint8_t i = 0; i < field.size; ++i) {
      const int value = cursor.next();
      if (cursor.failed()) {
        out.push_back(std::move(field));
        return out;
      }
      field.data.push_back(static_cast<uint8_t>(value));
    }
    const bool more = b8_of(introducer);
    out.push_back(std::move(field));
    if (!more) {
      return out;
    }
  }
}

}  // namespace

std::string NabtsRecordAddress::text() const {
  if (long_form) {
    return fmt::format("{:09X}", value & 0xFFFFFFFFFULL);
  }
  // §5.2.5: the short digits sit at A5, A6, A7 of the long form.
  return fmt::format("{:03X}", short_value() & 0xFFF);
}

uint16_t NabtsRecordAddress::short_value() const {
  // Short address PQR is stored as 0000PQR00, so it is a short address exactly
  // when the four leading and two trailing digits are zero.
  const uint64_t leading = (value >> 20) & 0xFFFFULL;
  const uint64_t trailing = value & 0xFFULL;
  if (leading != 0 || trailing != 0) {
    return 0x1000;  // Not expressible as a short address.
  }
  return static_cast<uint16_t>((value >> 8) & 0xFFFULL);
}

NabtsRecordHeader nabts_decode_record_header(const uint8_t* bytes,
                                             size_t length) {
  NabtsRecordHeader header;
  if (bytes == nullptr || length < kNabtsRecordHeaderMinBytes) {
    return header;
  }

  HammingCursor cursor(bytes, length);

  const int record_type = cursor.next();
  const int designator = cursor.next();
  if (cursor.failed()) {
    return header;
  }
  header.type = static_cast<uint8_t>(record_type);

  // §5.2.3: RD announces which optional sub-groups were transmitted, and they
  // follow in the order §5.2.1 lists them.
  const bool has_extension = b2_of(designator);
  const bool has_link = b4_of(designator);
  const bool has_classification = b6_of(designator);
  const bool has_header_extension = b8_of(designator);

  header.address = read_address(cursor, has_extension);
  if (cursor.failed()) {
    return header;
  }

  if (has_link) {
    const int l1 = cursor.next();
    const int l2 = cursor.next();
    if (cursor.failed()) {
      return header;
    }
    header.linked = true;
    // §5.2.6: L1 b8 says whether more linked records follow; the remaining
    // seven information bits are the order, L1 the more significant.
    header.more_links = b8_of(l1);
    header.link_order =
        static_cast<uint8_t>(((l1 & 0x7) << 4) | static_cast<uint8_t>(l2));
  }

  if (has_classification) {
    header.classification = read_classification(cursor);
    if (cursor.failed()) {
      return header;
    }
  }

  // Everything the record's identity rests on has now been read: RT and RD,
  // which decide how long the address is; the address itself; and the
  // classification sequence, which is where the version number lives
  // (§5.2.7.2 Y16). A header extension may follow, and it says nothing about
  // which record this is — only where its data starts — so the reading is taken
  // here rather than at the end of the walk.
  header.identity_attested = cursor.clean();

  if (has_header_extension) {
    header.extensions = read_extensions(cursor);
    if (cursor.failed()) {
      return header;
    }
  }

  header.valid = true;
  header.header_bytes = cursor.position();
  return header;
}

std::string nabts_reserved_purpose(uint16_t channel,
                                   const NabtsRecordAddress& address) {
  const uint16_t short_address = address.short_value();

  // §7.1.5. The support record is reserved on every channel, so it is tested
  // first; the rest name a channel as well.
  if (short_address == 0xFFF) {
    return "Support Record";
  }
  if (channel == 0x000 && short_address == 0x000) {
    return "Master Index Page and power-up Record";
  }
  if (channel == 0x000 && short_address == 0xFFE) {
    return "Service Application Record";
  }
  if (channel == 0xA00 && short_address == 0x000) {
    return "Start of captioning";
  }
  if (channel == 0xB00 && short_address == 0x000) {
    return "Start of Flash";
  }
  return {};
}

std::string NabtsFunctionDescriptor::code_text() const {
  return fmt::format("{}/{}", (code >> 4) & 0x7, code & 0xF);
}

std::vector<NabtsFunctionDescriptor> nabts_decode_application_record(
    const std::vector<uint8_t>& data) {
  std::vector<NabtsFunctionDescriptor> out;
  NabtsFunctionDescriptor current;
  bool in_descriptor = false;

  const auto close = [&] {
    // §7.2.2 recommends a leading delimiter, so the first close is usually of
    // nothing at all. An empty descriptor is dropped rather than reported.
    if (in_descriptor) {
      out.push_back(std::move(current));
      current = NabtsFunctionDescriptor{};
    }
    in_descriptor = false;
  };

  for (const uint8_t raw : data) {
    // §3.3 puts odd parity in b8 of every data byte of a type-zero group, so
    // the code-table value is the low seven bits.
    const uint8_t code = static_cast<uint8_t>(raw & 0x7F);

    if (code == kCodeNull) {
      continue;  // §7.2.2: nulls are ignored.
    }
    if (code == kCodeFunctionDelimiter) {
      close();
      continue;
    }
    if (code < kCodeFirstFunction || code > kCodeLastFunction) {
      // §7.2.2 forbids 0/1-0/12 and 0/14-1/15 in an application record. One
      // here is damage, and it ends the descriptor it appeared in rather than
      // being swallowed into its arguments.
      close();
      continue;
    }

    if (!in_descriptor) {
      current.code = code;
      in_descriptor = true;
    } else {
      current.arguments.push_back(code);
    }
  }

  // A record whose final delimiter was lost still yielded a descriptor.
  close();
  return out;
}

std::string NabtsRecordStats::summary() const {
  std::string out = "Teletext records\n";
  out += fmt::format(
      "  Groups:        {} seen, {} not teletext, {} header rejected\n"
      "  Records:       {} seen, {} unlinked, {} linked\n"
      "  Messages:      {} complete, {} partial\n",
      groups_seen, non_teletext_groups, header_failures, records_seen,
      unlinked_records, linked_records, messages_complete, messages_partial);
  for (const auto& [key, groups] : attested_foreign_groups) {
    out += fmt::format(
        "  Not teletext:  channel {:03X}, {} clean group(s) of type {}{}\n",
        key.first, groups, static_cast<int>(key.second),
        key.second == kNabtsPrivateGroupType ? " (private use)" : "");
  }
  if (messages_evicted > 0) {
    out += fmt::format(
        "  Evicted:       {} incomplete series dropped at the open-series "
        "limit\n",
        messages_evicted);
  }
  return out;
}

void NabtsRecordAssembler::finalise(NabtsMessage& message) {
  message.version = message.classification.version_present
                        ? message.classification.version
                        : 0;
  message.reserved_purpose =
      nabts_reserved_purpose(message.channel, message.address);
  if (message.type == kNabtsRecordTypeApplication) {
    message.functions = nabts_decode_application_record(message.data);
  }
}

void NabtsRecordAssembler::emit_unlinked(const NabtsRecord& record) {
  NabtsMessage message;
  message.channel = record.channel;
  message.address = record.header.address;
  message.type = record.header.type;
  message.classification = record.header.classification;
  message.extensions = record.header.extensions;
  message.data = record.data;
  message.present = record.present;
  message.confidence = record.confidence;
  message.records = 1;
  message.links_expected = 1;
  message.complete = true;
  message.intact = record.intact;
  message.identity_attested = record.identity_attested;
  // An unlinked record is the whole message, so it starts where the record
  // starts and its holes are already held open at their true width.
  message.aligned = true;
  finalise(message);

  ++stats_.messages_complete;
  if (callback_) {
    callback_(message);
  }
}

void NabtsRecordAssembler::emit(OpenMessage& open, bool complete) {
  NabtsMessage& message = open.message;
  message.complete = complete;
  // A series missing a link is missing that record's bytes from the middle of
  // the concatenation below, which moves every later link earlier.
  if (!complete) {
    message.aligned = false;
  }
  message.records = static_cast<uint32_t>(open.parts.size());
  message.links_expected = open.final_order <= kNabtsMaxLinkOrder
                               ? static_cast<uint32_t>(open.final_order) + 1
                               : 0;

  // Ordered by link number, so an out-of-order arrival is put back in its place
  // here rather than at insertion (§5.2.6).
  size_t total = 0;
  for (const auto& part : open.parts) {
    total += part.second.size();
  }
  message.data.clear();
  message.data.reserve(total);
  for (const auto& part : open.parts) {
    message.data.insert(message.data.end(), part.second.begin(),
                        part.second.end());
  }
  message.present.clear();
  message.present.reserve(total);
  for (const auto& part : open.part_present) {
    message.present.insert(message.present.end(), part.second.begin(),
                           part.second.end());
  }
  message.confidence.clear();
  message.confidence.reserve(total);
  for (const auto& part : open.part_confidence) {
    message.confidence.insert(message.confidence.end(), part.second.begin(),
                              part.second.end());
  }
  finalise(message);

  if (complete) {
    ++stats_.messages_complete;
  } else {
    ++stats_.messages_partial;
  }
  if (callback_) {
    callback_(message);
  }
}

void NabtsRecordAssembler::add_linked(const NabtsRecord& record) {
  const MessageKey key{record.channel, record.header.address.value,
                       record.header.classification.version_present
                           ? record.header.classification.version
                           : static_cast<uint8_t>(0)};

  auto it = open_.find(key);
  if (it == open_.end()) {
    if (open_.size() >= kNabtsMaxOpenMessages) {
      // Evict the series that has waited longest. It is delivered rather than
      // discarded: a partial message is still worth listing, and dropping it
      // silently would make a busy recording look like a sparse one.
      auto oldest = open_.begin();
      for (auto candidate = open_.begin(); candidate != open_.end();
           ++candidate) {
        if (candidate->second.sequence < oldest->second.sequence) {
          oldest = candidate;
        }
      }
      ++stats_.messages_evicted;
      emit(oldest->second, /*complete=*/false);
      open_.erase(oldest);
    }
    OpenMessage fresh;
    fresh.message.channel = record.channel;
    fresh.message.address = record.header.address;
    fresh.message.type = record.header.type;
    fresh.sequence = next_sequence_++;
    it = open_.emplace(key, std::move(fresh)).first;
  }
  OpenMessage& open = it->second;

  // §5.2.6: every record after the first defers to the first record's
  // classification sequence and header extension, and its own are to be
  // ignored. The first record is the one whose link order is zero.
  if (record.header.link_order == 0) {
    open.message.classification = record.header.classification;
    open.message.extensions = record.header.extensions;
    open.message.type = record.header.type;
  }
  if (!record.intact) {
    open.message.intact = false;
  }
  if (record.identity_attested) {
    open.message.identity_attested = true;
  }
  if (!record.header.more_links) {
    open.final_order = record.header.link_order;
  }
  open.parts[record.header.link_order] = record.data;
  open.part_present[record.header.link_order] = record.present;
  open.part_confidence[record.header.link_order] = record.confidence;

  // Complete when every link from 0 to the last has arrived. Counting rather
  // than checking each index is enough: parts is keyed on link order, so its
  // size can only reach final_order + 1 when all of them are distinct.
  if (open.final_order <= kNabtsMaxLinkOrder &&
      open.parts.size() == static_cast<size_t>(open.final_order) + 1) {
    emit(open, /*complete=*/true);
    open_.erase(it);
  }
}

void NabtsRecordAssembler::add_group(const NabtsDataGroup& group) {
  ++stats_.groups_seen;

  // §4.3: only a type-zero group's data is a teletext record. Another type is
  // some other application's, and guessing a record header out of it would
  // invent records that were never transmitted.
  if (group.header.type != kNabtsBroadcastGroupType) {
    ++stats_.non_teletext_groups;
    if (group.channel_attested && group.header.attested) {
      ++stats_.attested_foreign_groups[{group.channel, group.header.type}];
    }
    return;
  }

  const NabtsRecordHeader header =
      nabts_decode_record_header(group.data.data(), group.data.size());
  if (!header.valid) {
    ++stats_.header_failures;
    return;
  }

  NabtsRecord record;
  record.channel = group.channel;
  record.header = header;
  record.intact = group.intact();
  record.identity_attested = header.identity_attested && group.channel_attested;
  if (group.data.size() > header.header_bytes) {
    record.data.assign(
        group.data.begin() + static_cast<ptrdiff_t>(header.header_bytes),
        group.data.end());
    // §3.2.4's holes are held open in the group, so the record's bytes are
    // already at the offsets it gave them; which of them actually arrived comes
    // across with them. An empty mask says the group lost nothing, and is left
    // empty here rather than invented.
    if (group.present.size() >= group.data.size()) {
      record.present.assign(
          group.present.begin() + static_cast<ptrdiff_t>(header.header_bytes),
          group.present.begin() + static_cast<ptrdiff_t>(group.data.size()));
    }
    if (group.confidence.size() >= group.data.size()) {
      record.confidence.assign(
          group.confidence.begin() +
              static_cast<ptrdiff_t>(header.header_bytes),
          group.confidence.begin() + static_cast<ptrdiff_t>(group.data.size()));
    }
  }

  ++stats_.records_seen;
  if (!header.linked) {
    ++stats_.unlinked_records;
    emit_unlinked(record);
    return;
  }
  ++stats_.linked_records;
  add_linked(record);
}

void NabtsRecordAssembler::flush() {
  for (auto& entry : open_) {
    emit(entry.second, /*complete=*/false);
  }
  open_.clear();
}

}  // namespace orc
