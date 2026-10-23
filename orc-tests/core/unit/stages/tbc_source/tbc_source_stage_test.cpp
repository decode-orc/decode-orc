/*
 * File:        tbc_source_stage_test.cpp
 * Module:      orc-tests/core/unit/stages/tbc_source
 * Purpose:     Unit tests for TBCSourceStage parameter descriptors, execute
 *              contract, and DI-based loading. All I/O is fully mocked through
 *              ITBCSourceStageDeps.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/tbc_source/tbc_source_stage.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/error_types.h>
#include <orc/stage/node_type.h>
#include <orc/stage/observation/observation_context.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../../../orc/plugins/stages/tbc_source/tbc_level_derivation.h"
#include "../source_common/source_stage_descriptor_test_utils.h"

using testing::_;  // NOLINT(bugprone-reserved-identifier)
using testing::NiceMock;
using testing::Return;
using testing::StrictMock;

namespace orc_unit_test {

// ===========================================================================
// Mock for ITBCSourceStageDeps
// ===========================================================================

class MockTBCSourceStageDeps : public orc::ITBCSourceStageDeps {
 public:
  MOCK_METHOD(bool, validate_input_file,
              (const std::string& path, std::string& error_message),
              (const, override));

  MOCK_METHOD(std::optional<orc::TBCVideoParams>, load_video_params,
              (const std::string& db_path, std::string& error_message),
              (const, override));

  MOCK_METHOD(std::vector<orc::TBCFieldMeta>, load_all_field_meta,
              (const std::string& db_path, std::string& error_message),
              (const, override));

  MOCK_METHOD(std::vector<uint16_t>, read_field_samples,
              (const std::string& tbc_path, int32_t field_index,
               int32_t stored_samples_per_field, int32_t use_sample_count,
               std::string& error_message),
              (const, override));

  MOCK_METHOD(std::vector<uint16_t>, read_field_samples_at,
              (const std::string& tbc_path, int32_t field_index,
               int32_t stored_samples_per_field, int32_t sample_offset,
               int32_t use_sample_count, std::string& error_message),
              (const, override));

  MOCK_METHOD(bool, has_audio_file, (const std::string& pcm_path),
              (const, override));

  MOCK_METHOD(std::optional<orc::PcmAudioParameters>, load_pcm_audio_parameters,
              (const std::string& db_path), (const, override));

  MOCK_METHOD(std::optional<uint64_t>, get_audio_pair_count,
              (const std::string& pcm_path), (const, override));

  MOCK_METHOD(std::vector<int16_t>, read_audio_samples_at,
              (const std::string& pcm_path, size_t stereo_pair_offset,
               size_t stereo_pair_count),
              (const, override));

  MOCK_METHOD(bool, has_efm_file, (const std::string& efm_bin_path),
              (const, override));

  MOCK_METHOD(std::vector<uint8_t>, read_efm_bytes_at,
              (const std::string& efm_bin_path, size_t efm_byte_offset,
               size_t efm_byte_count),
              (const, override));

  MOCK_METHOD(bool, has_ac3_file, (const std::string& ac3_bin_path),
              (const, override));

  MOCK_METHOD(std::vector<uint8_t>, read_ac3_bytes_at,
              (const std::string& ac3_bin_path, size_t ac3_byte_offset,
               size_t ac3_byte_count),
              (const, override));
};

// ===========================================================================
// Helper builders
// ===========================================================================

// Build a minimal PAL TBCVideoParams.
static orc::TBCVideoParams make_pal_video_params(int32_t num_fields = 2) {
  orc::TBCVideoParams tvp;
  tvp.system = orc::VideoSystem::PAL;
  tvp.number_of_fields = num_fields;
  tvp.field_width = orc::kPalSamplesPerLineNominal;                // 1135
  tvp.field1_height = orc::kPalField1Lines;                        // 313
  tvp.field2_height = orc::kPalFrameLines - orc::kPalField1Lines;  // 312
  tvp.blanking_16b = 16384;
  tvp.white_16b = 54400;
  return tvp;
}

// Build minimal PAL TBCFieldMeta entries (one frame per pair of fields).
static std::vector<orc::TBCFieldMeta> make_pal_field_meta(
    int32_t num_fields = 2) {
  return std::vector<orc::TBCFieldMeta>(static_cast<size_t>(num_fields));
}

// Return a blanking-level field of the given size.
static std::vector<uint16_t> make_blanking_field(int32_t sample_count) {
  return std::vector<uint16_t>(static_cast<size_t>(sample_count),
                               static_cast<uint16_t>(16384));
}

// Build a minimal NTSC TBCVideoParams (audio on NTSC is resampled; PAL is not).
static orc::TBCVideoParams make_ntsc_video_params(int32_t num_fields = 2) {
  orc::TBCVideoParams tvp;
  tvp.system = orc::VideoSystem::NTSC;
  tvp.number_of_fields = num_fields;
  tvp.field_width = orc::kNtscSamplesPerLine;                        // 910
  tvp.field1_height = orc::kNtscField1Lines;                         // 263
  tvp.field2_height = orc::kNtscFrameLines - orc::kNtscField1Lines;  // 262
  tvp.blanking_16b = 16384;
  tvp.white_16b = 54400;
  return tvp;
}

// Build NTSC TBCFieldMeta entries, optionally carrying a per-field audio pair
// count so the source exposes lockable audio.
static std::vector<orc::TBCFieldMeta> make_ntsc_field_meta(
    int32_t num_fields, int32_t audio_pairs_per_field) {
  std::vector<orc::TBCFieldMeta> meta(static_cast<size_t>(num_fields));
  for (int i = 0; i < num_fields; ++i) {
    if (audio_pairs_per_field > 0) {
      meta[static_cast<size_t>(i)].audio_sample_count = audio_pairs_per_field;
    }
  }
  return meta;
}

// ===========================================================================
// Parameter descriptor tests
// ===========================================================================

TEST(TBCSourceStageTest, ParameterDescriptors_ContainsInputPath) {
  orc::TBCSourceStage stage;
  auto descs = stage.get_parameter_descriptors();
  expect_file_path_descriptor(descs, "input_path", ".tbc");
}

TEST(TBCSourceStageTest, ParameterDescriptors_ContainsYPath) {
  orc::TBCSourceStage stage;
  auto descs = stage.get_parameter_descriptors();
  expect_file_path_descriptor(descs, "y_path", ".tbcy");
}

TEST(TBCSourceStageTest, ParameterDescriptors_ContainsCPath) {
  orc::TBCSourceStage stage;
  auto descs = stage.get_parameter_descriptors();
  expect_file_path_descriptor(descs, "c_path", ".tbcc");
}

TEST(TBCSourceStageTest, ParameterDescriptors_ContainsPcmPath) {
  orc::TBCSourceStage stage;
  auto descs = stage.get_parameter_descriptors();
  expect_file_path_descriptor(descs, "pcm_path", ".pcm");
}

TEST(TBCSourceStageTest, ParameterDescriptors_AllAreOptional) {
  orc::TBCSourceStage stage;
  auto descs = stage.get_parameter_descriptors();
  expect_all_descriptors_optional(descs);
}

// A project is composite or Y/C, so the source offers only the matching file
// parameters; `orc-cli nodes set` relies on this to refuse input_path on a Y/C
// project.
TEST(TBCSourceStageTest, ParameterDescriptors_FollowTheProjectSourceType) {
  orc::TBCSourceStage stage;
  const auto has = [](const std::vector<orc::ParameterDescriptor>& descs,
                      const std::string& name) {
    return std::any_of(
        descs.begin(), descs.end(),
        [&name](const orc::ParameterDescriptor& d) { return d.name == name; });
  };

  const auto composite = stage.get_parameter_descriptors(
      orc::VideoSystem::PAL, orc::SourceType::Composite);
  EXPECT_TRUE(has(composite, "input_path"));
  EXPECT_FALSE(has(composite, "y_path"));
  EXPECT_FALSE(has(composite, "c_path"));
  EXPECT_TRUE(has(composite, "pcm_path"));

  const auto yc = stage.get_parameter_descriptors(orc::VideoSystem::PAL,
                                                  orc::SourceType::YC);
  EXPECT_FALSE(has(yc, "input_path"));
  EXPECT_TRUE(has(yc, "y_path"));
  EXPECT_TRUE(has(yc, "c_path"));
  EXPECT_TRUE(has(yc, "pcm_path"));
}

TEST(TBCSourceStageTest, Descriptor_DefaultsInputPathIsEmptyString) {
  orc::TBCSourceStage stage;
  auto descs = stage.get_parameter_descriptors();
  expect_empty_string_default(descs, "input_path");
}

// ===========================================================================
// NodeTypeInfo / stage identity
// ===========================================================================

TEST(TBCSourceStageTest, NodeTypeInfo_StageNameIsTBCSource) {
  const orc::TBCSourceStage stage;
  EXPECT_EQ(stage.get_node_type_info().stage_name, "tbc_source");
}

TEST(TBCSourceStageTest, NodeTypeInfo_CompatibilityIsAll) {
  const orc::TBCSourceStage stage;
  EXPECT_EQ(stage.get_node_type_info().compatible_formats,
            orc::VideoFormatCompatibility::ALL);
}

TEST(TBCSourceStageTest, NodeTypeInfo_ZeroInputsOneOutput) {
  const orc::TBCSourceStage stage;
  EXPECT_EQ(stage.get_node_type_info().min_inputs, 0u);
  EXPECT_EQ(stage.get_node_type_info().max_inputs, 0u);
  EXPECT_GE(stage.get_node_type_info().max_outputs, 1u);
}

// ===========================================================================
// set_parameters validation
// ===========================================================================

TEST(TBCSourceStageTest, SetParameters_AcceptsValidStringMap) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(*deps, load_video_params(_, _))
      .WillByDefault(Return(make_pal_video_params()));
  orc::TBCSourceStage stage(deps);
  EXPECT_TRUE(
      stage.set_parameters({{"input_path", std::string("/tmp/test.tbc")},
                            {"pcm_path", std::string("")}}));
}

TEST(TBCSourceStageTest, SetParameters_RejectsNonStringValues) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  EXPECT_FALSE(
      stage.set_parameters({{"input_path", static_cast<int32_t>(42)}}));
}

TEST(TBCSourceStageTest, SetParameters_AcceptsEmptyMap) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  EXPECT_TRUE(stage.set_parameters({}));
}

// ===========================================================================
// execute() contract
// ===========================================================================

TEST(TBCSourceStageTest, Execute_ThrowsWhenInputsProvided) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  orc::ObservationContext ctx;

  EXPECT_THROW(stage.execute({nullptr}, {}, ctx), std::runtime_error);
}

TEST(TBCSourceStageTest, Execute_ReturnsEmptyWhenNoInputPathConfigured) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  orc::ObservationContext ctx;

  auto outputs = stage.execute({}, {}, ctx);
  EXPECT_TRUE(outputs.empty());
}

TEST(TBCSourceStageTest, Execute_ReturnsEmptyWhenInputPathIsEmptyString) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  orc::ObservationContext ctx;

  auto outputs = stage.execute({}, {{"input_path", std::string("")}}, ctx);
  EXPECT_TRUE(outputs.empty());
}

TEST(TBCSourceStageTest, Execute_ThrowsUserDataErrorWhenFileNotFound) {
  auto deps = std::make_shared<StrictMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  orc::ObservationContext ctx;

  EXPECT_CALL(*deps, validate_input_file(_, _))
      .WillOnce([](const std::string&, std::string& err) {
        err = "File not found";
        return false;
      });

  EXPECT_THROW(stage.execute(
                   {}, {{"input_path", std::string("/no/such/file.tbc")}}, ctx),
               orc::UserDataError);
}

TEST(TBCSourceStageTest, Execute_ThrowsUserDataErrorWhenMetadataFails) {
  auto deps = std::make_shared<StrictMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  orc::ObservationContext ctx;

  EXPECT_CALL(*deps, validate_input_file(_, _)).WillOnce(Return(true));
  EXPECT_CALL(*deps, load_video_params(_, _))
      .WillOnce([](const std::string&, std::string& err) {
        err = "DB error";
        return std::optional<orc::TBCVideoParams>{};
      });
  EXPECT_CALL(*deps, has_audio_file(_)).WillRepeatedly(Return(false));
  EXPECT_CALL(*deps, has_efm_file(_)).WillRepeatedly(Return(false));
  EXPECT_CALL(*deps, has_ac3_file(_)).WillRepeatedly(Return(false));

  EXPECT_THROW(
      stage.execute({}, {{"input_path", std::string("/tmp/test.tbc")}}, ctx),
      orc::UserDataError);
}

TEST(TBCSourceStageTest,
     Execute_ReturnsSingleOutputWhenPALMetadataLoadSucceeds) {
  // Use NiceMock: this test focuses on the return value, not call counts on
  // sidecar-check methods (which may or may not be called depending on whether
  // paths are set).
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  orc::ObservationContext ctx;

  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(*deps, load_video_params(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return std::optional<orc::TBCVideoParams>{make_pal_video_params(2)};
      });
  ON_CALL(*deps, load_all_field_meta(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return make_pal_field_meta(2);
      });
  ON_CALL(*deps, has_audio_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_efm_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_ac3_file(_)).WillByDefault(Return(false));

  const auto outputs =
      stage.execute({}, {{"input_path", std::string("/tmp/test.tbc")}}, ctx);

  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_NE(outputs.front(), nullptr);
}

TEST(TBCSourceStageTest, Execute_DisplayNameSetToPALTBCCompositeAfterLoad) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  orc::ObservationContext ctx;

  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(*deps, load_video_params(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return std::optional<orc::TBCVideoParams>{make_pal_video_params(2)};
      });
  ON_CALL(*deps, load_all_field_meta(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return make_pal_field_meta(2);
      });
  ON_CALL(*deps, has_audio_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_efm_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_ac3_file(_)).WillByDefault(Return(false));

  stage.execute({}, {{"input_path", std::string("/tmp/test.tbc")}}, ctx);

  EXPECT_EQ(stage.get_node_type_info().display_name, "PAL TBC Composite");
}

// ===========================================================================
// VideoFrameRepresentation output contract
// ===========================================================================

TEST(TBCSourceStageTest, OutputIsVFR_FrameCountIsFieldCountDividedByTwo) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  orc::ObservationContext ctx;

  static constexpr int32_t kNumFields = 4;

  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(*deps, load_video_params(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return std::optional<orc::TBCVideoParams>{
            make_pal_video_params(kNumFields)};
      });
  ON_CALL(*deps, load_all_field_meta(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return make_pal_field_meta(kNumFields);
      });
  ON_CALL(*deps, has_audio_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_efm_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_ac3_file(_)).WillByDefault(Return(false));

  const auto outputs =
      stage.execute({}, {{"input_path", std::string("/tmp/test.tbc")}}, ctx);

  ASSERT_EQ(outputs.size(), 1u);
  const auto* vfr =
      dynamic_cast<orc::VideoFrameRepresentation*>(outputs.front().get());
  ASSERT_NE(vfr, nullptr);
  EXPECT_EQ(vfr->frame_count(), static_cast<size_t>(kNumFields / 2));
}

TEST(TBCSourceStageTest, FrameDescriptor_MarksPaddingOnlyWhenBothFieldsArePad) {
  // ld-decode marks player-skip fields with pad=true. A frame is a padding
  // frame only when both of its TBC fields are padded; a half-real frame
  // still carries measurable signal (issue #77).
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  orc::ObservationContext ctx;

  static constexpr int32_t kNumFields = 6;  // 3 frames

  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(*deps, load_video_params(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return std::optional<orc::TBCVideoParams>{
            make_pal_video_params(kNumFields)};
      });
  ON_CALL(*deps, load_all_field_meta(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        auto meta = make_pal_field_meta(kNumFields);
        meta[2].is_pad = true;  // frame 1: both fields padded
        meta[3].is_pad = true;
        meta[4].is_pad = true;  // frame 2: only the first field padded
        return meta;
      });
  ON_CALL(*deps, has_audio_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_efm_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_ac3_file(_)).WillByDefault(Return(false));

  const auto outputs =
      stage.execute({}, {{"input_path", std::string("/tmp/test.tbc")}}, ctx);

  ASSERT_EQ(outputs.size(), 1u);
  const auto* vfr =
      dynamic_cast<orc::VideoFrameRepresentation*>(outputs.front().get());
  ASSERT_NE(vfr, nullptr);

  const auto desc0 = vfr->get_frame_descriptor(0);
  const auto desc1 = vfr->get_frame_descriptor(1);
  const auto desc2 = vfr->get_frame_descriptor(2);
  ASSERT_TRUE(desc0.has_value());
  ASSERT_TRUE(desc1.has_value());
  ASSERT_TRUE(desc2.has_value());
  EXPECT_FALSE(desc0->is_padding_frame);
  EXPECT_TRUE(desc1->is_padding_frame);
  EXPECT_FALSE(desc2->is_padding_frame);
}

TEST(TBCSourceStageTest, OutputVFR_GetFrameLazilyAssemblesFromMockedDeps) {
  // Verify that get_frame() triggers read_field_samples() via deps for frame 0.
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  orc::ObservationContext ctx;

  constexpr int32_t kF1Samples =
      orc::kPalField1Lines * (orc::kPalSamplesPerLineNominal);  // 313 × 1135
  constexpr int32_t kF2Samples =
      (orc::kPalFrameLines - orc::kPalField1Lines) *
      (orc::kPalSamplesPerLineNominal);  // 312 × 1135

  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(*deps, load_video_params(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return std::optional<orc::TBCVideoParams>{make_pal_video_params(2)};
      });
  ON_CALL(*deps, load_all_field_meta(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return make_pal_field_meta(2);
      });
  ON_CALL(*deps, has_audio_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_efm_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_ac3_file(_)).WillByDefault(Return(false));

  // Expect read_field_samples for TBC fields 0 and 1 (frame 0).
  EXPECT_CALL(*deps, read_field_samples("/tmp/test.tbc", 0, _, kF1Samples, _))
      .WillOnce([kF1Samples](const std::string&, int32_t, int32_t, int32_t,
                             std::string&) {
        return make_blanking_field(kF1Samples);
      });
  EXPECT_CALL(*deps, read_field_samples("/tmp/test.tbc", 1, _, kF2Samples, _))
      .WillOnce([kF2Samples](const std::string&, int32_t, int32_t, int32_t,
                             std::string&) {
        return make_blanking_field(kF2Samples);
      });

  const auto outputs =
      stage.execute({}, {{"input_path", std::string("/tmp/test.tbc")}}, ctx);

  ASSERT_EQ(outputs.size(), 1u);
  auto* vfr =
      dynamic_cast<orc::VideoFrameRepresentation*>(outputs.front().get());
  ASSERT_NE(vfr, nullptr);

  // Trigger lazy loading.
  const auto* frame_ptr = vfr->get_frame(0);
  ASSERT_NE(frame_ptr, nullptr);

  // All samples must be at kPalBlanking (TBC blanking maps to CVBS blanking).
  EXPECT_EQ(frame_ptr[0], static_cast<int16_t>(orc::kPalBlanking));
}

// ===========================================================================
// set_parameters status feedback tests
// ===========================================================================

TEST(TBCSourceStageStatusTest, SetParameters_ShowsRed_WhenPathEmpty) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  EXPECT_TRUE(stage.set_parameters({}));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Red);
}

TEST(TBCSourceStageStatusTest, SetParameters_ShowsRed_WhenFileNotAccessible) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(false));
  orc::TBCSourceStage stage(deps);
  std::map<std::string, orc::ParameterValue> params;
  params["input_path"] = std::string("/path/to/video.tbc");
  EXPECT_TRUE(stage.set_parameters(params));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Red);
}

TEST(TBCSourceStageStatusTest,
     SetParameters_ShowsYellow_WhenMetadataNotAccessible) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(*deps, load_video_params(_, _)).WillByDefault(Return(std::nullopt));
  orc::TBCSourceStage stage(deps);
  std::map<std::string, orc::ParameterValue> params;
  params["input_path"] = std::string("/path/to/video.tbc");
  EXPECT_TRUE(stage.set_parameters(params));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Yellow);
}

TEST(TBCSourceStageStatusTest,
     SetParameters_ShowsGreen_WhenFileAndMetadataAccessible) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(*deps, load_video_params(_, _))
      .WillByDefault(Return(make_pal_video_params()));
  orc::TBCSourceStage stage(deps);
  std::map<std::string, orc::ParameterValue> params;
  params["input_path"] = std::string("/path/to/video.tbc");
  EXPECT_TRUE(stage.set_parameters(params));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Green);
}

TEST(TBCSourceStageStatusTest,
     SetParameters_ShowsGreen_WhenYCModeAndMetadataAccessible) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(*deps, load_video_params(_, _))
      .WillByDefault(Return(make_pal_video_params()));
  orc::TBCSourceStage stage(deps);
  std::map<std::string, orc::ParameterValue> params;
  params["y_path"] = std::string("/path/to/video_y.tbc");
  params["c_path"] = std::string("/path/to/video_c.tbc");
  EXPECT_TRUE(stage.set_parameters(params));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Green);
}

// ===========================================================================
// EFM sidecar loading (issue #210)
// ===========================================================================

// A TBC source with a raw .efm sidecar (no .efm.meta index — that is CVBS-only)
// must expose EFM data.  Per-field T-value counts come from the TBC metadata;
// the .efm file stores one byte per T-value in field order.
TEST(TBCSourceStageTest, OutputVFR_ExposesEFM_WhenEfmFileAndMetadataCountsSet) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  orc::ObservationContext ctx;

  static constexpr int32_t kNumFields = 4;  // two frames
  // Per-field T-value counts: frame 0 = fields 0+1 = 10+5 = 15 bytes @ off 0;
  //                           frame 1 = fields 2+3 = 8+3 = 11 bytes @ off 15.
  const std::array<int32_t, 4> kCounts = {10, 5, 8, 3};

  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(*deps, load_video_params(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return std::optional<orc::TBCVideoParams>{
            make_pal_video_params(kNumFields)};
      });
  ON_CALL(*deps, load_all_field_meta(_, _))
      .WillByDefault([kCounts](const std::string&, std::string&) {
        auto meta = make_pal_field_meta(kNumFields);
        for (size_t i = 0; i < meta.size(); ++i) {
          meta[i].efm_t_value_count = kCounts[i];
        }
        return meta;
      });
  ON_CALL(*deps, has_audio_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_efm_file(_)).WillByDefault(Return(true));
  ON_CALL(*deps, has_ac3_file(_)).WillByDefault(Return(false));

  // Return a synthetic payload that encodes the requested (offset, count) so
  // the test can assert both the byte range and the concatenation.
  ON_CALL(*deps, read_efm_bytes_at(_, _, _))
      .WillByDefault([](const std::string&, size_t offset, size_t count) {
        std::vector<uint8_t> bytes(count);
        for (size_t i = 0; i < count; ++i) {
          bytes[i] = static_cast<uint8_t>((offset + i) & 0xFF);
        }
        return bytes;
      });

  const auto outputs =
      stage.execute({},
                    {{"input_path", std::string("/tmp/test.tbc")},
                     {"efm_path", std::string("/tmp/test.efm")}},
                    ctx);

  ASSERT_EQ(outputs.size(), 1u);
  const auto* vfr =
      dynamic_cast<orc::VideoFrameRepresentation*>(outputs.front().get());
  ASSERT_NE(vfr, nullptr);
  EXPECT_TRUE(vfr->has_efm());

  // Per-frame T-value counts must be reported (EFM Sink sums these to size its
  // buffer; a zero here is what regressed as "no EFM t-values found").
  EXPECT_EQ(vfr->get_efm_sample_count(0), 15u);
  EXPECT_EQ(vfr->get_efm_sample_count(1), 11u);

  // Frame 0: 15 bytes starting at offset 0.
  const auto frame0 = vfr->get_efm_samples(0);
  ASSERT_EQ(frame0.size(), 15u);
  EXPECT_EQ(frame0.front(), 0u);
  EXPECT_EQ(frame0.back(), 14u);

  // Frame 1: 11 bytes starting at offset 15 (sum of frame 0's field counts).
  const auto frame1 = vfr->get_efm_samples(1);
  ASSERT_EQ(frame1.size(), 11u);
  EXPECT_EQ(frame1.front(), 15u);
  EXPECT_EQ(frame1.back(), 25u);
}

// When the .efm file exists but the metadata carries no T-value counts, EFM is
// unavailable — there is no way to index the raw stream without counts.
TEST(TBCSourceStageTest, OutputVFR_NoEFM_WhenEfmFileButMetadataCountsAbsent) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  orc::ObservationContext ctx;

  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(*deps, load_video_params(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return std::optional<orc::TBCVideoParams>{make_pal_video_params(2)};
      });
  ON_CALL(*deps, load_all_field_meta(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return make_pal_field_meta(2);  // no efm_t_value_count set
      });
  ON_CALL(*deps, has_audio_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_efm_file(_)).WillByDefault(Return(true));
  ON_CALL(*deps, has_ac3_file(_)).WillByDefault(Return(false));

  const auto outputs =
      stage.execute({},
                    {{"input_path", std::string("/tmp/test.tbc")},
                     {"efm_path", std::string("/tmp/test.efm")}},
                    ctx);

  ASSERT_EQ(outputs.size(), 1u);
  const auto* vfr =
      dynamic_cast<orc::VideoFrameRepresentation*>(outputs.front().get());
  ASSERT_NE(vfr, nullptr);
  EXPECT_FALSE(vfr->has_efm());
}

// A TBC source with a raw .ac3sym sidecar (no .meta index — that is CVBS-only)
// must expose AC3 RF symbols.  Per-field symbol counts come from the TBC
// metadata (ac3_symbols); the .ac3sym file stores one byte per symbol in field
// order.  Mirrors the EFM tests above.
TEST(TBCSourceStageTest, OutputVFR_ExposesAC3_WhenAc3FileAndMetadataCountsSet) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  orc::ObservationContext ctx;

  static constexpr int32_t kNumFields = 4;  // two frames
  // Per-field symbol counts: frame 0 = fields 0+1 = 10+5 = 15 bytes @ off 0;
  //                          frame 1 = fields 2+3 = 8+3 = 11 bytes @ off 15.
  const std::array<int32_t, 4> kCounts = {10, 5, 8, 3};

  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(*deps, load_video_params(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return std::optional<orc::TBCVideoParams>{
            make_pal_video_params(kNumFields)};
      });
  ON_CALL(*deps, load_all_field_meta(_, _))
      .WillByDefault([kCounts](const std::string&, std::string&) {
        auto meta = make_pal_field_meta(kNumFields);
        for (size_t i = 0; i < meta.size(); ++i) {
          meta[i].ac3rf_symbol_count = kCounts[i];
        }
        return meta;
      });
  ON_CALL(*deps, has_audio_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_efm_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_ac3_file(_)).WillByDefault(Return(true));

  // Return a synthetic payload that encodes the requested (offset, count) so
  // the test can assert both the byte range and the concatenation.
  ON_CALL(*deps, read_ac3_bytes_at(_, _, _))
      .WillByDefault([](const std::string&, size_t offset, size_t count) {
        std::vector<uint8_t> bytes(count);
        for (size_t i = 0; i < count; ++i) {
          bytes[i] = static_cast<uint8_t>((offset + i) & 0xFF);
        }
        return bytes;
      });

  const auto outputs =
      stage.execute({},
                    {{"input_path", std::string("/tmp/test.tbc")},
                     {"ac3rf_path", std::string("/tmp/test.ac3sym")}},
                    ctx);

  ASSERT_EQ(outputs.size(), 1u);
  const auto* vfr =
      dynamic_cast<orc::VideoFrameRepresentation*>(outputs.front().get());
  ASSERT_NE(vfr, nullptr);
  EXPECT_TRUE(vfr->has_ac3_rf());

  // Per-frame symbol counts must be reported.
  EXPECT_EQ(vfr->get_ac3_symbol_count(0), 15u);
  EXPECT_EQ(vfr->get_ac3_symbol_count(1), 11u);

  // Frame 0: 15 bytes starting at offset 0.
  const auto frame0 = vfr->get_ac3_symbols(0);
  ASSERT_EQ(frame0.size(), 15u);
  EXPECT_EQ(frame0.front(), 0u);
  EXPECT_EQ(frame0.back(), 14u);

  // Frame 1: 11 bytes starting at offset 15 (sum of frame 0's field counts).
  const auto frame1 = vfr->get_ac3_symbols(1);
  ASSERT_EQ(frame1.size(), 11u);
  EXPECT_EQ(frame1.front(), 15u);
  EXPECT_EQ(frame1.back(), 25u);
}

// When the .ac3sym file exists but the metadata carries no symbol counts, AC3
// is unavailable — there is no way to index the raw stream without counts.
TEST(TBCSourceStageTest, OutputVFR_NoAC3_WhenAc3FileButMetadataCountsAbsent) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  orc::ObservationContext ctx;

  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(*deps, load_video_params(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return std::optional<orc::TBCVideoParams>{make_pal_video_params(2)};
      });
  ON_CALL(*deps, load_all_field_meta(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return make_pal_field_meta(2);  // no ac3rf_symbol_count set
      });
  ON_CALL(*deps, has_audio_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_efm_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_ac3_file(_)).WillByDefault(Return(true));

  const auto outputs =
      stage.execute({},
                    {{"input_path", std::string("/tmp/test.tbc")},
                     {"ac3rf_path", std::string("/tmp/test.ac3sym")}},
                    ctx);

  ASSERT_EQ(outputs.size(), 1u);
  const auto* vfr =
      dynamic_cast<orc::VideoFrameRepresentation*>(outputs.front().get());
  ASSERT_NE(vfr, nullptr);
  EXPECT_FALSE(vfr->has_ac3_rf());
}

// ===========================================================================
// Audio ingest conversion (SMPTE 272M channel-pair model)
// ===========================================================================
// The .pcm sidecar (raw signed-LE 16-bit stereo, nominally 44100 Hz) is
// always converted on ingest — for all systems including PAL — to the only
// pipeline audio form: 48000 Hz synchronous 24-bit-in-int32 stereo channel
// pairs (SMPTE 272M-1994 §1.2/§1.3), cadence-segmented per §14.3.

namespace {

// Common source-with-PCM setup; returns the VFR (outputs kept alive by the
// caller).  |raw_pairs| is the sidecar length reported by
// get_audio_pair_count; |pcm_meta| is the metadata pcm_audio_parameters row.
orc::VideoFrameRepresentation* execute_audio_source(
    orc::TBCSourceStage& stage,
    const std::shared_ptr<NiceMock<MockTBCSourceStageDeps>>& deps,
    std::vector<orc::ArtifactPtr>& outputs_keepalive,
    orc::ObservationContext& ctx, const orc::TBCVideoParams& video_params,
    uint64_t raw_pairs,
    const std::optional<orc::PcmAudioParameters>& pcm_meta = std::nullopt) {
  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(*deps, load_video_params(_, _))
      .WillByDefault([video_params](const std::string&, std::string&) {
        return std::optional<orc::TBCVideoParams>{video_params};
      });
  ON_CALL(*deps, load_all_field_meta(_, _))
      .WillByDefault([video_params](const std::string&, std::string&) {
        return video_params.system == orc::VideoSystem::PAL
                   ? make_pal_field_meta(video_params.number_of_fields)
                   : make_ntsc_field_meta(video_params.number_of_fields, 0);
      });
  ON_CALL(*deps, has_audio_file(_)).WillByDefault(Return(true));
  ON_CALL(*deps, load_pcm_audio_parameters(_)).WillByDefault(Return(pcm_meta));
  ON_CALL(*deps, get_audio_pair_count(_))
      .WillByDefault(Return(std::optional<uint64_t>{raw_pairs}));
  ON_CALL(*deps, has_efm_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_ac3_file(_)).WillByDefault(Return(false));

  outputs_keepalive =
      stage.execute({},
                    {{"input_path", std::string("/tmp/test.tbc")},
                     {"pcm_path", std::string("/tmp/test.pcm")}},
                    ctx);
  if (outputs_keepalive.size() != 1) return nullptr;
  return dynamic_cast<orc::VideoFrameRepresentation*>(
      outputs_keepalive.front().get());
}

// Build a supported PcmAudioParameters row (signed-LE 16-bit) at |rate|.
orc::PcmAudioParameters make_pcm_meta(double rate = 44100.0, int32_t bits = 16,
                                      bool is_signed = true,
                                      bool is_little_endian = true) {
  orc::PcmAudioParameters meta;
  meta.sample_rate = rate;
  meta.bits = bits;
  meta.is_signed = is_signed;
  meta.is_little_endian = is_little_endian;
  return meta;
}

}  // namespace

// A .pcm sidecar becomes exactly one channel pair: {"Analogue", ANALOGUE}.
TEST(TBCSourceAudioTest, Descriptor_SinglePairNamedAnalogue) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  std::vector<orc::ArtifactPtr> outputs;
  orc::ObservationContext ctx;

  auto* vfr = execute_audio_source(stage, deps, outputs, ctx,
                                   make_pal_video_params(2), 1764);
  ASSERT_NE(vfr, nullptr);

  EXPECT_EQ(vfr->audio_channel_pair_count(), 1u);
  const auto desc = vfr->get_audio_channel_pair_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->name, "Analogue");
  EXPECT_EQ(desc->origin, orc::AudioOrigin::ANALOGUE);
  // Out-of-range pair.
  EXPECT_FALSE(vfr->get_audio_channel_pair_descriptor(1).has_value());
}

// Without a .pcm sidecar the representation carries no audio.
TEST(TBCSourceAudioTest, NoPcmSidecar_ReportsZeroChannelPairs) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  orc::ObservationContext ctx;

  ON_CALL(*deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(*deps, load_video_params(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return std::optional<orc::TBCVideoParams>{make_pal_video_params(2)};
      });
  ON_CALL(*deps, load_all_field_meta(_, _))
      .WillByDefault([](const std::string&, std::string&) {
        return make_pal_field_meta(2);
      });
  ON_CALL(*deps, has_audio_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_efm_file(_)).WillByDefault(Return(false));
  ON_CALL(*deps, has_ac3_file(_)).WillByDefault(Return(false));

  const auto outputs =
      stage.execute({}, {{"input_path", std::string("/tmp/test.tbc")}}, ctx);
  ASSERT_EQ(outputs.size(), 1u);
  const auto* vfr =
      dynamic_cast<orc::VideoFrameRepresentation*>(outputs.front().get());
  ASSERT_NE(vfr, nullptr);
  EXPECT_EQ(vfr->audio_channel_pair_count(), 0u);
  EXPECT_FALSE(vfr->get_audio_channel_pair_descriptor(0).has_value());
  EXPECT_TRUE(vfr->get_audio_samples(0, 0).empty());
}

// PAL ingest: the 44100 Hz sidecar is resampled to 48 kHz — every frame
// carries exactly 1920 stereo pairs (SMPTE 272M-1994 §3.7: 48000 / 25).
TEST(TBCSourceAudioTest, PalIngest_ResamplesTo1920PairFrames) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  std::vector<orc::ArtifactPtr> outputs;
  orc::ObservationContext ctx;

  constexpr int32_t kNumFields = 4;         // two frames
  constexpr uint64_t kRawPairs = 2 * 1764;  // 44100 Hz over two PAL frames

  ON_CALL(*deps, read_audio_samples_at(_, _, _))
      .WillByDefault([](const std::string&, size_t, size_t count) {
        return std::vector<int16_t>(count * 2, 0);
      });

  auto* vfr = execute_audio_source(
      stage, deps, outputs, ctx, make_pal_video_params(kNumFields), kRawPairs);
  ASSERT_NE(vfr, nullptr);

  for (orc::FrameID id = 0; id < 2; ++id) {
    const auto samples = vfr->get_audio_samples(0, id);
    ASSERT_EQ(samples.size(), static_cast<size_t>(orc::audio_pairs_in_frame(
                                  id, orc::VideoSystem::PAL)) *
                                  2);
    EXPECT_EQ(samples.size(), 1920u * 2);
    // Silence in, silence out (SoXR is linear).
    EXPECT_TRUE(std::all_of(samples.begin(), samples.end(),
                            [](int32_t s) { return s == 0; }));
  }
}

// NTSC ingest: the 5-frame audio frame sequence totals 8008 pairs
// (SMPTE 272M-1994 §14.3 Table 1: 1602/1601/1602/1601/1602).
TEST(TBCSourceAudioTest, NtscIngest_FollowsAudioFrameSequenceCadence) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  std::vector<orc::ArtifactPtr> outputs;
  orc::ObservationContext ctx;

  constexpr int32_t kNumFields = 10;        // five frames
  constexpr uint64_t kRawPairs = 5 * 1470;  // 44100 Hz over five NTSC frames

  ON_CALL(*deps, read_audio_samples_at(_, _, _))
      .WillByDefault([](const std::string&, size_t, size_t count) {
        return std::vector<int16_t>(count * 2, 0);
      });

  auto* vfr = execute_audio_source(
      stage, deps, outputs, ctx, make_ntsc_video_params(kNumFields), kRawPairs);
  ASSERT_NE(vfr, nullptr);

  const std::array<uint32_t, 5> kExpectedPairs = {1602, 1601, 1602, 1601, 1602};
  for (orc::FrameID id = 0; id < 5; ++id) {
    EXPECT_EQ(kExpectedPairs[static_cast<size_t>(id)],
              orc::audio_pairs_in_frame(id, orc::VideoSystem::NTSC));
    const auto samples = vfr->get_audio_samples(0, id);
    EXPECT_EQ(samples.size(),
              static_cast<size_t>(kExpectedPairs[static_cast<size_t>(id)]) * 2);
  }
}

// When the metadata declares a 48000 Hz sidecar the rate conversion is
// skipped and the ingest is an exact 16→24-bit widening (<< 8) — which also
// proves the metadata sample rate is honoured instead of the 44100 Hz
// default (a 44.1→48 resample would not reproduce the input exactly).
TEST(TBCSourceAudioTest, MetadataSampleRateHonoured_48kInputWidenedExactly) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  std::vector<orc::ArtifactPtr> outputs;
  orc::ObservationContext ctx;

  constexpr int32_t kNumFields = 2;     // one frame
  constexpr uint64_t kRawPairs = 1920;  // exactly one PAL frame at 48 kHz

  std::vector<int16_t> raw(kRawPairs * 2);
  for (size_t i = 0; i < raw.size(); ++i) {
    raw[i] = static_cast<int16_t>(static_cast<int32_t>(i % 3000) - 1500);
  }
  ON_CALL(*deps, read_audio_samples_at(_, _, _)).WillByDefault(Return(raw));

  auto* vfr = execute_audio_source(stage, deps, outputs, ctx,
                                   make_pal_video_params(kNumFields), kRawPairs,
                                   make_pcm_meta(48000.0));
  ASSERT_NE(vfr, nullptr);

  const auto samples = vfr->get_audio_samples(0, 0);
  ASSERT_EQ(samples.size(), raw.size());
  for (size_t i = 0; i < samples.size(); ++i) {
    ASSERT_EQ(samples[i], static_cast<int32_t>(raw[i]) << 8);
  }
}

// Unsupported .pcm layouts declared in the metadata (unsigned, big-endian,
// bits != 16) disable audio and raise an error observation.
TEST(TBCSourceAudioTest, UnsupportedPcmLayout_ErrorObservationAndNoAudio) {
  const std::array<orc::PcmAudioParameters, 3> kUnsupported = {
      make_pcm_meta(44100.0, 16, /*is_signed=*/false),
      make_pcm_meta(44100.0, 16, true, /*is_little_endian=*/false),
      make_pcm_meta(44100.0, /*bits=*/24),
  };

  for (const auto& meta : kUnsupported) {
    // Fresh stage per case: the representation cache is keyed by TBC path.
    auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
    orc::TBCSourceStage stage(deps);
    std::vector<orc::ArtifactPtr> outputs;
    orc::ObservationContext ctx;

    auto* vfr = execute_audio_source(stage, deps, outputs, ctx,
                                     make_pal_video_params(2), 1764, meta);
    ASSERT_NE(vfr, nullptr);

    EXPECT_EQ(vfr->audio_channel_pair_count(), 0u);
    EXPECT_FALSE(vfr->get_audio_channel_pair_descriptor(0).has_value());
    EXPECT_TRUE(vfr->get_audio_samples(0, 0).empty());

    const auto obs =
        ctx.get(orc::FieldID(0), "tbc_source", "pcm_audio_unsupported");
    ASSERT_TRUE(obs.has_value());
    ASSERT_TRUE(std::holds_alternative<std::string>(*obs));
    EXPECT_FALSE(std::get<std::string>(*obs).empty());
  }
}

