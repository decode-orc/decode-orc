/*
 * File:        nabts_framed_service.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Datacast services that send their data in counted frames
 *              striped across a family of addresses, under a fixed pad
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_framed_service.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include "nabts_bytes.h"
#include "nabts_data_group.h"
#include "nabts_datacast_files.h"
#include "nabts_framed_catalogue.h"
#include "nabts_framed_containers.h"
#include "nabts_service_census.h"

namespace orc {

namespace {

// The pad (nabts_frame_pad()): what each byte of a frame of zeros arrives as,
// bytes 0-18 the header's and the object's after them.
constexpr std::array<uint8_t, kNabtsFrameBytes> kFramePad = {
    0xE9, 0x93, 0x00, 0x00, 0xCC, 0x09, 0xEE, 0xCF, 0x83, 0x00, 0x00, 0x00,
    0x00, 0x7C, 0x60, 0xA5, 0xAD, 0x77, 0xB8, 0xC7, 0x3F, 0x8B, 0x78, 0x57,
    0x5C, 0x18, 0xB2, 0x32, 0xBC, 0x9D, 0x3E, 0x10, 0xCF, 0xBF, 0x70, 0x39,
    0x4B, 0x73, 0x39, 0x27, 0xA1, 0xD8, 0x54, 0xC0, 0x70, 0x79, 0xA1, 0x2E,
    0xAE, 0xEC, 0x71, 0x14, 0xAD, 0xF7, 0x83, 0xA0, 0x10, 0x1C, 0x05, 0x01,
    0xAE, 0x77, 0x23, 0x95, 0xDE, 0xF8, 0x77, 0xA3, 0x71, 0x79, 0xD7, 0xE0,
    0xF1, 0xC2, 0x8B, 0xB8, 0x49, 0x28, 0xA0, 0xEE, 0xD4, 0xD6, 0x96, 0x66,
    0xD3, 0xFA, 0x6C, 0x27, 0xAB, 0x9B, 0x12, 0x9C, 0xA0, 0x98, 0x3F, 0xBD,
    0xB8, 0x9C, 0xE5, 0x29, 0xB3, 0x04, 0x9A, 0x89, 0xD8, 0x0F, 0xB4, 0x1E,
    0xB2, 0xF2, 0x19, 0x5F, 0x86, 0x4C, 0x9F, 0xE5, 0x7B, 0x61, 0xE5, 0xE9,
    0xEA, 0x70, 0x22, 0xD5, 0x35, 0x65, 0xBF, 0x46, 0x46, 0xC7, 0xCA, 0x08,
    0xED, 0x9C, 0xFE, 0x40, 0xF5, 0x58, 0x02, 0x00, 0x46, 0x9C, 0xBE, 0x5D,
    0x26, 0xCF, 0xC9, 0xBE, 0x9F, 0x65, 0x89, 0x95, 0xCA, 0x7E, 0xFB, 0x1A,
    0xD0, 0xBA, 0xEA, 0xC6, 0xDD, 0x23, 0xA3, 0xC3, 0x74, 0xCE, 0x92, 0xBC,
    0x44, 0xF1, 0xF4, 0x58, 0xEA, 0xF0, 0x19, 0xB2, 0x1A, 0xF2, 0xC2, 0x10,
    0xB4, 0xA8, 0x5B, 0xAF, 0x8F, 0xF9, 0xB7, 0xF3, 0x4B, 0x9E, 0xA5, 0xD9,
    0xFC, 0x2D, 0x0C, 0xC2, 0xCE, 0x3F, 0x3D, 0x90, 0x3B, 0xCA, 0xBE, 0xDD,
    0xB7, 0x68, 0xE6, 0x29, 0xD6, 0xD6, 0x7B, 0xFA, 0x6D, 0xA7, 0x99, 0x7F,
    0x62, 0x25, 0x54, 0x00, 0x29, 0x0D, 0x19, 0x72, 0x43, 0x86, 0x7A, 0x4C,
    0x59, 0xC2, 0x50, 0xF7, 0x7B, 0xCC, 0x64, 0xA4, 0x20, 0x4E, 0xC4, 0x7C,
    0x72, 0x14, 0xEB, 0x2B, 0x62, 0xD3, 0xA1, 0x18, 0x28, 0x60, 0xC8, 0x25,
    0x6E, 0xCA, 0xA5, 0xB4, 0xF1, 0x34, 0x7E, 0xA0, 0x48, 0x45, 0x71, 0xB9,
    0xF9, 0x9A, 0x49, 0x9E, 0x7B, 0x0C, 0x42, 0x70, 0x98, 0x12, 0x07, 0x81,
    0x79, 0x4C, 0xB2, 0x5F, 0xB1, 0x84, 0x4C, 0x72, 0x49, 0xC5, 0x3C, 0x10,
    0x89, 0x23, 0xCE, 0x64, 0x6D, 0xBC, 0xF0, 0x99, 0x3E, 0xBD, 0xDD, 0x55,
    0xBA, 0x07, 0x5A, 0x34, 0x7E, 0x56, 0x9B, 0xD2, 0x70, 0xD4, 0x20, 0x63,
    0x64, 0xD2, 0x97, 0xBD, 0xEA, 0x86, 0xD7, 0xCD, 0x34, 0x08, 0x6E, 0x11,
    0x6B, 0x8B, 0x15, 0xF0, 0x45, 0x6A, 0xD0, 0x17, 0xC6, 0xD1, 0x17, 0x1D,
    0x9F, 0x48, 0x33, 0xBF, 0x2F, 0xD7, 0x60, 0xE5, 0x65, 0xA4, 0x2F, 0x0C,
    0x9B, 0x52, 0x81, 0x60, 0x0F, 0xF4, 0x03, 0x00, 0x65, 0xD2, 0xE1, 0x73,
    0xB5, 0xA8, 0x2D, 0x61, 0xD0, 0xD7, 0x4D, 0x5F, 0xAF, 0x41, 0x86, 0x97,
    0x38, 0xE7, 0x9F, 0xA5, 0xB3, 0xB2, 0x72, 0x22, 0x4E, 0xA9, 0xDB, 0xE2,
    0x66, 0x89, 0x0E, 0x74, 0x9F, 0x88, 0x15, 0x6B, 0x97, 0x8B, 0xA3, 0x18,
    0x6E, 0xFC, 0x76, 0x78, 0x48, 0x05, 0x6C, 0x0A, 0x6E, 0x51, 0xF7, 0x35,
    0x82, 0x3B, 0x8A, 0xA3, 0x29, 0x20, 0xA3, 0x58,
};

// The header bytes that are zero under the pad, which say a row opens a
// frame. Bytes 7-8 are too, but in the frame that ends its object.
constexpr std::array<size_t, 3> kHeaderZeroBytes = {0, 1, 4};

// Every row of a frame, a bit each.
constexpr uint16_t kAllRows = 0xFFFF;

// The Unix times an object's time is believed in: one outside them is no
// time the service gave.
constexpr int64_t kEarliestTime = 631152000;  // 1990-01-01
constexpr int64_t kLatestTime = 1893456000;   // 2030-01-01

// The gap left between one run of counts and the next: more than any step
// of a count, so no gap within a run is taken for one between runs.
constexpr int64_t kRunGap = int64_t{1} << 17;

// How many objects numbered either side of one whose header was lost are
// asked the length of their names.
constexpr uint16_t kSiblingReach = 2;

uint16_t row_bit(size_t row) { return static_cast<uint16_t>(1U << row); }

/// One frame read: its bytes, the pad taken off, and where and when it
/// arrived. A row that did not arrive is held as zeros.
struct FrameRead {
  std::array<uint8_t, kNabtsFrameBytes> bytes{};
  /// Where its bytes end: before kNabtsFrameBytes where its service cut it
  /// short.
  size_t length = kNabtsFrameBytes;
  /// The rows that arrived, and those of them that failed their row code and
  /// were taken as received, bit i for row i. The header row of a frame read
  /// by its own header always arrived, and passed.
  uint16_t rows = 0;
  uint16_t damaged = 0;
  uint16_t channel = 0;
  uint32_t arrived = 0;
  /// Its header was put back from the frames sent around it.
  bool put_back = false;

  bool whole() const { return rows == kAllRows; }
  uint16_t count() const { return nabts_le16(&bytes[5]); }
  uint32_t service() const {
    return (static_cast<uint32_t>(bytes[9]) << 24U) |
           (static_cast<uint32_t>(bytes[10]) << 16U) |
           (static_cast<uint32_t>(bytes[11]) << 8U) | bytes[12];
  }
  uint16_t object() const { return nabts_le16(&bytes[13]); }
  uint32_t place() const { return nabts_le32(&bytes[15]); }
  /// Whether it is the frame that ends its object (bytes 7-8 set).
  bool ends() const { return bytes[7] != 0 || bytes[8] != 0; }
  /// Whether byte |i| arrived.
  bool holds(size_t i) const {
    return i < length && (rows & row_bit(i / kNabtsFrameRowBytes)) != 0;
  }
};

/// How a bundle's header row read.
enum class Opening : uint8_t {
  kFrame,          // arrived, passing its row code, and opening a frame
  kHeaderLost,     // went missing
  kHeaderDamaged,  // failed its row code and was taken as received
  kNoHeader,       // arrived but opens no frame
};

/// A bundle not read as a frame of the service it is looked at for, and why.
struct Unread {
  enum class Why : uint8_t {
    kHeaderLost,
    kHeaderDamaged,
    kNoHeader,
    kUnlisted,    // a frame of a name no service listed bears
    kWildCount,   // a frame whose count no frame around it agrees with
    kOtherBlock,  // a frame read on an address of another block
  };
  FrameRead read;
  Why why = Why::kHeaderLost;
  bool used = false;
};

/// The bundle that starts at row |r| of |datacast|: its rows that arrived,
/// the pad taken off — those after the one where its service cut it short
/// holding only filler — and how its header row read. Nothing where the
/// datacast holds no bundle's rows there in order.
std::optional<std::pair<FrameRead, Opening>> bundle_at(
    const NabtsDatacastChannel& datacast, size_t r) {
  if (r + kNabtsFrameRows > datacast.rows.size()) {
    return std::nullopt;
  }
  const NabtsDatacastRow& head = datacast.rows[r];
  FrameRead read;
  uint32_t end = head.offset;
  bool ended = false;
  for (size_t i = 0; i < kNabtsFrameRows; ++i) {
    const NabtsDatacastRow& row = datacast.rows[r + i];
    if (row.offset != end || (ended && row.length != 0) ||
        row.offset + row.length > datacast.payload.size()) {
      return std::nullopt;
    }
    if (row.missing) {
      continue;
    }
    const size_t at = i * kNabtsFrameRowBytes;
    for (size_t j = 0; j < row.length; ++j) {
      read.bytes[at + j] = datacast.payload[row.offset + j] ^ kFramePad[at + j];
    }
    read.rows |= row_bit(i);
    read.damaged |= row.damaged ? row_bit(i) : 0;
    if (row.length != kNabtsFrameRowBytes && !ended) {
      ended = true;
      read.length = at + row.length;
    }
    end += row.length;
  }
  read.channel = datacast.channel;
  read.arrived = head.frame;
  Opening opening = Opening::kFrame;
  if (head.missing) {
    opening = Opening::kHeaderLost;
  } else if (head.damaged) {
    opening = Opening::kHeaderDamaged;
  } else if (head.length != kNabtsFrameRowBytes ||
             !nabts_frame_header(read.bytes.data())) {
    opening = Opening::kNoHeader;
  }
  return std::make_pair(read, opening);
}

/// The frame that starts at row |r| of |datacast|, if its header row arrived
/// and passed its row code (bundle_at()).
std::optional<FrameRead> frame_at(const NabtsDatacastChannel& datacast,
                                  size_t r) {
  if (r >= datacast.rows.size()) {
    return std::nullopt;
  }
  const NabtsDatacastRow& head = datacast.rows[r];
  if (head.missing || head.damaged || head.length != kNabtsFrameRowBytes ||
      head.offset + kNabtsFrameRowBytes > datacast.payload.size()) {
    return std::nullopt;
  }
  for (const size_t i : kHeaderZeroBytes) {
    if (datacast.payload[head.offset + i] != kFramePad[i]) {
      return std::nullopt;
    }
  }
  const auto bundle = bundle_at(datacast, r);
  if (!bundle || bundle->second != Opening::kFrame) {
    return std::nullopt;
  }
  return bundle->first;
}

/// The bytes of row |row| of a frame that are its object's: all of them but
/// the header's.
std::pair<size_t, size_t> object_bytes_of_row(size_t row) {
  return {std::max(row * kNabtsFrameRowBytes, kNabtsFrameHeaderBytes),
          (row + 1) * kNabtsFrameRowBytes};
}

/// The reading to believe of a frame from its |copies| in |frames|, row by
/// row: the commonest reading of the object's bytes of the row among the
/// copies it arrived in without failing its code, or, where it arrived in
/// none so, among those that took it as received. The header is that of the
/// first copy read by its own header, or of the first where none was.
FrameRead merged(const std::vector<FrameRead>& frames,
                 const std::vector<size_t>& copies) {
  size_t head = copies.front();
  for (const size_t c : copies) {
    if (!frames[c].put_back) {
      head = c;
      break;
    }
  }
  const FrameRead& first = frames[head];
  if (copies.size() == 1) {
    return first;
  }
  FrameRead out;
  std::copy(first.bytes.begin(), first.bytes.begin() + kNabtsFrameHeaderBytes,
            out.bytes.begin());
  out.channel = first.channel;
  out.arrived = first.arrived;
  out.put_back = first.put_back;
  out.length = first.length;
  for (const size_t c : copies) {
    out.length = std::min(out.length, frames[c].length);
  }
  // Each row's readings counted — the object's bytes of it — with the copy
  // each was first read in, which settles a tie.
  using RowBytes = std::array<uint8_t, kNabtsFrameRowBytes>;
  std::map<RowBytes, std::pair<size_t, size_t>> votes;
  for (size_t row = 0; row < kNabtsFrameRows; ++row) {
    const uint16_t bit = row_bit(row);
    const auto [from, to] = object_bytes_of_row(row);
    for (const bool clean : {true, false}) {
      votes.clear();
      for (size_t k = 0; k < copies.size(); ++k) {
        const FrameRead& f = frames[copies[k]];
        if ((f.rows & bit) == 0 || ((f.damaged & bit) == 0) != clean) {
          continue;
        }
        RowBytes reading{};
        std::copy(f.bytes.begin() + static_cast<ptrdiff_t>(from),
                  f.bytes.begin() + static_cast<ptrdiff_t>(to),
                  reading.begin());
        auto [entry, made] = votes.try_emplace(reading, 0, k);
        ++entry->second.first;
      }
      const auto best = std::max_element(
          votes.begin(), votes.end(), [](const auto& a, const auto& b) {
            return a.second.first < b.second.first ||
                   (a.second.first == b.second.first &&
                    a.second.second > b.second.second);
          });
      if (best != votes.end()) {
        std::copy(best->first.begin(),
                  best->first.begin() + static_cast<ptrdiff_t>(to - from),
                  out.bytes.begin() + static_cast<ptrdiff_t>(from));
        out.rows |= bit;
        out.damaged |= clean ? 0 : bit;
        break;
      }
    }
  }
  return out;
}

/// "0F0-0FF", or "071-07F (8 addresses)" for a family that skips some.
std::string channel_label(const std::vector<uint16_t>& channels) {
  if (channels.size() == 1) {
    return fmt::format("{:03X}", channels.front());
  }
  std::string label =
      fmt::format("{:03X}-{:03X}", channels.front(), channels.back());
  if (channels.back() - channels.front() + 1U != channels.size()) {
    label += fmt::format(" ({} addresses)", channels.size());
  }
  return label;
}

/// What an object's first frame says of it in its own header.
struct ObjectHeader {
  uint32_t time = 0;
  uint32_t size = 0;
  std::string name;
  /// Where in the frame's bytes after its header the object's bytes begin.
  size_t data_at = 0;
};

/// The object's header that opens |first|, the first frame of object
/// |number|: two zero bytes, the number again, its time, its size and its
/// name, printable, as a length and that many bytes — all of it arrived.
std::optional<ObjectHeader> object_header(const FrameRead& first,
                                          uint16_t number) {
  const auto arrived = [&first](size_t from, size_t to) {
    for (size_t i = from; i < to; ++i) {
      if (!first.holds(kNabtsFrameHeaderBytes + i)) {
        return false;
      }
    }
    return true;
  };
  if (!arrived(0, kNabtsFrameObjectHeaderBytes)) {
    return std::nullopt;
  }
  const uint8_t* p = first.bytes.data() + kNabtsFrameHeaderBytes;
  if (p[0] != 0 || p[1] != 0 || nabts_le16(p + 2) != number) {
    return std::nullopt;
  }
  const size_t name_length = nabts_le16(p + 12);
  if (name_length == 0 ||
      kNabtsFrameObjectHeaderBytes + name_length > kNabtsFramePayloadBytes ||
      !arrived(kNabtsFrameObjectHeaderBytes,
               kNabtsFrameObjectHeaderBytes + name_length)) {
    return std::nullopt;
  }
  ObjectHeader header;
  header.time = nabts_le32(p + 4);
  header.size = nabts_le32(p + 8);
  header.data_at = kNabtsFrameObjectHeaderBytes + name_length;
  for (size_t i = kNabtsFrameObjectHeaderBytes; i < header.data_at; ++i) {
    if (p[i] < 0x20 || p[i] >= 0x7F) {
      return std::nullopt;
    }
    header.name.push_back(static_cast<char>(p[i]));
  }
  return header;
}

/// The copies of the frame at each place of one version of an object.
using ObjectPlaces = std::map<uint32_t, std::vector<size_t>>;

/// The header a version of object |number| opens with, if its first frame
/// arrived and reads as one.
std::optional<ObjectHeader> version_header(const std::vector<FrameRead>& frames,
                                           uint16_t number,
                                           const ObjectPlaces& places) {
  const auto first = places.find(0);
  if (first == places.end()) {
    return std::nullopt;
  }
  return object_header(merged(frames, first->second), number);
}

/// An object put together: the file it holds, and whether all of it arrived
/// — its header and every byte its header says it has.
struct ObjectRead {
  NabtsCarriedFile file;
  bool whole = false;
};

/// A version of object |number|, |places| the copies of its frame at each
/// place, as the file it holds; nothing where too little of it arrived to say
/// what it is, or where it would take more than |budget| bytes. A version
/// whose first frame was not read is named and dated by |known|, the header
/// another version of the object gave, where one did. A version whose first
/// frame arrived without its header whole has that frame's bytes placed from
/// |assumed_data_at| where it is given, and left out otherwise.
std::optional<ObjectRead> object_file(
    const std::vector<FrameRead>& frames, uint16_t number,
    const ObjectPlaces& places, const std::optional<ObjectHeader>& known,
    const std::vector<uint16_t>& channels, size_t budget,
    std::optional<size_t> assumed_data_at = std::nullopt) {
  const std::optional<ObjectHeader> own =
      version_header(frames, number, places);
  const std::optional<ObjectHeader>& header = own ? own : known;
  if (!own && places.size() < kNabtsFramedMinObjectFrames) {
    return std::nullopt;
  }
  NabtsCarriedFile file;
  file.channel = channels.front();
  file.last_channel = channels.back();
  file.transfer = "framed service";
  size_t size = 0;
  if (header && header->size <= kNabtsCarriedBytesMax) {
    size = header->size;
  } else {
    for (const auto& [place, copies] : places) {
      const size_t length =
          frames[copies.front()].length - kNabtsFrameHeaderBytes;
      if (place > 0 && place + length <= kNabtsCarriedBytesMax) {
        size = std::max(size, place + length);
      }
    }
  }
  // Its data and the record of which bytes arrived are each |size| bytes, and
  // a size taken from a misread place can be tens of megabytes: an object that
  // would not fit what is left of the budget is not built.
  if (size > budget / 2) {
    return std::nullopt;
  }
  if (header) {
    file.name = header->name;
    const auto time = static_cast<int64_t>(header->time);
    if (time >= kEarliestTime && time < kLatestTime) {
      file.time = nabts_utc_text(time) + " UTC";
    }
  } else {
    file.name = fmt::format("object-{:04x}", number);
  }
  file.data.assign(size, 0);
  std::vector<uint8_t> present(size, kNabtsByteLost);
  for (const auto& [place, copies] : places) {
    file.copies = std::max(file.copies, static_cast<uint32_t>(copies.size()));
    size_t from = kNabtsFrameHeaderBytes;
    if (place == 0) {
      if (own) {
        from += own->data_at;
      } else if (assumed_data_at) {
        from += *assumed_data_at;
      } else {
        continue;  // its bytes cannot be placed without its header's length
      }
    }
    if (place >= size) {
      continue;
    }
    const FrameRead frame = merged(frames, copies);
    const size_t end = std::min(frame.length, from + (size - place));
    for (size_t j = from; j < end; ++j) {
      if (frame.holds(j)) {
        file.data[place + j - from] = frame.bytes[j];
        present[place + j - from] = kNabtsByteArrived;
      }
    }
  }
  const bool lost = std::find(present.begin(), present.end(), kNabtsByteLost) !=
                    present.end();
  if (lost) {
    file.data_present = std::move(present);
  }
  file.format = nabts_file_format(file.data, file.data_present);
  return ObjectRead{std::move(file),
                    own.has_value() && !lost && size == own->size};
}

/// Whether two sets of copies of the frame at one place read alike: the
/// object's bytes of every row both hold without its having failed its code.
bool read_alike(const std::vector<FrameRead>& frames,
                const std::vector<size_t>& a, const std::vector<size_t>& b) {
  const FrameRead x = merged(frames, a);
  const FrameRead y = merged(frames, b);
  const uint16_t both = x.rows & y.rows & ~x.damaged & ~y.damaged;
  for (size_t row = 0; row < kNabtsFrameRows; ++row) {
    if ((both & row_bit(row)) == 0) {
      continue;
    }
    const auto [from, to] = object_bytes_of_row(row);
    if (!std::equal(x.bytes.begin() + static_cast<ptrdiff_t>(from),
                    x.bytes.begin() + static_cast<ptrdiff_t>(to),
                    y.bytes.begin() + static_cast<ptrdiff_t>(from))) {
      return false;
    }
  }
  return true;
}

/// An object's versions from its frames |sent| — each a place and the frame
/// there, in the order they were sent. A pass over the object runs while its
/// places rise; one is taken with the passes before it while their frames read
/// alike wherever both hold one, and one that reads differently, the object
/// sent again changed, starts a version of its own.
std::vector<ObjectPlaces> object_versions(
    const std::vector<FrameRead>& frames,
    const std::vector<std::pair<uint32_t, size_t>>& sent) {
  std::vector<ObjectPlaces> versions;
  ObjectPlaces pass;
  const auto close = [&]() {
    if (pass.empty()) {
      return;
    }
    bool alike = !versions.empty();
    for (auto it = pass.begin(); alike && it != pass.end(); ++it) {
      const auto held = versions.back().find(it->first);
      alike = held == versions.back().end() ||
              read_alike(frames, held->second, it->second);
    }
    if (alike) {
      for (auto& [place, copies] : pass) {
        auto& into = versions.back()[place];
        into.insert(into.end(), copies.begin(), copies.end());
      }
    } else {
      versions.push_back(std::move(pass));
    }
    pass = {};
  };
  std::optional<uint32_t> last;
  for (const auto& [place, frame] : sent) {
    if (last && place <= *last) {
      close();
    }
    pass[place].push_back(frame);
    last = place;
  }
  close();
  return versions;
}

/// The files of |files| from |from| on that a service sent in numbered parts
/// — NAME.000, NAME.001 and on, of one time, every part whole and none
/// missing between — joined into one each, after them. Each file joined
/// records the parts it was joined from in |parts_of|, by index.
void join_parts(std::vector<NabtsCarriedFile>& files, size_t from,
                std::map<size_t, std::vector<size_t>>& parts_of) {
  // Each part's first version, by its stem and time and its number.
  std::map<std::pair<std::string, std::string>, std::map<unsigned, size_t>>
      groups;
  for (size_t i = from; i < files.size(); ++i) {
    const std::string& name = files[i].name;
    const size_t dot = name.size() >= 5 ? name.size() - 4 : std::string::npos;
    if (dot == std::string::npos || name[dot] != '.' ||
        !std::all_of(name.begin() + static_cast<ptrdiff_t>(dot) + 1, name.end(),
                     [](char c) { return c >= '0' && c <= '9'; })) {
      continue;
    }
    const auto part = static_cast<unsigned>(std::stoul(name.substr(dot + 1)));
    groups[{name.substr(0, dot), files[i].time}].try_emplace(part, i);
  }
  for (const auto& [key, parts] : groups) {
    const bool whole =
        std::all_of(parts.begin(), parts.end(), [&files](const auto& entry) {
          const NabtsCarriedFile& part = files[entry.second];
          return part.data_present.empty() && !part.cut_short;
        });
    if (parts.size() < 2 || parts.begin()->first != 0 ||
        parts.rbegin()->first != parts.size() - 1 || !whole) {
      continue;
    }
    NabtsCarriedFile joined;
    const NabtsCarriedFile& first = files[parts.begin()->second];
    joined.channel = first.channel;
    joined.last_channel = first.last_channel;
    joined.transfer = first.transfer;
    joined.time = first.time;
    joined.copies = first.copies;
    joined.name = key.first;
    joined.joined_parts = static_cast<uint32_t>(parts.size());
    std::vector<size_t> joined_from;
    for (const auto& [number, index] : parts) {
      const std::vector<uint8_t>& data = files[index].data;
      if (joined.data.size() + data.size() > kNabtsCarriedBytesMax) {
        break;
      }
      joined.copies = std::min(joined.copies, files[index].copies);
      joined.data.insert(joined.data.end(), data.begin(), data.end());
      joined_from.push_back(index);
    }
    joined.format = nabts_file_format(joined.data, joined.data_present);
    parts_of[files.size()] = std::move(joined_from);
    files.push_back(std::move(joined));
  }
}

/// The step from count |a| to count |b|, across a wrap.
int64_t count_step(uint16_t a, uint16_t b) {
  return static_cast<int16_t>(static_cast<uint16_t>(b - a));
}

/// A service's frames counted: each frame of |frames| (in the order they
/// arrived) under its count, followed across a wrap, a count that falls back
/// further than kNabtsFramedReorderFrames starting a run of its own after a
/// gap of kRunGap.
struct Counted {
  std::map<int64_t, std::vector<size_t>> by_count;
  uint64_t runs = 0;
};

Counted count_frames(const std::vector<FrameRead>& frames) {
  Counted out;
  if (frames.empty()) {
    return out;
  }
  int64_t at = frames.front().count();
  int64_t top = at;
  out.runs = 1;
  out.by_count[at].push_back(0);
  for (size_t i = 1; i < frames.size(); ++i) {
    const int64_t step = count_step(frames[i - 1].count(), frames[i].count());
    if (step < -kNabtsFramedReorderFrames) {
      at = top + kRunGap + frames[i].count();
      ++out.runs;
    } else {
      at += step;
    }
    top = std::max(top, at);
    out.by_count[at].push_back(i);
  }
  return out;
}

/// The most frames of |counted| counted close together: none more than
/// kNabtsFramedCloseCounts after the one before.
uint64_t longest_close(const Counted& counted) {
  uint64_t stretch = 0;
  uint64_t longest = 0;
  std::optional<int64_t> before;
  for (const auto& entry : counted.by_count) {
    stretch = before && entry.first - *before <= kNabtsFramedCloseCounts
                  ? stretch + 1
                  : 1;
    longest = std::max(longest, stretch);
    before = entry.first;
  }
  return longest;
}

/// Whether, among |frames|, at least kNabtsFramedMinFollowingFrames follow
/// each other: each counted one after the one before and carrying its object
/// on at a later place.
bool frames_follow(const std::vector<FrameRead>& frames) {
  std::vector<const FrameRead*> by_count;
  for (const FrameRead& frame : frames) {
    by_count.push_back(&frame);
  }
  std::sort(by_count.begin(), by_count.end(),
            [](const FrameRead* a, const FrameRead* b) {
              return a->count() < b->count();
            });
  size_t following = 1;
  for (size_t i = 1; i < by_count.size(); ++i) {
    const FrameRead& a = *by_count[i - 1];
    const FrameRead& b = *by_count[i];
    following = count_step(a.count(), b.count()) == 1 &&
                        a.object() == b.object() && b.place() > a.place()
                    ? following + 1
                    : 1;
    if (following >= kNabtsFramedMinFollowingFrames) {
      return true;
    }
  }
  return false;
}

/// The object and place a frame missing between |before| and |after| — the
/// frames counted one either side of it, where read — must have had: the
/// object they carry on and the place between theirs; the first place of the
/// object the frame after it carries on, where it is that object's second
/// frame and the frame before it ended its own; or the place after
/// |before|'s on its object, where the frame after it opens an object of its
/// own. Nothing where they do not say, or say otherwise of each other.
std::optional<std::pair<uint16_t, uint32_t>> header_between(
    const std::optional<FrameRead>& before,
    const std::optional<FrameRead>& after) {
  const uint32_t step = kNabtsFramePayloadBytes;
  if (before && after && before->object() == after->object()) {
    if (after->place() <= step || after->place() - step <= before->place()) {
      return std::nullopt;
    }
    return std::make_pair(after->object(), after->place() - step);
  }
  if (after && after->place() > 0 && after->place() < step &&
      (!before || before->ends())) {
    return std::make_pair(after->object(), uint32_t{0});
  }
  if (before && !before->ends() && (!after || after->place() == 0)) {
    if (before->place() > 0) {
      return std::make_pair(before->object(), before->place() + step);
    }
    if (const std::optional<ObjectHeader> header =
            object_header(*before, before->object())) {
      return std::make_pair(
          before->object(),
          static_cast<uint32_t>(kNabtsFramePayloadBytes - header->data_at));
    }
  }
  if (!before && after && after->place() > step) {
    return std::make_pair(after->object(), after->place() - step);
  }
  return std::nullopt;
}

/// |read| as frame |count| of service |identifier|, object |object| at
/// |place|: its header put back, its header row's own bytes of the object
/// taken as received where the row arrived.
FrameRead put_back(FrameRead read, uint32_t identifier, uint16_t count,
                   uint16_t object, uint32_t place) {
  std::fill(read.bytes.begin(), read.bytes.begin() + kNabtsFrameHeaderBytes,
            uint8_t{0});
  read.bytes[5] = static_cast<uint8_t>(count & 0xFFU);
  read.bytes[6] = static_cast<uint8_t>(count >> 8U);
  for (size_t i = 0; i < 4; ++i) {
    read.bytes[9 + i] = static_cast<uint8_t>(identifier >> (24 - 8 * i));
    read.bytes[15 + i] = static_cast<uint8_t>(place >> (8 * i));
  }
  read.bytes[13] = static_cast<uint8_t>(object & 0xFFU);
  read.bytes[14] = static_cast<uint8_t>(object >> 8U);
  if ((read.rows & row_bit(0)) != 0) {
    read.damaged |= row_bit(0);
  }
  read.put_back = true;
  return read;
}

/// A listed service as its frames are read: the frames kept, by count.
struct ServiceFrames {
  uint32_t identifier = 0;
  std::vector<FrameRead> frames;
  Counted counted;
  /// The blocks of addresses its frames came on.
  std::set<uint16_t> blocks;
};

/// The kind of file |file| is, as a reader names it.
std::string kind_of(const NabtsCarriedFile& file) {
  if (file.format == "gif") return "GIF image";
  if (file.format == "png") return "PNG image";
  if (file.format == "jpeg") return "JPEG image";
  if (file.format == "html") return "HTML page";
  if (file.format == "text") return "text file";
  return "binary file";
}

/// How whole |file| came out, in words.
std::string condition_of(const NabtsCarriedFile& file) {
  const size_t lost = file.bytes_lost();
  if (lost > 0) {
    return fmt::format("{} byte(s) no copy delivered", lost);
  }
  if (file.cut_short) {
    return "unpacked as far as its container's bytes went";
  }
  if (file.checksum_holds.has_value()) {
    return *file.checksum_holds ? "whole, its check holding"
                                : "its check failing: bytes of it misread";
  }
  return "whole";
}

/// How many members of the ZIP archive |data| holds unpack with their CRC-32
/// holding; 0 for no archive.
size_t members_checked(const std::vector<uint8_t>& data) {
  const std::vector<NabtsArchiveMember> members = nabts_archive_members(data);
  return static_cast<size_t>(
      std::count_if(members.begin(), members.end(),
                    [](const NabtsArchiveMember& m) { return m.crc_holds; }));
}

/// What the archive |members| hold, as text: each member's name, date, size
/// and check.
std::string members_text(const std::vector<NabtsArchiveMember>& members) {
  std::string out =
      fmt::format("A ZIP archive of {} member(s):\n", members.size());
  for (const NabtsArchiveMember& member : members) {
    out += fmt::format("  {}  {}  {} bytes, ", member.name, member.time,
                       member.size);
    if (!member.unpacked) {
      out +=
          member.method == 0 || member.method == 8
              ? "its bytes broken off"
              : fmt::format("stored by method {}, not unpacked", member.method);
    } else {
      out += member.crc_holds ? "its CRC-32 holding" : "its CRC-32 failing";
    }
    out += "\n";
  }
  return out;
}

/// A service's frames read one to a count, in the order of the counts, with
/// the addresses each count's copies came on and how many frames were put
/// back from the frames around them.
struct CountedFrames {
  std::vector<FrameRead> frames;
  std::vector<std::vector<uint16_t>> on;
  uint64_t put_back = 0;
};

/// The length of the name the objects numbered either side of |number| give,
/// within kSiblingReach, where they agree; nothing where none gives one or
/// two disagree.
std::optional<size_t> sibling_name_length(
    const std::map<uint16_t, size_t>& name_lengths, uint16_t number) {
  std::optional<size_t> length;
  for (uint16_t d = 1; d <= kSiblingReach; ++d) {
    for (const uint16_t sibling : {static_cast<uint16_t>(number - d),
                                   static_cast<uint16_t>(number + d)}) {
      const auto it = name_lengths.find(sibling);
      if (it == name_lengths.end()) {
        continue;
      }
      if (length && *length != it->second) {
        return std::nullopt;
      }
      length = it->second;
    }
  }
  return length;
}

/// What the report says of a service's frames, objects and containers.
std::string service_summary(const NabtsFramedService& framed, uint64_t runs,
                            uint64_t damaged, uint64_t put_back_frames) {
  std::string summary = fmt::format(
      "frames on {}: {} read, {} of them only in part, counted {:04X} to "
      "{:04X}{}, {} sent and not read, {} read again",
      channel_label(framed.channels), framed.frames, framed.frames_in_part,
      framed.first_frame, framed.last_frame,
      runs > 1 ? fmt::format(" in {} runs", runs) : std::string(),
      framed.frames_lost, framed.repeats);
  if (damaged > 0) {
    summary += fmt::format(", {} with a row taken as received", damaged);
  }
  if (put_back_frames > 0) {
    summary += fmt::format(", {} put back from the frames around them",
                           put_back_frames);
  }
  summary += fmt::format(
      "; read under its pad, {} object(s) in {} version(s), {} of them "
      "whole",
      framed.objects, framed.versions, framed.versions_whole);
  if (framed.containers > 0) {
    summary += fmt::format(
        "; {} in the service's containers, {} unpacked whole and checked, "
        "{} only in part",
        framed.containers, framed.containers_unpacked,
        framed.containers_in_part);
    if (framed.containers_reopened > 0) {
      summary +=
          fmt::format(", {} of them opened again where their opening was lost",
                      framed.containers_reopened);
    }
  }
  return summary;
}

/// A dBASE table's contents as the file listing shows them.
std::string table_contents(const NabtsTable& table) {
  return fmt::format(
             "A dBASE table, last written {}: {} field(s), {} record(s){}\n\n",
             table.updated, table.fields.size(), table.records.size(),
             table.deleted > 0
                 ? fmt::format(", {} more marked deleted", table.deleted)
                 : std::string()) +
         nabts_table_text(table);
}

/**
 * @brief The framed services read out of datacasts, step by step
 *
 * What nabts_framed_services() does, each step a method and what one leaves
 * the next held here: the bundles no service read (each with why), every
 * address's use, the files built and the bytes they and the frames streamed
 * hold against their bounds.
 */
