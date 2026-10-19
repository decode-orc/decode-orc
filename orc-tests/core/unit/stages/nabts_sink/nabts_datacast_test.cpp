/*
 * File:        nabts_datacast_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for decoding NABTS FEC datacasts (RFC 2728 §3.3,
 *              Appendix A)
 *
 * Covers: the row and column code's sums, one-byte correction and erasure
 * replacement; the bundle layouts the stream shows — RFC 2728's own, check
 * rows closing their data, and a layout sending one check row of the two ahead
 * of its data — with a lost packet rebuilt from the column code, and a row
 * that passed its own code wrong found and put right by both; two and three
 * rows failing their code rebuilt by the column code and the rows' own codes
 * together, and what no one reading puts right — rows wrong in one byte, a
 * row lost beside one failing — left as it is; the frame
 * each row arrived in, and a row taken as received marked so; filler
 * removal, and rows taken whole on a channel that flags every row as not full;
 * and the text a payload holds.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_datacast.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace orc_unit_test {
namespace {

using Row = std::array<uint8_t, orc::kNabtsFecRowBytes>;

/// A data row of |data| (26 bytes) with its two row check bytes, so that its
/// codeword — the check bytes at positions 0 and 1, the data after — sums to
/// zero both ways.
Row data_row(const std::vector<uint8_t>& data) {
  std::array<uint8_t, orc::kNabtsFecRowBytes> codeword{};
  std::copy(data.begin(), data.end(), codeword.begin() + 2);
  orc::nabts_fec_replace(codeword.data(), codeword.size(), 0, 1);
  Row row{};
  std::copy(codeword.begin() + 2, codeword.end(), row.begin());
  row[26] = codeword[0];
  row[27] = codeword[1];
  return row;
}

/// The two check rows of |data_rows|, codeword positions 0 and 1 of every
/// column.
std::array<Row, 2> check_rows(const std::vector<Row>& data_rows) {
  std::array<Row, 2> checks{};
  std::vector<uint8_t> column(data_rows.size() + 2);
  for (size_t j = 0; j < orc::kNabtsFecRowBytes; ++j) {
    for (size_t i = 0; i < data_rows.size(); ++i) {
      column[2 + i] = data_rows[i][j];
    }
    column[0] = column[1] = 0;
    orc::nabts_fec_replace(column.data(), column.size(), 0, 1);
    checks[0][j] = column[0];
    checks[1][j] = column[1];
  }
  return checks;
}

/// Distinct bytes for data row |row| of bundle |bundle|.
std::vector<uint8_t> row_bytes(uint32_t bundle, uint32_t row) {
  std::vector<uint8_t> out(orc::kNabtsFecDataBytes);
  for (size_t i = 0; i < out.size(); ++i) {
    out[i] = static_cast<uint8_t>((bundle * 131 + row * 17 + i * 7 + 3) & 0xFF);
  }
  return out;
}

/// Whether |row|'s code puts it right, in one byte or none.
bool row_code_holds(const Row& row) {
  std::array<uint8_t, orc::kNabtsFecRowBytes> codeword{};
  std::copy(row.begin(), row.begin() + 26, codeword.begin() + 2);
  codeword[0] = row[26];
  codeword[1] = row[27];
  return orc::nabts_fec_sums(codeword.data(), codeword.size()) ==
             std::array<uint8_t, 2>{0, 0} ||
         orc::nabts_fec_correct_byte(codeword.data(), codeword.size());
}

/// Whether |row|'s code changes it into a codeword other than |right|'s.
bool miscorrected(const Row& row, const Row& right) {
  std::array<uint8_t, orc::kNabtsFecRowBytes> codeword{};
  std::copy(row.begin(), row.begin() + 26, codeword.begin() + 2);
  codeword[0] = row[26];
  codeword[1] = row[27];
  if (orc::nabts_fec_sums(codeword.data(), codeword.size()) ==
          std::array<uint8_t, 2>{0, 0} ||
      !orc::nabts_fec_correct_byte(codeword.data(), codeword.size())) {
    return false;
  }
  return !std::equal(codeword.begin() + 2, codeword.end(), right.begin());
}

/// Feeds rows to a decoder as packets of one channel.
class Feed {
 public:
  explicit Feed(uint16_t channel) : channel_(channel) {}

  void send(const Row& row, bool check, bool filler = false,
            uint32_t frame = 0) {
    orc::NabtsPacket packet;
    packet.valid = true;
    packet.address_attested = true;
    packet.channel = channel_;
    packet.continuity = continuity_;
    packet.not_full = filler;
    packet.suffix = check ? orc::NabtsSuffixKind::kBundle
                          : orc::NabtsSuffixKind::kLongitudinalPlusReserved;
    std::array<uint8_t, orc::kNabtsPacketBytes> raw{};
    std::copy(row.begin(), row.end(), raw.begin() + orc::kNabtsPrefixBytes);
    decoder_.add_packet(packet, raw.data(), frame);
    skip();
  }
  /// A packet the stream never carried, its continuity index skipped.
  void skip() {
    continuity_ =
        static_cast<uint8_t>((continuity_ + 1) % orc::kNabtsContinuityModulus);
  }

  std::vector<orc::NabtsDatacastChannel> finish() { return decoder_.finish(); }

 private:
  uint16_t channel_;
  uint8_t continuity_ = 0;
  orc::NabtsDatacastDecoder decoder_;
};

TEST(NabtsDatacast, CorrectsOneWrongByteOfARow) {
  Row row = data_row(row_bytes(1, 2));
  std::array<uint8_t, orc::kNabtsFecRowBytes> codeword{};
  std::copy(row.begin(), row.begin() + 26, codeword.begin() + 2);
  codeword[0] = row[26];
  codeword[1] = row[27];
  const auto good = codeword;
  codeword[9] ^= 0x5A;
  EXPECT_NE(orc::nabts_fec_sums(codeword.data(), codeword.size()),
            (std::array<uint8_t, 2>{0, 0}));
  ASSERT_TRUE(orc::nabts_fec_correct_byte(codeword.data(), codeword.size()));
  EXPECT_EQ(codeword, good);
}

TEST(NabtsDatacast, ReplacesOneOrTwoErasures) {
  std::vector<uint8_t> column{0, 0, 11, 22, 33, 44, 55, 66, 77, 88};
  orc::nabts_fec_replace(column.data(), column.size(), 0, 1);
  const std::vector<uint8_t> good = column;
  column[4] = 0;
  EXPECT_TRUE(orc::nabts_fec_replace(column.data(), column.size(), 4, 4));
  EXPECT_EQ(column, good);
  column[3] = column[7] = 0;
  EXPECT_TRUE(orc::nabts_fec_replace(column.data(), column.size(), 3, 7));
  EXPECT_EQ(column, good);
}

// RFC 2728 §3.3: fourteen data rows, then the check rows at continuity
// indices 14 and 15. A lost data row comes back from the column code.
TEST(NabtsDatacast, DecodesRfc2728BundlesAndRebuildsALostRow) {
  Feed feed(0x242);
  std::vector<uint8_t> expected;
  for (uint32_t bundle = 0; bundle < 8; ++bundle) {
    std::vector<Row> data;
    for (uint32_t r = 0; r < 14; ++r) {
      data.push_back(data_row(row_bytes(bundle, r)));
      const auto bytes = row_bytes(bundle, r);
      expected.insert(expected.end(), bytes.begin(), bytes.end());
    }
    const auto checks = check_rows(data);
    for (uint32_t r = 0; r < 14; ++r) {
      if (bundle == 5 && r == 6) {
        feed.skip();  // lost on the way
        continue;
      }
      feed.send(data[r], false);
    }
    feed.send(checks[0], true);
    feed.send(checks[1], true);
  }
  const auto decoded = feed.finish();
  ASSERT_EQ(decoded.size(), 1u);
  const orc::NabtsDatacastChannel& channel = decoded[0];
  EXPECT_EQ(channel.layout.data_rows, 14u);
  EXPECT_EQ(channel.layout.checks, "CC");
  EXPECT_FALSE(channel.layout.checks_lead);
  EXPECT_EQ(channel.rows_rebuilt, 1u);
  EXPECT_EQ(channel.rows_missing, 0u);
  EXPECT_EQ(channel.payload, expected);
  EXPECT_EQ(channel.columns_verified, channel.columns_judged);
}

// A layout sending sixteen data rows a bundle and only the second of its two
// check rows, ahead of the data, skipping the first's continuity index: the
// unsent row is one erasure, a lost data row the other.
TEST(NabtsDatacast, DecodesBundlesWithOneCheckRowSentAheadOfTheirData) {
  Feed feed(0x0F3);
  std::vector<uint8_t> expected;
  for (uint32_t bundle = 0; bundle < 8; ++bundle) {
    std::vector<Row> data;
    for (uint32_t r = 0; r < 16; ++r) {
      data.push_back(data_row(row_bytes(bundle, r)));
    }
    const auto checks = check_rows(data);
    feed.skip();                 // position 0, never sent
    feed.send(checks[1], true);  // position 1
    for (uint32_t r = 0; r < 16; ++r) {
      const auto bytes = row_bytes(bundle, r);
      expected.insert(expected.end(), bytes.begin(), bytes.end());
      if (bundle == 3 && r == 9) {
        feed.skip();
        continue;
      }
      feed.send(data[r], false);
    }
  }
  const auto decoded = feed.finish();
  ASSERT_EQ(decoded.size(), 1u);
  const orc::NabtsDatacastChannel& channel = decoded[0];
  EXPECT_EQ(channel.layout.data_rows, 16u);
  EXPECT_EQ(channel.layout.checks, "MC");
  EXPECT_TRUE(channel.layout.checks_lead);
  EXPECT_EQ(channel.rows_rebuilt, 1u);
  EXPECT_EQ(channel.payload, expected);
}

// Each row keeps the frame of the recording it arrived in, and one the column
// code rebuilt the frame its bundle's first row came in; a row that failed its
// code and that nothing could rebuild is marked as taken as received.
TEST(NabtsDatacast, KeepsTheFrameEachRowArrivedIn) {
  Feed feed(0x242);
  for (uint32_t bundle = 0; bundle < 8; ++bundle) {
    std::vector<Row> data;
    for (uint32_t r = 0; r < 14; ++r) {
      data.push_back(data_row(row_bytes(bundle, r)));
    }
    const auto checks = check_rows(data);
    const uint32_t frame = 100 + 10 * bundle;
    for (uint32_t r = 0; r < 14; ++r) {
      if ((bundle == 2 && r == 4) ||
          (bundle == 5 && (r == 1 || r == 2 || r == 3))) {
        feed.skip();
        continue;
      }
      Row row = data[r];
      if (bundle == 5 && r == 8) {
        // Two bytes wrong, the second chosen so that the row code cannot take
        // them for one.
        row[3] ^= 0x41;
        for (uint8_t x = 1; row_code_holds(row); ++x) {
          row[11] = static_cast<uint8_t>(data[r][11] ^ x);
        }
      }
      feed.send(row, false, false, frame + r / 4);
    }
    feed.send(checks[0], true, false, frame + 4);
    feed.send(checks[1], true, false, frame + 4);
  }
  const auto decoded = feed.finish();
  ASSERT_EQ(decoded.size(), 1u);
  const std::vector<orc::NabtsDatacastRow>& rows = decoded[0].rows;
  ASSERT_EQ(rows.size(), 8u * 14u);
  EXPECT_EQ(rows[0].frame, 100u);
  EXPECT_EQ(rows[13].frame, 103u);
  // Bundle 2's lost row, rebuilt.
  EXPECT_FALSE(rows[2 * 14 + 4].missing);
  EXPECT_EQ(rows[2 * 14 + 4].frame, 120u);
  // Bundle 5: three rows lost and one failing its code, more erasures than
  // the column code and the rows' own codes take together.
  EXPECT_TRUE(rows[5 * 14 + 1].missing);
  EXPECT_EQ(rows[5 * 14 + 1].frame, 150u);
  EXPECT_TRUE(rows[5 * 14 + 8].damaged);
  EXPECT_FALSE(rows[5 * 14 + 9].damaged);
  EXPECT_EQ(decoded[0].rows_damaged, 1u);
}

// A row two bytes wrong that its own code changes into the wrong codeword, in
// a bundle sending one check row of the two: the column code says the bundle
// is wrong, and the row is the one its code changed whose rebuilding from the
// columns lies within two bytes of the row as it arrived.
TEST(NabtsDatacast, PutsRightARowThatPassedItsCodeWrong) {
  Feed feed(0x0F3);
  std::vector<uint8_t> expected;
  for (uint32_t bundle = 0; bundle < 8; ++bundle) {
    std::vector<Row> data;
    for (uint32_t r = 0; r < 16; ++r) {
      data.push_back(data_row(row_bytes(bundle, r)));
    }
    const auto checks = check_rows(data);
    feed.skip();
    feed.send(checks[1], true);
    for (uint32_t r = 0; r < 16; ++r) {
      const auto bytes = row_bytes(bundle, r);
      expected.insert(expected.end(), bytes.begin(), bytes.end());
      Row row = data[r];
      if (bundle == 4 && r == 6) {
        // Two bytes wrong, the second chosen so that the row code takes them
        // for one, and changes a third.
        row[3] ^= 0x41;
        for (uint8_t x = 1; !miscorrected(row, data[r]); ++x) {
          row[11] = static_cast<uint8_t>(data[r][11] ^ x);
        }
      }
      feed.send(row, false);
    }
  }
  const auto decoded = feed.finish();
  ASSERT_EQ(decoded.size(), 1u);
  EXPECT_EQ(decoded[0].payload, expected);
  EXPECT_EQ(decoded[0].rows_rebuilt, 1u);
}

/// |row| two bytes wrong, at |first| and |second|, so that its own code
/// cannot put it right.
Row two_bytes_wrong(const Row& row, size_t first, size_t second) {
  Row wrong = row;
  wrong[first] ^= 0x41;
  for (uint8_t x = 1; row_code_holds(wrong); ++x) {
    wrong[second] = static_cast<uint8_t>(row[second] ^ x);
  }
  return wrong;
}

// Bundles sending one check row of the two hold more erasures than the
// column code takes alone — two rows failing their code, or three — and the
// column code and the rows' own codes together put them right: the columns
// that do not hold say where the failing rows are wrong, and the check row
// never sent, rebuilt, checks the reading.
TEST(NabtsDatacast, RebuildsWhatTheColumnCodeCannotAloneWithTheRowCodes) {
  Feed feed(0x0F3);
  std::vector<uint8_t> expected;
  for (uint32_t bundle = 0; bundle < 8; ++bundle) {
    std::vector<Row> data;
    for (uint32_t r = 0; r < 16; ++r) {
      data.push_back(data_row(row_bytes(bundle, r)));
    }
    const auto checks = check_rows(data);
    feed.skip();
    feed.send(checks[1], true);
    for (uint32_t r = 0; r < 16; ++r) {
      const auto bytes = row_bytes(bundle, r);
      expected.insert(expected.end(), bytes.begin(), bytes.end());
      if (bundle == 2 && (r == 3 || r == 12)) {
        feed.send(two_bytes_wrong(data[r], r == 3 ? 5 : 9, r == 3 ? 17 : 20),
                  false);
      } else if (bundle == 5 && (r == 1 || r == 7 || r == 10)) {
        feed.send(two_bytes_wrong(data[r], r, r + 12), false);
      } else {
        feed.send(data[r], false);
      }
    }
  }
  const auto decoded = feed.finish();
  ASSERT_EQ(decoded.size(), 1u);
  EXPECT_EQ(decoded[0].payload, expected);
  EXPECT_EQ(decoded[0].rows_rebuilt_jointly, 5u);
  EXPECT_EQ(decoded[0].rows_damaged, 0u);
  EXPECT_EQ(decoded[0].rows_missing, 0u);
}

// What no one reading puts right is left as it is: two failing rows wrong in
// the same byte, which no column can say which; and a row lost beside a
// failing one, where every reading holds as well as any other.
TEST(NabtsDatacast, LeavesABundleNoOneReadingPutsRight) {
  std::vector<Row> data;
  for (uint32_t r = 0; r < 16; ++r) {
    data.push_back(data_row(row_bytes(1, r)));
  }
  const auto checks = check_rows(data);
  std::vector<Row> rows = {Row{}, checks[1]};
  rows.insert(rows.end(), data.begin(), data.end());
  std::vector<bool> arrived(rows.size(), true);
  std::vector<bool> good(rows.size(), true);
  arrived[0] = false;  // never sent
  rows[5] = two_bytes_wrong(rows[5], 4, 15);
  rows[9] = two_bytes_wrong(rows[9], 15, 20);
  good[5] = good[9] = false;
  EXPECT_FALSE(orc::nabts_fec_repair_bundle(rows, arrived, good).has_value());
  // Wrong in bytes of their own, the one reading that holds puts them right.
  rows[9] = two_bytes_wrong(data[7], 9, 20);
  const auto repaired = orc::nabts_fec_repair_bundle(rows, arrived, good);
  ASSERT_TRUE(repaired.has_value());
  EXPECT_EQ((*repaired)[0], checks[0]);
  EXPECT_EQ((*repaired)[5], data[3]);
  EXPECT_EQ((*repaired)[9], data[7]);
  // A row lost beside one failing.
  arrived[12] = false;
  rows[12] = Row{};
  rows[9] = data[7];
  good[9] = true;
  EXPECT_FALSE(orc::nabts_fec_repair_bundle(rows, arrived, good).has_value());
}

// RFC 2728 §3.2: a block not full of data ends in 0x15 and as many 0xEA as
// fill it, and the payload leaves them out.
TEST(NabtsDatacast, LeavesTheFillerOut) {
  Feed feed(0x242);
  std::vector<uint8_t> expected;
  for (uint32_t bundle = 0; bundle < 6; ++bundle) {
    std::vector<Row> data;
    for (uint32_t r = 0; r < 14; ++r) {
      std::vector<uint8_t> bytes = row_bytes(bundle, r);
      size_t useful = bytes.size();
      if (r == 13) {
        useful = 20;
        bytes[20] = 0x15;
        for (size_t i = 21; i < bytes.size(); ++i) {
          bytes[i] = 0xEA;
        }
      }
      expected.insert(expected.end(), bytes.begin(),
                      bytes.begin() + static_cast<ptrdiff_t>(useful));
      data.push_back(data_row(bytes));
    }
    const auto checks = check_rows(data);
    for (uint32_t r = 0; r < 14; ++r) {
      feed.send(data[r], false, /*filler=*/r == 13);
    }
    feed.send(checks[0], true);
    feed.send(checks[1], true);
  }
  const auto decoded = feed.finish();
  ASSERT_EQ(decoded.size(), 1u);
  EXPECT_EQ(decoded[0].payload, expected);
}

