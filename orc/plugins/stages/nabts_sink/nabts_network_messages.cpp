/*
 * File:        nabts_network_messages.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     A network's data services for its stations, read out of the
 *              private-use data groups that carried them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_network_messages.h"

#include <fmt/format.h>

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "nabts_service_census.h"
#include "vbi-services/teletext_page_decoder.h"
#include "vbi-services/teletext_slicer.h"

namespace orc {
namespace {

// A byte that never arrived, among bytes held as int.
constexpr int kLost = -1;
// A seven-bit character that failed its odd parity in every copy: it reads as
// no control and is shown as the replacement character, as the receivers of
// the day drew such a character as a block rather than as what it read as.
constexpr int kDamaged = kNabtsDamagedCharacter;
constexpr const char* kReplacement = "\xEF\xBF\xBD";  // U+FFFD in UTF-8

constexpr int kNul = 0x00;
constexpr int kSoh = 0x01;
constexpr int kStx = 0x02;
constexpr int kEtx = 0x03;
constexpr int kEot = 0x04;
constexpr int kTab = 0x09;
constexpr int kLf = 0x0A;
constexpr int kCr = 0x0D;
constexpr int kShiftOut = 0x0E;
constexpr int kShiftIn = 0x0F;
constexpr int kEsc = 0x1B;
constexpr int kDc1 = 0x11;
constexpr int kDc4 = 0x14;
constexpr int kSyn = 0x16;
constexpr int kUs = 0x1F;
constexpr int kSpace = 0x20;
constexpr int kDel = 0x7F;
// NUL with its odd parity bit, which pads a group out.
constexpr int kNulWithParity = 0x80;

// The sequence number's place among a header's nibbles, and the addressing
// field's after it.
constexpr size_t kSequenceNibble = 4;
constexpr size_t kAddressingNibble = 5;
// An addressing field that names every station: its mode nibble, then F's,
// three nibbles in all.
constexpr uint8_t kAddressAll = 0x7;
constexpr size_t kAddressAllNibbles = 3;
// One that names stations by a bitmap after its mode nibble.
constexpr uint8_t kAddressBitmap = 0x3;
// One that names a station: a mode of D or above, a nibble, and the station's
// address in four nibbles.
constexpr uint8_t kAddressStationLowest = 0xD;
constexpr size_t kStationFormNibbles = 6;

// A run of NUL this long ends an item that does not run on into the next
// group: what is left of a group after its item is padding.
constexpr size_t kPaddingRun = 4;
// How often a header must be proven before a longer run is cut back to it.
constexpr size_t kProvenHeaderMinCount = 2;
// The share of a stream's bytes, in percent, that must carry odd parity for it
// to be read as seven-bit characters.
constexpr size_t kSevenBitMinPercent = 90;
// An item needs this many bytes that arrived and are not padding to be kept.
constexpr size_t kItemMinBytes = 4;
// Two items are copies of one where they disagree on no more than this share
// of the bytes both have, in percent: what a tape misreads.
constexpr size_t kCopyMaxDifferPercent = 2;
// A service whose groups open with SOH at least this often, in percent, sends
// one at the head of every group, a part of an item as well as the first.
constexpr size_t kSohPerGroupPercent = 90;

// The 4-for-3 coding's characters, a token's length, and the share of a run's
// tokens, in percent, that must be four of them for the run to be coded: half
// where the framing around the run vouches for it, most where nothing does.
constexpr int k4for3Low = 0x30;
constexpr int k4for3High = 0x6F;
constexpr size_t k4for3TokenChars = 4;
constexpr size_t k4for3MinPercent = 50;
constexpr size_t k4for3UnframedMinPercent = 75;
// The share of a decoded run's bytes, in percent, that must be text for a run
// with no framing to be read as a coded message's text.
constexpr size_t kCodedTextMinPercent = 90;
// The fill a coded message's text ends on.
constexpr int kTextFill = 0xFF;

// The binary heading block of a coded message: least size, magic, and where
// its fields lie.
constexpr size_t kHeadingBlockMinBytes = 0x100;
constexpr size_t kHeadingBlockMagicAt = 10;
constexpr uint8_t kHeadingBlockMagicLow = 0xCD;
constexpr uint8_t kHeadingBlockMagicHigh = 0xAB;
// Padding inside the block's text fields: '0' with its top bit set.
constexpr uint8_t kHeadingBlockPad = 0xB0;
struct BlockField {
  const char* name;
  size_t at;
  size_t size;
};
constexpr BlockField kHeadingBlockFields[] = {
    {"Service level", 12, 1}, {"Number", 13, 4},   {"Selector", 17, 5},
    {"Keyword", 22, 22},      {"Version", 44, 20}, {"Priority", 64, 1},
    {"Category", 65, 1},      {"Cycle", 66, 2},    {"Region", 77, 2},
    {"Wire", 124, 2},
};

// A wire's mark for the end of a line set as it stands, before the line end.
constexpr int kQuadLeft = '<';
// The mark a wire's text puts before a headline.
constexpr char kHeadlineMark = '^';

bool is_padding(int byte) { return byte == kNul || byte == kNulWithParity; }

bool is_damaged(int byte) { return byte >= 0 && (byte & kDamaged) != 0; }

/// A byte as read, a damaged character as the seven bits it arrived with.
int value_of(int byte) { return byte < 0 ? byte : (byte & ~kDamaged); }

/// Where a byte of a stream stands in the vote among its copies.
enum class VoteTier : uint8_t { kLost, kFailing, kClean };

/**
 * @brief One byte as its copies voted it
 *
 * A byte that arrived beats one that did not, and in a stream of seven-bit
 * characters one that passes its odd parity beats one that fails it, however
 * many copies hold that one. Within a tier the copies' majority decides,
 * counted as it goes — the value held and the copies by which it leads — so
 * where two copies disagree and nothing else tells them apart the first stands.
 */
struct VotedByte {
  int value = 0;
  VoteTier tier = VoteTier::kLost;
  uint32_t lead = 0;
};

/// |held| with |other|'s copies voted into it.
void vote(VotedByte& held, const VotedByte& other) {
  if (other.tier > held.tier) {
    held = other;
    return;
  }
  if (other.tier < held.tier || other.tier == VoteTier::kLost) {
    return;
  }
  if (other.value == held.value) {
    held.lead += other.lead;
  } else if (other.lead > held.lead) {
    held.value = other.value;
    held.lead = other.lead - held.lead;
  } else {
    held.lead -= other.lead;
  }
}

