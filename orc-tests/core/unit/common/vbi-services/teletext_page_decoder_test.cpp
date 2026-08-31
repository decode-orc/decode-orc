/*
 * File:        teletext_page_decoder_test.cpp
 * Module:      orc-tests/core/unit/support
 * Purpose:     Unit tests for the PAL WST teletext page decoder
 *
 * Covers: page assembly across header/body packets, serial vs parallel
 * magazine modes, sub-page replacement and row retention, Hamming 8/4
 * single-bit correction and double-bit rejection, odd-parity cell flagging,
 * Level 1 attribute rendering, and the subtitle cue lifecycle (C6 page
 * arrival / clear / erase). Hand-built packet sequences only; deterministic,
 * no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "vbi-services/teletext_page_decoder.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "teletext_line_synthesizer.h"
#include "vbi-services/teletext_row_squasher.h"

namespace orc_unit_test {

namespace {

using orc::kTeletextPacketBytes;
using orc::TeletextColour;
using orc::TeletextPageDecoder;
using orc::TeletextPageSnapshot;
using orc::TeletextSubtitleCue;
using orc::tests::make_mrag;

struct HeaderFlags {
  bool erase_page = false;
  bool newsflash = false;
  bool subtitle = false;
  bool magazine_serial = false;
  int national_option_subset = 0;
};

// Build an X/0 page header packet (ETSI EN 300 706 §9.3.1): MRAG, Hamming
// 8/4 page number / sub-code / control nibbles, then 32 odd-parity header
// display characters.
std::array<uint8_t, kTeletextPacketBytes> make_header(
    int magazine, int page_number, int subcode, HeaderFlags flags = {},
    const std::string& header_text = "") {
  std::array<uint8_t, kTeletextPacketBytes> packet{};
  const auto mrag = make_mrag(magazine, 0);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  packet[2] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>(page_number & 0xF));  // page units
  packet[3] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>((page_number >> 4) & 0xF));  // page tens
  packet[4] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>(subcode & 0xF));  // S1
  packet[5] = orc::teletext_hamming84_encode(static_cast<uint8_t>(
      ((subcode >> 4) & 0x7) | (flags.erase_page ? 0x8 : 0x0)));  // S2 + C4
  packet[6] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>((subcode >> 7) & 0xF));  // S3
  packet[7] = orc::teletext_hamming84_encode(static_cast<uint8_t>(
      ((subcode >> 11) & 0x3) | (flags.newsflash ? 0x4 : 0x0) |
      (flags.subtitle ? 0x8 : 0x0)));               // S4 + C5 + C6
  packet[8] = orc::teletext_hamming84_encode(0x0);  // C7-C10
  // C11 in bit 2 of byte 13, then C12, C13, C14 in bits 4, 6 and 8
  // (§9.3.1.3 Table 2) = Hamming data bits D1-D4. The sub-set number indexes
  // §15.2 Table 32 with C12 as its most significant bit, so it goes out
  // least-significant-bit-last.
  const int subset = flags.national_option_subset & 0x7;
  packet[9] = orc::teletext_hamming84_encode(static_cast<uint8_t>(
      (flags.magazine_serial ? 0x1 : 0x0) | (((subset >> 2) & 0x1) << 1) |
      (((subset >> 1) & 0x1) << 2) | ((subset & 0x1) << 3)));  // C11-C14
  for (size_t i = 0; i < 32; ++i) {
    const char c = i < header_text.size() ? header_text[i] : ' ';
    packet[10 + i] = orc::teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return packet;
}

// Build a directly displayable row packet X/1 to X/24 (EN 300 706 §9.3.2):
// MRAG then 40 odd-parity display bytes. Bytes of |text| are used verbatim
// (they may include spacing-attribute codes < 0x20), padded with spaces.
std::array<uint8_t, kTeletextPacketBytes> make_row(int magazine, int row,
                                                   const std::string& text) {
  std::array<uint8_t, kTeletextPacketBytes> packet{};
  const auto mrag = make_mrag(magazine, row);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  for (size_t i = 0; i < 40; ++i) {
    const char c = i < text.size() ? text[i] : ' ';
    packet[2 + i] = orc::teletext_odd_parity_encode(static_cast<uint8_t>(c));
  }
  return packet;
}

// A time-filling header (page number FF) that terminates transmissions
// without opening a page (EN 300 706 §7.3).
std::array<uint8_t, kTeletextPacketBytes> make_time_filling_header(
    int magazine, bool magazine_serial = false) {
  HeaderFlags flags;
  flags.magazine_serial = magazine_serial;
  return make_header(magazine, 0xFF, 0x3F7F & 0x1FFF, flags);
}

std::string row_text(const TeletextPageSnapshot& snapshot, int row) {
  std::string text;
  for (const auto& cell : snapshot.cells[static_cast<size_t>(row)]) {
    text.push_back(static_cast<char>(cell.character));
  }
  while (!text.empty() && text.back() == ' ') {
    text.pop_back();
  }
  return text;
}

class TeletextPageDecoderTest : public ::testing::Test {
 protected:
  TeletextPageDecoderTest() {
    decoder_.set_page_callback([this](const TeletextPageSnapshot& snapshot) {
      snapshots_.push_back(snapshot);
    });
  }

  TeletextPageDecoder decoder_;
  std::vector<TeletextPageSnapshot> snapshots_;
};

}  // namespace

////////////////////////////////////////////////////////////////////////////////////////////

TEST_F(TeletextPageDecoderTest, ParsePageNumber_AcceptsConventionalForms) {
  const auto p100 = TeletextPageDecoder::parse_page_number("100");
  ASSERT_TRUE(p100.has_value());
  EXPECT_EQ(p100->first, 1);
  EXPECT_EQ(p100->second, 0x00);

  const auto p888 = TeletextPageDecoder::parse_page_number("888");
  ASSERT_TRUE(p888.has_value());
  EXPECT_EQ(p888->first, 8);
  EXPECT_EQ(p888->second, 0x88);

  const auto hex_page = TeletextPageDecoder::parse_page_number("1fA");
  ASSERT_TRUE(hex_page.has_value());
  EXPECT_EQ(hex_page->first, 1);
  EXPECT_EQ(hex_page->second, 0xFA);
}

TEST_F(TeletextPageDecoderTest, ParsePageNumber_RejectsMalformedStrings) {
  EXPECT_FALSE(TeletextPageDecoder::parse_page_number("").has_value());
  EXPECT_FALSE(TeletextPageDecoder::parse_page_number("88").has_value());
  EXPECT_FALSE(TeletextPageDecoder::parse_page_number("8888").has_value());
  EXPECT_FALSE(TeletextPageDecoder::parse_page_number("088").has_value());
  EXPECT_FALSE(TeletextPageDecoder::parse_page_number("988").has_value());
  EXPECT_FALSE(TeletextPageDecoder::parse_page_number("8G8").has_value());
}

TEST_F(TeletextPageDecoderTest, AssemblesPageAcrossHeaderAndBodyPackets) {
  decoder_.process_packet(make_header(1, 0x00, 0x0001, {}, "P100 HEADER"), 0);
  decoder_.process_packet(make_row(1, 1, "HELLO TELETEXT"), 1);
  decoder_.process_packet(make_row(1, 3, "ROW THREE"), 2);
  decoder_.process_packet(make_time_filling_header(1), 3);

  ASSERT_EQ(snapshots_.size(), 1u);
  const auto& page = snapshots_[0];
  EXPECT_EQ(page.magazine, 1);
  EXPECT_EQ(page.page_number, 0x00);
  EXPECT_EQ(page.subcode, 0x0001);
  EXPECT_EQ(page.header_field_index, 0);
  EXPECT_EQ(page.last_field_index, 2);
  // Header display text lands in row 0 columns 8-39 (EN 300 706 §9.3.1.4).
  EXPECT_EQ(row_text(page, 0), "        P100 HEADER");
  EXPECT_EQ(row_text(page, 1), "HELLO TELETEXT");
  EXPECT_EQ(row_text(page, 2), "");
  EXPECT_EQ(row_text(page, 3), "ROW THREE");
}

// A mis-corrected MRAG or page-number byte does not damage a page: it copies
// it to a number the service never sent, and a catalogue cannot tell the copy
// from the original by looking at it. So the snapshot carries whether the
// header that opened the transmission named the page as transmitted
// (issue #267).
TEST_F(TeletextPageDecoderTest, AttestsAPageNumberThatArrivedAsCodewords) {
  decoder_.process_packet(make_header(1, 0x20, 0, {}, "CLEAN"), 0);
  decoder_.process_packet(make_time_filling_header(1), 1);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_TRUE(snapshots_[0].identity_attested);
}

TEST_F(TeletextPageDecoderTest, DoesNotAttestACorrectedAddressingByte) {
  // Bytes 0 and 1 are the MRAG (§7.1.2), 2 and 3 the page number (§9.3.1.1):
  // the whole of what names the page.
  for (size_t byte = 0; byte < 4; ++byte) {
    snapshots_.clear();
    auto header = make_header(1, 0x20, 0, {}, "DAMAGED");
    header[byte] = static_cast<uint8_t>(header[byte] ^ 0x01);
    decoder_.process_packet(header, 0);
    decoder_.process_packet(make_time_filling_header(1), 1);

    ASSERT_EQ(snapshots_.size(), 1u) << "byte " << byte;
    // Corrected, and correctly — the page still comes out at 1/20.
    EXPECT_EQ(snapshots_[0].magazine, 1) << "byte " << byte;
    EXPECT_EQ(snapshots_[0].page_number, 0x20) << "byte " << byte;
    EXPECT_FALSE(snapshots_[0].identity_attested) << "byte " << byte;
  }
}

// The sub-code and control bytes describe the page rather than name it, and a
// correction in one is not a doubt about which page this is.
TEST_F(TeletextPageDecoderTest, AttestationIgnoresTheSubCodeAndControlBytes) {
  auto header = make_header(1, 0x20, 0, {}, "SUBCODE");
  header[6] = static_cast<uint8_t>(header[6] ^ 0x01);  // S3
  decoder_.process_packet(header, 0);
  decoder_.process_packet(make_time_filling_header(1), 1);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_TRUE(snapshots_[0].identity_attested);
}

// A rolling header (§9.3.1.4) is the same transmission, so it must not restamp
// what the opening header said — for the same reason it does not restamp
// header_field_index.
TEST_F(TeletextPageDecoderTest, ARollingHeaderDoesNotRestampAttestation) {
  decoder_.process_packet(make_header(1, 0x20, 0, {}, "CLEAN"), 0);
  auto rolling = make_header(1, 0x20, 0, {}, "ROLLED");
  rolling[2] = static_cast<uint8_t>(rolling[2] ^ 0x01);
  decoder_.process_packet(rolling, 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_TRUE(snapshots_.back().identity_attested);
}

TEST_F(TeletextPageDecoderTest, FinalizeFlushesOpenPageAssemblies) {
  decoder_.process_packet(make_header(2, 0x34, 0, {}), 0);
  decoder_.process_packet(make_row(2, 1, "UNTERMINATED"), 1);
  EXPECT_TRUE(snapshots_.empty());

  decoder_.finalize(2);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(snapshots_[0].magazine, 2);
  EXPECT_EQ(snapshots_[0].page_number, 0x34);
  EXPECT_EQ(row_text(snapshots_[0], 1), "UNTERMINATED");
}

TEST_F(TeletextPageDecoderTest, ParallelMode_InterleavedMagazinesAssemble) {
  // Parallel mode (C11 clear): a header only terminates the page of its own
  // magazine (EN 300 706 §7.2.1), so rows of two magazines may interleave.
  decoder_.process_packet(make_header(1, 0x11, 0, {}), 0);
  decoder_.process_packet(make_header(2, 0x22, 0, {}), 1);
  decoder_.process_packet(make_row(1, 1, "MAGAZINE ONE"), 2);
  decoder_.process_packet(make_row(2, 1, "MAGAZINE TWO"), 3);
  decoder_.process_packet(make_row(1, 2, "MORE OF ONE"), 4);
  EXPECT_TRUE(snapshots_.empty());

  decoder_.process_packet(make_time_filling_header(1), 5);
  decoder_.process_packet(make_time_filling_header(2), 6);

  ASSERT_EQ(snapshots_.size(), 2u);
  EXPECT_EQ(snapshots_[0].page_number, 0x11);
  EXPECT_EQ(row_text(snapshots_[0], 1), "MAGAZINE ONE");
  EXPECT_EQ(row_text(snapshots_[0], 2), "MORE OF ONE");
  EXPECT_EQ(snapshots_[1].page_number, 0x22);
  EXPECT_EQ(row_text(snapshots_[1], 1), "MAGAZINE TWO");
}

TEST_F(TeletextPageDecoderTest, SerialMode_AnyHeaderTerminatesOpenPage) {
  // Serial mode (C11 set): any page header terminates the page in
  // transmission regardless of magazine (EN 300 706 §7.2.1).
  HeaderFlags serial;
  serial.magazine_serial = true;
  decoder_.process_packet(make_header(1, 0x11, 0, serial), 0);
  decoder_.process_packet(make_row(1, 1, "SERIAL PAGE"), 1);

  decoder_.process_packet(make_header(2, 0x22, 0, serial), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(snapshots_[0].magazine, 1);
  EXPECT_EQ(snapshots_[0].page_number, 0x11);
  EXPECT_TRUE(snapshots_[0].magazine_serial);
  EXPECT_EQ(row_text(snapshots_[0], 1), "SERIAL PAGE");

  // A row for magazine 1 after the terminating header is an orphan (no page
  // open) and must be dropped.
  decoder_.process_packet(make_row(1, 2, "ORPHAN"), 3);
  decoder_.finalize(4);
  ASSERT_EQ(snapshots_.size(), 2u);
  EXPECT_EQ(snapshots_[1].page_number, 0x22);
  EXPECT_EQ(row_text(snapshots_[1], 2), "");
}

TEST_F(TeletextPageDecoderTest, SubpageReplacement_ClearsStoredRows) {
  decoder_.process_packet(make_header(1, 0x50, 0x0001, {}), 0);
  decoder_.process_packet(make_row(1, 1, "SUBPAGE ONE"), 1);
  decoder_.process_packet(make_row(1, 2, "SHARED ROW"), 2);

  // Same page, new sub-code: sub-page replacement starts from a clean grid.
  decoder_.process_packet(make_header(1, 0x50, 0x0002, {}), 3);
  decoder_.process_packet(make_row(1, 1, "SUBPAGE TWO"), 4);
  decoder_.process_packet(make_time_filling_header(1), 5);

  ASSERT_EQ(snapshots_.size(), 2u);
  EXPECT_EQ(snapshots_[0].subcode, 0x0001);
  EXPECT_EQ(row_text(snapshots_[0], 1), "SUBPAGE ONE");
  EXPECT_EQ(snapshots_[1].subcode, 0x0002);
  EXPECT_EQ(row_text(snapshots_[1], 1), "SUBPAGE TWO");
  EXPECT_EQ(row_text(snapshots_[1], 2), "");
}

TEST_F(TeletextPageDecoderTest, RetransmissionWithoutErase_RetainsStoredRows) {
  decoder_.process_packet(make_header(1, 0x50, 0x0001, {}), 0);
  decoder_.process_packet(make_row(1, 1, "ROW ONE"), 1);
  decoder_.process_packet(make_row(1, 2, "ROW TWO"), 2);

  // Retransmission of the same page and sub-code without C4: stored rows
  // persist and newly received rows overwrite (EN 300 706 §9.3.1.3, C4).
  decoder_.process_packet(make_header(1, 0x50, 0x0001, {}), 3);
  decoder_.process_packet(make_row(1, 1, "ROW ONE UPDATED"), 4);
  decoder_.process_packet(make_time_filling_header(1), 5);

  ASSERT_EQ(snapshots_.size(), 2u);
  EXPECT_EQ(row_text(snapshots_[1], 1), "ROW ONE UPDATED");
  EXPECT_EQ(row_text(snapshots_[1], 2), "ROW TWO");
}

TEST_F(TeletextPageDecoderTest, EraseControlBit_ClearsStoredRows) {
  decoder_.process_packet(make_header(1, 0x50, 0x0001, {}), 0);
  decoder_.process_packet(make_row(1, 2, "STALE ROW"), 1);

  HeaderFlags erase;
  erase.erase_page = true;
  decoder_.process_packet(make_header(1, 0x50, 0x0001, erase), 2);
  decoder_.process_packet(make_row(1, 1, "FRESH ROW"), 3);
  decoder_.process_packet(make_time_filling_header(1), 4);

  ASSERT_EQ(snapshots_.size(), 2u);
  EXPECT_TRUE(snapshots_[1].erase_page);
  EXPECT_EQ(row_text(snapshots_[1], 1), "FRESH ROW");
  EXPECT_EQ(row_text(snapshots_[1], 2), "");
}

// A header's own MRAG may be corrected: the identity it opens the page under
// travels with the snapshot for the catalogue to reconcile afterwards (see
// TeletextPageSnapshot::identity_attested), so a page opened at a mis-corrected
// address is recoverable in a way a row filed against the wrong page is not.
TEST_F(TeletextPageDecoderTest,
       Hamming_SingleBitErrorInAHeaderMragIsCorrected) {
  auto header = make_header(1, 0x00, 0, {});
  header[0] ^= 0x10;  // single-bit error in the first MRAG byte (§8.2)

  decoder_.process_packet(header, 0);
  decoder_.process_packet(make_row(1, 1, "OPENED ANYWAY"), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(snapshots_[0].page_number, 0x00);
  EXPECT_EQ(row_text(snapshots_[0], 1), "OPENED ANYWAY");
  EXPECT_FALSE(snapshots_[0].identity_attested);
}

TEST_F(TeletextPageDecoderTest, Hamming_DoubleBitErrorDropsPacket) {
  auto row = make_row(1, 1, "REJECTED");
  row[0] ^= 0x12;  // double-bit error: uncorrectable (§8.2)

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(row, 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(row_text(snapshots_[0], 1), "");
}

TEST_F(TeletextPageDecoderTest,
       Hamming_UncorrectablePageNumberDropsHeaderOnly) {
  decoder_.process_packet(make_header(1, 0x11, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "FIRST PAGE"), 1);

  // A header whose page-units byte carries a double-bit error cannot be
  // attributed to a page; the packet is dropped and the open page keeps
  // assembling.
  auto bad_header = make_header(1, 0x22, 0, {});
  bad_header[2] ^= 0x12;
  decoder_.process_packet(bad_header, 2);
  decoder_.process_packet(make_row(1, 2, "STILL FIRST"), 3);
  decoder_.process_packet(make_time_filling_header(1), 4);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(snapshots_[0].page_number, 0x11);
  EXPECT_EQ(row_text(snapshots_[0], 1), "FIRST PAGE");
  EXPECT_EQ(row_text(snapshots_[0], 2), "STILL FIRST");
}

// Row 24 is the display address the non-display packets X/25 to X/31 are
// mis-corrected into, and the row almost no page transmits — so nothing ever
// out-votes the intruder and it reaches the screen looking like content. A
// corrected MRAG is the one thing those packets have in common, so row 24 is
// the row that will not take one.
TEST_F(TeletextPageDecoderTest, LastDisplayRow_RejectsACorrectedAddress) {
  auto row = make_row(1, 24, "MISCORRECTED");
  row[1] ^= 0x10;  // single-bit error in the packet-number MRAG byte (§8.2)

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "REAL CONTENT"), 1);
  decoder_.process_packet(row, 2);
  decoder_.process_packet(make_time_filling_header(1), 3);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(row_text(snapshots_[0], 1), "REAL CONTENT");
  EXPECT_EQ(row_text(snapshots_[0], 24), "");
  EXPECT_FALSE(snapshots_[0].row_received[24]);
}

TEST_F(TeletextPageDecoderTest, LastDisplayRow_KeepsAnUncorrectedAddress) {
  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 24, "FLOF LABELS"), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(row_text(snapshots_[0], 24), "FLOF LABELS");
  EXPECT_TRUE(snapshots_[0].row_received[24]);
}

// The rule is not only for the last row. Combining repeated rows votes across
// the copies of one row, and it cannot tell a copy of the row from a copy of
// something a mis-corrected address moved here — enough of those and the page
// is a blend of every page mis-addressed into it, which is worse the more
// copies are combined. Over the 525-line reference capture, the magazines that
// service never transmits a row on receive 531 display packets, 530 of them
// with a corrected address.
TEST_F(TeletextPageDecoderTest, OrdinaryRows_RejectACorrectedAddress) {
  auto row = make_row(1, 23, "CORRECTED");
  row[1] ^= 0x10;

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "REAL CONTENT"), 1);
  decoder_.process_packet(row, 2);
  decoder_.process_packet(make_time_filling_header(1), 3);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(row_text(snapshots_[0], 1), "REAL CONTENT");
  EXPECT_EQ(row_text(snapshots_[0], 23), "");
  EXPECT_FALSE(snapshots_[0].row_received[23]);
}

TEST_F(TeletextPageDecoderTest, OrdinaryRows_KeepAnUncorrectedAddress) {
  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 23, "AS TRANSMITTED"), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(row_text(snapshots_[0], 23), "AS TRANSMITTED");
}

TEST_F(TeletextPageDecoderTest, ParityError_FlagsCellWithoutCorruptingPage) {
  auto row = make_row(1, 1, "AXC");
  row[2 + 1] ^= 0x01;  // break odd parity on the 'X' (EN 300 706 §8.1)

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(row, 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  const auto& cells = snapshots_[0].cells[1];
  EXPECT_EQ(cells[0].character, 'A');
  EXPECT_FALSE(cells[0].parity_error);
  EXPECT_EQ(cells[1].character, 0x20);
  EXPECT_TRUE(cells[1].parity_error);
  EXPECT_EQ(cells[2].character, 'C');
  EXPECT_FALSE(cells[2].parity_error);
}

// A row that never arrived renders identically to a transmitted blank row,
// so only row_received can tell a recovery gap from page content.
TEST_F(TeletextPageDecoderTest, RowReceivedMarksTheRowsThatArrived) {
  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "ROW ONE"), 1);
  // No packet for row 2 — lost in recovery.
  decoder_.process_packet(make_row(1, 3, "   "), 2);  // transmitted but blank
  decoder_.process_packet(make_time_filling_header(1), 3);

  ASSERT_EQ(snapshots_.size(), 1u);
  const auto& snapshot = snapshots_[0];
  EXPECT_TRUE(snapshot.row_received[0]);  // the X/0 header
  EXPECT_TRUE(snapshot.row_received[1]);
  EXPECT_FALSE(snapshot.row_received[2]);
  // A transmitted blank row looks like row 2 in the cells but is not a gap.
  EXPECT_TRUE(snapshot.row_received[3]);
  EXPECT_EQ(row_text(snapshot, 2), row_text(snapshot, 3));
  EXPECT_FALSE(snapshot.row_received[4]);
}

// With a squasher attached, rows recovered during an earlier transmission
// stay available when a later one is clipped, and repeated copies of a row
// correct each other (vbi-services/teletext_row_squasher.h).
TEST_F(TeletextPageDecoderTest, SquasherKeepsRowsAcrossClippedTransmissions) {
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "ROW ONE"), 1);
  decoder_.process_packet(make_row(1, 2, "ROW TWO"), 2);
  // A second transmission carrying only row 1 — the rest fell outside.
  decoder_.process_packet(make_header(1, 0x00, 0, {}), 3);
  decoder_.process_packet(make_row(1, 1, "ROW ONE"), 4);
  decoder_.process_packet(make_time_filling_header(1), 5);

  ASSERT_EQ(snapshots_.size(), 2u);
  const auto& clipped = snapshots_.back();
  EXPECT_EQ(row_text(clipped, 1), "ROW ONE");
  EXPECT_EQ(row_text(clipped, 2), "ROW TWO")
      << "a row the clipped transmission did not carry was lost";
  EXPECT_TRUE(clipped.row_received[2]);
}

// How many copies a row rests on is the page's confidence in it. Hamming 8/4
// corrects one bit and detects two (EN 300 706 §8.2), but a longer burst can
// carry a row's address onto another valid one, and a row that arrived once
// has nothing to contradict it if that happens.
TEST_F(TeletextPageDecoderTest, RowCopiesCountWhatEachRowRestsOn) {
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "ROW ONE"), 1);
  decoder_.process_packet(make_row(1, 2, "ROW TWO"), 2);
  // A second pass of the carousel confirms row 1 but not row 2.
  decoder_.process_packet(make_header(1, 0x00, 0, {}), 3);
  decoder_.process_packet(make_row(1, 1, "ROW ONE"), 4);
  decoder_.process_packet(make_time_filling_header(1), 5);

  ASSERT_EQ(snapshots_.size(), 2u);
  const auto& snapshot = snapshots_.back();
  EXPECT_EQ(snapshot.row_copies[0], 0) << "header rows are never squashed";
  EXPECT_EQ(snapshot.row_copies[1], 2);
  EXPECT_EQ(snapshot.row_copies[2], 1);
  EXPECT_EQ(snapshot.row_copies[3], 0) << "no packet was received for row 3";
}

TEST_F(TeletextPageDecoderTest, RowCopiesIsOneWithoutASquasher) {
  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "ROW ONE"), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(snapshots_[0].row_copies[1], 1);
  EXPECT_EQ(snapshots_[0].row_copies[2], 0);
}

TEST_F(TeletextPageDecoderTest, SquasherRepairsAParityDamagedByte) {
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  auto damaged = make_row(1, 1, "HELLO");
  damaged[2] ^= 0x01;  // break odd parity on the leading display byte

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(damaged, 1);
  decoder_.process_packet(make_header(1, 0x00, 0, {}), 2);
  decoder_.process_packet(make_row(1, 1, "HELLO"), 3);
  decoder_.process_packet(make_time_filling_header(1), 4);

  ASSERT_GE(snapshots_.size(), 2u);
  const auto& combined = snapshots_.back();
  EXPECT_EQ(row_text(combined, 1), "HELLO");
  EXPECT_FALSE(combined.cells[1][0].parity_error)
      << "the damaged byte was not repaired from the clean copy";
}

// C4 replaces the page rather than updating it (EN 300 706 §9.3.1.3 Table 2),
// so accumulated copies must not bleed into the new content.
TEST_F(TeletextPageDecoderTest, SquasherDropsAccumulatedRowsOnErasePage) {
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "OLD ONE"), 1);
  decoder_.process_packet(make_row(1, 2, "OLD TWO"), 2);
  // Erase (C4) then a page that only uses row 1.
  HeaderFlags erase;
  erase.erase_page = true;
  decoder_.process_packet(make_header(1, 0x00, 0, erase), 3);
  decoder_.process_packet(make_row(1, 1, "NEW ONE"), 4);
  decoder_.process_packet(make_time_filling_header(1), 5);

  ASSERT_EQ(snapshots_.size(), 2u);
  const auto& fresh = snapshots_.back();
  EXPECT_EQ(row_text(fresh, 1), "NEW ONE");
  EXPECT_EQ(row_text(fresh, 2), "") << "erased row survived the erase";
  EXPECT_FALSE(fresh.row_received[2]);
}

// The copies from before an erase are separated from those after it by the
// key's erase_epoch, not deleted: a consumer replaying the same stream reaches
// the same epoch at the same packet and can still ask about the earlier run.
TEST_F(TeletextPageDecoderTest, ErasePageSeparatesRunsWithoutDiscardingThem) {
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "OLD ONE"), 1);
  HeaderFlags erase;
  erase.erase_page = true;
  decoder_.process_packet(make_header(1, 0x00, 0, erase), 2);
  decoder_.process_packet(make_row(1, 1, "NEW ONE"), 3);

  const orc::TeletextPageKey before{1, 0x00, 0, 0};
  const orc::TeletextPageKey after{1, 0x00, 0, 1};
  ASSERT_EQ(squasher.copy_count(before, 1), 1u);
  ASSERT_EQ(squasher.copy_count(after, 1), 1u);

  const auto old_row = squasher.squashed_row(before, 1);
  ASSERT_TRUE(old_row.has_value());
  EXPECT_EQ((*old_row)[0], orc::teletext_odd_parity_encode('O'));
  const auto new_row = squasher.squashed_row(after, 1);
  ASSERT_TRUE(new_row.has_value());
  EXPECT_EQ((*new_row)[0], orc::teletext_odd_parity_encode('N'));
}

// Erasing one page must not orphan the copies of another page carried in the
// same magazine: the epoch is counted per sub-page, not per magazine.
TEST_F(TeletextPageDecoderTest, ErasePageLeavesOtherPagesOfTheMagazineAlone) {
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  decoder_.process_packet(make_header(1, 0x10, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, "PAGE TEN"), 1);
  HeaderFlags erase;
  erase.erase_page = true;
  decoder_.process_packet(make_header(1, 0x20, 0, erase), 2);
  decoder_.process_packet(make_row(1, 1, "PAGE TWENTY"), 3);
  // Page 0x10 comes round again and must land in the epoch it started in.
  decoder_.process_packet(make_header(1, 0x10, 0, {}), 4);
  decoder_.process_packet(make_row(1, 1, "PAGE TEN"), 5);

  EXPECT_EQ(squasher.copy_count(orc::TeletextPageKey{1, 0x10, 0, 0}, 1), 2u);
  EXPECT_EQ(squasher.copy_count(orc::TeletextPageKey{1, 0x20, 0, 1}, 1), 1u);
}

// A page number transmitted as a sequence of sub-pages (ETSI EN 300 706 Annex
// A.1) carries different content under each sub-code, so copies must be
// combined per sub-page: squashing them together would blend one sub-page's
// rows into another's. The sub-code is part of the squasher key, and this is
// what holds it there.
TEST_F(TeletextPageDecoderTest, SquasherCombinesCopiesPerSubpage) {
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  // Two cycles of a two-sub-page carousel on page 150.
  for (int cycle = 0; cycle < 2; ++cycle) {
    const int64_t field = cycle * 4;
    decoder_.process_packet(make_header(1, 0x50, 0x0001, {}), field);
    decoder_.process_packet(make_row(1, 1, "SUBPAGE ONE"), field + 1);
    decoder_.process_packet(make_header(1, 0x50, 0x0002, {}), field + 2);
    decoder_.process_packet(make_row(1, 1, "SUBPAGE TWO"), field + 3);
  }
  decoder_.finalize(8);

  // Each sub-page has two copies of its own row and none of its sibling's.
  const orc::TeletextPageKey first{1, 0x50, 0x0001, 0};
  const orc::TeletextPageKey second{1, 0x50, 0x0002, 0};
  EXPECT_EQ(squasher.copy_count(first, 1), 2u);
  EXPECT_EQ(squasher.copy_count(second, 1), 2u);

  const auto first_row = squasher.squashed_row(first, 1);
  ASSERT_TRUE(first_row.has_value());
  EXPECT_EQ((*first_row)[8], orc::teletext_odd_parity_encode('O'));
  const auto second_row = squasher.squashed_row(second, 1);
  ASSERT_TRUE(second_row.has_value());
  EXPECT_EQ((*second_row)[8], orc::teletext_odd_parity_encode('T'));

  // And each renders from its own copies: two confirmed rows, not four.
  ASSERT_EQ(snapshots_.size(), 4u);
  EXPECT_EQ(row_text(snapshots_[2], 1), "SUBPAGE ONE");
  EXPECT_EQ(snapshots_[2].row_copies[1], 2);
  EXPECT_EQ(row_text(snapshots_[3], 1), "SUBPAGE TWO");
  EXPECT_EQ(snapshots_[3].row_copies[1], 2);
}

// A consumer rewriting a recovered stream feeds it once to build the squasher
// and again to apply it. The second pass must not destroy what the first
// built — which is what deleting the copies on C4 used to do, leaving the
// rewrite with nothing to correct against.
TEST_F(TeletextPageDecoderTest, ReplayingAStreamPreservesTheAccumulatedCopies) {
  orc::TeletextRowSquasher squasher;

  HeaderFlags erase;
  erase.erase_page = true;
  const std::vector<std::array<uint8_t, kTeletextPacketBytes>> stream{
      make_header(1, 0x00, 0, erase),
      make_row(1, 1, "HELLO"),
      make_header(1, 0x00, 0, erase),
      make_row(1, 1, "HELLO"),
  };

  TeletextPageDecoder first;
  first.set_row_squasher(&squasher);
  for (size_t i = 0; i < stream.size(); ++i) {
    first.process_packet(stream[i], static_cast<int64_t>(i),
                         static_cast<int64_t>(i));
  }
  const size_t runs_after_first_pass = squasher.page_count();

  // Second pass, fresh decoder, same squasher, same source ids.
  TeletextPageDecoder second;
  second.set_row_squasher(&squasher);
  for (size_t i = 0; i < stream.size(); ++i) {
    second.process_packet(stream[i], static_cast<int64_t>(i),
                          static_cast<int64_t>(i));
  }

  EXPECT_EQ(squasher.page_count(), runs_after_first_pass)
      << "the replay created runs the first pass did not";
  // Each copy was replaced under its own source id, not counted again.
  EXPECT_EQ(squasher.copy_count(orc::TeletextPageKey{1, 0x00, 0, 1}, 1), 1u);
  EXPECT_EQ(squasher.copy_count(orc::TeletextPageKey{1, 0x00, 0, 2}, 1), 1u);
}

TEST_F(TeletextPageDecoderTest, RendersLevel1ColourAndMosaicAttributes) {
  // 0/1 alpha red ("Set-After"), text, 1/2 mosaic green, mosaic glyphs.
  std::string row;
  row.push_back(0x01);  // Alpha Red
  row += "AB";
  row.push_back(0x02);  // Alpha Green
  row.push_back(0x1D);  // New Background (Set-At: background = red)
  row += "C";
  row.push_back(0x13);  // Mosaic Yellow
  row.push_back(0x35);  // G1 mosaic glyph
  row.push_back(0x45);  // G1 column 4: alphanumeric capital even in mosaics

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, row), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  const auto& cells = snapshots_[0].cells[1];

  // Column 0: the colour code itself renders as a white space (Set-After).
  EXPECT_EQ(cells[0].character, 0x20);
  EXPECT_EQ(cells[0].foreground, TeletextColour::White);
  // Columns 1-2: red alphanumerics.
  EXPECT_EQ(cells[1].character, 'A');
  EXPECT_EQ(cells[1].foreground, TeletextColour::Red);
  EXPECT_FALSE(cells[1].mosaic);
  // Column 3 is the Alpha Green code (still red foreground, Set-After);
  // column 4 is New Background, Set-At: background becomes green.
  EXPECT_EQ(cells[3].foreground, TeletextColour::Red);
  EXPECT_EQ(cells[4].background, TeletextColour::Green);
  // Column 5: green 'C' on green background.
  EXPECT_EQ(cells[5].character, 'C');
  EXPECT_EQ(cells[5].foreground, TeletextColour::Green);
  EXPECT_EQ(cells[5].background, TeletextColour::Green);
  // Column 7: yellow mosaic glyph.
  EXPECT_EQ(cells[7].character, 0x35);
  EXPECT_TRUE(cells[7].mosaic);
  EXPECT_EQ(cells[7].foreground, TeletextColour::Yellow);
  // Column 8: G1 column 4/5 codes stay alphanumeric in mosaics mode.
  EXPECT_EQ(cells[8].character, 0x45);
  EXPECT_FALSE(cells[8].mosaic);
}

TEST_F(TeletextPageDecoderTest, BlackColourCodesSetABlackForeground) {
  // Mosaics Black (1/0) and Alpha Black (0/0) are colour codes like the other
  // seven of each range: they set a black foreground and select their
  // character set, and cancel conceal (EN 300 706 §12.2 Table 26).
  std::string row;
  row.push_back(0x11);  // Mosaic Red ("Set-After")
  row.push_back(0x35);  // G1 mosaic glyph, red
  row.push_back(0x10);  // Mosaics Black ("Set-After")
  row.push_back(0x3A);  // G1 mosaic glyph, black
  row.push_back(0x18);  // Conceal ("Set-At")
  row.push_back(0x00);  // Alpha Black ("Set-After"): cancels conceal
  row.push_back('Z');

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, row), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  const auto& cells = snapshots_[0].cells[1];

  // Column 1: red mosaic, as before the black code.
  EXPECT_EQ(cells[1].character, 0x35);
  EXPECT_TRUE(cells[1].mosaic);
  EXPECT_EQ(cells[1].foreground, TeletextColour::Red);
  // Column 2 is the Mosaics Black code itself: a space still in the previous
  // colour, because the code is "Set-After".
  EXPECT_EQ(cells[2].character, 0x20);
  EXPECT_EQ(cells[2].foreground, TeletextColour::Red);
  // Column 3: the same graphics, now black and still mosaics.
  EXPECT_EQ(cells[3].character, 0x3A);
  EXPECT_TRUE(cells[3].mosaic);
  EXPECT_EQ(cells[3].foreground, TeletextColour::Black);
  // Column 5 is the Alpha Black code, and is still concealed: like every
  // colour code it cancels conceal from the following character-space.
  EXPECT_TRUE(cells[5].conceal);
  // Column 6: black alphanumeric — the alpha code returned the row to the G0
  // set as well as setting the colour.
  EXPECT_EQ(cells[6].character, 'Z');
  EXPECT_FALSE(cells[6].mosaic);
  EXPECT_FALSE(cells[6].conceal);
  EXPECT_EQ(cells[6].foreground, TeletextColour::Black);
}

TEST_F(TeletextPageDecoderTest, DoubleHeight_ConsumesTheRowBelow) {
  std::string row;
  row.push_back(0x0D);  // Double Height ("Set-After")
  row += "BIG";

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, row), 1);
  decoder_.process_packet(make_row(1, 2, "IGNORED DATA"), 2);
  decoder_.process_packet(make_time_filling_header(1), 3);

  ASSERT_EQ(snapshots_.size(), 1u);
  const auto& page = snapshots_[0];
  EXPECT_TRUE(page.cells[1][1].double_height);
  EXPECT_EQ(page.cells[1][1].character, 'B');
  // The transmitted row 2 is ignored; it renders as the lower half of the
  // double-height pair (EN 300 706 §12.2 0/D).
  EXPECT_TRUE(page.cells[2][1].double_height_lower);
  EXPECT_EQ(page.cells[2][1].character, 0x20);
  EXPECT_EQ(row_text(page, 2), "");
}

TEST_F(TeletextPageDecoderTest, HoldMosaics_SubstitutesHeldCharacter) {
  std::string row;
  row.push_back(0x11);  // Mosaic Red
  row.push_back(0x1E);  // Hold Mosaics ("Set-At")
  row.push_back(0x3F);  // mosaic glyph -> becomes the held character
  row.push_back(0x12);  // Mosaic Green: displayed as the held glyph
  row.push_back(0x3A);

  decoder_.process_packet(make_header(1, 0x00, 0, {}), 0);
  decoder_.process_packet(make_row(1, 1, row), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  const auto& cells = snapshots_[0].cells[1];
  EXPECT_EQ(cells[2].character, 0x3F);
  EXPECT_TRUE(cells[2].mosaic);
  // The colour-change cell shows the held mosaic instead of a space.
  EXPECT_EQ(cells[3].character, 0x3F);
  EXPECT_TRUE(cells[3].held_mosaic);
  EXPECT_EQ(cells[3].foreground, TeletextColour::Red);
  EXPECT_EQ(cells[4].character, 0x3A);
  EXPECT_EQ(cells[4].foreground, TeletextColour::Green);
}

TEST_F(TeletextPageDecoderTest, NationalOptionSubsetReadsC12AsMostSignificant) {
  // Table 32 lists the sub-sets against C12 C13 C14 read in that order, so
  // French is 1 0 0 = 4. Byte 13 sends C12 first, i.e. in the low data bit.
  HeaderFlags flags;
  flags.national_option_subset =
      static_cast<int>(orc::TeletextNationalOption::French);

  decoder_.process_packet(make_header(1, 0x00, 0, flags), 0);
  decoder_.process_packet(make_row(1, 1, "A"), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  ASSERT_EQ(snapshots_.size(), 1u);
  EXPECT_EQ(snapshots_[0].national_option_subset,
            static_cast<int>(orc::TeletextNationalOption::French));
}

////////////////////////////////////////////////////////////////////////////////////////////

TEST(TeletextLatinG0Test, PrimarySetPositionsAreAscii) {
  // EN 300 706 §15.6.1 Table 35: every position the national option sub-sets
  // leave alone coincides with ASCII.
  const int english = static_cast<int>(orc::TeletextNationalOption::English);
  for (uint8_t code = 0x20; code < 0x7F; ++code) {
    const bool reserved = code == 0x23 || code == 0x24 || code == 0x40 ||
                          (code >= 0x5B && code <= 0x60) ||
                          (code >= 0x7B && code <= 0x7E);
    if (reserved) {
      continue;
    }
    EXPECT_EQ(
        orc::teletext_g0_to_unicode(code, orc::TeletextG0Set::Latin, english),
        static_cast<char32_t>(code))
        << "code " << std::hex << static_cast<int>(code);
  }
  // Table 35 NOTE 1: 2/0 is SPACE. NOTE 4: 7/F is a filled rectangle.
  EXPECT_EQ(
      orc::teletext_g0_to_unicode(0x20, orc::TeletextG0Set::Latin, english),
      U' ');
  EXPECT_EQ(
      orc::teletext_g0_to_unicode(0x7F, orc::TeletextG0Set::Latin, english),
      U'■');
  // Spacing attributes are not characters (§15.5).
  EXPECT_EQ(
      orc::teletext_g0_to_unicode(0x0D, orc::TeletextG0Set::Latin, english),
      U' ');
}

TEST(TeletextLatinG0Test, SubsetsSubstituteTheThirteenReservedPositions) {
  // EN 300 706 §15.6.2 Table 36, one row apiece.
  struct Case {
    orc::TeletextNationalOption subset;
    std::array<char32_t, 13> expected;
  };
  const std::array<uint8_t, 13> positions = {0x23, 0x24, 0x40, 0x5B, 0x5C,
                                             0x5D, 0x5E, 0x5F, 0x60, 0x7B,
                                             0x7C, 0x7D, 0x7E};
  const std::array<Case, 7> cases = {{
      {orc::TeletextNationalOption::English,
       {U'£', U'$', U'@', U'←', U'½', U'→', U'↑', U'#', U'—', U'¼', U'‖', U'¾',
        U'÷'}},
      {orc::TeletextNationalOption::German,
       {U'#', U'$', U'§', U'Ä', U'Ö', U'Ü', U'^', U'_', U'°', U'ä', U'ö', U'ü',
        U'ß'}},
      {orc::TeletextNationalOption::SwedishFinnishHungarian,
       {U'#', U'¤', U'É', U'Ä', U'Ö', U'Å', U'Ü', U'_', U'é', U'ä', U'ö', U'å',
        U'ü'}},
      {orc::TeletextNationalOption::Italian,
       {U'£', U'$', U'é', U'°', U'ç', U'→', U'↑', U'#', U'ù', U'à', U'ò', U'è',
        U'ì'}},
      {orc::TeletextNationalOption::French,
       {U'é', U'ï', U'à', U'ë', U'ê', U'ù', U'î', U'#', U'è', U'â', U'ô', U'û',
        U'ç'}},
      {orc::TeletextNationalOption::PortugueseSpanish,
       {U'ç', U'$', U'¡', U'á', U'é', U'í', U'ó', U'ú', U'¿', U'ü', U'ñ', U'è',
        U'à'}},
      {orc::TeletextNationalOption::CzechSlovak,
       {U'#', U'ů', U'č', U'ť', U'ž', U'ý', U'í', U'ř', U'é', U'á', U'ě', U'ú',
        U'š'}},
  }};

  for (const auto& c : cases) {
    for (size_t i = 0; i < positions.size(); ++i) {
      EXPECT_EQ(
          orc::teletext_g0_to_unicode(positions[i], orc::TeletextG0Set::Latin,
                                      static_cast<int>(c.subset)),
          c.expected[i])
          << "subset " << static_cast<int>(c.subset) << " position " << i;
    }
  }
}

TEST(TeletextLatinG0Test, UndefinedDesignationRendersAsEnglish) {
  // Table 32 defines no sub-set for C12-C14 = 1 1 1 and Table 35's own glyphs
  // at these positions apply only through a packet X/26 (Table 35 NOTE 2).
  const int undefined =
      static_cast<int>(orc::TeletextNationalOption::Undefined);
  EXPECT_EQ(
      orc::teletext_g0_to_unicode(0x23, orc::TeletextG0Set::Latin, undefined),
      U'£');
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x23, orc::TeletextG0Set::Latin, -1),
            U'£');
}

TEST(TeletextLatinG0Test, Utf8MatchesTheUnicodeMapping) {
  const int english = static_cast<int>(orc::TeletextNationalOption::English);
  EXPECT_EQ(orc::teletext_g0_to_utf8('A', orc::TeletextG0Set::Latin, english),
            "A");
  EXPECT_EQ(orc::teletext_g0_to_utf8(0x23, orc::TeletextG0Set::Latin, english),
            "£");  // 2 bytes
  EXPECT_EQ(orc::teletext_g0_to_utf8(0x5B, orc::TeletextG0Set::Latin, english),
            "←");  // 3 bytes
  EXPECT_EQ(orc::teletext_g0_to_utf8(0x7F, orc::TeletextG0Set::Latin, english),
            "■");
  EXPECT_EQ(orc::teletext_g0_to_utf8(
                0x7E, orc::TeletextG0Set::Latin,
                static_cast<int>(orc::TeletextNationalOption::German)),
            "ß");
}

////////////////////////////////////////////////////////////////////////////////////////////

namespace {

using orc::TeletextG0Set;

constexpr std::array<TeletextG0Set, 3> kCyrillicSets = {
    TeletextG0Set::Cyrillic1, TeletextG0Set::Cyrillic2,
    TeletextG0Set::Cyrillic3};

// UTF-8 of a whole transmitted string through one G0 set, which is how the
// tables are worth reading: as words rather than as code points.
std::string through(const std::string& codes, TeletextG0Set set) {
  std::string out;
  for (const char c : codes) {
    out += orc::teletext_g0_to_utf8(static_cast<uint8_t>(c), set, 0);
  }
  return out;
}

}  // namespace

TEST(TeletextCyrillicG0Test, PositionsSharedWithLatinKeepTheirAsciiMeaning) {
  // EN 300 706 §15.6.4-§15.6.6 Tables 38-40: the Cyrillic sets replace columns
  // 4 to 7 outright, but keep the digits, the punctuation of column 3 and most
  // of column 2 exactly where the Latin set has them — which is what lets a
  // clock, a page number or a time of day read the same in either alphabet.
  for (const TeletextG0Set set : kCyrillicSets) {
    for (uint8_t code = 0x20; code <= 0x3F; ++code) {
      // 2/6 is the one position in this range the sets take for a letter: the
      // block at 7/F leaves one lowercase letter homeless and it lands there.
      // Option 1 has no such letter and keeps '&'.
      if (code == 0x26 && set != TeletextG0Set::Cyrillic1) {
        continue;
      }
      EXPECT_EQ(orc::teletext_g0_to_unicode(code, set, 0),
                static_cast<char32_t>(code))
          << "set " << static_cast<int>(set) << " code " << std::hex
          << static_cast<int>(code);
    }
    // Each table's NOTE 1 and NOTE 3, as for Latin.
    EXPECT_EQ(orc::teletext_g0_to_unicode(0x20, set, 0), U' ');
    EXPECT_EQ(orc::teletext_g0_to_unicode(0x7F, set, 0), U'■');
    // Spacing attributes are not characters in any set (§15.5).
    EXPECT_EQ(orc::teletext_g0_to_unicode(0x0D, set, 0), U' ');
  }
}

TEST(TeletextCyrillicG0Test, EveryPositionIsDefinedAndTheSetsDiffer) {
  for (const TeletextG0Set set : kCyrillicSets) {
    for (uint8_t code = 0x20; code < 0x7F; ++code) {
      EXPECT_NE(orc::teletext_g0_to_unicode(code, set, 0), U'\0')
          << "set " << static_cast<int>(set) << " code " << std::hex
          << static_cast<int>(code);
    }
  }

  // The three are distinct alphabets, not one table reached three ways: 5/9,
  // 5/C and 5/F are where Tables 39 and 40 part company (Ъ/Э/Ы against І/Є/Ї)
  // and column 4 is where Table 38 does.
  EXPECT_NE(orc::teletext_g0_to_unicode(0x59, TeletextG0Set::Cyrillic2, 0),
            orc::teletext_g0_to_unicode(0x59, TeletextG0Set::Cyrillic3, 0));
  EXPECT_NE(orc::teletext_g0_to_unicode(0x40, TeletextG0Set::Cyrillic1, 0),
            orc::teletext_g0_to_unicode(0x40, TeletextG0Set::Cyrillic2, 0));
}

TEST(TeletextCyrillicG0Test, TheNationalOptionSubsetIsNotConsulted) {
  // §15.2: the sub-set replaces reserved positions of the *Latin* set. The
  // Cyrillic tables reserve none, so the header's C12-C14 bits must not reach
  // them — a Cyrillic page whose header happens to say "French" is still
  // Cyrillic.
  for (const TeletextG0Set set : kCyrillicSets) {
    for (int subset = 0; subset < 8; ++subset) {
      for (const uint8_t code : {0x23, 0x24, 0x40, 0x5B, 0x5F, 0x60, 0x7E}) {
        EXPECT_EQ(orc::teletext_g0_to_unicode(code, set, subset),
                  orc::teletext_g0_to_unicode(code, set, 0))
            << "set " << static_cast<int>(set) << " subset " << subset;
      }
    }
  }
}

// Table 39 read as the words it produces. These strings are transmitted codes
// lifted from a recovered Russian broadcast, so the test is the table against
// real traffic rather than against a second copy of itself.
TEST(TeletextCyrillicG0Test, RussianBulgarianSetReadsARecoveredBroadcast) {
  const TeletextG0Set ru = TeletextG0Set::Cyrillic2;
  EXPECT_EQ(through("Dawlenie", ru), "Давление");
  EXPECT_EQ(through("Wtornik", ru), "Вторник");
  EXPECT_EQ(through("Sreda", ru), "Среда");
  EXPECT_EQ(through("Peterburg", ru), "Петербург");
  EXPECT_EQ(through("WOL[EBNAQ LINIQ", ru), "ВОЛШЕБНАЯ ЛИНИЯ");
  EXPECT_EQ(through("OBOZNA^ENIQ", ru), "ОБОЗНАЧЕНИЯ");
  EXPECT_EQ(through("MANU\\LA", ru), "МАНУЭЛА");
  // The two positions the block at 7/F displaced, and the hard sign that took
  // the place KOI-7 gives Ы.
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x26, ru, 0), U'ы');
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x5F, ru, 0), U'Ы');
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x59, ru, 0), U'Ъ');
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x79, ru, 0), U'ъ');
}

TEST(TeletextCyrillicG0Test, UkrainianSetCarriesTheLettersRussianDoesNot) {
  // Table 40 against Table 39: І, Є and Ї take the positions Ъ, Э and Ы hold,
  // and ї takes ы's place at 2/6.
  const TeletextG0Set ua = TeletextG0Set::Cyrillic3;
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x59, ua, 0), U'І');
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x5C, ua, 0), U'Є');
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x5F, ua, 0), U'Ї');
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x79, ua, 0), U'і');
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x7C, ua, 0), U'є');
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x26, ua, 0), U'ї');
  // 2/6 is where the words most in need of it put their letter.
  EXPECT_EQ(through("Ukra&na", ua), "Україна");
  EXPECT_EQ(through("Ki&w", ua), "Київ");
  // Everything else is Table 39's layout, so a word using none of the six
  // differing positions reads identically in both.
  EXPECT_EQ(through("Petro", ua), through("Petro", TeletextG0Set::Cyrillic2));
  EXPECT_EQ(through("Petro", ua), "Петро");
}

TEST(TeletextCyrillicG0Test, SerbianCroatianSetIsTheOneWithLatinPunctuation) {
  // Table 38 keeps '&' at 2/6 where the other two take it for a letter, and is
  // the only one whose column 4 opens on Ч rather than Ю.
  const TeletextG0Set sr = TeletextG0Set::Cyrillic1;
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x26, sr, 0), U'&');
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x40, sr, 0), U'Ч');
  EXPECT_EQ(through("BEOGRAD", sr), "БЕОГРАД");
  EXPECT_EQ(through("Hrvatska", sr), "Хрватска");
  // The Macedonian letters Table 38 carries, and the digraphs at 5/8-5/9.
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x51, sr, 0), U'Ќ');
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x57, sr, 0), U'Ѓ');
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x58, sr, 0), U'Љ');
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x59, sr, 0), U'Њ');
  // Table 38's block at 7/F sits where lowercase џ would be, so the set has Џ
  // and no lowercase form of it. That is the table as printed.
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x5F, sr, 0), U'Џ');
  EXPECT_EQ(orc::teletext_g0_to_unicode(0x7F, sr, 0), U'■');
}

TEST(TeletextCyrillicG0Test, SetNamesRoundTrip) {
  for (const TeletextG0Set set :
       {TeletextG0Set::Latin, TeletextG0Set::Cyrillic1,
        TeletextG0Set::Cyrillic2, TeletextG0Set::Cyrillic3}) {
    const std::string name = orc::to_string(set);
    EXPECT_FALSE(name.empty());
    const auto parsed = orc::teletext_g0_set_from_string(name);
    ASSERT_TRUE(parsed.has_value()) << name;
    EXPECT_EQ(*parsed, set) << name;
  }
  EXPECT_FALSE(orc::teletext_g0_set_from_string("Klingon").has_value());
  EXPECT_FALSE(orc::teletext_g0_set_from_string("").has_value());
}

////////////////////////////////////////////////////////////////////////////////////////////

namespace {

// Subtitle-style row: boxed text per the C5/C6 double Start Box convention
// (EN 300 706 §12.2 0/B).
std::string boxed(const std::string& text) {
  std::string row;
  row.push_back(0x0B);
  row.push_back(0x0B);
  row += text;
  row.push_back(0x0A);
  return row;
}

HeaderFlags subtitle_header_flags(bool erase) {
  HeaderFlags flags;
  flags.subtitle = true;
  flags.erase_page = erase;
  return flags;
}

}  // namespace

class TeletextSubtitleCueTest : public ::testing::Test {
 protected:
  TeletextSubtitleCueTest() { EXPECT_TRUE(decoder_.set_subtitle_page("888")); }

  // Transmission magazine 0 carries displayed magazine 8 (page 888).
  static constexpr int kMagazine = 0;
  static constexpr int kPage = 0x88;

  TeletextPageDecoder decoder_;
};

TEST_F(TeletextSubtitleCueTest, SetSubtitlePage_RejectsMalformedPage) {
  TeletextPageDecoder decoder;
  EXPECT_FALSE(decoder.set_subtitle_page("98"));
  EXPECT_FALSE(decoder.set_subtitle_page("hello"));
}

TEST_F(TeletextSubtitleCueTest, PageArrivalOpensCueAndFinalizeClosesIt) {
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 10);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("HELLO SUBTITLE")), 12);
  decoder_.process_packet(make_time_filling_header(kMagazine), 14);
  EXPECT_TRUE(decoder_.subtitle_cues().empty());  // cue still on screen

  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "HELLO SUBTITLE");
  EXPECT_EQ(cues[0].start_field_index, 12);
  EXPECT_EQ(cues[0].end_field_index, 100);
}

TEST_F(TeletextSubtitleCueTest, CueTextUsesThePagesNationalOptionSubset) {
  // Code 2/3 is "£" on an English service and "#" on a German one
  // (EN 300 706 §15.6.2 Table 36); the cue carries the character the page
  // displays, UTF-8 encoded, not the transmitted code as ASCII.
  HeaderFlags flags = subtitle_header_flags(true);
  decoder_.process_packet(make_header(kMagazine, kPage, 0, flags), 0);
  decoder_.process_packet(make_row(kMagazine, 20,
                                   boxed("COST \x23"
                                         "5")),
                          2);

  flags.national_option_subset =
      static_cast<int>(orc::TeletextNationalOption::German);
  decoder_.process_packet(make_header(kMagazine, kPage, 0, flags), 40);
  decoder_.process_packet(make_row(kMagazine, 20,
                                   boxed("KOST \x23"
                                         "5")),
                          42);
  decoder_.process_packet(make_time_filling_header(kMagazine), 44);
  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 2u);
  EXPECT_EQ(cues[0].text, "COST £5");
  EXPECT_EQ(cues[1].text, "KOST #5");
}

TEST_F(TeletextSubtitleCueTest, EraseHeaderWithEmptyPageClearsTheCue) {
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 0);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("FIRST CUE")), 2);
  decoder_.process_packet(make_time_filling_header(kMagazine), 4);

  // Erase transmission with no rows: clears the display at header arrival.
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 50);
  decoder_.process_packet(make_time_filling_header(kMagazine), 52);
  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "FIRST CUE");
  EXPECT_EQ(cues[0].start_field_index, 2);
  EXPECT_EQ(cues[0].end_field_index, 50);
}

TEST_F(TeletextSubtitleCueTest, ChangedTextReplacesTheOpenCue) {
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 0);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("FIRST CUE")), 2);

  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 40);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("SECOND CUE")), 42);
  decoder_.process_packet(make_time_filling_header(kMagazine), 44);
  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 2u);
  EXPECT_EQ(cues[0].text, "FIRST CUE");
  EXPECT_EQ(cues[0].start_field_index, 2);
  EXPECT_EQ(cues[0].end_field_index, 40);
  EXPECT_EQ(cues[1].text, "SECOND CUE");
  EXPECT_EQ(cues[1].start_field_index, 42);
  EXPECT_EQ(cues[1].end_field_index, 100);
}

TEST_F(TeletextSubtitleCueTest, UnchangedRetransmissionKeepsTheCueOnScreen) {
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 0);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("STEADY CUE")), 2);

  // Carousel repeat of the identical page must not split the cue. The
  // repeat is a non-erase retransmission (rows retained).
  HeaderFlags repeat = subtitle_header_flags(false);
  decoder_.process_packet(make_header(kMagazine, kPage, 0, repeat), 40);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("STEADY CUE")), 42);
  decoder_.process_packet(make_time_filling_header(kMagazine), 44);
  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "STEADY CUE");
  EXPECT_EQ(cues[0].start_field_index, 2);
  EXPECT_EQ(cues[0].end_field_index, 100);
}

TEST_F(TeletextSubtitleCueTest, HeaderWithoutSubtitleFlagClearsTheCue) {
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 0);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("VANISHING")), 2);
  decoder_.process_packet(make_time_filling_header(kMagazine), 4);

  // The page reappears without C6: no longer a subtitle page → clear.
  decoder_.process_packet(make_header(kMagazine, kPage, 0, {}), 60);
  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].end_field_index, 60);
}

TEST_F(TeletextSubtitleCueTest, OtherPagesDoNotEmitCues) {
  decoder_.process_packet(
      make_header(kMagazine, 0x77, 0, subtitle_header_flags(true)), 0);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("WRONG PAGE")), 2);
  decoder_.process_packet(make_time_filling_header(kMagazine), 4);
  decoder_.finalize(100);

  EXPECT_TRUE(decoder_.subtitle_cues().empty());
}

TEST_F(TeletextSubtitleCueTest, UnboxedTextIsExcludedFromSubtitlePageCues) {
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 0);
  // "NOISE" is outside the boxed region and must not leak into the cue
  // (EN 300 706 §12.2 0/B: characters outside the box are not displayed).
  decoder_.process_packet(
      make_row(kMagazine, 20, boxed("BOXED TEXT") + " NOISE"), 2);
  decoder_.process_packet(make_time_filling_header(kMagazine), 4);
  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "BOXED TEXT");
}

TEST_F(TeletextSubtitleCueTest, MultiRowSubtitlesJoinWithNewlines) {
  decoder_.process_packet(
      make_header(kMagazine, kPage, 0, subtitle_header_flags(true)), 0);
  decoder_.process_packet(make_row(kMagazine, 20, boxed("LINE ONE")), 1);
  decoder_.process_packet(make_row(kMagazine, 22, boxed("LINE TWO")), 2);
  decoder_.process_packet(make_time_filling_header(kMagazine), 3);
  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "LINE ONE\nLINE TWO");
}

// ---------------------------------------------------------------------------
// 525-line WST (ITU-R BT.653 Table 1b): a 34-byte packet, so 32 display bytes
// per row packet and 24 header-text characters, with the remaining 8 columns
// of each row carried in the service's row-extension packets. Everything the
// decoder reads by position — MRAG, page number, sub-code, control bits — is at
// the same offsets, so these build on the 625-line helpers and stop short.
// ---------------------------------------------------------------------------

using orc::kTeletext525PacketBytes;

// Display columns of a 525-line page: the packet less its two MRAG bytes.
constexpr int k525Columns = static_cast<int>(kTeletext525PacketBytes) - 2;

std::array<uint8_t, kTeletextPacketBytes> make_525_header(
    int magazine, int page_number, int subcode, HeaderFlags flags = {},
    const std::string& header_text = "") {
  auto packet = make_header(magazine, page_number, subcode, flags, header_text);
  // Bytes past the 34 the service transmits were never sent.
  for (size_t i = kTeletext525PacketBytes; i < kTeletextPacketBytes; ++i) {
    packet[i] = 0;
  }
  return packet;
}

std::array<uint8_t, kTeletextPacketBytes> make_525_row(
    int magazine, int row, const std::string& text) {
  auto packet = make_row(magazine, row, text);
  for (size_t i = kTeletext525PacketBytes; i < kTeletextPacketBytes; ++i) {
    packet[i] = 0;
  }
  return packet;
}

std::array<uint8_t, kTeletextPacketBytes> make_525_time_filling_header(
    int magazine) {
  return make_525_header(magazine, 0xFF, 0x3F7F & 0x1FFF);
}

// A 525-line row-extension packet: addressed to magazine|4, carrying columns
// 32-39 of the block of four display rows |packet_number| identifies, as four
// groups of eight (see the TeletextPageDecoder class comment). |tails| supplies
// the groups in row order; missing or short ones pad with spaces.
std::array<uint8_t, kTeletextPacketBytes> make_525_extension(
    int magazine, int packet_number, const std::vector<std::string>& tails) {
  std::array<uint8_t, kTeletextPacketBytes> packet{};
  const auto mrag = make_mrag(magazine | 0x4, packet_number);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  for (size_t group = 0; group < 4; ++group) {
    const std::string& tail = group < tails.size() ? tails[group] : "";
    for (size_t i = 0; i < 8; ++i) {
      const char c = i < tail.size() ? tail[i] : ' ';
      packet[2 + group * 8 + i] =
          orc::teletext_odd_parity_encode(static_cast<uint8_t>(c));
    }
  }
  return packet;
}

class Teletext525PageDecoderTest : public TeletextPageDecoderTest {
 protected:
  // Every packet of a 525-line stream carries its own length; the decoder
  // takes the row width from it.
  void feed(const std::array<uint8_t, kTeletextPacketBytes>& packet,
            int64_t field_index,
            int64_t source = TeletextPageDecoder::kAutoSource) {
    decoder_.process_packet(packet, field_index, source,
                            /*confidence=*/nullptr, kTeletext525PacketBytes);
  }

  // Show the decoder that magazine|4 carries row extensions rather than pages
  // of its own, which it learns from a page's worth of block-numbered packets
  // (see the TeletextPageDecoder class comment). Fed before any header, so the
  // packets themselves are dropped for want of an open page and contribute
  // nothing to what the tests then assert.
  void settle_extension_carrier(int magazine) {
    for (const int block : {1, 4, 8, 12, 16, 20}) {
      feed(make_525_extension(magazine, block, {}), -1);
    }
  }
};

