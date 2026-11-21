/*
 * File:        cvbs_sink_stage_deps_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for CVBSSinkStageDeps's Y/C-to-composite combining
 *              and its refusals
 *
 * write_cvbs() writes its primary payload through a real std::ofstream with
 * no injectable writer service to mock, unlike the sinks built on
 * IStageServices's buffered writers. CVBSSinkStageDeps exposes
 * open_primary_output() as a protected virtual precisely so a test can
 * substitute an in-memory stream instead of ever touching a real file. The
 * output path names a directory that does not exist, so the .meta and
 * sidecars written beside the payload cannot land anywhere either.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "cvbs_sink_stage_deps.h"

#include <gtest/gtest.h>

#include <cstring>
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
  MOCK_METHOD(const sample_type*, get_frame_luma, (orc::FrameID),
              (const, override));
  MOCK_METHOD(const sample_type*, get_frame_chroma, (orc::FrameID),
              (const, override));
};

// An output base in a directory that does not exist: nothing can be written
// to it, and nothing stale beside it can be removed.
constexpr const char* kUnwritableBase = "/nonexistent/combine";

// Test seam: captures the primary payload into an in-memory stream instead of
// ever opening a real file.
class TestableCVBSSinkStageDeps : public orc::CVBSSinkStageDeps {
 public:
  std::ostringstream captured;
  std::string opened_path;
  bool fail_open = false;

 protected:
  std::ostream* open_primary_output(const std::string& primary_path,
                                    std::ofstream& /*file_storage*/) override {
    opened_path = primary_path;
    if (fail_open) return nullptr;
    return &captured;
  }
};

// Sets up a single valid PAL Y/C frame with 4 samples per channel. The
// chroma DC offset is left unset so the CVBS spec default (512) applies.
void expect_one_pal_yc_frame(StrictMock<MockVfrWithSidecars>& rep,
                             std::vector<int16_t>& luma_storage,
                             std::vector<int16_t>& chroma_storage) {
  EXPECT_CALL(rep, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  orc::SourceParameters params;
  params.system = orc::VideoSystem::PAL;
  EXPECT_CALL(rep, get_video_parameters()).WillRepeatedly(Return(params));
  EXPECT_CALL(rep, has_separate_channels()).WillRepeatedly(Return(true));
  EXPECT_CALL(rep, has_frame(orc::FrameID(0))).WillRepeatedly(Return(true));
  orc::FrameDescriptor desc;
  desc.samples_total = 4;
  EXPECT_CALL(rep, get_frame_descriptor(orc::FrameID(0)))
      .WillRepeatedly(Return(desc));
  luma_storage = {256, 400, 600, 844};
  chroma_storage = {512, 562, 462, 612};
  EXPECT_CALL(rep, get_frame_luma(orc::FrameID(0)))
      .WillRepeatedly(Return(luma_storage.data()));
  EXPECT_CALL(rep, get_frame_chroma(orc::FrameID(0)))
      .WillRepeatedly(Return(chroma_storage.data()));
  EXPECT_CALL(rep, has_efm()).WillRepeatedly(Return(false));
  EXPECT_CALL(rep, has_ac3_rf()).WillRepeatedly(Return(false));
  EXPECT_CALL(rep, audio_channel_pair_count()).WillRepeatedly(Return(0));
  EXPECT_CALL(rep, get_dropout_hints(orc::FrameID(0)))
      .WillRepeatedly(Return(std::vector<orc::DropoutRun>{}));
}

std::vector<int16_t> captured_u10_samples(const std::string& bytes) {
  std::vector<int16_t> samples(bytes.size() / sizeof(int16_t));
  std::memcpy(samples.data(), bytes.data(), samples.size() * sizeof(int16_t));
  return samples;
}

}  // namespace

// combine_yc writes one composite plane whose samples are Y + C with the
// chroma DC offset removed. get_frame() is never used (StrictMock). The .meta
// cannot be written beside an unwritable base, so the run itself fails after
// the plane is written; the plane is what is under test.
TEST(CVBSSinkStageDeps, WriteCvbs_CombinesYCIntoOneCompositePlane) {
  TestableCVBSSinkStageDeps deps;
  deps.init({}, nullptr);

  StrictMock<MockVfrWithSidecars> rep;
  std::vector<int16_t> luma;
  std::vector<int16_t> chroma;
  expect_one_pal_yc_frame(rep, luma, chroma);

  orc::CVBSSinkWriteConfig config;
  config.output_base_path = kUnwritableBase;
  config.combine_yc = true;

  const auto result = deps.write_cvbs(&rep, config);

  EXPECT_EQ(result.frames_written, 1u) << result.status_message;
  EXPECT_EQ(deps.opened_path, std::string(kUnwritableBase) + ".cvbs");
  EXPECT_EQ(captured_u10_samples(deps.captured.str()),
            (std::vector<int16_t>{256, 450, 550, 944}));
}

