/*
 * File:        cvbs_source_stage_test.cpp
 * Module:      orc-tests/core/unit/stages/cvbs_source
 * Purpose:     Unit tests for PAL, NTSC, and PAL_M CVBS source stages
 *
 * Tests: stage identity, signal state validation, encoding normalisation,
 * SourceParameters from spec constants, sidecar loading (dropout / audio /
 * EFM / AC3), and NTSC-J black level.
 * All I/O is fully mocked through ICVBSSourceStageDeps.
 *
 * Colour burst phase measurement is not performed by the source stage;
 * see colour_frame_phase_observer_test.cpp.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/plugins/stages/cvbs_source/cvbs_source_stage.h"

#include <gtest/gtest.h>
#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/dropout/dropout_run.h>
#include <orc/stage/error_types.h>
#include <orc/stage/node_type.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/params/parameter_types.h>
#include <orc/stage/video_frame_representation.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace orc {
namespace tests {

namespace {

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a payload where every sample = blanking (normalised output = blanking).
// Encoded as CVBS_U16_4FSC (blanking × 64).
std::vector<uint16_t> make_blank_payload_u16(size_t word_count,
                                             int32_t blanking_10bit) {
  return std::vector<uint16_t>(word_count,
                               static_cast<uint16_t>(blanking_10bit * 64));
}

// ---------------------------------------------------------------------------
// FakeCVBSSourceStageDeps — fully in-memory fake, no I/O
// ---------------------------------------------------------------------------

class FakeCVBSSourceStageDeps final : public ICVBSSourceStageDeps {
 public:
  // --- validation ---
  bool input_file_valid = true;
  std::string input_file_error = "Input file error";

  // --- metadata ---
  bool metadata_available = true;
  std::string metadata_error = "Metadata read error";
  CVBSMetadataRecord metadata_record{};
  mutable std::string last_metadata_path;

  // --- payload words ---
  std::vector<uint16_t> payload_words;

  // --- dropout sidecar ---
  std::vector<DropoutRun> dropout_runs;

  // --- audio sidecars: full WAV path → payload samples (flat
  // 24-bit-in-int32 buffer, header already stripped). Presence in the map =
  // file exists.
  std::map<std::string, std::vector<int32_t>> audio_files;

  // --- per-file WAV header overrides; files without an entry report a
  // spec-conformant header (PCM, 2 channels, 48000 Hz, 24-bit) with
  // data_bytes derived from the payload.
  std::map<std::string, CVBSAudioWavInfo> audio_wav_info_overrides;

  // --- .meta audio_channel_pair table; nullopt = table absent
  std::optional<std::vector<CVBSAudioChannelPairRecord>> audio_pair_table;

  // --- EFM sidecar ---
  std::optional<std::vector<CVBSExtensionFrameRef>> efm_table;
  std::vector<uint8_t> efm_data;

  // --- AC3 sidecar ---
  std::optional<std::vector<CVBSExtensionFrameRef>> ac3_table;
  std::vector<uint8_t> ac3_data;

  explicit FakeCVBSSourceStageDeps(
      const std::string& preset = "PAL",
      const std::string& encoding = "CVBS_U16_4FSC") {
    metadata_record.preset = preset;
    metadata_record.sample_encoding_preset = encoding;
    metadata_record.signal_state_preset = "STANDARD_TBC_LOCKED";
    metadata_record.signal_type = "composite";
    metadata_record.number_of_sequential_frames = 1;

    // Default payload: one PAL frame at blanking in CVBS_U16_4FSC.
    payload_words = make_blank_payload_u16(
        static_cast<size_t>(kPalFrameSamples), kPalBlanking);
  }

  bool validate_input_file(const std::string& /*path*/,
                           std::string& error_message) const override {
    if (!input_file_valid) {
      error_message = input_file_error;
      return false;
    }
    return true;
  }

  std::optional<CVBSMetadataRecord> load_metadata(
      const std::string& meta_path, std::string& error_message) const override {
    last_metadata_path = meta_path;
    if (!metadata_available) {
      error_message = metadata_error;
      return std::nullopt;
    }
    return metadata_record;
  }

  std::optional<size_t> get_input_word_count(
      const std::string& /*path*/, std::string& error_message) const override {
    if (payload_words.empty()) {
      error_message = "Empty payload";
      return std::nullopt;
    }
    return payload_words.size();
  }

  bool read_input_words_at(const std::string& /*path*/, size_t word_offset,
                           size_t word_count, std::vector<uint16_t>& out_words,
                           std::string& error_message) const override {
    if (word_offset + word_count > payload_words.size()) {
      error_message = "Read out of bounds";
      return false;
    }
    out_words.assign(
        payload_words.begin() + static_cast<std::ptrdiff_t>(word_offset),
        payload_words.begin() +
            static_cast<std::ptrdiff_t>(word_offset + word_count));
    return true;
  }

  std::vector<DropoutRun> load_dropout_sidecar(
      const std::string& /*path*/,
      std::string& /*error_message*/) const override {
    return dropout_runs;
  }

  std::optional<CVBSAudioWavInfo> read_audio_wav_info(
      const std::string& path) const override {
    const auto override_it = audio_wav_info_overrides.find(path);
    if (override_it != audio_wav_info_overrides.end()) {
      return override_it->second;
    }
    const auto it = audio_files.find(path);
    if (it == audio_files.end()) return std::nullopt;
    CVBSAudioWavInfo info;
    info.format_tag = 1;
    info.channels = 2;
    info.sample_rate_hz = 48000;
    info.bits_per_sample = 24;
    info.data_bytes = static_cast<uint64_t>(it->second.size()) * 3;
    return info;
  }

  std::optional<std::vector<CVBSAudioChannelPairRecord>>
  load_audio_channel_pair_table(const std::string& /*path*/,
                                std::string& /*error_message*/) const override {
    return audio_pair_table;
  }

  // Read-call bookkeeping: per-frame serving must seek by cadence offset.
  mutable int audio_read_calls = 0;
  mutable uint64_t last_audio_read_offset = 0;
  mutable size_t last_audio_read_count = 0;

  std::vector<int32_t> read_audio_pairs_at(
      const std::string& path, uint64_t stereo_pair_offset,
      size_t stereo_pair_count) const override {
    ++audio_read_calls;
    last_audio_read_offset = stereo_pair_offset;
    last_audio_read_count = stereo_pair_count;
    const auto it = audio_files.find(path);
    if (it == audio_files.end()) return {};
    const std::vector<int32_t>& samples = it->second;
    const size_t start = static_cast<size_t>(stereo_pair_offset) * 2;
    const size_t end = std::min(start + stereo_pair_count * 2, samples.size());
    if (start >= samples.size()) return {};
    return std::vector<int32_t>(
        samples.begin() + static_cast<std::ptrdiff_t>(start),
        samples.begin() + static_cast<std::ptrdiff_t>(end));
  }

  std::optional<std::vector<CVBSExtensionFrameRef>> load_efm_frame_table(
      const std::string& /*path*/,
      std::string& /*error_message*/) const override {
    return efm_table;
  }

  std::vector<uint8_t> read_efm_bytes_at(const std::string& /*path*/,
                                         uint64_t byte_offset,
                                         uint32_t count) const override {
    const size_t start = static_cast<size_t>(byte_offset);
    const size_t end = std::min(start + count, efm_data.size());
    if (start >= efm_data.size()) return {};
    return std::vector<uint8_t>(
        efm_data.begin() + static_cast<std::ptrdiff_t>(start),
        efm_data.begin() + static_cast<std::ptrdiff_t>(end));
  }

  std::optional<std::vector<CVBSExtensionFrameRef>> load_ac3_frame_table(
      const std::string& /*path*/,
      std::string& /*error_message*/) const override {
    return ac3_table;
  }

  std::vector<uint8_t> read_ac3_bytes_at(const std::string& /*path*/,
                                         uint64_t byte_offset,
                                         uint32_t count) const override {
    const size_t start = static_cast<size_t>(byte_offset);
    const size_t end = std::min(start + count, ac3_data.size());
    if (start >= ac3_data.size()) return {};
    return std::vector<uint8_t>(
        ac3_data.begin() + static_cast<std::ptrdiff_t>(start),
        ac3_data.begin() + static_cast<std::ptrdiff_t>(end));
  }
};