class FramedServicesReader {
 public:
  explicit FramedServicesReader(size_t max_file_bytes)
      : max_file_bytes_(max_file_bytes) {}

  NabtsFramedDatacasts read(
      const std::vector<NabtsDatacastChannel>& datacasts) && {
    std::map<uint32_t, std::vector<FrameRead>> gathered = gather(datacasts);
    std::vector<ServiceFrames> listed = list(gathered);
    put_back_missing(listed);
    for (ServiceFrames& service : listed) {
      read_service(service);
    }
    list_archive_members();
    account_channels();
    return std::move(out_);
  }

 private:
  /// Every frame of the datacasts, by the service its header names. A
  /// datacast in bundles of a frame's rows holds a frame to a bundle, so a
  /// frame is looked for only where one starts: a header row filed under the
  /// wrong address opens none. Each such bundle is accounted for, and one
  /// whose header did not read is kept as a bundle not read.
  std::map<uint32_t, std::vector<FrameRead>> gather(
      const std::vector<NabtsDatacastChannel>& datacasts) {
    std::map<uint32_t, std::vector<FrameRead>> gathered;
    size_t held = 0;
    for (const NabtsDatacastChannel& datacast : datacasts) {
      if (datacast.raw) {
        continue;
      }
      if (datacast.layout.data_rows == kNabtsFrameRows) {
        for (size_t r = 0; r + kNabtsFrameRows <= datacast.rows.size();
             r += kNabtsFrameRows) {
          const auto bundle = bundle_at(datacast, r);
          if (!bundle || held + sizeof(FrameRead) > kNabtsFramedMaxBytes) {
            continue;
          }
          held += sizeof(FrameRead);
          NabtsFramedChannelUse& use = uses_[datacast.channel];
          use.channel = datacast.channel;
          ++use.bundles;
          const FrameRead& read = bundle->first;
          switch (bundle->second) {
            case Opening::kFrame:
              gathered[read.service()].push_back(read);
              break;
            case Opening::kHeaderLost:
              unread_.push_back({read, Unread::Why::kHeaderLost});
              break;
            case Opening::kHeaderDamaged:
              unread_.push_back({read, Unread::Why::kHeaderDamaged});
              break;
            case Opening::kNoHeader:
              unread_.push_back({read, Unread::Why::kNoHeader});
              break;
          }
        }
        continue;
      }
      for (size_t r = 0; r < datacast.rows.size(); ++r) {
        std::optional<FrameRead> frame = frame_at(datacast, r);
        if (!frame || held + sizeof(FrameRead) > kNabtsFramedMaxBytes) {
          continue;
        }
        r += kNabtsFrameRows - 1;
        held += sizeof(FrameRead);
        uses_[datacast.channel].channel = datacast.channel;
        gathered[frame->service()].push_back(*frame);
      }
    }
    return gathered;
  }

