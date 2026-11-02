/*
 * File:        nabts_datacast_files_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the files taken out of datacast payloads
 *
 * Covers: a file transfer's chunks joined into files, its file number read as
 * the time the file was queued; a message across a row that went missing not
 * believed; a carousel's file read from its directory entry and its blocks,
 * voted across copies and with what no copy delivered marked; a carousel's
 * text file read through its byte table, from bytes as a recording sent them;
 * and the text an HTML page shows. Hand-built payloads only; deterministic,
 * no I/O.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_datacast_files.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace orc_unit_test {
namespace {

// ---------------------------------------------------------------------------
// File transfer
// ---------------------------------------------------------------------------

/// A datacast whose payload is |bytes| in rows of 26, those numbered in
/// |missing| left out of the payload and held open.
orc::NabtsDatacastChannel datacast_of(const std::vector<uint8_t>& bytes,
                                      const std::set<size_t>& missing = {}) {
  orc::NabtsDatacastChannel datacast;
  datacast.channel = 0x242;
  for (size_t r = 0; r * 26 < bytes.size(); ++r) {
    if (missing.count(r) > 0) {
      datacast.rows.push_back(orc::NabtsDatacastRow{
          static_cast<uint32_t>(datacast.payload.size()), 0, true});
      continue;
    }
    const size_t n = std::min<size_t>(26, bytes.size() - r * 26);
    datacast.rows.push_back(
        orc::NabtsDatacastRow{static_cast<uint32_t>(datacast.payload.size()),
                              static_cast<uint16_t>(n), false});
    datacast.payload.insert(datacast.payload.end(),
                            bytes.begin() + static_cast<ptrdiff_t>(r * 26),
                            bytes.begin() + static_cast<ptrdiff_t>(r * 26 + n));
  }
  return datacast;
}

/// One chunk message of file |number|: its |chunk|th 128 bytes, each |fill|
/// but where |head| overwrites them.
std::vector<uint8_t> chunk_message(uint32_t number, uint16_t chunk,
                                   uint8_t fill, const std::string& head = {}) {
  std::vector<uint8_t> out = {0xFF, 0x8B, 0x1A};
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<uint8_t>((number >> (8 * i)) & 0xFF));
  }
  out.push_back(static_cast<uint8_t>(chunk & 0xFF));
  out.push_back(static_cast<uint8_t>(chunk >> 8));
  for (size_t i = 0; i < orc::kNabtsTransferChunkData; ++i) {
    out.push_back(i < head.size() ? static_cast<uint8_t>(head[i]) : fill);
  }
  out.insert(out.end(), {1, 2, 3, 4, 5});  // trailer
  return out;
}

// 2003-05-19 03:14:06 UTC.
constexpr uint32_t kQueued = 1053314046;

TEST(NabtsDatacastFiles, JoinsATransfersChunksIntoItsFile) {
  std::vector<uint8_t> payload;
  for (int copy = 0; copy < 2; ++copy) {
    const auto first = chunk_message(kQueued, 0, 'a', "GIF89a");
    payload.insert(payload.end(), first.begin(), first.end());
    for (uint16_t c = 1; c < 3; ++c) {
      const auto chunk =
          chunk_message(kQueued, c, static_cast<uint8_t>('a' + c));
      payload.insert(payload.end(), chunk.begin(), chunk.end());
    }
    // Another kind of message between them, which is no chunk.
    payload.insert(payload.end(), {0xFF, 0x03, 0x04, 9, 9, 9});
  }
  const auto files = orc::nabts_datacast_files(datacast_of(payload));
  ASSERT_EQ(files.size(), 1u);
  const orc::NabtsCarriedFile& file = files[0];
  EXPECT_EQ(file.transfer, "file transfer");
  EXPECT_EQ(file.name, "3ec84bfe");
  EXPECT_EQ(file.time, "2003-05-19 03:14:06 UTC");
  EXPECT_EQ(file.copies, 2u);
  EXPECT_EQ(file.format, "gif");
  ASSERT_EQ(file.data.size(), 3 * orc::kNabtsTransferChunkData);
  EXPECT_EQ(file.data[orc::kNabtsTransferChunkData], 'b');
  EXPECT_EQ(file.data.back(), 'c');
  EXPECT_TRUE(file.data_present.empty()) << "every chunk arrived";
}

// A message across a row that went missing is the pieces of two: the chunk is
// not believed, and where no other copy delivered it the file holds a hole.
TEST(NabtsDatacastFiles, BelievesNoMessageAcrossALostRow) {
  std::vector<uint8_t> payload;
  for (uint16_t c = 0; c < 6; ++c) {
    const auto chunk = chunk_message(kQueued, c, static_cast<uint8_t>('a' + c));
    payload.insert(payload.end(), chunk.begin(), chunk.end());
  }
  // Chunk 1 is bytes 142 to 283 of the payload: row 7 is inside it.
  const auto files = orc::nabts_datacast_files(datacast_of(payload, {7}));
  ASSERT_EQ(files.size(), 1u);
  const orc::NabtsCarriedFile& file = files[0];
  ASSERT_FALSE(file.data_present.empty());
  EXPECT_EQ(file.data_present[orc::kNabtsTransferChunkData], 0u)
      << "chunk 1 never arrived whole";
  EXPECT_NE(file.data_present[0], 0u);
}

// ---------------------------------------------------------------------------
// File carousel
// ---------------------------------------------------------------------------

/// A carousel's stream in rows of 25 bytes and their sum, the rows numbered
/// in |missing| held open.
orc::NabtsDatacastChannel carousel_of(std::vector<uint8_t> stream,
                                      const std::set<size_t>& missing = {}) {
  // Enough rows that the channel is read as a carousel.
  if (stream.size() < 80 * 25) {
    stream.resize(80 * 25, 0);
  }
  stream.resize((stream.size() + 24) / 25 * 25, 0);
  std::vector<uint8_t> rows;
  for (size_t r = 0; r * 25 < stream.size(); ++r) {
    unsigned sum = 0;
    for (size_t i = 0; i < 25; ++i) {
      rows.push_back(stream[r * 25 + i]);
      sum += stream[r * 25 + i];
    }
    rows.push_back(static_cast<uint8_t>(sum & 0xFF));
  }
  orc::NabtsDatacastChannel datacast = datacast_of(rows, missing);
  datacast.channel = 0x243;
  return datacast;
}

/// Pads |stream| with zeros to the next row boundary.
void to_row(std::vector<uint8_t>& stream) {
  stream.resize((stream.size() + 24) / 25 * 25, 0);
}

/// One copy of file |name| on the carousel: its directory entry, then its
/// data in blocks of 392 bytes, each in a bundle of 16 rows.
void put_file(std::vector<uint8_t>& stream, const std::string& name,
              const std::vector<uint8_t>& data, uint32_t checksum = 0x3412) {
  const std::string entry =
      "\\ACS\\CURRENT\\     " + name + "2003-05-1906:27:10";
  stream.insert(stream.end(), entry.begin(), entry.end());
  for (int i = 0; i < 4; ++i) {
    stream.push_back(static_cast<uint8_t>((data.size() >> (8 * i)) & 0xFF));
  }
  // The sum of the file's bytes as read, then the entry's closing bytes.
  for (int i = 0; i < 4; ++i) {
    stream.push_back(static_cast<uint8_t>((checksum >> (8 * i)) & 0xFF));
  }
  stream.insert(stream.end(), {0x00, 0x01, 0x01, 0x01});
  to_row(stream);
  for (size_t at = 0, block = 1; at < data.size();
       at += orc::kNabtsCarouselBlockData, ++block) {
    const size_t n = std::min(orc::kNabtsCarouselBlockData, data.size() - at);
    const size_t start = stream.size();
    stream.insert(stream.end(), {0x01, 0x01, static_cast<uint8_t>(block), 0x00,
                                 static_cast<uint8_t>(n & 0xFF),
                                 static_cast<uint8_t>(n >> 8)});
    stream.insert(stream.end(), data.begin() + static_cast<ptrdiff_t>(at),
                  data.begin() + static_cast<ptrdiff_t>(at + n));
    stream.resize(start + 16 * 25, 0);
  }
}

std::vector<uint8_t> binary_data(size_t size) {
  std::vector<uint8_t> data(size);
  for (size_t i = 0; i < size; ++i) {
    data[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
  }
  return data;
}

TEST(NabtsDatacastFiles, ReadsACarouselFileFromItsEntryAndBlocks) {
  const std::vector<uint8_t> data = binary_data(500);
  std::vector<uint8_t> stream;
  put_file(stream, "TEST0001.BIN", data);
  put_file(stream, "TEST0001.BIN", data);
  const auto files = orc::nabts_datacast_files(carousel_of(stream));
  ASSERT_EQ(files.size(), 1u);
  const orc::NabtsCarriedFile& file = files[0];
  EXPECT_EQ(file.transfer, "file carousel");
  EXPECT_EQ(file.name, "TEST0001.BIN");
  EXPECT_EQ(file.time, "2003-05-19 06:27:10");
  EXPECT_EQ(file.copies, 2u);
  EXPECT_EQ(file.data, data);
  EXPECT_TRUE(file.data_present.empty());
  EXPECT_FALSE(file.deciphered) << "binary bytes do not read as text";
  EXPECT_EQ(file.format, "binary");
}

// A row lost from one copy is held open, so the copy's other bytes stay where
// they belong, and the other copy delivers what it lost.
TEST(NabtsDatacastFiles, VotesACarouselFileFromItsCopies) {
  const std::vector<uint8_t> data = binary_data(500);
  std::vector<uint8_t> stream;
  put_file(stream, "TEST0002.BIN", data);
  const size_t second = stream.size() / 25;
  put_file(stream, "TEST0002.BIN", data);
  // Row 3 of the first copy's first block, and row 5 of the second's.
  const auto files =
      orc::nabts_datacast_files(carousel_of(stream, {3 + 3, second + 3 + 5}));
  ASSERT_EQ(files.size(), 1u);
  EXPECT_EQ(files[0].data, data);
  EXPECT_TRUE(files[0].data_present.empty());
}

TEST(NabtsDatacastFiles, MarksWhatNoCopyOfACarouselFileDelivered) {
  const std::vector<uint8_t> data = binary_data(500);
  std::vector<uint8_t> stream;
  put_file(stream, "TEST0003.BIN", data);
  // The entry takes three rows; the first block's fourth row is lost.
  const auto files = orc::nabts_datacast_files(carousel_of(stream, {3 + 3}));
  ASSERT_EQ(files.size(), 1u);
  const orc::NabtsCarriedFile& file = files[0];
  ASSERT_EQ(file.data_present.size(), data.size());
  EXPECT_EQ(file.bytes_lost(), 25u);
  EXPECT_EQ(file.data_present[3 * 25 - 6], 0u);
  EXPECT_NE(file.data_present[0], 0u);
}

// The opening of a text file as a recording sent it: the fixed header every
// file of the carousel opens with, read through its byte table under the key
// its name gives.
TEST(NabtsDatacastFiles, ReadsACarouselTextFileThroughItsByteTable) {
  const std::vector<uint8_t> sent = {0x3e, 0x66, 0x71, 0x71, 0x3e, 0xee, 0x99,
                                     0x84, 0x6b, 0xee, 0xae, 0x8e, 0x3e, 0x5e,
                                     0x84, 0xae, 0xc2, 0x70, 0x8e};
  std::vector<uint8_t> stream;
  put_file(stream, "035J0036.HTM", sent);
  const auto files = orc::nabts_datacast_files(carousel_of(stream));
  ASSERT_EQ(files.size(), 1u);
  const orc::NabtsCarriedFile& file = files[0];
  EXPECT_TRUE(file.deciphered);
  EXPECT_EQ(std::string(file.data.begin(), file.data.end()),
            "<!--<ACSTAG><MSGID>");
  EXPECT_EQ(file.format, "html");
}

/// |text| as a carousel sends it under |key|, through the sink's byte table.
std::vector<uint8_t> enciphered(const std::string& text, unsigned key) {
  std::array<int, 256> by_index{};
  by_index.fill(-1);
  for (int b = 0; b < 256; ++b) {
    const int index = orc::nabts_carousel_byte_index(static_cast<uint8_t>(b));
    if (index >= 0) {
      by_index[static_cast<size_t>(index)] = b;
    }
  }
  std::vector<uint8_t> out;
  for (const char c : text) {
    const int b = by_index[(static_cast<unsigned char>(c) + key) & 0xFFU];
    EXPECT_GE(b, 0) << "no table entry for " << c;
    out.push_back(static_cast<uint8_t>(b < 0 ? 0 : b));
  }
  return out;
}

uint32_t byte_sum(const std::string& text) {
  uint32_t sum = 0;
  for (const char c : text) {
    sum += static_cast<unsigned char>(c);
  }
  return sum;
}

unsigned name_sum(const std::string& name) {
  unsigned sum = 0;
  for (const char c : name) {
    sum += static_cast<unsigned char>(c);
  }
  return sum;
}

// A list file as a carousel sent many copies of, each under its own key.
const std::string kListText =
    "#2002-12-13#\r\n\"I021213_03,I021213_05,I021213_06,I021213_07,"
    "I021213_09,I021213_11\"\r\n";

// A carousel whose keys sit an offset from its names' sums: the directory's
// sums of its whole files say which.
TEST(NabtsDatacastFiles, LearnsACarouselsKeyOffsetFromItsFilesSums) {
  const unsigned offset = 256 - 24;
  std::vector<uint8_t> stream;
  for (const std::string name : {"106.CHK", "109.CHK"}) {
    put_file(stream, name,
             enciphered(kListText, (name_sum(name) + offset) & 0xFFU),
             byte_sum(kListText));
  }
  const auto files = orc::nabts_datacast_files(carousel_of(stream));
  ASSERT_EQ(files.size(), 2u);
  for (const orc::NabtsCarriedFile& file : files) {
    EXPECT_TRUE(file.deciphered) << file.name;
    EXPECT_EQ(std::string(file.data.begin(), file.data.end()), kListText);
    ASSERT_TRUE(file.checksum_holds.has_value()) << file.name;
    EXPECT_TRUE(*file.checksum_holds);
  }
}

TEST(NabtsDatacastFiles, SaysWhenAWholeFilesBytesDoNotAddUp) {
  const unsigned offset = 256 - 24;
  std::vector<uint8_t> stream;
  put_file(stream, "106.CHK",
           enciphered(kListText, (name_sum("106.CHK") + offset) & 0xFFU),
           byte_sum(kListText));
  put_file(stream, "109.CHK",
           enciphered(kListText, (name_sum("109.CHK") + offset) & 0xFFU),
           byte_sum(kListText) + 1);
  const auto files = orc::nabts_datacast_files(carousel_of(stream));
  ASSERT_EQ(files.size(), 2u);
  EXPECT_TRUE(files[0].checksum_holds.value_or(false));
  ASSERT_TRUE(files[1].checksum_holds.has_value());
  EXPECT_FALSE(*files[1].checksum_holds);
  // The good file still teaches the offset both are read with.
  EXPECT_EQ(std::string(files[1].data.begin(), files[1].data.end()), kListText);
}

TEST(NabtsDatacastFiles, FindsNothingInAPayloadOfNeitherFraming) {
  EXPECT_TRUE(
      orc::nabts_datacast_files(datacast_of(binary_data(4000))).empty());
}

TEST(NabtsDatacastFiles, ReadsTheTextAnHtmlPageShows) {
  const std::string html =
      "<!--<ACSTAG>x</ACSTAG>--><HEAD><TITLE>Title</TITLE><style>p "
      "{margin:0}</style></HEAD><body><p>Timings&nbsp;for 5/20</p>\r\n"
      "<table><tr><td>ABC Promo</td><td>8:51</td></tr></table></body>";
  const std::string text =
      orc::nabts_html_text(std::vector<uint8_t>(html.begin(), html.end()));
  EXPECT_NE(text.find("Timings for 5/20"), std::string::npos) << text;
  EXPECT_NE(text.find("ABC Promo 8:51"), std::string::npos) << text;
  EXPECT_EQ(text.find('<'), std::string::npos) << text;
  EXPECT_EQ(text.find("margin"), std::string::npos) << text;
  EXPECT_EQ(text.find("Title"), std::string::npos) << text;
}

}  // namespace
}  // namespace orc_unit_test