// ---------------------------------------------------------------------------
// Test helpers
// ---------------------------------------------------------------------------

VideoFrameRepresentationPtr execute_and_get_vfr(
    DAGStage& stage, const std::map<std::string, ParameterValue>& params) {
  std::vector<ArtifactPtr> inputs;
  ObservationContext obs;
  auto results = stage.execute(inputs, params, obs);
  EXPECT_EQ(results.size(), 1u);
  if (results.empty()) return nullptr;
  return std::dynamic_pointer_cast<VideoFrameRepresentation>(results.front());
}

void expect_user_data_error(DAGStage& stage,
                            const std::map<std::string, ParameterValue>& params,
                            const std::string& msg_fragment) {
  std::vector<ArtifactPtr> inputs;
  ObservationContext obs;
  try {
    stage.execute(inputs, params, obs);
    ADD_FAILURE() << "Expected UserDataError containing: " << msg_fragment;
  } catch (const UserDataError& e) {
    EXPECT_NE(std::string(e.what()).find(msg_fragment), std::string::npos)
        << "Actual error: " << e.what();
  }
}

const std::map<std::string, ParameterValue> kDefaultParams{
    {"input_path", std::string("/fake/video.cvbs")}};

}  // namespace

// ===========================================================================
// Stage identity
// ===========================================================================

TEST(CVBSSourceStageIdentityTest, PAL_HasCorrectStageName) {
  PALCVBSSourceStage stage;
  EXPECT_EQ(stage.get_node_type_info().stage_name, "PAL_CVBS_Source");
}

TEST(CVBSSourceStageIdentityTest, NTSC_HasCorrectStageName) {
  NTSCCVBSSourceStage stage;
  EXPECT_EQ(stage.get_node_type_info().stage_name, "NTSC_CVBS_Source");
}

TEST(CVBSSourceStageIdentityTest, PALM_HasCorrectStageName) {
  PALMCVBSSourceStage stage;
  EXPECT_EQ(stage.get_node_type_info().stage_name, "PAL_M_CVBS_Source");
}

TEST(CVBSSourceStageIdentityTest, PAL_NodeTypeIsSource) {
  PALCVBSSourceStage stage;
  EXPECT_EQ(stage.get_node_type_info().type, NodeType::SOURCE);
}

TEST(CVBSSourceStageIdentityTest, Stage_RequiredInputCountIsZero) {
  PALCVBSSourceStage stage;
  EXPECT_EQ(stage.required_input_count(), 0u);
}

TEST(CVBSSourceStageIdentityTest, Stage_VersionIsCurrentPhase) {
  PALCVBSSourceStage stage;
  EXPECT_EQ(stage.version(), "2.2.0");
}

// ===========================================================================
// Parameter descriptors
// ===========================================================================

TEST(CVBSSourceStageParamTest, UnknownSourceType_HasAllFourParameters) {
  PALCVBSSourceStage stage;
  auto descs = stage.get_parameter_descriptors();
  ASSERT_EQ(descs.size(), 4u);
  EXPECT_EQ(descs[0].name, "input_path");
  EXPECT_EQ(descs[1].name, "y_path");
  EXPECT_EQ(descs[2].name, "c_path");
  EXPECT_EQ(descs[3].name, "sample_encoding");
}

TEST(CVBSSourceStageParamTest, CompositeSourceType_OmitsYCPaths) {
  PALCVBSSourceStage stage;
  auto descs =
      stage.get_parameter_descriptors(VideoSystem::PAL, SourceType::Composite);
  ASSERT_EQ(descs.size(), 2u);
  EXPECT_EQ(descs[0].name, "input_path");
  EXPECT_EQ(descs[1].name, "sample_encoding");
}

TEST(CVBSSourceStageParamTest, YCSourceType_OmitsCompositePath) {
  PALCVBSSourceStage stage;
  auto descs =
      stage.get_parameter_descriptors(VideoSystem::PAL, SourceType::YC);
  ASSERT_EQ(descs.size(), 3u);
  EXPECT_EQ(descs[0].name, "y_path");
  EXPECT_EQ(descs[1].name, "c_path");
  EXPECT_EQ(descs[2].name, "sample_encoding");
}

TEST(CVBSSourceStageParamTest, SampleEncoding_DefaultsToFromMetadata) {
  PALCVBSSourceStage stage;
  auto descs = stage.get_parameter_descriptors();
  const auto& pd = descs[3];
  ASSERT_EQ(pd.name, "sample_encoding");
  EXPECT_EQ(pd.type, ParameterType::STRING);
  ASSERT_TRUE(pd.constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*pd.constraints.default_value),
            "From metadata");
  // "From metadata" plus the four supported encodings.
  ASSERT_EQ(pd.constraints.allowed_strings.size(), 5u);
  EXPECT_EQ(pd.constraints.allowed_strings[0], "From metadata");
  EXPECT_EQ(pd.constraints.allowed_strings[1], "CVBS_U10_4FSC");
  EXPECT_EQ(pd.constraints.allowed_strings[2], "CVBS_U16_4FSC");
  EXPECT_EQ(pd.constraints.allowed_strings[3], "CVBS_TPG21_4FSC");
  EXPECT_EQ(pd.constraints.allowed_strings[4], "CVBS_S16_4FSC");
}

TEST(CVBSSourceStageParamTest, SetGet_SampleEncoding_RoundTrips) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"sample_encoding", std::string("CVBS_U16_4FSC")}};
  ASSERT_TRUE(stage.set_parameters(p));
  auto got = stage.get_parameters();
  EXPECT_EQ(std::get<std::string>(got["sample_encoding"]), "CVBS_U16_4FSC");
}

TEST(CVBSSourceStageParamTest, InputPath_IsFilePathType) {
  PALCVBSSourceStage stage;
  auto descs = stage.get_parameter_descriptors();
  EXPECT_EQ(descs[0].type, ParameterType::FILE_PATH);
}

TEST(CVBSSourceStageParamTest, InputPath_DefaultIsEmptyString) {
  PALCVBSSourceStage stage;
  auto descs = stage.get_parameter_descriptors();
  ASSERT_TRUE(descs[0].constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*descs[0].constraints.default_value), "");
}

TEST(CVBSSourceStageParamTest, SetGet_InputPath_RoundTrips) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/test/video.cvbs")}};
  ASSERT_TRUE(stage.set_parameters(p));
  auto got = stage.get_parameters();
  EXPECT_EQ(std::get<std::string>(got["input_path"]), "/test/video.cvbs");
}

TEST(CVBSSourceStageParamTest, SetParameters_RejectsUnknownKey) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{{"no_such_param", std::string("x")}};
  EXPECT_FALSE(stage.set_parameters(p));
}

// ===========================================================================
// Configuration status from set_parameters
// ===========================================================================

TEST(CVBSSourceStageStatusTest, SetParameters_ShowsRed_WhenPathEmpty) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{{"input_path", std::string("")}};
  ASSERT_TRUE(stage.set_parameters(p));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Red);
}

TEST(CVBSSourceStageStatusTest, SetParameters_ShowsRed_WhenFileNotAccessible) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->input_file_valid = false;
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")}};
  ASSERT_TRUE(stage.set_parameters(p));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Red);
}

