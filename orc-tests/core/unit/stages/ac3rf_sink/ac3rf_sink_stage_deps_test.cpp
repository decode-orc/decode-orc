/*
 * File:        ac3rf_sink_stage_deps_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for AC3RFSinkStageDeps's "-" stdio convention
 *
 * decode_and_write_ac3() writes through a real std::ofstream (or, when
 * piping, real stdout) with no injectable writer service to mock, unlike the
 * sinks built on IStageServices's buffered writers. AC3RFSinkStageDeps
 * exposes open_output() as a protected virtual precisely so a test can
 * substitute an in-memory stream instead of ever touching a real file or the
 * process's real stdout.
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
// instead of ever opening a real file or writing to the process's real
// stdout.
class TestableAC3RFSinkStageDeps : public orc::AC3RFSinkStageDeps {
 public:
  std::ostringstream captured;
  std::string opened_path;
  bool opened_piping = false;

 protected:
  std::ostream* open_output(const std::string& output_path, bool piping,
                            std::ofstream& /*file_storage*/) override {
    opened_path = output_path;
    opened_piping = piping;
    return &captured;
  }
};

}  // namespace

// "-" is detected and passed straight through to the seam unchanged — there
// is no extension-mangling logic in this sink to guard, so this is the whole
// piping contract for AC3RFSinkStageDeps: is_pipe_path() must fire and the
// seam must receive "-" itself, not a mangled path.
TEST(AC3RFSinkStageDeps, DecodeAndWriteAc3_PipesToStdoutWithPathUnchanged) {
  TestableAC3RFSinkStageDeps deps;
  deps.init({}, nullptr);

  StrictMock<MockVideoFrameRepresentationArtifact> rep;
  // Empty range: an AC3 bitstream fixture is unnecessary to exercise the
  // piping seam itself, and decode_and_write_ac3() does not treat zero
  // frames written as a failure (unlike CVBS Sink).
  EXPECT_CALL(rep, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{1, 0}));

  const auto result = deps.decode_and_write_ac3(&rep, "-");

  EXPECT_TRUE(result.success) << result.status_message;
  EXPECT_EQ(deps.opened_path, "-");
  EXPECT_TRUE(deps.opened_piping);
}

TEST(AC3RFSinkStageDeps, DecodeAndWriteAc3_RealPathIsNotTreatedAsPiping) {
  TestableAC3RFSinkStageDeps deps;
  deps.init({}, nullptr);

  StrictMock<MockVideoFrameRepresentationArtifact> rep;
  EXPECT_CALL(rep, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{1, 0}));

  const auto result = deps.decode_and_write_ac3(&rep, "out.ac3");

  EXPECT_TRUE(result.success) << result.status_message;
  EXPECT_EQ(deps.opened_path, "out.ac3");
  EXPECT_FALSE(deps.opened_piping);
}

}  // namespace orc_unit_test