// A supported metadata layout at the default rate produces no observation.
TEST(TBCSourceAudioTest, SupportedPcmLayout_NoErrorObservation) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  std::vector<orc::ArtifactPtr> outputs;
  orc::ObservationContext ctx;

  auto* vfr =
      execute_audio_source(stage, deps, outputs, ctx, make_pal_video_params(2),
                           1764, make_pcm_meta());
  ASSERT_NE(vfr, nullptr);

  EXPECT_EQ(vfr->audio_channel_pair_count(), 1u);
  EXPECT_FALSE(ctx.get(orc::FieldID(0), "tbc_source", "pcm_audio_unsupported")
                   .has_value());
}

// ===========================================================================
// Audio ingest conversion is deferred (issue #209)
// ===========================================================================

// Building the source representation is the video-preview /
// getAvailableOutputs hot path and never needs audio.  The ingest conversion
// reads the whole PCM and runs a full SoXR pass, so doing it eagerly during
// execute() stalled the render worker for minutes on long sources ("stuck on
// rendering").  It must be deferred to the first audio access and run at most
// once.
TEST(TBCSourceAudioTest, IngestConversionDeferredUntilFirstAudioAccess) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);
  std::vector<orc::ArtifactPtr> outputs;
  orc::ObservationContext ctx;

  constexpr int32_t kNumFields = 4;         // two frames
  constexpr uint64_t kRawPairs = 2 * 1470;  // 44100 Hz over two NTSC frames

  // Building the representation must not read the PCM.
  EXPECT_CALL(*deps, read_audio_samples_at(_, _, _)).Times(0);

  auto* vfr = execute_audio_source(
      stage, deps, outputs, ctx, make_ntsc_video_params(kNumFields), kRawPairs);
  ASSERT_NE(vfr, nullptr);

  // The descriptor is a cheap metadata query — it must not trigger the
  // conversion either.
  const auto desc = vfr->get_audio_channel_pair_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->origin, orc::AudioOrigin::ANALOGUE);

  // The eager read would have fired by now; confirm it did not, then arm the
  // expectation for the deferred, one-shot read.
  ::testing::Mock::VerifyAndClearExpectations(deps.get());

  // First audio access reads the entire PCM exactly once and converts it.
  EXPECT_CALL(*deps, read_audio_samples_at("/tmp/test.pcm", 0u,
                                           static_cast<size_t>(kRawPairs)))
      .Times(1)
      .WillOnce([](const std::string&, size_t, size_t count) {
        return std::vector<int16_t>(count * 2, 0);
      });

  const auto samples0 = vfr->get_audio_samples(0, 0);
  EXPECT_EQ(samples0.size(), static_cast<size_t>(orc::audio_pairs_in_frame(
                                 0, orc::VideoSystem::NTSC)) *
                                 2);

  // A second access reuses the cached conversion — no further PCM reads (the
  // Times(1) expectation above would fail otherwise).
  const auto samples1 = vfr->get_audio_samples(0, 1);
  EXPECT_EQ(samples1.size(), static_cast<size_t>(orc::audio_pairs_in_frame(
                                 1, orc::VideoSystem::NTSC)) *
                                 2);
}

