/*
 * File:        raw_efm_sink_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for RawEFMSinkStage parameter contracts and trigger
 * behavior
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>

#include "../../../../orc/plugins/stages/raw_efm_sink/efm_sink_stage.h"
#include "../../../../orc/plugins/stages/raw_efm_sink/efm_sink_stage_deps_interface.h"
#include "../../include/observation_context_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"

namespace orc_unit_test {
using testing::NiceMock;
using testing::Return;
using testing::StrictMock;

class MockRawEFMSinkStageDeps : public orc::IRawEFMSinkStageDeps {
 public:
  MOCK_METHOD(void, init,
              (orc::TriggerProgressCallback progress_callback,
               std::atomic<bool>* cancel_requested),
              (override));
  MOCK_METHOD(orc::RawEFMSinkWriteResult, write_raw_efm,
              (const orc::VideoFrameRepresentation* representation,
               const std::string& output_path, bool include_confidence),
              (override));
};

class MockVFRArtifactWithEFM : public MockVideoFrameRepresentationArtifact {
 public:
  MOCK_METHOD(bool, has_efm, (), (const, override));
};

TEST(RawEFMSinkStageTest, Descriptor_DefaultsOutputPathIsEmptyEfm) {
  orc::RawEFMSinkStage stage;
  const auto descriptors = stage.get_parameter_descriptors();

  auto it = std::find_if(descriptors.begin(), descriptors.end(),
                         [](const orc::ParameterDescriptor& d) {
                           return d.name == "output_path";
                         });

  ASSERT_NE(it, descriptors.end());
  EXPECT_EQ(it->type, orc::ParameterType::FILE_PATH);
  EXPECT_EQ(it->file_extension_hint, ".efm");
  if (!it->constraints.default_value.has_value()) {
    FAIL() << "Expected default_value to have a value";
    return;
  }
  EXPECT_EQ(std::get<std::string>(*it->constraints.default_value), "");
}

TEST(RawEFMSinkStageTest, Trigger_FailsWhenNoInputProvided) {
  orc::RawEFMSinkStage stage;
  MockObservationContext observation_context;

  const bool result = stage.trigger({}, {}, observation_context);

  EXPECT_FALSE(result);
  EXPECT_EQ(stage.get_trigger_status(),
            "Error: RawEFM sink requires one input (VideoFrameRepresentation)");
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

TEST(RawEFMSinkStageTest, Trigger_FailsWhenInputHasNoEfm) {
  orc::RawEFMSinkStage stage;
  MockObservationContext observation_context;
  auto vfr = std::make_shared<NiceMock<MockVFRArtifactWithEFM>>();

  EXPECT_CALL(*vfr, has_efm()).WillOnce(Return(false));

  const bool result = stage.trigger(
      {vfr}, {{"output_path", std::string("out.efm")}}, observation_context);

  EXPECT_FALSE(result);
  EXPECT_EQ(stage.get_trigger_status(),
            "Error: Input VFrameR does not have EFM data (no EFM file "
            "specified in source?)");
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

TEST(RawEFMSinkStageTest, Trigger_FailsWhenOutputPathMissing) {
  orc::RawEFMSinkStage stage;
  MockObservationContext observation_context;
  auto vfr = std::make_shared<NiceMock<MockVFRArtifactWithEFM>>();

  EXPECT_CALL(*vfr, has_efm()).WillOnce(Return(true));

  const bool result = stage.trigger({vfr}, {}, observation_context);

  EXPECT_FALSE(result);
  EXPECT_EQ(stage.get_trigger_status(),
            "Error: output_path parameter is required");
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

TEST(RawEFMSinkStageTest, Trigger_UsesDepsSeamAndReportsSuccess) {
  orc::RawEFMSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockRawEFMSinkStageDeps>>();
  stage.set_deps_override(deps);

  MockObservationContext observation_context;
  auto vfr = std::make_shared<NiceMock<MockVFRArtifactWithEFM>>();

  EXPECT_CALL(*vfr, has_efm()).WillOnce(Return(true));
  // Unset include_confidence keeps the confidence nibble: the raw sink's
  // default is the lossless export.
  EXPECT_CALL(*deps, write_raw_efm(vfr.get(), "out.efm", true))
      .WillOnce(Return(orc::RawEFMSinkWriteResult{
          true, 1337, "Success: 1337 t-values written"}));

  const bool result = stage.trigger(
      {vfr}, {{"output_path", std::string("out.efm")}}, observation_context);

  EXPECT_TRUE(result);
  EXPECT_EQ(stage.get_trigger_status(), "Success: 1337 t-values written");
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

TEST(RawEFMSinkStageTest, Trigger_PassesIncludeConfidenceParameterToDeps) {
  orc::RawEFMSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockRawEFMSinkStageDeps>>();
  stage.set_deps_override(deps);

  MockObservationContext observation_context;
  auto vfr = std::make_shared<NiceMock<MockVFRArtifactWithEFM>>();

  EXPECT_CALL(*vfr, has_efm()).WillOnce(Return(true));
  EXPECT_CALL(*deps, write_raw_efm(vfr.get(), "out.efm", false))
      .WillOnce(Return(orc::RawEFMSinkWriteResult{
          true, 1337, "Success: 1337 t-values written"}));

  const bool result = stage.trigger(
      {vfr},
      {{"output_path", std::string("out.efm")}, {"include_confidence", false}},
      observation_context);

  EXPECT_TRUE(result);
}

TEST(RawEFMSinkStageTest, Trigger_UsesDepsSeamAndPropagatesFailure) {
  orc::RawEFMSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockRawEFMSinkStageDeps>>();
  stage.set_deps_override(deps);

  MockObservationContext observation_context;
  auto vfr = std::make_shared<NiceMock<MockVFRArtifactWithEFM>>();

  EXPECT_CALL(*vfr, has_efm()).WillOnce(Return(true));
  EXPECT_CALL(*deps, write_raw_efm(vfr.get(), "out.efm", true))
      .WillOnce(
          Return(orc::RawEFMSinkWriteResult{false, 0, "Cancelled by user"}));

  const bool result = stage.trigger(
      {vfr}, {{"output_path", std::string("out.efm")}}, observation_context);

  EXPECT_FALSE(result);
  EXPECT_EQ(stage.get_trigger_status(), "Cancelled by user");
  EXPECT_FALSE(stage.is_trigger_in_progress());
}
}  // namespace orc_unit_test
