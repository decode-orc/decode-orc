/*
 * File:        nabts_datacast_files.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     The files datacast services sent, taken out of their payloads
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_datacast_files.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <map>
#include <optional>

#include "nabts_data_group.h"
#include "nabts_service_census.h"

namespace orc {

namespace {

// The byte table a file carousel hides its text files behind: a byte p of a
// file is sent as the byte whose entry here is (p + its key) modulo 256, so
// reading a byte back is its entry less the key. -1 for a byte no file of the
// recordings sent, whose reading is unknown. The key is the sum of the name's
// bytes and an offset the carousel keeps for all its files, which the
// directory's sums of the files' bytes give (carousel_key_offset()).
// Recovered from the recordings: the files' fixed headers gave some entries
// outright, quadgram statistics of English and of the files' own markup gave
// more, and files of identical content sent under many keys, each checked by
// its sum, gave the rest.
constexpr std::array<int16_t, 256> kCarouselByteIndex = {
    -1,  64,  -1,  131, 201, -1,  130, 217, 24,  56,  255, 249, -1,  163, 127,
    137, 12,  8,   155, 37,  34,  196, 253, 93,  189, 18,  221, 54,  91,  233,
    98,  213, 57,  232, 157, 44,  179, 237, -1,  -1,  -1,  236, 88,  97,  35,
    -1,  42,  215, 126, 128, 47,  -1,  211, 147, -1,  158, 149, 13,  121, 198,
    -1,  186, 254, 30,  143, 60,  -1,  140, 168, 207, 208, 226, 235, 135, -1,
    -1,  -1,  49,  141, 240, 230, 161, 132, 58,  191, 229, 144, 190, 2,   50,
    100, 39,  19,  173, 15,  187, 197, 124, -1,  250, 133, 96,  227, 122, 156,
    46,  123, 22,  234, 154, 61,  160, 6,   239, 134, 246, 51,  -1,  10,  148,
    164, 4,   125, 151, 224, -1,  176, 31,  223, 245, 188, -1,  21,  45,  205,
    33,  -1,  218, 238, 181, 244, 169, 0,   14,  -1,  202, -1,  247, 200, 241,
    16,  63,  -1,  5,   195, 145, 139, 231, 225, 120, -1,  1,   220, 192, -1,
    23,  -1,  203, 28,  29,  -1,  162, 62,  -1,  9,   41,  43,  38,  184, 142,
    20,  193, 159, 167, 182, 214, 171, 183, -1,  -1,  212, 242, 219, -1,  11,
    194, 136, 228, -1,  129, 206, 222, 166, 185, 209, 251, 146, 36,  115, -1,
    17,  119, 174, 48,  -1,  52,  55,  -1,  27,  -1,  32,  175, -1,  25,  40,
    177, 26,  153, -1,  248, 95,  86,  172, 152, 138, 7,   204, 199, 3,   118,
    180, 165, -1,  -1,  -1,  -1,  210, 170, 252, 243, 53,  -1,  178, -1,  59,
    -1};

// The Unix times a file transfer's file numbers are believed in: a number
// outside them is a misread one.
constexpr int64_t kTransferEarliestTime = 631152000;  // 1990-01-01
constexpr int64_t kTransferLatestTime = 1893456000;   // 2030-01-01

bool is_text_byte(uint8_t byte) {
  return byte == 0x09 || byte == 0x0A || byte == 0x0D ||
         (byte >= 0x20 && byte < 0x7F);
}

/// The share of |data|'s bytes that arrived which are text, in percent.
uint64_t text_percent(const std::vector<uint8_t>& data,
                      const std::vector<uint8_t>& present) {
  uint64_t arrived = 0;
  uint64_t text = 0;
  for (size_t i = 0; i < data.size(); ++i) {
    if (!present.empty() && present[i] == kNabtsByteLost) {
      continue;
    }
    ++arrived;
    text += is_text_byte(data[i]) ? 1 : 0;
  }
  return arrived == 0 ? 0 : text * 100 / arrived;
}

/// What |data| is, from its own opening.
std::string file_format(const std::vector<uint8_t>& data,
                        const std::vector<uint8_t>& present) {
  const auto starts = [&data](const char* magic, size_t length) {
    return data.size() >= length &&
           std::equal(data.begin(),
                      data.begin() + static_cast<ptrdiff_t>(length),
                      reinterpret_cast<const uint8_t*>(magic));
  };
  if (starts("GIF87a", 6) || starts("GIF89a", 6)) {
    return "gif";
  }
  if (starts("\x89PNG\r\n\x1a\n", 8)) {
    return "png";
  }
  if (starts("\xff\xd8\xff", 3)) {
    return "jpeg";
  }
  if (text_percent(data, present) < kNabtsCarouselTextPercent) {
    return "binary";
  }
  std::string head;
  for (size_t i = 0; i < data.size() && i < 512; ++i) {
    head.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(data[i]))));
  }
  if (head.find("<html") != std::string::npos ||
      head.find("<!--") != std::string::npos ||
      head.find("<head") != std::string::npos) {
    return "html";
  }
  return "text";
}

/// Total bytes of |files|' data.
size_t bytes_held(const std::vector<NabtsCarriedFile>& files) {
  size_t total = 0;
  for (const NabtsCarriedFile& file : files) {
    total += file.data.size();
  }
  return total;
}

// ---------------------------------------------------------------------------
// File transfer
// ---------------------------------------------------------------------------

struct TransferFile {
  std::map<uint16_t, std::array<uint8_t, kNabtsTransferChunkData>> chunks;
  uint32_t first_chunk_copies = 0;
};

std::vector<NabtsCarriedFile> transfer_files(
    const NabtsDatacastChannel& datacast) {
  const std::vector<uint8_t>& payload = datacast.payload;
  // Where rows went missing: a message across one of these is two messages'
  // pieces, not one.
  std::vector<uint32_t> gaps;
  for (const NabtsDatacastRow& row : datacast.rows) {
    if (row.missing && (gaps.empty() || gaps.back() != row.offset)) {
      gaps.push_back(row.offset);
    }
  }
  const auto crosses_gap = [&gaps](size_t from, size_t to) {
    const auto it =
        std::upper_bound(gaps.begin(), gaps.end(), static_cast<uint32_t>(from));
    return it != gaps.end() && *it < to;
  };

  std::map<uint32_t, TransferFile> files;
  size_t chunks = 0;
  size_t i = 0;
  while (i + 3 <= payload.size()) {
    if (payload[i] != kNabtsTransferMark) {
      ++i;
      continue;
    }
    const size_t length = payload[i + 1];
    const uint8_t kind = payload[i + 2];
    const size_t end = i + 3 + length;
    if (end > payload.size()) {
      break;
    }
    if (crosses_gap(i, end)) {
      ++i;
      continue;
    }
    if (kind == kNabtsTransferChunkKind &&
        length == kNabtsTransferChunkLength) {
      const uint8_t* body = &payload[i + 3];
      const uint32_t number = static_cast<uint32_t>(body[0]) |
                              (static_cast<uint32_t>(body[1]) << 8U) |
                              (static_cast<uint32_t>(body[2]) << 16U) |
                              (static_cast<uint32_t>(body[3]) << 24U);
      const auto chunk = static_cast<uint16_t>(
          body[4] | (static_cast<unsigned>(body[5]) << 8U));
      const auto time = static_cast<int64_t>(number);
      if (time >= kTransferEarliestTime && time < kTransferLatestTime &&
          (files.count(number) > 0 || files.size() < kNabtsCarriedFilesMax)) {
        TransferFile& file = files[number];
        if (file.chunks.count(chunk) == 0) {
          std::array<uint8_t, kNabtsTransferChunkData> data{};
          std::copy(body + 6, body + 6 + kNabtsTransferChunkData, data.begin());
          file.chunks.emplace(chunk, data);
        }
        if (chunk == 0) {
          ++file.first_chunk_copies;
        }
        ++chunks;
      }
    }
    i = end;
  }
  if (chunks < kNabtsTransferMinChunks) {
    return {};
  }

  std::vector<NabtsCarriedFile> out;
  for (const auto& [number, file] : files) {
    if (file.chunks.count(0) == 0) {
      continue;  // no start: nothing to say what the file is
    }
    // A file runs to the last chunk at which at least half of those before
    // it arrived: a chunk number read wrong otherwise stretches it to that
    // number with nothing but holes.
    uint16_t last = 0;
    size_t seen = 0;
    for (const auto& entry : file.chunks) {
      ++seen;
      if (seen * 2 >= static_cast<size_t>(entry.first) + 1) {
        last = entry.first;
      }
    }
    NabtsCarriedFile carried;
    carried.channel = datacast.channel;
    carried.transfer = "file transfer";
    carried.name = fmt::format("{:08x}", number);
    carried.time = nabts_utc_text(static_cast<int64_t>(number)) + " UTC";
    carried.copies = file.first_chunk_copies;
    const size_t size =
        (static_cast<size_t>(last) + 1) * kNabtsTransferChunkData;
    // A file that would overflow what is held is passed over rather than
    // ending the listing: a misread chunk number makes one file large, and
    // those after it are no less real for that.
    if (bytes_held(out) + size > kNabtsCarriedBytesMax) {
      continue;
    }
    carried.data.assign(size, 0);
    bool whole = true;
    std::vector<uint8_t> present(size, kNabtsByteArrived);
    for (uint16_t c = 0; c <= last; ++c) {
      const auto it = file.chunks.find(c);
      const size_t at = static_cast<size_t>(c) * kNabtsTransferChunkData;
      if (it == file.chunks.end()) {
        whole = false;
        std::fill(present.begin() + static_cast<ptrdiff_t>(at),
                  present.begin() +
                      static_cast<ptrdiff_t>(at + kNabtsTransferChunkData),
                  kNabtsByteLost);
        continue;
      }
      std::copy(it->second.begin(), it->second.end(),
                carried.data.begin() + static_cast<ptrdiff_t>(at));
    }
    if (!whole) {
      carried.data_present = std::move(present);
    }
    carried.format = file_format(carried.data, carried.data_present);
    out.push_back(std::move(carried));
  }
  return out;
}

// ---------------------------------------------------------------------------
// File carousel
// ---------------------------------------------------------------------------

/// The carousel's stream: each row's 25 bytes, a row that went missing or
/// whose sum fails held open as lost.
struct CarouselStream {
  std::vector<uint8_t> bytes;
  std::vector<bool> lost;
  uint64_t rows_whole = 0;
  uint64_t rows_summed = 0;

  bool arrived(size_t from, size_t count) const {
    if (from + count > bytes.size()) {
      return false;
    }
    for (size_t i = from; i < from + count; ++i) {
      if (lost[i]) {
        return false;
      }
    }
    return true;
  }
};

CarouselStream carousel_stream(const NabtsDatacastChannel& datacast) {
  CarouselStream stream;
  stream.bytes.reserve(datacast.rows.size() * kNabtsCarouselRowData);
  stream.lost.reserve(datacast.rows.size() * kNabtsCarouselRowData);
  for (const NabtsDatacastRow& row : datacast.rows) {
    bool good = false;
    if (!row.missing && row.length == kNabtsCarouselRowBytes &&
        row.offset + kNabtsCarouselRowBytes <= datacast.payload.size()) {
      ++stream.rows_whole;
      const uint8_t* bytes = &datacast.payload[row.offset];
      unsigned sum = 0;
      for (size_t i = 0; i < kNabtsCarouselRowData; ++i) {
        sum += bytes[i];
      }
      good = (sum & 0xFFU) == bytes[kNabtsCarouselRowData];
      stream.rows_summed += good ? 1 : 0;
      stream.bytes.insert(
          stream.bytes.end(), bytes,
          bytes + static_cast<ptrdiff_t>(kNabtsCarouselRowData));
    } else {
      stream.bytes.insert(stream.bytes.end(), kNabtsCarouselRowData, 0);
    }
    stream.lost.insert(stream.lost.end(), kNabtsCarouselRowData, !good);
  }
  return stream;
}

bool is_digit(uint8_t byte) { return byte >= '0' && byte <= '9'; }

bool is_name_byte(uint8_t byte) {
  return is_digit(byte) || (byte >= 'A' && byte <= 'Z') || byte == '_';
}

/// Whether the 18 bytes at |at| are a time as the directory writes it,
/// YYYY-MM-DDhh:mm:ss, of a plausible date.
bool directory_time(const CarouselStream& stream, size_t at) {
  // The cheap test first: every byte of the stream is tried.
  if (at + 18 > stream.bytes.size() || stream.bytes[at + 4] != '-' ||
      stream.bytes[at + 12] != ':' || !is_digit(stream.bytes[at]) ||
      !stream.arrived(at, 18)) {
    return false;
  }
  static const char kShape[] = "dddd-dd-dddd:dd:dd";
  for (size_t i = 0; i < 18; ++i) {
    const uint8_t byte = stream.bytes[at + i];
    if (kShape[i] == 'd' ? !is_digit(byte)
                         : byte != static_cast<uint8_t>(kShape[i])) {
      return false;
    }
  }
  const auto number = [&](size_t from, size_t count) {
    int value = 0;
    for (size_t i = from; i < from + count; ++i) {
      value = value * 10 + (stream.bytes[at + i] - '0');
    }
    return value;
  };
  const int year = number(0, 4);
  const int month = number(5, 2);
  const int day = number(8, 2);
  return year >= 1980 && year < 2040 && month >= 1 && month <= 12 && day >= 1 &&
         day <= 31 && number(10, 2) < 24 && number(13, 2) < 60 &&
         number(16, 2) < 60;
}

/// The 8.3 name whose last byte is at |end| - 1, or nothing.
std::optional<std::string> directory_name(const CarouselStream& stream,
                                          size_t end) {
  // Three bytes of extension, a dot, then one to eight of name.
  if (end < 5 || !stream.arrived(end - 4, 4) || stream.bytes[end - 4] != '.') {
    return std::nullopt;
  }
  for (size_t i = end - 3; i < end; ++i) {
    if (!is_name_byte(stream.bytes[i]) || stream.bytes[i] == '_') {
      return std::nullopt;
    }
  }
  size_t start = end - 4;
  while (start > 0 && end - 4 - start < 8 && !stream.lost[start - 1] &&
         is_name_byte(stream.bytes[start - 1])) {
    --start;
  }
  if (start == end - 4) {
    return std::nullopt;
  }
  return std::string(stream.bytes.begin() + static_cast<ptrdiff_t>(start),
                     stream.bytes.begin() + static_cast<ptrdiff_t>(end));
}

struct BlockHeader {
  uint16_t number = 0;
  uint16_t length = 0;
};

/// The block header at |at|, or nothing.
std::optional<BlockHeader> block_header(const CarouselStream& stream,
                                        size_t at) {
  if (!stream.arrived(at, kNabtsCarouselBlockHeader) ||
      stream.bytes[at] != 0x01 || stream.bytes[at + 1] != 0x01) {
    return std::nullopt;
  }
  BlockHeader header;
  header.number = static_cast<uint16_t>(
      stream.bytes[at + 2] |
      (static_cast<unsigned>(stream.bytes[at + 3]) << 8U));
  header.length = static_cast<uint16_t>(
      stream.bytes[at + 4] |
      (static_cast<unsigned>(stream.bytes[at + 5]) << 8U));
  if (header.number == 0 || header.length == 0 ||
      header.length > kNabtsCarouselBlockData) {
    return std::nullopt;
  }
  return header;
}

/// One file of the carousel as its copies came round.
struct CarouselFile {
  std::string name;
  std::string time;
  uint32_t length = 0;
  /// The sum of the file's bytes, as the directory entry gives it.
  std::optional<uint32_t> checksum;
  uint32_t copies = 0;
  /// Per block: the copies of its bytes, each with the positions that were
  /// lost.
  std::vector<std::vector<std::pair<std::vector<uint8_t>, std::vector<bool>>>>
      blocks;
};

/// Reads the file whose directory entry ends at |entry_end| into |file|'s
/// blocks: block 1 within kNabtsCarouselFirstBlockReach of the entry, each
/// later one a bundle on.
void read_copy(const CarouselStream& stream, size_t entry_end,
               CarouselFile& file) {
  const size_t blocks =
      (file.length + kNabtsCarouselBlockData - 1) / kNabtsCarouselBlockData;
  // How long block |b| is: whole but for the last.
  const auto block_length = [&](size_t b) {
    return b < blocks ? kNabtsCarouselBlockData
                      : file.length - (blocks - 1) * kNabtsCarouselBlockData;
  };
  // A block starts a bundle, so its header starts a row, and it gives the
  // length its place in the file says: the bytes of a directory record that
  // happen to read as a header do neither.
  const auto is_block = [&](size_t at, size_t b) {
    if (at % kNabtsCarouselRowData != 0) {
      return false;
    }
    const std::optional<BlockHeader> header = block_header(stream, at);
    return header && header->number == b && header->length == block_length(b);
  };
  std::optional<size_t> first;
  for (size_t at = (entry_end + kNabtsCarouselRowData - 1) /
                   kNabtsCarouselRowData * kNabtsCarouselRowData;
       at < entry_end + kNabtsCarouselFirstBlockReach &&
       at + kNabtsCarouselBlockHeader <= stream.bytes.size();
       at += kNabtsCarouselRowData) {
    if (is_block(at, 1)) {
      first = at;
      break;
    }
  }
  if (!first) {
    return;
  }
  ++file.copies;
  const size_t stride = kNabtsCarouselBlockRows * kNabtsCarouselRowData;
  size_t expected = *first;
  for (size_t b = 1; b <= blocks; ++b, expected += stride) {
    // The header where the bundles say it is, or near it where a row went
    // missing uncounted.
    std::optional<size_t> found;
    for (size_t slack = 0; slack <= kNabtsCarouselBlockSlack && !found;
         slack += kNabtsCarouselRowData) {
      for (const size_t at :
           {expected - std::min(expected, slack), expected + slack}) {
        if (is_block(at, b)) {
          found = at;
          break;
        }
      }
    }
    if (!found) {
      continue;  // this copy lost the block's header; its next may be there
    }
    expected = *found;
    const size_t want = block_length(b);
    const size_t from = *found + kNabtsCarouselBlockHeader;
    if (from + want > stream.bytes.size()) {
      break;
    }
    auto& copies = file.blocks[b - 1];
    if (copies.size() >= kNabtsCarouselMaxCopies) {
      continue;
    }
    copies.emplace_back(
        std::vector<uint8_t>(
            stream.bytes.begin() + static_cast<ptrdiff_t>(from),
            stream.bytes.begin() + static_cast<ptrdiff_t>(from + want)),
        std::vector<bool>(
            stream.lost.begin() + static_cast<ptrdiff_t>(from),
            stream.lost.begin() + static_cast<ptrdiff_t>(from + want)));
  }
}

/// |file|'s bytes, each the commonest reading its copies gave of it, with the
/// positions no copy delivered marked lost.
NabtsCarriedFile assemble(const CarouselFile& file, uint16_t channel) {
  NabtsCarriedFile carried;
  carried.channel = channel;
  carried.transfer = "file carousel";
  carried.name = file.name;
  carried.time = file.time;
  carried.copies = file.copies;
  carried.checksum = file.checksum;
  carried.data.assign(file.length, 0);
  std::vector<uint8_t> present(file.length, kNabtsByteLost);
  bool whole = true;
  for (size_t b = 0; b < file.blocks.size(); ++b) {
    const size_t base = b * kNabtsCarouselBlockData;
    // A block no copy of arrived is lost whole, and voting it byte by byte
    // would only clear a tally per byte of a length that may be misread.
    if (file.blocks[b].empty()) {
      whole = false;
      continue;
    }
    const size_t span = std::min(kNabtsCarouselBlockData,
                                 static_cast<size_t>(file.length) - base);
    for (size_t i = 0; i < span; ++i) {
      std::array<uint32_t, 256> votes{};
      uint32_t best = 0;
      uint8_t value = 0;
      for (const auto& copy : file.blocks[b]) {
        if (i >= copy.first.size() || copy.second[i]) {
          continue;
        }
        const uint8_t byte = copy.first[i];
        if (++votes[byte] > best) {
          best = votes[byte];
          value = byte;
        }
      }
      if (best == 0) {
        whole = false;
        continue;
      }
      carried.data[base + i] = value;
      present[base + i] = kNabtsByteArrived;
    }
  }
  if (!whole) {
    carried.data_present = std::move(present);
  }
  return carried;
}

/// |file| read through the carousel's byte table, keyed by its name, where
/// that reads as text; unchanged otherwise.
/// A carousel file's key under |offset|: the sum of its name's bytes and the
/// offset, modulo 256.
unsigned carousel_key(const std::string& name, unsigned offset) {
  unsigned key = offset;
  for (const char c : name) {
    key += static_cast<unsigned char>(c);
  }
  return key & 0xFFU;
}

/// The offset a carousel's keys take from its files' names: the one under
/// which its whole files read back to the sums its directory gives them,
/// most often. Nothing where no whole file can tell.
std::optional<unsigned> carousel_key_offset(
    const std::vector<NabtsCarriedFile>& files) {
  std::array<uint32_t, 256> votes{};
  uint32_t asked = 0;
  for (const NabtsCarriedFile& file : files) {
    if (!file.checksum.has_value() || !file.data_present.empty() ||
        file.data.size() < kNabtsCarouselKeyMinBytes ||
        asked >= kNabtsCarouselKeyFiles) {
      continue;
    }
    const uint32_t wanted = *file.checksum;
    if (std::any_of(file.data.begin(), file.data.end(),
                    [](uint8_t b) { return kCarouselByteIndex[b] < 0; })) {
      continue;  // a byte the table cannot read: no sum can be made
    }
    ++asked;
    for (unsigned offset = 0; offset < 256; ++offset) {
      const unsigned key = carousel_key(file.name, offset);
      uint64_t sum = 0;
      for (const uint8_t b : file.data) {
        sum +=
            (static_cast<unsigned>(kCarouselByteIndex[b]) + 256U - key) & 0xFFU;
      }
      if (sum == wanted) {
        ++votes[offset];
        break;
      }
    }
  }
  const auto best = std::max_element(votes.begin(), votes.end());
  if (*best == 0) {
    return std::nullopt;
  }
  return static_cast<unsigned>(best - votes.begin());
}

void decipher(NabtsCarriedFile& file, unsigned offset) {
  const unsigned key = carousel_key(file.name, offset);
  std::vector<uint8_t> read(file.data.size(), 0);
  std::vector<uint8_t> present = file.data_present;
  if (present.empty()) {
    present.assign(file.data.size(), kNabtsByteArrived);
  }
  bool unknown = false;
  uint64_t arrived = 0;
  uint64_t text = 0;
  for (size_t i = 0; i < file.data.size(); ++i) {
    if (present[i] == kNabtsByteLost) {
      continue;
    }
    ++arrived;
    const int16_t index = kCarouselByteIndex[file.data[i]];
    if (index < 0) {
      // A byte the table has no reading of arrived, but cannot be read; it
      // counts against the file being text, since a file the table was
      // made from reads in full.
      present[i] = kNabtsByteLost;
      unknown = true;
      continue;
    }
    read[i] = static_cast<uint8_t>((static_cast<unsigned>(index) + 256U - key) &
                                   0xFFU);
    text += is_text_byte(read[i]) ? 1 : 0;
  }
  if (arrived == 0 || text * 100 < arrived * kNabtsCarouselTextPercent) {
    return;
  }
  file.data = std::move(read);
  const bool whole =
      std::none_of(present.begin(), present.end(),
                   [](uint8_t p) { return p == kNabtsByteLost; });
  file.data_present =
      whole && !unknown ? std::vector<uint8_t>{} : std::move(present);
  file.deciphered = true;
}

std::vector<NabtsCarriedFile> carousel_files(
    const NabtsDatacastChannel& datacast) {
  const CarouselStream stream = carousel_stream(datacast);
  if (stream.rows_whole < kNabtsCarouselMinRows ||
      stream.rows_summed * 100 <
          stream.rows_whole * kNabtsCarouselMinSumPercent) {
    return {};
  }
  std::map<std::string, CarouselFile> files;
  const auto& bytes = stream.bytes;
  for (size_t at = 5; at + 22 <= bytes.size(); ++at) {
    if (!directory_time(stream, at)) {
      continue;
    }
    // The name ends at the time, or before a run of padding.
    size_t end = at;
    while (end > 0 && at - end < 40 && !stream.lost[end - 1] &&
           (bytes[end - 1] == 0x00 || bytes[end - 1] == 0x20)) {
      --end;
    }
    const std::optional<std::string> name = directory_name(stream, end);
    if (!name || !stream.arrived(at + 18, 4)) {
      continue;
    }
    const uint32_t length = static_cast<uint32_t>(bytes[at + 18]) |
                            (static_cast<uint32_t>(bytes[at + 19]) << 8U) |
                            (static_cast<uint32_t>(bytes[at + 20]) << 16U) |
                            (static_cast<uint32_t>(bytes[at + 21]) << 24U);
    if (length == 0 || length > kNabtsCarriedBytesMax) {
      continue;
    }
    // The four bytes after the length: the sum of the file's bytes.
    std::optional<uint32_t> checksum;
    if (stream.arrived(at + 22, 4)) {
      checksum = static_cast<uint32_t>(bytes[at + 22]) |
                 (static_cast<uint32_t>(bytes[at + 23]) << 8U) |
                 (static_cast<uint32_t>(bytes[at + 24]) << 16U) |
                 (static_cast<uint32_t>(bytes[at + 25]) << 24U);
    }
    const std::string time =
        std::string(bytes.begin() + static_cast<ptrdiff_t>(at),
                    bytes.begin() + static_cast<ptrdiff_t>(at + 10)) +
        " " +
        std::string(bytes.begin() + static_cast<ptrdiff_t>(at + 10),
                    bytes.begin() + static_cast<ptrdiff_t>(at + 18));
    const std::string key = *name + "|" + time + "|" + std::to_string(length);
    auto it = files.find(key);
    if (it == files.end()) {
      if (files.size() >= kNabtsCarriedFilesMax) {
        continue;
      }
      CarouselFile file;
      file.name = *name;
      file.time = time;
      file.length = length;
      file.checksum = checksum;
      file.blocks.resize((length + kNabtsCarouselBlockData - 1) /
                         kNabtsCarouselBlockData);
      it = files.emplace(key, std::move(file)).first;
    }
    if (!it->second.checksum) {
      it->second.checksum = checksum;
    }
    read_copy(stream, at + 22, it->second);
    at += 21;
  }

  std::vector<NabtsCarriedFile> out;
  for (const auto& entry : files) {
    const CarouselFile& file = entry.second;
    if (file.copies == 0) {
      continue;
    }
    // Passed over, not the end of the listing: see transfer_files().
    if (bytes_held(out) + file.length > kNabtsCarriedBytesMax) {
      continue;
    }
    out.push_back(assemble(file, datacast.channel));
  }
  // One offset serves every file of the carousel; without a whole file to
  // learn it from, the names alone give the keys.
  const unsigned offset = carousel_key_offset(out).value_or(0);
  for (NabtsCarriedFile& carried : out) {
    decipher(carried, offset);
    if (const std::optional<uint32_t> checksum = carried.checksum;
        checksum.has_value() && carried.data_present.empty()) {
      const uint32_t wanted = *checksum;
      uint64_t sum = 0;
      for (const uint8_t b : carried.data) {
        sum += b;
      }
      carried.checksum_holds = sum == wanted;
    }
    carried.format = file_format(carried.data, carried.data_present);
  }
  return out;
}

/// The character |name| of an HTML character reference names, or nothing.
std::optional<std::string> named_reference(const std::string& name) {
  static const std::map<std::string, std::string> kNames = {
      {"nbsp", " "},  {"amp", "&"},   {"lt", "<"},     {"gt", ">"},
      {"quot", "\""}, {"apos", "'"},  {"copy", "(c)"}, {"reg", "(R)"},
      {"mdash", "-"}, {"ndash", "-"}, {"bull", "*"},   {"middot", "."}};
  std::string lower;
  for (const char c : name) {
    lower.push_back(
        static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  }
  const auto it = kNames.find(lower);
  if (it == kNames.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace

std::string nabts_file_format(const std::vector<uint8_t>& data,
                              const std::vector<uint8_t>& present) {
  return file_format(data, present);
}

int nabts_carousel_byte_index(uint8_t byte) { return kCarouselByteIndex[byte]; }

std::vector<NabtsCarriedFile> nabts_datacast_files(
    const NabtsDatacastChannel& datacast) {
  std::vector<NabtsCarriedFile> files = transfer_files(datacast);
  if (files.empty()) {
    files = carousel_files(datacast);
  }
  return files;
}

std::string nabts_html_text(const std::vector<uint8_t>& html) {
  const std::string source(html.begin(), html.end());
  std::string lower(source.size(), '\0');
  std::transform(source.begin(), source.end(), lower.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  static const char* const kBreaks[] = {
      "br",  "p",  "/p",  "tr",   "/tr",   "div",    "/div",
      "h1",  "h2", "h3",  "h4",   "/h1",   "/h2",    "/h3",
      "/h4", "li", "pre", "/pre", "table", "/table", "hr"};
  std::string out;
  size_t i = 0;
  bool in_pre = false;
  while (i < source.size()) {
    if (lower.compare(i, 4, "<!--") == 0) {
      const size_t end = lower.find("-->", i + 4);
      i = end == std::string::npos ? source.size() : end + 3;
      continue;
    }
    if (source[i] == '<') {
      const size_t end = source.find('>', i + 1);
      if (end == std::string::npos) {
        break;
      }
      size_t name_end = i + 1;
      while (name_end < end &&
             !std::isspace(static_cast<unsigned char>(source[name_end]))) {
        ++name_end;
      }
      const std::string tag = lower.substr(i + 1, name_end - i - 1);
      if (tag == "style" || tag == "script" || tag == "title") {
        const size_t close = lower.find("</" + tag, end);
        i = close == std::string::npos ? source.size()
                                       : source.find('>', close) + 1;
        if (i == 0) {
          i = source.size();
        }
        continue;
      }
      in_pre = tag == "pre" ? true : (tag == "/pre" ? false : in_pre);
      if (std::find_if(std::begin(kBreaks), std::end(kBreaks),
                       [&tag](const char* b) { return tag == b; }) !=
          std::end(kBreaks)) {
        out.push_back('\n');
      } else if (tag == "td" || tag == "th") {
        out.push_back(' ');
      }
      i = end + 1;
      continue;
    }
    if (source[i] == '&') {
      const size_t end = source.find(';', i + 1);
      if (end != std::string::npos && end - i <= 8) {
        const std::string name = source.substr(i + 1, end - i - 1);
        std::optional<std::string> text;
        if (!name.empty() && name[0] == '#') {
          const int code = std::atoi(name.c_str() + 1);
          if (code > 0 && code < 128) {
            text = std::string(1, static_cast<char>(code));
          } else if (code >= 128) {
            text = " ";
          }
        } else {
          text = named_reference(name);
        }
        if (text) {
          out += *text;
          i = end + 1;
          continue;
        }
      }
    }
    const char c = source[i++];
    if (c == '\r') {
      continue;
    }
    if (c == '\n' && !in_pre) {
      out.push_back(' ');
      continue;
    }
    out.push_back(c);
  }
  // Spaces collapsed within lines, lines trimmed, and blank lines run
  // together.
  std::string tidy;
  std::string line;
  int blank = 0;
  const auto flush = [&]() {
    size_t a = line.find_first_not_of(' ');
    size_t b = line.find_last_not_of(' ');
    const std::string trimmed =
        a == std::string::npos ? std::string() : line.substr(a, b - a + 1);
    if (trimmed.empty()) {
      if (++blank == 1 && !tidy.empty()) {
        tidy.push_back('\n');
      }
    } else {
      blank = 0;
      tidy += trimmed;
      tidy.push_back('\n');
    }
    line.clear();
  };
  for (const char c : out) {
    if (c == '\n') {
      flush();
      continue;
    }
    if (c == ' ' && !line.empty() && line.back() == ' ') {
      continue;
    }
    line.push_back(c == '\t' ? ' ' : c);
  }
  flush();
  return tidy;
}

}  // namespace orc
