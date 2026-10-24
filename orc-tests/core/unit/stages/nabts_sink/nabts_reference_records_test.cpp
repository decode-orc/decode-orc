/*
 * File:        nabts_reference_records_test.cpp
 * Module:      orc-core-tests
 * Purpose:     The record files a run exported, read back as the records
 *              Reconstruct lost data rebuilds another recording from
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "nabts_catalogue_view.h"
#include "nabts_sink_deps.h"
#include "vbi-services/teletext_page_decoder.h"
#include "vbi-services/vbi_analysis_results.h"

namespace orc {
namespace {

// A run writes its stream as out.t33, in a folder named for the recording, and
// export_records each record beside it: the folder is what names the record's
// recording, as it names the stream's.
TEST(NabtsReferenceRecords, ARunsRecordFileIsNamedForItsFolder) {
  const std::optional<NabtsReferenceRecord> record =
      nabts_reference_record_file("capture_a", "out.t33.000-000500400-v0.rec",
                                  {0xC1, 0x00, 0x51});
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->source, "capture_a");
  EXPECT_EQ(record->key, "000-000500400-v0");
  EXPECT_EQ(record->data, (std::vector<uint8_t>{0xC1, 0x00, 0x51}));
  // No mask: the 0x00 a lost packet left is how the reference says it lost
  // that byte (NabtsRecordLibrary reads every reference so).
  EXPECT_TRUE(record->present.empty());
}

TEST(NabtsReferenceRecords, ANamedStreamsRecordFileIsNamedForTheStream) {
  const std::optional<NabtsReferenceRecord> record =
      nabts_reference_record_file("captures", "capture-d.t33.000-100-v9-2.rec",
                                  {0x51});
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->source, "capture-d.t33");
  // The second content under one identity keeps its "-2", as the file does.
  EXPECT_EQ(record->key, "000-100-v9-2");
}

TEST(NabtsReferenceRecords, OnlyTheFilesExportRecordsWritesAreRecords) {
  const std::vector<uint8_t> data = {0x51};
  EXPECT_FALSE(nabts_reference_record_file("x", "notes.rec", data));
  EXPECT_FALSE(nabts_reference_record_file("x", "out.t33.txt", data));
  EXPECT_FALSE(nabts_reference_record_file("x", "out.t33", data));
  EXPECT_FALSE(nabts_reference_record_file("x", "out.t33..rec", data));
  // The teletext sink's 34-byte stream exports no NABTS records.
  EXPECT_FALSE(
      nabts_reference_record_file("x", "out.t34.000-100-v0.rec", data));
  // A record with no data is never exported, so an empty file is not one.
  EXPECT_FALSE(nabts_reference_record_file("x", "out.t33.000-100-v0.rec", {}));
}

TEST(NabtsReferenceRecords, AStreamIsNamedAsItsRecordsAre) {
  EXPECT_EQ(nabts_reference_source("capture_b", "out.t33"), "capture_b");
  EXPECT_EQ(nabts_reference_source("captures", "capture-d.t33"),
            "capture-d.t33");
  EXPECT_EQ(nabts_reference_source("", "out.t33"), "out.t33");
}

// SO, then |commands| LINE ABS (2/8) commands to points that differ, each a
// three-byte coordinate word (X3.110 §5.3.2.2.3's default length), with the
// odd parity CEA-516 §3.3 gives a record's data bytes.
std::vector<uint8_t> lineDrawing(uint8_t commands) {
  std::vector<uint8_t> sent = {teletext_odd_parity_encode(0x0E)};
  for (uint8_t i = 0; i < commands; ++i) {
    for (const uint8_t byte :
         {uint8_t{0x28}, static_cast<uint8_t>(0x41 + i % 60),
          static_cast<uint8_t>(0x7E - i % 60),
          static_cast<uint8_t>(0x44 + (i % 5))}) {
      sent.push_back(teletext_odd_parity_encode(byte));
    }
  }
  return sent;
}

// |bytes| with the stretches [start, start + 8) lost with their packets, as
// the 0x00 of a lost packet's filler.
std::vector<uint8_t> losing(std::vector<uint8_t> bytes,
                            const std::vector<size_t>& starts) {
  for (const size_t start : starts) {
    std::fill(bytes.begin() + static_cast<std::ptrdiff_t>(start),
              bytes.begin() + static_cast<std::ptrdiff_t>(start + 8), 0);
  }
  return bytes;
}

// The page 000/000 as this recording received it: |sent|, less the stretches
// at |lost|.
NabtsCataloguedRecord pageLosing(const std::vector<uint8_t>& sent,
                                 const std::vector<size_t>& lost) {
  NabtsCataloguedRecord record;
  record.channel = 0x000;
  record.address_text = "000";
  record.channel_text = "000/000";
  record.record_type = 1;  // CEA-516 §5.2.2.3 non-cyclic presentation
  record.times_seen = 3;
  record.data = losing(sent, lost);
  record.data_present.resize(sent.size());
  for (size_t i = 0; i < sent.size(); ++i) {
    record.data_present[i] = record.data[i] != 0 ? 1 : 0;
  }
  return record;
}

// The condition line of the one page of |data|, with what it lost rebuilt.
std::string rebuiltCondition(const NabtsAnalysisDataset& data) {
  const CatalogueDataset rebuilt = build_nabts_catalogue(
      data, NaplpsRenderMode::kReference, /*reconstruct=*/true);
  EXPECT_EQ(rebuilt.payloads.size(), 1u);
  return rebuilt.payloads.empty() ? std::string()
                                  : rebuilt.payloads[0].condition;
}