  /// The services listed, their frames on another block of addresses left
  /// out, and the frames of every name not listed kept as bundles not read.
  std::vector<ServiceFrames> list(
      std::map<uint32_t, std::vector<FrameRead>>& gathered) {
    std::vector<ServiceFrames> listed;
    for (auto& [identifier, frames] : gathered) {
      // The service's block of addresses, and any other holding a share of
      // its frames; a frame on another came on a misread address.
      std::map<uint16_t, size_t> per_block;
      for (const FrameRead& frame : frames) {
        ++per_block[frame.channel >> 4U];
      }
      size_t most = 0;
      for (const auto& [block, count] : per_block) {
        most = std::max(most, count);
      }
      std::vector<FrameRead> kept;
      for (FrameRead& frame : frames) {
        if (per_block[frame.channel >> 4U] * kNabtsFramedBlockShare < most) {
          unread_.push_back({frame, Unread::Why::kOtherBlock});
        } else {
          kept.push_back(frame);
        }
      }
      frames = {};
      std::stable_sort(kept.begin(), kept.end(),
                       [](const FrameRead& a, const FrameRead& b) {
                         return a.arrived < b.arrived;
                       });
      const bool many =
          kept.size() >= kNabtsFramedMinFrames &&
          longest_close(count_frames(kept)) >= kNabtsFramedMinFrames;
      if (!many && !frames_follow(kept)) {
        for (FrameRead& frame : kept) {
          unread_.push_back({frame, Unread::Why::kUnlisted});
        }
        continue;
      }
      ServiceFrames service;
      service.identifier = identifier;
      // A frame whose count steps far from both the frames that arrived
      // either side of it, while those two agree, had its header misread.
      const auto far = [](const FrameRead& a, const FrameRead& b) {
        const int64_t step = count_step(a.count(), b.count());
        return step < -kNabtsFramedReorderFrames ||
               step > kNabtsFramedReorderFrames;
      };
      for (size_t i = 0; i < kept.size(); ++i) {
        if (i > 0 && i + 1 < kept.size() && far(kept[i - 1], kept[i]) &&
            far(kept[i], kept[i + 1]) && !far(kept[i - 1], kept[i + 1])) {
          unread_.push_back({kept[i], Unread::Why::kWildCount});
          continue;
        }
        service.blocks.insert(static_cast<uint16_t>(kept[i].channel >> 4U));
        service.frames.push_back(kept[i]);
      }
      service.counted = count_frames(service.frames);
      listed.push_back(std::move(service));
    }
    return listed;
  }