/// One copy's byte as it votes: lost, or failing its parity where the stream
/// is seven-bit, or clean.
VotedByte copy_vote(uint8_t byte, uint8_t present, bool seven_bit) {
  if (present == kNabtsByteLost) {
    return {};
  }
  return {byte,
          seven_bit && !teletext_odd_parity_valid(byte) ? VoteTier::kFailing
                                                        : VoteTier::kClean,
          1};
}

/// A voted byte as the readers take it: kLost, a seven-bit character (with
/// kDamaged where it failed its parity in every copy), or an eight-bit byte.
/// A NUL failing its parity is the padding's NUL whose parity bit, the last a
/// byte sends, the recording lost: padding still.
int shown(const VotedByte& byte) {
  switch (byte.tier) {
    case VoteTier::kLost:
      return kLost;
    case VoteTier::kFailing:
      return byte.value == kNul ? kNul : byte.value | kDamaged;
    case VoteTier::kClean:
      break;
  }
  return byte.value;
}

size_t bits_set(unsigned value) {
  size_t count = 0;
  for (; value != 0; value &= value - 1) {
    ++count;
  }
  return count;
}

/// A header's nibbles with its sequence number left out: what copies and
/// proofs of one header have in common.
std::vector<uint8_t> masked(const std::vector<uint8_t>& nibbles, size_t count) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < count && i < nibbles.size(); ++i) {
    if (i != kSequenceNibble) {
      out.push_back(nibbles[i]);
    }
  }
  return out;
}

uint16_t service_of(const std::vector<uint8_t>& nibbles) {
  return static_cast<uint16_t>((nibbles[1] << 8) | (nibbles[2] << 4) |
                               nibbles[3]);
}

std::string kind_label(uint8_t kind) {
  switch (kind) {
    case kNabtsNetworkPage:
      return "page";
    case kNabtsNetworkMessage:
      return "message";
    case kNabtsNetworkPageWithText:
      return "page with text";
    case kNabtsNetworkUrgent:
      return "urgent message";
    default:
      return fmt::format("kind {:X}", kind);
  }
}

/// The length of a header whose addressing field's form fixes it, or 0: the
/// field that names every station ends after its mode and two F's, however
/// many codewords the text after it happens to open with.
size_t fixed_header_length(const std::vector<uint8_t>& nibbles) {
  const size_t end = kAddressingNibble + kAddressAllNibbles;
  if (nibbles.size() < end || nibbles[kAddressingNibble] != kAddressAll) {
    return 0;
  }
  for (size_t i = kAddressingNibble + 1; i < end; ++i) {
    if (nibbles[i] != 0xF) {
      return 0;
    }
  }
  return end;
}

/// Whom a header addresses its group to, as far as its addressing field says.
/// A header a lost byte cut short takes its length from the service's proven
/// headers, so it can end past the nibbles that arrived: those are shown as
/// '-' and say nothing.
std::string addressing_of(const std::vector<uint8_t>& nibbles, size_t header) {
  if (header <= kAddressingNibble || nibbles.size() <= kAddressingNibble) {
    return {};
  }
  const size_t arrived = std::min(header, nibbles.size());
  const bool whole = arrived == header;
  std::string coded;
  for (size_t i = kAddressingNibble; i < header; ++i) {
    coded += i < arrived ? fmt::format("{:X}", nibbles[i]) : "-";
  }
  const uint8_t mode = nibbles[kAddressingNibble];
  const bool rest_all =
      whole &&
      std::all_of(
          nibbles.begin() + static_cast<std::ptrdiff_t>(kAddressingNibble + 1),
          nibbles.begin() + static_cast<std::ptrdiff_t>(header),
          [](uint8_t n) { return n == 0xF; });
  if (mode == kAddressAll && rest_all) {
    return "all stations";
  }
  // The short form: the mode, a nibble, and one station's 16-bit address, as
  // the stations' receivers matched one.
  if (whole && header - kAddressingNibble == kStationFormNibbles &&
      mode >= kAddressStationLowest) {
    const unsigned station = (nibbles[kAddressingNibble + 2] << 12) |
                             (nibbles[kAddressingNibble + 3] << 8) |
                             (nibbles[kAddressingNibble + 4] << 4) |
                             nibbles[kAddressingNibble + 5];
    return fmt::format("station address {} ({})", station, coded);
  }
  if (mode == kAddressBitmap) {
    size_t set = 0;
    for (size_t i = kAddressingNibble + 1; i < arrived; ++i) {
      set += bits_set(nibbles[i]);
    }
    return fmt::format("{}{} of {} stations by bitmap ({})",
                       whole ? "" : "at least ", set,
                       4 * (header - kAddressingNibble - 1), coded);
  }
  return "as coded: " + coded;
}

/// Bytes as text: a line end (CR LF, CR or LF) as '\n', a tab as itself, a
/// printable character as itself, a run of lost bytes as "[...]", a damaged
/// character as U+FFFD, the text fill dropped, and any other byte by its name.
/// |quad| drops the wire's mark at the end of a line set as it stands.
std::string as_text(const std::vector<int>& bytes, size_t from, size_t to,
                    bool quad) {
  std::string out;
  bool hole = false;
  for (size_t i = from; i < to && i < bytes.size(); ++i) {
    const int byte = bytes[i];
    if (byte == kLost) {
      if (!hole) {
        out += "[...]";
      }
      hole = true;
      continue;
    }
    hole = false;
    if (is_damaged(byte)) {
      out += kReplacement;
      continue;
    }
    if (byte == kCr && i + 1 < to && bytes[i + 1] == kLf) {
      continue;  // the LF after it ends the line
    }
    if (byte == kCr || byte == kLf) {
      if (quad && !out.empty() && out.back() == kQuadLeft) {
        out.pop_back();
      }
      out += '\n';
    } else if (byte == kTab || (byte >= kSpace && byte < kDel)) {
      out += static_cast<char>(byte);
    } else if (byte == kTextFill || byte == kNul || byte == kNulWithParity) {
      continue;
    } else {
      out += nabts_control_name(byte);
    }
  }
  if (quad && !out.empty() && out.back() == kQuadLeft) {
    out.pop_back();
  }
  return out;
}

std::string trim(const std::string& text) {
  const size_t from = text.find_first_not_of(" \t\n");
  if (from == std::string::npos) {
    return {};
  }
  const size_t to = text.find_last_not_of(" \t\n");
  return text.substr(from, to - from + 1);
}

/// The lines of |text| that are not empty, trimmed.
std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    const std::string line = trim(text.substr(start, end - start));
    if (!line.empty()) {
      out.push_back(line);
    }
    start = end + 1;
  }
  return out;
}