TEST(CVBSSourceStageStatusTest,
     SetParameters_ShowsYellow_WhenMetadataNotAccessible) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_available = false;
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")}};
  ASSERT_TRUE(stage.set_parameters(p));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Yellow);
}

TEST(CVBSSourceStageStatusTest,
     SetParameters_ShowsRed_WhenPalFileAddedToNtscStage) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  NTSCCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")}};
  ASSERT_TRUE(stage.set_parameters(p));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Red);
}

TEST(CVBSSourceStageStatusTest,
     SetParameters_ShowsRed_WhenNtscFileAddedToPalStage) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("NTSC");
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")}};
  ASSERT_TRUE(stage.set_parameters(p));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Red);
}

TEST(CVBSSourceStageStatusTest,
     SetParameters_ShowsGreen_WhenPalFileInPalStage) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")}};
  ASSERT_TRUE(stage.set_parameters(p));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Green);
}

TEST(CVBSSourceStageStatusTest,
     SetParameters_ShowsGreen_WhenNtscFileInNtscStage) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("NTSC");
  NTSCCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")}};
  ASSERT_TRUE(stage.set_parameters(p));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Green);
}

TEST(CVBSSourceStageStatusTest,
     SetParameters_ShowsGreen_WhenManualEncodingAndMetadataMissing) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_available = false;
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")},
      {"sample_encoding", std::string("CVBS_U16_4FSC")}};
  ASSERT_TRUE(stage.set_parameters(p));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Green);
}

TEST(CVBSSourceStageStatusTest,
     SetParameters_ShowsYellow_WhenFromMetadataAndMetadataMissing) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_available = false;
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")},
      {"sample_encoding", std::string("From metadata")}};
  ASSERT_TRUE(stage.set_parameters(p));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Yellow);
}

TEST(CVBSSourceStageStatusTest,
     SetParameters_ShowsRed_WhenManualEncodingButFileNotAccessible) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->input_file_valid = false;
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")},
      {"sample_encoding", std::string("CVBS_U16_4FSC")}};
  ASSERT_TRUE(stage.set_parameters(p));
  EXPECT_EQ(stage.get_configuration_status(), ConfigurationStatus::Red);
}

// ===========================================================================
// Signal state validation
// ===========================================================================

TEST(CVBSSourceStageValidationTest, RejectsNonTBCSignalState) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_record.signal_state_preset = "STANDARD_RAW";
  PALCVBSSourceStage stage(deps);
  expect_user_data_error(stage, kDefaultParams, "STANDARD_TBC_LOCKED");
}

TEST(CVBSSourceStageValidationTest, RejectsNonStandardSampleRateState) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_record.signal_state_preset = "NONSTANDARD_TBC_LOCKED";
  PALCVBSSourceStage stage(deps);
  expect_user_data_error(stage, kDefaultParams, "STANDARD_TBC_LOCKED");
}

// Burst lock is not required: colour-sequence phase is measured from the burst
// downstream, so an unlocked file (a disc skip during decode) still loads.
TEST(CVBSSourceStageValidationTest, AcceptsTBCUnlockedState) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_record.signal_state_preset = "STANDARD_TBC_UNLOCKED";
  PALCVBSSourceStage stage(deps);
  std::vector<ArtifactPtr> inputs;
  ObservationContext obs;
  EXPECT_NO_THROW(stage.execute(inputs, kDefaultParams, obs));
}

TEST(CVBSSourceStageValidationTest, RejectsMissingMetadata) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_available = false;
  PALCVBSSourceStage stage(deps);
  expect_user_data_error(stage, kDefaultParams, "metadata");
}

TEST(CVBSSourceStageValidationTest, RejectsWrongVideoStandard) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_record.preset = "NTSC";  // wrong system for PAL stage
  PALCVBSSourceStage stage(deps);
  expect_user_data_error(stage, kDefaultParams, "NTSC");
}

TEST(CVBSSourceStageValidationTest, RejectsInvalidInputFile) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->input_file_valid = false;
  PALCVBSSourceStage stage(deps);
  expect_user_data_error(stage, kDefaultParams, "Input file error");
}

TEST(CVBSSourceStageValidationTest, EmptyInputPath_ReturnsNoOutput) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  std::vector<ArtifactPtr> inputs;
  ObservationContext obs;
  std::map<std::string, ParameterValue> p{{"input_path", std::string("")}};
  auto results = stage.execute(inputs, p, obs);
  EXPECT_TRUE(results.empty());
}

TEST(CVBSSourceStageValidationTest, AcceptsAllFourSampleEncodings) {
  for (const auto& enc :
       {"CVBS_U10_4FSC", "CVBS_U16_4FSC", "CVBS_TPG21_4FSC", "CVBS_S16_4FSC"}) {
    auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL", enc);
    // For U10 encoding, payload must be int16_t stored as uint16_t.
    // Blanking=256 encoded as uint16_t bit-pattern of int16_t(256) = 256.
    if (std::string(enc) == "CVBS_U10_4FSC") {
      deps->payload_words.assign(static_cast<size_t>(kPalFrameSamples),
                                 static_cast<uint16_t>(kPalBlanking));
    } else if (std::string(enc) == "CVBS_S16_4FSC") {
      // CVBS_S16_4FSC: value=blanking stored as (value-blanking)*32 = 0.
      deps->payload_words.assign(static_cast<size_t>(kPalFrameSamples), 0u);
    } else if (std::string(enc) == "CVBS_TPG21_4FSC") {
      // CVBS_TPG21_4FSC: value=blanking stored as (value-508)*64 = (256-508)*64
      const int16_t stored = static_cast<int16_t>((kPalBlanking - 508) * 64);
      deps->payload_words.assign(static_cast<size_t>(kPalFrameSamples),
                                 static_cast<uint16_t>(stored));
    }
    PALCVBSSourceStage stage(deps);
    auto vfr = execute_and_get_vfr(stage, kDefaultParams);
    ASSERT_NE(vfr, nullptr) << "Failed for encoding: " << enc;
    EXPECT_EQ(vfr->frame_count(), 1u) << "Failed for encoding: " << enc;
  }
}

// ===========================================================================
// Manual sample encoding (metadata optional per CVBS file format spec)
// ===========================================================================

TEST(CVBSSourceManualEncodingTest, LoadsWithoutMetadata) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_available = false;
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")},
      {"sample_encoding", std::string("CVBS_U16_4FSC")}};
  auto vfr = execute_and_get_vfr(stage, p);
  ASSERT_NE(vfr, nullptr);
  EXPECT_EQ(vfr->frame_count(), 1u);
}

TEST(CVBSSourceManualEncodingTest, FrameCountMeasuredFromPayloadSize) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_available = false;
  deps->payload_words = make_blank_payload_u16(
      static_cast<size_t>(kPalFrameSamples) * 3, kPalBlanking);
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")},
      {"sample_encoding", std::string("CVBS_U16_4FSC")}};
  auto vfr = execute_and_get_vfr(stage, p);
  ASSERT_NE(vfr, nullptr);
  EXPECT_EQ(vfr->frame_count(), 3u);
}

TEST(CVBSSourceManualEncodingTest, MetadataIsIgnoredWhenManualEncodingSet) {
  // Even a metadata record that would be rejected (wrong system, unlocked
  // signal state) must not matter in manual mode — the sidecar is not read.
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("NTSC");
  deps->metadata_record.signal_state_preset = "STANDARD_RAW";
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")},
      {"sample_encoding", std::string("CVBS_U16_4FSC")}};
  auto vfr = execute_and_get_vfr(stage, p);
  ASSERT_NE(vfr, nullptr);
  EXPECT_EQ(vfr->frame_count(), 1u);
}

