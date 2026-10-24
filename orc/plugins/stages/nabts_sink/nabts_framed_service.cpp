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

#include "nabts_data_group.h"
#include "nabts_datacast_files.h"
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

uint16_t le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0] | (p[1] << 8U));
}

uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8U) |
         (static_cast<uint32_t>(p[2]) << 16U) |
         (static_cast<uint32_t>(p[3]) << 24U);
}

uint16_t row_bit(size_t row) { return static_cast<uint16_t>(1U << row); }

/// One frame read: its bytes, the pad taken off, and where and when it
/// arrived. A row that did not arrive is held as zeros.
struct FrameRead {
  std::array<uint8_t, kNabtsFrameBytes> bytes{};
  /// Where its bytes end: before kNabtsFrameBytes where its service cut it
  /// short.
  size_t length = kNabtsFrameBytes;
  /// The rows that arrived, and those of them that failed their row code and
  /// were taken as received, bit i for row i. The header row always arrived,
  /// and passed.
  uint16_t rows = 0;
  uint16_t damaged = 0;
  uint16_t channel = 0;
  uint32_t arrived = 0;

  bool whole() const { return rows == kAllRows; }
  uint16_t count() const { return le16(&bytes[5]); }
  uint32_t service() const {
    return (static_cast<uint32_t>(bytes[9]) << 24U) |
           (static_cast<uint32_t>(bytes[10]) << 16U) |
           (static_cast<uint32_t>(bytes[11]) << 8U) | bytes[12];
  }
  uint16_t object() const { return le16(&bytes[13]); }
  uint32_t place() const { return le32(&bytes[15]); }
  /// Whether byte |i| arrived.
  bool holds(size_t i) const {
    return i < length && (rows & row_bit(i / kNabtsFrameRowBytes)) != 0;
  }
};