/// Whether a line is only a date and time, as a dated item opens with one.
bool is_date_line(const std::string& line) {
  return !line.empty() && std::all_of(line.begin(), line.end(), [](char c) {
    return (c >= '0' && c <= '9') || c == '/' || c == '-' || c == ':' ||
           c == ' ';
  }) && std::count_if(line.begin(), line.end(), [](char c) {
          return c >= '0' && c <= '9';
        }) >= 6;
}

/// The first line of |text| that says something: one with a letter in it.
std::string first_line(const std::string& text) {
  for (const std::string& line : lines_of(text)) {
    if (!is_date_line(line) &&
        std::any_of(line.begin(), line.end(), [](char c) {
          return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        })) {
      return line;
    }
  }
  return {};
}

/// The printable runs of NAPLPS code worth reading: what it draws from the
/// text set, after SI, rather than the operands of what it draws after SO.
std::string naplps_text(const std::vector<int>& bytes, size_t from, size_t to) {
  std::string out;
  std::string run;
  bool text_set = true;
  const auto flush = [&out, &run]() {
    const size_t letters =
        static_cast<size_t>(std::count_if(run.begin(), run.end(), [](char c) {
          return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        }));
    if (letters >= 4 && letters * 2 >= run.size()) {
      out += trim(run) + "\n";
    }
    run.clear();
  };
  for (size_t i = from; i < to; ++i) {
    const int byte = bytes[i];
    if (byte == kShiftOut || byte == kShiftIn) {
      flush();
      text_set = byte == kShiftIn;
    } else if (byte == kEsc) {
      flush();
      ++i;  // the escape's own byte, which is no text
    } else if (byte >= kSpace && byte < kDel) {
      if (text_set) {
        run += static_cast<char>(byte);
      }
    } else if (is_damaged(byte)) {
      if (text_set) {
        run += kReplacement;
      }
    } else if (!run.empty()) {
      flush();
    }
  }
  flush();
  return out;
}

/// Whether the characters of |from| to |to| read as a run of the 4-for-3
/// coding: at least |min_percent| of their space-separated tokens are four of
/// its characters. A damaged character counts as the seven bits it arrived
/// with, here and wherever the coding's characters are read.
bool reads_as_4for3(const std::vector<int>& bytes, size_t from, size_t to,
                    size_t min_percent = k4for3MinPercent) {
  size_t tokens = 0;
  size_t coded = 0;
  size_t length = 0;
  bool in_range = true;
  const auto end_token = [&]() {
    if (length > 0) {
      ++tokens;
      coded += (length == k4for3TokenChars && in_range) ? 1 : 0;
    }
    length = 0;
    in_range = true;
  };
  for (size_t i = from; i < to; ++i) {
    const int c = value_of(bytes[i]);
    if (c == kSpace) {
      end_token();
    } else if (c != kCr && c != kLf) {
      ++length;
      in_range = in_range && c >= k4for3Low && c <= k4for3High;
    }
  }
  end_token();
  return tokens > 0 && coded * 100 >= tokens * min_percent;
}

/// Whether decoded bytes read as a wire's text: printable characters, tabs,
/// line ends, the text fill and the controls that end it, lost bytes aside.
bool reads_as_text(const std::vector<int>& bytes) {
  size_t known = 0;
  size_t text = 0;
  for (const int read : bytes) {
    if (read == kLost) {
      continue;
    }
    const int byte = value_of(read);
    ++known;
    if ((byte >= kSpace && byte < kDel) || byte == kTab || byte == kCr ||
        byte == kLf || byte == kTextFill || byte == kEtx || byte == kEot ||
        byte == kNul) {
      ++text;
    }
  }
  return known > 0 && text * 100 >= known * kCodedTextMinPercent;
}

/// The characters of |from| to |to| with line ends taken out.
std::vector<int> without_line_ends(const std::vector<int>& bytes, size_t from,
                                   size_t to) {
  std::vector<int> out;
  for (size_t i = from; i < to && i < bytes.size(); ++i) {
    const int c = value_of(bytes[i]);
    if (c != kCr && c != kLf) {
      out.push_back(bytes[i]);
    }
  }
  return out;
}

/// Where "CR LF STX" stands from |from| on, or |to|.
size_t find_text_start(const std::vector<int>& bytes, size_t from, size_t to) {
  for (size_t i = from; i + 2 < to; ++i) {
    if (bytes[i] == kCr && bytes[i + 1] == kLf && bytes[i + 2] == kStx) {
      return i;
    }
  }
  return to;
}

size_t find_byte(const std::vector<int>& bytes, int byte, size_t from,
                 size_t to) {
  for (size_t i = from; i < to; ++i) {
    if (bytes[i] == byte) {
      return i;
    }
  }
  return to;
}

/// One service's stream, as the items in it are read.
struct ServiceStream {
  uint16_t channel = 0;
  uint16_t service = 0;
  /// Whether the service sends seven-bit characters with odd parity.
  bool seven_bit = false;
  /// Its bytes as their copies voted them, parity taken off where the stream
  /// is seven-bit.
  std::vector<VotedByte> votes;
  /// The same as the readers take them (shown()).
  std::vector<int> bytes;
  /// For each byte, the group it came from (an index into the log's entries).
  std::vector<size_t> owner;
  /// For each group that began a stretch of the stream, the copies of it
  /// merged into that stretch.
  std::map<size_t, uint32_t> copies;
};

/// An item's bytes and where they came from, before it is read.
struct Span {
  bool seven_bit = false;
  std::vector<VotedByte> votes;
  /// |votes| as the readers take them (shown()).
  std::vector<int> bytes;
  std::set<size_t> owners;
  uint32_t copies = 0;
};

/// Whether two items are copies of one: of a length, and agreeing where both
/// have a byte but for the few a tape misreads.
bool same_item(const std::vector<int>& a, const std::vector<int>& b) {
  if (a.size() != b.size()) {
    return false;
  }
  size_t both = 0;
  size_t differ = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != kLost && b[i] != kLost) {
      ++both;
      differ += value_of(a[i]) != value_of(b[i]) ? 1 : 0;
    }
  }
  return both >= kItemMinBytes && differ * 100 <= both * kCopyMaxDifferPercent;
}

}  // namespace

std::optional<std::array<uint8_t, 3>> nabts_decode_4for3(const char* four) {
  std::array<int, 4> values{};
  for (size_t i = 0; i < values.size(); ++i) {
    const int c = static_cast<unsigned char>(four[i]);
    if (c < k4for3Low || c > k4for3High) {
      return std::nullopt;
    }
    values[i] = c - k4for3Low;
  }
  std::array<uint8_t, 3> out{};
  for (size_t k = 0; k < out.size(); ++k) {
    out[k] =
        static_cast<uint8_t>(values[k] | (((values[3] >> (2 * k)) & 0x3) << 6));
  }
  return out;
}

