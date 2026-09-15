/*
 * File:        nabts_sink_deps_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for NabtsSinkDeps's "-" stdio convention
 *
 * The concrete recovery pass (packet slicing, group/record/catalogue
 * assembly) is exercised through its own component tests
 * (nabts_frame_slicer_test.cpp, nabts_data_group_test.cpp, ...); this file
 * covers only the piping guard added to NabtsSinkDeps::analyse(), which
 * returns before any of that machinery runs.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "nabts_sink_deps.h"

#include <gtest/gtest.h>

#include "../../include/video_frame_representation_artifact_mock.h"
#include "../../stage_services_mock.h"

using testing::_;  // NOLINT(bugprone-reserved-identifier)
using testing::Return;
using testing::StrictMock;

namespace orc_unit_test {

namespace {

orc::SourceParameters make_ntsc_params() {
  orc::SourceParameters p{};
  p.system = orc::VideoSystem::NTSC;
  return p;
}

}  // namespace

class NabtsSinkDeps : public ::testing::Test {
 protected:
  orc::NabtsSinkDeps make_deps() {
    orc::NabtsSinkDeps deps(&mockStageServices_);
    deps.init({}, &cancelRequested_);
    return deps;
  }

  MockStageServices mockStageServices_;
  StrictMock<MockVideoFrameRepresentationArtifact> mockRepresentation_;
  std::atomic<bool> cancelRequested_{};
};

////////////////////////////////////////////////////////////////////////////////////////////
// The "-" stdio convention
////////////////////////////////////////////////////////////////////////////////////////////

// A pipe carries the primary packet stream alone: the report, per-record, and
// caption files are separate outputs named after output_path, which "-" does
// not identify a location for. Refused up front — before any writer is
// created, and before the slicing machinery runs at all — rather than
// silently dropped.
TEST_F(NabtsSinkDeps, Analyse_RefusesPipingTogetherWithExportRecords) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_ntsc_params()));
  EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
      .Times(0);

  auto deps = make_deps();
  orc::NabtsSinkOptions options;
  options.output_path = "-";
  options.export_records = true;

  const auto result = deps.analyse(&mockRepresentation_, options);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.message.find("Cannot pipe the NABTS stream to stdout"),
            std::string::npos)
      << result.message;
}

TEST_F(NabtsSinkDeps, Analyse_RefusesPipingTogetherWithExportCaptions) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_ntsc_params()));
  EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
      .Times(0);

  auto deps = make_deps();
  orc::NabtsSinkOptions options;
  options.output_path = "-";
  options.export_captions = true;

  const auto result = deps.analyse(&mockRepresentation_, options);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.message.find("Cannot pipe the NABTS stream to stdout"),
            std::string::npos)
      << result.message;
}

TEST_F(NabtsSinkDeps, Analyse_RefusesPipingTogetherWithWriteReport) {
  EXPECT_CALL(mockRepresentation_, get_video_parameters())
      .WillRepeatedly(Return(make_ntsc_params()));
  EXPECT_CALL(mockStageServices_, create_buffered_file_writer_uint8(_))
      .Times(0);

  auto deps = make_deps();
  orc::NabtsSinkOptions options;
  options.output_path = "-";
  options.write_report = true;

  const auto result = deps.analyse(&mockRepresentation_, options);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.message.find("Cannot pipe the NABTS stream to stdout"),
            std::string::npos)
      << result.message;
}

}  // namespace orc_unit_test
