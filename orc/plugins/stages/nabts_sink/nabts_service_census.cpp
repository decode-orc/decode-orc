/*
 * File:        nabts_service_census.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Recognise the services on a NABTS recording that are not
 *              teletext: FEC datacasts, idle fill, test sequences and clocks
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_service_census.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <utility>

#include "vbi-services/teletext_page_decoder.h"
#include "vbi-services/teletext_slicer.h"

namespace orc {

namespace {

// RFC 2728 Appendix A: GF(2^8) "modulo 100011101" with "a primitive element
// alpha of 00011101". The check bytes make alpha and alpha^3 roots of every
// row, so the generator is (x - alpha)(x - alpha^3); with alpha = a^8 for a =
// x that is x^2 + 146x + 157, the code found on recordings before its source
// was.
constexpr uint16_t kRowCodeModulus = 0x11D;
constexpr uint8_t kRowCodeAlpha = 0x1D;

constexpr uint8_t gf_multiply(uint8_t a, uint8_t b) {
  uint16_t shifted = a;
  uint8_t product = 0;
  while (b != 0) {
    if ((b & 1U) != 0) {
      product = static_cast<uint8_t>(product ^ shifted);
    }
    shifted = static_cast<uint16_t>(shifted << 1U);
    if ((shifted & 0x100U) != 0) {
      shifted = static_cast<uint16_t>(shifted ^ kRowCodeModulus);
    }
    b = static_cast<uint8_t>(b >> 1U);
  }
  return product;
}

constexpr uint8_t kRowCodeAlphaCubed =
    gf_multiply(gf_multiply(kRowCodeAlpha, kRowCodeAlpha), kRowCodeAlpha);
static_assert((kRowCodeAlpha ^ kRowCodeAlphaCubed) == 146,
              "g(x) = x^2 + 146x + 157: the x coefficient");
static_assert(gf_multiply(kRowCodeAlpha, kRowCodeAlphaCubed) == 157,
              "g(x) = x^2 + 146x + 157: the constant");

// Multiplication by alpha and by alpha^3, tabulated: the syndromes are read by
// Horner's rule, one table lookup per byte each.
struct RowCodeTables {
  std::array<uint8_t, 256> times_alpha{};
  std::array<uint8_t, 256> times_alpha_cubed{};
};

constexpr RowCodeTables make_row_code_tables() {
  RowCodeTables tables{};
  for (unsigned value = 0; value < 256; ++value) {
    tables.times_alpha[value] =
        gf_multiply(static_cast<uint8_t>(value), kRowCodeAlpha);
    tables.times_alpha_cubed[value] =
        gf_multiply(static_cast<uint8_t>(value), kRowCodeAlphaCubed);
  }
  return tables;
}

constexpr RowCodeTables kRowCode = make_row_code_tables();

// RFC 2728 §3.3: a bundle is sixteen packets, data at continuity indices 0 to
// 13 and the two packets of check bytes at 14 and 15.
constexpr uint8_t kRfc2728FirstCheckIndex = 14;

// An FEC datacast passes its row code on most packets as recovered: 95.5 % on
// EP tapes, 99.8 % on SP ones. Teletext's two-byte suffix is a
// longitudinal check (§3.4) and passes it by chance — at most 0.02 % of an
// address anywhere in the corpus.
constexpr uint64_t kFecMinCleanPercent = 50;

// Idle fill is judged on at least this many packets the suffix check does not
// call damaged, which repeat their contents on average at least this often.
constexpr uint64_t kIdleFillMinPackets = 200;
constexpr uint64_t kIdleFillMinRepeats = 4;

// Ross Video TES3/TES5 Software User Manual (Iss. 02) p. 10-3: "BERT data is
// always transmitted on packet address F00 and with a byte sync of E7."
constexpr uint16_t kNorpakBertChannel = 0xF00;

// Packets of a test sequence: enough sampled, and the share of them that must
// be stretches of the one sequence.
constexpr uint32_t kLfsrMinSampled = 16;
constexpr uint32_t kLfsrMinPercent = 75;
// A stretch that repeats with a period this short is a fixed pattern — a run
// of one byte, say — not a shift register sequence, whatever its complexity.
constexpr size_t kLfsrMaxPeriodBits = 64;

// Clock packets: almost every byte a Hamming 8/4 codeword as received, enough
// time packets, and almost every step from one to the next forward by no more
// than kClockMaxStepSeconds — a time of day advances, a misread one does not
// fall into line with its neighbours. Before 1980 nothing is a date this sink
// can be shown.
constexpr uint64_t kClockMinCleanPercent = 95;
constexpr uint64_t kClockMinTimePackets = 8;
constexpr uint64_t kClockMinStepPercent = 90;
constexpr int64_t kClockMaxStepSeconds = 600;
constexpr int64_t kUnixTime1980 = 315532800;
// Names a clock's other packets carry, kept for the report.
constexpr size_t kClockMaxNames = 8;

// The clock packets of a PBS datacast are fourteen bytes sent as nibble pairs,
// low nibble first: type 1 = 01 02 00 00 00 then the network name, type 2 = 02
// 02 00 00 00 then a Unix time, least significant byte first, then a counter.
constexpr uint8_t kClockNamePacket = 1;
constexpr uint8_t kClockTimePacket = 2;
constexpr size_t kClockFieldOffset = 5;

uint64_t fnv1a(const uint8_t* bytes, size_t count) {
  uint64_t hash = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < count; ++i) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

uint64_t percent(uint64_t part, uint64_t whole) {
  return whole == 0 ? 0 : (part * 100) / whole;
}

std::string channel_text(uint16_t channel) {
  return fmt::format("{:03X}", channel);
}

// "x^11 + x^9 + 1" for a connection polynomial held as a bit mask.
std::string polynomial_text(uint64_t polynomial) {
  std::string out;
  for (int degree = 63; degree >= 0; --degree) {
    if (((polynomial >> degree) & 1U) == 0) {
      continue;
    }
    if (!out.empty()) {
      out += " + ";
    }
    out += degree == 0   ? std::string("1")
           : degree == 1 ? std::string("x")
                         : fmt::format("x^{}", degree);
  }
  return out;
}

// Index of the largest entry, the lower index on a tie.
template <size_t N>
size_t mode_of(const std::array<uint64_t, N>& histogram) {
  size_t best = 0;
  for (size_t i = 1; i < N; ++i) {
    if (histogram[i] > histogram[best]) {
      best = i;
    }
  }
  return best;
}

// Days from 1970-01-01 to a civil date, and back: H. Hinnant's algorithms,
// which hold for every date this sink can be shown.
void civil_from_days(int64_t days, int64_t& year, unsigned& month,
                     unsigned& day) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era = (day_of_era - day_of_era / 1460 +
                                day_of_era / 36524 - day_of_era / 146096) /
                               365;
  const unsigned day_of_year =
      day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
  const unsigned shifted_month = (5 * day_of_year + 2) / 153;
  day = day_of_year - (153 * shifted_month + 2) / 5 + 1;
  month = shifted_month < 10 ? shifted_month + 3 : shifted_month - 9;
  year = static_cast<int64_t>(year_of_era) + era * 400 + (month <= 2 ? 1 : 0);
}

}  // namespace

std::array<uint8_t, 2> nabts_rfc2728_row_syndromes(const uint8_t* block) {
  // Codeword position i holds c_i; positions 0 and 1 are the two check bytes
  // at the block's end, positions 2 to 27 the data bytes in order. Horner's
  // rule from the highest position down.
  uint8_t s0 = 0;
  uint8_t s1 = 0;
  for (int position = static_cast<int>(kNabtsMaxDataBlockBytes) - 1;
       position >= 0; --position) {
    const size_t index = position >= 2 ? static_cast<size_t>(position - 2)
                                       : static_cast<size_t>(26 + position);
    s0 = static_cast<uint8_t>(kRowCode.times_alpha[s0] ^ block[index]);
    s1 = static_cast<uint8_t>(kRowCode.times_alpha_cubed[s1] ^ block[index]);
  }
  return {s0, s1};
}

uint32_t nabts_linear_complexity(const std::vector<uint8_t>& bits,
                                 uint64_t& polynomial) {
  // Berlekamp-Massey over GF(2): c is the connection polynomial so far, b the
  // one before its last length change.
  const size_t n = bits.size();
  std::vector<uint8_t> c(n + 1, 0);
  std::vector<uint8_t> b(n + 1, 0);
  c[0] = 1;
  b[0] = 1;
  size_t length = 0;
  int64_t last_change = -1;
  for (size_t i = 0; i < n; ++i) {
    uint8_t discrepancy = bits[i];
    for (size_t j = 1; j <= length; ++j) {
      discrepancy ^= static_cast<uint8_t>(c[j] & bits[i - j]);
    }
    if (discrepancy == 0) {
      continue;
    }
    const std::vector<uint8_t> before = c;
    const size_t shift =
        static_cast<size_t>(static_cast<int64_t>(i) - last_change);
    for (size_t j = 0; j + shift <= n; ++j) {
      c[j + shift] ^= b[j];
    }
    if (2 * length <= i) {
      length = i + 1 - length;
      last_change = static_cast<int64_t>(i);
      b = before;
    }
  }
  polynomial = 0;
  if (length < 64) {
    for (size_t j = 0; j <= length; ++j) {
      polynomial |= static_cast<uint64_t>(c[j] & 1U) << j;
    }
  }
  return static_cast<uint32_t>(length);
}

std::string nabts_utc_text(int64_t unix_seconds) {
  const int64_t days =
      (unix_seconds >= 0 ? unix_seconds : unix_seconds - 86399) / 86400;
  const int64_t seconds = unix_seconds - days * 86400;
  int64_t year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civil_from_days(days, year, month, day);
  return fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", year, month, day,
                     seconds / 3600, (seconds / 60) % 60, seconds % 60);
}

void NabtsServiceCensus::add_packet(const NabtsPacket& packet,
                                    const uint8_t* raw) {
  if (!packet.valid || !packet.address_attested || raw == nullptr) {
    return;
  }
  auto it = channels_.find(packet.channel);
  if (it == channels_.end()) {
    if (channels_.size() >= kNabtsCensusMaxChannels) {
      return;
    }
    it = channels_.emplace(packet.channel, ChannelCensus{}).first;
  }
  ChannelCensus& census = it->second;
  ++census.packets;
  count_row_code(census, packet, raw);
  count_contents(census, packet);
  count_lfsr(census, packet);
  count_clock(census, packet);
}

void NabtsServiceCensus::count_row_code(ChannelCensus& census,
                                        const NabtsPacket& packet,
                                        const uint8_t* raw) const {
  const bool check_row = packet.suffix == NabtsSuffixKind::kBundle;
  const bool data_row =
      packet.suffix == NabtsSuffixKind::kLongitudinalPlusReserved;
  if (data_row) {
    ++census.row_coded;
    const std::array<uint8_t, 2> syndromes =
        nabts_rfc2728_row_syndromes(raw + kNabtsPrefixBytes);
    if (syndromes[0] == 0 && syndromes[1] == 0) {
      ++census.row_clean;
    }
  }

  // How the bundles run: a check row is a packet that is all suffix (§3.4 PS
  // b8,b6 = 1,1), and the data rows between check rows say how many rows each
  // bundle protects. Lost packets shorten a run, so the report takes the
  // commonest length rather than any one.
  if (check_row) {
    ++census.check_rows;
    if (census.last_was_check) {
      ++census.check_pairs;
      if (census.last_ci == kRfc2728FirstCheckIndex &&
          packet.continuity == kRfc2728FirstCheckIndex + 1) {
        ++census.check_pairs_at_14;
      }
    }
    if (census.data_run > 0) {
      ++census.data_runs[std::min<size_t>(census.data_run,
                                          census.data_runs.size() - 1)];
      census.data_run = 0;
    }
    ++census.check_run;
  } else if (data_row) {
    if (census.check_run > 0) {
      ++census.check_runs[std::min<size_t>(census.check_run,
                                           census.check_runs.size() - 1)];
      census.check_run = 0;
    }
    ++census.data_run;
  }
  census.last_was_check = check_row;
  census.last_ci = packet.continuity;
}

void NabtsServiceCensus::count_contents(ChannelCensus& census,
                                        const NabtsPacket& packet) const {
  if (packet.data_length == 0) {
    return;
  }
  const uint8_t* data = packet.data.data();
  for (size_t i = 0; i + 1 < packet.data_length; ++i) {
    if (data[i] == data[i + 1]) {
      continue;
    }
    ++census.serial_pairs;
    if (nabts_serial_pattern_step(data[i], data[i + 1])) {
      ++census.serial_steps;
    }
  }

  // A copy the suffix check calls damaged is a content of its own that was
  // never sent; leaving those out keeps a noisy tape's fill as few contents
  // as a clean one's.
  if (packet.integrity == NabtsBlockIntegrity::kUncorrectable) {
    return;
  }
  ++census.vouched;
  if (census.too_many_contents) {
    return;
  }
  census.contents.insert(fnv1a(data, packet.data_length));
  if (census.contents.size() > kNabtsIdleFillMaxContents) {
    census.too_many_contents = true;
    std::unordered_set<uint64_t>().swap(census.contents);
  }
}

void NabtsServiceCensus::count_lfsr(ChannelCensus& census,
                                    const NabtsPacket& packet) const {
  if (census.lfsr_sampled >= kNabtsLfsrSamplePackets) {
    return;
  }
  ++census.lfsr_sampled;

  // The 224 bits after the prefix, least significant bit of each byte first:
  // the order a NABTS line transmits them in (CEA-516 §3.1).
  std::vector<uint8_t> bits;
  bits.reserve(kNabtsMaxDataBlockBytes * 8);
  for (const uint8_t byte : packet.data) {
    for (unsigned bit = 0; bit < 8; ++bit) {
      bits.push_back(static_cast<uint8_t>((byte >> bit) & 1U));
    }
  }
  for (size_t period = 1; period <= kLfsrMaxPeriodBits; ++period) {
    bool repeats = true;
    for (size_t i = 0; i + period < bits.size() && repeats; ++i) {
      repeats = bits[i] == bits[i + period];
    }
    if (repeats) {
      return;
    }
  }
  uint64_t polynomial = 0;
  const uint32_t complexity = nabts_linear_complexity(bits, polynomial);
  if (complexity == 0 || complexity > kNabtsLfsrMaxComplexity) {
    return;
  }
  ++census.lfsr_polynomials[polynomial];
}

void NabtsServiceCensus::count_clock(ChannelCensus& census,
                                     const NabtsPacket& packet) const {
  // Read over the whole 28 bytes, whatever the PS byte says of a suffix: the
  // clock packets carry no data group, and their suffix code is no part of
  // what they say.
  std::array<int, kNabtsMaxDataBlockBytes> nibbles{};
  bool decodable = true;
  for (size_t i = 0; i < kNabtsMaxDataBlockBytes; ++i) {
    const uint8_t byte = packet.data[i];
    census.hamming_clean += teletext_hamming84_clean(byte) ? 1 : 0;
    nibbles[i] = teletext_hamming84_decode(byte);
    decodable = decodable && nibbles[i] >= 0;
  }
  census.hamming_bytes += kNabtsMaxDataBlockBytes;
  if (!decodable) {
    return;
  }

  std::array<uint8_t, kNabtsMaxDataBlockBytes / 2> values{};
  for (size_t k = 0; k < values.size(); ++k) {
    values[k] =
        static_cast<uint8_t>(nibbles[2 * k] | (nibbles[2 * k + 1] << 4));
  }
  if (values[0] == kClockTimePacket) {
    const int64_t time =
        static_cast<int64_t>(values[kClockFieldOffset]) |
        (static_cast<int64_t>(values[kClockFieldOffset + 1]) << 8) |
        (static_cast<int64_t>(values[kClockFieldOffset + 2]) << 16) |
        (static_cast<int64_t>(values[kClockFieldOffset + 3]) << 24);
    ++census.time_packets;
    if (census.last_time >= 0 && time >= census.last_time &&
        time - census.last_time <= kClockMaxStepSeconds) {
      // Two neighbours agree, so both are readings of the clock and either
      // may bound the span it covered.
      ++census.time_steps;
      if (census.first_confirmed < 0 ||
          census.last_time < census.first_confirmed) {
        census.first_confirmed = census.last_time;
      }
      census.last_confirmed = std::max(census.last_confirmed, time);
    }
    census.last_time = time;
  } else if (values[0] == kClockNamePacket) {
    std::string name;
    for (size_t k = kClockFieldOffset; k < values.size(); ++k) {
      if (values[k] >= 0x20 && values[k] < 0x7F) {
        name += static_cast<char>(values[k]);
      }
    }
    if (!name.empty() && (census.names.size() < kClockMaxNames ||
                          census.names.count(name) > 0)) {
      ++census.names[name];
    }
  }
}

std::string NabtsServiceCensus::clock_text(const Verdict& clock) {
  std::string text = fmt::format("Hamming-coded time of day, {} to {} UTC",
                                 nabts_utc_text(clock.clock_first),
                                 nabts_utc_text(clock.clock_last));
  if (!clock.clock_name.empty()) {
    text += ", named \"" + clock.clock_name + "\"";
  }
  return text;
}

bool NabtsServiceCensus::classify(uint16_t channel, const ChannelCensus& census,
                                  Verdict& verdict) const {
  if (census.packets < kNabtsCensusMinPackets) {
    return false;
  }

  if (census.row_coded >= kNabtsCensusMinPackets &&
      percent(census.row_clean, census.row_coded) >= kFecMinCleanPercent) {
    verdict.kind = NabtsOtherService::Kind::kFecDatacast;
    std::string layout;
    if (census.check_rows == 0) {
      layout = "no check rows";
    } else {
      const size_t checks = std::max<size_t>(mode_of(census.check_runs), 1);
      const size_t data = mode_of(census.data_runs);
      if (checks == 2 && census.check_pairs > 0 &&
          percent(census.check_pairs_at_14, census.check_pairs) >= 80) {
        layout = fmt::format(
            "RFC 2728 bundles, {} data rows and check rows at CI 14 and 15",
            data);
      } else {
        std::string rows = fmt::format("{} check rows", checks);
        if (checks == 1) {
          rows = "check row";
        } else if (checks == 2) {
          rows = "pair of check rows";
        }
        layout = fmt::format("{} data rows to each {}", data, rows);
      }
    }
    verdict.key = "fec/" + layout;
    verdict.text = layout;
    return true;
  }

  if (!census.too_many_contents && census.vouched >= kIdleFillMinPackets &&
      !census.contents.empty() &&
      census.vouched >= kIdleFillMinRepeats * census.contents.size() &&
      NabtsSerialEvidence{census.serial_pairs, census.serial_steps}
          .is_pattern()) {
    verdict.kind = NabtsOtherService::Kind::kIdleFill;
    verdict.key = "fill";
    verdict.text = fmt::format(
        "{} distinct contents repeated, each byte the last shifted a place "
        "(a bit-error-rate test pattern){}",
        census.contents.size(),
        channel == kNorpakBertChannel
            ? std::string(
                  "; F00 is where Ross Video (Norpak) encoders send their "
                  "BERT data")
            : std::string());
    return true;
  }

  if (percent(census.hamming_clean, census.hamming_bytes) >=
          kClockMinCleanPercent &&
      census.time_packets >= kClockMinTimePackets &&
      percent(census.time_steps, census.time_packets - 1) >=
          kClockMinStepPercent &&
      census.first_confirmed >= kUnixTime1980) {
    verdict.kind = NabtsOtherService::Kind::kClock;
    std::string name;
    uint32_t most = 0;
    for (const auto& [text, count] : census.names) {
      if (count > most) {
        most = count;
        name = text;
      }
    }
    verdict.clock_first = census.first_confirmed;
    verdict.clock_last = census.last_confirmed;
    verdict.clock_name = name;
    verdict.text = clock_text(verdict);
    verdict.key = "clock/" + name;
    return true;
  }
  if (census.lfsr_sampled >= kLfsrMinSampled &&
      !census.lfsr_polynomials.empty()) {
    const auto best = std::max_element(
        census.lfsr_polynomials.begin(), census.lfsr_polynomials.end(),
        [](const auto& a, const auto& b) { return a.second < b.second; });
    if (percent(best->second, census.lfsr_sampled) >= kLfsrMinPercent) {
      verdict.kind = NabtsOtherService::Kind::kTestSequence;
      verdict.key = "lfsr/" + polynomial_text(best->first);
      verdict.text =
          "the pseudo-random sequence " + polynomial_text(best->first);
      return true;
    }
  }

  return false;
}

std::vector<NabtsOtherService> NabtsServiceCensus::services() const {
  // One entry per run of consecutive addresses carrying the same service, with
  // the run's figures summed.
  struct Run {
    Verdict verdict;
    uint16_t first = 0;
    uint16_t last = 0;
    uint64_t packets = 0;
    uint64_t row_coded = 0;
    uint64_t row_clean = 0;
  };
  std::vector<Run> runs;
  for (const auto& [channel, census] : channels_) {
    Verdict verdict;
    if (!classify(channel, census, verdict)) {
      continue;
    }
    // A run stays inside one block of sixteen addresses (the first two hex
    // digits): datacasts come in families of sixteen, and
    // two families side by side are two services.
    if (!runs.empty() && runs.back().last + 1 == channel &&
        (runs.back().last >> 4) == (channel >> 4) &&
        runs.back().verdict.kind == verdict.kind &&
        runs.back().verdict.key == verdict.key) {
      Run& run = runs.back();
      run.last = channel;
      run.packets += census.packets;
      run.row_coded += census.row_coded;
      run.row_clean += census.row_clean;
      continue;
    }
    runs.push_back(Run{std::move(verdict), channel, channel, census.packets,
                       census.row_coded, census.row_clean});
  }

  std::vector<NabtsOtherService> out;
  // Where each clock was listed, and the span it covers. A clock may be
  // carried on more than one address at once — on EF0 and FC0 alike, say,
  // their first readings a second apart — and is listed once, under all of
  // them: same name, overlapping spans.
  std::vector<std::pair<size_t, Verdict>> clocks;
  for (const Run& run : runs) {
    const std::string channels =
        run.first == run.last
            ? channel_text(run.first)
            : channel_text(run.first) + "-" + channel_text(run.last);
    if (run.verdict.kind == NabtsOtherService::Kind::kClock) {
      const Verdict& clock = run.verdict;
      const auto listed = std::find_if(
          clocks.begin(), clocks.end(), [&clock](const auto& entry) {
            return entry.second.clock_name == clock.clock_name &&
                   entry.second.clock_first <=
                       clock.clock_last + kClockMaxStepSeconds &&
                   clock.clock_first <=
                       entry.second.clock_last + kClockMaxStepSeconds;
          });
      if (listed != clocks.end()) {
        Verdict& merged = listed->second;
        merged.clock_first = std::min(merged.clock_first, clock.clock_first);
        merged.clock_last = std::max(merged.clock_last, clock.clock_last);
        NabtsOtherService& service = out[listed->first];
        service.channels += ", " + channels;
        service.packets += run.packets;
        service.description = clock_text(merged);
        continue;
      }
      clocks.emplace_back(out.size(), clock);
    }

    NabtsOtherService service;
    service.kind = run.verdict.kind;
    service.channels = channels;
    service.packets = run.packets;
    service.description = run.verdict.text;
    if (service.kind == NabtsOtherService::Kind::kFecDatacast &&
        run.row_coded > 0) {
      // The run's pass rate, not any one address's.
      service.description += fmt::format(
          "; the RFC 2728 row code passes on {:.1f} % of the data rows as "
          "received",
          100.0 * static_cast<double>(run.row_clean) /
              static_cast<double>(run.row_coded));
    }
    out.push_back(std::move(service));
  }
  return out;
}

namespace {

// NUL with its odd parity bit, which pads a group out.
constexpr uint8_t kNulWithParity = 0x80;

bool is_text_byte(uint8_t byte) {
  if (!teletext_odd_parity_valid(byte)) {
    return false;
  }
  const auto character = static_cast<uint8_t>(byte & 0x7F);
  return (character >= 0x20 && character < 0x7F) || character == '\r' ||
         character == '\n' || character == '\t';
}

}  // namespace

void NabtsPrivateTextLog::add_group(const NabtsDataGroup& group,
                                    uint64_t frame) {
  if (group.header.type == kNabtsBroadcastGroupType ||
      !group.channel_attested || !group.header.attested) {
    return;
  }
  const auto arrived = [&group](size_t i) {
    return group.present.empty() || group.present[i] != kNabtsByteLost;
  };
  size_t received = 0;
  size_t text = 0;
  for (size_t i = 0; i < group.data.size(); ++i) {
    if (arrived(i)) {
      ++received;
      text += is_text_byte(group.data[i]) ? 1 : 0;
    }
  }
  const bool is_text = text >= kNabtsPrivateTextMinBytes &&
                       text * 100 >= received * kNabtsPrivateTextMinPercent;

  Channel& channel = channels_[{group.channel, group.header.type}];
  ++channel.groups;
  channel.packets += group.packets;
  channel.text_groups += is_text ? 1 : 0;
  channel.characters += is_text ? text : 0;

  std::string rendered = fmt::format(
      "--- {:03X}  frame {}  type {}  group {}{}{}\n", group.channel, frame,
      group.header.type, group.header.continuity,
      is_text ? ""
      : group.data.empty()
          ? "  (no data)"
          : fmt::format("  ({} bytes, not text)", group.data.size()),
      group.outcome == NabtsGroupOutcome::kComplete ? "" : "  (ended short)");
  if (is_text) {
    bool in_hole = false;
    for (size_t i = 0; i < group.data.size(); ++i) {
      if (!arrived(i)) {
        if (!in_hole) {
          rendered += "[...]";
          in_hole = true;
        }
        continue;
      }
      in_hole = false;
      const uint8_t byte = group.data[i];
      const auto character = static_cast<char>(byte & 0x7F);
      if (!is_text_byte(byte)) {
        rendered += '.';
      } else if (character != '\r') {  // CR LF ends a line; LF says so alone
        rendered += character;
      }
    }
    rendered += "\n\n";
  } else {
    // Sixteen bytes to a line, with the seven-bit characters they read as.
    for (size_t row = 0; row < group.data.size(); row += 16) {
      std::string characters;
      rendered += fmt::format("{:04x} ", row);
      for (size_t i = row; i < row + 16; ++i) {
        if (i >= group.data.size()) {
          rendered += "   ";
          continue;
        }
        if (!arrived(i)) {
          rendered += " ??";
          characters += '?';
          continue;
        }
        const uint8_t byte = group.data[i];
        rendered += fmt::format(" {:02x}", byte);
        const auto character = static_cast<char>(byte & 0x7F);
        characters += character >= 0x20 && character < 0x7F ? character : '.';
      }
      rendered += "  " + characters + "\n";
    }
    // Control data a provider sends as Hamming 8/4 codewords, padded out with
    // NUL: the nibbles they carry.
    std::string nibbles;
    size_t padding = 0;
    bool hamming = true;
    for (size_t i = 0; i < group.data.size() && hamming; ++i) {
      if (!arrived(i)) {
        nibbles += padding == 0 ? " ?" : "";
        continue;
      }
      const uint8_t byte = group.data[i];
      if (byte == kNulWithParity) {
        ++padding;
      } else if (padding == 0 && teletext_hamming84_clean(byte)) {
        nibbles += fmt::format(" {:X}", teletext_hamming84_decode(byte));
      } else {
        hamming = false;
      }
    }
    if (hamming && !nibbles.empty()) {
      rendered += "Hamming 8/4:" + nibbles +
                  (padding > 0 ? fmt::format(", then {} NUL", padding) : "") +
                  "\n";
    }
    rendered += "\n";
  }
  if (channel.shown.size() + rendered.size() > kNabtsPrivateShownBytes) {
    ++channel.not_shown;
  } else {
    channel.shown += rendered;
  }
  if (document_.size() + rendered.size() > kNabtsPrivateTextMaxBytes) {
    ++dropped_;
    return;
  }
  document_ += rendered;
}

std::vector<NabtsOtherService> NabtsPrivateTextLog::services() const {
  std::vector<NabtsOtherService> out;
  for (const auto& [key, figures] : channels_) {
    if (figures.text_groups == 0) {
      continue;
    }
    NabtsOtherService service;
    service.kind = NabtsOtherService::Kind::kPrivateText;
    service.channels = channel_text(key.first);
    service.packets = figures.packets;
    service.description = fmt::format(
        "seven-bit text in {} data groups of type {}, which CEA-516 reserves "
        "for the provider's own use; {} characters",
        figures.text_groups, key.second, figures.characters);
    out.push_back(std::move(service));
  }
  return out;
}

std::vector<NabtsPrivateData> NabtsPrivateTextLog::channels() const {
  std::vector<NabtsPrivateData> out;
  for (const auto& [key, figures] : channels_) {
    NabtsPrivateData data;
    data.channel = key.first;
    data.type = key.second;
    data.groups = figures.groups;
    data.packets = figures.packets;
    data.text_groups = figures.text_groups;
    data.shown = figures.shown;
    data.not_shown = figures.not_shown;
    out.push_back(std::move(data));
  }
  return out;
}

std::string NabtsPrivateTextLog::document() const {
  if (document_.empty()) {
    return {};
  }
  std::string out =
      "Data sent in data groups of a type CEA-516 §4.2.2 reserves, in the "
      "order received\n(each under its channel, the frame it ended on, its "
      "type and group continuity; a group that reads as text as its text, "
      "'.' a byte that is not text and [...] bytes lost with their packet; "
      "any other as a dump of its bytes, ?? a byte lost)\n\n";
  out += document_;
  if (dropped_ > 0) {
    out += fmt::format("{} more groups were not kept\n", dropped_);
  }
  return out;
}

std::string nabts_other_services_summary(
    const std::vector<NabtsOtherService>& services) {
  if (services.empty()) {
    return {};
  }
  std::string out = "Other services (not teletext)\n";
  for (const NabtsOtherService& service : services) {
    std::string kind;
    switch (service.kind) {
      case NabtsOtherService::Kind::kFecDatacast:
        kind = "FEC datacast";
        break;
      case NabtsOtherService::Kind::kIdleFill:
        kind = "idle fill";
        break;
      case NabtsOtherService::Kind::kTestSequence:
        kind = "test sequence";
        break;
      case NabtsOtherService::Kind::kClock:
        kind = "clock";
        break;
      case NabtsOtherService::Kind::kPrivateText:
        kind = "private text";
        break;
    }
    out += fmt::format("  {:<9} {}: {} packets, {}\n", service.channels, kind,
                       service.packets, service.description);
  }
  return out;
}

}  // namespace orc