std::vector<int> nabts_decode_4for3_run(const std::vector<int>& characters) {
  std::vector<int> out;
  std::vector<int> token;
  const auto flush = [&out, &token]() {
    if (token.empty()) {
      return;
    }
    bool whole = token.size() == k4for3TokenChars;
    std::array<char, k4for3TokenChars> four{};
    for (size_t i = 0; whole && i < four.size(); ++i) {
      whole = token[i] != kLost;
      four[i] = static_cast<char>(value_of(token[i]));
    }
    const auto bytes = whole ? nabts_decode_4for3(four.data()) : std::nullopt;
    if (bytes) {
      // A damaged character spoils the byte whose low six bits it carries,
      // and the fourth, which carries the top two of all three, every one.
      const bool top_damaged = is_damaged(token[3]);
      for (size_t k = 0; k < bytes->size(); ++k) {
        const bool damaged = top_damaged || is_damaged(token[k]);
        out.push_back((*bytes)[k] | (damaged ? kDamaged : 0));
      }
    } else {
      // A token that lost characters stands for as many tokens as its length
      // would have held, a space after each: their bytes are lost.
      const size_t tokens =
          std::max<size_t>(1, (token.size() + 1) / (k4for3TokenChars + 1));
      out.insert(out.end(), 3 * tokens, kLost);
    }
    token.clear();
  };
  for (const int c : characters) {
    if (value_of(c) == kSpace) {
      flush();
    } else {
      token.push_back(c);
    }
  }
  flush();
  return out;
}

std::vector<std::pair<std::string, std::string>> nabts_anpa_heading_fields(
    const std::string& heading) {
  // The heading's parts, split at the controls that separate them: the
  // service level and number, US, the selector, LF, priority and category,
  // the format's two controls, then the keyword line.
  std::vector<std::string> parts;
  std::string format;
  std::string part;
  size_t i = 0;
  while (i < heading.size() && (heading[i] == kSyn || heading[i] == kSoh ||
                                heading[i] == kCr || heading[i] == kLf)) {
    ++i;
  }
  for (; i < heading.size(); ++i) {
    const int c = static_cast<unsigned char>(heading[i]);
    if (c == kUs || c == kLf || c == kCr) {
      parts.push_back(part);
      part.clear();
    } else if (c >= kDc1 && c <= kDc4) {
      format += nabts_control_name(c);
      parts.push_back(part);
      part.clear();
    } else if (c >= kSpace && c < kDel) {
      part += static_cast<char>(c);
    } else if (heading.compare(i, 3, kReplacement) == 0) {
      part += kReplacement;  // a damaged character, as as_text() shows one
      i += 2;
    }
  }
  parts.push_back(part);
  parts.erase(
      std::remove_if(parts.begin(), parts.end(),
                     [](const std::string& p) { return trim(p).empty(); }),
      parts.end());
  std::vector<std::pair<std::string, std::string>> out;
  if (parts.size() < 4 || trim(parts[0]).size() < 2 ||
      trim(parts[2]).size() < 3) {
    return out;
  }
  const std::string first = trim(parts[0]);
  out.emplace_back("Service level", first.substr(0, 1));
  out.emplace_back("Number", first.substr(1));
  out.emplace_back("Selector", trim(parts[1]));
  const std::string level = trim(parts[2]);
  out.emplace_back("Priority", level.substr(0, 1));
  out.emplace_back("Category", level.substr(level.size() - 1));
  if (!format.empty()) {
    out.emplace_back("Format", format);
  }
  std::vector<std::string> words;
  for (size_t p = 3; p < parts.size(); ++p) {
    std::string word;
    for (const char c : parts[p] + " ") {
      if (c == ' ') {
        if (!word.empty()) {
          words.push_back(word);
        }
        word.clear();
      } else {
        word += c;
      }
    }
  }
  static constexpr const char* kAfterKeyword[] = {
      "Keyword", "Version", "Reference", "Filing date", "Word count"};
  for (size_t w = 0; w < words.size(); ++w) {
    const char* name = w < std::size(kAfterKeyword) ? kAfterKeyword[w] : "More";
    out.emplace_back(name, words[w]);
  }
  return out;
}

std::vector<std::pair<std::string, std::string>> nabts_heading_block_fields(
    const std::vector<uint8_t>& block) {
  std::vector<std::pair<std::string, std::string>> out;
  if (block.size() < kHeadingBlockMinBytes ||
      block[kHeadingBlockMagicAt] != kHeadingBlockMagicLow ||
      block[kHeadingBlockMagicAt + 1] != kHeadingBlockMagicHigh) {
    return out;
  }
  for (const BlockField& field : kHeadingBlockFields) {
    std::string value;
    bool printable = true;
    for (size_t i = field.at; i < field.at + field.size; ++i) {
      const uint8_t byte = block[i];
      if (byte == kHeadingBlockPad || byte == kNul) {
        continue;
      }
      printable = printable && byte >= kSpace && byte < kDel;
      value += static_cast<char>(byte);
    }
    value = trim(value);
    if (printable && !value.empty()) {
      out.emplace_back(field.name, value);
    }
  }
  return out;
}

std::string nabts_network_message_document(const NabtsNetworkMessage& message) {
  std::string out = message.title.empty() ? "(untitled)" : message.title;
  out += "\n\n";
  out += fmt::format("{} on {:03X}, record address {}, sequence {:X}",
                     message.kind, message.channel & 0xFFFU,
                     message.record_address, message.sequence);
  if (!message.addressing.empty()) {
    out += "; for " + message.addressing;
  }
  out += fmt::format("\n{}; {} group{}, frames {}-{}", message.form,
                     message.groups, message.groups == 1 ? "" : "s",
                     message.first_frame, message.last_frame);
  if (message.copies > 0) {
    out += fmt::format(", {} more cop{}", message.copies,
                       message.copies == 1 ? "y" : "ies");
  }
  if (message.lost_bytes > 0) {
    out += fmt::format("; {} byte{} never arrived", message.lost_bytes,
                       message.lost_bytes == 1 ? "" : "s");
  }
  if (message.damaged_bytes > 0) {
    out += fmt::format("; {} character{} failed {} parity in every copy",
                       message.damaged_bytes,
                       message.damaged_bytes == 1 ? "" : "s",
                       message.damaged_bytes == 1 ? "its" : "their");
  }
  if (!message.complete) {
    out += "; its end did not arrive";
  }
  out += "\n";
  if (!message.fields.empty()) {
    out += "\n";
    for (const auto& [name, value] : message.fields) {
      out += name + ": " + value + "\n";
    }
  }
  if (!message.text.empty()) {
    out += "\n" + message.text;
    if (message.text.back() != '\n') {
      out += "\n";
    }
  }
  if (!message.trailer.empty()) {
    out += "\n-- " + message.trailer + "\n";
  }
  if (!message.frame.empty()) {
    out += fmt::format("\nHeading block, {} bytes:\n", message.frame.size());
    for (size_t row = 0; row < message.frame.size(); row += 16) {
      std::string characters;
      out += fmt::format("{:04x} ", row);
      for (size_t i = row; i < row + 16 && i < message.frame.size(); ++i) {
        const uint8_t byte = message.frame[i];
        out += fmt::format(" {:02x}", byte);
        characters +=
            byte >= kSpace && byte < kDel ? static_cast<char>(byte) : '.';
      }
      out +=
          std::string(
              3 * (16 - std::min<size_t>(16, message.frame.size() - row)) + 2,
              ' ') +
          characters + "\n";
    }
  }
  return out;
}