// The pcm_audio_timing / lock_audio parameters are removed: there is only
// one pipeline audio form (48 kHz synchronous 24-bit channel pairs).
TEST(TBCSourceAudioTest, Descriptors_TimingParametersRemoved) {
  orc::TBCSourceStage stage;
  const auto descs = stage.get_parameter_descriptors();
  const auto has_descriptor = [&](const std::string& name) {
    return std::any_of(
        descs.begin(), descs.end(),
        [&](const orc::ParameterDescriptor& d) { return d.name == name; });
  };
  EXPECT_FALSE(has_descriptor("pcm_audio_timing"));
  EXPECT_FALSE(has_descriptor("lock_audio"));
}

// ===========================================================================
// ld-decode 16-bit level derivation and NTSC-J detection
// ===========================================================================

TEST(TbcLevelDerivationTest, NtscStandardSetup_DerivesNominalBlanking) {
  const auto levels = orc::derive_tbc_domain_levels(
      orc::VideoSystem::NTSC, orc::kTbcNtscBlack, orc::kTbcNtscWhite);
  EXPECT_EQ(levels.blanking_16b, orc::kTbcNtscBlanking);
  ASSERT_TRUE(levels.black_16b.has_value());
  EXPECT_EQ(*levels.black_16b, orc::kTbcNtscBlack);
  EXPECT_FALSE(orc::is_ntsc_j_black_level(levels));
}