TEST_F(Teletext525PageDecoderTest, ShortPacketsStillGiveTheFortyColumnGrid) {
  // The display grid is 40 columns whatever the packet length; a service that
  // fills only 32 of them leaves the rest blank, as it would on a receiver.
  feed(make_525_header(1, 0x00, 0, {}, "ELECTRA NEWS"), 0);
  feed(make_525_row(1, 1, "TOP STORY"), 1);
  feed(make_525_time_filling_header(1), 2);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.front();
  EXPECT_EQ(page.columns, TeletextPageSnapshot::kColumns);
  EXPECT_EQ(page.magazine, 1);
  EXPECT_EQ(page.page_number, 0x00);
  EXPECT_EQ(row_text(page, 1), "TOP STORY");
}

TEST_F(Teletext525PageDecoderTest, HeaderTextStopsAtTheServiceWidth) {
  // 24 header-text characters from column 8, not 32 (Table 1b §3.4 leaves a
  // 32-byte data block, of which the header spends 8 on addressing).
  const std::string text = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
  feed(make_525_header(1, 0x00, 0, {}, text), 0);
  feed(make_525_time_filling_header(1), 1);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.front();
  EXPECT_EQ(row_text(page, 0), "        " + text.substr(0, k525Columns - 8));
}

TEST_F(Teletext525PageDecoderTest, ColumnsBeyondTheServiceWidthStayBlank) {
  // The bytes past the packet were never transmitted; they must not surface as
  // content, and — being zero — must not surface as parity damage either.
  feed(make_525_header(1, 0x00, 0), 0);
  feed(make_525_row(1, 5, std::string(40, 'X')), 1);
  feed(make_525_time_filling_header(1), 2);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.front();
  EXPECT_EQ(row_text(page, 5), std::string(k525Columns, 'X'));
  for (int column = k525Columns; column < TeletextPageSnapshot::kColumns;
       ++column) {
    const auto& cell = page.cells[5][static_cast<size_t>(column)];
    EXPECT_EQ(cell.character, 0x20) << "column " << column;
    EXPECT_FALSE(cell.parity_error) << "column " << column;
  }
}