std::string nabts_network_message_file_stem(const NabtsNetworkMessage& message,
                                            size_t index) {
  std::string title;
  for (const char c : message.title) {
    const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-';
    if (safe) {
      title += c;
    } else if (!title.empty() && title.back() != '_') {
      title += '_';
    }
    if (title.size() >= 48) {
      break;
    }
  }
  while (!title.empty() && title.back() == '_') {
    title.pop_back();
  }
  return fmt::format("{:04}-{:X}{}{}", index, message.sequence,
                     title.empty() ? "" : "-", title);
}

void NabtsNetworkMessageLog::add_group(const NabtsDataGroup& group,
                                       uint64_t frame) {
  if (group.header.type != kNabtsPrivateGroupType || !group.channel_attested ||
      !group.header.attested) {
    return;
  }
  Entry entry;
  for (size_t i = 0;
       i < group.data.size() && i < kNabtsProviderHeaderMaxNibbles; ++i) {
    if (!group.present.empty() && group.present[i] == kNabtsByteLost) {
      break;
    }
    const int nibble = teletext_hamming84_decode(group.data[i]);
    if (nibble < 0 || !teletext_hamming84_clean(group.data[i])) {
      break;
    }
    entry.nibbles.push_back(static_cast<uint8_t>(nibble));
  }
  if (entry.nibbles.size() < kNabtsProviderHeaderMinNibbles) {
    return;
  }
  if (bytes_ + group.data.size() > kNabtsNetworkMaxBytes) {
    ++dropped_;
    return;
  }
  entry.channel = group.channel;
  entry.frame = frame;
  entry.data = group.data;
  entry.present = group.present;
  entry.present.resize(entry.data.size(), kNabtsByteArrived);
  bytes_ += entry.data.size();
  entries_.push_back(std::move(entry));
}

