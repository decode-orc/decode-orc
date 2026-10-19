/*
 * File:        naplps_state.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     NAPLPS presentation state implementation (X3.110 §5.3.2, §6.2)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_state.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace orc {

namespace {

// Bits of colour map address, which Table D1 item 5(4) fixes at four — sixteen
// simultaneous colours.
constexpr size_t kColourMapAddressBits = 4;

// Table D1 item 11's accounting for a DRCS character: 96 normal-size characters
// fit in the shared 3072 bytes and still leave 2048 for macros, so a character
// costs (3072 - 2048) / 96 rounded up.
constexpr size_t kDrcsBytesPerCharacter = 11;

// A DRCS buffer bigger than this is refused rather than allocated: Table D1
// item 8 sizes the buffer from the character field, and a character field the
// size of the screen would ask for the whole physical resolution.
constexpr uint16_t kMaxDrcsDimension = 256;

}  // namespace

// ---------------------------------------------------------------------------
// Colour
// ---------------------------------------------------------------------------

void NaplpsColourState::reset() {
  mode_ = NabtsColourMode::kDirect;
  // Table II-3: current-foreground-colour is "colour = white mode = direct".
  direct_colour_ = kNabtsNominalWhite;
  drawing_address_ = 0;
  background_address_ = 0;
  reset_map();
}

void NaplpsColourState::reset_map() {
  NabtsColour defaults[kNabtsColourMapEntries];
  nabts_default_colour_map(defaults);
  for (size_t i = 0; i < kNabtsColourMapEntries; ++i) {
    store(i, defaults[i]);
    used_[i] = false;
  }
}

void NaplpsColourState::reset_drawing_to_white() {
  mode_ = NabtsColourMode::kDirect;
  direct_colour_ = kNabtsNominalWhite;
  const size_t white = find_in_map(kNabtsNominalWhite);
  if (white != kNabtsColourMapEntries) {
    drawing_address_ = static_cast<uint32_t>(white);
  }
}

void NaplpsColourState::store(size_t index, const NabtsColour& colour) {
  map_[index] = colour;
  writes_.push_back(MapWrite{static_cast<uint8_t>(index), colour});
}

std::vector<NaplpsColourState::MapWrite> NaplpsColourState::take_writes() {
  std::vector<MapWrite> out;
  out.swap(writes_);
  return out;
}

int16_t NaplpsColourState::drawing_map_address() const {
  const size_t address = drawing_address_ % kNabtsColourMapEntries;
  if (mode_ != NabtsColourMode::kDirect) {
    return static_cast<int16_t>(address);
  }
  return map_[address] == direct_colour_ ? static_cast<int16_t>(address)
                                         : int16_t{-1};
}

size_t NaplpsColourState::claimable_entry() const {
  // "the lowest address that has not been used ... and that is not the address
  // of nominal black or nominal white" (§5.3.2.5.1).
  for (size_t i = 0; i < kNabtsColourMapEntries; ++i) {
    if (!used_[i] && !(map_[i] == kNabtsNominalBlack) &&
        !(map_[i] == kNabtsNominalWhite)) {
      return i;
    }
  }
  return kNabtsColourMapEntries;
}

void NaplpsColourState::claim_entry_for_drawing_colour() {
  const size_t entry = claimable_entry();
  if (entry == kNabtsColourMapEntries) {
    return;
  }
  store(entry, direct_colour_);
  used_[entry] = true;
  drawing_address_ = static_cast<uint32_t>(entry);
}

void NaplpsColourState::select_direct_mode() {
  mode_ = NabtsColourMode::kDirect;
}

void NaplpsColourState::select_mapped_mode(uint32_t drawing_address) {
  mode_ = NabtsColourMode::kMapped;
  drawing_address_ = drawing_address % kNabtsColourMapEntries;
  used_[drawing_address_] = true;
}

void NaplpsColourState::select_mapped_background_mode(
    uint32_t drawing_address, uint32_t background_address) {
  mode_ = NabtsColourMode::kMappedWithBackground;
  const uint32_t drawing = drawing_address % kNabtsColourMapEntries;
  const uint32_t background = background_address % kNabtsColourMapEntries;
  // §5.3.2.6.3: "For the special case in which the two operands are identical,
  // ie, the drawing color is specified to be the same as the background color,
  // the drawing color is, instead, left at its current value and only the
  // background color is changed."
  if (drawing != background) {
    drawing_address_ = drawing;
    used_[drawing_address_] = true;
  }
  background_address_ = background;
  used_[background_address_] = true;
}

void NaplpsColourState::reset_to_mapped_mode(bool drawing_white) {
  reset_map();
  mode_ = NabtsColourMode::kMapped;
  if (!drawing_white) {
    return;
  }
  const size_t white = find_in_map(kNabtsNominalWhite);
  if (white != kNabtsColourMapEntries) {
    drawing_address_ = static_cast<uint32_t>(white);
  }
}

size_t NaplpsColourState::find_in_map(const NabtsColour& colour) const {
  for (size_t i = 0; i < kNabtsColourMapEntries; ++i) {
    if (map_[i] == colour) {
      return i;
    }
  }
  return kNabtsColourMapEntries;
}

void NaplpsColourState::set_colour(const NabtsColour& colour) {
  if (mode_ != NabtsColourMode::kDirect) {
    // §5.3.2.5.1: in modes 1 and 2 SET COLOR "is used to load color values into
    // the color map. The address of the entry to be loaded is taken to be the
    // one indicated by the drawing color".
    write_map_entry(drawing_address_, colour);
    return;
  }

  direct_colour_ = colour;

  // §5.3.2.5.1 for mode 0: "If the color specified ... has already been
  // specified in the color map, then the address of the drawing color is set to
  // the lowest address containing that color and the color map is not changed."
  const size_t existing = find_in_map(colour);
  if (existing != kNabtsColourMapEntries) {
    drawing_address_ = static_cast<uint32_t>(existing);
    // That address has now "been used (via a color mode 0 ... SET COLOR
    // command)", so the next colour mode 0 has to find room for may not claim
    // it and recolour what is drawn through it here.
    used_[existing] = true;
    return;
  }

  // "If that color has not already been specified in the color map, color mode
  // 0 makes use of the lowest address that has not been used ... and that is
  // not the address of nominal black or nominal white." "If no addresses are
  // available, then the color map shall not be changed and the drawing color is
  // established in an implementation-dependent manner": the direct colour is
  // kept, and drawn as a value rather than an address (drawing_map_address()).
  claim_entry_for_drawing_colour();
}

void NaplpsColourState::write_map_entry(uint32_t address,
                                        const NabtsColour& colour) {
  const uint32_t index = address % kNabtsColourMapEntries;
  store(index, colour);
  used_[index] = true;
}

void NaplpsColourState::set_transparent() {
  if (mode_ == NabtsColourMode::kDirect) {
    direct_colour_ = kNabtsTransparent;
    return;
  }
  store(drawing_address_ % kNabtsColourMapEntries, kNabtsTransparent);
}

NabtsColour NaplpsColourState::drawing_colour() const {
  if (mode_ == NabtsColourMode::kDirect) {
    return direct_colour_;
  }
  return map_[drawing_address_ % kNabtsColourMapEntries];
}

NabtsColour NaplpsColourState::nominal_black() const {
  return mode_ == NabtsColourMode::kDirect ? kNabtsNominalBlack : map_[0];
}

NabtsColour NaplpsColourState::background_colour() const {
  if (mode_ != NabtsColourMode::kMappedWithBackground) {
    // Modes 0 and 1 have no background: §5.3.2.5.1 has drawings "merely
    // overwrite the existing contents".
    return NabtsColour{};
  }
  return map_[background_address_ % kNabtsColourMapEntries];
}

void NaplpsColourState::copy_map_to(
    NabtsColour (&out)[kNabtsColourMapEntries]) const {
  for (size_t i = 0; i < kNabtsColourMapEntries; ++i) {
    out[i] = map_[i];
  }
}

bool NaplpsColourState::increment_map_address(uint32_t& address) {
  // §5.3.2.5.1: "change the most significant zero to a one and ... all ones to
  // the left of it to zero", within the implemented address width. Reading the
  // address as N bits with b6 leftmost, "most significant" is the highest bit.
  for (int bit = static_cast<int>(kColourMapAddressBits) - 1; bit >= 0; --bit) {
    const uint32_t mask = 1u << bit;
    if ((address & mask) == 0) {
      address |= mask;
      // Clear the ones above it.
      for (int higher = static_cast<int>(kColourMapAddressBits) - 1;
           higher > bit; --higher) {
        address &= ~(1u << higher);
      }
      return true;
    }
  }
  // All ones: §5.3.2.5.1 stops there — "This incrementing process stops and
  // subsequent operand data are ignored when the physical limit (all ones) of
  // the implemented color map is reached."
  return false;
}

uint32_t NaplpsColourState::map_address(uint64_t bits, size_t bit_count) {
  // §5.3.2.6.1: "the number of bits required to specify the color map address
  // is left justified within the single-value operand", so the high N bits of
  // the operand's 6, 12, 18 or 24 are the address. INCREMENTAL POINT's
  // specifications (§5.3.3.6.3) are as wide as their packing count, which may
  // be narrower than the address.
  if (bit_count < kColourMapAddressBits) {
    return static_cast<uint32_t>(bits << (kColourMapAddressBits - bit_count));
  }
  return static_cast<uint32_t>(bits >> (bit_count - kColourMapAddressBits));
}

// ---------------------------------------------------------------------------
// The whole state
// ---------------------------------------------------------------------------

void NaplpsState::reset_all() {
  domain.reset();
  text.reset();
  texture.reset();
  colour.reset();
  reset_active_field();
  // The text cursor starts at home; the drawing point starts at the geometric
  // origin, which is the lower left of the unit screen (T.101 Table II-3 lists
  // the two positions separately for the data syntaxes that have both).
  cursor = home_position();
  drawing_point = NabtsPoint{0.0, 0.0};
  blinks.clear();
  clear_macros();
  clear_drcs();
  for (NabtsTextureMask& mask : texture_masks) {
    mask = NabtsTextureMask{};
  }
  storage_used_ = 0;
}

NabtsPoint NaplpsState::home_position() const {
  // X3.110 §6.1.2.6 and §6.1.2.8: home is "the upper left character position in
  // the display area, in which the top of the character field coincides with
  // the top boundary of the display area", and §5.3.2.9.3 sends a reset cursor
  // to "its home position (top left character position in the display area)".
  // The cursor is the *lower* left corner of that character field (§5.3.2.3.2),
  // so it sits one field height below the top.
  //
  // T.101 Table II-3 lists data syntax III's current-text-position as "lower
  // left corner", which is the corner of the character field rather than of the
  // screen: reading it as the bottom of the screen puts every record that opens
  // with text and line feeds — which is how the reference ExtraVision service
  // writes — on the bottom row, with every line feed clamped against it. The
  // table gives the other two data syntaxes an "upper left corner", and its
  // character height for this one is a known typo, so X3.110's three statements
  // decide it.
  return NabtsPoint{
      0.0, kNabtsDisplayAreaHeight - std::fabs(text.character_field.dy)};
}

size_t NaplpsState::index_of_code(uint8_t code) {
  if (code < kNaplpsFirstCode || code > kNaplpsLastCode) {
    return kNaplpsMacroCount;
  }
  return static_cast<size_t>(code - kNaplpsFirstCode);
}

const NaplpsMacro* NaplpsState::macro(uint8_t code) const {
  const size_t index = index_of_code(code);
  return index < macros_.size() ? &macros_[index] : nullptr;
}

bool NaplpsState::define_macro(uint8_t code, std::vector<uint8_t> body,
                               bool transmit) {
  const size_t index = index_of_code(code);
  if (index >= macros_.size()) {
    return false;
  }
  NaplpsMacro& target = macros_[index];

  // Whatever was there is released first: §6.2.2.1 has a definition replace the
  // previous one, and "A macro may be longer or shorter than the previously
  // existing macro that it replaces".
  storage_used_ -= std::min(storage_used_, target.code.size());
  target = NaplpsMacro{};

  // §6.2.2.1: a null definition deletes the macro, so an empty body is a
  // success that stores nothing.
  if (body.empty()) {
    return true;
  }

  // Table D1 item 11: "one byte for each byte of defining code excluding the
  // DEF and terminating codes", which is exactly |body|.
  if (!storage_available(body.size())) {
    return false;
  }
  storage_used_ += body.size();
  target.code = std::move(body);
  target.transmit = transmit;
  return true;
}

void NaplpsState::clear_macros() {
  for (NaplpsMacro& entry : macros_) {
    storage_used_ -= std::min(storage_used_, entry.code.size());
    entry = NaplpsMacro{};
  }
}

void NaplpsState::drcs_buffer_size(uint16_t& width, uint16_t& height) const {
  // §6.2.3: "The aspect ratio of the storage buffer shall be the same as that
  // of the character field dimensions when the DEF DRCS character is received",
  // at the physical resolution the character field covers.
  //
  // Both axes are measured in the receiver's pixels, which is a different
  // number from the grid's row count: the grid's rows span only the visible
  // kNabtsDisplayAreaHeight of unit y, so a unit-y height covers dy / pitch_y
  // pixels rather than dy * height. Table D1 item 8's own example turns on the
  // difference — "if the character field size is dx = 6/256 and dy = 10/256,
  // then the storage buffer shall be an array of at least 6 elements horizontal
  // by at least 10 elements vertical" — and 10/256 of unit y is 10 rows of the
  // reference grid precisely because 200 / 0,78125 is 256.
  const double dx = std::fabs(text.character_field.dx);
  const double dy = std::fabs(text.character_field.dy);
  const double columns = dx / render_grid_.pitch_x();
  const double rows = dy / render_grid_.pitch_y();
  width = static_cast<uint16_t>(std::clamp<int64_t>(
      std::llround(columns), 1, static_cast<int64_t>(kMaxDrcsDimension)));
  height = static_cast<uint16_t>(std::clamp<int64_t>(
      std::llround(rows), 1, static_cast<int64_t>(kMaxDrcsDimension)));
}

NabtsDrcsCharacter* NaplpsState::begin_drcs(uint8_t code, uint16_t width,
                                            uint16_t height) {
  const size_t index = index_of_code(code);
  if (index >= drcs_.size() || width == 0 || height == 0) {
    return nullptr;
  }
  NabtsDrcsCharacter& target = drcs_[index];

  const bool was_defined = target.defined();
  if (!was_defined && !storage_available(kDrcsBytesPerCharacter)) {
    return nullptr;
  }
  if (!was_defined) {
    storage_used_ += kDrcsBytesPerCharacter;
  }

  // §6.2.3: "At the beginning of each DRCS definition, all of the elements in
  // its storage buffer shall be set to the off state by the receiving device."
  target.code = code;
  target.width = width;
  target.height = height;
  target.elements.assign(static_cast<size_t>(width) * height, false);
  return &target;
}

void NaplpsState::clear_drcs() {
  for (NabtsDrcsCharacter& entry : drcs_) {
    if (entry.defined()) {
      storage_used_ -= std::min(storage_used_, kDrcsBytesPerCharacter);
    }
    entry = NabtsDrcsCharacter{};
  }
}

void NaplpsState::free_drcs(uint8_t code) {
  const size_t index = index_of_code(code);
  if (index >= drcs_.size()) {
    return;
  }
  NabtsDrcsCharacter& entry = drcs_[index];
  if (entry.defined()) {
    storage_used_ -= std::min(storage_used_, kDrcsBytesPerCharacter);
  }
  entry = NabtsDrcsCharacter{};
}

std::vector<NabtsDrcsCharacter> NaplpsState::defined_drcs() const {
  std::vector<NabtsDrcsCharacter> out;
  for (const NabtsDrcsCharacter& entry : drcs_) {
    if (entry.defined()) {
      out.push_back(entry);
    }
  }
  return out;
}

}  // namespace orc