TEST(TbcLevelDerivationTest, NtscJBlackAtBlanking_DetectedAsNtscJ) {
  // NTSC-J stores picture black at the 0 IRE blanking level (no setup).
  const auto levels = orc::derive_tbc_domain_levels(
      orc::VideoSystem::NTSC, orc::kTbcNtscBlanking, orc::kTbcNtscWhite);
  EXPECT_EQ(levels.blanking_16b, orc::kTbcNtscBlanking);
  EXPECT_TRUE(orc::is_ntsc_j_black_level(levels));
}

TEST(TbcLevelDerivationTest, NtscNonNominalLevels_StillClassifiedStandard) {
  // A capture with slightly shifted levels must keep the standard-setup
  // interpretation, not flip to NTSC-J.
  const auto levels = orc::derive_tbc_domain_levels(orc::VideoSystem::NTSC,
                                                    orc::kTbcNtscBlack + 300,
                                                    orc::kTbcNtscWhite + 300);
  EXPECT_FALSE(orc::is_ntsc_j_black_level(levels));
}

TEST(TbcLevelDerivationTest, Pal_BlackIsBlanking) {
  const auto levels = orc::derive_tbc_domain_levels(
      orc::VideoSystem::PAL, orc::kTbcPalBlanking, orc::kTbcPalWhite);
  EXPECT_EQ(levels.blanking_16b, orc::kTbcPalBlanking);
}