// A channel that flags every row as not full of data while its rows are full —
// they end in filler's bytes no more often than chance — has its rows taken
// whole: a row whose last byte happens to be 0x15 keeps it.
TEST(NabtsDatacast, TakesRowsWholeOnAChannelFlaggingEveryRowNotFull) {
  Feed feed(0x0F3);
  std::vector<uint8_t> expected;
  for (uint32_t bundle = 0; bundle < 8; ++bundle) {
    std::vector<Row> data;
    for (uint32_t r = 0; r < 14; ++r) {
      std::vector<uint8_t> bytes = row_bytes(bundle, r);
      if (r == 3) {
        bytes.back() = 0x15;
      }
      expected.insert(expected.end(), bytes.begin(), bytes.end());
      data.push_back(data_row(bytes));
    }
    const auto checks = check_rows(data);
    for (uint32_t r = 0; r < 14; ++r) {
      feed.send(data[r], false, /*filler=*/true);
    }
    feed.send(checks[0], true);
    feed.send(checks[1], true);
  }
  const auto decoded = feed.finish();
  ASSERT_EQ(decoded.size(), 1u);
  EXPECT_EQ(decoded[0].payload, expected);
}

TEST(NabtsDatacast, WritesTheTextAPayloadHolds) {
  const std::string bulletin =
      "TORNADO WARNING FOR THE COUNTIES LISTED BELOW UNTIL 7 PM\r\n"
      "MOVE TO AN INTERIOR ROOM ON THE LOWEST FLOOR";
  std::vector<uint8_t> payload{0x01, 0x9F, 0x00};
  payload.insert(payload.end(), bulletin.begin(), bulletin.end());
  payload.push_back(0xC0);
  const std::string short_run = "TOO SHORT";
  payload.insert(payload.end(), short_run.begin(), short_run.end());
  payload.push_back(0x00);
  EXPECT_EQ(orc::nabts_datacast_text(payload),
            "TORNADO WARNING FOR THE COUNTIES LISTED BELOW UNTIL 7 PM\n"
            "MOVE TO AN INTERIOR ROOM ON THE LOWEST FLOOR\n");
}