namespace {

using Entry = NabtsNetworkMessageLog::Entry;
using ServiceKey = std::pair<uint16_t, uint16_t>;

/// Where each group's header ends, what the header says less its sequence
/// number (|key|, what copies of a group have in common), and each service's
/// groups in order of arrival.
struct HeaderEnds {
  std::vector<size_t> end;
  std::vector<std::vector<uint8_t>> key;
  std::map<ServiceKey, std::vector<size_t>> by_service;
};

/// Where each group's header ends: at the longest proven header its run
/// begins with, or where its addressing field's form ends it, or where its
/// run ends — or, where a lost byte cut the run short, at the end of the
/// commonest proven header it agrees with as far as it goes, the lost bytes
/// having been the rest of it. A service's proven headers are those the byte
/// after which arrived and is not a Hamming codeword, counted by their nibbles
/// less the sequence number.
HeaderEnds header_ends(const std::vector<Entry>& entries) {
  std::map<ServiceKey, std::map<std::vector<uint8_t>, size_t>> proven;
  for (const Entry& entry : entries) {
    const size_t end = entry.nibbles.size();
    if (end < entry.data.size() && entry.present[end] != kNabtsByteLost) {
      ++proven[{entry.channel, service_of(entry.nibbles)}]
              [masked(entry.nibbles, end)];
    }
  }

  HeaderEnds out;
  out.end.resize(entries.size());
  out.key.resize(entries.size());
  for (size_t index = 0; index < entries.size(); ++index) {
    const Entry& entry = entries[index];
    const ServiceKey key{entry.channel, service_of(entry.nibbles)};
    const size_t run = entry.nibbles.size();
    const bool cut_by_loss =
        run < entry.data.size() && entry.present[run] == kNabtsByteLost;
    const std::vector<uint8_t> known = masked(entry.nibbles, run);
    size_t within = 0;
    const std::vector<uint8_t>* within_tail = nullptr;
    size_t beyond = 0;
    size_t beyond_count = 0;
    const std::vector<uint8_t>* beyond_tail = nullptr;
    for (const auto& [tail, count] : proven[key]) {
      if (count < kProvenHeaderMinCount) {
        continue;
      }
      const size_t candidate = tail.size() + 1;
      if (candidate <= run) {
        if (candidate > within && masked(entry.nibbles, candidate) == tail) {
          within = candidate;
          within_tail = &tail;
        }
      } else if (cut_by_loss && candidate <= entry.data.size() &&
                 count > beyond_count &&
                 std::equal(known.begin(), known.end(), tail.begin())) {
        beyond = candidate;
        beyond_count = count;
        beyond_tail = &tail;
      }
    }
    if (beyond > 0) {
      out.end[index] = beyond;
      out.key[index] = *beyond_tail;
    } else if (within > 0) {
      out.end[index] = within;
      out.key[index] = *within_tail;
    } else {
      const size_t fixed = fixed_header_length(entry.nibbles);
      out.end[index] = fixed > 0 ? fixed : run;
      out.key[index] = masked(entry.nibbles, out.end[index]);
    }
    out.by_service[key].push_back(index);
  }
  return out;
}

/// A status signal — every group of the service a header of five nibbles and
/// padding — as its values over the recording, in runs; nothing where the
/// service is not one.
std::optional<NabtsNetworkMessage> status_signal(
    const std::vector<Entry>& entries, const std::vector<size_t>& indices,
    const std::vector<size_t>& header, ServiceKey key) {
  const bool status = std::all_of(
      indices.begin(), indices.end(), [&entries, &header](size_t index) {
        const Entry& entry = entries[index];
        if (header[index] != kNabtsProviderHeaderMinNibbles) {
          return false;
        }
        for (size_t i = header[index]; i < entry.data.size(); ++i) {
          if (entry.present[i] != kNabtsByteLost &&
              !is_padding(entry.data[i])) {
            return false;
          }
        }
        return true;
      });
  if (!status) {
    return std::nullopt;
  }
  NabtsNetworkMessage message;
  message.channel = key.first;
  message.record_address = fmt::format("{:03X}", key.second);
  message.kind = "status";
  message.form = "status";
  message.complete = true;
  message.first_frame = entries[indices.front()].frame;
  message.last_frame = entries[indices.back()].frame;
  message.groups = static_cast<uint32_t>(indices.size());
  std::set<uint8_t> values;
  size_t run_start = 0;
  for (size_t i = 0; i <= indices.size(); ++i) {
    if (i < indices.size() &&
        entries[indices[i]].nibbles[kSequenceNibble] ==
            entries[indices[run_start]].nibbles[kSequenceNibble]) {
      continue;
    }
    const uint8_t value = entries[indices[run_start]].nibbles[kSequenceNibble];
    values.insert(value);
    message.text += fmt::format("{:X}  frames {}-{}, {} groups\n", value,
                                entries[indices[run_start]].frame,
                                entries[indices[i - 1]].frame, i - run_start);
    run_start = i;
  }
  std::string list;
  for (const uint8_t value : values) {
    list += (list.empty() ? "" : ", ") + fmt::format("{:X}", value);
  }
  message.title = "Status values " + list;
  message.fields.emplace_back("Values", list);
  return message;
}

/// Whether a service sends seven-bit characters with odd parity: nearly every
/// byte its groups carry after their headers, padding aside, passes it.
bool seven_bit_service(const std::vector<Entry>& entries,
                       const std::vector<size_t>& indices,
                       const std::vector<size_t>& header) {
  size_t counted = 0;
  size_t odd = 0;
  for (const size_t index : indices) {
    const Entry& entry = entries[index];
    if (entry.nibbles[0] == kNabtsNetworkFill) {
      continue;
    }
    for (size_t i = header[index]; i < entry.data.size(); ++i) {
      if (entry.present[i] != kNabtsByteLost && !is_padding(entry.data[i])) {
        ++counted;
        odd += bits_set(entry.data[i]) % 2 == 1 ? 1 : 0;
      }
    }
  }
  return counted > 0 && odd * 100 >= counted * kSevenBitMinPercent;
}

/// One service's stream: each group's data after its header, the copies of a
/// group sent again at once voted into it, a sequence number skipped marked;
/// read as seven-bit characters, parity taken off, where nearly every byte
/// carries odd parity.
ServiceStream service_stream(const std::vector<Entry>& entries,
                             const std::vector<size_t>& indices,
                             const HeaderEnds& headers, ServiceKey key) {
  const std::vector<size_t>& header = headers.end;
  ServiceStream stream;
  stream.channel = key.first;
  stream.service = key.second;
  stream.seven_bit = seven_bit_service(entries, indices, header);
  // A service that opens every group with SOH, a part of an item as well as
  // the first: the SOH of a group that continues an item is no item's own.
  size_t opened = 0;
  size_t opening = 0;
  for (const size_t index : indices) {
    const Entry& entry = entries[index];
    const size_t at = header[index];
    if (entry.nibbles[0] != kNabtsNetworkFill && at < entry.data.size() &&
        entry.present[at] != kNabtsByteLost) {
      ++opened;
      opening += (entry.data[at] & 0x7F) == kSoh ? 1 : 0;
    }
  }
  const bool soh_per_group =
      opened > 0 && opening * 100 >= opened * kSohPerGroupPercent;
  // Whether the stream so far ends inside an item: its last byte that says
  // anything is not the EOT that ends one.
  const auto inside_item = [&stream]() {
    for (size_t i = stream.votes.size(); i > 0; --i) {
      const VotedByte& byte = stream.votes[i - 1];
      if (byte.tier == VoteTier::kLost || is_padding(byte.value) ||
          byte.value == kLf || byte.value == kCr) {
        continue;
      }
      return (byte.value & 0x7F) != kEot;
    }
    return false;
  };
  int last_sequence = -1;
  std::vector<uint8_t> last_header;
  size_t last_start = 0;
  size_t last_owner = 0;
  bool last_skipped = false;
  for (const size_t index : indices) {
    const Entry& entry = entries[index];
    if (entry.nibbles[0] == kNabtsNetworkFill) {
      continue;
    }
    const int sequence = entry.nibbles[kSequenceNibble];
    const std::vector<uint8_t>& here = headers.key[index];
    const bool copy = sequence == last_sequence && here == last_header;
    // A copy leaves out what the group it copies left out.
    const bool skip =
        copy ? last_skipped
             : soh_per_group && header[index] < entry.data.size() &&
                   entry.present[header[index]] != kNabtsByteLost &&
                   (entry.data[header[index]] & 0x7F) == kSoh && inside_item();
    const size_t length = header[index] + (skip ? 1 : 0);
    const size_t payload = entry.data.size() - length;
    if (copy && payload == stream.votes.size() - last_start) {
      for (size_t i = 0; i < payload; ++i) {
        vote(stream.votes[last_start + i],
             copy_vote(entry.data[length + i], entry.present[length + i],
                       stream.seven_bit));
      }
      ++stream.copies[last_owner];
      continue;
    }
    if (last_sequence >= 0 && sequence != (last_sequence + 1) % 16) {
      stream.votes.emplace_back();
      stream.owner.push_back(index);
    }
    last_start = stream.votes.size();
    last_owner = index;
    for (size_t i = length; i < entry.data.size(); ++i) {
      stream.votes.push_back(
          copy_vote(entry.data[i], entry.present[i], stream.seven_bit));
      stream.owner.push_back(index);
    }
    last_sequence = sequence;
    last_header = here;
    last_skipped = skip;
  }

  stream.bytes.reserve(stream.votes.size());
  for (VotedByte& byte : stream.votes) {
    if (stream.seven_bit) {
      byte.value &= 0x7F;
    }
    stream.bytes.push_back(shown(byte));
  }
  return stream;
}

/// The items of a stream: cut at an EOT (kept with the item it ends), at an
/// SOH no SYN SYN leads, and at a run of padding; each with the groups it
/// came from and the copies of them merged into it.
std::vector<Span> cut_items(const ServiceStream& stream) {
  std::vector<Span> spans;
  const std::vector<int>& bytes = stream.bytes;
  size_t begin = 0;
  const auto cut = [&](size_t from, size_t to) {
    size_t content = 0;
    for (size_t i = from; i < to; ++i) {
      const int c = value_of(bytes[i]);
      content +=
          (c != kLost && !is_padding(c) && c != kLf && c != kCr && c != kSpace)
              ? 1
              : 0;
    }
    if (content < kItemMinBytes) {
      return;
    }
    Span span;
    span.seven_bit = stream.seven_bit;
    span.votes.assign(stream.votes.begin() + static_cast<std::ptrdiff_t>(from),
                      stream.votes.begin() + static_cast<std::ptrdiff_t>(to));
    span.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(from),
                      bytes.begin() + static_cast<std::ptrdiff_t>(to));
    for (size_t i = from; i < to; ++i) {
      span.owners.insert(stream.owner[i]);
    }
    for (const size_t owner : span.owners) {
      const auto copies = stream.copies.find(owner);
      span.copies += copies == stream.copies.end() ? 0 : copies->second;
    }
    spans.push_back(std::move(span));
  };
  // Whether what the item holds so far is no more than SOH and a plain
  // heading, as a relayed message opens before its own SYN SYN SOH.
  const auto relay_heading = [&bytes](size_t from, size_t to) {
    if (from >= to || bytes[from] != kSoh) {
      return false;
    }
    for (size_t i = from + 1; i < to; ++i) {
      const int c = value_of(bytes[i]);
      if (c == kLost || c < kSpace || c >= kDel) {
        return false;
      }
    }
    return true;
  };
  for (size_t i = 0; i < bytes.size(); ++i) {
    const int byte = bytes[i];
    if (byte == kEot) {
      cut(begin, i + 1);
      begin = i + 1;
    } else if (byte == kSyn && i + 2 < bytes.size() && bytes[i + 1] == kSyn &&
               bytes[i + 2] == kSoh) {
      if (!relay_heading(begin, i)) {
        cut(begin, i);
        begin = i;
      }
      i += 2;  // the SOH it leads opens no item of its own
    } else if (byte == kSoh) {
      cut(begin, i);
      begin = i;
    } else if (is_padding(byte)) {
      size_t end = i;
      while (end < bytes.size() &&
             (is_padding(bytes[end]) || bytes[end] == kLost)) {
        ++end;
      }
      if (end - i >= kPaddingRun) {
        cut(begin, i);
        begin = end;
        i = end - 1;
      }
    }
  }
  cut(begin, bytes.size());
  return spans;
}

