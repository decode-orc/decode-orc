/*
 * File:        nabts_decompress.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     The two compressions a framed datacast service's containers
 *              use: zlib's (RFC 1950/1951) and the implode of PKWARE's Data
 *              Compression Library
 *
 * Both decoders follow the method of Mark Adler's puff and blast, the small
 * reference decoders that come with zlib: canonical Huffman codes decoded a
 * bit at a time from their counts by length.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_decompress.h"

#include <array>
#include <optional>

namespace orc {

namespace {

// The longest code either format uses.
constexpr int kMaxCodeBits = 15;

/// Bits of a stream, least significant first, as both formats pack them.
class BitReader {
 public:
  BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  /// The next |need| bits (at most 32), or nothing where the stream ran out.
  std::optional<uint32_t> bits(int need) {
    uint64_t value = buffer_;
    while (count_ < need) {
      if (at_ == size_) {
        return std::nullopt;
      }
      value |= static_cast<uint64_t>(data_[at_++]) << count_;
      count_ += 8;
    }
    buffer_ = value >> need;
    count_ -= need;
    return static_cast<uint32_t>(value & ((uint64_t{1} << need) - 1));
  }

  /// The bits left of the byte being read dropped.
  void align() {
    buffer_ = 0;
    count_ = 0;
  }

  /// The stream's bytes taken.
  size_t used() const { return at_; }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t at_ = 0;
  uint64_t buffer_ = 0;
  int count_ = 0;
};

/// A canonical Huffman code: how many codes each length has, and the symbols
/// in the order of their codes.
struct Huffman {
  std::array<uint16_t, kMaxCodeBits + 1> count{};
  std::vector<uint16_t> symbol;
};

/// The code whose symbols have |lengths|, 0 for a symbol it lacks; false
/// where the lengths ask for more codes than there are.
bool build(Huffman& code, const uint8_t* lengths, size_t symbols) {
  code.count.fill(0);
  for (size_t s = 0; s < symbols; ++s) {
    ++code.count[lengths[s]];
  }
  int left = 1;
  for (int length = 1; length <= kMaxCodeBits; ++length) {
    left = (left << 1) - code.count[length];
    if (left < 0) {
      return false;
    }
  }
  std::array<uint16_t, kMaxCodeBits + 1> offset{};
  for (int length = 1; length < kMaxCodeBits; ++length) {
    offset[length + 1] =
        static_cast<uint16_t>(offset[length] + code.count[length]);
  }
  code.symbol.assign(symbols, 0);
  for (size_t s = 0; s < symbols; ++s) {
    if (lengths[s] != 0) {
      code.symbol[offset[lengths[s]]++] = static_cast<uint16_t>(s);
    }
  }
  return true;
}

/// The next symbol of |code|, its bits first to last as the stream holds
/// them — each taken the other way where |inverted|, as an imploded stream
/// sends them. Nothing where the stream ran out or holds no code.
std::optional<int> decode(BitReader& in, const Huffman& code,
                          bool inverted = false) {
  int bits = 0;
  int first = 0;
  int index = 0;
  for (int length = 1; length <= kMaxCodeBits; ++length) {
    const std::optional<uint32_t> bit = in.bits(1);
    if (!bit) {
      return std::nullopt;
    }
    bits |= static_cast<int>(*bit ^ (inverted ? 1U : 0U));
    const int count = code.count[length];
    if (bits - count < first) {
      return code.symbol[index + (bits - first)];
    }
    index += count;
    first = (first + count) << 1;
    bits <<= 1;
  }
  return std::nullopt;
}

// ---------------------------------------------------------------------------
// Inflate (RFC 1951)
// ---------------------------------------------------------------------------

constexpr std::array<uint16_t, 29> kLengthBase = {
    3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<uint8_t, 29> kLengthExtra = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1,
                                                  1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
                                                  4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<uint16_t, 30> kDistanceBase = {
    1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<uint8_t, 30> kDistanceExtra = {
    0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
// The order a dynamic block sends the lengths of its code-length code in.
constexpr std::array<uint8_t, 19> kCodeLengthOrder = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

constexpr size_t kLiteralSymbols = 288;
constexpr size_t kDistanceSymbols = 30;

/// One block's literals, lengths and distances, to its end code, onto
/// |out|; false where the stream broke off or holds no such block.
bool inflate_codes(BitReader& in, const Huffman& literals,
                   const Huffman& distances, std::vector<uint8_t>& out,
                   size_t limit) {
  while (true) {
    const std::optional<int> symbol = decode(in, literals);
    if (!symbol) {
      return false;
    }
    if (*symbol < 256) {
      if (out.size() >= limit) {
        return false;
      }
      out.push_back(static_cast<uint8_t>(*symbol));
      continue;
    }
    if (*symbol == 256) {
      return true;
    }
    const auto l = static_cast<size_t>(*symbol - 257);
    if (l >= kLengthBase.size()) {
      return false;
    }
    const std::optional<uint32_t> length_extra = in.bits(kLengthExtra[l]);
    const std::optional<int> d = decode(in, distances);
    if (!length_extra || !d ||
        static_cast<size_t>(*d) >= kDistanceBase.size()) {
      return false;
    }
    const std::optional<uint32_t> distance_extra =
        in.bits(kDistanceExtra[static_cast<size_t>(*d)]);
    if (!distance_extra) {
      return false;
    }
    const size_t length = kLengthBase[l] + *length_extra;
    const size_t distance =
        kDistanceBase[static_cast<size_t>(*d)] + *distance_extra;
    if (distance > out.size() || out.size() + length > limit) {
      return false;
    }
    for (size_t i = 0; i < length; ++i) {
      out.push_back(out[out.size() - distance]);
    }
  }
}

/// The code lengths a dynamic block sends, and its codes from them.
bool dynamic_codes(BitReader& in, Huffman& literals, Huffman& distances) {
  const std::optional<uint32_t> hlit = in.bits(5);
  const std::optional<uint32_t> hdist = in.bits(5);
  const std::optional<uint32_t> hclen = in.bits(4);
  if (!hlit || !hdist || !hclen) {
    return false;
  }
  const size_t nlen = *hlit + 257;
  const size_t ndist = *hdist + 1;
  if (nlen > 286 || ndist > kDistanceSymbols) {
    return false;
  }
  std::array<uint8_t, kCodeLengthOrder.size()> code_lengths{};
  for (size_t i = 0; i < *hclen + 4; ++i) {
    const std::optional<uint32_t> length = in.bits(3);
    if (!length) {
      return false;
    }
    code_lengths[kCodeLengthOrder[i]] = static_cast<uint8_t>(*length);
  }
  Huffman lengths_code;
  if (!build(lengths_code, code_lengths.data(), code_lengths.size())) {
    return false;
  }
  std::array<uint8_t, kLiteralSymbols + kDistanceSymbols> lengths{};
  size_t at = 0;
  while (at < nlen + ndist) {
    const std::optional<int> symbol = decode(in, lengths_code);
    if (!symbol) {
      return false;
    }
    if (*symbol < 16) {
      lengths[at++] = static_cast<uint8_t>(*symbol);
      continue;
    }
    uint8_t repeated = 0;
    std::optional<uint32_t> times;
    if (*symbol == 16) {
      if (at == 0) {
        return false;
      }
      repeated = lengths[at - 1];
      times = in.bits(2);
      times = times ? std::optional<uint32_t>(*times + 3) : std::nullopt;
    } else if (*symbol == 17) {
      times = in.bits(3);
      times = times ? std::optional<uint32_t>(*times + 3) : std::nullopt;
    } else {
      times = in.bits(7);
      times = times ? std::optional<uint32_t>(*times + 11) : std::nullopt;
    }
    if (!times || at + *times > nlen + ndist) {
      return false;
    }
    for (uint32_t k = 0; k < *times; ++k) {
      lengths[at++] = repeated;
    }
  }
  if (lengths[256] == 0) {
    return false;  // no end code: no block can end
  }
  return build(literals, lengths.data(), nlen) &&
         build(distances, lengths.data() + nlen, ndist);
}

/// The fixed codes (RFC 1951 §3.2.6).
void fixed_codes(Huffman& literals, Huffman& distances) {
  std::array<uint8_t, kLiteralSymbols> lengths{};
  for (size_t s = 0; s < kLiteralSymbols; ++s) {
    lengths[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
  }
  build(literals, lengths.data(), lengths.size());
  std::array<uint8_t, kDistanceSymbols> distance_lengths{};
  distance_lengths.fill(5);
  build(distances, distance_lengths.data(), distance_lengths.size());
}

uint32_t adler32(const std::vector<uint8_t>& bytes) {
  constexpr uint32_t kModulus = 65521;
  uint32_t a = 1;
  uint32_t b = 0;
  for (const uint8_t byte : bytes) {
    a = (a + byte) % kModulus;
    b = (b + a) % kModulus;
  }
  return (b << 16U) | a;
}

// ---------------------------------------------------------------------------
// Explode (PKWARE Data Compression Library)
// ---------------------------------------------------------------------------

// The codes' lengths, each byte a run of symbols of one length: its low four
// bits the length, its high four one less than the run.
constexpr std::array<uint8_t, 98> kExplodeLiteralLengths = {
    11,  124, 8,  7,  28, 7,  188, 13, 76, 4,  10, 8,  12, 10,  12,  10,  8,
    23,  8,   9,  7,  6,  7,  8,   7,  6,  55, 8,  23, 24, 12,  11,  7,   9,
    11,  12,  6,  7,  22, 5,  7,   24, 6,  11, 9,  6,  7,  22,  7,   11,  38,
    7,   9,   8,  25, 11, 8,  11,  9,  12, 8,  12, 5,  38, 5,   38,  5,   11,
    7,   5,   6,  21, 6,  10, 53,  8,  7,  24, 10, 27, 44, 253, 253, 253, 252,
    252, 252, 13, 12, 45, 12, 45,  12, 61, 12, 45, 44, 173};
constexpr std::array<uint8_t, 6> kExplodeLengthLengths = {2,  35, 36,
                                                          53, 38, 23};
constexpr std::array<uint8_t, 7> kExplodeDistanceLengths = {2,   20,  53, 230,
                                                            247, 151, 248};
constexpr std::array<uint16_t, 16> kExplodeLengthBase = {
    3, 2, 4, 5, 6, 7, 8, 9, 10, 12, 16, 24, 40, 72, 136, 264};
constexpr std::array<uint8_t, 16> kExplodeLengthExtra = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
// The match length that ends the stream.
constexpr size_t kExplodeEnd = 519;

template <size_t N>
Huffman explode_code(const std::array<uint8_t, N>& runs) {
  std::vector<uint8_t> lengths;
  for (const uint8_t run : runs) {
    lengths.insert(lengths.end(), (run >> 4U) + 1U,
                   static_cast<uint8_t>(run & 0x0FU));
  }
  Huffman code;
  build(code, lengths.data(), lengths.size());
  return code;
}

}  // namespace

NabtsDecompressed nabts_inflate(const uint8_t* data, size_t size,
                                size_t limit) {
  NabtsDecompressed out;
  // RFC 1950 §2.2: deflate, a window of at most 32 KiB, no preset dictionary,
  // the header a multiple of 31.
  if (size < 2 || (data[0] & 0x0FU) != 8 || (data[0] >> 4U) > 7 ||
      ((data[0] << 8U) | data[1]) % 31 != 0 || (data[1] & 0x20U) != 0) {
    return out;
  }
  BitReader in(data + 2, size - 2);
  bool last = false;
  while (!last) {
    const std::optional<uint32_t> header = in.bits(3);
    if (!header) {
      return out;
    }
    last = (*header & 1U) != 0;
    const uint32_t type = *header >> 1U;
    bool read = false;
    if (type == 0) {
      // A stored block: its length and the length's complement, then its
      // bytes as they are.
      in.align();
      const std::optional<uint32_t> length = in.bits(16);
      const std::optional<uint32_t> complement = in.bits(16);
      const uint32_t count = length.value_or(0);
      read = length && complement && (count ^ 0xFFFFU) == *complement &&
             out.bytes.size() + count <= limit;
      for (uint32_t i = 0; read && i < count; ++i) {
        const std::optional<uint32_t> byte = in.bits(8);
        read = byte.has_value();
        if (read) {
          out.bytes.push_back(static_cast<uint8_t>(*byte));
        }
      }
    } else if (type == 1 || type == 2) {
      Huffman literals;
      Huffman distances;
      if (type == 1) {
        fixed_codes(literals, distances);
        read = true;
      } else {
        read = dynamic_codes(in, literals, distances);
      }
      read = read && inflate_codes(in, literals, distances, out.bytes, limit);
    }
    if (!read) {
      out.used = 2 + in.used();
      return out;
    }
  }
  // The Adler-32 of what the blocks gave, most significant byte first.
  in.align();
  uint32_t check = 0;
  for (int i = 0; i < 4; ++i) {
    const std::optional<uint32_t> byte = in.bits(8);
    if (!byte) {
      out.used = 2 + in.used();
      return out;
    }
    check = (check << 8U) | *byte;
  }
  out.used = 2 + in.used();
  out.ended = true;
  out.complete = check == adler32(out.bytes);
  return out;
}

NabtsDecompressed nabts_explode(const uint8_t* data, size_t size,
                                size_t limit) {
  static const Huffman kLiterals = explode_code(kExplodeLiteralLengths);
  static const Huffman kLengths = explode_code(kExplodeLengthLengths);
  static const Huffman kDistances = explode_code(kExplodeDistanceLengths);
  NabtsDecompressed out;
  BitReader in(data, size);
  const std::optional<uint32_t> coded = in.bits(8);
  const std::optional<uint32_t> dictionary = in.bits(8);
  if (!coded || !dictionary || *coded > 1 || *dictionary < 4 ||
      *dictionary > 6) {
    return out;
  }
  while (true) {
    out.used = in.used();
    const std::optional<uint32_t> match = in.bits(1);
    if (!match) {
      return out;
    }
    if (*match == 0) {
      // A literal, coded or as its eight bits.
      std::optional<int> literal;
      if (*coded != 0) {
        literal = decode(in, kLiterals, true);
      } else if (const std::optional<uint32_t> byte = in.bits(8)) {
        literal = static_cast<int>(*byte);
      }
      if (!literal || out.bytes.size() >= limit) {
        return out;
      }
      out.bytes.push_back(static_cast<uint8_t>(*literal));
      continue;
    }
    const std::optional<int> l = decode(in, kLengths, true);
    if (!l) {
      return out;
    }
    const std::optional<uint32_t> length_extra =
        in.bits(kExplodeLengthExtra[static_cast<size_t>(*l)]);
    if (!length_extra) {
      return out;
    }
    const size_t length =
        kExplodeLengthBase[static_cast<size_t>(*l)] + *length_extra;
    if (length == kExplodeEnd) {
      out.used = in.used();
      out.ended = true;
      out.complete = true;
      return out;
    }
    // A match of two takes two low bits of its distance, any other as many
    // as the dictionary's size says.
    const int low_bits = length == 2 ? 2 : static_cast<int>(*dictionary);
    const std::optional<int> high = decode(in, kDistances, true);
    if (!high) {
      return out;
    }
    const std::optional<uint32_t> low = in.bits(low_bits);
    if (!low) {
      return out;
    }
    const size_t distance =
        (static_cast<size_t>(*high) << static_cast<size_t>(low_bits)) + *low +
        1;
    if (distance > out.bytes.size() || out.bytes.size() + length > limit) {
      return out;
    }
    for (size_t i = 0; i < length; ++i) {
      out.bytes.push_back(out.bytes[out.bytes.size() - distance]);
    }
  }
}

uint32_t nabts_crc32(const uint8_t* data, size_t size) {
  static const std::array<uint32_t, 256> kTable = [] {
    std::array<uint32_t, 256> table{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1U) != 0 ? 0xEDB88320U ^ (c >> 1U) : c >> 1U;
      }
      table[n] = c;
    }
    return table;
  }();
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < size; ++i) {
    crc = kTable[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8U);
  }
  return crc ^ 0xFFFFFFFFU;
}

}  // namespace orc
