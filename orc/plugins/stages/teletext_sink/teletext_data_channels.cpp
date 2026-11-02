/*
 * File:        teletext_data_channels.cpp
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     The data a teletext service sent outside its pages: broadcast
 *              service data and independent data lines
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "teletext_data_channels.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <utility>

#include "vbi-services/teletext_page_decoder.h"

namespace orc {

namespace {

// The packet numbers that carry data rather than page rows (EN 300 706 §9.7).
constexpr int kServiceDataPacket = 30;
constexpr int kDataLinePacket = 31;
// MJD of 1970-01-01, the Unix epoch.
constexpr int64_t kMjdUnixEpoch = 40587;
// 1980 to 2040, in seconds since 1970: a broadcast date outside is a
// misreading.
constexpr int64_t kEarliestBroadcastTime = 315532800;
constexpr int64_t kLatestBroadcastTime = 2208988800;
// Two readings of the time this close agree: a damaged one agrees with
// neither neighbour.
constexpr int64_t kTimeAgreementSeconds = 120;
// Distinct readings of a service data field held, past which a new one is
// not kept: damaged packets each read differently.
constexpr size_t kServiceDataMaxReadings = 256;
// Identical bytes after which an IDL Format A user data group holds a dummy
// byte (EN 300 708 §6.5.7.1).
constexpr int kIdlDummyRun = 8;
// How far behind the newest packet a Datavideo repeat may come (§6.6.2.1),
// and the continuity index's modulus.
constexpr size_t kDatavideoRepeatWindow = 32;
constexpr int kDatavideoContinuityModulus = 64;
// Datavideo's bytes 11 to 45, and the sequence a masked packet is sent
// exclusive-ored with (§6.6.3).
constexpr size_t kDatavideoBodyBytes = 35;
constexpr std::array<uint8_t, kDatavideoBodyBytes> kDatavideoMask = {
    0xAF, 0xAA, 0x81, 0x4A, 0xF2, 0xEE, 0x07, 0x3A, 0x4F, 0x5D, 0x44, 0x86,
    0x70, 0x9D, 0x83, 0x43, 0xBC, 0x3F, 0xE0, 0xF7, 0xC5, 0xCC, 0x82, 0x53,
    0xB4, 0x79, 0xF3, 0x62, 0xA4, 0x71, 0xB5, 0x71, 0x31, 0x10, 0x08};
// GF(2^8) of the Format B code: x^8 + x^4 + x^3 + x^2 + 1 (§6.8.2.3).
constexpr unsigned kFormatBFieldPolynomial = 0x11D;

uint8_t reversed(uint8_t byte) {
  uint8_t out = 0;
  for (int bit = 0; bit < 8; ++bit) {
    out = static_cast<uint8_t>((out << 1) | ((byte >> bit) & 1));
  }
  return out;
}

/// Hamming 8/4 decoded, or -1 where |byte| is not a codeword.
int exact(uint8_t byte) {
  const int value = teletext_hamming84_decode(byte);
  return value >= 0 &&
                 teletext_hamming84_encode(static_cast<uint8_t>(value)) == byte
             ? value
             : -1;
}

/// A digit sent one up (§9.8.1 MJD and UTC), or -1.
int digit(unsigned nibble) {
  return nibble >= 1 && nibble <= 10 ? static_cast<int>(nibble) - 1 : -1;
}

/// A page address coded as bytes 7 to 12 of packet 8/30 (§9.8.1 Table 18):
/// units, tens, S1, S2 with M1, S3, S4 with M2 and M3, Hamming 8/4
/// throughout. Empty where it does not decode or names no page.
std::string page_address(const std::array<uint8_t, kTeletextPacketBytes>& p,
                         size_t at) {
  int nibble[6];
  for (int i = 0; i < 6; ++i) {
    nibble[i] = teletext_hamming84_decode(p[at + static_cast<size_t>(i)]);
    if (nibble[i] < 0) {
      return {};
    }
  }
  const int page = (nibble[1] << 4) | nibble[0];
  if (page == 0xFF) {
    return {};  // §9.8.1 NOTE 2: no page specified
  }
  const int magazine = ((nibble[3] >> 3) & 1) | (((nibble[5] >> 2) & 1) << 1) |
                       (((nibble[5] >> 3) & 1) << 2);
  const int subcode = nibble[2] | ((nibble[3] & 0x7) << 4) | (nibble[4] << 8) |
                      ((nibble[5] & 0x3) << 12);
  std::string out = fmt::format("{}{:02X}", magazine == 0 ? 8 : magazine, page);
  if (subcode != 0 && subcode != 0x3F7F) {
    out += fmt::format(":{:04X}", subcode);
  }
  return out;
}

/// Bytes |from| to |to| as the status display's characters (odd parity, the
/// default G0 set's common characters), a byte failing its parity as '?',
/// trailing spaces trimmed.
std::string status_text(const std::array<uint8_t, kTeletextPacketBytes>& p,
                        size_t from, size_t to) {
  std::string out;
  for (size_t i = from; i < to; ++i) {
    const uint8_t byte = p[i];
    const auto code = static_cast<char>(byte & 0x7F);
    out.push_back(!teletext_odd_parity_valid(byte) ? '?'
                  : (code >= 0x20 && code < 0x7F)  ? code
                                                   : ' ');
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  const size_t lead = out.find_first_not_of(' ');
  return lead == std::string::npos ? std::string{} : out.substr(lead);
}

/// The programme label of packet 8/30 Format 2, bytes 13 to 25 (EN 300 231
/// §8.2.1 Tables 7 and 8, the parameters as §6.2 codes them), as text; empty
/// where a byte does not decode.
std::string programme_label(
    const std::array<uint8_t, kTeletextPacketBytes>& p) {
  int nibble[13];
  for (size_t i = 0; i < 13; ++i) {
    nibble[i] = teletext_hamming84_decode(p[9 + i]);
    if (nibble[i] < 0) {
      return {};
    }
  }
  // Message bit |at| of byte 13 + |byte|.
  const auto bit = [&nibble](int byte, int at) {
    return (nibble[byte] >> at) & 1;
  };
  // A parameter's bits b1 to bn, b1 the most significant (§6.2).
  const auto field = [&bit](std::initializer_list<std::pair<int, int>> bits) {
    int value = 0;
    for (const auto& [byte, at] : bits) {
      value = (value << 1) | bit(byte, at);
    }
    return value;
  };
  const int label_channel = field({{0, 0}, {0, 1}});
  const int sound = field({{1, 0}, {1, 1}});
  // The CNI: bits b1 to b8 name the country, b9 to b16 the network (§6.2 a).
  const int country =
      field({{2, 0}, {2, 1}, {2, 2}, {2, 3}, {8, 2}, {8, 3}, {9, 0}, {9, 1}});
  const int provider = field(
      {{3, 0}, {3, 1}, {9, 2}, {9, 3}, {10, 0}, {10, 1}, {10, 2}, {10, 3}});
  const int network = (country << 8) | provider;
  const int day = field({{3, 2}, {3, 3}, {4, 0}, {4, 1}, {4, 2}});
  const int month = field({{4, 3}, {5, 0}, {5, 1}, {5, 2}});
  const int hour = field({{5, 3}, {6, 0}, {6, 1}, {6, 2}, {6, 3}});
  const int minute = field({{7, 0}, {7, 1}, {7, 2}, {7, 3}, {8, 0}, {8, 1}});
  const int type = field(
      {{11, 0}, {11, 1}, {11, 2}, {11, 3}, {12, 0}, {12, 1}, {12, 2}, {12, 3}});

  // §6.2 e): the programme identification label's service codes.
  std::string programme;
  if (day == 0 && month == 15 && minute == 63 && hour >= 28) {
    static const char* const kServiceCodes[] = {
        "continuation", "interruption", "recording inhibited", "timer control"};
    programme = kServiceCodes[hour - 28];
  } else if (day == 31 && month == 15 && hour == 31 && minute == 63) {
    programme = "none (chosen by programme type)";
  } else if (day >= 1 && month >= 1 && month <= 12 && hour <= 23 &&
             minute <= 59) {
    programme =
        fmt::format("{:02}-{:02} {:02}:{:02}", day, month, hour, minute);
  } else {
    programme = fmt::format("0x{:05X}",
                            (day << 15) | (month << 11) | (hour << 6) | minute);
  }
  static const char* const kSound[] = {"sound unknown", "mono", "stereo",
                                       "dual sound"};
  std::string out =
      fmt::format("label channel {}: network 0x{:04X}, programme {}, {}",
                  label_channel, network, programme, kSound[sound]);
  if (type != 0xFF) {
    out += fmt::format(", programme type 0x{:02X}", type);
  }
  if (bit(0, 2) != 0) {
    out += ", label update";
  }
  if (bit(0, 3) != 0) {
    out += ", prepare to record";
  }
  if (bit(1, 2) != 0) {
    out += ", mode identifier set";
  }
  return out;
}

template <typename Map>
std::string commonest(const Map& counts) {
  std::string best;
  uint64_t most = 0;
  for (const auto& [value, count] : counts) {
    if (count > most) {
      most = count;
      best = value;
    }
  }
  return best;
}

/// Count |value| in |counts|, a new one only while there is room.
template <typename Map, typename Key>
void tally(Map& counts, const Key& value) {
  auto it = counts.find(value);
  if (it != counts.end()) {
    ++it->second;
  } else if (counts.size() < kServiceDataMaxReadings) {
    counts.emplace(value, 1);
  }
}

std::string utc_text(int64_t seconds) {
  // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
  int64_t days = seconds >= 0 ? seconds / 86400 : (seconds - 86399) / 86400;
  const int64_t rest = seconds - days * 86400;
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const int64_t doe = days - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int64_t year = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t day = doy - (153 * mp + 2) / 5 + 1;
  const int64_t month = mp < 10 ? mp + 3 : mp - 9;
  year += month <= 2 ? 1 : 0;
  return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC", year, month,
                     day, rest / 3600, (rest / 60) % 60, rest % 60);
}

/// GF(2^8) as the Format B code builds it, alpha = 2.
struct GaloisField {
  std::array<uint8_t, 512> exp{};
  std::array<int, 256> log{};

  GaloisField() {
    unsigned x = 1;
    for (int i = 0; i < 255; ++i) {
      exp[static_cast<size_t>(i)] = static_cast<uint8_t>(x);
      log[x] = i;
      x <<= 1U;
      if ((x & 0x100U) != 0) {
        x ^= kFormatBFieldPolynomial;
      }
    }
    for (size_t i = 255; i < exp.size(); ++i) {
      exp[i] = exp[i - 255];
    }
    log[0] = -1;
  }
  uint8_t mul(uint8_t a, uint8_t b) const {
    if (a == 0 || b == 0) {
      return 0;
    }
    const int index = log[a] + log[b];
    return exp[static_cast<size_t>(index)];
  }
  uint8_t div(uint8_t a, uint8_t b) const {
    if (a == 0) {
      return 0;
    }
    const int index = log[a] - log[b] + 255;
    return exp[static_cast<size_t>(index)];
  }
  uint8_t power(int e) const { return exp[static_cast<size_t>(e % 255)]; }
};

const GaloisField& galois_field() {
  static const GaloisField kField;
  return kField;
}

/// The two syndromes of |n| values |stride| apart under the Format B code,
/// the i-th weighted alpha^(n-1-i) (EN 300 708 §6.8.2.5).
std::pair<uint8_t, uint8_t> syndromes(const uint8_t* values, size_t n,
                                      size_t stride) {
  const GaloisField& gf = galois_field();
  uint8_t x = 0;
  uint8_t y = 0;
  for (size_t i = 0; i < n; ++i) {
    const uint8_t value = values[i * stride];
    x ^= value;
    y = static_cast<uint8_t>(gf.mul(y, 2) ^ value);
  }
  return {x, y};
}

/// Correct a single byte error in |n| values |stride| apart; false where
/// they hold more than one. |corrected| counts the byte put right.
bool correct_single(uint8_t* values, size_t n, size_t stride,
                    uint64_t& corrected) {
  const auto [x, y] = syndromes(values, n, stride);
  if (x == 0 && y == 0) {
    return true;
  }
  if (x == 0 || y == 0) {
    return false;
  }
  const GaloisField& gf = galois_field();
  // X = E and Y = alpha^(n-1-p) E: the error's place from their ratio.
  const int distance = (gf.log[y] - gf.log[x] + 255) % 255;
  if (static_cast<size_t>(distance) >= n) {
    return false;
  }
  values[(n - 1 - static_cast<size_t>(distance)) * stride] ^= x;
  ++corrected;
  return true;
}

}  // namespace

uint16_t teletext_data_line_crc_register(const uint8_t* bytes, size_t count,
                                         uint16_t polynomial) {
  uint16_t reg = 0;
  for (size_t i = 0; i < count; ++i) {
    for (int bit = 0; bit < 8; ++bit) {
      const unsigned in = (bytes[i] >> bit) & 1U;
      const unsigned feedback = ((reg >> 15) & 1U) ^ in;
      reg = static_cast<uint16_t>(reg << 1U);
      if (feedback != 0) {
        reg ^= polynomial;
      }
    }
  }
  return reg;
}

void TeletextDataChannels::add_packet(const Packet& packet,
                                      size_t packet_bytes) {
  // The address must arrive as codewords: a corrected one is as likely
  // another packet's (TeletextPageDecoder's address_attested()).
  const int low = exact(packet[0]);
  const int high = exact(packet[1]);
  if (low < 0 || high < 0) {
    return;
  }
  const int number = ((low >> 3) & 1) | (high << 1);
  if (number != kServiceDataPacket && number != kDataLinePacket) {
    return;
  }
  // EN 300 708 §6.4.2: the first address byte's four bits number the data
  // channel — the magazine, and whether the packet is 30 or 31 — and Table 16
  // says how each channel is coded.
  const int channel = low;
  const int format = teletext_hamming84_decode(packet[2]);
  switch (channel) {
    case 0:
      if (format >= 0 && format <= 3) {
        add_service_data(packet, packet_bytes, format);
      } else {
        ++refused_;
      }
      return;
    case 4:
    case 12:
      add_audio(packet, packet_bytes, channel);
      return;
    case 5:
    case 6:
    case 13:
    case 14:
      add_datavideo(packet, packet_bytes, channel);
      return;
    case 8:
    case 9:
    case 10:
    case 11:
    case 15:
      // Table 22: the format type's first bit clear is Format A, which
      // channel 15 does not carry; the first set and the second clear is
      // Format B; both set is reserved.
      if (format >= 0 && (format & 1) == 0 && channel != 15) {
        add_format_a(packet, packet_bytes, channel);
      } else if (format >= 0 && (format & 3) == 1) {
        add_format_b(packet, packet_bytes, channel, format);
      } else {
        ++refused_;
      }
      return;
    default:
      ++unassigned_;
      return;
  }
}

void TeletextDataChannels::add_service_data(const Packet& packet,
                                            size_t packet_bytes,
                                            int designation) {
  const bool format1 = designation <= 1;
  (format1 ? service_.format1_packets : service_.format2_packets) += 1;
  if ((designation & 1) == 0) {
    ++multiplexed_packets_;
  }
  service_.multiplexed = multiplexed_packets_ * 2 >
                         service_.format1_packets + service_.format2_packets;
  const std::string page = page_address(packet, 3);
  if (!page.empty()) {
    tally(initial_pages_, page);
    service_.initial_page = commonest(initial_pages_);
  }
  // Bytes 26 to 45, as far as the service's packet runs.
  const size_t status_end = std::min<size_t>(packet_bytes, 42);
  if (status_end > 22) {
    const std::string status = status_text(packet, 22, status_end);
    if (!status.empty()) {
      tally(status_displays_, status);
      service_.status_display = commonest(status_displays_);
    }
  }
  if (!format1) {
    const std::string label = programme_label(packet);
    if (!label.empty()) {
      tally(service_.programme_labels, label);
    }
    return;
  }
  // Network Identification: sent most significant bit first (§9.8.1), so
  // each byte as received holds its bits reversed.
  const auto network =
      static_cast<uint16_t>((reversed(packet[9]) << 8) | reversed(packet[10]));
  tally(service_.network_ids, network);
  // Time offset: bits 2-6 half hours, bit 7 the sign.
  const uint8_t offset = packet[11];
  const int half_hours = (offset >> 1) & 0x1F;
  const int offset_minutes =
      ((offset >> 6) & 1) != 0 ? -half_hours * 30 : half_hours * 30;
  // MJD and UTC: each digit one up, a nibble apiece (§9.8.1 Table 18).
  const int mjd_digits[5] = {digit(packet[12] & 0xFU), digit(packet[13] >> 4U),
                             digit(packet[13] & 0xFU), digit(packet[14] >> 4U),
                             digit(packet[14] & 0xFU)};
  const int utc_digits[6] = {digit(packet[15] >> 4U), digit(packet[15] & 0xFU),
                             digit(packet[16] >> 4U), digit(packet[16] & 0xFU),
                             digit(packet[17] >> 4U), digit(packet[17] & 0xFU)};
  int64_t mjd = 0;
  for (const int d : mjd_digits) {
    if (d < 0) {
      return;
    }
    mjd = mjd * 10 + d;
  }
  for (const int d : utc_digits) {
    if (d < 0) {
      return;
    }
  }
  const int hours = utc_digits[0] * 10 + utc_digits[1];
  const int minutes = utc_digits[2] * 10 + utc_digits[3];
  const int seconds = utc_digits[4] * 10 + utc_digits[5];
  if (hours > 23 || minutes > 59 || seconds > 59) {
    return;
  }
  const int64_t utc = (mjd - kMjdUnixEpoch) * 86400 + int64_t{hours} * 3600 +
                      int64_t{minutes} * 60 + seconds;
  if (utc < kEarliestBroadcastTime || utc > kLatestBroadcastTime) {
    return;
  }
  // A time is believed once the next packet's agrees with it, so a damaged
  // reading — which agrees with neither neighbour — never widens the span.
  const std::optional<int64_t> previous = last_time_read_;
  last_time_read_ = utc;
  if (!previous || std::llabs(utc - *previous) > kTimeAgreementSeconds) {
    return;
  }
  for (const int64_t time : {*previous, utc}) {
    if (!service_.first_utc || time < *service_.first_utc) {
      service_.first_utc = time;
    }
    if (!service_.last_utc || time > *service_.last_utc) {
      service_.last_utc = time;
    }
  }
  service_.offset_minutes = offset_minutes;
}

TeletextDataChannels::Held* TeletextDataChannels::stream_for(
    int channel, TeletextDataFormat format, uint32_t address,
    int address_digits) {
  const auto key = std::make_tuple(channel, static_cast<int>(format), address);
  auto it = streams_.find(key);
  if (it == streams_.end()) {
    if (streams_.size() >= kTeletextDataChannelMaxStreams) {
      return nullptr;
    }
    it = streams_.emplace(key, Held{}).first;
    it->second.stream.channel = channel;
    it->second.stream.format = format;
    it->second.stream.address = address;
    it->second.stream.address_digits = address_digits;
  }
  return &it->second;
}

void TeletextDataChannels::keep(TeletextDataStream& stream,
                                const uint8_t* bytes, size_t count,
                                size_t& held_bytes) {
  const size_t kept =
      std::min(count, kTeletextDataChannelMaxBytes - held_bytes);
  stream.payload.insert(stream.payload.end(), bytes, bytes + kept);
  held_bytes += kept;
}

void TeletextDataChannels::add_format_a(const Packet& packet,
                                        size_t packet_bytes, int channel) {
  const int format = teletext_hamming84_decode(packet[2]);
  const int ial = teletext_hamming84_decode(packet[3]);
  if (ial < 0 || (ial & 7) == 7 || packet_bytes < 8) {
    ++refused_;
    return;
  }
  // The service packet address, its first byte the least significant digit
  // (§6.5.3: an address's less significant bytes may be an address of their
  // own).
  const int digits = ial & 7;
  size_t at = 4;
  uint32_t address = 0;
  for (int i = 0; i < digits; ++i, ++at) {
    const int nibble = teletext_hamming84_decode(packet[at]);
    if (nibble < 0) {
      ++refused_;
      return;
    }
    address |= static_cast<uint32_t>(nibble) << (4 * i);
  }
  const bool has_repeat = (format & 2) != 0;
  const bool has_continuity = (format & 4) != 0;
  const bool has_length = (format & 8) != 0;
  // The repeat indicator (§6.5.4) says no more than the continuity
  // indicator does about which packet is new, so it is passed over.
  if (has_repeat) {
    ++at;
  }
  // The check word covers what follows the repeat indicator (§6.5.8).
  const size_t checked_from = at;
  std::optional<uint8_t> continuity;
  if (has_continuity) {
    continuity = packet[at++];
  }
  size_t length = packet_bytes > at + 2 ? packet_bytes - 2 - at : 0;
  if (has_length) {
    length = std::min<size_t>(length, packet[at++] & 0x3FU);
    length = std::min<size_t>(
        length, packet_bytes > at + 2 ? packet_bytes - 2 - at : 0);
  }
  if (checked_from + 2 > packet_bytes) {
    ++refused_;
    return;
  }
  const uint16_t reg = teletext_data_line_crc_register(
      &packet[checked_from], packet_bytes - checked_from,
      kTeletextIdlFormatAPolynomial);
  bool checked = false;
  if (has_continuity) {
    if (reg != 0) {
      ++refused_;
      return;
    }
    checked = true;
  } else {
    // An implicit continuity indicator: the register holds it twice, least
    // significant bit at the output end (§6.5.8.2).
    if ((reg >> 8) != (reg & 0xFFU)) {
      ++refused_;
      return;
    }
    continuity = reversed(static_cast<uint8_t>(reg & 0xFFU));
  }

  Held* held =
      stream_for(channel, TeletextDataFormat::kIdlFormatA, address, digits);
  if (held == nullptr) {
    ++refused_;
    return;
  }
  TeletextDataStream& stream = held->stream;
  ++stream.packets;
  stream.packets_checked += checked ? 1 : 0;
  // A packet whose continuity indicator is the last one's is a repeat of it
  // (§6.5.5: not incremented on repeats).
  if (held->last_continuity && *continuity == *held->last_continuity) {
    ++stream.repeats;
    return;
  }
  if (held->last_continuity) {
    stream.gaps +=
        static_cast<uint8_t>(*continuity - *held->last_continuity - 1U);
  }
  held->last_continuity = *continuity;

  // The user data, dummy bytes dropped (§6.5.7.1): after eight identical
  // bytes of all 0s or all 1s, the continuity indicator counting as the
  // first, the next byte is a dummy.
  std::vector<uint8_t> data;
  data.reserve(length);
  int run = 0;
  uint8_t run_byte = 0;
  if (has_continuity && (*continuity == 0x00 || *continuity == 0xFF)) {
    run = 1;
    run_byte = *continuity;
  }
  for (size_t i = 0; i < length; ++i) {
    const uint8_t byte = packet[at + i];
    if (run == kIdlDummyRun) {
      run = 0;
      continue;  // the dummy byte
    }
    if (byte == 0x00 || byte == 0xFF) {
      if (run > 0 && byte == run_byte) {
        ++run;
      } else {
        run_byte = byte;
        run = 1;
      }
    } else {
      run = 0;
    }
    data.push_back(byte);
  }
  keep(stream, data.data(), data.size(), held_bytes_);
}

void TeletextDataChannels::add_format_b(const Packet& packet,
                                        size_t packet_bytes, int channel,
                                        int format) {
  // Bytes 9 to 43 of user data and the two suffix bytes at 44 and 45: a
  // 625-line packet's full length (§6.8.1).
  const int application = teletext_hamming84_decode(packet[3]);
  const int continuity = teletext_hamming84_decode(packet[4]);
  if (packet_bytes < kTeletextPacketBytes || application < 0 ||
      continuity < 0) {
    ++refused_;
    return;
  }
  BundleRow row;
  std::copy_n(packet.begin() + 5, kBundleColumns, row.bytes.begin());
  row.present = true;
  uint64_t corrected = 0;
  row.clean = correct_single(row.bytes.data(), kBundleColumns, 1, corrected);

  // The application number (Table 22) and identifier name the service.
  const auto address =
      static_cast<uint32_t>(application | ((format >> 2) << 4));
  Held* held = stream_for(channel, TeletextDataFormat::kIdlFormatB, address, 0);
  if (held == nullptr) {
    ++refused_;
    return;
  }
  TeletextDataStream& stream = held->stream;
  ++stream.packets;
  stream.packets_checked += row.clean && corrected == 0 ? 1 : 0;
  BundleRow& slot = held->bundle[static_cast<size_t>(continuity)];
  // §6.8.1.3: the index is not incremented on a repeat, which may stand in
  // for a copy that arrived damaged.
  if (held->last_continuity && continuity == *held->last_continuity) {
    ++stream.repeats;
    if (row.clean && !slot.clean) {
      slot = row;
      stream.bytes_corrected += corrected;
    }
    return;
  }
  // A bundle runs from index 0 to 15: an index no later than the last
  // starts the next one.
  if (held->last_continuity) {
    const auto last = static_cast<uint64_t>(*held->last_continuity);
    if (continuity <= *held->last_continuity) {
      decode_bundle(stream, held->bundle, held_bytes_);
      held->bundle = Bundle{};
      stream.gaps +=
          (kBundleRows - 1 - last) + static_cast<uint64_t>(continuity);
    } else {
      stream.gaps += static_cast<uint64_t>(continuity) - last - 1;
    }
  }
  held->last_continuity = continuity;
  held->bundle[static_cast<size_t>(continuity)] = row;
  stream.bytes_corrected += corrected;
}

void TeletextDataChannels::decode_bundle(TeletextDataStream& stream,
                                         Bundle bundle, size_t& held_bytes) {
  std::vector<size_t> erased;
  bool any = false;
  for (size_t r = 0; r < kBundleRows; ++r) {
    any = any || bundle[r].present;
    if (!bundle[r].present || !bundle[r].clean) {
      erased.push_back(r);
    }
  }
  if (!any) {
    return;
  }
  // The vertical code (§6.8.2.4): each column of the sixteen packets is a
  // codeword of the same kind, so packets lost or past the horizontal
  // code's reach are restored from the others — two at most, their places
  // known.
  const GaloisField& gf = galois_field();
  if (erased.size() <= 2) {
    for (size_t c = 0; c < kBundleColumns; ++c) {
      std::array<uint8_t, kBundleRows> column{};
      for (size_t r = 0; r < kBundleRows; ++r) {
        column[r] = bundle[r].bytes[c];
      }
      if (erased.empty()) {
        if (correct_single(column.data(), kBundleRows, 1,
                           stream.bytes_corrected)) {
          for (size_t r = 0; r < kBundleRows; ++r) {
            bundle[r].bytes[c] = column[r];
          }
        }
        continue;
      }
      for (const size_t r : erased) {
        column[r] = 0;
      }
      const auto [x, y] = syndromes(column.data(), kBundleRows, 1);
      const size_t e1 = erased[0];
      uint8_t v1 = x;
      if (erased.size() == 2) {
        // R1 + R2 = X and w1 R1 + w2 R2 = Y, w the rows' weights.
        const size_t e2 = erased[1];
        const uint8_t w1 = gf.power(static_cast<int>(kBundleRows - 1 - e1));
        const uint8_t w2 = gf.power(static_cast<int>(kBundleRows - 1 - e2));
        v1 = gf.div(static_cast<uint8_t>(y ^ gf.mul(w2, x)),
                    static_cast<uint8_t>(w1 ^ w2));
        bundle[e2].bytes[c] = static_cast<uint8_t>(x ^ v1);
      }
      bundle[e1].bytes[c] = v1;
    }
    for (const size_t r : erased) {
      bundle[r].present = true;
      bundle[r].clean = true;
      if (r < kBundleDataRows) {
        ++stream.rows_restored;
      }
    }
  }
  for (size_t r = 0; r < kBundleDataRows; ++r) {
    if (!bundle[r].present) {
      ++stream.rows_lost;
      continue;
    }
    if (!bundle[r].clean) {
      ++stream.rows_damaged;
    }
    keep(stream, bundle[r].bytes.data(), kBundleColumns - 2, held_bytes);
  }
}

void TeletextDataChannels::add_datavideo(const Packet& packet,
                                         size_t packet_bytes, int channel) {
  // Bytes 6 to 10, Hamming 8/4: the packet address (12 bits, taken least
  // significant first as the rest of the packet is sent), the continuity
  // index (6 bits) and the masking and packet type bits (§6.6.1-6.6.4).
  if (packet_bytes < kTeletextPacketBytes) {
    ++refused_;
    return;
  }
  int nibble[5];
  for (size_t i = 0; i < 5; ++i) {
    nibble[i] = teletext_hamming84_decode(packet[2 + i]);
    if (nibble[i] < 0) {
      ++refused_;
      return;
    }
  }
  const auto address =
      static_cast<uint32_t>(nibble[0] | (nibble[1] << 4) | (nibble[2] << 8));
  const int continuity = nibble[3] | ((nibble[4] & 3) << 4);
  const bool masked = (nibble[4] & 4) != 0;
  const bool typed = (nibble[4] & 8) != 0;
  std::array<uint8_t, kDatavideoBodyBytes> body{};
  for (size_t i = 0; i < kDatavideoBodyBytes; ++i) {
    body[i] =
        static_cast<uint8_t>(packet[7 + i] ^ (masked ? kDatavideoMask[i] : 0));
  }
  // §6.6.6: the check word is sent complemented, and the check is that the
  // register returns to zero; either reading of that is accepted.
  bool holds = teletext_data_line_crc_register(
                   body.data(), body.size(), kTeletextDatavideoPolynomial) == 0;
  if (!holds) {
    std::array<uint8_t, kDatavideoBodyBytes> restored = body;
    restored[kDatavideoBodyBytes - 2] ^= 0xFF;
    restored[kDatavideoBodyBytes - 1] ^= 0xFF;
    holds = teletext_data_line_crc_register(restored.data(), restored.size(),
                                            kTeletextDatavideoPolynomial) == 0;
  }
  if (!holds) {
    ++refused_;
    return;
  }
  Held* held = stream_for(channel, TeletextDataFormat::kDatavideo, address, 3);
  if (held == nullptr) {
    ++refused_;
    return;
  }
  TeletextDataStream& stream = held->stream;
  ++stream.packets;
  stream.masked_packets += masked ? 1 : 0;
  stream.typed_packets += typed ? 1 : 0;
  // A repeat may come after as many as 32 newer packets (§6.6.2.1); one
  // whose original was lost comes too late to be put in its place.
  if (std::find(held->recent.begin(), held->recent.end(), continuity) !=
      held->recent.end()) {
    ++stream.repeats;
    return;
  }
  if (held->last_continuity) {
    const int step =
        (continuity - *held->last_continuity + kDatavideoContinuityModulus) %
        kDatavideoContinuityModulus;
    if (step > kDatavideoContinuityModulus -
                   static_cast<int>(kDatavideoRepeatWindow)) {
      ++stream.repeats;  // behind the newest: a late repeat
      return;
    }
    stream.gaps += static_cast<uint64_t>(step - 1);
  }
  held->last_continuity = continuity;
  held->recent.push_back(continuity);
  if (held->recent.size() > kDatavideoRepeatWindow) {
    held->recent.erase(held->recent.begin());
  }
  keep(stream, body.data(), kDatavideoBodyBytes - 2, held_bytes_);
}

void TeletextDataChannels::add_audio(const Packet& packet, size_t packet_bytes,
                                     int channel) {
  // No check word guards an audio packet (§6.7.1), so only one whose service
  // byte arrived as a codeword is believed.
  const int service = exact(packet[2]);
  if (service < 0 || packet_bytes <= 4) {
    ++refused_;
    return;
  }
  Held* held = stream_for(channel, TeletextDataFormat::kAudio,
                          static_cast<uint32_t>(service), 1);
  if (held == nullptr) {
    ++refused_;
    return;
  }
  TeletextDataStream& stream = held->stream;
  ++stream.packets;
  // Channel 4's control byte is the Hamming 8/4 fade byte (§6.7.2.2);
  // channel 12's is the application's (§6.7.3.2).
  const int control =
      channel == 4 ? teletext_hamming84_decode(packet[3]) : packet[3];
  tally(stream.controls, control);
  keep(stream, &packet[4], packet_bytes - 4, held_bytes_);
}

std::vector<TeletextDataStream> TeletextDataChannels::streams() const {
  std::vector<TeletextDataStream> out;
  size_t held_bytes = held_bytes_;
  for (const auto& entry : streams_) {
    if (!entry.second.stream.believed()) {
      continue;
    }
    TeletextDataStream stream = entry.second.stream;
    if (stream.format == TeletextDataFormat::kIdlFormatB) {
      decode_bundle(stream, entry.second.bundle, held_bytes);
    }
    out.push_back(std::move(stream));
  }
  return out;
}

uint64_t TeletextDataChannels::packets_refused() const {
  uint64_t refused = refused_;
  if (!service_.any()) {
    refused += service_.format1_packets + service_.format2_packets;
  }
  for (const auto& entry : streams_) {
    if (!entry.second.stream.believed()) {
      refused += entry.second.stream.packets;
    }
  }
  return refused;
}

bool TeletextDataStream::believed() const {
  switch (format) {
    case TeletextDataFormat::kIdlFormatA:
      return packets_checked >= 1 || packets >= kTeletextCheckedMinPackets;
    case TeletextDataFormat::kIdlFormatB:
      return packets_checked >= kTeletextCheckedMinPackets;
    case TeletextDataFormat::kDatavideo:
      return packets >= 1;
    case TeletextDataFormat::kAudio:
      return packets >= kTeletextAudioMinPackets;
  }
  return false;
}

std::vector<std::string> TeletextServiceData::summary() const {
  std::vector<std::string> lines;
  if (!any()) {
    return lines;
  }
  lines.push_back(fmt::format(
      "packet 8/30: {} of Format 1, {} of Format 2; sent {}", format1_packets,
      format2_packets,
      multiplexed ? "within a video signal" : "as full-field data"));
  if (!initial_page.empty()) {
    lines.push_back("initial page " + initial_page);
  }
  if (!network_ids.empty()) {
    // The code has no protection of its own (§9.8.1: "coded 8 bits data"),
    // so a damaged packet reads as some other: the commonest reading is the
    // network's.
    uint16_t code = 0;
    uint64_t most = 0;
    uint64_t total = 0;
    for (const auto& [value, count] : network_ids) {
      total += count;
      if (count > most) {
        most = count;
        code = value;
      }
    }
    std::string line =
        fmt::format("network identification 0x{:04X} ({} packet{})", code, most,
                    most == 1 ? "" : "s");
    if (total > most) {
      line += fmt::format("; {} packet{} read otherwise", total - most,
                          total - most == 1 ? "" : "s");
    }
    lines.push_back(std::move(line));
  }
  if (first_utc && last_utc) {
    lines.push_back(fmt::format(
        "time {} to {}, local time UTC{}{}:{:02}", utc_text(*first_utc),
        utc_text(*last_utc), offset_minutes < 0 ? "-" : "+",
        std::abs(offset_minutes) / 60, std::abs(offset_minutes) % 60));
  }
  if (!status_display.empty()) {
    lines.push_back("status display \"" + status_display + "\"");
  }
  // The labels by how often they were read, as many as a reader needs: a
  // service sends a few at a time (§8.3), and a damaged packet's reading is
  // one of the rarest.
  std::vector<std::pair<uint64_t, std::string>> labels;
  for (const auto& [label, count] : programme_labels) {
    labels.emplace_back(count, label);
  }
  std::stable_sort(
      labels.begin(), labels.end(),
      [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
  constexpr size_t kLabelsListed = 16;
  for (size_t i = 0; i < labels.size() && i < kLabelsListed; ++i) {
    lines.push_back(fmt::format("programme label, {} ({} packet{})",
                                labels[i].second, labels[i].first,
                                labels[i].first == 1 ? "" : "s"));
  }
  if (labels.size() > kLabelsListed) {
    lines.push_back(fmt::format("{} other programme label readings",
                                labels.size() - kLabelsListed));
  }
  return lines;
}

std::string TeletextDataStream::format_name() const {
  switch (format) {
    case TeletextDataFormat::kIdlFormatA:
      return "IDL Format A";
    case TeletextDataFormat::kIdlFormatB:
      return "IDL Format B";
    case TeletextDataFormat::kDatavideo:
      return "Datavideo";
    case TeletextDataFormat::kAudio:
      return "low bit-rate audio";
  }
  return {};
}

std::string TeletextDataStream::tag() const {
  switch (format) {
    case TeletextDataFormat::kIdlFormatA:
      return address_digits > 0
                 ? fmt::format("{}-{:0{}X}", channel, address, address_digits)
                 : fmt::format("{}", channel);
    case TeletextDataFormat::kIdlFormatB:
      return fmt::format("{}-b{}-{:X}", channel, address >> 4, address & 0xF);
    case TeletextDataFormat::kDatavideo:
      return fmt::format("{}-{:03X}", channel, address);
    case TeletextDataFormat::kAudio:
      return fmt::format("{}-s{:X}", channel, address);
  }
  return {};
}

std::string TeletextDataStream::summary() const {
  std::string where;
  switch (format) {
    case TeletextDataFormat::kIdlFormatA:
      if (address_digits > 0) {
        where = fmt::format(", address {:0{}X}", address, address_digits);
      }
      break;
    case TeletextDataFormat::kIdlFormatB:
      where = fmt::format(", application {}, number {}", address & 0xF,
                          address >> 4);
      break;
    case TeletextDataFormat::kDatavideo:
      where = fmt::format(", packet address {:03X}", address);
      break;
    case TeletextDataFormat::kAudio: {
      // §6.7.2.1 and Table 21.
      static const char* const kIndependent[] = {
          "single channel, 1 line a frame",
          "single channel, 2 lines a frame",
          "single channel, 3 lines a frame",
          "single channel, 4 lines a frame",
          "channel 1 muted",
          "two channels, 2 lines a frame",
          "channel 2 muted",
          "two channels, 4 lines a frame"};
      if (channel == 4) {
        where = address == 0 ? std::string(", AUDETEL")
                             : fmt::format(", service {:X}", address);
      } else {
        where = address < 8
                    ? fmt::format(", {}", kIndependent[address])
                    : fmt::format(", user-defined service {:X}", address);
      }
      break;
    }
  }
  std::string out = fmt::format(
      "data channel {}, {}{}: {} packet{} believed, {} repeat{} dropped, {} "
      "lost",
      channel, format_name(), where, packets, packets == 1 ? "" : "s", repeats,
      repeats == 1 ? "" : "s", gaps);
  if (format == TeletextDataFormat::kIdlFormatB) {
    out += fmt::format(
        "; error correction put right {} byte{} and restored {} packet{}, {} "
        "lost and {} left damaged",
        bytes_corrected, bytes_corrected == 1 ? "" : "s", rows_restored,
        rows_restored == 1 ? "" : "s", rows_lost, rows_damaged);
  }
  if (format == TeletextDataFormat::kDatavideo &&
      (masked_packets > 0 || typed_packets > 0)) {
    out += fmt::format("; {} masked, {} with the packet type bit set",
                       masked_packets, typed_packets);
  }
  if (format == TeletextDataFormat::kAudio && !controls.empty()) {
    std::string seen;
    for (const auto& [control, count] : controls) {
      std::string value;
      if (channel == 4 && address == 0 && control >= 0) {
        // Tables 19 and 20: the fade byte.
        static const int kFadeDb[] = {0, 4, 8, 12, 18, 24, 30, 0};
        value = fmt::format("fade {} dB{}", kFadeDb[control & 7],
                            (control & 8) != 0 ? ", speech muted" : "");
      } else {
        value = control < 0 ? std::string("control unreadable")
                            : fmt::format("control 0x{:02X}", control);
      }
      seen += fmt::format("{}{} in {}", seen.empty() ? "" : "; ", value, count);
    }
    out += "; " + seen;
  }
  out += fmt::format(
      "; {} byte{} of {}", payload.size(), payload.size() == 1 ? "" : "s",
      format == TeletextDataFormat::kAudio ? "audio data" : "payload");
  return out;
}

}  // namespace orc