// A TBC Y/C source reports its own chroma DC offset; that value, not the
// CVBS default, is removed before summing.
TEST(CVBSSinkStageDeps, WriteCvbs_CombineYCUsesSourceChromaDcOffset) {
  TestableCVBSSinkStageDeps deps;
  deps.init({}, nullptr);

  StrictMock<MockVfrWithSidecars> rep;
  std::vector<int16_t> luma;
  std::vector<int16_t> chroma;
  expect_one_pal_yc_frame(rep, luma, chroma);
  orc::SourceParameters params;
  params.system = orc::VideoSystem::PAL;
  params.chroma_dc_offset = 500;
  EXPECT_CALL(rep, get_video_parameters()).WillRepeatedly(Return(params));

  orc::CVBSSinkWriteConfig config;
  config.output_base_path = kUnwritableBase;
  config.combine_yc = true;

  const auto result = deps.write_cvbs(&rep, config);

  EXPECT_EQ(result.frames_written, 1u) << result.status_message;
  EXPECT_EQ(captured_u10_samples(deps.captured.str()),
            (std::vector<int16_t>{268, 462, 562, 956}));
}

TEST(CVBSSinkStageDeps, WriteCvbs_CombineYCRefusesCompositeInput) {
  TestableCVBSSinkStageDeps deps;
  deps.init({}, nullptr);

  StrictMock<MockVfrWithSidecars> rep;
  EXPECT_CALL(rep, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 0}));
  orc::SourceParameters params;
  params.system = orc::VideoSystem::PAL;
  EXPECT_CALL(rep, get_video_parameters()).WillRepeatedly(Return(params));
  EXPECT_CALL(rep, has_separate_channels()).WillRepeatedly(Return(false));

  orc::CVBSSinkWriteConfig config;
  config.output_base_path = kUnwritableBase;
  config.combine_yc = true;

  const auto result = deps.write_cvbs(&rep, config);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.status_message.find("separate Y/C channels"),
            std::string::npos)
      << result.status_message;
  EXPECT_TRUE(deps.opened_path.empty());
}

TEST(CVBSSinkStageDeps, WriteCvbs_CombineYCRefusesYCSignalType) {
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
  config.output_base_path = kUnwritableBase;
  config.signal_type = "yc";
  config.combine_yc = true;

  const auto result = deps.write_cvbs(&rep, config);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.status_message.find("signal_type 'composite'"),
            std::string::npos)
      << result.status_message;
  EXPECT_TRUE(deps.opened_path.empty());
}

// Audio a RIFF sidecar cannot describe is refused before anything is
// written: the CVBS format allows only RIFF until
// cvbs-file-format-specification#16 is resolved, and a wrapped size would
// lose the audio past it.
TEST(CVBSSinkStageDeps, WriteCvbs_RefusesAudioPastTheRiffLimit) {
  TestableCVBSSinkStageDeps deps;
  deps.init({}, nullptr);

  StrictMock<MockVfrWithSidecars> rep;
  EXPECT_CALL(rep, frame_range())
      .WillRepeatedly(Return(orc::FrameIDRange{0, 540000 - 1}));
  orc::SourceParameters params;
  params.system = orc::VideoSystem::PAL;
  EXPECT_CALL(rep, get_video_parameters()).WillRepeatedly(Return(params));
  EXPECT_CALL(rep, audio_channel_pair_count()).WillRepeatedly(Return(1));

  orc::CVBSSinkWriteConfig config;
  config.output_base_path = "/nonexistent/six_hours.cvbs";

  const auto result = deps.write_cvbs(&rep, config);

  EXPECT_FALSE(result.success);
  EXPECT_NE(result.status_message.find("cvbs-file-format-specification#16"),
            std::string::npos)
      << result.status_message;
  EXPECT_TRUE(deps.opened_path.empty());
}

}  // namespace orc_unit_test
