/*
 * File:        tbc_lock_state_roundtrip_test.cpp
 * Module:      orc-core functional tests
 * Purpose:     TBC Sink's is_subcarrier_locked read back by TBC Source
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * Functional (not unit): writes and reads a real SQLite .tbc.db in a
 * temporary directory, which unit tests may not touch (AGENTS.md §4.2).
 */

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/orc_source_parameters.h>
#include <sqlite3.h>

#include <filesystem>
#include <string>

#include "tbc_metadata_reader.h"
#include "tbc_metadata_writer.h"

namespace orc {
namespace {

// PAL_M shares NTSC's 10-bit levels (cvbs_signal_constants.h).
SourceParameters pal_m_params(bool locked) {
  SourceParameters p;
  p.system = VideoSystem::PAL_M;
  p.frame_width_nominal = kPalMSamplesPerLine;
  p.frame_height = kPalMFrameLines;
  p.number_of_sequential_frames = 1;
  p.sync_tip_level = kNtscSyncTip;
  p.blanking_level = kNtscBlanking;
  p.black_level = kNtscBlack;
  p.white_level = kNtscWhite;
  p.peak_level = kNtscPeak;
  p.active_video_start = kNtscActiveVideoStart;
  p.active_video_end = kNtscActiveVideoEnd;
  p.decoder = "vhs-decode";
  p.is_subcarrier_locked = locked;
  return p;
}

// TBC Sink writes the pipeline's lock state into the capture record, and
// TBC Source reads it back for PAL_M, the one system where it is honoured.
// A vhs-decode PAL_M capture (unlocked) therefore stays unlocked through a
// TBC Sink → TBC Source round trip.
TEST(TBCLockStateRoundTripTest, SinkWritesWhatSourceReads) {
  const auto dir =
      std::filesystem::path(::testing::TempDir()) / "orc_tbc_lock_state";
  std::filesystem::create_directories(dir);
  for (const bool locked : {false, true}) {
    SCOPED_TRACE(locked);
    const auto db = dir / (locked ? "locked.tbc.db" : "unlocked.tbc.db");
    std::filesystem::remove(db);
    {
      TBCMetadataWriter writer;
      ASSERT_TRUE(writer.open(db.string()));
      ASSERT_TRUE(writer.write_video_parameters(pal_m_params(locked)));
      writer.close();
    }
    TBCMetadataSqliteReader reader;
    ASSERT_TRUE(reader.open(db.string()));
    const auto read = reader.read_video_parameters();
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->system, VideoSystem::PAL_M);
    EXPECT_EQ(read->is_subcarrier_locked, locked);
    reader.close();
    std::filesystem::remove(db);
  }
}

// The ld-decode schema has no SECAM: TBC Sink writes a SECAM capture as PAL
// with a capture note, and TBC Source reads a PAL capture (a SECAM project
// then reads it as SECAM, tbc_system_resolution.h).
TEST(TBCLockStateRoundTripTest, SecamIsWrittenAsPalWithACaptureNote) {
  const auto dir =
      std::filesystem::path(::testing::TempDir()) / "orc_tbc_lock_state";
  std::filesystem::create_directories(dir);
  const auto db = dir / "secam.tbc.db";
  std::filesystem::remove(db);
  SourceParameters p;
  p.system = VideoSystem::SECAM;
  p.frame_width_nominal = kSecamSamplesPerLineNominal;
  p.frame_height = kSecamFrameLines;
  p.number_of_sequential_frames = 1;
  p.sync_tip_level = kSecamSyncTip;
  p.blanking_level = kSecamBlanking;
  p.black_level = kSecamBlack;
  p.white_level = kSecamWhite;
  p.peak_level = kSecamPeak;
  p.decoder = "vhs-decode";
  p.is_subcarrier_locked = false;
  {
    TBCMetadataWriter writer;
    ASSERT_TRUE(writer.open(db.string()));
    ASSERT_TRUE(writer.write_video_parameters(p));
    writer.close();
  }

  sqlite3* handle = nullptr;
  ASSERT_EQ(sqlite3_open_v2(db.string().c_str(), &handle, SQLITE_OPEN_READONLY,
                            nullptr),
            SQLITE_OK);
  sqlite3_stmt* stmt = nullptr;
  ASSERT_EQ(
      sqlite3_prepare_v2(handle, "SELECT system, capture_notes FROM capture",
                         -1, &stmt, nullptr),
      SQLITE_OK);
  ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
  EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)),
               "PAL");
  const std::string notes =
      reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
  EXPECT_EQ(notes.rfind("SECAM:", 0), 0u) << notes;
  sqlite3_finalize(stmt);
  sqlite3_close(handle);

  TBCMetadataSqliteReader reader;
  ASSERT_TRUE(reader.open(db.string()));
  const auto read = reader.read_video_parameters();
  ASSERT_TRUE(read.has_value());
  EXPECT_EQ(read->system, VideoSystem::PAL);
  reader.close();
  std::filesystem::remove(db);
}

}  // namespace
}  // namespace orc
