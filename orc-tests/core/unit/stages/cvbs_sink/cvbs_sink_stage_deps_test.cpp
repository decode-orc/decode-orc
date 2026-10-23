/*
 * File:        cvbs_sink_stage_deps_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for CVBSSinkStageDeps's "-" stdio convention
 *
 * write_cvbs() writes its primary payload through a real std::ofstream (or,
 * when piping, real stdout) with no injectable writer service to mock, unlike
 * the sinks built on IStageServices's buffered writers. CVBSSinkStageDeps
 * exposes open_primary_output() as a protected virtual precisely so a test
 * can substitute an in-memory stream instead of ever touching a real file or
 * the process's real stdout.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "cvbs_sink_stage_deps.h"

#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

#include "../../include/video_frame_representation_artifact_mock.h"

using testing::Return;
using testing::StrictMock;

namespace orc_unit_test {

namespace {

// has_efm(), has_ac3_rf(), has_separate_channels() and get_ac3_symbols() are
// plain virtuals on VideoFrameRepresentation rather than mocked members of
// the shared artifact mock (they default to false/empty), so the sidecar and
// Y/C tests extend it locally — the same pattern TBC Sink's deps test uses.
class MockVfrWithSidecars : public MockVideoFrameRepresentationArtifact {
 public:
  MOCK_METHOD(bool, has_efm, (), (const, override));
  MOCK_METHOD(bool, has_ac3_rf, (), (const, override));
  MOCK_METHOD(bool, has_separate_channels, (), (const, override));
};

// Test seam: captures the primary payload into an in-memory stream instead of
// ever opening a real file or writing to the process's real stdout.
class TestableCVBSSinkStageDeps : public orc::CVBSSinkStageDeps {
 public:
  std::ostringstream captured;
  std::string opened_path;
  bool opened_piping = false;
  bool fail_open = false;

 protected:
  std::ostream* open_primary_output(const std::string& primary_path,
                                    bool piping,
                                    std::ofstream& /*file_storage*/) override {
    opened_path = primary_path;
    opened_piping = piping;
    if (fail_open) return nullptr;
    return &captured;
  }
};

// Sets up a single valid PAL frame with 4 samples, values 100/200/300/400 —
// enough for write_cvbs() to reach the primary write and succeed.
void expect_one_pal_frame(StrictMock<MockVfrWithSidecars>& rep,
                          std::vector<int16_t>& sample_storage) {
  EXPECT_CALL(rep, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  orc::SourceParameters params;
  params.system = orc::VideoSystem::PAL;
  EXPECT_CALL(rep, get_video_parameters()).WillRepeatedly(Return(params));
  EXPECT_CALL(rep, has_frame(orc::FrameID(0))).WillRepeatedly(Return(true));
  orc::FrameDescriptor desc;
  desc.samples_total = 4;
  EXPECT_CALL(rep, get_frame_descriptor(orc::FrameID(0)))
      .WillRepeatedly(Return(desc));
  sample_storage = {100, 200, 300, 400};
  EXPECT_CALL(rep, get_frame(orc::FrameID(0)))
      .WillRepeatedly(Return(sample_storage.data()));
}

}  // namespace

// "-" is passed straight through to the seam unchanged — no
// derive_cvbs_output_base() mangling, no .cvbs extension appended — and the
// frame's samples still reach the (captured) primary stream.
TEST(CVBSSinkStageDeps, WriteCvbs_PipesToStdoutWithPathUnchanged) {
  TestableCVBSSinkStageDeps deps;
  deps.init({}, nullptr);

  StrictMock<MockVfrWithSidecars> rep;
  std::vector<int16_t> samples;
  expect_one_pal_frame(rep, samples);
  EXPECT_CALL(rep, has_efm()).WillRepeatedly(Return(false));
  EXPECT_CALL(rep, has_ac3_rf()).WillRepeatedly(Return(false));
  EXPECT_CALL(rep, audio_channel_pair_count()).WillRepeatedly(Return(0));

  orc::CVBSSinkWriteConfig config;
  config.output_base_path = "-";

  const auto result = deps.write_cvbs(&rep, config);

  ASSERT_TRUE(result.success) << result.status_message;
  EXPECT_EQ(result.frames_written, 1u);
  EXPECT_EQ(deps.opened_path, "-");
  EXPECT_TRUE(deps.opened_piping);
  // One uint16_t per sample, encoded.
  EXPECT_EQ(deps.captured.str().size(), samples.size() * sizeof(uint16_t));
}

// A pipe carries the primary payload alone: a Y/C capture needs two separate
// streams (luma + chroma), which a single pipe cannot carry. Refused before
// any output is opened, rather than silently writing only one of the two.
TEST(CVBSSinkStageDeps, WriteCvbs_RefusesYcOutputWhenPiping) {
  TestableCVBSSinkStageDeps deps;
  deps.init({}, nullptr);

  StrictMock<MockVfrWithSidecars> rep;
  EXPECT_CALL(rep, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  orc::SourceParameters params;
  params.system = orc::VideoSystem::PAL;
  EXPECT_CALL(rep, get_video_parameters()).WillRepeatedly(Return(params));
  EXPECT_CALL(rep, has_separate_channels()).WillRepeatedly(Return(true));

  orc::CVBSSinkWriteConfig config;
  config.output_base_path = "-";
  config.signal_type = "yc";

  const auto result = deps.write_cvbs(&rep, config);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.status_message.find("Cannot pipe Y/C output"),
            std::string::npos)
      << result.status_message;
  EXPECT_TRUE(deps.opened_path.empty());
}

// A pipe carries only the primary CVBS stream: the .dropouts.meta, EFM and
// AC3 sidecars this input carries are never opened or queried for sample
// data when piping — only has_efm()/has_ac3_rf() are read, to decide whether
// to log that they were dropped.
TEST(CVBSSinkStageDeps, WriteCvbs_DropsEfmAndAc3SidecarsWhenPiping) {
  TestableCVBSSinkStageDeps deps;
  deps.init({}, nullptr);

  StrictMock<MockVfrWithSidecars> rep;
  std::vector<int16_t> samples;
  expect_one_pal_frame(rep, samples);
  // has_efm() true short-circuits the warning's `||` chain, so neither
  // has_ac3_rf() nor audio_channel_pair_count() is even queried — and none of
  // get_efm_samples()/get_ac3_symbols()/get_audio_samples() is ever called
  // (a StrictMock fails the test if they are).
  EXPECT_CALL(rep, has_efm()).WillRepeatedly(Return(true));

  orc::CVBSSinkWriteConfig config;
  config.output_base_path = "-";

  const auto result = deps.write_cvbs(&rep, config);

  ASSERT_TRUE(result.success) << result.status_message;
  EXPECT_EQ(result.frames_written, 1u);
}

}  // namespace orc_unit_test