// A channel whose rows mostly fail the row code is no datacast, and is left.
TEST(NabtsDatacast, LeavesAChannelWhoseRowsFailTheRowCode) {
  Feed feed(0x123);
  for (uint32_t r = 0; r < 200; ++r) {
    Row row{};
    for (size_t i = 0; i < row.size(); ++i) {
      row[i] = static_cast<uint8_t>(r * 7 + i);
    }
    feed.send(row, false);
  }
  EXPECT_TRUE(feed.finish().empty());
}

// A channel sending its data in no data groups and with no row code is written
// out as a raw stream: its blocks in the order the continuity index gives
// them, a gap where it skipped.
TEST(NabtsDatacast, WritesAStreamOfNoGroupsAsARawStream) {
  orc::NabtsDatacastDecoder decoder;
  const std::string text =
      "121302@240010 PROGRAMME EVENT LOG ENTRY WITH A TITLE AND A HOUSE "
      "NUMBER 05AE380 ";
  std::vector<uint8_t> expected;
  std::array<uint8_t, orc::kNabtsPacketBytes> raw{};
  uint8_t continuity = 0;
  for (uint32_t i = 0; i < 120; ++i) {
    orc::NabtsPacket packet;
    packet.valid = true;
    packet.address_attested = true;
    packet.channel = 0x729;
    packet.continuity = continuity;
    packet.suffix = orc::NabtsSuffixKind::kNone;
    packet.data_length = orc::kNabtsMaxDataBlockBytes;
    for (size_t k = 0; k < packet.data_length; ++k) {
      packet.data[k] = static_cast<uint8_t>(
          text[(i * packet.data_length + k) % text.size()]);
    }
    continuity =
        static_cast<uint8_t>((continuity + 1) % orc::kNabtsContinuityModulus);
    if (i == 50) {
      continue;  // lost on the way
    }
    decoder.add_packet(packet, raw.data());
    expected.insert(
        expected.end(), packet.data.begin(),
        packet.data.begin() + static_cast<ptrdiff_t>(packet.data_length));
  }
  const auto decoded = decoder.finish();
  ASSERT_EQ(decoded.size(), 1u);
  EXPECT_TRUE(decoded[0].raw);
  EXPECT_EQ(decoded[0].channel, 0x729u);
  EXPECT_EQ(decoded[0].payload, expected);
  EXPECT_EQ(decoded[0].payload_gaps, 1u);
  EXPECT_EQ(decoded[0].rows_missing, 1u);
  EXPECT_EQ(decoded[0].text_percent, 100u);
}