  /// The counts missing from each service's runs, and the bundles not read
  /// that arrived where each belongs: a count and a bundle that can only be
  /// each other are paired, the frame put back under its count.
  void put_back_missing(std::vector<ServiceFrames>& listed) {
    struct Slot {
      size_t service = 0;
      int64_t count = 0;
      std::vector<size_t> candidates;
    };
    std::vector<Slot> slots;
    std::vector<size_t> slots_per_bundle(unread_.size(), 0);
    // The bundles not read, in the order they arrived.
    std::vector<size_t> by_arrival(unread_.size());
    for (size_t u = 0; u < unread_.size(); ++u) {
      by_arrival[u] = u;
    }
    std::stable_sort(by_arrival.begin(), by_arrival.end(),
                     [this](size_t a, size_t b) {
                       return unread_[a].read.arrived < unread_[b].read.arrived;
                     });
    for (size_t s = 0; s < listed.size(); ++s) {
      const ServiceFrames& service = listed[s];
      const auto& by_count = service.counted.by_count;
      std::optional<int64_t> previous;
      for (const auto& [count, copies] : by_count) {
        if (previous && count - *previous > 1 &&
            count - *previous <= kNabtsFramedPutBackGap) {
          for (int64_t missing = *previous + 1; missing < count; ++missing) {
            // When the bundle that could fill it arrived: between the frames
            // counted within reach of it, and the frames read nearest it on
            // either side — a few arrive out of turn.
            uint32_t first = UINT32_MAX;
            uint32_t last = 0;
            for (auto it = by_count.lower_bound(
                     std::min(missing - kNabtsFramedPutBackReach, *previous));
                 it != by_count.end() &&
                 it->first <=
                     std::max(missing + kNabtsFramedPutBackReach, count);
                 ++it) {
              for (const size_t c : it->second) {
                first = std::min(first, service.frames[c].arrived);
                last = std::max(last, service.frames[c].arrived);
              }
            }
            Slot slot{s, missing, {}};
            const auto from =
                std::lower_bound(by_arrival.begin(), by_arrival.end(), first,
                                 [this](size_t u, uint32_t arrived) {
                                   return unread_[u].read.arrived < arrived;
                                 });
            for (auto it = from;
                 it != by_arrival.end() && unread_[*it].read.arrived <= last;
                 ++it) {
              const size_t u = *it;
              const Unread& bundle = unread_[u];
              // A bundle none of whose rows arrived has nothing to give.
              const bool kind_fits =
                  bundle.read.rows != 0 &&
                  (bundle.why == Unread::Why::kHeaderLost ||
                   bundle.why == Unread::Why::kHeaderDamaged ||
                   bundle.why == Unread::Why::kUnlisted ||
                   (bundle.why == Unread::Why::kWildCount &&
                    bundle.read.service() == service.identifier));
              if (kind_fits && service.blocks.count(static_cast<uint16_t>(
                                   bundle.read.channel >> 4U)) > 0) {
                slot.candidates.push_back(u);
                ++slots_per_bundle[u];
              }
            }
            if (!slot.candidates.empty()) {
              slots.push_back(std::move(slot));
            }
          }
        }
        previous = count;
      }
    }
    for (const Slot& slot : slots) {
      if (slot.candidates.size() != 1 ||
          slots_per_bundle[slot.candidates.front()] != 1) {
        continue;
      }
      Unread& bundle = unread_[slot.candidates.front()];
      ServiceFrames& service = listed[slot.service];
      const auto& by_count = service.counted.by_count;
      const auto neighbour = [&](int64_t count) -> std::optional<FrameRead> {
        const auto it = by_count.find(count);
        if (it == by_count.end()) {
          return std::nullopt;
        }
        return merged(service.frames, it->second);
      };
      const std::optional<FrameRead> before = neighbour(slot.count - 1);
      const std::optional<FrameRead> after = neighbour(slot.count + 1);
      const auto header = header_between(before, after);
      if (!header) {
        continue;
      }
      // A frame whose own header arrived — of a misread name or count — has
      // to agree with what the frames around it say in its object or its
      // place.
      const bool own_header = bundle.why == Unread::Why::kUnlisted ||
                              bundle.why == Unread::Why::kWildCount;
      if (own_header && bundle.read.object() != header->first &&
          bundle.read.place() != header->second) {
        continue;
      }
      const uint16_t count = before ? static_cast<uint16_t>(before->count() + 1)
                                    : static_cast<uint16_t>(after->count() - 1);
      bundle.used = true;
      service.frames.push_back(put_back(bundle.read, service.identifier, count,
                                        header->first, header->second));
      service.counted.by_count[slot.count].push_back(service.frames.size() - 1);
    }
  }