TEST(CVBSSourceManualEncodingTest, NormalisesSelectedEncoding) {
  // CVBS_S16_4FSC: stored 0 decodes to the PAL blanking level.
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_available = false;
  deps->payload_words.assign(static_cast<size_t>(kPalFrameSamples), 0u);
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")},
      {"sample_encoding", std::string("CVBS_S16_4FSC")}};
  auto vfr = execute_and_get_vfr(stage, p);
  ASSERT_NE(vfr, nullptr);
  const auto* line = vfr->get_line(0, 0);
  ASSERT_NE(line, nullptr);
  EXPECT_EQ(line[0], static_cast<int16_t>(kPalBlanking));
}

TEST(CVBSSourceManualEncodingTest, RejectsUnknownManualEncoding) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")},
      {"sample_encoding", std::string("RAW_S16_40M")}};
  expect_user_data_error(stage, p, "RAW_S16_40M");
}

TEST(CVBSSourceManualEncodingTest, FromMetadataValue_StillRequiresMetadata) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_available = false;
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")},
      {"sample_encoding", std::string("From metadata")}};
  expect_user_data_error(stage, p, "metadata");
}

TEST(CVBSSourceManualEncodingTest, AudioWithoutMetadata_GetsDerivedName) {
  // In manual-encoding mode no metadata is read, so a WAV sidecar still
  // becomes a channel pair with a name derived from the channel pair number.
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_available = false;
  deps->audio_files["/fake/video_audio_0.wav"] =
      std::vector<int32_t>(1920 * 2, int32_t{0});
  PALCVBSSourceStage stage(deps);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")},
      {"sample_encoding", std::string("CVBS_U16_4FSC")}};
  auto vfr = execute_and_get_vfr(stage, p);
  ASSERT_NE(vfr, nullptr);
  EXPECT_TRUE(vfr->has_audio());
  const auto desc = vfr->get_audio_channel_pair_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->name, "Channel pair 0");
  EXPECT_EQ(desc->origin, AudioOrigin::UNKNOWN);
}

TEST(CVBSSourceManualEncodingTest, EncodingChangeInvalidatesCache) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  std::vector<ArtifactPtr> inputs;
  ObservationContext obs;
  auto r1 = stage.execute(inputs, kDefaultParams, obs);
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")},
      {"sample_encoding", std::string("CVBS_U16_4FSC")}};
  auto r2 = stage.execute(inputs, p, obs);
  ASSERT_EQ(r1.size(), 1u);
  ASSERT_EQ(r2.size(), 1u);
  EXPECT_NE(r1.front().get(), r2.front().get());
}

// ===========================================================================
// Sample encoding normalisation
// ===========================================================================

// Helper: decode one frame with a given encoding and return the decoded sample
// value at a specific word index.
int16_t decode_one_sample(const std::string& preset, uint16_t raw_word,
                          const std::string& encoding, int32_t frame_samples) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>(preset, encoding);
  deps->payload_words.assign(static_cast<size_t>(frame_samples), raw_word);
  deps->metadata_record.sample_encoding_preset = encoding;
  deps->metadata_record.number_of_sequential_frames = 1;

  std::shared_ptr<FixedFormatCVBSSourceStage> stage;
  if (preset == "PAL") {
    stage = std::make_shared<PALCVBSSourceStage>(deps);
  } else if (preset == "NTSC") {
    stage = std::make_shared<NTSCCVBSSourceStage>(deps);
  } else {
    stage = std::make_shared<PALMCVBSSourceStage>(deps);
  }

  auto vfr = execute_and_get_vfr(*stage, kDefaultParams);
  if (!vfr) return 0;
  const auto* line = vfr->get_line(0, 0);
  return line ? line[0] : 0;
}

TEST(CVBSSourceEncodingTest, U10_IdentityTransform) {
  // CVBS_U10_4FSC: raw uint16_t reinterpreted as int16_t.
  const int16_t expected = 256;  // PAL blanking
  const uint16_t raw = static_cast<uint16_t>(expected);
  EXPECT_EQ(decode_one_sample("PAL", raw, "CVBS_U10_4FSC", kPalFrameSamples),
            expected);
}

TEST(CVBSSourceEncodingTest, U10_PreservesHeadroomAboveWhite) {
  // Raw 1023 (just below peak) encoded as uint16_t bit-pattern of
  // int16_t(1023).
  const int16_t expected = 1023;
  const uint16_t raw = static_cast<uint16_t>(expected);
  EXPECT_EQ(decode_one_sample("PAL", raw, "CVBS_U10_4FSC", kPalFrameSamples),
            expected);
}

TEST(CVBSSourceEncodingTest, U16_DividesBy64) {
  // PAL blanking = 256. Stored as 256 * 64 = 16384.
  const uint16_t raw = static_cast<uint16_t>(kPalBlanking * 64);
  EXPECT_EQ(decode_one_sample("PAL", raw, "CVBS_U16_4FSC", kPalFrameSamples),
            static_cast<int16_t>(kPalBlanking));
}

TEST(CVBSSourceEncodingTest, U16_WhiteLevelRoundtrips) {
  const int32_t white_10bit = kPalWhite;
  const uint16_t raw = static_cast<uint16_t>(white_10bit * 64);
  EXPECT_EQ(decode_one_sample("PAL", raw, "CVBS_U16_4FSC", kPalFrameSamples),
            static_cast<int16_t>(kPalWhite));
}

TEST(CVBSSourceEncodingTest, TPG21_BlankingLevelRoundtrips) {
  // CVBS_TPG21_4FSC: stored = (value - 508) * 64
  const int32_t value = kPalBlanking;  // 256
  const int16_t stored = static_cast<int16_t>((value - 508) * 64);
  const uint16_t raw = static_cast<uint16_t>(stored);
  EXPECT_EQ(decode_one_sample("PAL", raw, "CVBS_TPG21_4FSC", kPalFrameSamples),
            static_cast<int16_t>(kPalBlanking));
}

TEST(CVBSSourceEncodingTest, TPG21_WhiteLevelRoundtrips) {
  const int32_t value = kPalWhite;  // 844
  const int16_t stored = static_cast<int16_t>((value - 508) * 64);
  const uint16_t raw = static_cast<uint16_t>(stored);
  EXPECT_EQ(decode_one_sample("PAL", raw, "CVBS_TPG21_4FSC", kPalFrameSamples),
            static_cast<int16_t>(kPalWhite));
}

TEST(CVBSSourceEncodingTest, S16_BlankingLevelRoundtrips) {
  // CVBS_S16_4FSC: stored = (value - blanking) * 32. At blanking → stored = 0.
  EXPECT_EQ(decode_one_sample("PAL", 0u, "CVBS_S16_4FSC", kPalFrameSamples),
            static_cast<int16_t>(kPalBlanking));
}

TEST(CVBSSourceEncodingTest, S16_WhiteLevelRoundtrips) {
  // PAL white = 844; (844-256)*32 = 18816.
  const int16_t stored = static_cast<int16_t>((kPalWhite - kPalBlanking) * 32);
  const uint16_t raw = static_cast<uint16_t>(stored);
  EXPECT_EQ(decode_one_sample("PAL", raw, "CVBS_S16_4FSC", kPalFrameSamples),
            static_cast<int16_t>(kPalWhite));
}

// ===========================================================================
// SourceParameters from spec constants
// ===========================================================================

TEST(CVBSSourceParamsTest, PAL_FrameWidthNominalIs1135) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  auto params = vfr->get_video_parameters();
  ASSERT_TRUE(params.has_value());
  EXPECT_EQ(params->frame_width_nominal, kPalSamplesPerLineNominal);
}

