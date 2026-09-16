/*
 * File:        tbc_metadata_writer_test.cpp
 * Module:      orc-core functional tests
 * Purpose:     Tests for TBCMetadataWriter::update_sequential_field_count() —
 *              the fix that keeps a piped/unbounded tbc_stream_source export's
 *              persisted .tbc.db from claiming its huge placeholder frame
 *              count instead of the real number of fields written
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 *
 * Functional (not unit): writes a real SQLite database to a temporary file
 * on disk, which unit tests may not touch (AGENTS.md §4.2).
 */

#include "tbc_metadata_writer.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace orc_unit_test {
namespace {

// Reads back the single capture row's number_of_sequential_fields column
// directly, independent of TBCMetadataWriter, so the test verifies what is
// actually persisted rather than trusting the class under test to report on
// itself correctly.
int32_t read_persisted_sequential_field_count(const std::string& db_path) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(db_path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    sqlite3_close(db);
    return -1;
  }
  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT number_of_sequential_fields FROM capture";
  int32_t value = -1;
  if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) == SQLITE_OK &&
      sqlite3_step(stmt) == SQLITE_ROW) {
    value = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return value;
}

class TBCMetadataWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           (std::string("orc-tbc-meta-") + info->test_suite_name() + "-" +
            info->name());
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    db_path_ = (dir_ / "capture.tbc.db").string();
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  std::filesystem::path dir_;
  std::string db_path_;
};

TEST_F(TBCMetadataWriterTest,
       UpdateSequentialFieldCount_OverwritesTheValueWriteVideoParametersSet) {
  orc::TBCMetadataWriter writer;
  ASSERT_TRUE(writer.open(db_path_));

  orc::SourceParameters params;
  params.system = orc::VideoSystem::PAL;
  // The capture table's decoder column is NOT NULL with a CHECK constraint
  // limiting it to these two values — tbc_sink_stage_deps.cpp normalises
  // any other value before calling write_video_parameters(), so a direct
  // caller has to supply one too.
  params.decoder = "ld-decode";
  // Stands in for the huge placeholder an unbounded (frame_count=0)
  // tbc_stream_source declares up front — see kUnboundedFrameCount in
  // tbc_stream_source_stage.cpp.
  constexpr int32_t kPlaceholderFrameCount = 1'000'000;
  params.number_of_sequential_frames = kPlaceholderFrameCount;
  ASSERT_TRUE(writer.write_video_parameters(params));
  EXPECT_EQ(read_persisted_sequential_field_count(db_path_),
            kPlaceholderFrameCount * 2);

  // The real export turned out much shorter — correct the persisted value.
  constexpr int32_t kActualFieldsWritten = 4200;
  EXPECT_TRUE(writer.update_sequential_field_count(kActualFieldsWritten));

  writer.close();

  EXPECT_EQ(read_persisted_sequential_field_count(db_path_),
            kActualFieldsWritten);
}

TEST_F(TBCMetadataWriterTest,
       UpdateSequentialFieldCount_FailsWithoutAPriorCaptureRecord) {
  orc::TBCMetadataWriter writer;
  ASSERT_TRUE(writer.open(db_path_));

  // No write_video_parameters() call yet, so there is no capture row (and no
  // capture_id) to attach the correction to.
  EXPECT_FALSE(writer.update_sequential_field_count(100));

  writer.close();
}

}  // namespace
}  // namespace orc_unit_test
