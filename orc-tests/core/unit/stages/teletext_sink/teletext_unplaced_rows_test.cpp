/*
 * File:        teletext_unplaced_rows_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the text of the display packets that reached no
 *              page
 *
 * Covers: one line a packet with its field, magazine and packet number;
 * control codes shown as spaces and parity failures as '~'; the 525-line
 * packet's 32 display bytes; and the empty case. Hand-built packets only;
 * deterministic, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "teletext_unplaced_rows.h"

#include <gtest/gtest.h>

#include <string>

#include "vbi-services/teletext_page_decoder.h"

namespace orc_unit_test {

namespace {

// A packet of |magazine| (0-7) and packet number |number| whose display bytes
// carry |text|, odd parity throughout, space-padded.
std::array<uint8_t, orc::kTeletextPacketBytes> make_packet(
    int magazine, int number, const std::string& text) {
  std::array<uint8_t, orc::kTeletextPacketBytes> packet{};
  packet[0] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>((magazine & 0x7) | ((number & 0x1) << 3)));
  packet[1] =
      orc::teletext_hamming84_encode(static_cast<uint8_t>((number >> 1) & 0xF));
  for (size_t i = 2; i < packet.size(); ++i) {
    const char c = i - 2 < text.size() ? text[i - 2] : ' ';
    packet[i] = orc::teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return packet;
}

}  // namespace

TEST(TeletextUnplacedRows, WritesNothingWhenEveryPacketWasPlaced) {
  const orc::TeletextUnplacedRows rows;
  EXPECT_EQ(rows.rows(), 0u);
  EXPECT_TRUE(rows.text().empty());
}

// One line a packet: the field it came in, its magazine — 0 is called 8, as
// its pages are numbered — its packet number, and its characters.
TEST(TeletextUnplacedRows, ListsEachPacketWithItsAddress) {
  orc::TeletextUnplacedRows rows;
  rows.add(1234, make_packet(2, 3, "SYM   PRC     VOL"), 32);
  rows.add(1236, make_packet(0, 21, "TEST"), 32);
  EXPECT_EQ(rows.rows(), 2u);
  const std::string text = rows.text();
  EXPECT_NE(text.find("field     1234  M2  packet 03  |SYM   PRC     VOL"),
            std::string::npos)
      << text;
  EXPECT_NE(text.find("field     1236  M8  packet 21  |TEST"),
            std::string::npos)
      << text;
}

// A spacing attribute shows on a set as a space, and a byte failing its odd
// parity is shown as damaged rather than as the character its bits say.
TEST(TeletextUnplacedRows, ShowsControlsAsSpacesAndParityFailuresAsTildes) {
  auto packet = make_packet(1, 5, "AB");
  packet[2] = orc::teletext_odd_parity_encode(0x03);  // alpha yellow
  packet[3] ^= 0x01;                                  // 'B' damaged
  orc::TeletextUnplacedRows rows;
  rows.add(7, packet, 32);
  EXPECT_NE(rows.text().find("| ~"), std::string::npos) << rows.text();
}

// A 525-line packet carries 32 display bytes: the line holds those and no
// more.
TEST(TeletextUnplacedRows, HoldsOnlyTheDisplayBytesThePacketCarried) {
  orc::TeletextUnplacedRows rows;
  rows.add(1, make_packet(3, 1, std::string(40, 'X')), 32);
  EXPECT_NE(rows.text().find("|" + std::string(32, 'X') + "|"),
            std::string::npos)
      << rows.text();
}

}  // namespace orc_unit_test
