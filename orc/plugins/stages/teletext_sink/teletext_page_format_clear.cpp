/*
 * File:        teletext_page_format_clear.cpp
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     Data a teletext service sent on pages: EN 300 708 Page
 *              Format - Clear streams, and the scrambled pages of Page
 *              Format - CA
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "teletext_page_format_clear.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

#include "vbi-services/teletext_page_decoder.h"

namespace orc {

namespace {

constexpr int kTimeFillingPage = 0xFF;
// Packet 28 and its designation codes (EN 300 708 §4.3.3, §5.4.2).
constexpr int kPacket28 = 28;
constexpr int kPageFunctionDesignation = 0;
constexpr int kPageKeyDesignation = 2;
// Table 4: page function 0001, a data broadcasting page; page coding 001,
// the coding of §4.3.2.
constexpr int32_t kPageFunctionClear = 0x1;
constexpr int32_t kPageCodingClear = 0x1;
// The last display packet a data page may use (§4.3.2.1), and the default
// where its header names none.
constexpr int kLastDataPacket = 25;
constexpr int kDefaultLastPacket = 23;
// The Block Separator and the filler byte as transmitted: the Hamming 8/4
// codes of 0xC and 0x3 (§4.3.2.3, §4.3.2.5).
constexpr uint8_t kBlockSeparator = 0xA1;
constexpr uint8_t kFillerByte = 0x5E;
// Block Pointer values: 0 to 0xC point to a separator at data byte
// 3 x pointer of the packet; 0xD says there is none (Table 3).
constexpr int kLastPointer = 0xC;
constexpr int kNoSeparator = 0xD;
// The packets of a page whose Block Pointer may fail to say it is a data
// page, one in this many: a damaged one.
constexpr int kStructureSlack = 8;
// A continuity index this far behind the last is an earlier page sent
// again, not one sixteen pages on.
constexpr int kContinuityBehind = 8;

/// Hamming 8/4 decoded, or -1 where |byte| is not a codeword.
int exact(uint8_t byte) {
  const int value = teletext_hamming84_decode(byte);
  return value >= 0 &&
                 teletext_hamming84_encode(static_cast<uint8_t>(value)) == byte
             ? value
             : -1;
}

int displayed(int magazine) { return magazine == 0 ? 8 : magazine; }

}  // namespace

void TeletextPageFormatClear::add_packet(const Packet& packet,
                                         size_t packet_bytes) {
  const int low = exact(packet[0]);
  const int high = exact(packet[1]);
  if (low < 0 || high < 0) {
    return;
  }
  packet_bytes_ = packet_bytes;
  const int magazine = low & 0x7;
  const int number = ((low >> 3) & 1) | (high << 1);
  if (number == 0) {
    // A header ends the page in progress in its magazine, or in every
    // magazine where it says the service sends them one at a time (C11).
    const int c11_c14 = teletext_hamming84_decode(packet[9]);
    if (c11_c14 >= 0 && (c11_c14 & 1) != 0) {
      for (int m = 0; m < static_cast<int>(open_.size()); ++m) {
        close_page(m);
      }
    } else {
      close_page(magazine);
    }
    // The page number must arrive as transmitted; the sub-code may be
    // corrected (§4.3.1.2: continuity, Last Transmitted Packet, stream).
    const int units = exact(packet[2]);
    const int tens = exact(packet[3]);
    const int s1 = teletext_hamming84_decode(packet[4]);
    const int s2_c4 = teletext_hamming84_decode(packet[5]);
    const int s3 = teletext_hamming84_decode(packet[6]);
    const int s4_c5_c6 = teletext_hamming84_decode(packet[7]);
    if (units < 0 || tens < 0 || s1 < 0 || s2_c4 < 0 || s3 < 0 ||
        s4_c5_c6 < 0) {
      return;
    }
    const int page_number = (tens << 4) | units;
    if (page_number == kTimeFillingPage) {
      return;
    }
    OpenPage& page = open_[static_cast<size_t>(magazine)];
    page = OpenPage{};
    page.open = true;
    page.magazine = magazine;
    page.page_number = page_number;
    page.continuity = s1;
    page.last_packet = (s2_c4 & 0x7) | ((s4_c5_c6 & 0x3) << 3);
    page.stream_id = s3;
    return;
  }
  OpenPage& page = open_[static_cast<size_t>(magazine)];
  if (!page.open) {
    return;
  }
  if (number >= 1 && number <= kLastDataPacket) {
    page.rows[static_cast<size_t>(number)] = packet;
    return;
  }
  if (number != kPacket28) {
    return;
  }
  const int designation = teletext_hamming84_decode(packet[2]);
  if (designation == kPageFunctionDesignation) {
    const int32_t triplet =
        teletext_hamming2418_decode(packet[3], packet[4], packet[5]);
    if (triplet >= 0 && (triplet & 0xF) == kPageFunctionClear &&
        ((triplet >> 4) & 0x7) == kPageCodingClear) {
      page.marked_clear = true;
    }
  } else if (designation == kPageKeyDesignation) {
    // A page key packet (§5.4.2), believed only whole.
    for (size_t i = 0; i < 13; ++i) {
      const size_t at = 3 + 3 * i;
      if (teletext_hamming2418_decode(packet[at], packet[at + 1],
                                      packet[at + 2]) < 0) {
        return;
      }
    }
    page.marked_scrambled = true;
  }
}

bool TeletextPageFormatClear::looks_like_data_page(const OpenPage& page,
                                                   size_t packet_bytes) {
  // Every packet of a data page opens with a Block Pointer, and one that
  // points finds a separator there. A page of text passes as one only where
  // nearly every row begins with one of fourteen codes and has '!' where the
  // code points.
  int present = 0;
  int failed = 0;
  int pointing = 0;
  for (size_t r = 1; r <= static_cast<size_t>(kLastDataPacket); ++r) {
    const auto& row = page.rows[r];
    if (!row.has_value()) {
      continue;
    }
    ++present;
    const int pointer = teletext_hamming84_decode((*row)[2]);
    if (pointer < 0 || pointer > kNoSeparator) {
      ++failed;
      continue;
    }
    if (pointer <= kLastPointer) {
      const size_t at = 3 + 3 * static_cast<size_t>(pointer);
      if (at >= packet_bytes || (*row)[at] != kBlockSeparator) {
        ++failed;
        continue;
      }
      ++pointing;
    }
  }
  return present - failed >= kTeletextPageDataMinPackets && pointing > 0 &&
         failed * kStructureSlack <= present;
}

void TeletextPageFormatClear::close_page(int magazine) {
  OpenPage& page = open_[static_cast<size_t>(magazine)];
  if (!page.open) {
    return;
  }
  page.open = false;
  if (page.marked_scrambled) {
    ++scrambled_;
    scrambled_pages_.emplace(displayed(page.magazine), page.page_number);
    return;
  }
  if (!page.marked_clear && !looks_like_data_page(page, packet_bytes_)) {
    return;
  }
  const auto key =
      std::make_tuple(page.magazine, page.page_number, page.stream_id);
  auto it = streams_.find(key);
  if (it == streams_.end()) {
    if (streams_.size() >= kTeletextPageDataMaxStreams) {
      return;
    }
    it = streams_.emplace(key, Stream{}).first;
    it->second.magazine = displayed(page.magazine);
    it->second.page_number = page.page_number;
    it->second.stream_id = page.stream_id;
  }
  Stream& stream = it->second;
  const int last = page.last_packet >= 1 && page.last_packet <= kLastDataPacket
                       ? page.last_packet
                       : kDefaultLastPacket;
  if (stream.pending.has_value()) {
    const int step = (page.continuity - stream.pending->continuity + 16) % 16;
    if (step == 0 || step > 16 - kContinuityBehind) {
      // A fragment or a repeat of the page in hand (§4.3a), or an earlier
      // page sent again: it fills in what is missing and adds no page.
      if (step == 0) {
        for (size_t r = 1; r < kRows; ++r) {
          if (!stream.pending->rows[r].has_value() &&
              page.rows[r].has_value()) {
            stream.pending->rows[r] = page.rows[r];
          }
        }
      }
      return;
    }
    stream.pages_lost += static_cast<uint64_t>(step - 1);
    append_page(stream, *stream.pending, packet_bytes_, held_bytes_);
  }
  PendingPage pending;
  pending.continuity = page.continuity;
  pending.last_packet = last;
  pending.rows = page.rows;
  stream.pending = std::move(pending);
}

void TeletextPageFormatClear::append_page(Stream& stream,
                                          const PendingPage& page,
                                          size_t packet_bytes,
                                          size_t& held_bytes) {
  ++stream.pages;
  // A packet's data follows its address and its Block Pointer.
  const size_t data_bytes = packet_bytes > 3 ? packet_bytes - 3 : 0;
  for (size_t r = 1; r <= static_cast<size_t>(page.last_packet); ++r) {
    if (held_bytes + data_bytes > kTeletextPageDataMaxBytes) {
      return;
    }
    const size_t at = stream.bytes.size();
    const auto& row = page.rows[r];
    if (!row.has_value()) {
      ++stream.packets_lost;
      stream.bytes.insert(stream.bytes.end(), data_bytes, 0);
      stream.present.insert(stream.present.end(), data_bytes, false);
    } else {
      stream.bytes.insert(
          stream.bytes.end(), row->begin() + 3,
          row->begin() + 3 + static_cast<ptrdiff_t>(data_bytes));
      stream.present.insert(stream.present.end(), data_bytes, true);
      const int pointer = teletext_hamming84_decode((*row)[2]);
      if (pointer >= 0 && pointer <= kLastPointer &&
          3 * static_cast<size_t>(pointer) < data_bytes) {
        stream.pointed.push_back(at + 3 * static_cast<size_t>(pointer));
      }
    }
    held_bytes += data_bytes;
  }
}

std::vector<TeletextPageDataStream> TeletextPageFormatClear::parse(
    const Stream& stream) {
  const std::vector<uint8_t>& bytes = stream.bytes;
  const std::vector<bool>& present = stream.present;
  const std::vector<size_t>& pointed = stream.pointed;
  std::map<int, TeletextPageDataStream> applications;
  std::map<int, uint16_t> types;
  const auto entry = [&](int application) -> TeletextPageDataStream& {
    auto [it, made] = applications.try_emplace(application);
    if (made) {
      it->second.magazine = stream.magazine;
      it->second.page_number = stream.page_number;
      it->second.stream_id = stream.stream_id;
      it->second.application = application;
    }
    return it->second;
  };
  // The first separator a pointer points to at or after |at|.
  const auto next_pointer = [&pointed](size_t at) -> std::optional<size_t> {
    const auto it = std::lower_bound(pointed.begin(), pointed.end(), at);
    return it == pointed.end() ? std::nullopt : std::optional<size_t>(*it);
  };
  const auto whole = [&present](size_t from, size_t to) {
    return std::all_of(present.begin() + static_cast<ptrdiff_t>(from),
                       present.begin() + static_cast<ptrdiff_t>(to),
                       [](bool arrived) { return arrived; });
  };

  size_t pos = 0;
  bool searching = true;
  while (true) {
    if (searching) {
      const auto pointer = next_pointer(pos);
      if (!pointer.has_value()) {
        break;
      }
      pos = *pointer;
      searching = false;
    }
    if (pos + 5 > bytes.size()) {
      break;
    }
    // A separator, then the Structure Header: Application ID bits 1-4; its
    // bit 5 with length bits 1-3; length bits 4-7; length bits 8-11
    // (§4.2.2 Figure 2).
    int nibble[4] = {};
    bool header = present[pos] && bytes[pos] == kBlockSeparator;
    for (size_t k = 0; header && k < 4; ++k) {
      nibble[k] = teletext_hamming84_decode(bytes[pos + 1 + k]);
      header = present[pos + 1 + k] && nibble[k] >= 0;
    }
    if (!header) {
      searching = true;
      ++pos;
      continue;
    }
    const int application = nibble[0] | ((nibble[1] & 1) << 4);
    const auto length = static_cast<size_t>(
        (nibble[1] >> 1) | (nibble[2] << 3) | (nibble[3] << 7));
    const size_t start = pos + 5;
    const size_t end = start + length;
    TeletextPageDataStream& block_owner = entry(application);
    if (end > bytes.size()) {
      ++block_owner.blocks_damaged;  // the recording ends inside it
      break;
    }
    // A separator a pointer points to inside the block says its length, or
    // a byte of it, was misread.
    const auto inside = next_pointer(start);
    if (inside.has_value() && *inside < end) {
      ++block_owner.blocks_damaged;
      pos = *inside;
      continue;
    }
    if (!whole(start, end)) {
      ++block_owner.blocks_damaged;
    } else if (application == 0) {
      // Bundle Information (§4.2.3): checksum, the number of applications,
      // then each one's type, Hamming 8/4 throughout, least significant
      // nibble first; the nibbles of the header and the fields sum to zero
      // with the checksum.
      std::vector<int> values;
      bool decoded = true;
      for (size_t i = start; i < end; ++i) {
        const int value = teletext_hamming84_decode(bytes[i]);
        decoded = decoded && value >= 0;
        values.push_back(value);
      }
      // The checksum byte is the first two nibbles; the fields it covers
      // are the header's nibbles and every one after it.
      int sum = nibble[0] + nibble[1] + nibble[2] + nibble[3];
      for (size_t i = 2; i < values.size(); ++i) {
        sum += values[i];
      }
      const int checksum =
          values.size() >= 2 ? values[0] | (values[1] << 4) : -1;
      const size_t count =
          values.size() >= 4 ? static_cast<size_t>(values[2] | (values[3] << 4))
                             : 0;
      if (decoded && values.size() == 4 + 4 * count &&
          ((sum + checksum) & 0xFF) == 0) {
        ++block_owner.blocks;
        for (size_t i = 0; i < count; ++i) {
          const int* t = &values[4 + 4 * i];
          types[static_cast<int>(i) + 1] = static_cast<uint16_t>(
              t[0] | (t[1] << 4) | (t[2] << 8) | (t[3] << 12));
        }
      } else {
        ++block_owner.blocks_damaged;
      }
    } else {
      ++block_owner.blocks;
      block_owner.payload.insert(block_owner.payload.end(),
                                 bytes.begin() + static_cast<ptrdiff_t>(start),
                                 bytes.begin() + static_cast<ptrdiff_t>(end));
    }
    // Filler, then the next separator, or the stream resumes where a
    // pointer points.
    pos = end;
    while (pos < bytes.size() && present[pos] && bytes[pos] == kFillerByte) {
      ++pos;
    }
    if (!(pos < bytes.size() && present[pos] &&
          bytes[pos] == kBlockSeparator)) {
      searching = true;
    }
  }

  std::vector<TeletextPageDataStream> out;
  for (auto& [application, data] : applications) {
    if (application == 0) {
      continue;  // the Bundle Information, read for the types
    }
    const auto type = types.find(application);
    if (type != types.end()) {
      data.application_type = type->second;
    }
    out.push_back(std::move(data));
  }
  // A stream whose pages came but no block of which was read is still
  // listed, as application 0.
  if (out.empty()) {
    TeletextPageDataStream none;
    none.magazine = stream.magazine;
    none.page_number = stream.page_number;
    none.stream_id = stream.stream_id;
    const auto bundle = applications.find(0);
    if (bundle != applications.end()) {
      none.blocks = bundle->second.blocks;
      none.blocks_damaged = bundle->second.blocks_damaged;
    }
    out.push_back(std::move(none));
  }
  for (TeletextPageDataStream& data : out) {
    data.pages = stream.pages;
    data.pages_lost = stream.pages_lost;
    data.packets_lost = stream.packets_lost;
  }
  return out;
}

std::vector<TeletextPageDataStream> TeletextPageFormatClear::streams() const {
  std::vector<TeletextPageDataStream> out;
  size_t held_bytes = held_bytes_;
  for (const auto& entry : streams_) {
    Stream stream = entry.second;
    if (stream.pending.has_value()) {
      append_page(stream, *stream.pending, packet_bytes_, held_bytes);
    }
    for (TeletextPageDataStream& data : parse(stream)) {
      out.push_back(std::move(data));
    }
  }
  return out;
}

std::string TeletextPageDataStream::tag() const {
  return fmt::format("{}{:02X}-s{}-a{}", magazine, page_number, stream_id + 1,
                     application);
}

std::string TeletextPageDataStream::summary() const {
  std::string out = fmt::format("page {}{:02X}, data stream {}", magazine,
                                page_number, stream_id + 1);
  if (application > 0) {
    out += fmt::format(", application {}", application);
    if (application_type.has_value()) {
      out += fmt::format(" of type 0x{:04X}", *application_type);
    }
  }
  out += fmt::format(
      ": {} page{} read, {} lost, {} packet{} lost; {} block{} read whole, {} "
      "damaged",
      pages, pages == 1 ? "" : "s", pages_lost, packets_lost,
      packets_lost == 1 ? "" : "s", blocks, blocks == 1 ? "" : "s",
      blocks_damaged);
  if (application > 0) {
    out += fmt::format("; {} byte{} of payload", payload.size(),
                       payload.size() == 1 ? "" : "s");
  }
  return out;
}

}  // namespace orc