TEST(TbcLevelDerivationTest, PalM_AlwaysSubtractsSetup) {
  // PAL_M has no NTSC-J variant; the 7.5 IRE setup is always subtracted.
  const auto levels = orc::derive_tbc_domain_levels(
      orc::VideoSystem::PAL_M, orc::kTbcNtscBlack, orc::kTbcNtscWhite);
  EXPECT_EQ(levels.blanking_16b, orc::kTbcNtscBlanking);
}

// ===========================================================================
// NTSC-J black-level override and is_mapped propagation through the VFR
// ===========================================================================

namespace {

orc::VideoFrameRepresentation* execute_and_get_vfr_raw(
    orc::TBCSourceStage& stage, std::vector<orc::ArtifactPtr>& outputs) {
  orc::ObservationContext ctx;
  outputs =
      stage.execute({}, {{"input_path", std::string("/tmp/test.tbc")}}, ctx);
  if (outputs.size() != 1u) return nullptr;
  return dynamic_cast<orc::VideoFrameRepresentation*>(outputs.front().get());
}

void set_default_source_expectations(
    NiceMock<MockTBCSourceStageDeps>& deps, const orc::TBCVideoParams& tvp,
    const std::vector<orc::TBCFieldMeta>& field_meta) {
  ON_CALL(deps, validate_input_file(_, _)).WillByDefault(Return(true));
  ON_CALL(deps, load_video_params(_, _))
      .WillByDefault([tvp](const std::string&, std::string&) {
        return std::optional<orc::TBCVideoParams>{tvp};
      });
  ON_CALL(deps, load_all_field_meta(_, _))
      .WillByDefault([field_meta](const std::string&, std::string&) {
        return field_meta;
      });
  ON_CALL(deps, has_audio_file(_)).WillByDefault(Return(false));
  ON_CALL(deps, has_efm_file(_)).WillByDefault(Return(false));
  ON_CALL(deps, has_ac3_file(_)).WillByDefault(Return(false));
}

}  // namespace