TEST_F(Teletext525PageDecoderTest, SubtitleTextStopsAtTheServiceWidth) {
  HeaderFlags flags;
  flags.subtitle = true;
  ASSERT_TRUE(decoder_.set_subtitle_page("100"));
  feed(make_525_header(1, 0x00, 0, flags), 0);
  // Start Box / End Box around the text (EN 300 706 §12.2 0/A-0/B).
  std::string boxed_row;
  boxed_row.push_back(0x0B);
  boxed_row.push_back(0x0B);
  boxed_row += "ELECTRA";
  boxed_row.push_back(0x0A);
  feed(make_525_row(1, 20, boxed_row), 1);
  feed(make_525_time_filling_header(1), 2);
  decoder_.finalize(100);

  const auto& cues = decoder_.subtitle_cues();
  ASSERT_EQ(cues.size(), 1u);
  EXPECT_EQ(cues[0].text, "ELECTRA");
}

TEST_F(Teletext525PageDecoderTest, RowExtensionPacketsCompleteTheFortyColumns) {
  settle_extension_carrier(1);
  feed(make_525_header(1, 0x00, 0), 0);
  feed(make_525_row(1, 4, "The San Francisco 49ers have won"), 1);
  feed(make_525_row(1, 5, "NFL title game 28-3 over the Chi"), 2);
  feed(make_525_extension(1, 4, {" the", "cago"}), 3);
  feed(make_525_time_filling_header(1), 4);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.front();
  EXPECT_EQ(page.columns, TeletextPageSnapshot::kColumns);
  EXPECT_EQ(row_text(page, 4), "The San Francisco 49ers have won the");
  EXPECT_EQ(row_text(page, 5), "NFL title game 28-3 over the Chicago");
}

