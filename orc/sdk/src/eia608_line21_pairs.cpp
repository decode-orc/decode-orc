/*
 * File:        eia608_line21_pairs.cpp
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Which line 21 byte pairs a receiver takes: their parity, the
 *              codes EIA-608 defines for each field, and the field order
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include <orc/support/eia608_line21_pairs.h>

#include <algorithm>
#include <cstddef>
#include <variant>

namespace orc {

namespace {

// The signs of field order: this many, and this many times the other order's,
// before a capture is read with its fields reversed. A captioned programme
// sends hundreds of control codes a minute; a handful could be a damaged pair.
constexpr uint64_t kMinFieldOrderSigns = 8;
constexpr uint64_t kFieldOrderMargin = 4;

// A pair is taken only where its field carries line 21 data around it: at
// least this many pairs with both parities good within this many frames
// either side, the pair's own included (see EIA608Line21Run).
constexpr uint32_t kStreamRadiusFrames = 30;
constexpr size_t kMinStreamPairs = 3;

// A line next to line 21 reads as a caption line over the run with this many
// fields carrying its waveform, this share of them with both parities good —
// cc_decoder's Cea608Lines measure (the Unlicense; lib/cc_decode.py): a
// caption line passes on nearly every field, noise that slices into a pair
// at chance — and this many control codes among those, which a caption
// stream carries and a test signal reading as one fixed pair does not.
constexpr uint64_t kMinNeighbourFields = 20;
constexpr uint64_t kNeighbourGoodNumerator = 3;
constexpr uint64_t kNeighbourGoodDenominator = 4;
constexpr uint64_t kMinNeighbourCodes = 8;

// A line next to line 21 is read only where line 21 carried nothing for this
// many frames either side: where the caption line has moved for a while, as on
// tapes whose time base slipped, rather than where line 21 lost a field. On
// one cx23885 dump, line 20 read a pair on 995 fields where line 21 had lost
// one amid its own, and taking them lowered agreement with cc_decoder (word
// recall 0.990 to 0.982, precision 0.997 to 0.977).
constexpr uint32_t kSlipQuietFrames = 15;

// The index of line 22 and of line 20 among the lines next to line 21.
size_t neighbour_index(int line) { return line == 20 ? 1 : 0; }

// A control pair's second byte is a code of CTA-608-E's tables for its first
// byte. |first| is the first byte with the data channel bit (08h) cleared.
bool is_control_code(uint8_t first, uint8_t second) {
  if (second >= 0x40) {
    // Preamble address codes [Table 53]; row 11's (10h) are 40h-5Fh only.
    return first != 0x10 || second <= 0x5F;
  }
  if (second >= 0x30) {
    // Special characters (11h) and extended characters (12h, 13h) [Table 49,
    // Tables 5-10].
    return first >= 0x11 && first <= 0x13;
  }
  if (second >= 0x20) {
    switch (first) {
      case 0x10:  // background attributes [Table 3]
      case 0x11:  // mid-row codes [Table 51]
      case 0x12:  // extended characters
      case 0x13:
      case 0x14:  // miscellaneous control codes, field 1's spelling and
      case 0x15:  // field 2's [Table 52, §8.4]
        return true;
      case 0x17:
        // Tab offsets 21h-23h [Table 52], the special assignments 24h-2Ah
        // [Table 4], background transparent and foreground black 2Dh-2Fh
        // [Table 3].
        return (second >= 0x21 && second <= 0x2A) || second >= 0x2D;
      default:
        return false;
    }
  }
  return false;
}

// A boolean observation of the closed caption observer, false when absent.
bool observed_flag(const IObservationContext& context, FieldID field_id,
                   const char* key) {
  const auto value = context.get(field_id, "closed_caption", key);
  return value.has_value() && std::holds_alternative<bool>(*value) &&
         std::get<bool>(*value);
}

// One of the observer's data bytes, its seven data bits.
std::optional<uint8_t> observed_byte(const IObservationContext& context,
                                     FieldID field_id, const char* key) {
  const auto value = context.get(field_id, "closed_caption", key);
  if (!value.has_value() || !std::holds_alternative<int32_t>(*value)) {
    return std::nullopt;
  }
  return static_cast<uint8_t>(std::get<int32_t>(*value) & 0x7F);
}

}  // namespace

EIA608FieldReading eia608_other_line_reading(const IObservationContext& context,
                                             FieldID field_id) {
  EIA608FieldReading reading;
  const auto line = context.get(field_id, "closed_caption", "other_line");
  if (!line.has_value() || !std::holds_alternative<int32_t>(*line)) {
    return reading;
  }
  const int32_t number = std::get<int32_t>(*line);
  const std::optional<uint8_t> data0 =
      observed_byte(context, field_id, "other_data0");
  const std::optional<uint8_t> data1 =
      observed_byte(context, field_id, "other_data1");
  if ((number != 20 && number != 22) || !data0.has_value() ||
      !data1.has_value()) {
    return reading;
  }
  reading.present = true;
  reading.line = number;
  reading.data0 = *data0;
  reading.data1 = *data1;
  reading.parity0_valid =
      observed_flag(context, field_id, "other_parity0_valid");
  reading.parity1_valid =
      observed_flag(context, field_id, "other_parity1_valid");
  return reading;
}

EIA608FieldReading eia608_field_reading(const IObservationContext& context,
                                        FieldID field_id) {
  EIA608FieldReading reading;
  if (!observed_flag(context, field_id, "present")) {
    return reading;
  }
  const std::optional<uint8_t> data0 =
      observed_byte(context, field_id, "data0");
  const std::optional<uint8_t> data1 =
      observed_byte(context, field_id, "data1");
  if (!data0.has_value() || !data1.has_value()) {
    return reading;
  }
  reading.present = true;
  reading.data0 = *data0;
  reading.data1 = *data1;
  reading.parity0_valid = observed_flag(context, field_id, "parity0_valid");
  reading.parity1_valid = observed_flag(context, field_id, "parity1_valid");
  return reading;
}

std::optional<EIA608Pair> eia608_pair_by_parity(
    const EIA608FieldReading& reading) {
  if (!reading.present || !reading.parity0_valid) {
    return std::nullopt;
  }
  EIA608Pair pair;
  pair.data0 = static_cast<uint8_t>(reading.data0 & 0x7F);
  pair.data1 = static_cast<uint8_t>(reading.data1 & 0x7F);
  if (!reading.parity1_valid) {
    if (pair.data0 < 0x20) {
      return std::nullopt;
    }
    pair.data1 = 0x00;
    pair.first_only = true;
  }
  return pair;
}

EIA608Pair eia608_pair_for_field(int television_field, EIA608Pair pair) {
  const bool xds =
      television_field == 1 && pair.data0 >= 0x01 && pair.data0 <= 0x0F;
  if (pair.data0 <= 0x0F && !xds) {
    pair.data0 = 0x00;
  }
  if ((pair.data0 == 0x00 || pair.data0 >= 0x20) && pair.data1 >= 0x01 &&
      pair.data1 <= 0x1F) {
    pair.data1 = 0x00;
  }
  return pair;
}

bool eia608_pair_is_defined(int television_field, uint8_t data0,
                            uint8_t data1) {
  data0 = static_cast<uint8_t>(data0 & 0x7F);
  data1 = static_cast<uint8_t>(data1 & 0x7F);
  if (data0 >= 0x01 && data0 <= 0x0F) {
    // The Extended Data Service's control codes, its characters following
    // them [CTA-608-E §8.6]: an XDS decoder checks the rest, and checksums
    // each packet.
    return television_field == 1;
  }
  if (data0 >= 0x10 && data0 <= 0x1F) {
    return is_control_code(static_cast<uint8_t>(data0 & ~0x08U), data1);
  }
  // Characters, or nulls: a second byte 01h-1Fh is neither.
  return data1 == 0x00 || data1 >= 0x20;
}

void EIA608FieldOrder::add(int stored_field, uint8_t data0, uint8_t data1) {
  data0 = static_cast<uint8_t>(data0 & 0x7F);
  data1 = static_cast<uint8_t>(data1 & 0x7F);
  const bool misc = (data0 & 0xF6) == 0x14 && data1 >= 0x20 && data1 <= 0x2F;
  const bool field1_sign = misc && (data0 & 0x01) == 0;
  const bool field2_sign = (misc && (data0 & 0x01) != 0) ||
                           (data0 >= 0x01 && data0 <= 0x0F && data1 >= 0x01);
  if (!field1_sign && !field2_sign) {
    return;
  }
  const bool first_stored = stored_field == 0;
  if (field1_sign == first_stored) {
    ++normal_;
  } else {
    ++reversed_;
  }
}

bool EIA608FieldOrder::reversed() const {
  return reversed_ >= kMinFieldOrderSigns &&
         reversed_ > kFieldOrderMargin * normal_;
}

void EIA608Line21Run::add(uint32_t frame_offset, int stored_field,
                          const EIA608FieldReading& reading,
                          const EIA608FieldReading& other) {
  const size_t field = stored_field == 0 ? 0 : 1;
  ++lines_[field];
  if (!reading.present) {
    // The line next to it, kept apart until the run says whether it carries
    // captions (carries_captions()).
    if (!other.present || (other.line != 20 && other.line != 22)) {
      return;
    }
    const size_t line = neighbour_index(other.line);
    LineTally& tally = other_tally_[field][line];
    ++tally.read;
    if (other.parity0_valid && other.parity1_valid) {
      ++tally.good;
      const auto first = static_cast<uint8_t>(other.data0 & 0x7F);
      if (first >= 0x01 && first <= 0x1F) {
        ++tally.codes;
      }
    }
    const std::optional<EIA608Pair> pair = eia608_pair_by_parity(other);
    if (!pair.has_value()) {
      return;
    }
    OtherRead read;
    read.read.frame_offset = frame_offset;
    read.read.data0 = pair->data0;
    read.read.data1 = pair->data1;
    read.read.stored_field = static_cast<uint8_t>(field);
    read.read.first_only = pair->first_only;
    read.line = static_cast<uint8_t>(line);
    if (!pair->first_only) {
      other_order_[field][line].add(static_cast<int>(field), pair->data0,
                                    pair->data1);
    }
    other_read_.push_back(read);
    return;
  }
  ++carried_[field];
  line21_frames_[field].push_back(frame_offset);
  const std::optional<EIA608Pair> pair = eia608_pair_by_parity(reading);
  if (!pair.has_value()) {
    return;
  }
  ReadPair read;
  read.frame_offset = frame_offset;
  read.data0 = pair->data0;
  read.data1 = pair->data1;
  read.stored_field = static_cast<uint8_t>(field);
  read.first_only = pair->first_only;
  if (!pair->first_only) {
    field_order_.add(static_cast<int>(field), pair->data0, pair->data1);
    good_frames_[field].push_back(frame_offset);
  }
  read_.push_back(read);
}

uint64_t EIA608Line21Run::lines(int stored_field) const {
  return lines_[stored_field == 0 ? 0 : 1];
}

uint64_t EIA608Line21Run::carried(int stored_field) const {
  return carried_[stored_field == 0 ? 0 : 1];
}

uint64_t EIA608Line21Run::taken_from(int stored_field, int line) const {
  const size_t field = stored_field == 0 ? 0 : 1;
  if ((line != 20 && line != 22) ||
      !carries_captions(field, neighbour_index(line))) {
    return 0;
  }
  return static_cast<uint64_t>(std::count_if(
      other_read_.begin(), other_read_.end(), [&](const OtherRead& read) {
        return read.read.stored_field == field &&
               read.line == neighbour_index(line) && takes(read);
      }));
}

bool EIA608Line21Run::takes(const OtherRead& other) const {
  if (!carries_captions(other.read.stored_field, other.line)) {
    return false;
  }
  const std::vector<uint32_t>& frames = line21_frames_[other.read.stored_field];
  const uint32_t frame = other.read.frame_offset;
  const uint32_t from = frame > kSlipQuietFrames ? frame - kSlipQuietFrames : 0;
  const auto first = std::lower_bound(frames.begin(), frames.end(), from);
  return first == frames.end() || *first > frame + kSlipQuietFrames;
}

bool EIA608Line21Run::carries_captions(size_t field, size_t line) const {
  const LineTally& tally = other_tally_[field][line];
  return tally.read >= kMinNeighbourFields &&
         tally.good * kNeighbourGoodDenominator >=
             tally.read * kNeighbourGoodNumerator &&
         tally.codes >= kMinNeighbourCodes;
}

EIA608FieldOrder EIA608Line21Run::field_order() const {
  EIA608FieldOrder order = field_order_;
  for (size_t field = 0; field < 2; ++field) {
    for (size_t line = 0; line < 2; ++line) {
      if (carries_captions(field, line)) {
        order.absorb(other_order_[field][line]);
      }
    }
  }
  return order;
}

bool EIA608Line21Run::in_stream(const std::vector<uint32_t>& frames,
                                uint32_t frame) {
  const uint32_t from =
      frame > kStreamRadiusFrames ? frame - kStreamRadiusFrames : 0;
  const auto first = std::lower_bound(frames.begin(), frames.end(), from);
  const auto last = std::upper_bound(frames.begin(), frames.end(),
                                     frame + kStreamRadiusFrames);
  return static_cast<size_t>(last - first) >= kMinStreamPairs;
}

void EIA608Line21Run::for_each(
    const std::function<void(const Pair&)>& fn) const {
  // Line 21's pairs, joined, where it carried none, by those of a line next
  // to it that carries captions over the run; in transmission order, and
  // counted towards the stream around each pair as line 21's are.
  std::vector<ReadPair> merged;
  std::array<std::vector<uint32_t>, 2> good_frames = good_frames_;
  bool neighbours = false;
  for (const OtherRead& other : other_read_) {
    neighbours = neighbours || takes(other);
  }
  if (neighbours) {
    merged.reserve(read_.size() + other_read_.size());
    size_t next = 0;
    const auto before = [](const ReadPair& a, const ReadPair& b) {
      return a.frame_offset != b.frame_offset ? a.frame_offset < b.frame_offset
                                              : a.stored_field < b.stored_field;
    };
    for (const OtherRead& other : other_read_) {
      if (!takes(other)) {
        continue;
      }
      while (next < read_.size() && before(read_[next], other.read)) {
        merged.push_back(read_[next++]);
      }
      merged.push_back(other.read);
      if (!other.read.first_only) {
        good_frames[other.read.stored_field].push_back(other.read.frame_offset);
      }
    }
    merged.insert(merged.end(),
                  read_.begin() + static_cast<std::ptrdiff_t>(next),
                  read_.end());
    for (std::vector<uint32_t>& frames : good_frames) {
      std::sort(frames.begin(), frames.end());
    }
  }

  const bool reversed = field_order().reversed();
  for (const ReadPair& read : neighbours ? merged : read_) {
    Pair pair;
    pair.frame_offset = read.frame_offset;
    pair.stored_field = read.stored_field;
    pair.television_field =
        reversed ? 1 - read.stored_field : read.stored_field;
    EIA608Pair taken;
    taken.data0 = read.data0;
    taken.data1 = read.data1;
    taken = eia608_pair_for_field(pair.television_field, taken);
    pair.data0 = taken.data0;
    pair.data1 = taken.data1;
    pair.first_only = read.first_only;
    pair.defined =
        eia608_pair_is_defined(pair.television_field, pair.data0, pair.data1);
    pair.isolated = pair.defined && !in_stream(good_frames[read.stored_field],
                                               read.frame_offset);
    fn(pair);
  }
}

}  // namespace orc
