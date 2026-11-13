/*
 * File:        ac3rf_sink_stage_deps_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for AC3RFSinkStageDeps's output handling
 *
 * decode_and_write_ac3() writes through a real std::ofstream with no
 * injectable writer service to mock, unlike the sinks built on
 * IStageServices's buffered writers. AC3RFSinkStageDeps exposes open_output()
 * as a protected virtual precisely so a test can substitute an in-memory
 * stream instead of ever touching a real file.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "ac3rf_sink_stage_deps.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "../../include/video_frame_representation_artifact_mock.h"

using testing::Return;
using testing::StrictMock;

namespace orc_unit_test {

namespace {

// Test seam: captures whatever would be written into an in-memory stream
// instead of ever opening a real file.
class TestableAC3RFSinkStageDeps : public orc::AC3RFSinkStageDeps {
 public:
  std::ostringstream captured;
  std::string opened_path;

 protected:
  std::ostream* open_output(const std::string& output_path,
                            std::ofstream& /*file_storage*/) override {
    opened_path = output_path;
    return &captured;
  }
};

}  // namespace

TEST(AC3RFSinkStageDeps, DecodeAndWriteAc3_OpensTheOutputPathUnchanged) {
  TestableAC3RFSinkStageDeps deps;
  deps.init({}, nullptr);

  StrictMock<MockVideoFrameRepresentationArtifact> rep;
  // Empty range: an AC3 bitstream fixture is unnecessary to exercise the
  // output seam itself, and decode_and_write_ac3() does not treat zero
  // frames written as a failure (unlike CVBS Sink).
  EXPECT_CALL(rep, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{1, 0}));

  const auto result = deps.decode_and_write_ac3(&rep, "out.ac3");

  EXPECT_TRUE(result.success) << result.status_message;
  EXPECT_EQ(deps.opened_path, "out.ac3");
}

}  // namespace orc_unit_test