// An extension packet is addressed the same way a display row is, so it is
// refused on the same terms: a mis-corrected address puts eight columns of some
// other page across four rows of this one, and nothing downstream can tell.
TEST_F(Teletext525PageDecoderTest, ARowExtensionRejectsACorrectedAddress) {
  settle_extension_carrier(1);
  feed(make_525_header(1, 0x00, 0), 0);
  feed(make_525_row(1, 4, "The San Francisco 49ers have won"), 1);
  auto extension = make_525_extension(1, 4, {" the", "cago"});
  extension[1] ^= 0x10;  // single-bit error in the packet-number MRAG byte
  feed(extension, 2);
  feed(make_525_time_filling_header(1), 3);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.front();
  EXPECT_EQ(row_text(page, 4), "The San Francisco 49ers have won");
}

TEST_F(Teletext525PageDecoderTest, TheFirstBlockIsNumberedOneAndServesRowZero) {
  settle_extension_carrier(1);
  // The extension packet number identifies a block of four rows, not a row, so
  // the first block's packets carry 1 — 0 being the page header's own number —
  // and serve rows 0 to 3. It must complete the header rather than being read
  // as a page header, which in serial mode would terminate every open page.
  HeaderFlags flags;
  flags.magazine_serial = true;
  feed(make_525_header(1, 0x25, 0, flags, "20:37:21 Mon Jan  9 P125"), 0);
  feed(make_525_extension(1, 1, {" ELECTRA", "row one", "", "row three"}), 1);
  feed(make_525_row(1, 1, "TOP STORY"), 2);
  feed(make_525_row(1, 3, std::string(32, 'X')), 3);
  feed(make_525_time_filling_header(1), 4);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.front();
  EXPECT_EQ(page.page_number, 0x25);
  EXPECT_EQ(row_text(page, 0), "        20:37:21 Mon Jan  9 P125 ELECTRA");
  EXPECT_EQ(row_text(page, 1), "TOP STORY                       row one");
  EXPECT_EQ(row_text(page, 3), std::string(32, 'X') + "row thre");
}