/// Copies of one item, as a carousel sends its items over and over, voted
/// into the first: what one lost, another fills, and a byte one copy received
/// failing its parity another can hold clean.
std::vector<Span> merge_copies(std::vector<Span> spans) {
  std::vector<Span> unique;
  for (Span& span : spans) {
    bool merged = false;
    for (Span& kept : unique) {
      if (same_item(kept.bytes, span.bytes)) {
        for (size_t i = 0; i < kept.votes.size(); ++i) {
          vote(kept.votes[i], span.votes[i]);
          kept.bytes[i] = shown(kept.votes[i]);
        }
        kept.owners.insert(span.owners.begin(), span.owners.end());
        kept.copies += 1 + span.copies;
        merged = true;
        break;
      }
    }
    if (!merged) {
      unique.push_back(std::move(span));
    }
  }
  return unique;
}

/// A voted byte as NAPLPS code takes it: as it arrived, a seven-bit character
/// with its odd parity bit — failing still where it failed in every copy, so
/// that the interpreter skips it as a control, as the receivers did — and NUL
/// where it never arrived.
uint8_t code_byte(const VotedByte& byte, bool seven_bit) {
  if (byte.tier == VoteTier::kLost) {
    return kNul;
  }
  const auto value = static_cast<uint8_t>(byte.value);
  if (!seven_bit) {
    return value;
  }
  const uint8_t sent = teletext_odd_parity_encode(value);
  return byte.tier == VoteTier::kFailing ? static_cast<uint8_t>(sent ^ 0x80)
                                         : sent;
}

/// A page: its NAPLPS code, and the text it draws for a title.
void read_page(NabtsNetworkMessage& message, const Span& span, size_t from,
               size_t to) {
  const std::vector<int>& b = span.bytes;
  message.form = "NAPLPS";
  for (size_t i = from; i < to; ++i) {
    message.code.push_back(code_byte(span.votes[i], span.seven_bit));
  }
  message.text = naplps_text(b, from, to);
  message.title = first_line(message.text);
  if (message.title.empty()) {
    message.title = "Page";
  }
  message.complete = message.lost_bytes == 0;
}

/// The coded form, from the byte after its SOH: a plain heading and SYN SYN
/// SOH or nothing, CR LF, the coded heading block, CR LF STX CR LF, the coded
/// text. False, leaving |message| as it was, where the item is not in it.
bool read_coded(NabtsNetworkMessage& message, const std::vector<int>& b,
                size_t after, size_t to) {
  size_t k = after;
  while (k < to && b[k] != kSyn && b[k] != kSoh && b[k] != kEot &&
         b[k] != kCr) {
    ++k;
  }
  size_t block_start = to;
  if (k + 4 < to && b[k] == kSyn && b[k + 1] == kSyn && b[k + 2] == kSoh &&
      b[k + 3] == kCr) {
    block_start = k + 5;
  } else if (k == after && k + 1 < to && b[k] == kCr && b[k + 1] == kLf) {
    block_start = k + 2;
  }
  const size_t text_mark =
      block_start < to ? find_text_start(b, block_start, to) : to;
  if (block_start >= to || text_mark >= to ||
      !reads_as_4for3(b, block_start, text_mark)) {
    return false;
  }
  message.form = "ANPA 1312, 4-for-3 coded";
  const std::string plain = trim(as_text(b, after, k, false));
  if (!plain.empty()) {
    message.fields.emplace_back("Relay heading", plain);
  }
  const std::vector<int> block =
      nabts_decode_4for3_run(without_line_ends(b, block_start, text_mark));
  for (const int byte : block) {
    message.frame.push_back(
        static_cast<uint8_t>(byte == kLost ? 0 : value_of(byte)));
  }
  for (auto& field : nabts_heading_block_fields(message.frame)) {
    message.fields.push_back(std::move(field));
  }
  // The coded text runs to the next control that is not a line end.
  size_t text_end = text_mark + 3;
  while (text_end < to && (b[text_end] == kLost || b[text_end] >= kSpace ||
                           b[text_end] == kCr || b[text_end] == kLf)) {
    ++text_end;
  }
  const std::vector<int> text =
      nabts_decode_4for3_run(without_line_ends(b, text_mark + 3, text_end));
  // The text ends at an ETX: coded with it, or sent plain after it, the
  // trailer then running plain to the EOT.
  const size_t etx = find_byte(text, kEtx, 0, text.size());
  const size_t eot = find_byte(text, kEot, etx, text.size());
  message.text = as_text(text, 0, etx, true);
  if (etx < text.size()) {
    message.trailer = trim(as_text(text, etx + 1, eot, false));
  }
  const bool plain_end = text_end < to && b[text_end] == kEtx;
  if (plain_end) {
    message.trailer = trim(
        as_text(b, text_end + 1, find_byte(b, kEot, text_end + 1, to), false));
  }
  message.complete = etx < text.size() || plain_end;
  for (const auto& [name, value] : message.fields) {
    if (name == "Keyword") {
      message.title = value;
    }
  }
  if (message.title.empty()) {
    message.title = first_line(message.text);
  }
  return true;
}