// A channel of data groups sends synchronizing packets throughout, and is no
// raw stream.
TEST(NabtsDatacast, TakesNoChannelOfDataGroupsForARawStream) {
  orc::NabtsDatacastDecoder decoder;
  std::array<uint8_t, orc::kNabtsPacketBytes> raw{};
  for (uint32_t i = 0; i < 2000; ++i) {
    orc::NabtsPacket packet;
    packet.valid = true;
    packet.address_attested = true;
    packet.channel = 0x000;
    packet.continuity = static_cast<uint8_t>(i % 16);
    packet.synchronizing = i % 20 == 0;
    packet.suffix = orc::NabtsSuffixKind::kNone;
    packet.data_length = orc::kNabtsMaxDataBlockBytes;
    packet.data.fill(static_cast<uint8_t>('A' + i % 26));
    decoder.add_packet(packet, raw.data());
  }
  EXPECT_TRUE(decoder.finish().empty());
}

/// |count| packets of text on |channel|, the continuity index of packet i
/// being |continuity|(i), the address of those |attested|(i) says arrived as
/// transmitted.
template <typename Continuity, typename Attested>
std::vector<orc::NabtsDatacastChannel> raw_channel(uint16_t channel,
                                                   uint32_t count,
                                                   Continuity continuity,
                                                   Attested attested) {
  orc::NabtsDatacastDecoder decoder;
  std::array<uint8_t, orc::kNabtsPacketBytes> raw{};
  for (uint32_t i = 0; i < count; ++i) {
    orc::NabtsPacket packet;
    packet.valid = true;
    packet.address_attested = attested(i);
    packet.channel = channel;
    packet.continuity = continuity(i);
    packet.suffix = orc::NabtsSuffixKind::kNone;
    packet.data_length = orc::kNabtsMaxDataBlockBytes;
    // Text, which no short shift register produces (the test-sequence check).
    const std::string text =
        "BULLETIN 0417 SEVERE THUNDERSTORM WATCH FOR COUNTIES NORTH OF THE "
        "RIVER UNTIL 9 PM ";
    for (size_t k = 0; k < packet.data_length; ++k) {
      packet.data[k] = static_cast<uint8_t>(
          text[(i * packet.data_length + k) % text.size()]);
    }
    decoder.add_packet(packet, raw.data());
  }
  return decoder.finish();
}