TEST_F(Teletext525PageDecoderTest, SixBlocksCoverTheWholePage) {
  settle_extension_carrier(1);
  // The six packet numbers the scheme uses tile rows 0 to 23 exactly once —
  // which is what says packet 1 serves rows 0-3 rather than 1-4, the reading
  // that would leave row 0 unserved and row 4 claimed twice.
  feed(make_525_header(1, 0x00, 0), 0);
  for (int row = 1; row < TeletextPageSnapshot::kRows; ++row) {
    feed(make_525_row(1, row, std::string(32, 'X')), 1);
  }
  for (const int block : {1, 4, 8, 12, 16, 20}) {
    feed(make_525_extension(
             1, block,
             {std::to_string(block) + "a", std::to_string(block) + "b",
              std::to_string(block) + "c", std::to_string(block) + "d"}),
         2);
  }
  feed(make_525_time_filling_header(1), 3);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.front();
  const char* kGroup = "abcd";
  for (const int block : {1, 4, 8, 12, 16, 20}) {
    const int first_row = block == 1 ? 0 : block;
    for (int group = 0; group < 4; ++group) {
      const int row = first_row + group;
      const std::string expected =
          std::to_string(block) + std::string(1, kGroup[group]);
      const std::string actual = row_text(page, row);
      EXPECT_EQ(actual.substr(actual.size() - expected.size()), expected)
          << "row " << row << " read \"" << actual << "\"";
    }
  }
}

TEST_F(Teletext525PageDecoderTest, ExtensionColumnsSurviveARollingHeader) {
  settle_extension_carrier(1);
  // A re-sent header rewrites row 0's own display bytes; the extension columns
  // came from another packet and it says nothing about them.
  feed(make_525_header(1, 0x25, 0, {}, "20:37:21 Mon Jan  9 P125"), 0);
  feed(make_525_extension(1, 1, {" ELECTRA"}), 1);
  feed(make_525_header(1, 0x25, 0, {}, "20:37:22 Mon Jan  9 P125"), 2);
  feed(make_525_time_filling_header(1), 3);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(row_text(snapshots_.back(), 0),
            "        20:37:22 Mon Jan  9 P125 ELECTRA");
}

TEST_F(Teletext525PageDecoderTest,
       ADamagedExtensionByteNeverReplacesACleanOne) {
  settle_extension_carrier(1);
  feed(make_525_header(1, 0x00, 0), 0);
  feed(make_525_row(1, 8, std::string(32, 'X')), 1);
  feed(make_525_extension(1, 8, {"ABCDEFGH"}), 2);
  auto damaged = make_525_extension(1, 8, {"abcdefgh"});
  damaged[2] ^= 0x01;  // breaks odd parity on the first extension byte
  feed(damaged, 3);
  feed(make_525_time_filling_header(1), 4);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.front();
  // The clean 'A' stands; the rest of the packet was undamaged and replaces.
  EXPECT_EQ(row_text(page, 8), std::string(32, 'X') + "Abcdefgh");
  EXPECT_FALSE(page.cells[8][32].parity_error);
}

TEST_F(Teletext525PageDecoderTest, RowsWithNoExtensionKeepBlankColumns) {
  settle_extension_carrier(1);
  // Widening the page must not turn the columns of an unextended row into
  // whatever the short packet left behind.
  feed(make_525_header(1, 0x00, 0), 0);
  feed(make_525_row(1, 3, std::string(32, 'X')), 1);
  feed(make_525_row(1, 20, std::string(32, 'Y')), 2);
  feed(make_525_extension(1, 20, {"tail"}), 3);
  feed(make_525_time_filling_header(1), 4);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.front();
  EXPECT_EQ(page.columns, TeletextPageSnapshot::kColumns);
  EXPECT_EQ(row_text(page, 3), std::string(32, 'X'));
  for (int column = 32; column < TeletextPageSnapshot::kColumns; ++column) {
    const auto& cell = page.cells[3][static_cast<size_t>(column)];
    EXPECT_EQ(cell.character, 0x20) << "column " << column;
    EXPECT_FALSE(cell.parity_error) << "column " << column;
  }
}

TEST_F(Teletext525PageDecoderTest, ExtensionColumnsSurviveRowSquashing) {
  settle_extension_carrier(1);
  // With a squasher attached the head columns come from the combined copies;
  // the extension columns are not display rows and must still reach the page.
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  feed(make_525_header(1, 0x00, 0), 0);
  feed(make_525_row(1, 6, "Mirrors have been made in Venice"), 1);
  feed(make_525_extension(1, 4, {"", "", ", Italy,"}), 2);
  feed(make_525_header(1, 0x00, 0), 3);
  feed(make_525_row(1, 6, "Mirrors have been made in Venice"), 4);
  feed(make_525_extension(1, 4, {"", "", ", Italy,"}), 5);
  feed(make_525_time_filling_header(1), 6);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(row_text(snapshots_.back(), 6),
            "Mirrors have been made in Venice, Italy,");
}