/// A plain item from the byte after its SOH: a news wire's message as ANPA
/// 1312 frames it, or a station message (its text, ETX, a trailer line, EOT).
void read_framed(NabtsNetworkMessage& message, const std::vector<int>& b,
                 size_t after, size_t to) {
  const size_t etx = find_byte(b, kEtx, after, to);
  const size_t eot = find_byte(b, kEot, after, to);
  const size_t stx = find_byte(b, kStx, after, std::min(etx, eot));
  if (stx < std::min(etx, eot)) {
    message.form = "ANPA 1312";
    std::string heading;
    for (size_t i = after; i < stx; ++i) {
      if (b[i] == kLost) {
        heading += ' ';
      } else if (is_damaged(b[i])) {
        heading += kReplacement;
      } else if (b[i] < 0x80) {
        heading += static_cast<char>(b[i]);
      }
    }
    message.fields = nabts_anpa_heading_fields(heading);
    if (message.fields.empty()) {
      message.fields.emplace_back("Heading",
                                  trim(as_text(b, after, stx, false)));
    }
    message.text = as_text(b, stx + 1, std::min(etx, to), true);
    if (etx < to) {
      message.trailer = trim(as_text(b, etx + 1, eot, false));
    }
    for (const auto& [name, value] : message.fields) {
      if (name == "Keyword") {
        message.title = value;
      }
    }
  } else {
    message.form = etx < to ? "station message" : "text";
    message.text = as_text(b, after, std::min(etx, to), false);
    if (etx < to) {
      message.trailer = trim(as_text(b, etx + 1, eot, false));
    }
    for (const std::string& line : lines_of(message.text)) {
      if (line.rfind("RE:", 0) == 0) {
        message.title = trim(line.substr(3));
        break;
      }
    }
  }
  if (message.title.empty()) {
    message.title = first_line(message.text);
  }
}

/// The rest of a coded message whose start the recording does not have: its
/// coded text, to the plain ETX that ends it. False, leaving |message| as it
/// was, where the item does not read so.
bool read_coded_remnant(NabtsNetworkMessage& message, const std::vector<int>& b,
                        size_t from, size_t to) {
  size_t coded_end = from;
  while (coded_end < to && (b[coded_end] == kLost || b[coded_end] >= kSpace ||
                            b[coded_end] == kCr || b[coded_end] == kLf)) {
    ++coded_end;
  }
  const std::vector<int> text =
      coded_end > from &&
              reads_as_4for3(b, from, coded_end, k4for3UnframedMinPercent)
          ? nabts_decode_4for3_run(without_line_ends(b, from, coded_end))
          : std::vector<int>{};
  if (!reads_as_text(text)) {
    return false;
  }
  message.form = "ANPA 1312, 4-for-3 coded, its start not received";
  const size_t etx = find_byte(text, kEtx, 0, text.size());
  message.text = as_text(text, 0, etx, true);
  message.complete =
      etx < text.size() || (coded_end < to && b[coded_end] == kEtx);
  message.title = first_line(message.text);
  return true;
}

/// Text with no framing of its own, dated where its first line is a date.
void read_plain_text(NabtsNetworkMessage& message, const std::vector<int>& b,
                     size_t from, size_t to) {
  message.form = "text";
  message.text = as_text(b, from, to, false);
  const std::vector<std::string> lines = lines_of(message.text);
  if (!lines.empty() && is_date_line(lines.front())) {
    message.fields.emplace_back("Sent", lines.front());
  }
  message.title = first_line(message.text);
  message.complete = message.lost_bytes == 0;
}

/// One item of a service, read by its framing.
NabtsNetworkMessage read_item(const Span& span,
                              const std::vector<Entry>& entries,
                              const std::vector<size_t>& header,
                              ServiceKey key) {
  const std::vector<int>& b = span.bytes;
  // Leading line ends, spaces and padding are not the item.
  size_t from = 0;
  while (from < b.size() &&
         (is_padding(b[from]) || b[from] == kLf || b[from] == kCr ||
          b[from] == kSpace || b[from] == kSyn)) {
    ++from;
  }
  const size_t to = b.size();
  const size_t first_index = *span.owners.begin();
  const Entry& first = entries[first_index];
  NabtsNetworkMessage message;
  message.channel = key.first;
  message.record_address = fmt::format("{:03X}", key.second);
  message.kind = kind_label(first.nibbles[0]);
  message.sequence = first.nibbles[kSequenceNibble];
  message.addressing = addressing_of(first.nibbles, header[first_index]);
  message.groups = static_cast<uint32_t>(span.owners.size());
  message.copies = span.copies;
  message.first_frame = first.frame;
  message.last_frame = entries[*span.owners.rbegin()].frame;
  message.lost_bytes =
      static_cast<uint32_t>(std::count(b.begin(), b.end(), kLost));
  message.damaged_bytes =
      static_cast<uint32_t>(std::count_if(b.begin(), b.end(), is_damaged));
  message.complete = !b.empty() && (b.back() == kEot);

  const uint8_t kind = first.nibbles[0];
  if (kind == kNabtsNetworkPage || kind == kNabtsNetworkPageWithText) {
    read_page(message, span, from, to);
  } else if (from < to && b[from] == kSoh) {
    if (!read_coded(message, b, from + 1, to)) {
      read_framed(message, b, from + 1, to);
    }
  } else if (!read_coded_remnant(message, b, from, to)) {
    read_plain_text(message, b, from, to);
  }
  return message;
}

}  // namespace

std::vector<NabtsNetworkMessage> NabtsNetworkMessageLog::messages() const {
  const HeaderEnds headers = header_ends(entries_);
  std::vector<NabtsNetworkMessage> out;
  for (const auto& [key, indices] : headers.by_service) {
    if (std::optional<NabtsNetworkMessage> status =
            status_signal(entries_, indices, headers.end, key)) {
      out.push_back(std::move(*status));
      continue;
    }
    const ServiceStream stream =
        service_stream(entries_, indices, headers, key);
    for (const Span& span : merge_copies(cut_items(stream))) {
      out.push_back(read_item(span, entries_, headers.end, key));
    }
  }
  return out;
}

}  // namespace orc
