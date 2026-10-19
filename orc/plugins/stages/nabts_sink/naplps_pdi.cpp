/*
 * File:        naplps_pdi.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     PDI operand decoding implementation (X3.110 §5.3.1)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_pdi.h"

#include <algorithm>
#include <cmath>

namespace orc {

namespace {

// Bits a gun in the stored colour map (nabts_page.h).
constexpr int kGunBits = kNabtsGunBits;
constexpr auto kGunMax = static_cast<uint32_t>(kNabtsGunMax);

/**
 * @brief A binary fraction from |bits| significant bits, MSB first
 *
 * §5.3.1: coordinates are "binary decimals where the MSB represents the digit
 * just to the right of the decimal point", so bit k from the top contributes
 * 2^-(k+1).
 */
double fraction_from_bits(uint32_t bits, int bit_count) {
  double value = 0.0;
  double weight = 0.5;
  for (int i = bit_count - 1; i >= 0; --i) {
    if ((bits >> i) & 0x1u) {
      value += weight;
    }
    weight *= 0.5;
  }
  return value;
}

/**
 * @brief Two's-complement reading of the same bits
 *
 * The MSB is the sign, weighted -1 rather than +1/2, which puts the value in
 * [-1, 1) — exactly the range §5.3.1 needs for a displacement that may run
 * either way across the unit screen.
 */
double signed_fraction_from_bits(uint32_t bits, int bit_count) {
  if (bit_count <= 0) {
    return 0.0;
  }
  const bool negative = ((bits >> (bit_count - 1)) & 0x1u) != 0;
  const uint32_t magnitude_bits =
      bits & ((bit_count >= 32) ? 0xFFFFFFFFu : ((1u << (bit_count - 1)) - 1u));
  const double magnitude = fraction_from_bits(magnitude_bits, bit_count - 1);
  return negative ? magnitude - 1.0 : magnitude;
}

}  // namespace

uint8_t NaplpsOperandReader::next() {
  if (position_ >= length_) {
    truncated_ = true;
    return 0;  // §5.3.2.2.5: "trailing zero bits are supplied"
  }
  return static_cast<uint8_t>(bytes_[position_++] & kNaplpsNumericMask);
}

uint8_t NaplpsOperandReader::read_fixed_byte() {
  truncated_ = false;
  return next();
}

uint32_t NaplpsOperandReader::read_single_value() {
  truncated_ = false;
  uint32_t value = 0;
  for (size_t i = 0; i < format_.single_value_bytes; ++i) {
    value = (value << kNaplpsNumericBits) | next();
  }
  return value;
}

NabtsPoint NaplpsOperandReader::read_coordinate() {
  truncated_ = false;
  const size_t components = format_.coordinate_components();

  // Figure 11: within each byte the components are contiguous fields, X in the
  // high part, and the six payload bits are divided equally between them — X in
  // b6-b4 and Y in b3-b1 in two-dimensional mode; X in b6-b5, Y in b4-b3 and Z
  // in b2-b1 in three-dimensional mode. Across bytes the first carries the most
  // significant bits of each component and the last the least.
  const int bits_per_byte = static_cast<int>(kNaplpsNumericBits / components);
  const uint32_t field_mask = (1u << bits_per_byte) - 1u;
  // X is the first field and Y the second. Z, when present, is the third and is
  // never read: §5.3.2.2.4 has the receiver ignore it, projecting the image
  // into the X-Y plane.
  const int x_shift = kNaplpsNumericBits - bits_per_byte;
  const int y_shift = kNaplpsNumericBits - 2 * bits_per_byte;

  uint32_t x_bits = 0;
  uint32_t y_bits = 0;
  int bits_per_component = 0;

  for (size_t byte_index = 0; byte_index < format_.multi_value_bytes;
       ++byte_index) {
    const uint8_t payload = next();
    x_bits = (x_bits << bits_per_byte) |
             ((static_cast<uint32_t>(payload) >> x_shift) & field_mask);
    y_bits = (y_bits << bits_per_byte) |
             ((static_cast<uint32_t>(payload) >> y_shift) & field_mask);
    bits_per_component += bits_per_byte;
  }

  NabtsPoint point;
  point.x = signed_fraction_from_bits(x_bits, bits_per_component);
  point.y = signed_fraction_from_bits(y_bits, bits_per_component);
  return point;
}

