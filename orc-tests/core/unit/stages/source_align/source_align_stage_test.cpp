/*
 * File:        source_align_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for SourceAlignStage (VFrameR)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/source_align/source_align_stage.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/support/frame_line_util.h>

#include <algorithm>
#include <cstdint>

#include "../../include/video_frame_representation_artifact_mock.h"

using ::testing::NiceMock;
using ::testing::Return;

namespace orc_unit_test {
namespace {

const orc::ParameterDescriptor* find_descriptor(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::string& name) {
  auto it = std::find_if(
      descriptors.begin(), descriptors.end(),
      [&](const orc::ParameterDescriptor& d) { return d.name == name; });
  return it == descriptors.end() ? nullptr : &(*it);
}

}  // namespace

TEST(SourceAlignStageTest, RequiredInputCount_IsOne) {
  orc::SourceAlignStage stage;
  EXPECT_EQ(stage.required_input_count(), 1u);
}

TEST(SourceAlignStageTest, OutputCount_IsUnboundedSentinel) {
  orc::SourceAlignStage stage;
  EXPECT_EQ(stage.output_count(), static_cast<size_t>(UINT32_MAX));
}

TEST(SourceAlignStageTest, NodeTypeInfo_HasExpectedMetadata) {
  orc::SourceAlignStage stage;
  auto info = stage.get_node_type_info();
  EXPECT_EQ(info.type, orc::NodeType::COMPLEX);
  EXPECT_EQ(info.stage_name, "source_align");
  EXPECT_EQ(info.compatible_formats, orc::VideoFormatCompatibility::ALL);
}

TEST(SourceAlignStageTest, Descriptor_DefaultsMatchRuntimeDefaults) {
  orc::SourceAlignStage stage;
  const auto descriptors = stage.get_parameter_descriptors();
  const auto params = stage.get_parameters();

  const auto* alignment_map = find_descriptor(descriptors, "alignmentMap");
  ASSERT_NE(alignment_map, nullptr);
  ASSERT_TRUE(alignment_map->constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*alignment_map->constraints.default_value),
            std::get<std::string>(params.at("alignmentMap")));
}

TEST(SourceAlignStageTest, SetParameters_AcceptsAlignmentMapString) {
  orc::SourceAlignStage stage;
  EXPECT_TRUE(stage.set_parameters({{"alignmentMap", std::string("1+2,2+4")}}));
  const auto params = stage.get_parameters();
  EXPECT_EQ(std::get<std::string>(params.at("alignmentMap")), "1+2,2+4");
}

TEST(SourceAlignStageTest, SetParameters_RejectsUnknownParameter) {
  orc::SourceAlignStage stage;
  EXPECT_FALSE(stage.set_parameters({{"unknown", true}}));
}

TEST(SourceAlignStageTest, Execute_ThrowsWhenNoInputsProvided) {
  orc::SourceAlignStage stage;
  orc::ObservationContext ctx;
  EXPECT_THROW(stage.execute({}, {}, ctx), orc::DAGExecutionError);
}

TEST(SourceAlignStageTest, Execute_ThrowsWhenTooManyInputsProvided) {
  orc::SourceAlignStage stage;
  orc::ObservationContext ctx;
  std::vector<orc::ArtifactPtr> inputs(17);
  EXPECT_THROW(stage.execute(inputs, {}, ctx), orc::DAGExecutionError);
}

TEST(SourceAlignStageTest, Execute_ThrowsWhenInputIsWrongType) {
  struct FakeArt : public orc::Artifact {
    FakeArt() : Artifact(orc::ArtifactID("x"), orc::Provenance{}) {}
    std::string type_name() const override { return "x"; }
  };
  orc::SourceAlignStage stage;
  orc::ObservationContext ctx;
  EXPECT_THROW(stage.execute({std::make_shared<FakeArt>()}, {}, ctx),
               orc::DAGExecutionError);
}

TEST(SourceAlignStageTest, Execute_ThrowsWhenManualAlignmentMapIsInvalid) {
  orc::SourceAlignStage stage;
  orc::ObservationContext ctx;
  auto source =
      std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  ON_CALL(*source, type_name()).WillByDefault(Return("test_vfr_artifact"));

  EXPECT_THROW(
      stage.execute({source}, {{"alignmentMap", std::string("invalid")}}, ctx),
      orc::DAGExecutionError);
}

TEST(SourceAlignStageTest, Execute_PassesThroughSingleSourceWithZeroOffset) {
  orc::SourceAlignStage stage;
  orc::ObservationContext ctx;
  auto source =
      std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  ON_CALL(*source, type_name()).WillByDefault(Return("test_vfr_artifact"));
  ON_CALL(*source, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0u, 9u}));
  ON_CALL(*source, frame_count()).WillByDefault(Return(10u));

  const auto outputs = stage.execute({source}, {}, ctx);

  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_EQ(outputs[0].get(), source.get());  // pass-through pointer equality
}

TEST(SourceAlignStageTest, Execute_AppliesManualOffset_InFirstCommonFrameMode) {
  orc::SourceAlignStage stage;
  orc::ObservationContext ctx;
  auto source =
      std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  ON_CALL(*source, type_name()).WillByDefault(Return("test_vfr_artifact"));
  ON_CALL(*source, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0u, 9u}));  // 10 frames
  ON_CALL(*source, frame_count()).WillByDefault(Return(10u));

  ASSERT_TRUE(stage.set_parameters(
      {{"alignmentMode", std::string("first_common_frame")},
       {"alignmentMap", std::string("1+2")}}));

  const auto outputs = stage.execute({source}, {}, ctx);

  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_NE(outputs[0].get(), source.get());

  // Skip offset 2: output should have 10 - 2 = 8 frames.
  auto vfr =
      std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(outputs[0]);
  ASSERT_NE(vfr, nullptr);
  EXPECT_EQ(vfr->frame_count(), 8u);
}

TEST(SourceAlignStageTest,
     Execute_PrependsPaddingFrames_InPadForAlignmentMode) {
  orc::SourceAlignStage stage;
  orc::ObservationContext ctx;
  auto source =
      std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();

  ON_CALL(*source, type_name()).WillByDefault(Return("test_vfr_artifact"));
  ON_CALL(*source, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0u, 9u}));  // 10 frames
  ON_CALL(*source, frame_count()).WillByDefault(Return(10u));

  ASSERT_TRUE(
      stage.set_parameters({{"alignmentMode", std::string("pad_for_alignment")},
                            {"alignmentMap", std::string("1+3")}}));

  const auto outputs = stage.execute({source}, {}, ctx);

  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_NE(outputs[0].get(), source.get());

  // Pad prefix 3: output should have 3 + 10 = 13 frames.
  auto vfr =
      std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(outputs[0]);
  ASSERT_NE(vfr, nullptr);
  EXPECT_EQ(vfr->frame_count(), 13u);

  // First 3 frames must be marked as padding.
  for (orc::FrameID fid = 0; fid < 3; ++fid) {
    EXPECT_TRUE(vfr->has_frame(fid));
    auto desc = vfr->get_frame_descriptor(fid);
    ASSERT_TRUE(desc.has_value()) << "no descriptor for padding frame " << fid;
    EXPECT_TRUE(desc->is_padding_frame) << "frame " << fid << " not padding";
  }
}

// ── Audio channel pairs through the alignment decorators ────────────────────

namespace {

// Interleaved stereo samples for source frame |frame|: stereo pair p carries
// the value frame × 10000 + p on both channels, sized to the frame's native
// SMPTE 272M-1994 §14.3 cadence count — so any output sample is traceable to
// its source frame and in-frame pair position.
std::vector<int32_t> frame_audio(uint64_t frame, orc::VideoSystem system) {
  const uint32_t pairs = orc::audio_pairs_in_frame(frame, system);
  std::vector<int32_t> samples;
  samples.reserve(static_cast<size_t>(pairs) * 2);
  for (uint32_t p = 0; p < pairs; ++p) {
    const auto value = static_cast<int32_t>(frame * 10000 + p);
    samples.push_back(value);
    samples.push_back(value);
  }
  return samples;
}

// 10-frame mocked source carrying one audio channel pair whose per-frame
// samples come from frame_audio(); no filesystem or real media involved.
std::shared_ptr<NiceMock<MockVideoFrameRepresentationArtifact>>
make_audio_source(orc::VideoSystem system) {
  auto source =
      std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();
  ON_CALL(*source, type_name()).WillByDefault(Return("test_vfr_artifact"));
  ON_CALL(*source, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0u, 9u}));
  ON_CALL(*source, frame_count()).WillByDefault(Return(10u));
  orc::SourceParameters params;
  params.system = system;
  params.frame_width_nominal = 4;
  params.frame_height = 4;
  params.blanking_level = 256;
  ON_CALL(*source, get_video_parameters()).WillByDefault(Return(params));

  ON_CALL(*source, audio_channel_pair_count()).WillByDefault(Return(1u));
  ON_CALL(*source, get_audio_channel_pair_descriptor(0))
      .WillByDefault(Return(orc::AudioChannelPairDescriptor{
          "Analogue", orc::AudioOrigin::ANALOGUE}));
  ON_CALL(*source, get_audio_samples(0, ::testing::_))
      .WillByDefault(::testing::Invoke([system](size_t, orc::FrameID id) {
        return frame_audio(id, system);
      }));
  return source;
}

// Runs the stage over one source and returns the wrapped output.
std::shared_ptr<orc::VideoFrameRepresentation> align_one(
    orc::SourceAlignStage& stage, orc::ArtifactPtr source,
    const std::string& mode, const std::string& map) {
  orc::ObservationContext ctx;
  EXPECT_TRUE(
      stage.set_parameters({{"alignmentMode", mode}, {"alignmentMap", map}}));
  auto outputs = stage.execute({std::move(source)}, {}, ctx);
  EXPECT_EQ(outputs.size(), 1u);
  return std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(outputs[0]);
}

}  // namespace

TEST(SourceAlignStageTest, TrimMode_PairCountAndDescriptorForwardFromSource) {
  orc::SourceAlignStage stage;
  auto source = make_audio_source(orc::VideoSystem::PAL);

  auto aligned = align_one(stage, source, "first_common_frame", "1+2");
  ASSERT_NE(aligned, nullptr);

  EXPECT_EQ(aligned->audio_channel_pair_count(), 1u);
  const auto desc = aligned->get_audio_channel_pair_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->name, "Analogue");
  EXPECT_EQ(desc->origin, orc::AudioOrigin::ANALOGUE);
}

TEST(SourceAlignStageTest, TrimMode_PalAudioFollowsShiftedFramesSampleExact) {
  orc::SourceAlignStage stage;
  auto source = make_audio_source(orc::VideoSystem::PAL);

  // PAL is constant-cadence (1920 pairs per frame), so any trim offset is
  // sample-exact: output frame 0 serves source frame 2's audio untouched.
  auto aligned = align_one(stage, source, "first_common_frame", "1+2");
  ASSERT_NE(aligned, nullptr);

  const auto samples = aligned->get_audio_samples(0, orc::FrameID{0});
  EXPECT_EQ(samples, frame_audio(2, orc::VideoSystem::PAL));
  EXPECT_EQ(samples.size(), 1920u * 2u);
}

TEST(SourceAlignStageTest, TrimMode_PhasePreservingNtscOffsetIsSampleExact) {
  orc::SourceAlignStage stage;
  auto source = make_audio_source(orc::VideoSystem::NTSC);

  // Offset 5 preserves the SMPTE 272M five-frame audio sequence phase, so
  // every shifted frame is served sample-exact.
  auto aligned = align_one(stage, source, "first_common_frame", "1+5");
  ASSERT_NE(aligned, nullptr);

  for (uint64_t p = 0; p < 5; ++p) {
    EXPECT_EQ(aligned->get_audio_samples(0, orc::FrameID{p}),
              frame_audio(p + 5, orc::VideoSystem::NTSC))
        << "output frame " << p;
  }
}

TEST(SourceAlignStageTest, TrimMode_PhaseBreakingNtscOffsetObeysOnePairRule) {
  orc::SourceAlignStage stage;
  auto source = make_audio_source(orc::VideoSystem::NTSC);

  // Offset 1 breaks the SMPTE 272M-1994 §14.3 five-frame sequence phase.
  auto aligned = align_one(stage, source, "first_common_frame", "1+1");
  ASSERT_NE(aligned, nullptr);

  // Output frame 0 needs 1602 pairs; source frame 1 natively carries 1601 —
  // one trailing silence pair is appended.
  const auto padded = aligned->get_audio_samples(0, orc::FrameID{0});
  ASSERT_EQ(padded.size(), 1602u * 2u);
  const auto native1 = frame_audio(1, orc::VideoSystem::NTSC);
  EXPECT_TRUE(std::equal(native1.begin(), native1.end(), padded.begin()));
  EXPECT_EQ(padded[padded.size() - 2], 0);
  EXPECT_EQ(padded[padded.size() - 1], 0);

  // Output frame 1 needs 1601 pairs; source frame 2 natively carries 1602 —
  // the trailing pair is truncated.
  const auto truncated = aligned->get_audio_samples(0, orc::FrameID{1});
  ASSERT_EQ(truncated.size(), 1601u * 2u);
  auto expected = frame_audio(2, orc::VideoSystem::NTSC);
  expected.resize(1601u * 2u);
  EXPECT_EQ(truncated, expected);
}

TEST(SourceAlignStageTest, PadMode_PaddingFramesCarryCadenceSizedSilence) {
  orc::SourceAlignStage stage;
  auto source = make_audio_source(orc::VideoSystem::NTSC);

  auto padded = align_one(stage, source, "pad_for_alignment", "1+2");
  ASSERT_NE(padded, nullptr);

  // Silence blocks are sized by the OUTPUT frame's cadence position
  // (SMPTE 272M-1994 §14.3): 1602 pairs at index 0, 1601 at index 1.
  const auto silence0 = padded->get_audio_samples(0, orc::FrameID{0});
  EXPECT_EQ(silence0.size(), 1602u * 2u);
  EXPECT_TRUE(std::all_of(silence0.begin(), silence0.end(),
                          [](int32_t s) { return s == 0; }));
  const auto silence1 = padded->get_audio_samples(0, orc::FrameID{1});
  EXPECT_EQ(silence1.size(), 1601u * 2u);
  EXPECT_TRUE(std::all_of(silence1.begin(), silence1.end(),
                          [](int32_t s) { return s == 0; }));

  // Out-of-range pairs stay empty, including on padding frames.
  EXPECT_TRUE(padded->get_audio_samples(1, orc::FrameID{0}).empty());
}

TEST(SourceAlignStageTest, PadMode_ShiftedRealFramesObeyOnePairRule) {
  orc::SourceAlignStage stage;
  auto source = make_audio_source(orc::VideoSystem::NTSC);

  // Pad count 1 breaks the SMPTE 272M-1994 §14.3 five-frame sequence phase.
  auto padded = align_one(stage, source, "pad_for_alignment", "1+1");
  ASSERT_NE(padded, nullptr);

  // Output frame 1 needs 1601 pairs; source frame 0 natively carries 1602 —
  // the trailing pair is truncated.
  const auto truncated = padded->get_audio_samples(0, orc::FrameID{1});
  ASSERT_EQ(truncated.size(), 1601u * 2u);
  auto expected = frame_audio(0, orc::VideoSystem::NTSC);
  expected.resize(1601u * 2u);
  EXPECT_EQ(truncated, expected);

  // Output frame 2 needs 1602 pairs; source frame 1 natively carries 1601 —
  // one trailing silence pair is appended.
  const auto appended = padded->get_audio_samples(0, orc::FrameID{2});
  ASSERT_EQ(appended.size(), 1602u * 2u);
  const auto native1 = frame_audio(1, orc::VideoSystem::NTSC);
  EXPECT_TRUE(std::equal(native1.begin(), native1.end(), appended.begin()));
  EXPECT_EQ(appended[appended.size() - 2], 0);
  EXPECT_EQ(appended[appended.size() - 1], 0);
}

// ── Dropout hint forwarding (issue #216, plan Phase 3 Task 3.2) ─────────────
// The alignment wrappers must forward the dropout hints of their *input*
// (translated to the wrapper's frame IDs), so any upstream dropout_map edit is
// visible to a downstream analysis sink.

TEST(SourceAlignStageTest, TrimMode_ForwardsDropoutHintsFromShiftedSource) {
  orc::SourceAlignStage stage;
  auto source =
      std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();
  ON_CALL(*source, type_name()).WillByDefault(Return("test_vfr_artifact"));
  ON_CALL(*source, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0u, 9u}));
  ON_CALL(*source, frame_count()).WillByDefault(Return(10u));
  // Output frame 0 maps to source frame 2 (offset 2).
  ON_CALL(*source, get_dropout_hints(orc::FrameID{2}))
      .WillByDefault(Return(
          std::vector<orc::DropoutRun>{{orc::FrameID{2}, 1000u, 50u, 128}}));

  auto aligned = align_one(stage, source, "first_common_frame", "1+2");
  ASSERT_NE(aligned, nullptr);

  const auto hints = aligned->get_dropout_hints(orc::FrameID{0});
  ASSERT_EQ(hints.size(), 1u);
  EXPECT_EQ(hints[0].sample_start, 1000u);
  EXPECT_EQ(hints[0].sample_count, 50u);
  // Frame ID is rewritten to this representation's output frame.
  EXPECT_EQ(hints[0].frame_id, orc::FrameID{0});
}

TEST(SourceAlignStageTest,
     PadMode_ForwardsSourceHintsForRealFrames_EmptyForPadding) {
  orc::SourceAlignStage stage;
  auto source =
      std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();
  ON_CALL(*source, type_name()).WillByDefault(Return("test_vfr_artifact"));
  ON_CALL(*source, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0u, 9u}));
  ON_CALL(*source, frame_count()).WillByDefault(Return(10u));
  // Real output frame 3 maps to source frame 0 (3 padding frames prepended).
  ON_CALL(*source, get_dropout_hints(orc::FrameID{0}))
      .WillByDefault(Return(
          std::vector<orc::DropoutRun>{{orc::FrameID{0}, 2000u, 30u, 128}}));

  auto padded = align_one(stage, source, "pad_for_alignment", "1+3");
  ASSERT_NE(padded, nullptr);

  // Synthetic padding frames carry no dropout hints.
  EXPECT_TRUE(padded->get_dropout_hints(orc::FrameID{0}).empty());

  const auto hints = padded->get_dropout_hints(orc::FrameID{3});
  ASSERT_EQ(hints.size(), 1u);
  EXPECT_EQ(hints[0].sample_start, 2000u);
  EXPECT_EQ(hints[0].sample_count, 30u);
  EXPECT_EQ(hints[0].frame_id, orc::FrameID{3});
}

TEST(SourceAlignStageTest, PadMode_PalAudioFollowsShiftedFramesSampleExact) {
  orc::SourceAlignStage stage;
  auto source = make_audio_source(orc::VideoSystem::PAL);

  auto padded = align_one(stage, source, "pad_for_alignment", "1+2");
  ASSERT_NE(padded, nullptr);

  // PAL is constant-cadence, so shifted real frames are sample-exact and
  // padding silence is the constant 1920 pairs.
  const auto silence = padded->get_audio_samples(0, orc::FrameID{1});
  ASSERT_EQ(silence.size(), 1920u * 2u);
  EXPECT_TRUE(std::all_of(silence.begin(), silence.end(),
                          [](int32_t s) { return s == 0; }));
  EXPECT_EQ(padded->get_audio_samples(0, orc::FrameID{2}),
            frame_audio(0, orc::VideoSystem::PAL));
}

// ── Padding frame geometry and tail padding ─────────────────────────────────

namespace {

// Artifact-backed mocked source of |count| frames with the given broadcast
// geometry; real frames are described as non-padding.
std::shared_ptr<NiceMock<MockVideoFrameRepresentationArtifact>>
make_geometry_source(orc::VideoSystem system, int32_t width, int32_t height,
                     uint64_t count) {
  auto source =
      std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();
  ON_CALL(*source, type_name()).WillByDefault(Return("test_vfr_artifact"));
  ON_CALL(*source, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0u, count - 1}));
  ON_CALL(*source, frame_count()).WillByDefault(Return(count));
  ON_CALL(*source, has_frame(::testing::_))
      .WillByDefault(
          ::testing::Invoke([count](orc::FrameID id) { return id < count; }));
  ON_CALL(*source, get_frame_descriptor(::testing::_))
      .WillByDefault(::testing::Invoke(
          [=](orc::FrameID id) -> std::optional<orc::FrameDescriptor> {
            if (id >= count) return std::nullopt;
            orc::FrameDescriptor desc;
            desc.frame_id = id;
            desc.system = system;
            desc.height = static_cast<size_t>(height);
            desc.samples_per_line_nominal = static_cast<size_t>(width);
            desc.samples_total = orc::frame_line_sample_offset(
                system, static_cast<size_t>(width),
                static_cast<size_t>(height));
            return desc;
          }));
  orc::SourceParameters params;
  params.system = system;
  params.frame_width_nominal = width;
  params.frame_height = height;
  params.blanking_level = 256;
  ON_CALL(*source, get_video_parameters()).WillByDefault(Return(params));
  return source;
}

// A pad frame must be a whole frame: its descriptor, its sample buffer and
// its last line all have to agree with the source geometry.
void expect_pad_frame_geometry(const orc::VideoFrameRepresentation& vfr,
                               orc::FrameID id, size_t height,
                               size_t expected_total) {
  const auto desc = vfr.get_frame_descriptor(id);
  ASSERT_TRUE(desc.has_value());
  EXPECT_TRUE(desc->is_padding_frame);
  EXPECT_EQ(desc->samples_total, expected_total);

  const auto copy = vfr.get_frame_copy(id);
  EXPECT_EQ(copy.size(), expected_total);

  const auto* frame = vfr.get_frame(id);
  ASSERT_NE(frame, nullptr);
  const auto* last_line = vfr.get_line(id, height - 1);
  ASSERT_NE(last_line, nullptr);
  const size_t width = desc->samples_per_line_nominal;
  const size_t last_count =
      orc::frame_line_sample_count(desc->system, width, height - 1);
  EXPECT_EQ(static_cast<size_t>(last_line - frame) + last_count,
            expected_total);
  EXPECT_EQ(vfr.get_line(id, height), nullptr);
}

std::vector<std::shared_ptr<orc::VideoFrameRepresentation>> align_all(
    orc::SourceAlignStage& stage, const std::vector<orc::ArtifactPtr>& sources,
    const std::string& mode, const std::string& map) {
  orc::ObservationContext ctx;
  EXPECT_TRUE(
      stage.set_parameters({{"alignmentMode", mode}, {"alignmentMap", map}}));
  std::vector<std::shared_ptr<orc::VideoFrameRepresentation>> out;
  for (const auto& a : stage.execute(sources, {}, ctx)) {
    out.push_back(std::dynamic_pointer_cast<orc::VideoFrameRepresentation>(a));
  }
  return out;
}

}  // namespace

// PAL lines 312 and 624 carry two extra samples each, so a PAL frame is
// 709,379 samples, not 625 x 1135. A short pad shifts every later frame of a
// CVBS export.
TEST(SourceAlignStageTest, PadMode_PalPaddingIsAWholeFrame) {
  orc::SourceAlignStage stage;
  auto source = make_geometry_source(orc::VideoSystem::PAL, 1135, 625, 4);
  auto padded = align_one(stage, source, "pad_for_alignment", "1+2");
  ASSERT_NE(padded, nullptr);
  expect_pad_frame_geometry(*padded, orc::FrameID{0}, 625, 709379u);
}

TEST(SourceAlignStageTest, PadMode_NtscPaddingIsHeightTimesWidth) {
  orc::SourceAlignStage stage;
  auto source = make_geometry_source(orc::VideoSystem::NTSC, 910, 525, 4);
  auto padded = align_one(stage, source, "pad_for_alignment", "1+2");
  ASSERT_NE(padded, nullptr);
  expect_pad_frame_geometry(*padded, orc::FrameID{0}, 525, 910u * 525u);
}

TEST(SourceAlignStageTest, PadMode_PalMPaddingIsHeightTimesWidth) {
  orc::SourceAlignStage stage;
  auto source = make_geometry_source(orc::VideoSystem::PAL_M, 909, 525, 4);
  auto padded = align_one(stage, source, "pad_for_alignment", "1+2");
  ASSERT_NE(padded, nullptr);
  expect_pad_frame_geometry(*padded, orc::FrameID{0}, 525, 909u * 525u);
}

// Sources of different lengths come out equal in length, so downstream
// stages keep the pictures only the longer captures carry.
TEST(SourceAlignStageTest, PadMode_PadsTheTailToTheLongestOutput) {
  orc::SourceAlignStage stage;
  auto early = make_geometry_source(orc::VideoSystem::PAL, 1135, 625, 10);
  auto late = make_geometry_source(orc::VideoSystem::PAL, 1135, 625, 6);
  auto longest = make_geometry_source(orc::VideoSystem::PAL, 1135, 625, 12);

  // Output lengths before tail padding: 10, 3 + 6 = 9, 12.
  const auto outputs =
      align_all(stage, {early, late, longest}, "pad_for_alignment",
                "1+0, 2+3, 3+0");
  ASSERT_EQ(outputs.size(), 3u);
  for (const auto& out : outputs) {
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(out->frame_count(), 12u);
    EXPECT_EQ(out->frame_range().last, orc::FrameID{11});
    EXPECT_TRUE(out->has_frame(orc::FrameID{11}));
    EXPECT_FALSE(out->has_frame(orc::FrameID{12}));
  }

  // The longest source needs no padding and passes straight through.
  EXPECT_EQ(outputs[2].get(),
            static_cast<orc::VideoFrameRepresentation*>(longest.get()));

  // Source 1: real 0-9, tail padding 10-11.
  EXPECT_FALSE(outputs[0]->get_frame_descriptor(orc::FrameID{9})
                   ->is_padding_frame);
  expect_pad_frame_geometry(*outputs[0], orc::FrameID{10}, 625, 709379u);
  expect_pad_frame_geometry(*outputs[0], orc::FrameID{11}, 625, 709379u);

  // Source 2: head padding 0-2, real 3-8, tail padding 9-11.
  expect_pad_frame_geometry(*outputs[1], orc::FrameID{2}, 625, 709379u);
  const auto real = outputs[1]->get_frame_descriptor(orc::FrameID{8});
  ASSERT_TRUE(real.has_value());
  EXPECT_FALSE(real->is_padding_frame);
  EXPECT_EQ(real->frame_id, orc::FrameID{8});
  expect_pad_frame_geometry(*outputs[1], orc::FrameID{9}, 625, 709379u);
  EXPECT_TRUE(outputs[1]->get_dropout_hints(orc::FrameID{11}).empty());
  EXPECT_EQ(outputs[1]->get_efm_sample_count(orc::FrameID{11}), 0u);
}

// Trimming mode cuts the head and leaves the tail alone.
TEST(SourceAlignStageTest, TrimMode_DoesNotPadTheTail) {
  orc::SourceAlignStage stage;
  auto a = make_geometry_source(orc::VideoSystem::PAL, 1135, 625, 10);
  auto b = make_geometry_source(orc::VideoSystem::PAL, 1135, 625, 6);

  const auto outputs =
      align_all(stage, {a, b}, "first_common_frame", "1+2, 2+0");
  ASSERT_EQ(outputs.size(), 2u);
  EXPECT_EQ(outputs[0]->frame_count(), 8u);
  EXPECT_EQ(outputs[1]->frame_count(), 6u);
}

}  // namespace orc_unit_test