// A page whose record lost a stretch, and the same page as another recording's
// run exported it, which lost a different stretch: read back from its file, it
// fills the first and the page says where from.
TEST(NabtsReferenceRecords, ARecordFileFillsWhatTheRecordingLost) {
  const std::vector<uint8_t> sent = lineDrawing(20);
  NabtsAnalysisDataset data;
  data.records.push_back(pageLosing(sent, {37}));
  data.references.push_back(*nabts_reference_record_file(
      "capture_b", "out.t33.000-000-v0.rec", losing(sent, {70})));

  const std::string condition = rebuiltCondition(data);
  EXPECT_NE(condition.find("8 bytes lost in the recording, 8 bytes rebuilt "
                           "from capture_b"),
            std::string::npos)
      << condition;
}

// Where a page's lost stretches came back from several recordings, the line
// under it names them, so a reader can tell which recordings vouched for it.
TEST(NabtsReferenceRecords, ARebuiltPageNamesTheRecordingsItCameFrom) {
  const std::vector<uint8_t> sent = lineDrawing(60);
  NabtsAnalysisDataset data;
  data.records.push_back(pageLosing(sent, {30, 150}));
  data.references.push_back(*nabts_reference_record_file(
      "capture_b", "out.t33.000-000-v0.rec", losing(sent, {150})));
  data.references.push_back(*nabts_reference_record_file(
      "capture_c", "out.t33.000-000-v0.rec", losing(sent, {30})));

  const std::string condition = rebuiltCondition(data);
  EXPECT_NE(condition.find("16 bytes rebuilt from capture_b and "
                           "capture_c"),
            std::string::npos)
      << condition;
}

// Past three recordings the rest are counted, which keeps the line readable.
TEST(NabtsReferenceRecords, PastThreeRecordingsTheRestAreCounted) {
  const std::vector<uint8_t> sent = lineDrawing(60);
  const std::vector<size_t> holes = {30, 90, 150, 210};
  NabtsAnalysisDataset data;
  data.records.push_back(pageLosing(sent, holes));
  for (size_t i = 0; i < holes.size(); ++i) {
    // Each recording kept one of the stretches and lost the rest.
    std::vector<size_t> others;
    for (const size_t hole : holes) {
      if (hole != holes[i]) {
        others.push_back(hole);
      }
    }
    data.references.push_back(*nabts_reference_record_file(
        std::string("REF_") + static_cast<char>('A' + i),
        "out.t33.000-000-v0.rec", losing(sent, others)));
  }

  const std::string condition = rebuiltCondition(data);
  EXPECT_NE(condition.find("32 bytes rebuilt from REF_A, REF_B, REF_C and 1 "
                           "other recording"),
            std::string::npos)
      << condition;
}

}  // namespace
}  // namespace orc