/// The frame that starts at row |r| of |datacast|, if its header row arrived
/// and passed its row code: the rows of it that arrived, those after the one
/// where its service cut it short holding only filler.
std::optional<FrameRead> frame_at(const NabtsDatacastChannel& datacast,
                                  size_t r) {
  if (r + kNabtsFrameRows > datacast.rows.size()) {
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
  return read;
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
/// none so, among those that took it as received. The header is the first
/// copy's.
FrameRead merged(const std::vector<FrameRead>& frames,
                 const std::vector<size_t>& copies) {
  const FrameRead& first = frames[copies.front()];
  if (copies.size() == 1) {
    return first;
  }
  FrameRead out;
  std::copy(first.bytes.begin(), first.bytes.begin() + kNabtsFrameHeaderBytes,
            out.bytes.begin());
  out.channel = first.channel;
  out.arrived = first.arrived;
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
  if (p[0] != 0 || p[1] != 0 || le16(p + 2) != number) {
    return std::nullopt;
  }
  const size_t name_length = le16(p + 12);
  if (name_length == 0 ||
      kNabtsFrameObjectHeaderBytes + name_length > kNabtsFramePayloadBytes ||
      !arrived(kNabtsFrameObjectHeaderBytes,
               kNabtsFrameObjectHeaderBytes + name_length)) {
    return std::nullopt;
  }
  ObjectHeader header;
  header.time = le32(p + 4);
  header.size = le32(p + 8);
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
/// another version of the object gave, where one did.
std::optional<ObjectRead> object_file(const std::vector<FrameRead>& frames,
                                      uint16_t number,
                                      const ObjectPlaces& places,
                                      const std::optional<ObjectHeader>& known,
                                      const std::vector<uint16_t>& channels,
                                      size_t budget) {
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
  // Checked before anything is allocated: a size taken from a misread place
  // can be tens of megabytes, and the data and its presence map both take it.
  if (size > budget) {
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
      if (!own) {
        continue;  // its bytes cannot be placed without its header's length
      }
      from += own->data_at;
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
/// missing between — joined into one each, after them.
void join_parts(std::vector<NabtsCarriedFile>& files, size_t from) {
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
    for (const auto& [number, index] : parts) {
      const std::vector<uint8_t>& data = files[index].data;
      if (joined.data.size() + data.size() > kNabtsCarriedBytesMax) {
        break;
      }
      joined.copies = std::min(joined.copies, files[index].copies);
      joined.data.insert(joined.data.end(), data.begin(), data.end());
    }
    joined.format = nabts_file_format(joined.data, joined.data_present);
    files.push_back(std::move(joined));
  }
}

}  // namespace

const std::array<uint8_t, kNabtsFrameBytes>& nabts_frame_pad() {
  return kFramePad;
}

bool nabts_frame_header(const uint8_t* frame) {
  return std::all_of(kHeaderZeroBytes.begin(), kHeaderZeroBytes.end(),
                     [frame](size_t i) { return frame[i] == 0; });
}

NabtsFramedDatacasts nabts_framed_services(
    const std::vector<NabtsDatacastChannel>& datacasts) {
  std::map<uint32_t, std::vector<FrameRead>> gathered;
  size_t held = 0;
  for (const NabtsDatacastChannel& datacast : datacasts) {
    if (datacast.raw) {
      continue;
    }
    // A datacast in bundles of a frame's rows holds a frame to a bundle, so a
    // frame is looked for only where one starts: a header row filed under
    // the wrong address opens none.
    const bool bundled = datacast.layout.data_rows == kNabtsFrameRows;
    for (size_t r = 0; r < datacast.rows.size();
         r += bundled ? kNabtsFrameRows : 1) {
      std::optional<FrameRead> frame = frame_at(datacast, r);
      if (!frame) {
        continue;
      }
      r += bundled ? 0 : kNabtsFrameRows - 1;
      if (held + sizeof(FrameRead) > kNabtsFramedMaxBytes) {
        continue;
      }
      held += sizeof(FrameRead);
      gathered[frame->service()].push_back(*frame);
    }
  }

  NabtsFramedDatacasts out;
  size_t streamed = 0;
  // Bytes of every object file kept so far, across all services: each object
  // is sized against what is left, so the total never passes
  // kNabtsCarriedBytesMax however its sizes were misread.
  size_t held_bytes = 0;
  for (auto& [identifier, frames] : gathered) {
    // The service's block of addresses, and any other holding a share of its
    // frames; a frame on another came on a misread address.
    std::map<uint16_t, size_t> per_block;
    for (const FrameRead& frame : frames) {
      ++per_block[frame.channel >> 4U];
    }
    size_t most = 0;
    for (const auto& [block, count] : per_block) {
      most = std::max(most, count);
    }
    frames.erase(std::remove_if(frames.begin(), frames.end(),
                                [&](const FrameRead& frame) {
                                  return per_block[frame.channel >> 4U] *
                                             kNabtsFramedBlockShare <
                                         most;
                                }),
                 frames.end());
    if (frames.size() < kNabtsFramedMinFrames) {
      continue;
    }
    std::set<uint16_t> channels;
    for (const FrameRead& frame : frames) {
      channels.insert(frame.channel);
    }
    // The counts in the order the frames arrived, followed across a wrap; one
    // that falls back further than the family's frames arrive out of turn
    // starts a run of its own.
    std::stable_sort(frames.begin(), frames.end(),
                     [](const FrameRead& a, const FrameRead& b) {
                       return a.arrived < b.arrived;
                     });
    std::map<int64_t, std::vector<size_t>> by_count;
    int64_t at = frames.front().count();
    int64_t top = at;
    uint64_t runs = 1;
    by_count[at].push_back(0);
    for (size_t i = 1; i < frames.size(); ++i) {
      const auto step = static_cast<int16_t>(
          static_cast<uint16_t>(frames[i].count() - frames[i - 1].count()));
      if (step < -kNabtsFramedReorderFrames) {
        at = top + kRunGap + frames[i].count();
        ++runs;
      } else {
        at += step;
      }
      top = std::max(top, at);
      by_count[at].push_back(i);
    }
    // The most frames counted close together.
    uint64_t stretch = 0;
    uint64_t longest = 0;
    std::optional<int64_t> before;
    for (const auto& entry : by_count) {
      stretch = before && entry.first - *before <= kNabtsFramedCloseCounts
                    ? stretch + 1
                    : 1;
      longest = std::max(longest, stretch);
      before = entry.first;
    }
    if (longest < kNabtsFramedMinFrames) {
      continue;
    }

    // One reading of each count, in the order of the counts.
    std::vector<FrameRead> counted;
    counted.reserve(by_count.size());
    NabtsFramedService framed;
    framed.identifier = identifier;
    framed.channels.assign(channels.begin(), channels.end());
    framed.repeats = frames.size() - by_count.size();
    std::optional<int64_t> previous;
    for (const auto& [count, copies] : by_count) {
      if (previous && count - *previous < kRunGap) {
        framed.frames_lost += static_cast<uint64_t>(count - *previous - 1);
      }
      previous = count;
      counted.push_back(merged(frames, copies));
    }
    frames = {};
    framed.frames = counted.size();
    framed.first_frame = counted.front().count();
    framed.last_frame = counted.back().count();

    uint64_t damaged = 0;
    // Each object's frames in the order of their counts; the objects in the
    // order their frames were first counted.
    std::map<uint16_t, std::vector<std::pair<uint32_t, size_t>>> objects;
    std::vector<uint16_t> object_order;
    for (size_t i = 0; i < counted.size(); ++i) {
      const FrameRead& frame = counted[i];
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
          streamed + kNabtsFrameBytes <= kNabtsFramedMaxBytes) {
        framed.stream.insert(framed.stream.end(), frame.bytes.begin(),
                             frame.bytes.end());
        streamed += kNabtsFrameBytes;
      }
    }
    const size_t files_from = out.files.size();
    for (const uint16_t number : object_order) {
      std::vector<ObjectPlaces> versions =
          object_versions(counted, objects.at(number));
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
            version_header(counted, number, together);
        const std::optional<ObjectRead> merged =
            object_file(counted, number, together, header, framed.channels,
                        kNabtsCarriedBytesMax - held_bytes);
        if (merged && nabts_framed_container(merged->file)) {
          const std::optional<NabtsCarriedFile> content =
              nabts_framed_container_content(merged->file);
          if (content && !content->cut_short &&
              content->checksum_holds.value_or(false)) {
            versions = {std::move(together)};
          }
        }
      }
      std::optional<ObjectHeader> known;
      for (const ObjectPlaces& version : versions) {
        known = version_header(counted, number, version);
        if (known) {
          break;
        }
      }
      bool listed = false;
      for (const ObjectPlaces& version : versions) {
        std::optional<ObjectRead> object =
            object_file(counted, number, version, known, framed.channels,
                        kNabtsCarriedBytesMax - held_bytes);
        if (!object) {
          continue;
        }
        listed = true;
        ++framed.versions;
        framed.versions_whole += object->whole ? 1 : 0;
        // A file in one of the service's containers, unpacked: what it holds
        // stands for it.
        if (nabts_framed_container(object->file)) {
          ++framed.containers;
          if (std::optional<NabtsCarriedFile> content =
                  nabts_framed_container_content(object->file)) {
            const bool whole =
                !content->cut_short && content->checksum_holds.value_or(false);
            ++(whole ? framed.containers_unpacked : framed.containers_in_part);
            object->file = std::move(*content);
          }
        }
        if (object->file.data.size() > kNabtsCarriedBytesMax - held_bytes) {
          continue;  // its container unpacked to more than is left
        }
        held_bytes += object->file.data.size();
        out.files.push_back(std::move(object->file));
      }
      framed.objects += listed ? 1 : 0;
    }
    join_parts(out.files, files_from);
    framed.summary = fmt::format(
        "frames on {}: {} read, {} of them only in part, counted {:04X} to "
        "{:04X}{}, {} sent and not read, {} read again",
        channel_label(framed.channels), framed.frames, framed.frames_in_part,
        framed.first_frame, framed.last_frame,
        runs > 1 ? fmt::format(" in {} runs", runs) : std::string(),
        framed.frames_lost, framed.repeats);
    if (damaged > 0) {
      framed.summary +=
          fmt::format(", {} with a row taken as received", damaged);
    }
    framed.summary += fmt::format(
        "; read under its pad, {} object(s) in {} version(s), {} of them "
        "whole",
        framed.objects, framed.versions, framed.versions_whole);
    if (framed.containers > 0) {
      framed.summary += fmt::format(
          "; {} in the service's containers, {} unpacked whole and checked, "
          "{} only in part",
          framed.containers, framed.containers_unpacked,
          framed.containers_in_part);
    }
    out.services.push_back(std::move(framed));
  }
  return out;
}

}  // namespace orc
