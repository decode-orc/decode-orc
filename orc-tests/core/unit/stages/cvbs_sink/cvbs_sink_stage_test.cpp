/*
 * File:        cvbs_sink_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for CVBSSinkStage parameter contracts and trigger
 * dependency seam
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/cvbs_sink/cvbs_sink_stage.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "../../../../orc/plugins/stages/cvbs_sink/cvbs_sink_container.h"
#include "../../../../orc/plugins/stages/cvbs_sink/cvbs_sink_encode.h"
#include "../../../../orc/plugins/stages/cvbs_sink/cvbs_sink_stage_deps_interface.h"
#include "../../include/observation_context_interface_mock.h"
#include "../../include/video_frame_representation_artifact_mock.h"

namespace orc_unit_test {
using testing::_;
using testing::NiceMock;
using testing::Return;
using testing::StrictMock;

class MockCVBSSinkStageDeps : public orc::ICVBSSinkStageDeps {
 public:
  MOCK_METHOD(void, init,
              (orc::TriggerProgressCallback progress_callback,
               std::atomic<bool>* cancel_requested),
              (override));

  MOCK_METHOD(orc::CVBSSinkWriteResult, write_cvbs,
              (const orc::VideoFrameRepresentation* representation,
               const orc::CVBSSinkWriteConfig& config),
              (override));
};

namespace {

const orc::ParameterDescriptor* find_descriptor(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::string& name) {
  const auto it = std::find_if(
      descriptors.begin(), descriptors.end(),
      [&name](const orc::ParameterDescriptor& d) { return d.name == name; });
  return (it != descriptors.end()) ? &*it : nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// Stage identity
// ---------------------------------------------------------------------------

TEST(CVBSSinkStageTest, StageInterfaceInvariants_MatchSink) {
  orc::CVBSSinkStage stage;
  EXPECT_EQ(stage.required_input_count(), 1u);
  EXPECT_EQ(stage.output_count(), 0u);
  EXPECT_EQ(stage.get_node_type_info().type, orc::NodeType::SINK);
}

TEST(CVBSSinkStageTest, StageName_IsCVBSSink) {
  orc::CVBSSinkStage stage;
  EXPECT_EQ(stage.get_node_type_info().stage_name, "CVBSSink");
}

TEST(CVBSSinkStageTest, DisplayName_IsCVBSSink) {
  orc::CVBSSinkStage stage;
  EXPECT_EQ(stage.get_node_type_info().display_name, "CVBS Sink");
}

// ---------------------------------------------------------------------------
// Parameter descriptors
// ---------------------------------------------------------------------------

TEST(CVBSSinkStageTest, Descriptor_OutputPathIsCompositeFile) {
  orc::CVBSSinkStage stage;
  const auto descriptors = stage.get_parameter_descriptors();

  const auto* desc = find_descriptor(descriptors, "output_path");
  ASSERT_NE(desc, nullptr);
  EXPECT_EQ(desc->type, orc::ParameterType::FILE_PATH);
  EXPECT_EQ(desc->file_extension_hint, ".cvbs");
  EXPECT_TRUE(desc->constraints.required);
  ASSERT_TRUE(desc->constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*desc->constraints.default_value), "");
}

TEST(CVBSSinkStageTest, Descriptor_SampleEncodingOffersAllOutputEncodings) {
  orc::CVBSSinkStage stage;
  const auto descriptors = stage.get_parameter_descriptors();

  const auto* desc = find_descriptor(descriptors, "sample_encoding");
  ASSERT_NE(desc, nullptr);
  EXPECT_EQ(desc->type, orc::ParameterType::STRING);
  EXPECT_FALSE(desc->constraints.required);
  ASSERT_TRUE(desc->constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*desc->constraints.default_value),
            "CVBS_U10_4FSC");
  EXPECT_THAT(
      desc->constraints.allowed_strings,
      testing::UnorderedElementsAre("CVBS_U10_4FSC", "CVBS_U16_4FSC",
                                    "CVBS_TPG21_4FSC", "CVBS_S16_4FSC"));
}

TEST(CVBSSinkStageTest, Descriptor_SignalTypeIsNotUserSelectable) {
  orc::CVBSSinkStage stage;
  const auto descriptors = stage.get_parameter_descriptors();
  EXPECT_EQ(find_descriptor(descriptors, "signal_type"), nullptr);
}

TEST(CVBSSinkStageTest, Descriptor_OutputPathHintFollowsProjectType) {
  orc::CVBSSinkStage stage;

  const auto composite_descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::PAL, orc::SourceType::Composite);
  const auto* composite_desc =
      find_descriptor(composite_descriptors, "output_path");
  ASSERT_NE(composite_desc, nullptr);
  EXPECT_EQ(composite_desc->file_extension_hint, ".cvbs");

  const auto yc_descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::PAL, orc::SourceType::YC);
  const auto* yc_desc = find_descriptor(yc_descriptors, "output_path");
  ASSERT_NE(yc_desc, nullptr);
  EXPECT_EQ(yc_desc->file_extension_hint, ".cvbsy");
}

TEST(CVBSSinkStageTest, Descriptor_CaptureNotesIsOptionalString) {
  orc::CVBSSinkStage stage;
  const auto descriptors = stage.get_parameter_descriptors();

  const auto* desc = find_descriptor(descriptors, "capture_notes");
  ASSERT_NE(desc, nullptr);
  EXPECT_EQ(desc->type, orc::ParameterType::STRING);
  EXPECT_FALSE(desc->constraints.required);
  ASSERT_TRUE(desc->constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*desc->constraints.default_value), "");
}

// ---------------------------------------------------------------------------
// Trigger validation
// ---------------------------------------------------------------------------

TEST(CVBSSinkStageTest, Trigger_FailsWhenNoInputProvided) {
  orc::CVBSSinkStage stage;
  MockObservationContext observation_context;

  const bool result = stage.trigger({}, {}, observation_context);

  EXPECT_FALSE(result);
  EXPECT_THAT(stage.get_trigger_status(),
              testing::HasSubstr("requires one input"));
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

TEST(CVBSSinkStageTest, Trigger_FailsWhenInputIsNotVideoFrameRepresentation) {
  orc::CVBSSinkStage stage;
  MockObservationContext observation_context;

  const bool result =
      stage.trigger({nullptr}, {{"output_path", std::string("out.cvbs")}},
                    observation_context);

  EXPECT_FALSE(result);
  EXPECT_THAT(stage.get_trigger_status(),
              testing::HasSubstr("VideoFrameRepresentation"));
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

TEST(CVBSSinkStageTest, Trigger_FailsWhenOutputPathMissing) {
  orc::CVBSSinkStage stage;
  MockObservationContext observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  const bool result = stage.trigger({vfr}, {}, observation_context);

  EXPECT_FALSE(result);
  EXPECT_THAT(stage.get_trigger_status(),
              testing::HasSubstr("output_path parameter is required"));
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

TEST(CVBSSinkStageTest, Trigger_FailsWithoutCallingDepsOnUnknownEncoding) {
  orc::CVBSSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockCVBSSinkStageDeps>>();
  stage.set_deps_override(deps);

  MockObservationContext observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  const bool result =
      stage.trigger({vfr},
                    {{"output_path", std::string("out.cvbs")},
                     {"sample_encoding", std::string("RAW_S16_40M")}},
                    observation_context);

  EXPECT_FALSE(result);
  EXPECT_THAT(stage.get_trigger_status(),
              testing::HasSubstr("Unsupported sample encoding"));
}

// ---------------------------------------------------------------------------
// Trigger → deps seam
// ---------------------------------------------------------------------------

TEST(CVBSSinkStageTest, Trigger_PassesDefaultConfigToDeps) {
  orc::CVBSSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockCVBSSinkStageDeps>>();
  stage.set_deps_override(deps);

  MockObservationContext observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  orc::CVBSSinkWriteConfig seen;
  EXPECT_CALL(*deps, write_cvbs(vfr.get(), _))
      .WillOnce([&seen](const orc::VideoFrameRepresentation*,
                        const orc::CVBSSinkWriteConfig& config) {
        seen = config;
        return orc::CVBSSinkWriteResult{true, 42, "Success: 42 frames written"};
      });

  const bool result = stage.trigger(
      {vfr}, {{"output_path", std::string("out.cvbs")}}, observation_context);

  EXPECT_TRUE(result);
  EXPECT_EQ(stage.get_trigger_status(), "Success: 42 frames written");
  EXPECT_EQ(seen.output_base_path, "out.cvbs");
  EXPECT_EQ(seen.sample_encoding, orc::CVBSSampleEncoding::U10_4FSC);
  EXPECT_EQ(seen.signal_type, "composite");
  EXPECT_EQ(seen.capture_notes, "");
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

TEST(CVBSSinkStageTest, Trigger_PassesSelectedParametersToDeps) {
  orc::CVBSSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockCVBSSinkStageDeps>>();
  stage.set_deps_override(deps);

  MockObservationContext observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  orc::CVBSSinkWriteConfig seen;
  EXPECT_CALL(*deps, write_cvbs(vfr.get(), _))
      .WillOnce([&seen](const orc::VideoFrameRepresentation*,
                        const orc::CVBSSinkWriteConfig& config) {
        seen = config;
        return orc::CVBSSinkWriteResult{true, 1, "Success: 1 frames written"};
      });

  const bool result = stage.trigger(
      {vfr},
      {{"output_path", std::string("capture")},
       {"sample_encoding", std::string("CVBS_S16_4FSC")},
       {"capture_notes", std::string("stacked from three captures")}},
      observation_context);

  EXPECT_TRUE(result);
  EXPECT_EQ(seen.output_base_path, "capture");
  EXPECT_EQ(seen.sample_encoding, orc::CVBSSampleEncoding::S16_4FSC);
  EXPECT_EQ(seen.signal_type, "composite");
  EXPECT_EQ(seen.capture_notes, "stacked from three captures");
}

TEST(CVBSSinkStageTest, Trigger_DerivesYCSignalTypeFromInput) {
  // The mock's has_separate_channels() defaults to false (composite); this
  // local subclass models a Y/C project's representation.
  class YCMockVFR : public NiceMock<MockVideoFrameRepresentationArtifact> {
   public:
    bool has_separate_channels() const override { return true; }
  };

  orc::CVBSSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockCVBSSinkStageDeps>>();
  stage.set_deps_override(deps);

  MockObservationContext observation_context;
  auto vfr = std::make_shared<YCMockVFR>();

  orc::CVBSSinkWriteConfig seen;
  EXPECT_CALL(*deps, write_cvbs(vfr.get(), _))
      .WillOnce([&seen](const orc::VideoFrameRepresentation*,
                        const orc::CVBSSinkWriteConfig& config) {
        seen = config;
        return orc::CVBSSinkWriteResult{true, 1, "Success: 1 frames written"};
      });

  const bool result = stage.trigger(
      {vfr}, {{"output_path", std::string("capture")}}, observation_context);

  EXPECT_TRUE(result);
  EXPECT_EQ(seen.signal_type, "yc");
}

TEST(CVBSSinkStageTest, Trigger_UsesDepsSeamAndPropagatesFailure) {
  orc::CVBSSinkStage stage;
  auto deps = std::make_shared<StrictMock<MockCVBSSinkStageDeps>>();
  stage.set_deps_override(deps);

  MockObservationContext observation_context;
  auto vfr = std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  EXPECT_CALL(*deps, write_cvbs(vfr.get(), _))
      .WillOnce(
          Return(orc::CVBSSinkWriteResult{false, 0, "Cancelled by user"}));

  const bool result = stage.trigger(
      {vfr}, {{"output_path", std::string("out.cvbs")}}, observation_context);

  EXPECT_FALSE(result);
  EXPECT_EQ(stage.get_trigger_status(), "Cancelled by user");
  EXPECT_FALSE(stage.is_trigger_in_progress());
}

// ---------------------------------------------------------------------------
// Sample encoding helpers
// ---------------------------------------------------------------------------

TEST(CVBSSinkEncodeTest, ParseSampleEncoding_AcceptsAllOutputEncodings) {
  EXPECT_EQ(orc::parse_cvbs_sample_encoding("CVBS_U10_4FSC"),
            orc::CVBSSampleEncoding::U10_4FSC);
  EXPECT_EQ(orc::parse_cvbs_sample_encoding("CVBS_U16_4FSC"),
            orc::CVBSSampleEncoding::U16_4FSC);
  EXPECT_EQ(orc::parse_cvbs_sample_encoding("CVBS_TPG21_4FSC"),
            orc::CVBSSampleEncoding::TPG21_4FSC);
  EXPECT_EQ(orc::parse_cvbs_sample_encoding("CVBS_S16_4FSC"),
            orc::CVBSSampleEncoding::S16_4FSC);
  EXPECT_EQ(orc::parse_cvbs_sample_encoding("RAW_S16_40M"), std::nullopt);
  EXPECT_EQ(orc::parse_cvbs_sample_encoding(""), std::nullopt);
}

TEST(CVBSSinkEncodeTest, EncodingNames_RoundTripThroughParse) {
  for (const auto encoding :
       {orc::CVBSSampleEncoding::U10_4FSC, orc::CVBSSampleEncoding::U16_4FSC,
        orc::CVBSSampleEncoding::TPG21_4FSC,
        orc::CVBSSampleEncoding::S16_4FSC}) {
    EXPECT_EQ(orc::parse_cvbs_sample_encoding(
                  orc::cvbs_sample_encoding_name(encoding)),
              encoding);
  }
}

TEST(CVBSSinkEncodeTest, EncodeU10_StoresBitwiseIncludingHeadroom) {
  using orc::CVBSSampleEncoding;
  // CVBS file format spec §3.1: int16_t stored bitwise; headroom preserved.
  EXPECT_EQ(orc::encode_cvbs_u10_sample(0, CVBSSampleEncoding::U10_4FSC, 256),
            0u);
  EXPECT_EQ(
      orc::encode_cvbs_u10_sample(1023, CVBSSampleEncoding::U10_4FSC, 256),
      1023u);
  // Negative headroom is stored as the two's-complement bit pattern.
  EXPECT_EQ(orc::encode_cvbs_u10_sample(-1, CVBSSampleEncoding::U10_4FSC, 256),
            0xFFFFu);
  EXPECT_EQ(
      orc::encode_cvbs_u10_sample(2000, CVBSSampleEncoding::U10_4FSC, 256),
      2000u);
}

TEST(CVBSSinkEncodeTest, EncodeU16_ScalesBy64AndClamps) {
  using orc::CVBSSampleEncoding;
  // CVBS file format spec: u16 = 10-bit value × 64.
  EXPECT_EQ(orc::encode_cvbs_u10_sample(256, CVBSSampleEncoding::U16_4FSC, 256),
            256u * 64u);
  EXPECT_EQ(
      orc::encode_cvbs_u10_sample(1023, CVBSSampleEncoding::U16_4FSC, 256),
      1023u * 64u);
  // The unsigned container cannot represent headroom; values clamp.
  EXPECT_EQ(orc::encode_cvbs_u10_sample(-5, CVBSSampleEncoding::U16_4FSC, 256),
            0u);
  EXPECT_EQ(
      orc::encode_cvbs_u10_sample(1500, CVBSSampleEncoding::U16_4FSC, 256),
      1023u * 64u);
}

TEST(CVBSSinkEncodeTest, EncodeTPG21_UsesDeviceOffsetAndClampsToLegalRange) {
  using orc::CVBSSampleEncoding;
  // Sample encoding preset spec: int16 = (value − 508) × 64.
  EXPECT_EQ(
      orc::encode_cvbs_u10_sample(508, CVBSSampleEncoding::TPG21_4FSC, 256),
      0u);
  EXPECT_EQ(
      orc::encode_cvbs_u10_sample(1019, CVBSSampleEncoding::TPG21_4FSC, 256),
      32704u);  // (1019 − 508) × 64, spec's maximum legal encoded value
  // Clamps to the legal [4, 1019] domain before encoding.
  const uint16_t sync_tip =
      orc::encode_cvbs_u10_sample(4, CVBSSampleEncoding::TPG21_4FSC, 256);
  EXPECT_EQ(orc::encode_cvbs_u10_sample(0, CVBSSampleEncoding::TPG21_4FSC, 256),
            sync_tip);
  EXPECT_EQ(
      orc::encode_cvbs_u10_sample(1023, CVBSSampleEncoding::TPG21_4FSC, 256),
      32704u);
}

TEST(CVBSSinkEncodeTest, EncodeS164FSC_UsesBlankingOffsetAndScalesBy32) {
  using orc::CVBSSampleEncoding;
  // Sample encoding preset spec encoded level examples (PAL, blanking 256):
  // blanking → 0, black (282) → 832, white (844) → 18816, peak → 24416.
  EXPECT_EQ(orc::encode_cvbs_u10_sample(256, CVBSSampleEncoding::S16_4FSC, 256),
            0u);
  EXPECT_EQ(orc::encode_cvbs_u10_sample(282, CVBSSampleEncoding::S16_4FSC, 256),
            832u);
  EXPECT_EQ(orc::encode_cvbs_u10_sample(844, CVBSSampleEncoding::S16_4FSC, 256),
            18816u);
  EXPECT_EQ(
      orc::encode_cvbs_u10_sample(1019, CVBSSampleEncoding::S16_4FSC, 256),
      24416u);
  // NTSC blanking 240: sync tip (16) → −7168 (two's complement bit pattern).
  EXPECT_EQ(orc::encode_cvbs_u10_sample(16, CVBSSampleEncoding::S16_4FSC, 240),
            static_cast<uint16_t>(static_cast<int16_t>(-7168)));
}

TEST(CVBSSinkEncodeTest, DeriveOutputBase_StripsKnownPayloadExtensions) {
  EXPECT_EQ(orc::derive_cvbs_output_base("/tmp/out.cvbs"), "/tmp/out");
  EXPECT_EQ(orc::derive_cvbs_output_base("/tmp/out.cvbsy"), "/tmp/out");
  EXPECT_EQ(orc::derive_cvbs_output_base("/tmp/out.cvbsc"), "/tmp/out");
  EXPECT_EQ(orc::derive_cvbs_output_base("/tmp/out"), "/tmp/out");
  EXPECT_EQ(orc::derive_cvbs_output_base("/tmp/out.meta"), "/tmp/out.meta");
  // Only the current payload extensions are stripped; the superseded ones are
  // left in place as an ordinary part of the base name.
  EXPECT_EQ(orc::derive_cvbs_output_base("/tmp/out.composite"),
            "/tmp/out.composite");
  EXPECT_EQ(orc::derive_cvbs_output_base("/tmp/out.y"), "/tmp/out.y");
  EXPECT_EQ(orc::derive_cvbs_output_base("/tmp/out.c"), "/tmp/out.c");
}

// ---------------------------------------------------------------------------
// Container-format helpers (CVBS file format spec v1.4.0)
// ---------------------------------------------------------------------------

namespace {

uint16_t le16_at(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint16_t>(bytes[offset]) |
         (static_cast<uint16_t>(bytes[offset + 1]) << 8);
}

uint32_t le32_at(const std::vector<uint8_t>& bytes, size_t offset) {
  return static_cast<uint32_t>(bytes[offset]) |
         (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

}  // namespace

TEST(CVBSSinkContainerTest, AudioPairPath_UsesSingleDigitSuffix) {
  // CVBS file format spec v1.4.0: <basename>_audio_<channel_pair>.wav with
  // a single-digit channel pair number 0–7.
  EXPECT_EQ(orc::cvbs_audio_pair_path("/tmp/out", 0), "/tmp/out_audio_0.wav");
  EXPECT_EQ(orc::cvbs_audio_pair_path("/tmp/out", 7), "/tmp/out_audio_7.wav");
}

TEST(CVBSSinkContainerTest, WavHeader_Is24Bit48kStereoPcm) {
  const uint32_t data_bytes = 1920 * 6;  // one PAL frame of stereo pairs
  const auto header = orc::make_cvbs_audio_wav_header(data_bytes);
  ASSERT_EQ(header.size(), 44u);

  // RIFF/WAVE framing.
  EXPECT_EQ(std::string(header.begin(), header.begin() + 4), "RIFF");
  EXPECT_EQ(le32_at(header, 4), 36u + data_bytes);
  EXPECT_EQ(std::string(header.begin() + 8, header.begin() + 12), "WAVE");
  EXPECT_EQ(std::string(header.begin() + 12, header.begin() + 16), "fmt ");
  EXPECT_EQ(le32_at(header, 16), 16u);  // canonical fmt chunk, no extension

  // CVBS file format spec v1.4.0 WAV properties: PCM, 2 channels, 48000 Hz,
  // 24-bit (block align 6, byte rate 288000).
  EXPECT_EQ(le16_at(header, 20), 1u);       // wFormatTag = PCM
  EXPECT_EQ(le16_at(header, 22), 2u);       // nChannels
  EXPECT_EQ(le32_at(header, 24), 48000u);   // nSamplesPerSec
  EXPECT_EQ(le32_at(header, 28), 288000u);  // nAvgBytesPerSec
  EXPECT_EQ(le16_at(header, 32), 6u);       // nBlockAlign
  EXPECT_EQ(le16_at(header, 34), 24u);      // wBitsPerSample

  EXPECT_EQ(std::string(header.begin() + 36, header.begin() + 40), "data");
  EXPECT_EQ(le32_at(header, 40), data_bytes);
}

TEST(CVBSSinkContainerTest, MetaSchema_UsesUserVersion11AndChannelPairTable) {
  const std::string schema = orc::kCVBSCoreMetaSchemaSql;
  // CVBS file format spec v1.6.0 metadata schema.
  EXPECT_NE(schema.find("PRAGMA user_version = 11"), std::string::npos);
  EXPECT_NE(schema.find("sequence_continuous         BOOLEAN"),
            std::string::npos);
  // v1.6.0 renamed the TBC preset-name token to STABLE; the old names must
  // not be admitted by the CHECK constraint.
  EXPECT_NE(schema.find("'STANDARD_STABLE_LOCKED'"), std::string::npos);
  EXPECT_NE(schema.find("'NONSTANDARD_STABLE_UNLOCKED'"), std::string::npos);
  EXPECT_EQ(schema.find("TBC_LOCKED"), std::string::npos);
  EXPECT_EQ(schema.find("TBC_UNLOCKED"), std::string::npos);
  EXPECT_NE(schema.find("CREATE TABLE audio_channel_pair"), std::string::npos);
  EXPECT_NE(schema.find("channel_pair                INTEGER PRIMARY KEY"),
            std::string::npos);
  EXPECT_NE(schema.find("CHECK (channel_pair BETWEEN 0 AND 7)"),
            std::string::npos);
  // The legacy v1.2.0 audio_track table (with its audio_locked column) is
  // gone.
  EXPECT_EQ(schema.find("audio_track"), std::string::npos);
  EXPECT_EQ(schema.find("audio_locked"), std::string::npos);
}

TEST(CVBSSinkContainerTest, PackS24LE_WritesLittleEndian24BitWords) {
  const std::vector<int32_t> audio{0x123456, -0x123456, 0, -1};
  const auto bytes = orc::pack_audio_s24le(audio, audio.size());
  ASSERT_EQ(bytes.size(), audio.size() * 3);
  // 0x123456 → 56 34 12.
  EXPECT_EQ(bytes[0], 0x56u);
  EXPECT_EQ(bytes[1], 0x34u);
  EXPECT_EQ(bytes[2], 0x12u);
  // −0x123456 = 0xFFEDCBAA → AA CB ED.
  EXPECT_EQ(bytes[3], 0xAAu);
  EXPECT_EQ(bytes[4], 0xCBu);
  EXPECT_EQ(bytes[5], 0xEDu);
  // 0 → 00 00 00.
  EXPECT_EQ(bytes[6], 0x00u);
  EXPECT_EQ(bytes[7], 0x00u);
  EXPECT_EQ(bytes[8], 0x00u);
  // −1 → FF FF FF.
  EXPECT_EQ(bytes[9], 0xFFu);
  EXPECT_EQ(bytes[10], 0xFFu);
  EXPECT_EQ(bytes[11], 0xFFu);
}

TEST(CVBSSinkContainerTest, PackS24LE_SaturatesToThe24BitRange) {
  const std::vector<int32_t> audio{8388608, -8388609, 8388607, -8388608};
  const auto bytes = orc::pack_audio_s24le(audio, audio.size());
  ASSERT_EQ(bytes.size(), 12u);
  // Above-range saturates to 8388607 (0x7FFFFF).
  EXPECT_EQ(bytes[0], 0xFFu);
  EXPECT_EQ(bytes[1], 0xFFu);
  EXPECT_EQ(bytes[2], 0x7Fu);
  // Below-range saturates to −8388608 (0x800000).
  EXPECT_EQ(bytes[3], 0x00u);
  EXPECT_EQ(bytes[4], 0x00u);
  EXPECT_EQ(bytes[5], 0x80u);
  // Range extremes pass through unchanged.
  EXPECT_EQ(bytes[8], 0x7Fu);
  EXPECT_EQ(bytes[11], 0x80u);
}

TEST(CVBSSinkContainerTest, PackS24LE_PadsShortInputWithSilence) {
  const std::vector<int32_t> audio{0x010203};
  const auto bytes = orc::pack_audio_s24le(audio, 3);
  ASSERT_EQ(bytes.size(), 9u);
  EXPECT_EQ(bytes[0], 0x03u);
  EXPECT_EQ(bytes[1], 0x02u);
  EXPECT_EQ(bytes[2], 0x01u);
  for (size_t i = 3; i < bytes.size(); ++i) {
    EXPECT_EQ(bytes[i], 0x00u) << "byte " << i;
  }
}

TEST(CVBSSinkContainerTest, PackS24LE_TruncatesLongInput) {
  const std::vector<int32_t> audio{1, 2, 3, 4};
  const auto bytes = orc::pack_audio_s24le(audio, 2);
  ASSERT_EQ(bytes.size(), 6u);
  EXPECT_EQ(bytes[0], 0x01u);
  EXPECT_EQ(bytes[3], 0x02u);
}
}  // namespace orc_unit_test