TEST(CVBSSourceParamsTest, PAL_FrameHeightIs625) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  auto params = vfr->get_video_parameters();
  ASSERT_TRUE(params.has_value());
  EXPECT_EQ(params->frame_height, kPalFrameLines);
}

TEST(CVBSSourceParamsTest, PAL_LevelsMatchSpecConstants) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  auto p = vfr->get_video_parameters();
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->sync_tip_level, kPalSyncTip);
  EXPECT_EQ(p->blanking_level, kPalBlanking);
  EXPECT_EQ(p->black_level, kPalBlack);
  EXPECT_EQ(p->white_level, kPalWhite);
  EXPECT_EQ(p->peak_level, kPalPeak);
}

TEST(CVBSSourceParamsTest, PAL_LevelsNotFromMetadata) {
  // Even if metadata were to provide different level values, the source
  // MUST populate SourceParameters from cvbs_signal_constants.h.
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_record.ntsc_j_black_level = 200;  // only valid for NTSC
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  auto p = vfr->get_video_parameters();
  ASSERT_TRUE(p.has_value());
  // PAL black level must always be the spec value.
  EXPECT_EQ(p->black_level, kPalBlack);
}

TEST(CVBSSourceParamsTest, NTSC_LevelsMatchSpecConstants) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("NTSC");
  deps->payload_words = make_blank_payload_u16(
      static_cast<size_t>(kNtscFrameSamples), kNtscBlanking);
  NTSCCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  auto p = vfr->get_video_parameters();
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->sync_tip_level, kNtscSyncTip);
  EXPECT_EQ(p->blanking_level, kNtscBlanking);
  EXPECT_EQ(p->black_level, kNtscBlack);
  EXPECT_EQ(p->white_level, kNtscWhite);
  EXPECT_EQ(p->peak_level, kNtscPeak);
}

TEST(CVBSSourceParamsTest, PALM_FrameWidthIs909) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL_M");
  deps->metadata_record.preset = "PAL_M";
  deps->payload_words = make_blank_payload_u16(
      static_cast<size_t>(kPalMFrameSamples), kNtscBlanking);
  PALMCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  auto p = vfr->get_video_parameters();
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->frame_width_nominal, kPalMSamplesPerLine);
}

TEST(CVBSSourceParamsTest, ProvenanceFieldsComeFromMetadata) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_record.decoder = "vhs-decode";
  deps->metadata_record.git_branch = "main";
  deps->metadata_record.git_commit = "abc1234";
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  auto p = vfr->get_video_parameters();
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->decoder, "vhs-decode");
  EXPECT_EQ(p->git_branch, "main");
  EXPECT_EQ(p->git_commit, "abc1234");
}

TEST(CVBSSourceParamsTest, ProvenanceFieldsEmptyWhenMetadataOmitsThem) {
  // The source must not invent provenance: absent .meta columns yield empty
  // fields, and the CVBS format has no tape_format / is_mapped equivalents.
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  auto p = vfr->get_video_parameters();
  ASSERT_TRUE(p.has_value());
  EXPECT_TRUE(p->decoder.empty());
  EXPECT_TRUE(p->git_branch.empty());
  EXPECT_TRUE(p->git_commit.empty());
  EXPECT_TRUE(p->tape_format.empty());
  EXPECT_FALSE(p->is_mapped);
}

TEST(CVBSSourceParamsTest, FrameCountMatchesMetadata) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_record.number_of_sequential_frames = 3;
  deps->payload_words = make_blank_payload_u16(
      static_cast<size_t>(kPalFrameSamples) * 3, kPalBlanking);
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  EXPECT_EQ(vfr->frame_count(), 3u);
  auto p = vfr->get_video_parameters();
  ASSERT_TRUE(p.has_value());
  EXPECT_EQ(p->number_of_sequential_frames, 3);
}

// ===========================================================================
// Colour-frame phase: source stage does not measure
// ===========================================================================
// The source stage loads raw samples only; colour-frame phase measurement is
// delegated to ColourFramePhaseObserver.  See colour_frame_phase_observer_test.

TEST(CVBSSourcePhaseContractTest, DescriptorCarriesNoColourPhase) {
  // The source stage loads raw samples only and produces a descriptor with no
  // colour-sequence phase: FrameDescriptor no longer has a colour_frame_index
  // field (measurement is ColourFramePhaseObserver's responsibility, pulled via
  // colour_frame_phase_query.h). This test guards that the source still yields
  // a valid descriptor after that field was removed.
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  auto desc = vfr->get_frame_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->system, orc::VideoSystem::PAL);
}

// ===========================================================================
// VFR navigation contract
// ===========================================================================

TEST(CVBSVFRTest, FrameRange_StartsAtZero) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  const auto range = vfr->frame_range();
  // frame_range() is a closed [first, last] range; one frame → last = 0.
  EXPECT_EQ(range.first, 0u);
  EXPECT_EQ(range.last, 0u);
}

TEST(CVBSVFRTest, HasFrame_TrueForValidFrameID) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  EXPECT_TRUE(vfr->has_frame(0));
  EXPECT_FALSE(vfr->has_frame(1));
}

TEST(CVBSVFRTest, GetFrameDescriptor_ReturnsCorrectHeight) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  auto desc = vfr->get_frame_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->height, static_cast<size_t>(kPalFrameLines));
}

TEST(CVBSVFRTest, GetFrameDescriptor_ReturnsCorrectSampleCount) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  auto desc = vfr->get_frame_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->samples_total, static_cast<size_t>(kPalFrameSamples));
}

TEST(CVBSVFRTest, GetFrame_ReturnsNonNullPointerForValidFrame) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  EXPECT_NE(vfr->get_frame(0), nullptr);
  EXPECT_EQ(vfr->get_frame(1), nullptr);
}

TEST(CVBSVFRTest, GetLine_ReturnsNonNullForValidLine) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  EXPECT_NE(vfr->get_line(0, 0), nullptr);
  EXPECT_NE(vfr->get_line(0, kPalFrameLines - 1), nullptr);
  EXPECT_EQ(vfr->get_line(0, kPalFrameLines), nullptr);
}

TEST(CVBSVFRTest, GetFrameCopy_SizeMatchesFrameSamples) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  auto copy = vfr->get_frame_copy(0);
  EXPECT_EQ(copy.size(), static_cast<size_t>(kPalFrameSamples));
}

TEST(CVBSVFRTest, PAL_Line0StartsAtOffset0) {
  // PAL line 0 offset = 0 (first 1135-sample line).
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL", "CVBS_U10_4FSC");
  // Store a sentinel value only at position 0.
  deps->payload_words.assign(static_cast<size_t>(kPalFrameSamples),
                             static_cast<uint16_t>(kPalBlanking));
  deps->payload_words[0] = static_cast<uint16_t>(500);  // sentinel
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  const auto* line0 = vfr->get_line(0, 0);
  ASSERT_NE(line0, nullptr);
  EXPECT_EQ(line0[0], int16_t{500});
}

TEST(CVBSVFRTest, PAL_Line156_ConstantWidthOffset) {
  // EBU Tech. 3280-E normative: the 4 extra samples sit on line 312 (0-based,
  // = line 313 1-based, the last of field 1) and line 624 (0-based, = line 625
  // 1-based, the last of field 2) — 2 extras each. Lines 0–312 therefore start
  // at exactly n×1135 (constant-width).  Line 156 offset = 156×1135 = 177060.
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL", "CVBS_U10_4FSC");
  deps->payload_words.assign(static_cast<size_t>(kPalFrameSamples),
                             static_cast<uint16_t>(kPalBlanking));
  constexpr size_t kLine156Offset = 156 * 1135;                      // = 177060
  deps->payload_words[kLine156Offset] = static_cast<uint16_t>(500);  // sentinel
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  const auto* line156 = vfr->get_line(0, 156);
  ASSERT_NE(line156, nullptr);
  EXPECT_EQ(line156[0], int16_t{500});
}

