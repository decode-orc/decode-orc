/*
 * File:        nabts_data_export_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the export of everything a NABTS run decoded
 *
 * Covers: each kind of data written as the file it is under its own folder,
 * the indexes and the README, a file that cannot be written leaving the rest,
 * and the names carried files are written under. Files are collected through
 * the writer the export is given, so nothing touches a disk.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_data_export.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace orc_unit_test {
namespace {

using orc::NabtsAnalysisDataset;

/// What an export wrote, path by path.
struct Collected {
  std::map<std::string, std::string> files;
  std::vector<std::string> refuse;

  orc::NabtsExportWriter writer() {
    return [this](const std::string& path, const uint8_t* data, size_t size) {
      for (const std::string& refused : refuse) {
        if (path == refused) {
          return false;
        }
      }
      files[path] = std::string(reinterpret_cast<const char*>(data), size);
      return true;
    };
  }
  bool has(const std::string& path) const { return files.count(path) > 0; }
};

std::vector<uint8_t> bytes_of(const std::string& text) {
  return {text.begin(), text.end()};
}

/// A run that found one of every kind of data.
NabtsAnalysisDataset every_kind() {
  NabtsAnalysisDataset dataset;

  orc::NabtsCataloguedRecord record;
  record.channel = 0x000;
  record.address_text = "150";
  record.version = 3;
  record.times_seen = 5;
  record.update = true;
  record.data = {0x0E, 0x41, 0x42, 0x43};
  record.data_present = {1, 1, 0, 1};
  dataset.records.push_back(record);
  orc::NabtsCataloguedRecord header_only;
  header_only.channel = 0x000;
  header_only.address_text = "151";
  dataset.records.push_back(header_only);

  orc::NabtsDatacastListing datacast;
  datacast.channel = 0x242;
  datacast.payload = bytes_of("FORECAST FOR TODAY\x01\x02");
  datacast.payload_bytes = datacast.payload.size();
  datacast.text = "FORECAST FOR TODAY\n";
  datacast.carried = "readings\n";
  datacast.summary = "20 bytes";
  datacast.table_columns = {"Date", "Title"};
  datacast.table_rows = {{"010203", "A\tTITLE"}};
  dataset.datacasts.push_back(datacast);

  orc::NabtsCarriedFile file;
  file.channel = 0x243;
  file.last_channel = 0x243;
  file.transfer = "file carousel";
  file.name = "MEMO0001.HTM";
  file.time = "2001-02-03 04:05:06";
  file.format = "html";
  file.data = bytes_of("<html>weather</html>");
  file.copies = 4;
  file.contents = "what it holds\n";
  dataset.carried_files.push_back(file);

  orc::NabtsCarriedFile page;
  page.channel = 0x0F0;
  page.last_channel = 0x0FF;
  page.transfer = "framed service";
  page.name = "index.html";
  page.source = "http://example.com/news/index.html";
  page.format = "html";
  page.data = bytes_of("<html>news</html>");
  dataset.carried_files.push_back(page);

  orc::NabtsFramedService framed;
  framed.identifier = 0x95a17491U;
  framed.channels = {0x0F0, 0x0FF};
  framed.stream = {1, 2, 3};
  framed.summary = "3 frames";
  framed.headers = "00 01\n";
  dataset.framed_services.push_back(framed);

  orc::NabtsPrivateData reserved;
  reserved.channel = 0xCFD;
  reserved.type = 15;
  reserved.groups = 2;
  reserved.packets = 9;
  reserved.text_groups = 2;
  reserved.shown = "the groups as they arrived\n";
  dataset.private_data.push_back(reserved);

  orc::NabtsNetworkMessage message;
  message.channel = 0xCD0;
  message.record_address = "CB2";
  message.kind = "message";
  message.form = "station message";
  message.title = "SCHEDULE CHANGE";
  message.text = "THE PROGRAM MOVES.\n";
  message.complete = true;
  dataset.network_messages.push_back(message);

  dataset.unplaced_runs = "a run of bytes\n";
  return dataset;
}

TEST(NabtsDataExportTest, EachKindIsWrittenAsTheFileItIsUnderItsOwnFolder) {
  Collected out;
  const orc::NabtsExportTally tally =
      orc::nabts_export_data(every_kind(), "the report\n", out.writer());
  EXPECT_TRUE(tally.failures.empty());
  EXPECT_EQ(tally.files, out.files.size());

  // A record as its copies voted it, named for its identity; one whose every
  // copy was header-only has nothing to write.
  ASSERT_TRUE(out.has("records/000/000-150-v3.rec"));
  EXPECT_EQ(out.files["records/000/000-150-v3.rec"], "\x0E\x41\x42\x43");
  EXPECT_FALSE(out.has("records/000/000-151-v0.rec"));
  EXPECT_NE(out.files["records/index.tsv"].find(
                "000/000-150-v3.rec\t000\t150\t3\t1\t0\tupdate\t"),
            std::string::npos)
      << out.files["records/index.tsv"];

  EXPECT_EQ(out.files["datacasts/242.bin"], "FORECAST FOR TODAY\x01\x02");
  EXPECT_EQ(out.files["datacasts/242.txt"], "FORECAST FOR TODAY\n");
  EXPECT_EQ(out.files["datacasts/242.carried.txt"], "readings\n");
  EXPECT_EQ(out.files["datacasts/242.tsv"], "Date\tTitle\n010203\tA TITLE\n");
  EXPECT_TRUE(out.has("datacasts/index.tsv"));

  // A carousel's file as sent, dated by its own time; a framed service's in
  // the web folder, as the site it came from.
  ASSERT_TRUE(out.has("files/243/MEMO0001-20010203T040506.htm"));
  EXPECT_EQ(out.files["files/243/MEMO0001-20010203T040506.htm"],
            "<html>weather</html>");
  EXPECT_EQ(out.files["files/243/MEMO0001-20010203T040506.htm.contents.txt"],
            "what it holds\n");
  EXPECT_TRUE(out.has("web/example.com/news/index.html"));
  EXPECT_TRUE(out.has("web/index.html"));
  EXPECT_FALSE(out.has("files/0F0-0FF/index.html"));

  EXPECT_EQ(out.files["frames/0F0-0FF.95a17491.frames"], "\x01\x02\x03");
  EXPECT_NE(out.files["frames/0F0-0FF.95a17491.txt"].find("3 frames"),
            std::string::npos);

  EXPECT_TRUE(out.has("messages/CD0-CB2/0000-0-SCHEDULE_CHANGE.txt"));
  EXPECT_NE(out.files["messages/index.tsv"].find("SCHEDULE CHANGE"),
            std::string::npos);

  EXPECT_NE(
      out.files["private/CFD-type15.txt"].find("the groups as they arrived"),
      std::string::npos);
  EXPECT_EQ(out.files["fragments.txt"], "a run of bytes\n");
  EXPECT_EQ(out.files["report.txt"], "the report\n");

  // The README says what each folder written holds, and what came of it.
  const std::string& readme = out.files["README.txt"];
  for (const char* part : {"records/", "datacasts/", "files/", "frames/",
                           "web/", "messages/", "private/", "fragments.txt"}) {
    EXPECT_NE(readme.find(part), std::string::npos) << part;
  }
  EXPECT_EQ(readme.find("captions/"), std::string::npos);
  EXPECT_NE(readme.find("1 record as their copies voted them"),
            std::string::npos)
      << readme;
}

TEST(NabtsDataExportTest, AFileThatCannotBeWrittenLeavesTheRest) {
  Collected out;
  out.refuse = {"datacasts/242.bin"};
  const orc::NabtsExportTally tally =
      orc::nabts_export_data(every_kind(), {}, out.writer());
  ASSERT_EQ(tally.failures.size(), 1U);
  EXPECT_EQ(tally.failures[0], "datacasts/242.bin");
  EXPECT_TRUE(out.has("records/000/000-150-v3.rec"));
  EXPECT_TRUE(out.has("messages/CD0-CB2/0000-0-SCHEDULE_CHANGE.txt"));
  EXPECT_FALSE(out.has("report.txt"));
}

TEST(NabtsDataExportTest, ARunThatFoundNothingWritesNothing) {
  Collected out;
  const orc::NabtsExportTally tally =
      orc::nabts_export_data(NabtsAnalysisDataset{}, {}, out.writer());
  EXPECT_EQ(tally.files, 0U);
  EXPECT_TRUE(out.files.empty());
}

TEST(NabtsDataExportTest, ACarriedFileIsNamedForWhatItsBytesAre) {
  orc::NabtsCarriedFile picture;
  picture.name = "a b?.JPG";
  picture.format = "gif";
  const orc::NabtsCarriedFileName named = orc::nabts_carried_file_name(picture);
  EXPECT_EQ(named.base, "a_b_");
  EXPECT_EQ(named.extension, "gif");

  orc::NabtsCarriedFile log;
  log.name = "LOG";
  log.format = "text";
  log.time = "2001-02-04 04:05:07";
  const orc::NabtsCarriedFileName dated = orc::nabts_carried_file_name(log);
  EXPECT_EQ(dated.base, "LOG-20010204T040507");
  EXPECT_EQ(dated.extension, "txt");

  log.channel = 0x0F0;
  log.last_channel = 0x0FF;
  EXPECT_EQ(orc::nabts_carried_file_addresses(log), "0F0-0FF");
}

}  // namespace
}  // namespace orc_unit_test