TEST(TBCSourceStageTest, NtscJBlackLevel_PropagatesToParamsAndDescriptor) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);

  auto tvp = make_ntsc_video_params(2);
  // NTSC-J: picture black at the 0 IRE blanking level.
  tvp.ntsc_j_black_level_16b = tvp.blanking_16b;
  set_default_source_expectations(*deps, tvp, make_ntsc_field_meta(2, 0));

  std::vector<orc::ArtifactPtr> outputs;
  auto* vfr = execute_and_get_vfr_raw(stage, outputs);
  ASSERT_NE(vfr, nullptr);

  const auto p = vfr->get_video_parameters();
  ASSERT_TRUE(p.has_value());
  EXPECT_TRUE(p->has_nonstandard_values);
  EXPECT_EQ(p->black_level, orc::kNtscBlanking);  // 0 IRE black

  const auto desc = vfr->get_frame_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  ASSERT_TRUE(desc->black_level_override.has_value());
  EXPECT_EQ(*desc->black_level_override, orc::kNtscBlanking);
}

TEST(TBCSourceStageTest, StandardNtsc_NoBlackLevelOverride) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);

  set_default_source_expectations(*deps, make_ntsc_video_params(2),
                                  make_ntsc_field_meta(2, 0));

  std::vector<orc::ArtifactPtr> outputs;
  auto* vfr = execute_and_get_vfr_raw(stage, outputs);
  ASSERT_NE(vfr, nullptr);

  const auto p = vfr->get_video_parameters();
  ASSERT_TRUE(p.has_value());
  EXPECT_FALSE(p->has_nonstandard_values);
  EXPECT_EQ(p->black_level, orc::kNtscBlack);

  const auto desc = vfr->get_frame_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  EXPECT_FALSE(desc->black_level_override.has_value());
}

TEST(TBCSourceStageTest, IsMapped_PropagatesFromMetadata) {
  auto deps = std::make_shared<NiceMock<MockTBCSourceStageDeps>>();
  orc::TBCSourceStage stage(deps);

  auto tvp = make_pal_video_params(2);
  tvp.is_mapped = true;
  set_default_source_expectations(*deps, tvp, make_pal_field_meta(2));

  std::vector<orc::ArtifactPtr> outputs;
  auto* vfr = execute_and_get_vfr_raw(stage, outputs);
  ASSERT_NE(vfr, nullptr);

  const auto p = vfr->get_video_parameters();
  ASSERT_TRUE(p.has_value());
  EXPECT_TRUE(p->is_mapped);
}

}  // namespace orc_unit_test