TEST_F(Teletext525PageDecoderTest, RepeatedExtensionsCorrectEachOther) {
  settle_extension_carrier(1);
  // The point of squashing the extension columns: a damaged copy loses to the
  // clean copies of the same columns, exactly as a damaged display row does.
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  for (int pass = 0; pass < 3; ++pass) {
    const int64_t field = pass * 4;
    feed(make_525_header(1, 0x00, 0), field);
    feed(make_525_row(1, 8, std::string(32, 'X')), field + 1);
    auto extension = make_525_extension(1, 8, {"RECOVERY"});
    if (pass == 1) {
      // One transmission arrives damaged in three of the eight columns.
      extension[2] = 0x00;
      extension[3] = 0x00;
      extension[4] = 0x00;
    }
    feed(extension, field + 2);
  }
  feed(make_525_time_filling_header(1), 100);
  decoder_.finalize(200);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.back();
  EXPECT_EQ(row_text(page, 8), std::string(32, 'X') + "RECOVERY");
  for (int column = 32; column < TeletextPageSnapshot::kColumns; ++column) {
    EXPECT_FALSE(page.cells[8][static_cast<size_t>(column)].parity_error)
        << "column " << column;
  }
}

TEST_F(Teletext525PageDecoderTest, TheHeaderExtensionIsSquashedNotItsClock) {
  settle_extension_carrier(1);
  // The header's own display bytes carry a live clock and cannot be combined
  // across transmissions, but the columns its extension packet brings hold the
  // service name and can — which matters because they are otherwise the one
  // part of a page that never gets the benefit of a repeat.
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  for (int pass = 0; pass < 3; ++pass) {
    const int64_t field = pass * 4;
    feed(make_525_header(1, 0x00, 0, {},
                         "20:37:2" + std::to_string(pass) + " Mon Jan  9 P100"),
         field);
    auto extension = make_525_extension(1, 1, {" ELECTRA"});
    if (pass == 1) {
      extension[3] ^= 0x01;  // breaks odd parity on the name's second letter
      extension[4] ^= 0x01;
    }
    feed(extension, field + 1, field + 1);
  }
  feed(make_525_time_filling_header(1), 100);
  decoder_.finalize(200);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.back();
  // The clock is the last one transmitted, the name the combined copies.
  EXPECT_EQ(row_text(page, 0), "        20:37:22 Mon Jan  9 P100 ELECTRA");
  for (int column = 32; column < TeletextPageSnapshot::kColumns; ++column) {
    EXPECT_FALSE(page.cells[0][static_cast<size_t>(column)].parity_error)
        << "column " << column;
  }
  // And it is still not counted as a squashed row: row 0 rests on its header.
  EXPECT_EQ(page.row_copies[0], 0);
}

TEST_F(Teletext525PageDecoderTest, ExtensionCopiesDoNotCountAsRowCopies) {
  settle_extension_carrier(1);
  // "How many times was this row transmitted" means the display packets; the
  // packets that only extend it must not inflate the count a consumer weighs
  // its confidence in the row by.
  orc::TeletextRowSquasher squasher;
  decoder_.set_row_squasher(&squasher);

  for (int pass = 0; pass < 2; ++pass) {
    const int64_t field = pass * 4;
    feed(make_525_header(1, 0x00, 0), field);
    feed(make_525_row(1, 8, std::string(32, 'X')), field + 1);
    feed(make_525_extension(1, 8, {"tail"}), field + 2);
  }
  feed(make_525_time_filling_header(1), 100);
  decoder_.finalize(200);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(snapshots_.back().row_copies[8], 2);
}

TEST_F(Teletext525PageDecoderTest, AShortPacketServiceHasNoBlastThroughRegion) {
  feed(make_525_header(1, 0x00, 0), 0);
  feed(make_525_time_filling_header(1), 1);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_FALSE(snapshots_.front().mosaic_blast_through);
}

TEST_F(TeletextPageDecoderTest, A625LineServiceKeepsItsBlastThroughRegion) {
  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(make_time_filling_header(1), 1);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_TRUE(snapshots_.front().mosaic_blast_through);
}

TEST_F(Teletext525PageDecoderTest, AMagazineWithPagesOfItsOwnIsNotACarrier) {
  // Magazines 4-7 are only borrowed for row extensions where the service is not
  // using them for pages. This one is: it sends headers, so its packets are its
  // own page's rows and must be decoded as such.
  feed(make_525_header(4, 0x00, 0, {}, "CLOCK CRACKER 1"), 0);
  feed(make_525_row(4, 4, std::string(32, 'Z')), 1);
  feed(make_525_time_filling_header(4), 2);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.front();
  EXPECT_EQ(page.magazine, 4);
  EXPECT_EQ(row_text(page, 0), "        CLOCK CRACKER 1");
  EXPECT_EQ(row_text(page, 4), std::string(32, 'Z'));
}

TEST_F(Teletext525PageDecoderTest, ADisplayRowNumberStopsTheCarrierReading) {
  // A magazine that sends a packet numbered anything but a block number is
  // transmitting rows, so the evidence for it being a carrier starts again —
  // otherwise the block-numbered rows of a page magazine would be taken for
  // extensions before its first header arrived.
  for (int pass = 0; pass < 4; ++pass) {
    for (const int row : {1, 2, 3, 4, 5, 6, 7, 8}) {
      feed(make_525_extension(1, row, {"XXXXXXXX"}), pass);
    }
  }
  feed(make_525_header(1, 0x00, 0), 10);
  feed(make_525_row(1, 4, std::string(32, 'Y')), 11);
  feed(make_525_time_filling_header(1), 12);
  decoder_.finalize(20);

  ASSERT_FALSE(snapshots_.empty());
  // Never settled as a carrier, so nothing reached the page's columns 32-39.
  EXPECT_EQ(row_text(snapshots_.front(), 4), std::string(32, 'Y'));
}

TEST_F(Teletext525PageDecoderTest, AMisreadAddressDoesNotUnsettleACarrier) {
  // The MRAG is Hamming 8/4, which mis-corrects on a burst, so a settled
  // carrier does occasionally address a packet to something that is not a
  // block. Taking that as evidence of a change of role cost the six packets
  // that followed it — a whole page's extensions — on real media.
  settle_extension_carrier(1);
  feed(make_525_extension(1, 7, {"MISREAD"}), 0);   // not a block number
  feed(make_525_extension(1, 21, {"MISREAD"}), 0);  // nor this
  feed(make_525_header(1, 0x00, 0), 1);
  feed(make_525_row(1, 8, std::string(32, 'X')), 2);
  feed(make_525_extension(1, 8, {"recovery"}), 3);
  feed(make_525_time_filling_header(1), 4);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(row_text(snapshots_.front(), 8), std::string(32, 'X') + "recovery");
}

TEST_F(Teletext525PageDecoderTest, AMisreadHeaderDoesNotUnsettleACarrier) {
  // Same failure from the other side: one packet of a settled carrier whose
  // address mis-corrected to X/0 used to mark the magazine as carrying pages
  // of its own, which disabled its extensions for the rest of the recording.
  settle_extension_carrier(1);
  auto misread = make_525_extension(1, 0, {"MISREAD"});
  feed(misread, 0);
  feed(make_525_header(1, 0x00, 0), 1);
  feed(make_525_row(1, 8, std::string(32, 'X')), 2);
  feed(make_525_extension(1, 8, {"recovery"}), 3);
  feed(make_525_time_filling_header(1), 4);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(snapshots_.front().magazine, 1);
  EXPECT_EQ(row_text(snapshots_.front(), 8), std::string(32, 'X') + "recovery");
}

TEST_F(Teletext525PageDecoderTest, ACorrectedHeaderDoesNotClaimACarrier) {
  // The reading that a magazine carries pages is made once and holds for the
  // rest of the recording, so it takes a header that arrived as transmitted.
  // On the 1984 Keyfax capture a corrupted packet 121 packets in corrected into
  // a header for a page of magazine 7 — magazine 3's extension carrier — while
  // the carrier was still four block numbers into the six that settle it, and
  // every page of magazine 3 lost its columns 32-39 for the whole recording.
  for (const int block : {1, 4, 8, 12}) {
    feed(make_525_extension(3, block, {}), -1);
  }
  auto corrected = make_525_header(7, 0x30, 0, {}, "NOT A PAGE");
  corrected[0] ^= 0x01;  // single-bit error: corrected back to X/0 of mag 7
  feed(corrected, -1);

  // The evidence starts again, as it does for any packet not numbered a block,
  // and the next cycle settles the carrier: the damage is one page's worth of
  // extensions rather than the rest of the recording's.
  settle_extension_carrier(3);
  feed(make_525_header(3, 0x05, 0), 0);
  feed(make_525_row(3, 8, std::string(32, 'X')), 1);
  feed(make_525_extension(3, 8, {"recovery"}), 2);
  feed(make_525_time_filling_header(3), 3);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& page = snapshots_.front();
  EXPECT_EQ(page.magazine, 3);
  EXPECT_EQ(page.page_number, 0x05);
  EXPECT_EQ(row_text(page, 8), std::string(32, 'X') + "recovery");
}

TEST_F(Teletext525PageDecoderTest, ATimeFillingHeaderDoesNotClaimACarrier) {
  // §7.3 makes the time-filling header a terminator rather than a page, so a
  // magazine that has sent nothing else has still shown no page of its own —
  // even though this one arrives perfectly intact.
  feed(make_525_time_filling_header(7), -1);
  settle_extension_carrier(3);
  feed(make_525_header(3, 0x05, 0), 0);
  feed(make_525_row(3, 8, std::string(32, 'X')), 1);
  feed(make_525_extension(3, 8, {"recovery"}), 2);
  feed(make_525_time_filling_header(3), 3);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(row_text(snapshots_.front(), 8), std::string(32, 'X') + "recovery");
}

TEST_F(Teletext525PageDecoderTest, ExtensionsAreHeldUntilTheCarrierIsKnown) {
  // Until the role is settled the packets are discarded, not guessed at: a
  // squasher keeps every copy it is given, so one written to the wrong page
  // could never be taken back.
  feed(make_525_header(1, 0x00, 0), 0);
  feed(make_525_row(1, 8, std::string(32, 'X')), 1);
  feed(make_525_extension(1, 8, {"TOOEARLY"}), 2);
  feed(make_525_time_filling_header(1), 3);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(row_text(snapshots_.front(), 8), std::string(32, 'X'));
}

TEST_F(TeletextPageDecoderTest, A625LineStreamKeepsMagazinesFourToSeven) {
  // Extension decoding is a reading of a *short* packet's magazine bit; a
  // 625-line service uses all eight magazines for pages and must be untouched.
  decoder_.process_packet(make_header(5, 0x00, 0), 0);
  decoder_.process_packet(make_row(5, 1, std::string(40, 'Z')), 1);
  decoder_.process_packet(make_time_filling_header(5), 2);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(snapshots_.front().magazine, 5);
  EXPECT_EQ(row_text(snapshots_.front(), 1), std::string(40, 'Z'));
}