  /// One reading of each count of |service|, in the order of the counts,
  /// with the addresses its copies came on; |framed|'s counts and channels
  /// set. A frame counted between two frames of one object whose places are
  /// two frames apart is that object's frame between them: where its own
  /// header says otherwise, its object and place were misread with its count
  /// right, and are put back.
  CountedFrames read_counts(ServiceFrames& service,
                            NabtsFramedService& framed) {
    std::vector<FrameRead>& frames = service.frames;
    const auto& by_count = service.counted.by_count;
    std::set<uint16_t> channels;
    for (const FrameRead& frame : frames) {
      channels.insert(frame.channel);
    }
    CountedFrames counted;
    counted.frames.reserve(by_count.size());
    framed.identifier = service.identifier;
    framed.channels.assign(channels.begin(), channels.end());
    framed.repeats = frames.size() - by_count.size();
    std::optional<int64_t> previous;
    for (const auto& [count, copies] : by_count) {
      if (previous && count - *previous < kRunGap) {
        framed.frames_lost += static_cast<uint64_t>(count - *previous - 1);
      }
      previous = count;
      counted.frames.push_back(merged(frames, copies));
      std::vector<uint16_t> on;
      for (const size_t c : copies) {
        on.push_back(frames[c].channel);
        NabtsFramedChannelUse& use = uses_[frames[c].channel];
        auto entry = std::find_if(
            use.frames.begin(), use.frames.end(),
            [&](const auto& pair) { return pair.first == service.identifier; });
        if (entry == use.frames.end()) {
          use.frames.emplace_back(service.identifier, 0);
          entry = use.frames.end() - 1;
        }
        ++entry->second;
        use.frames_put_back += frames[c].put_back ? 1 : 0;
        counted.put_back += frames[c].put_back ? 1 : 0;
      }
      counted.on.push_back(std::move(on));
    }
    frames = {};
    std::vector<int64_t> keys;
    keys.reserve(by_count.size());
    for (const auto& entry : by_count) {
      keys.push_back(entry.first);
    }
    for (size_t i = 1; i + 1 < counted.frames.size(); ++i) {
      if (keys[i - 1] + 1 != keys[i] || keys[i] + 1 != keys[i + 1]) {
        continue;
      }
      const FrameRead& before = counted.frames[i - 1];
      const FrameRead& after = counted.frames[i + 1];
      FrameRead& frame = counted.frames[i];
      if (before.object() != after.object() ||
          after.place() != before.place() + 2 * kNabtsFramePayloadBytes ||
          (frame.object() == before.object() &&
           frame.place() == before.place() + kNabtsFramePayloadBytes)) {
        continue;
      }
      frame = put_back(
          frame, service.identifier, frame.count(), before.object(),
          static_cast<uint32_t>(before.place() + kNabtsFramePayloadBytes));
      counted.put_back += 1;
      for (const uint16_t channel : counted.on[i]) {
        ++uses_[channel].frames_put_back;
      }
    }
    framed.frames = counted.frames.size();
    framed.first_frame = counted.frames.front().count();
    framed.last_frame = counted.frames.back().count();
    return counted;
  }