TEST(CVBSVFRTest, PAL_Line313_EBU3280Offset) {
  // EBU Tech. 3280-E normative: line 312 (0-based) has 1137 samples (2 extra).
  // Line 313 (0-based) therefore starts at 312×1135 + 1137 = 355257 = 313×1135
  // + 2.
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL", "CVBS_U10_4FSC");
  deps->payload_words.assign(static_cast<size_t>(kPalFrameSamples),
                             static_cast<uint16_t>(kPalBlanking));
  constexpr size_t kLine313Offset = 313 * 1135 + 2;                  // = 355257
  deps->payload_words[kLine313Offset] = static_cast<uint16_t>(500);  // sentinel
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  const auto* line313 = vfr->get_line(0, 313);
  ASSERT_NE(line313, nullptr);
  EXPECT_EQ(line313[0], int16_t{500});
}

// ===========================================================================
// Execute() caches the representation
// ===========================================================================

TEST(CVBSSourceCacheTest, SecondExecuteReturnsSameArtifact) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  std::vector<ArtifactPtr> inputs;
  ObservationContext obs;
  auto r1 = stage.execute(inputs, kDefaultParams, obs);
  auto r2 = stage.execute(inputs, kDefaultParams, obs);
  ASSERT_EQ(r1.size(), 1u);
  ASSERT_EQ(r2.size(), 1u);
  EXPECT_EQ(r1.front().get(), r2.front().get());
}

// ===========================================================================
// Sidecar: dropout
// ===========================================================================

TEST(CVBSSidecarDropoutTest, NoDropoutSidecar_GetDropoutHintsEmpty) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  // dropout_runs default = empty
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  EXPECT_TRUE(vfr->get_dropout_hints(0).empty());
}

TEST(CVBSSidecarDropoutTest, WithDropoutRuns_HintsReturnedForCorrectFrame) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_record.number_of_sequential_frames = 2;
  deps->payload_words = make_blank_payload_u16(
      static_cast<size_t>(kPalFrameSamples) * 2, kPalBlanking);

  DropoutRun run0;
  run0.frame_id = 0;
  run0.sample_start = 100;
  run0.sample_count = 50;
  run0.severity = 1;
  DropoutRun run1;
  run1.frame_id = 1;
  run1.sample_start = 200;
  run1.sample_count = 20;
  run1.severity = 2;
  deps->dropout_runs = {run0, run1};

  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);

  auto hints0 = vfr->get_dropout_hints(0);
  ASSERT_EQ(hints0.size(), 1u);
  EXPECT_EQ(hints0[0].sample_start, 100u);

  auto hints1 = vfr->get_dropout_hints(1);
  ASSERT_EQ(hints1.size(), 1u);
  EXPECT_EQ(hints1[0].sample_start, 200u);
}

// ===========================================================================
// Sidecar: audio
// ===========================================================================

namespace {

// One PAL frame (1920 stereo pairs) of interleaved 24-bit-in-int32 samples
// at a constant value.
std::vector<int32_t> one_pal_frame_audio(int32_t value) {
  return std::vector<int32_t>(1920 * 2, value);
}

}  // namespace

TEST(CVBSSidecarAudioTest, NoAudioSidecar_HasAudioFalse) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  // audio_files empty (default): no channel pairs.
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  EXPECT_FALSE(vfr->has_audio());
  EXPECT_EQ(vfr->audio_channel_pair_count(), 0u);
  EXPECT_FALSE(vfr->get_audio_channel_pair_descriptor(0).has_value());
  EXPECT_TRUE(vfr->get_audio_samples(0, 0).empty());
}

TEST(CVBSSidecarAudioTest, AudioPresent_ServesCadenceBlocksSampleExact) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  // Ramp payload: value i at interleaved position i, full 24-bit domain.
  std::vector<int32_t> payload;
  for (int32_t i = 0; i < 1920 * 2; ++i) {
    payload.push_back(i * 1000 - 1920000);
  }
  deps->audio_files["/fake/video_audio_0.wav"] = payload;
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  EXPECT_TRUE(vfr->has_audio());
  ASSERT_EQ(vfr->audio_channel_pair_count(), 1u);

  // The container payload is already in the pipeline audio form: samples
  // are served bit-exact with no conversion.
  const auto samples = vfr->get_audio_samples(0, 0);
  ASSERT_EQ(samples.size(),
            static_cast<size_t>(audio_pairs_in_frame(0, VideoSystem::PAL)) * 2);
  for (size_t i = 0; i < samples.size(); ++i) {
    ASSERT_EQ(samples[i], payload[i]) << "sample " << i;
  }
}

TEST(CVBSSidecarAudioTest, PerFrameReads_SeekByCadenceOffset) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_record.number_of_sequential_frames = 2;
  deps->payload_words = make_blank_payload_u16(
      static_cast<size_t>(kPalFrameSamples) * 2, kPalBlanking);
  deps->audio_files["/fake/video_audio_0.wav"] =
      std::vector<int32_t>(2 * 1920 * 2, int32_t{9});
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);

  // Frame 1 of a PAL stream starts at cumulative pair offset 1920.
  (void)vfr->get_audio_samples(0, 1);
  EXPECT_EQ(deps->last_audio_read_offset, 1920u);
  EXPECT_EQ(deps->last_audio_read_count, 1920u);
}

TEST(CVBSSidecarAudioTest, ShortAudio_IsSilencePaddedToCadence) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  // Only 100 pairs of material for a 1920-pair PAL frame (also triggers the
  // equal-length warning; serving still pads with silence).
  deps->audio_files["/fake/video_audio_0.wav"] =
      std::vector<int32_t>(100 * 2, int32_t{7});
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);

  const auto samples = vfr->get_audio_samples(0, 0);
  ASSERT_EQ(samples.size(), 1920u * 2);
  EXPECT_EQ(samples[0], 7);
  EXPECT_EQ(samples[199], 7);
  EXPECT_EQ(samples[200], 0);
  EXPECT_EQ(samples.back(), 0);
}

TEST(CVBSSidecarAudioTest, NTSC_ServesCadenceSizedFrames) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("NTSC");
  deps->metadata_record.number_of_sequential_frames = 2;
  deps->payload_words = make_blank_payload_u16(
      static_cast<size_t>(kNtscFrameSamples) * 2, kNtscBlanking);
  // SMPTE 272M-1994 Section 14.3: frames 0/1 of the audio frame sequence
  // carry 1602 and 1601 stereo pairs.
  deps->audio_files["/fake/video_audio_0.wav"] =
      std::vector<int32_t>((1602 + 1601) * 2, int32_t{11});
  NTSCCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);

  EXPECT_EQ(vfr->get_audio_samples(0, 0).size(), 1602u * 2);
  EXPECT_EQ(vfr->get_audio_samples(0, 1).size(), 1601u * 2);
  // Frame 1 starts at cumulative pair offset 1602.
  EXPECT_EQ(deps->last_audio_read_offset, 1602u);
}

TEST(CVBSSidecarAudioTest, OutOfRangePairOrFrame_ReturnsEmpty) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->audio_files["/fake/video_audio_0.wav"] = one_pal_frame_audio(1);
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  EXPECT_TRUE(vfr->get_audio_samples(1, 0).empty());
  EXPECT_TRUE(vfr->get_audio_samples(0, 1).empty());  // only one frame
}

// --- WAV header validation (CVBS file format spec v1.4.0) ---

