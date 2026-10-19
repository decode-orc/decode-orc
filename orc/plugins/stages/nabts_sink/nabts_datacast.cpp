/*
 * File:        nabts_datacast.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Decoding of the FEC datacasts NABTS carried (RFC 2728 §3.3,
 *              Appendix A)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_datacast.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <optional>

#include "nabts_service_census.h"

namespace orc {

namespace {

// RFC 2728 Appendix A 12.1: GF(2^8) "modulo 100011101" with "a primitive
// element alpha of 00011101". Tabulated over a = x, of which alpha is a^8.
constexpr unsigned kFieldModulus = 0x11D;
constexpr unsigned kFieldOrder = 255;
constexpr unsigned kAlphaExponent = 8;

struct Field {
  std::array<uint8_t, size_t{2} * kFieldOrder> exp{};
  std::array<int16_t, 256> log{};
};

constexpr Field make_field() {
  Field field{};
  unsigned value = 1;
  for (unsigned i = 0; i < kFieldOrder; ++i) {
    field.exp[i] = static_cast<uint8_t>(value);
    field.log[value] = static_cast<int16_t>(i);
    value <<= 1U;
    if ((value & 0x100U) != 0) {
      value ^= kFieldModulus;
    }
  }
  for (unsigned i = kFieldOrder; i < 2 * kFieldOrder; ++i) {
    field.exp[i] = field.exp[i - kFieldOrder];
  }
  field.log[0] = -1;
  return field;
}

constexpr Field kField = make_field();
static_assert(kField.exp[kAlphaExponent] == 0x1D,
              "RFC 2728 Appendix A: alpha is 00011101");

uint8_t field_multiply(uint8_t a, uint8_t b) {
  if (a == 0 || b == 0) {
    return 0;
  }
  return kField.exp[(static_cast<unsigned>(kField.log[a]) +
                     static_cast<unsigned>(kField.log[b])) %
                    kFieldOrder];
}

uint8_t field_divide(uint8_t a, uint8_t b) {
  if (a == 0 || b == 0) {
    return 0;
  }
  return kField.exp[(static_cast<unsigned>(kField.log[a]) + kFieldOrder -
                     static_cast<unsigned>(kField.log[b])) %
                    kFieldOrder];
}

/// alpha to the power |power|.
uint8_t alpha_to(size_t power) {
  return kField.exp[(kAlphaExponent * power) % kFieldOrder];
}

// RFC 2728 §3.2: filler is 0x15 then as many 0xEA as fill the block.
constexpr uint8_t kFillerStart = 0x15;
constexpr uint8_t kFillerByte = 0xEA;

// RFC 2728 §3.4: SLIP's END and ESC, and what ESC makes of the byte after it.
constexpr uint8_t kSlipEnd = 0xC0;
constexpr uint8_t kSlipEscape = 0xDB;
constexpr uint8_t kSlipEscapedEnd = 0xDC;
constexpr uint8_t kSlipEscapedEscape = 0xDD;
// A frame shorter than its four CRC bytes and a byte of schema carries none.
constexpr size_t kMinCrcFrameBytes = 5;

// Placement in a bundle: the cycle it falls in and the offset in it, the data
// rows first and the check rows after, as they are sent from a data run's
// first row.
struct Placement {
  int64_t cycle = 0;
  size_t offset = 0;
};

/// RFC 2728 §3.5 and ISO/IEC 13818-1: x^32 + x^26 + x^23 + x^22 + x^16 + x^12
/// + x^11 + x^10 + x^8 + x^7 + x^5 + x^4 + x^2 + x + 1, all ones initially,
/// no reflection, no final inversion — so a frame carrying its CRC sums to
/// zero.
uint32_t crc32_mpeg2(const std::vector<uint8_t>& bytes) {
  uint32_t crc = 0xFFFFFFFFU;
  for (const uint8_t byte : bytes) {
    crc ^= static_cast<uint32_t>(byte) << 24U;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x80000000U) != 0 ? (crc << 1U) ^ 0x04C11DB7U : crc << 1U;
    }
  }
  return crc;
}

/// The data bytes of a row, its filler removed: the whole of the block where
/// |filler| is false. A row rebuilt from the column code has no packet
/// structure byte to say, so its block is read as filled where it ends in
/// 0x15 and at least one 0xEA, which data seldom does.
size_t useful_bytes(const std::array<uint8_t, kNabtsFecRowBytes>& row,
                    std::optional<bool> filler) {
  size_t end = kNabtsFecDataBytes;
  if (filler.has_value() && !*filler) {
    return end;
  }
  size_t k = end;
  while (k > 0 && row[k - 1] == kFillerByte) {
    --k;
  }
  if (k > 0 && row[k - 1] == kFillerStart && (filler.has_value() || k < end)) {
    return k - 1;
  }
  return end;
}

/// A row in RFC 2728's codeword order: its two suffix bytes at positions 0
/// and 1, its data after (Appendix A 12.3).
std::array<uint8_t, kNabtsFecRowBytes> to_codeword(
    const std::array<uint8_t, kNabtsFecRowBytes>& row) {
  std::array<uint8_t, kNabtsFecRowBytes> codeword{};
  codeword[0] = row[kNabtsFecDataBytes];
  codeword[1] = row[kNabtsFecDataBytes + 1];
  std::copy(row.begin(), row.begin() + kNabtsFecDataBytes,
            codeword.begin() + 2);
  return codeword;
}

std::array<uint8_t, kNabtsFecRowBytes> from_codeword(
    const std::array<uint8_t, kNabtsFecRowBytes>& codeword) {
  std::array<uint8_t, kNabtsFecRowBytes> row{};
  std::copy(codeword.begin() + 2, codeword.end(), row.begin());
  row[kNabtsFecDataBytes] = codeword[0];
  row[kNabtsFecDataBytes + 1] = codeword[1];
  return row;
}

struct FileSignature {
  const char* name;
  std::vector<uint8_t> bytes;
};

const std::vector<FileSignature>& file_signatures() {
  static const std::vector<FileSignature> signatures{
      {"GIF87a", {'G', 'I', 'F', '8', '7', 'a'}},
      {"GIF89a", {'G', 'I', 'F', '8', '9', 'a'}},
      {"PNG", {0x89, 'P', 'N', 'G', 0x0D, 0x0A}},
      {"JPEG", {0xFF, 0xD8, 0xFF}},
      {"ZIP", {'P', 'K', 0x03, 0x04}},
      {"PDF", {'%', 'P', 'D', 'F', '-'}},
      {"HTML", {'<', 'H', 'T', 'M', 'L'}},
      {"html", {'<', 'h', 't', 'm', 'l'}},
  };
  return signatures;
}

}  // namespace

std::array<uint8_t, 2> nabts_fec_sums(const uint8_t* codeword, size_t length) {
  uint8_t s0 = 0;
  uint8_t s1 = 0;
  for (size_t i = 0; i < length; ++i) {
    if (codeword[i] == 0) {
      continue;
    }
    const auto log = static_cast<unsigned>(kField.log[codeword[i]]);
    s0 = static_cast<uint8_t>(
        s0 ^ kField.exp[(log + kAlphaExponent * i) % kFieldOrder]);
    s1 = static_cast<uint8_t>(
        s1 ^ kField.exp[(log + size_t{3} * kAlphaExponent * i) % kFieldOrder]);
  }
  return {s0, s1};
}

bool nabts_fec_correct_byte(uint8_t* codeword, size_t length) {
  const std::array<uint8_t, 2> sums = nabts_fec_sums(codeword, length);
  if (sums[0] == 0 && sums[1] == 0) {
    return true;
  }
  if (sums[0] == 0 || sums[1] == 0) {
    return false;
  }
  // Appendix A 12.4.3: "Byte = 1/2*LOGalpha(S1/S0)". Over a = x, where alpha
  // is a^8, S1/S0 is a^(16 * byte), and 16 is its own inverse modulo 255.
  const unsigned ratio =
      (static_cast<unsigned>(kField.log[sums[1]]) + kFieldOrder -
       static_cast<unsigned>(kField.log[sums[0]])) %
      kFieldOrder;
  const size_t byte = (ratio * 16U) % kFieldOrder;
  if (byte >= length) {
    return false;
  }
  codeword[byte] = static_cast<uint8_t>(codeword[byte] ^
                                        field_divide(sums[0], alpha_to(byte)));
  return true;
}

bool nabts_fec_replace(uint8_t* codeword, size_t length, size_t first,
                       size_t second) {
  codeword[first] = 0;
  codeword[second] = 0;
  const std::array<uint8_t, 2> sums = nabts_fec_sums(codeword, length);
  if (first == second) {
    const uint8_t mask = field_divide(sums[0], alpha_to(first));
    codeword[first] = mask;
    return field_multiply(mask, alpha_to(3 * first)) == sums[1];
  }
  // Appendix A 12.4.4's two equations in two unknowns, solved by Cramer's
  // rule over the field.
  const uint8_t x1 = alpha_to(first);
  const uint8_t x2 = alpha_to(second);
  const uint8_t x1_cubed = alpha_to(3 * first);
  const uint8_t x2_cubed = alpha_to(3 * second);
  const uint8_t determinant =
      field_multiply(x1, x2_cubed) ^ field_multiply(x2, x1_cubed);
  if (determinant == 0) {
    return false;
  }
  codeword[first] = field_divide(
      field_multiply(sums[0], x2_cubed) ^ field_multiply(sums[1], x2),
      determinant);
  codeword[second] = field_divide(
      field_multiply(x1, sums[1]) ^ field_multiply(x1_cubed, sums[0]),
      determinant);
  return true;
}

namespace {

using RowBytes = std::array<uint8_t, kNabtsFecRowBytes>;
using RowErasures = std::array<bool, kNabtsFecRowBytes>;

/// Where byte |i| of a row as sent lies in its codeword (to_codeword()).
size_t codeword_position(size_t i) {
  return i < kNabtsFecDataBytes ? i + 2 : i - kNabtsFecDataBytes;
}

/// |rows| with the bytes |erased| marks filled from the columns' code and the
/// rows' own codes in turn, each taking two erasures; nothing where some stay
/// erased, or where a column or row code does not hold once all are filled.
std::optional<std::vector<RowBytes>> fill_erasures(
    std::vector<RowBytes> rows, std::vector<RowErasures> erased) {
  const size_t positions = rows.size();
  std::vector<uint8_t> column(positions);
  for (bool progress = true; progress;) {
    progress = false;
    for (size_t j = 0; j < kNabtsFecRowBytes; ++j) {
      std::vector<size_t> at;
      for (size_t q = 0; q < positions && at.size() <= 2; ++q) {
        if (erased[q][j]) {
          at.push_back(q);
        }
      }
      if (at.empty() || at.size() > 2) {
        continue;
      }
      for (size_t q = 0; q < positions; ++q) {
        column[q] = rows[q][j];
      }
      if (!nabts_fec_replace(column.data(), positions, at.front(), at.back())) {
        return std::nullopt;
      }
      for (const size_t q : at) {
        rows[q][j] = column[q];
        erased[q][j] = false;
      }
      progress = true;
    }
    for (size_t q = 0; q < positions; ++q) {
      std::vector<size_t> at;
      for (size_t i = 0; i < kNabtsFecRowBytes && at.size() <= 2; ++i) {
        if (erased[q][i]) {
          at.push_back(codeword_position(i));
        }
      }
      if (at.empty() || at.size() > 2) {
        continue;
      }
      RowBytes codeword = to_codeword(rows[q]);
      if (!nabts_fec_replace(codeword.data(), kNabtsFecRowBytes,
                             std::min(at.front(), at.back()),
                             std::max(at.front(), at.back()))) {
        return std::nullopt;
      }
      rows[q] = from_codeword(codeword);
      erased[q].fill(false);
      progress = true;
    }
  }
  constexpr std::array<uint8_t, 2> kHolds{0, 0};
  for (size_t q = 0; q < positions; ++q) {
    const RowBytes codeword = to_codeword(rows[q]);
    if (std::find(erased[q].begin(), erased[q].end(), true) !=
            erased[q].end() ||
        nabts_fec_sums(codeword.data(), kNabtsFecRowBytes) != kHolds) {
      return std::nullopt;
    }
  }
  for (size_t j = 0; j < kNabtsFecRowBytes; ++j) {
    for (size_t q = 0; q < positions; ++q) {
      column[q] = rows[q][j];
    }
    if (nabts_fec_sums(column.data(), positions) != kHolds) {
      return std::nullopt;
    }
  }
  return rows;
}

}  // namespace

std::optional<std::vector<std::array<uint8_t, kNabtsFecRowBytes>>>
nabts_fec_repair_bundle(
    const std::vector<std::array<uint8_t, kNabtsFecRowBytes>>& rows,
    const std::vector<bool>& arrived, const std::vector<bool>& good) {
  const size_t positions = rows.size();
  std::vector<size_t> missing;
  std::vector<size_t> failing;
  for (size_t q = 0; q < positions; ++q) {
    if (!arrived[q]) {
      missing.push_back(q);
    } else if (!good[q]) {
      failing.push_back(q);
    }
  }
  // With two rows missing, every column's two sums go to filling them, and
  // what the rows' own codes add follows from the columns (a product code's
  // checks are not all independent): no reading can be told from another.
  if (failing.size() < 2 || missing.size() > 1) {
    return std::nullopt;
  }
  // Every reading erases the rows that never arrived whole.
  std::vector<RowBytes> received = rows;
  std::vector<RowErasures> shared(positions, RowErasures{});
  for (const size_t q : missing) {
    received[q].fill(0);
    shared[q].fill(true);
  }
  std::optional<std::vector<RowBytes>> found;
  bool ambiguous = false;
  size_t tried = 0;
  // Tries the reading that the failing rows are wrong at |wrong|'s columns,
  // a list for each; false once no more are to be tried.
  const auto attempt = [&](const std::vector<std::vector<size_t>>& wrong) {
    std::vector<RowErasures> erased = shared;
    for (size_t k = 0; k < failing.size(); ++k) {
      for (const size_t j : wrong[k]) {
        erased[failing[k]][j] = true;
      }
    }
    std::optional<std::vector<RowBytes>> filled =
        fill_erasures(received, std::move(erased));
    if (filled) {
      ambiguous = ambiguous || (found && *found != *filled);
      found = std::move(filled);
    }
    return !ambiguous && ++tried < kNabtsFecJointReadings;
  };

  // The columns that say the bundle is wrong.
  std::vector<size_t> flagged;
  std::vector<uint8_t> column(positions);
  for (size_t j = 0; j < kNabtsFecRowBytes; ++j) {
    for (size_t q = 0; q < positions; ++q) {
      column[q] = received[q][j];
    }
    const bool holds =
        missing.empty() ? nabts_fec_sums(column.data(), positions) ==
                              std::array<uint8_t, 2>{0, 0}
                        : nabts_fec_replace(column.data(), positions,
                                            missing.front(), missing.front());
    if (!holds) {
      flagged.push_back(j);
    }
  }
  // Each given to one failing row, every failing row two at least.
  size_t readings = 1;
  for (size_t j = 0; j < flagged.size(); ++j) {
    readings *= failing.size();
    if (readings > kNabtsFecJointReadings) {
      return std::nullopt;
    }
  }
  if (flagged.size() < 2 * failing.size()) {
    return std::nullopt;
  }
  for (size_t reading = 0; reading < readings; ++reading) {
    std::vector<std::vector<size_t>> wrong(failing.size());
    size_t digits = reading;
    for (const size_t j : flagged) {
      wrong[digits % failing.size()].push_back(j);
      digits /= failing.size();
    }
    if (std::any_of(wrong.begin(), wrong.end(),
                    [](const std::vector<size_t>& columns) {
                      return columns.size() < 2;
                    })) {
      continue;
    }
    if (!attempt(wrong)) {
      break;
    }
  }

  if (ambiguous) {
    return std::nullopt;
  }
  return found;
}

std::string nabts_datacast_text(const std::vector<uint8_t>& payload) {
  std::string out;
  std::string run;
  size_t printable = 0;
  const auto end_run = [&]() {
    if (printable >= kNabtsDatacastTextRun &&
        out.size() + run.size() + 1 <= kNabtsDatacastTextMaxBytes) {
      out += run;
      if (out.back() != '\n') {
        out += '\n';
      }
    }
    run.clear();
    printable = 0;
  };
  for (size_t i = 0; i < payload.size(); ++i) {
    const uint8_t byte = payload[i];
    if ((byte >= 0x20 && byte < 0x7F) || byte == '\t') {
      run += static_cast<char>(byte);
      ++printable;
    } else if (byte == '\n' || byte == '\r') {
      // CR LF is one line end.
      if (byte == '\r' && i + 1 < payload.size() && payload[i + 1] == '\n') {
        ++i;
      }
      if (!run.empty() && run.back() != '\n') {
        run += '\n';
      }
    } else {
      end_run();
    }
  }
  end_run();
  return out;
}

std::string NabtsDatacastChannel::summary() const {
  std::string text;
  if (raw) {
    text = fmt::format(
        "{:03X}: a stream of {} packet(s) in no data groups and with no row "
        "code; {} more the continuity index says were lost",
        channel, data_rows, rows_missing);
  } else {
    text = fmt::format("{:03X}: {} data row(s), {} check row(s)", channel,
                       data_rows, check_rows);
  }
  if (!raw && !layout.checks.empty()) {
    text += fmt::format(
        "; bundles of {} data rows and two check rows ({} sent{}), {} them",
        layout.data_rows, layout.checks == "CC" ? "both" : "one",
        layout.checks == "CC" ? "" : ", its fellow never",
        layout.checks_lead ? "leading" : "closing");
    text += fmt::format("; the column code held on {} of {} columns judged",
                        columns_verified, columns_judged);
  } else if (!raw) {
    text += "; no bundles the column code could be read over";
  }
  if (!raw) {
    text += fmt::format(
        "; {} row(s) clean, {} corrected in a byte, {} rebuilt by the column "
        "code ({} of them with the rows' own codes), {} lost, {} left damaged",
        rows_clean, rows_corrected, rows_rebuilt, rows_rebuilt_jointly,
        rows_missing, rows_damaged);
  }
  text += fmt::format("; {} byte(s) of payload, {} gap(s)", payload.size(),
                      payload_gaps);
  if (frames_with_crc > 0) {
    text += fmt::format(
        "; {} of {} SLIP frame(s) carry RFC 2728's CRC (IP over VBI)",
        frames_with_crc, frames);
  }
  text += fmt::format("; {} % text", text_percent);
  for (const auto& [name, count] : signatures) {
    text += fmt::format("; {} {} file signature(s)", count, name);
  }
  if (rows_not_held > 0) {
    text +=
        fmt::format("; {} row(s) past the bound not decoded", rows_not_held);
  }
  return text;
}

void NabtsDatacastDecoder::add_packet(const NabtsPacket& packet,
                                      const uint8_t* raw, uint32_t frame) {
  if (!packet.valid || raw == nullptr) {
    return;
  }
  const bool data_row =
      packet.suffix == NabtsSuffixKind::kLongitudinalPlusReserved;
  const bool check_row = packet.suffix == NabtsSuffixKind::kBundle;
  if (!data_row && !check_row) {
    add_raw(packet, frame);
    return;
  }
  Row row;
  row.frame = frame;
  std::copy(raw + kNabtsPrefixBytes,
            raw + kNabtsPrefixBytes + kNabtsFecRowBytes, row.bytes.begin());
  const std::array<uint8_t, 2> sums =
      nabts_fec_sums(to_codeword(row.bytes).data(), kNabtsFecRowBytes);
  const bool clean = sums[0] == 0 && sums[1] == 0;
  // A packet address corrected into a neighbouring one files a row under a
  // channel the service never sent it on (NabtsPacket::address_attested);
  // one whose row code holds was most likely corrected right.
  if (!packet.address_attested && !clean) {
    return;
  }
  auto it = channels_.find(packet.channel);
  if (it == channels_.end()) {
    if (channels_.size() >= 512) {
      return;
    }
    it = channels_.emplace(packet.channel, Held{}).first;
  }
  Held& held = it->second;
  row.step = 1;
  if (held.started) {
    const auto step = static_cast<uint8_t>(
        (packet.continuity + kNabtsContinuityModulus - held.last_continuity) %
        kNabtsContinuityModulus);
    row.step = step == 0 ? static_cast<uint8_t>(kNabtsContinuityModulus) : step;
  }
  held.started = true;
  held.last_continuity = packet.continuity;
  row.check = check_row;
  row.filler = packet.not_full;
  if (data_row) {
    ++held.data_rows;
    held.clean_rows += clean ? 1 : 0;
  }
  if (held_bytes_ + sizeof(Row) > kNabtsDatacastMaxHeldBytes) {
    ++held.not_held;
    return;
  }
  held.rows.push_back(row);
  held_bytes_ += sizeof(Row);
}

void NabtsDatacastDecoder::add_raw(const NabtsPacket& packet, uint32_t frame) {
  if (packet.data_length == 0) {
    return;
  }
  auto it = raw_.find(packet.channel);
  if (it == raw_.end()) {
    if (raw_.size() >= 512 || !packet.address_attested) {
      return;
    }
    it = raw_.emplace(packet.channel, RawHeld{}).first;
  }
  RawHeld& held = it->second;
  ++held.packets;
  held.synchronizing += packet.synchronizing ? 1 : 0;
  if (held.grouped) {
    return;
  }
  // A channel of data groups sends synchronizing packets throughout: once it
  // has shown enough of them it is no raw stream, and what was held goes.
  if (held.packets >= 1000 &&
      held.synchronizing * 1000 >
          held.packets * kNabtsRawStreamMaxSyncPerThousand) {
    held.grouped = true;
    held_bytes_ -=
        std::min(held_bytes_, held.blocks.size() * kNabtsMaxDataBlockBytes);
    held.blocks = {};
    held.steps = {};
    held.frames = {};
    return;
  }
  // A packet whose address had to be corrected may be another channel's, and
  // in a stream with no row code nothing would show it.
  if (!packet.address_attested) {
    return;
  }
  uint8_t step = 1;
  if (held.started) {
    step = static_cast<uint8_t>(
        (packet.continuity + kNabtsContinuityModulus - held.last_continuity) %
        kNabtsContinuityModulus);
    if (step == 0) {
      step = static_cast<uint8_t>(kNabtsContinuityModulus);
    }
  }
  held.started = true;
  held.last_continuity = packet.continuity;
  if (held_bytes_ + kNabtsMaxDataBlockBytes > kNabtsDatacastMaxHeldBytes) {
    return;
  }
  held.blocks.emplace_back(
      packet.data.begin(),
      packet.data.begin() + static_cast<ptrdiff_t>(packet.data_length));
  held.steps.push_back(step);
  held.frames.push_back(frame);
  held_bytes_ += kNabtsMaxDataBlockBytes;
}

std::optional<NabtsDatacastChannel> NabtsDatacastDecoder::raw_stream(
    uint16_t channel, const RawHeld& held) {
  if (held.grouped || held.blocks.size() < kNabtsRawStreamMinPackets ||
      held.synchronizing * 1000 >
          held.packets * kNabtsRawStreamMaxSyncPerThousand) {
    return std::nullopt;
  }
  uint64_t lost = 0;
  for (const uint8_t step : held.steps) {
    lost += step - 1U;
  }
  if (lost > held.blocks.size() * kNabtsRawStreamMaxLostPerPacket) {
    return std::nullopt;
  }
  // A test sequence — a short shift register's output — is the census's to
  // name (NabtsServiceCensus), and holds nothing to write out. A few blocks
  // are tried, since one may have arrived damaged.
  constexpr size_t kSequenceBlocksTried = 8;
  for (size_t b = 0; b < held.blocks.size() && b < kSequenceBlocksTried; ++b) {
    std::vector<uint8_t> bits;
    for (const uint8_t byte : held.blocks[b]) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        bits.push_back(static_cast<uint8_t>((byte >> bit) & 1U));
      }
    }
    uint64_t polynomial = 0;
    const uint32_t complexity = nabts_linear_complexity(bits, polynomial);
    if (complexity > 0 && complexity <= kNabtsLfsrMaxComplexity) {
      return std::nullopt;
    }
  }
  NabtsDatacastChannel decoded;
  decoded.channel = channel;
  decoded.raw = true;
  decoded.data_rows = held.blocks.size();
  for (size_t i = 0; i < held.blocks.size(); ++i) {
    if (held.steps[i] > 1) {
      ++decoded.payload_gaps;
      decoded.rows_missing += held.steps[i] - 1U;
      for (unsigned k = 1; k < held.steps[i]; ++k) {
        decoded.rows.push_back(
            NabtsDatacastRow{static_cast<uint32_t>(decoded.payload.size()), 0,
                             true, false, held.frames[i]});
      }
    }
    decoded.rows.push_back(
        NabtsDatacastRow{static_cast<uint32_t>(decoded.payload.size()),
                         static_cast<uint16_t>(held.blocks[i].size()), false,
                         false, held.frames[i]});
    decoded.payload.insert(decoded.payload.end(), held.blocks[i].begin(),
                           held.blocks[i].end());
  }
  return decoded;
}

namespace {

/// The layout a channel's rows show (see NabtsDatacastDecoder): the commonest
/// run of consecutive data rows, and the commonest way the stream goes from
/// the last of such a run to the next data row.
template <typename Rows>
NabtsDatacastLayout learn_layout(const Rows& rows) {
  NabtsDatacastLayout layout;
  // Runs of consecutive data rows a check row closes say how many data rows a
  // bundle holds; a loss inside a run starts the count again, so a stretch
  // that lost every other packet offers runs of one, closed by data rows.
  std::map<uint32_t, uint64_t> runs;
  uint32_t run = 0;
  for (const auto& row : rows) {
    if (!row.check) {
      if (run > 0 && row.step != 1) {
        run = 0;
      }
      ++run;
      continue;
    }
    if (run > 0) {
      ++runs[run];
    }
    run = 0;
  }
  if (runs.empty()) {
    return layout;
  }
  const auto commonest = std::max_element(
      runs.begin(), runs.end(),
      [](const auto& a, const auto& b) { return a.second < b.second; });
  layout.data_rows = commonest->first;

  std::map<std::string, uint64_t> tails;
  run = 0;
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].check) {
      run = 0;
      continue;
    }
    run = rows[i].step == 1 ? run + 1 : 1;
    if (run != layout.data_rows) {
      continue;
    }
    std::string tail;
    size_t next = i + 1;
    while (next < rows.size() && rows[next].check && tail.size() < 4) {
      tail.append(rows[next].step - 1U, 'M');
      tail += 'C';
      ++next;
    }
    if (next < rows.size() && !rows[next].check && !tail.empty()) {
      tail.append(rows[next].step - 1U, 'M');
      ++tails[tail];
    }
  }
  if (tails.empty()) {
    return layout;
  }
  const auto tail = std::max_element(
      tails.begin(), tails.end(),
      [](const auto& a, const auto& b) { return a.second < b.second; });
  if (tail->first.size() == 2 && tail->first.find('C') != std::string::npos) {
    layout.checks = tail->first;
  }
  return layout;
}

/// Where each row falls, the cycle of data rows then check rows re-anchored at
/// every check row and at a data row where a check row was due.
template <typename Rows>
std::vector<Placement> place_rows(const Rows& rows,
                                  const NabtsDatacastLayout& layout) {
  const size_t data = layout.data_rows;
  const size_t period = data + 2;
  const auto sent_check = [&](size_t offset) {
    return offset >= data && layout.checks[offset - data] == 'C';
  };
  std::vector<Placement> placed(rows.size());
  int64_t cycle = 0;
  std::optional<size_t> at;
  for (size_t i = 0; i < rows.size(); ++i) {
    size_t offset = 0;
    if (at) {
      const size_t moved = *at + rows[i].step;
      cycle += static_cast<int64_t>(moved / period);
      offset = moved % period;
    }
    if (rows[i].check && !sent_check(offset)) {
      // The sent check position nearest ahead.
      size_t best = period;
      size_t best_distance = period;
      for (size_t o = data; o < period; ++o) {
        const size_t distance = (o + period - offset) % period;
        if (sent_check(o) && distance < best_distance) {
          best = o;
          best_distance = distance;
        }
      }
      if (at && best < offset) {
        ++cycle;
      }
      offset = best;
    } else if (!rows[i].check && offset >= data) {
      // A data row where a check row was due: the next bundle's data run.
      offset = 0;
      ++cycle;
    }
    at = offset;
    placed[i] = Placement{cycle, offset};
  }
  return placed;
}

/// What |decoded|'s payload is, as far as it says without its service: its
/// SLIP frames and those carrying RFC 2728's CRC, how much of it is text, and
/// which file formats' signatures it holds.
void analyse_payload(NabtsDatacastChannel& decoded) {
  std::vector<uint8_t> frame;
  bool escaped = false;
  const auto end_frame = [&]() {
    if (!frame.empty()) {
      ++decoded.frames;
      if (frame.size() >= kMinCrcFrameBytes && crc32_mpeg2(frame) == 0) {
        ++decoded.frames_with_crc;
      }
    }
    frame.clear();
  };
  uint64_t text = 0;
  for (const uint8_t byte : decoded.payload) {
    if ((byte >= 0x20 && byte < 0x7F) || byte == '\n' || byte == '\r' ||
        byte == '\t') {
      ++text;
    }
    if (escaped) {
      frame.push_back(byte == kSlipEscapedEnd      ? kSlipEnd
                      : byte == kSlipEscapedEscape ? kSlipEscape
                                                   : byte);
      escaped = false;
    } else if (byte == kSlipEnd) {
      end_frame();
    } else if (byte == kSlipEscape) {
      escaped = true;
    } else {
      frame.push_back(byte);
    }
  }
  end_frame();
  decoded.text_percent =
      decoded.payload.empty()
          ? 0
          : static_cast<uint32_t>(text * 100 / decoded.payload.size());
  for (const FileSignature& signature : file_signatures()) {
    uint64_t count = 0;
    auto from = decoded.payload.begin();
    while (true) {
      from = std::search(from, decoded.payload.end(), signature.bytes.begin(),
                         signature.bytes.end());
      if (from == decoded.payload.end()) {
        break;
      }
      ++count;
      ++from;
    }
    if (count > 0) {
      decoded.signatures.emplace_back(signature.name, count);
    }
  }
}

}  // namespace

std::vector<NabtsDatacastChannel> NabtsDatacastDecoder::finish() {
  std::vector<NabtsDatacastChannel> out;
  for (auto& [channel, held] : channels_) {
    if (held.data_rows < kNabtsDatacastMinRows ||
        held.clean_rows * 100 <
            held.data_rows * kNabtsDatacastMinCleanPercent) {
      continue;
    }
    NabtsDatacastChannel decoded;
    decoded.channel = channel;
    decoded.rows_not_held = held.not_held;
    std::vector<Row>& rows = held.rows;
    // Whether the channel's flag of rows not full of data marks its filler:
    // whether the rows it flags end in filler's bytes (see
    // kNabtsDatacastFillerEndPercent).
    uint64_t flagged = 0;
    uint64_t filler_ended = 0;
    for (const Row& row : rows) {
      ++(row.check ? decoded.check_rows : decoded.data_rows);
      if (!row.check && row.filler) {
        const uint8_t last = row.bytes[kNabtsFecDataBytes - 1];
        ++flagged;
        filler_ended += last == kFillerStart || last == kFillerByte ? 1 : 0;
      }
    }
    const bool flag_marks_filler =
        filler_ended * 100 >= flagged * kNabtsDatacastFillerEndPercent;
    const auto filler_of = [flag_marks_filler](std::optional<bool> filler) {
      return flag_marks_filler ? filler : std::optional<bool>(false);
    };

    // Each row corrected on its own first (Appendix A 12.4.3); a row its code
    // still condemns is an erasure for its column. A row its code changed is
    // kept as it arrived too: a row two bytes wrong can be changed into the
    // wrong codeword, which only the column code can tell.
    std::vector<bool> row_good(rows.size(), false);
    std::map<size_t, std::array<uint8_t, kNabtsFecRowBytes>> as_received;
    for (size_t i = 0; i < rows.size(); ++i) {
      std::array<uint8_t, kNabtsFecRowBytes> codeword =
          to_codeword(rows[i].bytes);
      const std::array<uint8_t, 2> sums =
          nabts_fec_sums(codeword.data(), kNabtsFecRowBytes);
      if (sums[0] == 0 && sums[1] == 0) {
        row_good[i] = true;
        ++decoded.rows_clean;
        continue;
      }
      if (nabts_fec_correct_byte(codeword.data(), kNabtsFecRowBytes)) {
        as_received[i] = rows[i].bytes;
        rows[i].bytes = from_codeword(codeword);
        row_good[i] = true;
        ++decoded.rows_corrected;
      }
    }

    NabtsDatacastLayout layout = learn_layout(rows);
    const bool bundled = !layout.checks.empty() && layout.data_rows > 0;
    const size_t positions = layout.data_rows + 2;
    std::vector<Placement> placed;
    if (bundled) {
      placed = place_rows(rows, layout);
    }

    // The rows of each bundle by codeword position, for one reading of which
    // data the check rows protect.
    const auto bundles_for = [&](bool checks_lead) {
      std::map<int64_t, std::vector<std::optional<size_t>>> bundles;
      for (size_t i = 0; i < rows.size(); ++i) {
        const Placement& p = placed[i];
        const bool check = p.offset >= layout.data_rows;
        const int64_t bundle = check && checks_lead ? p.cycle + 1 : p.cycle;
        const size_t position =
            check ? p.offset - layout.data_rows : 2 + p.offset;
        auto& slots = bundles[bundle];
        if (slots.empty()) {
          slots.resize(positions);
        }
        if (!slots[position]) {
          slots[position] = i;
        }
      }
      return bundles;
    };
    // Columns a reading verifies, on bundles with at most one erasure.
    const auto verified_by = [&](const auto& bundles) {
      uint64_t verified = 0;
      std::vector<uint8_t> column(positions);
      for (const auto& [bundle, slots] : bundles) {
        size_t erasures = 0;
        size_t erased = 0;
        for (size_t q = 0; q < positions; ++q) {
          if (!slots[q] || !row_good[*slots[q]]) {
            ++erasures;
            erased = q;
          }
        }
        if (erasures > 1) {
          continue;
        }
        for (size_t j = 0; j < kNabtsFecRowBytes; ++j) {
          for (size_t q = 0; q < positions; ++q) {
            column[q] = slots[q] && row_good[*slots[q]]
                            ? rows[*slots[q]].bytes[j]
                            : uint8_t{0};
          }
          const bool held =
              erasures == 0
                  ? nabts_fec_sums(column.data(), positions) ==
                        std::array<uint8_t, 2>{0, 0}
                  : nabts_fec_replace(column.data(), positions, erased, erased);
          verified += held ? 1 : 0;
        }
      }
      return verified;
    };

    std::map<int64_t, std::vector<std::optional<size_t>>> bundles;
    if (bundled) {
      auto closing = bundles_for(false);
      auto leading = bundles_for(true);
      layout.checks_lead = verified_by(leading) > verified_by(closing);
      bundles = layout.checks_lead ? std::move(leading) : std::move(closing);
    }
    decoded.layout = layout;

    // The payload: every data row in order, put right where the column code
    // can and left out where nothing can.
    bool in_gap = false;
    const auto take = [&](const std::array<uint8_t, kNabtsFecRowBytes>& bytes,
                          std::optional<bool> filler, uint32_t frame,
                          bool damaged) {
      const size_t useful = useful_bytes(bytes, filler_of(filler));
      decoded.rows.push_back(NabtsDatacastRow{
          static_cast<uint32_t>(decoded.payload.size()),
          static_cast<uint16_t>(useful), false, damaged, frame});
      decoded.payload.insert(decoded.payload.end(), bytes.begin(),
                             bytes.begin() + static_cast<ptrdiff_t>(useful));
      in_gap = false;
    };
    const auto lose = [&](uint64_t count, uint32_t frame) {
      if (!in_gap) {
        ++decoded.payload_gaps;
      }
      in_gap = true;
      for (uint64_t k = 0; k < count; ++k) {
        decoded.rows.push_back(
            NabtsDatacastRow{static_cast<uint32_t>(decoded.payload.size()), 0,
                             true, false, frame});
      }
    };
    if (!bundled) {
      for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].check) {
          continue;
        }
        if (rows[i].step > 1) {
          lose(rows[i].step - 1U, rows[i].frame);
          decoded.rows_missing += rows[i].step - 1U;
        }
        if (!row_good[i]) {
          ++decoded.rows_damaged;
        }
        take(rows[i].bytes, rows[i].filler, rows[i].frame, !row_good[i]);
      }
    }
    std::vector<uint8_t> column(positions);
    for (const auto& [bundle, slots] : bundles) {
      std::vector<size_t> erased;
      for (size_t q = 0; q < positions; ++q) {
        if (!slots[q] || !row_good[*slots[q]]) {
          erased.push_back(q);
        }
      }
      std::vector<std::array<uint8_t, kNabtsFecRowBytes>> work(positions);
      // The frame the bundle's first row to arrive came in, which its rows
      // that did not arrive take.
      uint32_t first_frame = UINT32_MAX;
      for (size_t q = 0; q < positions; ++q) {
        if (slots[q]) {
          work[q] = rows[*slots[q]].bytes;
          first_frame = std::min(first_frame, rows[*slots[q]].frame);
        }
      }
      bool repairable = erased.size() <= 2;
      // More erasures than the column code takes alone: its rows' own codes
      // and its columns together, where one reading of the failing rows
      // holds.
      bool jointly = false;
      if (!repairable) {
        std::vector<bool> arrived(positions);
        std::vector<bool> good(positions);
        for (size_t q = 0; q < positions; ++q) {
          arrived[q] = slots[q].has_value();
          good[q] = slots[q] && row_good[*slots[q]];
        }
        if (std::optional<std::vector<std::array<uint8_t, kNabtsFecRowBytes>>>
                repaired = nabts_fec_repair_bundle(work, arrived, good)) {
          work = std::move(*repaired);
          repairable = true;
          jointly = true;
        }
      }
      if (repairable && !jointly) {
        std::vector<std::array<uint8_t, kNabtsFecRowBytes>> repaired = work;
        bool all_held = true;
        for (size_t j = 0; j < kNabtsFecRowBytes; ++j) {
          for (size_t q = 0; q < positions; ++q) {
            column[q] = repaired[q][j];
          }
          bool held = false;
          if (erased.empty()) {
            held = nabts_fec_correct_byte(column.data(), positions);
          } else {
            held = nabts_fec_replace(column.data(), positions, erased.front(),
                                     erased.back());
          }
          if (erased.size() <= 1) {
            ++decoded.columns_judged;
            decoded.columns_verified += held ? 1 : 0;
          }
          all_held = all_held && held;
          for (size_t q = 0; q < positions; ++q) {
            repaired[q][j] = column[q];
          }
        }
        // A bundle with one erasure checks its own replacement. A column whose
        // other sum fails it says a row is wrong though its own code holds, or
        // that the rows were placed wrong. A row two bytes wrong that its code
        // changed into the wrong codeword is found among those its code
        // changed: rebuilt from the columns, it and the erasure put aside, it
        // lies within two bytes of the row as it arrived, as no other row's
        // rebuilding does. Where no row, or more than one, does so, nothing is
        // rebuilt from the bundle. Two erasures leave nothing to check by.
        if (erased.size() == 1 && !all_held) {
          repairable = false;
          std::optional<std::vector<std::array<uint8_t, kNabtsFecRowBytes>>>
              found;
          bool ambiguous = false;
          for (size_t q = 0; q < positions && !ambiguous; ++q) {
            const auto arrived =
                slots[q] ? as_received.find(*slots[q]) : as_received.end();
            if (q == erased.front() || arrived == as_received.end()) {
              continue;
            }
            std::vector<std::array<uint8_t, kNabtsFecRowBytes>> trial = work;
            for (size_t j = 0; j < kNabtsFecRowBytes; ++j) {
              for (size_t p = 0; p < positions; ++p) {
                column[p] = trial[p][j];
              }
              nabts_fec_replace(column.data(), positions,
                                std::min(q, erased.front()),
                                std::max(q, erased.front()));
              for (size_t p = 0; p < positions; ++p) {
                trial[p][j] = column[p];
              }
            }
            size_t differing = 0;
            for (size_t j = 0; j < kNabtsFecRowBytes; ++j) {
              differing += trial[q][j] != arrived->second[j] ? 1 : 0;
            }
            if (trial[q] != work[q] && differing <= 2) {
              ambiguous = found.has_value();
              found = std::move(trial);
            }
          }
          if (found && !ambiguous) {
            repairable = true;
            work = std::move(*found);
            ++decoded.rows_rebuilt;
          }
        } else {
          work = std::move(repaired);
        }
      }
      for (size_t q = 2; q < positions; ++q) {
        const bool erasure =
            std::find(erased.begin(), erased.end(), q) != erased.end();
        if (!erasure) {
          take(work[q], rows[*slots[q]].filler, rows[*slots[q]].frame, false);
          continue;
        }
        if (repairable) {
          ++decoded.rows_rebuilt;
          decoded.rows_rebuilt_jointly += jointly ? 1 : 0;
          take(work[q],
               slots[q] ? std::optional<bool>(rows[*slots[q]].filler)
                        : std::nullopt,
               slots[q] ? rows[*slots[q]].frame : first_frame, false);
          continue;
        }
        if (slots[q]) {
          ++decoded.rows_damaged;
          take(work[q], rows[*slots[q]].filler, rows[*slots[q]].frame, true);
        } else {
          ++decoded.rows_missing;
          lose(1, first_frame);
        }
      }
    }

    analyse_payload(decoded);
    out.push_back(std::move(decoded));
    rows.clear();
    rows.shrink_to_fit();
  }
  // The channels that carry neither groups nor the row code, as raw streams.
  for (const auto& [channel, held] : raw_) {
    if (channels_.count(channel) > 0) {
      const auto decoded_already =
          std::find_if(out.begin(), out.end(),
                       [channel = channel](const NabtsDatacastChannel& c) {
                         return c.channel == channel;
                       });
      if (decoded_already != out.end()) {
        continue;
      }
    }
    std::optional<NabtsDatacastChannel> stream = raw_stream(channel, held);
    if (stream) {
      analyse_payload(*stream);
      out.push_back(std::move(*stream));
    }
  }
  raw_.clear();
  std::stable_sort(
      out.begin(), out.end(),
      [](const NabtsDatacastChannel& a, const NabtsDatacastChannel& b) {
        return a.channel < b.channel;
      });
  held_bytes_ = 0;
  return out;
}

}  // namespace orc