  /// A service's files: its frames read one to a count, each object read
  /// from them in its versions, the files a service sent in parts joined,
  /// and the service listed with what its frames gave.
  void read_service(ServiceFrames& service) {
    NabtsFramedService framed;
    const CountedFrames counted = read_counts(service, framed);

    uint64_t damaged = 0;
    // Each object's frames in the order of their counts; the objects in the
    // order their frames were first counted.
    std::map<uint16_t, std::vector<std::pair<uint32_t, size_t>>> objects;
    std::vector<uint16_t> object_order;
    for (size_t i = 0; i < counted.frames.size(); ++i) {
      const FrameRead& frame = counted.frames[i];
      framed.frames_in_part += frame.whole() ? 0 : 1;
      damaged += frame.damaged != 0 ? 1 : 0;
      auto [object, made] = objects.try_emplace(frame.object());
      if (made) {
        object_order.push_back(frame.object());
      }
      object->second.emplace_back(frame.place(), i);
      if (i < kNabtsFramedHeadersShown) {
        std::string line = fmt::format(
            "{:04X}  object {:04X} at {:<8} frame {} on {:03X} ", frame.count(),
            frame.object(), frame.place(), frame.arrived, frame.channel);
        for (size_t j = 0; j < kNabtsFrameHeaderBytes; ++j) {
          line += fmt::format(" {:02x}", frame.bytes[j]);
        }
        framed.headers += line + "\n";
      }
      if (frame.whole() &&
          streamed_ + kNabtsFrameBytes <= kNabtsFramedMaxBytes) {
        framed.stream.insert(framed.stream.end(), frame.bytes.begin(),
                             frame.bytes.end());
        streamed_ += kNabtsFrameBytes;
      }
    }
    // The length of the name each object's header gives, where its first
    // frame arrived with it: what the objects numbered either side of one
    // whose header was lost are asked.
    std::map<uint16_t, size_t> name_lengths;
    for (const auto& [number, sent] : objects) {
      for (const auto& [place, index] : sent) {
        if (place != 0) {
          continue;
        }
        if (const std::optional<ObjectHeader> header =
                object_header(counted.frames[index], number)) {
          name_lengths.try_emplace(number, header->name.size());
        }
      }
    }

    const size_t files_from = out_.files.size();
    for (const uint16_t number : object_order) {
      read_object(counted, number, objects.at(number), name_lengths, framed);
    }
    std::map<size_t, std::vector<size_t>> parts_of;
    join_parts(out_.files, files_from, parts_of);
    for (const auto& [joined, parts] : parts_of) {
      for (const size_t part : parts) {
        const std::set<uint16_t>& on = file_channels_[part];
        file_channels_[joined].insert(on.begin(), on.end());
      }
    }
    framed.summary = service_summary(framed, service.counted.runs, damaged,
                                     counted.put_back);
    out_.services.push_back(std::move(framed));
  }