TEST_F(TeletextPageDecoderTest, DefaultPacketLengthKeepsTheFortyColumnPage) {
  // The 625-line default is unchanged by the width having become a parameter.
  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(make_row(1, 1, std::string(40, 'Y')), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(snapshots_.front().columns, TeletextPageSnapshot::kColumns);
  EXPECT_EQ(row_text(snapshots_.front(), 1), std::string(40, 'Y'));
}

////////////////////////////////////////////////////////////////////////////////////////////

namespace {

// ETSI EN 300 706 §15.3 Table 33: the Second G0 Set Designation value a
// service with only one alphabet transmits, which the table gives as "no second
// G0 set required". It is the helpers' default below so that a test saying
// nothing about a second set gets a packet that says there is none.
constexpr int kNoSecondG0Set = 0b1111111;

// Build an X/28 or M/29 packet carrying a designation code and its first two
// triplets. ETSI EN 300 706 §9.4.1 figure 11: byte 6 (index 2) is the Hamming
// 8/4 designation code and the 39 bytes after it are thirteen Hamming 24/18
// triplets. Triplets past the second are left as the all-zero data pattern,
// which is what a service with nothing to say in them transmits.
//
// |triplet2| defaults to the three bits that complete kNoSecondG0Set, the
// second G0 set straddling the two triplets (§15.3 Table 33).
std::array<uint8_t, kTeletextPacketBytes> make_enhancement_packet(
    int magazine, int packet_number, int designation_code, uint32_t triplet1,
    uint32_t triplet2 = (kNoSecondG0Set >> 4) & 0x7) {
  std::array<uint8_t, kTeletextPacketBytes> packet{};
  const auto mrag = make_mrag(magazine, packet_number);
  packet[0] = mrag[0];
  packet[1] = mrag[1];
  packet[2] = orc::teletext_hamming84_encode(
      static_cast<uint8_t>(designation_code & 0xF));
  uint8_t triplet_bytes[3] = {};
  orc::teletext_hamming2418_encode(triplet1, triplet_bytes);
  packet[3] = triplet_bytes[0];
  packet[4] = triplet_bytes[1];
  packet[5] = triplet_bytes[2];
  orc::teletext_hamming2418_encode(triplet2, triplet_bytes);
  packet[6] = triplet_bytes[0];
  packet[7] = triplet_bytes[1];
  packet[8] = triplet_bytes[2];
  orc::teletext_hamming2418_encode(0, triplet_bytes);
  for (size_t byte = 9; byte + 2 < kTeletextPacketBytes; byte += 3) {
    packet[byte] = triplet_bytes[0];
    packet[byte + 1] = triplet_bytes[1];
    packet[byte + 2] = triplet_bytes[2];
  }
  return packet;
}

// Triplet 1 of an X/28/0 Format 1, X/28/4, M/29/0 or M/29/4 for a basic Level 1
// page (§9.4.2.2 Table 4): page function 0000 and page coding 000 in bits 1-7,
// the seven Table 32 designation and national option bits in 8-14, and the low
// four of the seven Table 33 second-set bits in 15-18.
uint32_t character_set_triplet(int designation, int national_option,
                               int second_value = kNoSecondG0Set) {
  const uint32_t value = (static_cast<uint32_t>(designation & 0xF) << 3) |
                         static_cast<uint32_t>(national_option & 0x7);
  return (value << 7) | ((static_cast<uint32_t>(second_value) & 0xF) << 14);
}

// Triplet 2 of the same packet: the high three of the Table 33 second-set bits
// in its bits 1-3, everything above them (side panels, colour map) left zero.
uint32_t second_set_triplet(int second_value) {
  return (static_cast<uint32_t>(second_value) >> 4) & 0x7;
}

// Table 32 designation 0100 with national option 100: Cyrillic G0 Option 2.
constexpr int kCyrillicDesignation = 0b0100;
constexpr int kRussianBulgarianOption = 0b100;

// The same value read through Table 33, which codes its Cyrillic rows
// identically — and Table 33's plain Latin/English row, the natural second set
// of a Cyrillic service.
constexpr int kSecondSetCyrillic2 =
    (kCyrillicDesignation << 3) | kRussianBulgarianOption;
constexpr int kSecondSetLatin = 0b0000000;

// A row of the Russian broadcast the Cyrillic tables were read against.
const char* const kRussianRow = "Wtornik";

// The ESC (or Switch) spacing attribute, §12.2 Table 26 code 1/B.
constexpr char kEsc = '\x1B';

}  // namespace

TEST_F(TeletextPageDecoderTest, PagesAreLatinUntilToldOtherwise) {
  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(make_row(1, 1, kRussianRow), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(snapshots_.front().g0_set, TeletextG0Set::Latin);
  EXPECT_EQ(decoder_.default_g0_set(), TeletextG0Set::Latin);
}

TEST_F(TeletextPageDecoderTest, TheConfiguredSetAppliesWhenNoneIsDesignated) {
  // The local Code of Practice of §15.2, which is the only thing that can
  // settle the alphabet of a Level 1 service — and this is exactly such a
  // service: no packet above X/24 in the whole stream.
  decoder_.set_default_g0_set(TeletextG0Set::Cyrillic2);

  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(make_row(1, 1, kRussianRow), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(snapshots_.front().g0_set, TeletextG0Set::Cyrillic2);
  // The stored codes are the transmitted ones either way — the set changes how
  // they are read, not what was received.
  EXPECT_EQ(row_text(snapshots_.front(), 1), kRussianRow);
  EXPECT_EQ(through(kRussianRow, snapshots_.front().g0_set), "Вторник");
}

TEST_F(TeletextPageDecoderTest, AnX28DesignationOverridesTheConfiguredSet) {
  // A service that says what it is must be believed over the setting, in
  // either direction: §15.2 gives the packet the higher priority, and a user
  // who has set the wrong region should not corrupt a page that declares one.
  decoder_.set_default_g0_set(TeletextG0Set::Cyrillic3);

  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(
      make_enhancement_packet(
          1, 28, 0,
          character_set_triplet(kCyrillicDesignation, kRussianBulgarianOption)),
      1);
  decoder_.process_packet(make_row(1, 1, kRussianRow), 2);
  decoder_.process_packet(make_time_filling_header(1), 3);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(snapshots_.front().g0_set, TeletextG0Set::Cyrillic2);
}

TEST_F(TeletextPageDecoderTest, AnX28DesignationArrivingAfterRowsStillApplies) {
  // The packet belongs to the page rather than to a point in it, and a page is
  // only rendered when its transmission ends — so an X/28/0 sent after some of
  // the rows is still that page's designation.
  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(make_row(1, 1, kRussianRow), 1);
  decoder_.process_packet(
      make_enhancement_packet(
          1, 28, 0,
          character_set_triplet(kCyrillicDesignation, kRussianBulgarianOption)),
      2);
  decoder_.process_packet(make_time_filling_header(1), 3);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(snapshots_.front().g0_set, TeletextG0Set::Cyrillic2);
}

TEST_F(TeletextPageDecoderTest, AnX28DesignationDoesNotLeakToTheNextPage) {
  // X/28/0 is page-specific (§15.2), so the page after it goes back to the
  // magazine's default rather than inheriting the designation.
  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(
      make_enhancement_packet(
          1, 28, 0,
          character_set_triplet(kCyrillicDesignation, kRussianBulgarianOption)),
      1);
  decoder_.process_packet(make_row(1, 1, "A"), 2);
  decoder_.process_packet(make_header(1, 0x01, 0), 3);
  decoder_.process_packet(make_row(1, 1, "B"), 4);
  decoder_.process_packet(make_time_filling_header(1), 5);
  decoder_.finalize(10);

  ASSERT_EQ(snapshots_.size(), 2u);
  EXPECT_EQ(snapshots_[0].g0_set, TeletextG0Set::Cyrillic2);
  EXPECT_EQ(snapshots_[1].g0_set, TeletextG0Set::Latin);
}

TEST_F(TeletextPageDecoderTest, AnM29DesignationAppliesToTheWholeMagazine) {
  decoder_.process_packet(
      make_enhancement_packet(
          1, 29, 0,
          character_set_triplet(kCyrillicDesignation, kRussianBulgarianOption)),
      0);
  decoder_.process_packet(make_header(1, 0x00, 0), 1);
  decoder_.process_packet(make_row(1, 1, "A"), 2);
  decoder_.process_packet(make_header(1, 0x01, 0), 3);
  decoder_.process_packet(make_row(1, 1, "B"), 4);
  // A different magazine is untouched by it.
  decoder_.process_packet(make_header(2, 0x00, 0), 5);
  decoder_.process_packet(make_row(2, 1, "C"), 6);
  decoder_.process_packet(make_time_filling_header(1), 7);
  decoder_.process_packet(make_time_filling_header(2), 8);
  decoder_.finalize(10);

  ASSERT_EQ(snapshots_.size(), 3u);
  EXPECT_EQ(snapshots_[0].g0_set, TeletextG0Set::Cyrillic2);
  EXPECT_EQ(snapshots_[1].g0_set, TeletextG0Set::Cyrillic2);
  EXPECT_EQ(snapshots_[2].g0_set, TeletextG0Set::Latin);
}

TEST_F(TeletextPageDecoderTest, APageX28BeatsItsMagazineM29) {
  // §15.2: "superseded by a page-related X/28/0 Format 1".
  decoder_.process_packet(
      make_enhancement_packet(
          1, 29, 0,
          character_set_triplet(kCyrillicDesignation, kRussianBulgarianOption)),
      0);
  decoder_.process_packet(make_header(1, 0x00, 0), 1);
  decoder_.process_packet(
      make_enhancement_packet(1, 28, 0, character_set_triplet(0b0000, 0b000)),
      2);
  decoder_.process_packet(make_row(1, 1, "A"), 3);
  decoder_.process_packet(make_time_filling_header(1), 4);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(snapshots_.front().g0_set, TeletextG0Set::Latin);
}

TEST_F(TeletextPageDecoderTest, DesignationsThatSayNothingUsefulAreIgnored) {
  // Three ways a packet 28 carries no character set designation, each of which
  // must leave the configured set alone rather than be read as designation 0
  // (which is Latin, and would silently undo the setting).
  const uint32_t cyrillic =
      character_set_triplet(kCyrillicDesignation, kRussianBulgarianOption);
  decoder_.set_default_g0_set(TeletextG0Set::Cyrillic2);

  // X/28/1, one of the designation codes that carries no character set at all
  // (§9.4.2: only 0 and 4 are coded by Table 4).
  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(
      make_enhancement_packet(1, 28, 1, character_set_triplet(0, 0)), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  // X/28/0 Format 1 for a page that is not a basic Level 1 page: page function
  // 0001 is a data broadcasting page (§9.4.2.1 Table 3), whose triplet 1 bits
  // 8-14 are not a character set designation.
  decoder_.process_packet(make_header(1, 0x01, 0), 3);
  decoder_.process_packet(
      make_enhancement_packet(1, 28, 0, character_set_triplet(0, 0) | 0x1), 4);
  decoder_.process_packet(make_time_filling_header(1), 5);

  // An X/28/0 whose triplet is uncorrectable: two bits flipped in one byte.
  decoder_.process_packet(make_header(1, 0x02, 0), 6);
  auto damaged = make_enhancement_packet(1, 28, 0, cyrillic);
  damaged[3] ^= 0b0000'0011;
  decoder_.process_packet(damaged, 7);
  decoder_.process_packet(make_time_filling_header(1), 8);
  decoder_.finalize(10);

  ASSERT_EQ(snapshots_.size(), 3u);
  for (const TeletextPageSnapshot& snapshot : snapshots_) {
    EXPECT_EQ(snapshot.g0_set, TeletextG0Set::Cyrillic2)
        << "page " << std::hex << snapshot.page_number;
  }
}

TEST_F(TeletextPageDecoderTest, AGarbageDesignationPacketCannotPoisonTheSet) {
  // The failure this reproduces was found on the reference SECAM capture: a
  // noise burst whose MRAG mis-corrected to M/29/0 with a triplet 1 that
  // happened to decode, which re-designated the whole magazine to Latin and
  // silently undid the configured Cyrillic set for the rest of the recording.
  // What separates such a packet from a real one is the rest of it — §9.4.1
  // codes all thirteen triplets, and garbage never decodes them all.
  decoder_.set_default_g0_set(TeletextG0Set::Cyrillic2);

  auto fake =
      make_enhancement_packet(1, 29, 0, character_set_triplet(0b0000, 0b000));
  // Triplet 1 stays perfectly valid; a later triplet takes the double error
  // that makes it uncorrectable, as noise does.
  fake[9] ^= 0b0000'0011;
  decoder_.process_packet(fake, 0);

  decoder_.process_packet(make_header(1, 0x00, 0), 1);
  decoder_.process_packet(make_row(1, 1, kRussianRow), 2);
  decoder_.process_packet(make_time_filling_header(1), 3);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  EXPECT_EQ(snapshots_.front().g0_set, TeletextG0Set::Cyrillic2);
}

TEST_F(TeletextPageDecoderTest, SubtitleTextIsReadInThePagesOwnSet) {
  // The cue text is where a wrong alphabet is least visible and most annoying:
  // it goes to a subtitle file that nothing downstream will re-interpret.
  decoder_.set_default_g0_set(TeletextG0Set::Cyrillic2);
  ASSERT_TRUE(decoder_.set_subtitle_page("888"));

  HeaderFlags flags;
  flags.subtitle = true;
  decoder_.process_packet(make_header(8, 0x88, 0, flags), 0);
  decoder_.process_packet(make_row(8, 20, boxed(kRussianRow)), 1);
  decoder_.process_packet(make_time_filling_header(8), 2);
  decoder_.finalize(50);

  ASSERT_FALSE(decoder_.subtitle_cues().empty());
  EXPECT_EQ(decoder_.subtitle_cues().front().text, "Вторник");
}

////////////////////////////////////////////////////////////////////////////////////////////
// ESC (Switch) and the second G0 set — EN 300 706 §12.2 Table 26 code 1/B,
// §15.3 Table 33.
////////////////////////////////////////////////////////////////////////////////////////////

namespace {

// The G0 sets the cells of one rendered row resolved to, as UTF-8 through each
// cell's own set — which is the whole point of stamping them per cell.
std::string row_glyphs(const TeletextPageSnapshot& snapshot, int row) {
  std::string text;
  for (int column = 0; column < snapshot.columns; ++column) {
    const auto& cell =
        snapshot.cells[static_cast<size_t>(row)][static_cast<size_t>(column)];
    text += orc::teletext_g0_to_utf8(cell.character, cell.g0_set,
                                     cell.national_option_subset);
  }
  while (!text.empty() && text.back() == ' ') {
    text.pop_back();
  }
  return text;
}

}  // namespace

TEST_F(TeletextPageDecoderTest, EscapeIsInertWithoutASecondSet) {
  // What every page did before a second set could be designated, and what a
  // one-alphabet page must go on doing: 1/B is a blank spacing attribute and
  // the codes around it are all read in the one set.
  decoder_.set_default_g0_set(TeletextG0Set::Cyrillic2);

  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(make_row(1, 1, std::string("W") + kEsc + "tornik"),
                          1);
  decoder_.process_packet(make_time_filling_header(1), 2);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const TeletextPageSnapshot& page = snapshots_.front();
  EXPECT_FALSE(page.second_g0_set.has_value());
  // The attribute cell renders as SPACE; everything else stays Cyrillic.
  EXPECT_EQ(row_glyphs(page, 1), "В торник");
}

TEST_F(TeletextPageDecoderTest, EscapeSwitchesToTheConfiguredSecondSet) {
  // The local Code of Practice of §15.3 for a Level 1 service: no packet X/28
  // or M/29 anywhere in the stream, so the pairing can only come from the
  // setting — and with it, the Latin run between the two ESCs stays Latin.
  decoder_.set_default_g0_set(TeletextG0Set::Cyrillic2);
  decoder_.set_default_second_g0_set(
      orc::TeletextG0Designation{TeletextG0Set::Latin, 0});

  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(
      make_row(1, 1, std::string("W") + kEsc + "BBC" + kEsc + "tornik"), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const TeletextPageSnapshot& page = snapshots_.front();
  ASSERT_TRUE(page.second_g0_set.has_value());
  EXPECT_EQ(page.second_g0_set->g0_set, TeletextG0Set::Latin);
  // 'W' is Cyrillic В, "BBC" is Latin because the first ESC switched into the
  // second set, and "tornik" is Cyrillic again because the second switched
  // back. Read in one set throughout it would have been "ВББЦторник".
  EXPECT_EQ(row_glyphs(page, 1), "В BBC торник");
}

TEST_F(TeletextPageDecoderTest, EscapeIsSetAfterAndTheCellItselfIsSpace) {
  // §12.2 Table 26 lists 1/B as "Set-After": the switch applies from the cell
  // after it, and the attribute cell displays as SPACE.
  decoder_.set_default_g0_set(TeletextG0Set::Cyrillic2);
  decoder_.set_default_second_g0_set(
      orc::TeletextG0Designation{TeletextG0Set::Latin, 0});

  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(make_row(1, 1, std::string("W") + kEsc + "W"), 1);
  decoder_.process_packet(make_time_filling_header(1), 2);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& cells = snapshots_.front().cells[1];
  EXPECT_EQ(cells[0].g0_set, TeletextG0Set::Cyrillic2);
  EXPECT_EQ(cells[1].character, 0x20);
  EXPECT_EQ(cells[1].g0_set, TeletextG0Set::Cyrillic2);
  EXPECT_EQ(cells[2].g0_set, TeletextG0Set::Latin);
}

TEST_F(TeletextPageDecoderTest, TheDefaultSetIsReselectedAtEveryRowStart) {
  // §12.2 Table 26 code 1/B: "The default at the start of each row is the
  // default G0 set." This is what confines the damage of a lost ESC to the row
  // it was lost in, so it is worth a test of its own: row 1 ends inside the
  // second set and row 2 must still start in the first.
  decoder_.set_default_g0_set(TeletextG0Set::Cyrillic2);
  decoder_.set_default_second_g0_set(
      orc::TeletextG0Designation{TeletextG0Set::Latin, 0});

  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(make_row(1, 1, std::string(1, kEsc) + "BBC"), 1);
  decoder_.process_packet(make_row(1, 2, "W"), 2);
  decoder_.process_packet(make_time_filling_header(1), 3);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const TeletextPageSnapshot& page = snapshots_.front();
  EXPECT_EQ(row_glyphs(page, 1), " BBC");
  EXPECT_EQ(row_glyphs(page, 2), "В");
}

TEST_F(TeletextPageDecoderTest, ADamagedEscapeDoesNotSwitchButIsAdmitted) {
  // A byte that fails odd parity is never read as an attribute (§8.1), so
  // damage cannot silently re-alphabet a row. What it can do is hide an ESC
  // that was really sent, and the cells after it say so rather than claiming a
  // set that cannot be verified.
  decoder_.set_default_g0_set(TeletextG0Set::Cyrillic2);
  decoder_.set_default_second_g0_set(
      orc::TeletextG0Designation{TeletextG0Set::Latin, 0});

  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  auto row = make_row(1, 1, std::string("W") + kEsc + "BBC");
  row[3] ^= 0x80;  // break the ESC byte's parity, as a dropout would
  decoder_.process_packet(row, 1);
  decoder_.process_packet(make_time_filling_header(1), 2);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& cells = snapshots_.front().cells[1];
  EXPECT_TRUE(cells[1].parity_error);
  // No switch happened, so the run after it is read in the first set — but
  // every cell from the damage on is marked as being in one of the two.
  EXPECT_EQ(cells[2].g0_set, TeletextG0Set::Cyrillic2);
  EXPECT_FALSE(cells[0].g0_set_uncertain);
  EXPECT_FALSE(cells[1].g0_set_uncertain);
  EXPECT_TRUE(cells[2].g0_set_uncertain);
  EXPECT_TRUE(cells[39].g0_set_uncertain);
  // Nothing is uncertain on the row that follows: the set is re-selected there.
  EXPECT_FALSE(snapshots_.front().cells[2][0].g0_set_uncertain);
}

TEST_F(TeletextPageDecoderTest, DamageIsNotUncertaintyWithoutASecondSet) {
  // With one alphabet there is no unseen ESC that could have changed anything,
  // so ordinary parity damage must not be dressed up as an alphabet doubt.
  decoder_.set_default_g0_set(TeletextG0Set::Cyrillic2);

  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  auto row = make_row(1, 1, "WWWW");
  row[3] ^= 0x80;
  decoder_.process_packet(row, 1);
  decoder_.process_packet(make_time_filling_header(1), 2);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const auto& cells = snapshots_.front().cells[1];
  EXPECT_TRUE(cells[1].parity_error);
  EXPECT_FALSE(cells[2].g0_set_uncertain);
}

TEST_F(TeletextPageDecoderTest, AnX28DesignatesTheSecondSetToo) {
  // §15.3: triplet 1 bits 15-18 over triplet 2 bits 1-3 select a Table 33
  // entry. A service that designates the pair is believed over the setting for
  // both halves of it.
  decoder_.set_default_g0_set(TeletextG0Set::Latin);
  decoder_.set_default_second_g0_set(
      orc::TeletextG0Designation{TeletextG0Set::Cyrillic3, 0});

  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(
      make_enhancement_packet(
          1, 28, 0,
          character_set_triplet(kCyrillicDesignation, kRussianBulgarianOption,
                                kSecondSetLatin),
          second_set_triplet(kSecondSetLatin)),
      1);
  decoder_.process_packet(
      make_row(1, 1, std::string("W") + kEsc + "BBC" + kEsc + "tornik"), 2);
  decoder_.process_packet(make_time_filling_header(1), 3);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const TeletextPageSnapshot& page = snapshots_.front();
  EXPECT_EQ(page.g0_set, TeletextG0Set::Cyrillic2);
  ASSERT_TRUE(page.second_g0_set.has_value());
  EXPECT_EQ(page.second_g0_set->g0_set, TeletextG0Set::Latin);
  EXPECT_EQ(row_glyphs(page, 1), "В BBC торник");
}

TEST_F(TeletextPageDecoderTest, ANoSecondSetDesignationDisablesEscape) {
  // §15.3 reserves 1111111 for "no second G0 set required" and says a decoder
  // may read it as disabling ESC. A service saying so must beat the setting:
  // it knows what it transmits and the setting is a guess about material that
  // says nothing.
  decoder_.set_default_g0_set(TeletextG0Set::Cyrillic2);
  decoder_.set_default_second_g0_set(
      orc::TeletextG0Designation{TeletextG0Set::Latin, 0});

  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(
      make_enhancement_packet(
          1, 28, 0,
          character_set_triplet(kCyrillicDesignation, kRussianBulgarianOption)),
      1);
  decoder_.process_packet(make_row(1, 1, std::string("W") + kEsc + "BBC"), 2);
  decoder_.process_packet(make_time_filling_header(1), 3);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const TeletextPageSnapshot& page = snapshots_.front();
  EXPECT_EQ(page.g0_set, TeletextG0Set::Cyrillic2);
  EXPECT_FALSE(page.second_g0_set.has_value());
  EXPECT_EQ(row_glyphs(page, 1), "В ББЦ");
}

TEST_F(TeletextPageDecoderTest, AnM29DesignatesTheSecondSetForItsMagazine) {
  // The magazine-wide half of §15.3, including the case that catches a decoder
  // out: an M/29 saying the magazine has no second set is an answer, and must
  // silence ESC rather than falling through to the configured pairing.
  decoder_.set_default_g0_set(TeletextG0Set::Latin);
  decoder_.set_default_second_g0_set(
      orc::TeletextG0Designation{TeletextG0Set::Latin, 0});

  decoder_.process_packet(
      make_enhancement_packet(
          1, 29, 0,
          character_set_triplet(kCyrillicDesignation, kRussianBulgarianOption,
                                kSecondSetLatin),
          second_set_triplet(kSecondSetLatin)),
      0);
  // Magazine 2 says it needs only one set, so the configured pairing must not
  // reach its pages either.
  decoder_.process_packet(
      make_enhancement_packet(
          2, 29, 0,
          character_set_triplet(kCyrillicDesignation, kRussianBulgarianOption)),
      1);

  decoder_.process_packet(make_header(1, 0x00, 0), 2);
  decoder_.process_packet(make_row(1, 1, std::string("W") + kEsc + "BBC"), 3);
  decoder_.process_packet(make_header(2, 0x00, 0), 4);
  decoder_.process_packet(make_row(2, 1, std::string("W") + kEsc + "BBC"), 5);
  decoder_.process_packet(make_time_filling_header(1), 6);
  decoder_.process_packet(make_time_filling_header(2), 7);
  decoder_.finalize(10);

  ASSERT_EQ(snapshots_.size(), 2u);
  ASSERT_TRUE(snapshots_[0].second_g0_set.has_value());
  EXPECT_EQ(row_glyphs(snapshots_[0], 1), "В BBC");
  EXPECT_FALSE(snapshots_[1].second_g0_set.has_value());
  EXPECT_EQ(row_glyphs(snapshots_[1], 1), "В ББЦ");
}

TEST_F(TeletextPageDecoderTest, AnX284DesignatesTheSetsAndX280BeatsIt) {
  // §9.4.2.2: X/28/4 is coded by the same Table 4 as X/28/0 Format 1 — so it
  // carries the same designations — and "packet 28/0 takes precedence over
  // 28/4 for all but the colour map entry coding", in whichever order the two
  // arrive.
  decoder_.set_default_g0_set(TeletextG0Set::Latin);

  // X/28/4 alone: it designates the page.
  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(
      make_enhancement_packet(
          1, 28, 4,
          character_set_triplet(kCyrillicDesignation, kRussianBulgarianOption,
                                kSecondSetLatin),
          second_set_triplet(kSecondSetLatin)),
      1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  // X/28/0 after an X/28/4: the /0 wins.
  decoder_.process_packet(make_header(1, 0x01, 0), 3);
  decoder_.process_packet(
      make_enhancement_packet(
          1, 28, 4,
          character_set_triplet(kCyrillicDesignation, kRussianBulgarianOption,
                                kSecondSetLatin),
          second_set_triplet(kSecondSetLatin)),
      4);
  decoder_.process_packet(
      make_enhancement_packet(1, 28, 0, character_set_triplet(0b0000, 0b000)),
      5);
  decoder_.process_packet(make_time_filling_header(1), 6);

  // X/28/4 after an X/28/0: the /0 still wins, so the /4 changes nothing.
  decoder_.process_packet(make_header(1, 0x02, 0), 7);
  decoder_.process_packet(
      make_enhancement_packet(1, 28, 0, character_set_triplet(0b0000, 0b000)),
      8);
  decoder_.process_packet(
      make_enhancement_packet(
          1, 28, 4,
          character_set_triplet(kCyrillicDesignation, kRussianBulgarianOption,
                                kSecondSetLatin),
          second_set_triplet(kSecondSetLatin)),
      9);
  decoder_.process_packet(make_time_filling_header(1), 10);
  decoder_.finalize(20);

  ASSERT_EQ(snapshots_.size(), 3u);
  EXPECT_EQ(snapshots_[0].g0_set, TeletextG0Set::Cyrillic2);
  EXPECT_TRUE(snapshots_[0].second_g0_set.has_value());
  EXPECT_EQ(snapshots_[1].g0_set, TeletextG0Set::Latin);
  EXPECT_FALSE(snapshots_[1].second_g0_set.has_value());
  EXPECT_EQ(snapshots_[2].g0_set, TeletextG0Set::Latin);
  EXPECT_FALSE(snapshots_[2].second_g0_set.has_value());
}

TEST_F(TeletextPageDecoderTest, TheSecondSetDoesNotLeakToTheNextPage) {
  // As for the first set: an X/28 belongs to its page, and the page after it
  // falls back to the magazine's answer or to the setting — including the
  // X/28/0-over-X/28/4 precedence, which must not lock the next page out of
  // reading its own X/28/4.
  decoder_.set_default_g0_set(TeletextG0Set::Latin);

  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(
      make_enhancement_packet(1, 28, 0, character_set_triplet(0b0000, 0b000)),
      1);
  decoder_.process_packet(make_time_filling_header(1), 2);

  decoder_.process_packet(make_header(1, 0x01, 0), 3);
  decoder_.process_packet(
      make_enhancement_packet(
          1, 28, 4,
          character_set_triplet(kCyrillicDesignation, kRussianBulgarianOption,
                                kSecondSetLatin),
          second_set_triplet(kSecondSetLatin)),
      4);
  decoder_.process_packet(make_time_filling_header(1), 5);
  decoder_.finalize(10);

  ASSERT_EQ(snapshots_.size(), 2u);
  EXPECT_FALSE(snapshots_[0].second_g0_set.has_value());
  EXPECT_EQ(snapshots_[1].g0_set, TeletextG0Set::Cyrillic2);
  ASSERT_TRUE(snapshots_[1].second_g0_set.has_value());
  EXPECT_EQ(snapshots_[1].second_g0_set->g0_set, TeletextG0Set::Latin);
}

TEST_F(TeletextPageDecoderTest, ASecondLatinSetKeepsItsOwnNationalOption) {
  // §15.3: "The national option sub-set selected by the C12, C13 and C14 bits
  // is not relevant to the secondary set." The page header here says French,
  // and the second set's own designation says English — so 2/3 is `é` in the
  // first set and `£` in the second, on the same row.
  HeaderFlags flags;
  flags.national_option_subset = 4;  // Table 32: French

  decoder_.process_packet(make_header(1, 0x00, 0, flags), 0);
  decoder_.process_packet(
      make_enhancement_packet(
          1, 28, 0, character_set_triplet(0b0000, 0b100, kSecondSetLatin),
          second_set_triplet(kSecondSetLatin)),
      1);
  decoder_.process_packet(make_row(1, 1, std::string("#") + kEsc + "#"), 2);
  decoder_.process_packet(make_time_filling_header(1), 3);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const TeletextPageSnapshot& page = snapshots_.front();
  ASSERT_TRUE(page.second_g0_set.has_value());
  EXPECT_EQ(page.second_g0_set->national_option_subset, 0);
  EXPECT_EQ(row_glyphs(page, 1), "é £");
}

TEST_F(TeletextPageDecoderTest, SubtitleTextFollowsTheRowsEscapeSwitches) {
  // The cue text is where a wrong alphabet is least visible: a Latin name
  // switched into mid-row must not come out transliterated into Cyrillic.
  decoder_.set_default_g0_set(TeletextG0Set::Cyrillic2);
  decoder_.set_default_second_g0_set(
      orc::TeletextG0Designation{TeletextG0Set::Latin, 0});
  ASSERT_TRUE(decoder_.set_subtitle_page("888"));

  HeaderFlags flags;
  flags.subtitle = true;
  decoder_.process_packet(make_header(8, 0x88, 0, flags), 0);
  decoder_.process_packet(
      make_row(8, 20, boxed(std::string("W") + kEsc + "BBC")), 1);
  decoder_.process_packet(make_time_filling_header(8), 2);
  decoder_.finalize(50);

  ASSERT_FALSE(decoder_.subtitle_cues().empty());
  EXPECT_EQ(decoder_.subtitle_cues().front().text, "В BBC");
}

TEST_F(TeletextPageDecoderTest, TheConfiguredPairSurvivesAPageWithNoPackets) {
  // The whole point of the setting: a Level 1 service transmits neither X/28
  // nor M/29, so every page it carries must open with the configured pair
  // rather than only the first.
  decoder_.set_default_g0_set(TeletextG0Set::Cyrillic2);
  decoder_.set_default_second_g0_set(
      orc::TeletextG0Designation{TeletextG0Set::Latin, 0});

  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(make_row(1, 1, std::string(1, kEsc) + "BBC"), 1);
  decoder_.process_packet(make_header(1, 0x01, 0), 2);
  decoder_.process_packet(make_row(1, 1, std::string(1, kEsc) + "BBC"), 3);
  decoder_.process_packet(make_time_filling_header(1), 4);
  decoder_.finalize(10);

  ASSERT_EQ(snapshots_.size(), 2u);
  EXPECT_EQ(row_glyphs(snapshots_[0], 1), " BBC");
  EXPECT_EQ(row_glyphs(snapshots_[1], 1), " BBC");
}

TEST_F(TeletextPageDecoderTest, ACyrillicSecondSetReadsTable33AsTable32Does) {
  // Table 33 codes its Cyrillic rows at the same designation and national
  // option bits as Table 32, so a service can name a Cyrillic set as the
  // *second* one — a Latin service quoting Russian, the mirror of the usual
  // case. Worth its own test because the two tables are separate documents and
  // one shared lookup reads both.
  decoder_.process_packet(make_header(1, 0x00, 0), 0);
  decoder_.process_packet(
      make_enhancement_packet(
          1, 28, 0, character_set_triplet(0b0000, 0b000, kSecondSetCyrillic2),
          second_set_triplet(kSecondSetCyrillic2)),
      1);
  decoder_.process_packet(make_row(1, 1, std::string("W") + kEsc + "Wtornik"),
                          2);
  decoder_.process_packet(make_time_filling_header(1), 3);
  decoder_.finalize(10);

  ASSERT_FALSE(snapshots_.empty());
  const TeletextPageSnapshot& page = snapshots_.front();
  EXPECT_EQ(page.g0_set, TeletextG0Set::Latin);
  ASSERT_TRUE(page.second_g0_set.has_value());
  EXPECT_EQ(page.second_g0_set->g0_set, TeletextG0Set::Cyrillic2);
  EXPECT_EQ(row_glyphs(page, 1), "W Вторник");
}

}  // namespace orc_unit_test
