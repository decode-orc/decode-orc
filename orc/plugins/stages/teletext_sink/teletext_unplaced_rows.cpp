/*
 * File:        teletext_unplaced_rows.cpp
 * Module:      orc-stage-plugin-teletext_sink
 * Purpose:     The display packets no page could be held responsible for, as
 *              text
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "teletext_unplaced_rows.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>

#include "vbi-services/teletext_page_decoder.h"

namespace orc {

namespace {

// The MRAG's two Hamming 8/4 bytes (EN 300 706 §7.1.2).
constexpr size_t kMragBytes = 2;

}  // namespace

void TeletextUnplacedRows::add(
    int64_t field_index,
    const std::array<uint8_t, kTeletextPacketBytes>& packet,
    size_t display_bytes) {
  ++rows_;
  if (rows_ > kTeletextUnplacedMaxRows) {
    ++rows_dropped_;
    return;
  }
  const int mrag_low = teletext_hamming84_decode(packet[0]);
  const int mrag_high = teletext_hamming84_decode(packet[1]);
  const int magazine = mrag_low < 0 ? -1 : (mrag_low & 0x7);
  const int number = (mrag_low < 0 || mrag_high < 0)
                         ? -1
                         : (((mrag_low >> 3) & 0x1) | (mrag_high << 1));
  std::string characters;
  const size_t count =
      std::min(display_bytes, kTeletextPacketBytes - kMragBytes);
  characters.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const uint8_t byte = packet[kMragBytes + i];
    const auto code = static_cast<uint8_t>(byte & 0x7FU);
    if (!teletext_odd_parity_valid(byte)) {
      characters += '~';
    } else if (code < 0x20 || code == 0x7F) {
      characters += ' ';
    } else {
      characters += static_cast<char>(code);
    }
  }
  // §7.1.2: magazine 0 is called magazine 8, as its pages are numbered.
  lines_ +=
      fmt::format("field {:>8}  M{}  packet {}  |{}|\n", field_index,
                  magazine < 0 ? std::string("?")
                               : std::to_string(magazine == 0 ? 8 : magazine),
                  number < 0 ? std::string("??") : fmt::format("{:02}", number),
                  characters);
}

std::string TeletextUnplacedRows::text() const {
  if (rows_ == 0) {
    return {};
  }
  std::string out =
      "Display packets that reached no page: no page of their magazine was "
      "open when they arrived (the page's header was lost), or their address "
      "had to be corrected. One a line, in the order they arrived: the field "
      "of the range, the magazine (0 is called 8) and packet number, and the "
      "display bytes as characters — a control code as a space, a byte "
      "failing its parity as ~.\n";
  if (rows_dropped_ > 0) {
    out += fmt::format("The first {} of {} are listed.\n",
                       kTeletextUnplacedMaxRows, rows_);
  }
  out += "\n";
  out += lines_;
  return out;
}

}  // namespace orc