TEST(CVBSSidecarAudioTest, WrongSampleRate_IsRejected) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->audio_files["/fake/video_audio_0.wav"] = one_pal_frame_audio(1);
  deps->audio_wav_info_overrides["/fake/video_audio_0.wav"] =
      CVBSAudioWavInfo{1, 2, 44100, 24, 1920 * 6};
  PALCVBSSourceStage stage(deps);
  expect_user_data_error(stage, kDefaultParams, "44100");
}

TEST(CVBSSidecarAudioTest, WrongBitDepth_IsRejected) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->audio_files["/fake/video_audio_0.wav"] = one_pal_frame_audio(1);
  deps->audio_wav_info_overrides["/fake/video_audio_0.wav"] =
      CVBSAudioWavInfo{1, 2, 48000, 16, 1920 * 4};
  PALCVBSSourceStage stage(deps);
  expect_user_data_error(stage, kDefaultParams, "16-bit");
}

TEST(CVBSSidecarAudioTest, WrongChannelCount_IsRejected) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->audio_files["/fake/video_audio_0.wav"] = one_pal_frame_audio(1);
  deps->audio_wav_info_overrides["/fake/video_audio_0.wav"] =
      CVBSAudioWavInfo{1, 1, 48000, 24, 1920 * 3};
  PALCVBSSourceStage stage(deps);
  expect_user_data_error(stage, kDefaultParams, "1 channel");
}

TEST(CVBSSidecarAudioTest, NonPcmOrMalformedHeader_IsRejected) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->audio_files["/fake/video_audio_0.wav"] = one_pal_frame_audio(1);
  // A malformed RIFF header reports zeroed fields.
  deps->audio_wav_info_overrides["/fake/video_audio_0.wav"] =
      CVBSAudioWavInfo{};
  PALCVBSSourceStage stage(deps);
  expect_user_data_error(stage, kDefaultParams, "format tag 0");
}

TEST(CVBSSidecarAudioTest, LengthMismatch_ProducesWarningObservation) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  // 100 pairs instead of the 1920 required for one PAL frame.
  deps->audio_files["/fake/video_audio_0.wav"] =
      std::vector<int32_t>(100 * 2, int32_t{7});
  PALCVBSSourceStage stage(deps);
  std::vector<ArtifactPtr> inputs;
  ObservationContext obs;
  auto results = stage.execute(inputs, kDefaultParams, obs);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_TRUE(obs.has(FieldID(0), "cvbs_source", "audio_length_mismatch_0"));
}

TEST(CVBSSidecarAudioTest, ConformantLength_ProducesNoWarningObservation) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->audio_files["/fake/video_audio_0.wav"] = one_pal_frame_audio(7);
  deps->audio_pair_table = std::vector<CVBSAudioChannelPairRecord>{
      CVBSAudioChannelPairRecord{0, std::string("Analogue")}};
  PALCVBSSourceStage stage(deps);
  std::vector<ArtifactPtr> inputs;
  ObservationContext obs;
  auto results = stage.execute(inputs, kDefaultParams, obs);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_FALSE(obs.has(FieldID(0), "cvbs_source", "audio_length_mismatch_0"));
  EXPECT_FALSE(
      obs.has(FieldID(0), "cvbs_source", "audio_missing_metadata_row_0"));
}

// --- Multi-pair enumeration and per-pair metadata ---

TEST(CVBSSidecarAudioTest, MultipleWavSidecars_BecomeChannelPairsByNumber) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->audio_files["/fake/video_audio_0.wav"] = one_pal_frame_audio(1);
  deps->audio_files["/fake/video_audio_1.wav"] = one_pal_frame_audio(2);
  deps->audio_files["/fake/video_audio_7.wav"] = one_pal_frame_audio(3);
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  // Container pair numbers are preserved as pipeline indices: numbers need
  // not be contiguous, so pairs 2–6 are silent placeholders.
  ASSERT_EQ(vfr->audio_channel_pair_count(), 8u);

  // No audio_channel_pair rows: names derive from the channel pair number,
  // origin unknown (the CVBS metadata carries no origin information).
  const auto desc0 = vfr->get_audio_channel_pair_descriptor(0);
  const auto desc2 = vfr->get_audio_channel_pair_descriptor(2);
  const auto desc7 = vfr->get_audio_channel_pair_descriptor(7);
  ASSERT_TRUE(desc0 && desc2 && desc7);
  EXPECT_EQ(desc0->name, "Channel pair 0");
  EXPECT_EQ(desc2->name, "Channel pair 2");
  EXPECT_EQ(desc7->name, "Channel pair 7");
  EXPECT_EQ(desc0->origin, AudioOrigin::UNKNOWN);
  EXPECT_FALSE(vfr->get_audio_channel_pair_descriptor(8).has_value());

  // Backed pairs serve their file's samples; placeholders serve silence.
  EXPECT_EQ(vfr->get_audio_samples(7, 0)[0], 3);
  const auto placeholder = vfr->get_audio_samples(2, 0);
  ASSERT_EQ(placeholder.size(), 1920u * 2);
  EXPECT_EQ(placeholder[0], 0);
}

TEST(CVBSSidecarAudioTest, Enumeration_ProbesOnlySingleDigitNames) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  // Legacy v1.2.0 two-digit names must not be picked up.
  deps->audio_files["/fake/video_audio_00.wav"] = one_pal_frame_audio(1);
  deps->audio_files["/fake/video_audio_15.wav"] = one_pal_frame_audio(2);
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  EXPECT_EQ(vfr->audio_channel_pair_count(), 0u);
  EXPECT_FALSE(vfr->has_audio());
}

TEST(CVBSSidecarAudioTest, AudioChannelPairTable_ProvidesDescriptorNames) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->audio_files["/fake/video_audio_0.wav"] = one_pal_frame_audio(5);
  deps->audio_files["/fake/video_audio_1.wav"] = one_pal_frame_audio(6);
  deps->audio_pair_table = std::vector<CVBSAudioChannelPairRecord>{
      CVBSAudioChannelPairRecord{0, std::string("Analogue")},
      CVBSAudioChannelPairRecord{1, std::string("EFM digital audio")}};

  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  ASSERT_EQ(vfr->audio_channel_pair_count(), 2u);

  const auto desc0 = vfr->get_audio_channel_pair_descriptor(0);
  ASSERT_TRUE(desc0.has_value());
  EXPECT_EQ(desc0->name, "Analogue");
  EXPECT_EQ(desc0->origin, AudioOrigin::UNKNOWN);

  const auto desc1 = vfr->get_audio_channel_pair_descriptor(1);
  ASSERT_TRUE(desc1.has_value());
  EXPECT_EQ(desc1->name, "EFM digital audio");
  EXPECT_EQ(desc1->origin, AudioOrigin::UNKNOWN);
}

TEST(CVBSSidecarAudioTest, MissingMetadataRow_WarnsAndDerivesName) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->audio_files["/fake/video_audio_0.wav"] = one_pal_frame_audio(5);
  deps->audio_files["/fake/video_audio_1.wav"] = one_pal_frame_audio(6);
  // Only pair 0 has a metadata row; pair 1's is missing (spec violation).
  deps->audio_pair_table = std::vector<CVBSAudioChannelPairRecord>{
      CVBSAudioChannelPairRecord{0, std::string("Analogue")}};

  PALCVBSSourceStage stage(deps);
  std::vector<ArtifactPtr> inputs;
  ObservationContext obs;
  auto results = stage.execute(inputs, kDefaultParams, obs);
  ASSERT_EQ(results.size(), 1u);
  auto vfr =
      std::dynamic_pointer_cast<VideoFrameRepresentation>(results.front());
  ASSERT_NE(vfr, nullptr);

  const auto desc1 = vfr->get_audio_channel_pair_descriptor(1);
  ASSERT_TRUE(desc1.has_value());
  EXPECT_EQ(desc1->name, "Channel pair 1");
  EXPECT_FALSE(
      obs.has(FieldID(0), "cvbs_source", "audio_missing_metadata_row_0"));
  EXPECT_TRUE(
      obs.has(FieldID(0), "cvbs_source", "audio_missing_metadata_row_1"));
}