// Another service's line read as NABTS gives a continuity index that falls
// anywhere, and a packet sent over and over one that never moves: neither is
// a stream's, which counts its packets.
TEST(NabtsDatacast, TakesNoChannelWhoseIndexDoesNotCountForARawStream) {
  const auto anywhere = [](uint32_t i) {
    return static_cast<uint8_t>((i * 7U + 3U) % 16U);
  };
  const auto still = [](uint32_t) { return uint8_t{5}; };
  const auto always = [](uint32_t) { return true; };
  EXPECT_TRUE(raw_channel(0x101, 200, anywhere, always).empty());
  EXPECT_TRUE(raw_channel(0x101, 200, still, always).empty());
  // The same packets with an index that counts are a stream.
  const auto counting = [](uint32_t i) {
    return static_cast<uint8_t>(i % 16U);
  };
  EXPECT_EQ(raw_channel(0x101, 200, counting, always).size(), 1u);
}

// A channel whose packets nearly all arrived at an address that had to be
// corrected is other channels' packets misfiled, however many there are.
TEST(NabtsDatacast, TakesNoChannelOfCorrectedAddressesForARawStream) {
  const auto counting = [](uint32_t i) {
    return static_cast<uint8_t>(i % 16U);
  };
  const auto rarely = [](uint32_t i) { return i % 20 == 0; };
  EXPECT_TRUE(raw_channel(0x01B, 400, counting, rarely).empty());
}

}  // namespace
}  // namespace orc_unit_test
