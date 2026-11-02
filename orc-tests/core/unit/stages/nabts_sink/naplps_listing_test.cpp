/*
 * File:        naplps_listing_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the listing of a NAPLPS record's codes
 *
 * Covers: controls gathered on a line, an instruction named with its operand
 * bytes counted, characters quoted as a run; a stretch the recording lost as
 * its length; a cursor address and a repeat count read with their control; a
 * control that arrived failing its parity marked; bytes read from the default
 * code environment where a record's start was lost. Deterministic, no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "naplps_listing.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace orc_unit_test {
namespace {

/// |codes| with odd parity, as a record carries them (CEA-516 §3.3).
std::vector<uint8_t> with_parity(const std::vector<uint8_t>& codes) {
  std::vector<uint8_t> out;
  for (const uint8_t code : codes) {
    int ones = 0;
    for (int bit = 0; bit < 7; ++bit) {
      ones += (code >> bit) & 1;
    }
    out.push_back(static_cast<uint8_t>(code | ((ones % 2 == 0) ? 0x80 : 0)));
  }
  return out;
}

TEST(NaplpsListing, NamesTheControlsInstructionsAndText) {
  // SO, RECT FILLED with four operand bytes, SI, "HI", CR LF.
  const std::vector<uint8_t> data = with_parity(
      {0x0E, 0x31, 0x40, 0x41, 0x42, 0x43, 0x0F, 'H', 'I', 0x0D, 0x0A});
  EXPECT_EQ(orc::naplps_code_listing(data, {}),
            "SO\nRECT FILLED (4 bytes)\nSI\n\"HI\"\nAPR APD\n");
}

TEST(NaplpsListing, CountsAStretchTheRecordingLost) {
  const std::vector<uint8_t> data = with_parity({'A', 0x00, 0x00, 0x00, 'B'});
  const std::vector<uint8_t> present = {1, 0, 0, 0, 1};
  EXPECT_EQ(orc::naplps_code_listing(data, present),
            "\"A\"\n[3 bytes lost]\n\"B\"\n");
}

TEST(NaplpsListing, ReadsACursorAddressAndARepeatCount) {
  // NSR to row 2 column 5, 'X', ESC 4/6 (REPEAT) seven times more.
  const std::vector<uint8_t> data =
      with_parity({0x1F, 0x42, 0x45, 'X', 0x1B, 0x46, 0x47});
  EXPECT_EQ(orc::naplps_code_listing(data, {}),
            "NSR row 2 column 5\n\"X\"\nREPEAT 7\n");
}

TEST(NaplpsListing, MarksAControlThatFailedItsParity) {
  std::vector<uint8_t> data = with_parity({0x0E, 0x20, 0x0F});
  data[2] ^= 0x80;  // SI with its parity bit wrong
  EXPECT_EQ(orc::naplps_code_listing(data, {}), "SO\nRESET (0 bytes)\nSI?\n");
}

// A copy whose start was lost begins somewhere in a command: read from the
// default code environment, its operand bytes are characters of the primary
// set until a shift says otherwise.
TEST(NaplpsListing, ReadsALostStartFromTheDefaultSets) {
  const std::vector<uint8_t> data =
      with_parity({0x41, 0x47, 0x0E, 0x3C, 0x7F, 0x7F, 0x7F});
  EXPECT_EQ(orc::naplps_code_listing(data, {}),
            "\"AG\"\nSO\nSET COLOR (3 bytes)\n");
}

}  // namespace
}  // namespace orc_unit_test