  /// Object |number| of a service read from its frames |sent|, in each
  /// version it was sent in, as files.
  void read_object(const CountedFrames& counted, uint16_t number,
                   const std::vector<std::pair<uint32_t, size_t>>& sent,
                   const std::map<uint16_t, size_t>& name_lengths,
                   NabtsFramedService& framed) {
    std::vector<ObjectPlaces> versions = object_versions(counted.frames, sent);
    // Passes a misread split apart: a container whose versions, voted
    // together, unpack whole with its check holding was one file sent over
    // and over, each copy damaged somewhere else.
    if (versions.size() > 1) {
      ObjectPlaces together;
      for (const ObjectPlaces& version : versions) {
        for (const auto& [place, copies] : version) {
          auto& into = together[place];
          into.insert(into.end(), copies.begin(), copies.end());
        }
      }
      const std::optional<ObjectHeader> header =
          version_header(counted.frames, number, together);
      const std::optional<ObjectRead> merged_read =
          object_file(counted.frames, number, together, header, framed.channels,
                      max_file_bytes_ - held_files_);
      if (merged_read && nabts_framed_container(merged_read->file)) {
        const std::optional<NabtsCarriedFile> content =
            nabts_framed_container_content(merged_read->file);
        if (content && !content->cut_short &&
            content->checksum_holds.value_or(false)) {
          versions = {std::move(together)};
        }
      } else if (merged_read) {
        // So too an archive whose versions, voted together, hold more
        // members whose CRC-32 holds than any one of them does.
        const size_t voted = members_checked(merged_read->file.data);
        size_t best = 0;
        for (size_t v = 0; voted > 0 && v < versions.size(); ++v) {
          if (const std::optional<ObjectRead> one =
                  object_file(counted.frames, number, versions[v], header,
                              framed.channels, max_file_bytes_ - held_files_)) {
            best = std::max(best, members_checked(one->file.data));
          }
        }
        if (voted > best) {
          versions = {std::move(together)};
        }
      }
    }
    std::optional<ObjectHeader> known;
    for (const ObjectPlaces& version : versions) {
      known = version_header(counted.frames, number, version);
      if (known) {
        break;
      }
    }
    bool listed_object = false;
    for (const ObjectPlaces& version : versions) {
      std::optional<ObjectRead> object =
          object_file(counted.frames, number, version, known, framed.channels,
                      max_file_bytes_ - held_files_);
      if (!object) {
        continue;
      }
      listed_object = true;
      ++framed.versions;
      framed.versions_whole += object->whole ? 1 : 0;
      // A first frame that arrived without the length of its name: under the
      // length its neighbours give theirs, the file's opening is lost, and an
      // archive container may be opened again with it put back.
      bool reopened = false;
      if (!known && version.count(0) > 0) {
        if (const std::optional<size_t> length =
                sibling_name_length(name_lengths, number)) {
          const std::optional<ObjectRead> assumed =
              object_file(counted.frames, number, version, known,
                          framed.channels, max_file_bytes_ - held_files_,
                          kNabtsFrameObjectHeaderBytes + *length);
          if (assumed) {
            if (std::optional<NabtsCarriedFile> content =
                    nabts_framed_container_reopened(assumed->file)) {
              ++framed.containers;
              ++framed.containers_unpacked;
              ++framed.containers_reopened;
              object->file = std::move(*content);
              reopened = true;
            }
          }
        }
      }
      // A file in one of the service's containers, unpacked: what it holds
      // stands for it.
      if (!reopened && nabts_framed_container(object->file)) {
        ++framed.containers;
        if (std::optional<NabtsCarriedFile> content =
                nabts_framed_container_content(object->file)) {
          const bool whole =
              !content->cut_short && content->checksum_holds.value_or(false);
          ++(whole ? framed.containers_unpacked : framed.containers_in_part);
          object->file = std::move(*content);
        }
      }
      // The objects' bytes between them stay within the bound as they are
      // built: one too large is left out and the rest still read.
      if (object->file.data.size() > max_file_bytes_ - held_files_) {
        continue;
      }
      held_files_ += object->file.data.size();
      // The addresses its frames came on, and those frames, as part of it.
      std::set<uint16_t>& on = file_channels_[out_.files.size()];
      for (const auto& [place, copies] : version) {
        for (const size_t index : copies) {
          for (const uint16_t channel : counted.on[index]) {
            on.insert(channel);
            ++uses_[channel].frames_in_files;
          }
        }
      }
      out_.files.push_back(std::move(object->file));
    }
    framed.objects += listed_object ? 1 : 0;
  }

