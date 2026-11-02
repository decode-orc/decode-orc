/*
 * File:        teletext_data_export_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the export of everything a teletext run decoded
 *
 * Covers: a page as text — its characters, its block mosaics as the sextants
 * they draw, a row not received — and each kind of data written under its own
 * folder with its index and the README. Files are collected through the
 * writer the export is given, so nothing touches a disk.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "teletext_data_export.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace orc_unit_test {
namespace {

using orc::TeletextPageSnapshot;

/// The lines of |text|, split at '\n'.
std::vector<std::string> lines_of(const std::string& text) {
  std::vector<std::string> out;
  size_t start = 0;
  for (size_t end = text.find('\n'); end != std::string::npos;
       end = text.find('\n', start)) {
    out.push_back(text.substr(start, end - start));
    start = end + 1;
  }
  return out;
}

/// A page whose row 1 reads |text| from column 0, then the mosaic codes
/// |mosaics|.
TeletextPageSnapshot page_reading(const std::string& text,
                                  const std::vector<uint8_t>& mosaics = {}) {
  TeletextPageSnapshot page;
  page.row_received[1] = true;
  size_t col = 0;
  for (const char c : text) {
    page.cells[1][col++].character = static_cast<uint8_t>(c);
  }
  for (const uint8_t code : mosaics) {
    page.cells[1][col].character = code;
    page.cells[1][col++].mosaic = true;
  }
  return page;
}

TEST(TeletextDataExportTest, APageIsItsRowsAsText) {
  // A full block, the top-left sextant alone, and the left column, which
  // Unicode gives a character outside the sextants' own block.
  const std::string text =
      orc::teletext_page_text(page_reading("NEWS", {0x7F, 0x21, 0x35}));
  const std::vector<std::string> lines = lines_of(text);
  ASSERT_EQ(lines.size(), static_cast<size_t>(TeletextPageSnapshot::kRows));
  EXPECT_EQ(lines[0], "");  // the header row was not received
  EXPECT_EQ(lines[1], "NEWS█\U0001FB00▌");
  EXPECT_EQ(lines[2], "");
}

TEST(TeletextDataExportTest, CapitalsStayLettersInAMosaicRow) {
  // With blast-through, 4/0-5/F are capitals even in mosaic mode.
  const std::string text =
      orc::teletext_page_text(page_reading("", {0x41, 0x42}));
  EXPECT_EQ(lines_of(text)[1], "AB");
}

TEST(TeletextDataExportTest, EachKindIsWrittenUnderItsOwnFolder) {
  orc::TeletextAnalysisDataset dataset;
  orc::TeletextCataloguedPage page;
  page.magazine = 1;
  page.page_number = 0x00;
  orc::TeletextCataloguedSubPage sub;
  sub.subcode = 1;
  sub.page = page_reading("NOW");
  orc::TeletextPageVersion earlier;
  earlier.page = page_reading("BEFORE");
  sub.earlier_versions.push_back(earlier);
  page.subpages.push_back(sub);
  dataset.pages.push_back(page);
  dataset.service_data_packets = 3;
  dataset.service_data = {"Network: 0x1234"};
  orc::TeletextDataLine line;
  line.channel = 8;
  line.format = "IDL Format A";
  line.tag = "8";
  line.payload = {1, 2, 3};
  line.summary = "3 bytes";
  dataset.data_lines.push_back(line);

  std::map<std::string, std::string> files;
  const orc::VbiExportTally tally = orc::teletext_export_data(
      dataset, "the report\n",
      [&files](const std::string& path, const uint8_t* data, size_t size) {
        files[path] = std::string(reinterpret_cast<const char*>(data), size);
        return true;
      });
  EXPECT_TRUE(tally.failures.empty());
  EXPECT_EQ(lines_of(files["pages/100/100-0001.txt"])[1], "NOW");
  EXPECT_EQ(lines_of(files["pages/100/100-0001.v1.txt"])[1], "BEFORE");
  EXPECT_NE(files["pages/index.tsv"].find("100/100-0001.v1.txt\t100\t0001\t1"),
            std::string::npos);
  EXPECT_NE(files["service-data.txt"].find("Network: 0x1234"),
            std::string::npos);
  EXPECT_EQ(files["data-lines/8.bin"], std::string("\x01\x02\x03"));
  EXPECT_TRUE(files.count("data-lines/index.tsv") > 0);
  EXPECT_EQ(files["report.txt"], "the report\n");
  EXPECT_NE(files["README.txt"].find("pages/"), std::string::npos);
}

}  // namespace
}  // namespace orc_unit_test