NabtsColour NaplpsOperandReader::read_colour() {
  truncated_ = false;

  // §5.3.2.5.1 makes a short colour operand legal: "If the maximum size entry
  // that the color map can accommodate is larger than the number of bits
  // provided by the SET COLOR operand, trailing zero bits are supplied by the
  // receiving presentation process." The DOMAIN multi-value length is the most
  // this may read, not the least — the reference ExtraVision service sets
  // white with a single byte where DOMAIN declared three, and requiring the
  // full three dropped the write and left the CBS eye drawn in the background
  // colour.
  const size_t words =
      std::max<size_t>(1, std::min(format_.multi_value_bytes, remaining()));

  // Figure 12: "Each byte contains two three-tuples. Each three-tuple contains
  // one bit for each of the three primary colors ... in the order green, red,
  // blue". So the payloads, concatenated, are the bit string of
  // naplps_colour_from_bits().
  uint64_t bits = 0;
  for (size_t byte_index = 0; byte_index < words; ++byte_index) {
    bits = (bits << kNaplpsNumericBits) | next();
  }
  return naplps_colour_from_bits(
      bits, words * static_cast<size_t>(kNaplpsNumericBits));
}

NabtsColour naplps_colour_from_bits(uint64_t bits, size_t count) {
  uint32_t guns[3] = {0, 0, 0};
  int widths[3] = {0, 0, 0};
  for (size_t i = 0; i < count && i < 64; ++i) {
    const uint32_t bit =
        static_cast<uint32_t>((bits >> (count - 1 - i)) & 0x1u);
    const size_t gun = i % 3;  // green, red, blue
    guns[gun] = (guns[gun] << 1) | bit;
    ++widths[gun];
  }

  // §5.3.2.5.1 twice over: an operand with more bits than the map holds "is
  // truncated and only the most significant bits are used", and "For each
  // primary, the maximum color fraction attainable, given the number of bits
  // specified in the color value operand, shall be interpreted as full
  // intensity and intermediate values shall be equally distributed between zero
  // and full intensity". Truncation already satisfies both at four bits and
  // more — all ones truncates to all ones. Below four it does not: zero-filling
  // two bits would make the brightest colour the service can send 12 of 15
  // rather than white, so the short case is scaled instead, as the Norpak TTX
  // 650 scales it (01 to 5, 10 to 10, 11 to 15). The Panasonic TU-2000x
  // zero-fills and shows a one-byte white as a grey.
  const auto to_gun = [](uint32_t value, int width) -> uint8_t {
    if (width >= kGunBits) {
      return static_cast<uint8_t>(value >> (width - kGunBits));
    }
    const uint32_t maximum = (1u << width) - 1u;
    if (maximum == 0) {
      return 0;
    }
    return static_cast<uint8_t>((value * kGunMax + maximum / 2) / maximum);
  };

  NabtsColour colour;
  colour.green = to_gun(guns[0], widths[0]);
  colour.red = to_gun(guns[1], widths[1]);
  colour.blue = to_gun(guns[2], widths[2]);
  return colour;
}

NabtsPoint naplps_on_unit_screen(NabtsPoint point) {
  // Just below 1, since §5.3.1 makes the range noninclusive at the top.
  constexpr double kJustUnderOne = 0.9999999;
  if (!naplps_in_unit_screen(point.x)) {
    point.x = std::clamp(point.x, 0.0, kJustUnderOne);
  }
  if (!naplps_in_unit_screen(point.y)) {
    point.y = std::clamp(point.y, 0.0, kJustUnderOne);
  }
  return point;
}

}  // namespace orc