  /// The archives among the files: each member listed after them, a table
  /// read as its records, and a catalogue of services read as the catalogue.
  /// A member is listed where its CRC-32 holds, once for each content it had
  /// — copies of the archive the service sent again, damaged elsewhere or
  /// under a misread name, list nothing new.
  void list_archive_members() {
    std::optional<NabtsServiceCatalogue> catalogue;
    std::set<std::pair<std::string, std::vector<uint8_t>>> listed_members;
    const size_t archives_to = out_.files.size();
    for (size_t i = 0; i < archives_to; ++i) {
      std::vector<NabtsArchiveMember> members =
          nabts_archive_members(out_.files[i].data);
      const bool unpacked =
          std::any_of(members.begin(), members.end(),
                      [](const NabtsArchiveMember& m) { return m.unpacked; });
      if (!unpacked) {
        continue;
      }
      std::string contents = members_text(members);
      if (std::optional<NabtsServiceCatalogue> found =
              nabts_service_catalogue(members)) {
        contents = nabts_service_catalogue_text(*found) + "\n" + contents;
        if (!catalogue || found->updated > catalogue->updated) {
          catalogue = std::move(found);
        }
      }
      const NabtsCarriedFile archive = out_.files[i];
      out_.files[i].contents = std::move(contents);
      for (NabtsArchiveMember& member : members) {
        const std::string name = archive.name + "/" + member.name;
        if (!member.crc_holds || out_.files.size() >= kNabtsCarriedFilesMax ||
            member.data.size() > max_file_bytes_ - held_files_ ||
            !listed_members.emplace(member.name, member.data).second) {
          continue;
        }
        held_files_ += member.data.size();
        NabtsCarriedFile file;
        file.channel = archive.channel;
        file.last_channel = archive.last_channel;
        file.transfer = archive.transfer;
        file.copies = archive.copies;
        file.name = name;
        file.time = member.time;
        file.checksum = member.crc;
        file.checksum_holds = true;
        file.data = std::move(member.data);
        file.format = nabts_file_format(file.data, file.data_present);
        if (const std::optional<NabtsTable> table = nabts_table(file.data)) {
          file.contents = table_contents(*table);
        }
        file_channels_[out_.files.size()] = file_channels_[i];
        out_.files.push_back(std::move(file));
      }
    }
    if (!catalogue) {
      return;
    }
    for (NabtsFramedService& service : out_.services) {
      const uint32_t channel = service.channels.front() >> 4U;
      const std::vector<std::string> names =
          nabts_service_catalogue_names(*catalogue, channel);
      std::string list;
      for (const std::string& name : names) {
        list += (list.empty() ? "" : ", ") + name;
      }
      service.catalogue =
          names.empty()
              ? fmt::format(
                    "the catalogue of services lists no service on its data "
                    "channel, {}",
                    channel)
              : fmt::format(
                    "the catalogue of services lists its data channel, {}, "
                    "for {}",
                    channel, list);
    }
  }

  /// Each address's bundles not read, and the files its frames went into.
  void account_channels() {
    for (const Unread& bundle : unread_) {
      NabtsFramedChannelUse& use = uses_[bundle.read.channel];
      use.channel = bundle.read.channel;
      if (bundle.used) {
        continue;
      }
      switch (bundle.why) {
        case Unread::Why::kHeaderLost:
          ++use.header_lost;
          break;
        case Unread::Why::kHeaderDamaged:
          ++use.header_damaged;
          break;
        case Unread::Why::kNoHeader:
          ++use.no_header;
          break;
        case Unread::Why::kUnlisted:
        case Unread::Why::kWildCount:
          ++use.unlisted;
          break;
        case Unread::Why::kOtherBlock:
          ++use.other_block;
          break;
      }
    }
    for (const auto& [index, on] : file_channels_) {
      for (const uint16_t channel : on) {
        uses_[channel].files.push_back(index);
      }
    }
    for (auto& [channel, use] : uses_) {
      std::sort(use.files.begin(), use.files.end());
      out_.channels.push_back(std::move(use));
    }
  }

  const size_t max_file_bytes_;
  std::vector<Unread> unread_;
  std::map<uint16_t, NabtsFramedChannelUse> uses_;
  NabtsFramedDatacasts out_;
  /// Bytes of the frames streamed whole so far (kNabtsFramedMaxBytes) and of
  /// the files built so far (|max_file_bytes_|).
  size_t streamed_ = 0;
  size_t held_files_ = 0;
  /// The addresses each file's frames came on, by the file's index.
  std::map<size_t, std::set<uint16_t>> file_channels_;
};

}  // namespace

const std::array<uint8_t, kNabtsFrameBytes>& nabts_frame_pad() {
  return kFramePad;
}

bool nabts_frame_header(const uint8_t* frame) {
  return std::all_of(kHeaderZeroBytes.begin(), kHeaderZeroBytes.end(),
                     [frame](size_t i) { return frame[i] == 0; });
}

NabtsFramedDatacasts nabts_framed_services(
    const std::vector<NabtsDatacastChannel>& datacasts, size_t max_file_bytes) {
  return FramedServicesReader(max_file_bytes).read(datacasts);
}

std::string nabts_framed_channel_text(const NabtsFramedChannelUse& use,
                                      const NabtsFramedDatacasts& framed) {
  uint64_t frames = 0;
  for (const auto& [identifier, count] : use.frames) {
    frames += count;
  }
  std::string out;
  if (use.bundles > 0) {
    out = fmt::format(
        "Its payload is {} bundle(s) of sixteen rows, each a frame a framed "
        "service sent under the pad its frames go under, the service's frames "
        "taking turns on the sixteen addresses of its family; {} of them read "
        "as frames:\n",
        use.bundles, frames);
  } else {
    out = fmt::format(
        "Its payload holds {} frame(s) of framed services, each a frame a "
        "service sent under the pad its frames go under:\n",
        frames);
  }
  for (const auto& [identifier, count] : use.frames) {
    out += fmt::format("  {} frame(s) of service {:08x}", count, identifier);
    const auto service =
        std::find_if(framed.services.begin(), framed.services.end(),
                     [identifier = identifier](const NabtsFramedService& s) {
                       return s.identifier == identifier;
                     });
    if (service != framed.services.end() && !service->catalogue.empty()) {
      out += " (" + service->catalogue + ")";
    }
    out += "\n";
  }
  if (use.frames_put_back > 0) {
    out += fmt::format(
        "  {} of the frames put back from the frames sent around them: their "
        "header row lost, failing its code or misread\n",
        use.frames_put_back);
  }
  if (use.header_lost > 0) {
    out += fmt::format(
        "  {} bundle(s) whose header row was lost, and nothing to put it "
        "back: not read\n",
        use.header_lost);
  }
  if (use.header_damaged > 0) {
    out += fmt::format(
        "  {} bundle(s) whose header row failed its code, and nothing to put "
        "it back: not read\n",
        use.header_damaged);
  }
  if (use.no_header > 0) {
    out += fmt::format(
        "  {} bundle(s) whose header row arrived opening no frame: rows a "
        "lost packet moved, or no frame's\n",
        use.no_header);
  }
  if (use.unlisted > 0) {
    out += fmt::format(
        "  {} frame(s) of a name no service listed bears, or whose count no "
        "frame around it agrees with: a misread header\n",
        use.unlisted);
  }
  if (use.other_block > 0) {
    out += fmt::format(
        "  {} frame(s) of a service whose addresses are another block's: an "
        "address misread\n",
        use.other_block);
  }
  if (use.files.empty()) {
    out += "No file listed holds its frames' bytes.\n";
    return out;
  }
  out += fmt::format(
      "\n{} of its frames are part of these {} file(s), with the frames the "
      "service sent on its other addresses:\n",
      use.frames_in_files, use.files.size());
  for (const size_t index : use.files) {
    if (index >= framed.files.size()) {
      continue;
    }
    const NabtsCarriedFile& file = framed.files[index];
    out += "  " + (file.source.empty() ? file.name : file.source) + " — " +
           kind_of(file) + fmt::format(", {} bytes, ", file.data.size()) +
           condition_of(file) + "\n";
  }
  return out;
}

std::string nabts_framed_bundles_summary(const NabtsFramedDatacasts& framed) {
  NabtsFramedChannelUse total;
  uint64_t frames = 0;
  uint64_t addresses = 0;
  for (const NabtsFramedChannelUse& use : framed.channels) {
    if (use.bundles == 0) {
      continue;
    }
    ++addresses;
    total.bundles += use.bundles;
    for (const auto& [identifier, count] : use.frames) {
      frames += count;
    }
    total.frames_put_back += use.frames_put_back;
    total.frames_in_files += use.frames_in_files;
    total.header_lost += use.header_lost;
    total.header_damaged += use.header_damaged;
    total.no_header += use.no_header;
    total.unlisted += use.unlisted;
    total.other_block += use.other_block;
  }
  if (total.bundles == 0) {
    return {};
  }
  const uint64_t unread = total.header_lost + total.header_damaged +
                          total.no_header + total.unlisted + total.other_block;
  return fmt::format(
      "{} bundle(s) of a frame's rows on {} address(es): {} read as frames "
      "({} put back from the frames around them, {} part of a file listed), "
      "{} not read — {} whose header row was lost, {} whose header row failed "
      "its code, {} opening no frame, {} of a misread name or count, {} of a "
      "service whose addresses are another block's",
      total.bundles, addresses, frames, total.frames_put_back,
      total.frames_in_files, unread, total.header_lost, total.header_damaged,
      total.no_header, total.unlisted, total.other_block);
}

}  // namespace orc