TEST(CVBSSidecarAudioTest, AbsentTable_WarnsForEveryExistingFile) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->audio_files["/fake/video_audio_0.wav"] = one_pal_frame_audio(5);
  // audio_pair_table = nullopt (default): table absent entirely.
  PALCVBSSourceStage stage(deps);
  std::vector<ArtifactPtr> inputs;
  ObservationContext obs;
  auto results = stage.execute(inputs, kDefaultParams, obs);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_TRUE(
      obs.has(FieldID(0), "cvbs_source", "audio_missing_metadata_row_0"));
}

TEST(CVBSSidecarAudioTest, ManualEncodingMode_DoesNotWarnAboutMissingRows) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  deps->metadata_available = false;
  deps->audio_files["/fake/video_audio_0.wav"] = one_pal_frame_audio(5);
  PALCVBSSourceStage stage(deps);
  std::vector<ArtifactPtr> inputs;
  ObservationContext obs;
  std::map<std::string, ParameterValue> p{
      {"input_path", std::string("/fake/video.cvbs")},
      {"sample_encoding", std::string("CVBS_U16_4FSC")}};
  auto results = stage.execute(inputs, p, obs);
  ASSERT_EQ(results.size(), 1u);
  EXPECT_FALSE(
      obs.has(FieldID(0), "cvbs_source", "audio_missing_metadata_row_0"));
}

// ===========================================================================
// Sidecar: EFM
// ===========================================================================

TEST(CVBSSidecarEFMTest, NoEFMSidecar_HasEfmFalse) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  // efm_table = nullopt (default)
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  EXPECT_FALSE(vfr->has_efm());
  EXPECT_EQ(vfr->get_efm_sample_count(0), 0u);
  EXPECT_TRUE(vfr->get_efm_samples(0).empty());
}

TEST(CVBSSidecarEFMTest, WithEFMSidecar_ReturnsCorrectBytes) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  CVBSExtensionFrameRef ref0;
  ref0.offset = 0;
  ref0.count = 5;
  deps->efm_table = std::vector<CVBSExtensionFrameRef>{ref0};
  deps->efm_data = {0x01, 0x02, 0x03, 0x04, 0x05};
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  EXPECT_TRUE(vfr->has_efm());
  EXPECT_EQ(vfr->get_efm_sample_count(0), 5u);
  auto bytes = vfr->get_efm_samples(0);
  ASSERT_EQ(bytes.size(), 5u);
  EXPECT_EQ(bytes[0], uint8_t{0x01});
  EXPECT_EQ(bytes[4], uint8_t{0x05});
}

// ===========================================================================
// Sidecar: AC3 RF
// ===========================================================================

TEST(CVBSSidecarAC3Test, NoAC3Sidecar_HasAc3False) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  EXPECT_FALSE(vfr->has_ac3_rf());
  EXPECT_EQ(vfr->get_ac3_symbol_count(0), 0u);
  EXPECT_TRUE(vfr->get_ac3_symbols(0).empty());
}

TEST(CVBSSidecarAC3Test, WithAC3Sidecar_ReturnsCorrectBytes) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  CVBSExtensionFrameRef ref0;
  ref0.offset = 0;
  ref0.count = 3;
  deps->ac3_table = std::vector<CVBSExtensionFrameRef>{ref0};
  deps->ac3_data = {0xAA, 0xBB, 0xCC};
  PALCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  EXPECT_TRUE(vfr->has_ac3_rf());
  EXPECT_EQ(vfr->get_ac3_symbol_count(0), 3u);
  auto bytes = vfr->get_ac3_symbols(0);
  ASSERT_EQ(bytes.size(), 3u);
  EXPECT_EQ(bytes[1], uint8_t{0xBB});
}

// ===========================================================================
// NTSC-J black level override
// ===========================================================================

TEST(CVBSNTSCJTest, NTSCWithBlackLevelOverride_FrameDescriptorHasOverride) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("NTSC");
  deps->metadata_record.ntsc_j_black_level = 230;  // Japanese NTSC 0 IRE
  deps->payload_words = make_blank_payload_u16(
      static_cast<size_t>(kNtscFrameSamples), kNtscBlanking);
  NTSCCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  auto desc = vfr->get_frame_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  ASSERT_TRUE(desc->black_level_override.has_value());
  EXPECT_EQ(*desc->black_level_override, 230);
}

TEST(CVBSNTSCJTest, NTSCWithBlackLevelOverride_SourceParamsHasNonstandardFlag) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("NTSC");
  deps->metadata_record.ntsc_j_black_level = 230;
  deps->payload_words = make_blank_payload_u16(
      static_cast<size_t>(kNtscFrameSamples), kNtscBlanking);
  NTSCCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  auto p = vfr->get_video_parameters();
  ASSERT_TRUE(p.has_value());
  EXPECT_TRUE(p->has_nonstandard_values);
  EXPECT_EQ(p->black_level, 230);
}

TEST(CVBSNTSCJTest, NTSCWithoutBlackLevelOverride_UsesSpecDefault) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("NTSC");
  deps->payload_words = make_blank_payload_u16(
      static_cast<size_t>(kNtscFrameSamples), kNtscBlanking);
  NTSCCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  auto p = vfr->get_video_parameters();
  ASSERT_TRUE(p.has_value());
  EXPECT_FALSE(p->has_nonstandard_values);
  EXPECT_EQ(p->black_level, kNtscBlack);
}

// ===========================================================================
// PAL_M stage
// ===========================================================================

TEST(CVBSPALMTest, PALMStage_CanLoadPALMFile) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL_M");
  deps->metadata_record.preset = "PAL_M";
  deps->payload_words = make_blank_payload_u16(
      static_cast<size_t>(kPalMFrameSamples), kNtscBlanking);
  PALMCVBSSourceStage stage(deps);
  auto vfr = execute_and_get_vfr(stage, kDefaultParams);
  ASSERT_NE(vfr, nullptr);
  EXPECT_EQ(vfr->frame_count(), 1u);
  auto desc = vfr->get_frame_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->system, VideoSystem::PAL_M);
  EXPECT_EQ(desc->samples_total, static_cast<size_t>(kPalMFrameSamples));
}

TEST(CVBSPALMTest, PALMStage_RejectsNTSCFile) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL_M");
  deps->metadata_record.preset = "NTSC";  // wrong
  deps->payload_words = make_blank_payload_u16(
      static_cast<size_t>(kPalMFrameSamples), kNtscBlanking);
  PALMCVBSSourceStage stage(deps);
  expect_user_data_error(stage, kDefaultParams, "NTSC");
}

// ===========================================================================
// Preview and report (smoke tests)
// ===========================================================================

TEST(CVBSPreviewTest, PreviewCapability_InvalidBeforeLoad) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  EXPECT_FALSE(stage.get_preview_capability().is_valid());
}

TEST(CVBSPreviewTest, PreviewCapability_ValidAfterLoad) {
  auto deps = std::make_shared<FakeCVBSSourceStageDeps>("PAL");
  PALCVBSSourceStage stage(deps);
  execute_and_get_vfr(stage, kDefaultParams);
  auto cap = stage.get_preview_capability();
  EXPECT_TRUE(cap.is_valid());
  EXPECT_FALSE(cap.supported_data_types.empty());
}

}  // namespace tests
}  // namespace orc
