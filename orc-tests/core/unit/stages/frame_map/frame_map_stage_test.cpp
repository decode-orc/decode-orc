/*
 * File:        frame_map_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for FrameMapStage parameter defaults and validation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/plugins/stages/frame_map/frame_map_stage.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/support/frame_line_util.h>

#include <algorithm>

#include "../../include/video_frame_representation_artifact_mock.h"
#include "../../mocks/mock_video_frame_representation.h"

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

// ── NodeTypeInfo / interface contracts ──────────────────────────────────────

TEST(FrameMapStageTest, RequiredInputCount_IsOne) {
  orc::FrameMapStage stage;
  EXPECT_EQ(stage.required_input_count(), 1u);
}

TEST(FrameMapStageTest, OutputCount_IsOne) {
  orc::FrameMapStage stage;
  EXPECT_EQ(stage.output_count(), 1u);
}

TEST(FrameMapStageTest, NodeTypeInfo_HasExpectedMetadata) {
  orc::FrameMapStage stage;
  auto info = stage.get_node_type_info();
  EXPECT_EQ(info.type, orc::NodeType::TRANSFORM);
  EXPECT_EQ(info.stage_name, "frame_map");
  EXPECT_EQ(info.compatible_formats, orc::VideoFormatCompatibility::ALL);
}

// ── Parameter descriptors vs runtime defaults ────────────────────────────────

TEST(FrameMapStageTest, Descriptor_RangesMatchRuntimeDefault) {
  orc::FrameMapStage stage;
  const auto descs = stage.get_parameter_descriptors();
  const auto params = stage.get_parameters();

  const auto* d = find_descriptor(descs, "ranges");
  ASSERT_NE(d, nullptr);
  ASSERT_TRUE(d->constraints.default_value.has_value());
  ASSERT_TRUE(
      std::holds_alternative<std::string>(*d->constraints.default_value));
  ASSERT_TRUE(std::holds_alternative<std::string>(params.at("ranges")));
  EXPECT_EQ(std::get<std::string>(*d->constraints.default_value),
            std::get<std::string>(params.at("ranges")));
}

TEST(FrameMapStageTest, Descriptor_RemoveDuplicatesMatchRuntimeDefault) {
  orc::FrameMapStage stage;
  const auto descs = stage.get_parameter_descriptors();
  const auto params = stage.get_parameters();

  const auto* d = find_descriptor(descs, "remove_duplicates");
  ASSERT_NE(d, nullptr);
  ASSERT_TRUE(d->constraints.default_value.has_value());
  ASSERT_TRUE(std::holds_alternative<bool>(*d->constraints.default_value));
  ASSERT_TRUE(std::holds_alternative<bool>(params.at("remove_duplicates")));
  EXPECT_EQ(std::get<bool>(*d->constraints.default_value),
            std::get<bool>(params.at("remove_duplicates")));
}

TEST(FrameMapStageTest, Descriptor_PadGapsMatchRuntimeDefault) {
  orc::FrameMapStage stage;
  const auto descs = stage.get_parameter_descriptors();
  const auto params = stage.get_parameters();

  const auto* d = find_descriptor(descs, "pad_gaps");
  ASSERT_NE(d, nullptr);
  ASSERT_TRUE(d->constraints.default_value.has_value());
  ASSERT_TRUE(std::holds_alternative<bool>(*d->constraints.default_value));
  EXPECT_EQ(std::get<bool>(*d->constraints.default_value),
            std::get<bool>(params.at("pad_gaps")));
}

TEST(FrameMapStageTest, Descriptor_PadStrategyMatchRuntimeDefault) {
  orc::FrameMapStage stage;
  const auto descs = stage.get_parameter_descriptors();
  const auto params = stage.get_parameters();

  const auto* d = find_descriptor(descs, "pad_strategy");
  ASSERT_NE(d, nullptr);
  ASSERT_TRUE(d->constraints.default_value.has_value());
  ASSERT_TRUE(
      std::holds_alternative<std::string>(*d->constraints.default_value));
  EXPECT_EQ(std::get<std::string>(*d->constraints.default_value),
            std::get<std::string>(params.at("pad_strategy")));
}

// ── set_parameters validation ─────────────────────────────────────────────

TEST(FrameMapStageTest, SetParameters_AcceptsValidRanges) {
  orc::FrameMapStage stage;
  EXPECT_TRUE(stage.set_parameters({{"ranges", std::string("0-10,20-30")}}));
  EXPECT_EQ(std::get<std::string>(stage.get_parameters().at("ranges")),
            "0-10,20-30");
}

TEST(FrameMapStageTest, SetParameters_AcceptsEmptyRanges) {
  orc::FrameMapStage stage;
  EXPECT_TRUE(stage.set_parameters({{"ranges", std::string("")}}));
}

TEST(FrameMapStageTest, SetParameters_AcceptsRemoveDuplicatesTrue) {
  orc::FrameMapStage stage;
  EXPECT_TRUE(stage.set_parameters({{"remove_duplicates", true}}));
  EXPECT_TRUE(std::get<bool>(stage.get_parameters().at("remove_duplicates")));
}

TEST(FrameMapStageTest, SetParameters_AcceptsPadGapsTrue) {
  orc::FrameMapStage stage;
  EXPECT_TRUE(stage.set_parameters({{"pad_gaps", true}}));
  EXPECT_TRUE(std::get<bool>(stage.get_parameters().at("pad_gaps")));
}

TEST(FrameMapStageTest, SetParameters_AcceptsBlackPadStrategy) {
  orc::FrameMapStage stage;
  EXPECT_TRUE(stage.set_parameters({{"pad_strategy", std::string("black")}}));
  EXPECT_EQ(std::get<std::string>(stage.get_parameters().at("pad_strategy")),
            "black");
}

TEST(FrameMapStageTest, SetParameters_CoercesLegacyNearestToBlack) {
  // "nearest" was always a no-op that rendered black; it survives only as a
  // deprecated alias so older project files still load.
  orc::FrameMapStage stage;
  EXPECT_TRUE(stage.set_parameters({{"pad_strategy", std::string("nearest")}}));
  EXPECT_EQ(std::get<std::string>(stage.get_parameters().at("pad_strategy")),
            "black");
}

TEST(FrameMapStageTest, SetParameters_RejectsInvalidPadStrategy) {
  orc::FrameMapStage stage;
  EXPECT_FALSE(stage.set_parameters({{"pad_strategy", std::string("bogus")}}));
}

// ── execute() error handling ───────────────────────────────────────────────

TEST(FrameMapStageTest, Execute_ThrowsWhenInputMissing) {
  orc::FrameMapStage stage;
  orc::ObservationContext ctx;
  EXPECT_THROW(stage.execute({}, {}, ctx), orc::DAGExecutionError);
}

TEST(FrameMapStageTest, Execute_ThrowsWhenInputIsNotVFrameR) {
  orc::FrameMapStage stage;
  orc::ObservationContext ctx;
  struct DummyArtifact : public orc::Artifact {
    DummyArtifact()
        : orc::Artifact(orc::ArtifactID("dummy"), orc::Provenance{}) {}
    std::string type_name() const override { return "dummy"; }
  };
  orc::ArtifactPtr bad = std::make_shared<DummyArtifact>();
  EXPECT_THROW(stage.execute({bad}, {}, ctx), orc::DAGExecutionError);
}

// ── Dropout hint frame IDs ──────────────────────────────────────────────────

// Regression: hints returned for a mapped frame must carry the mapped
// representation's frame ID, not the source frame's — consumers such as
// dropout_map may rely on the frame_id field.
TEST(FrameMapStageTest, GetDropoutHints_RewritesFrameIdToMappedFrame) {
  auto source =
      std::make_shared<::testing::NiceMock<MockVideoFrameRepresentation>>();

  // Source frame 5 has one dropout run (carrying its own frame id, 5).
  std::vector<orc::DropoutRun> source_runs{{orc::FrameID{5}, 1000u, 50u, 128}};
  ON_CALL(*source, get_dropout_hints(orc::FrameID{5}))
      .WillByDefault(::testing::Return(source_runs));

  // Output frame 0 maps to source frame 5.
  const orc::FrameMappedRepresentation rep(source, {orc::FrameID{5}}, {},
                                           "test");

  const auto hints = rep.get_dropout_hints(orc::FrameID{0});
  ASSERT_EQ(hints.size(), 1u);
  EXPECT_EQ(hints[0].frame_id, orc::FrameID{0});
  EXPECT_EQ(hints[0].sample_start, 1000u);
  EXPECT_EQ(hints[0].sample_count, 50u);
}

// ── Audio channel pairs: cadence-aware frame remapping ──────────────────────

namespace {

constexpr orc::FrameID kPad{UINT64_MAX};

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

// Mocked source carrying one audio channel pair whose per-frame samples come
// from frame_audio(); no filesystem or real media involved.
std::shared_ptr<::testing::NiceMock<MockVideoFrameRepresentation>>
make_audio_source(orc::VideoSystem system) {
  auto source =
      std::make_shared<::testing::NiceMock<MockVideoFrameRepresentation>>();
  orc::SourceParameters params;
  params.system = system;
  ON_CALL(*source, get_video_parameters())
      .WillByDefault(::testing::Return(params));
  ON_CALL(*source, audio_channel_pair_count())
      .WillByDefault(::testing::Return(1u));
  ON_CALL(*source, get_audio_channel_pair_descriptor(0))
      .WillByDefault(::testing::Return(orc::AudioChannelPairDescriptor{
          "Analogue", orc::AudioOrigin::ANALOGUE}));
  ON_CALL(*source, get_audio_samples(0, ::testing::_))
      .WillByDefault(::testing::Invoke([system](size_t, orc::FrameID id) {
        return frame_audio(id, system);
      }));
  return source;
}

}  // namespace

TEST(FrameMapStageTest, Audio_PairCountAndDescriptorForwardFromSource) {
  auto source = make_audio_source(orc::VideoSystem::NTSC);
  const orc::FrameMappedRepresentation rep(source, {orc::FrameID{0}}, {},
                                           "forward");

  EXPECT_EQ(rep.audio_channel_pair_count(), 1u);
  const auto desc = rep.get_audio_channel_pair_descriptor(0);
  ASSERT_TRUE(desc.has_value());
  EXPECT_EQ(desc->name, "Analogue");
  EXPECT_EQ(desc->origin, orc::AudioOrigin::ANALOGUE);
}

TEST(FrameMapStageTest, Audio_PhasePreservingNtscMappingIsSampleExact) {
  auto source = make_audio_source(orc::VideoSystem::NTSC);
  // Offset 5 preserves the SMPTE 272M five-frame audio sequence phase:
  // output frame p maps to source frame p + 5 with identical native counts.
  const orc::FrameMappedRepresentation rep(
      source,
      {orc::FrameID{5}, orc::FrameID{6}, orc::FrameID{7}, orc::FrameID{8},
       orc::FrameID{9}},
      {}, "phase_preserving");

  for (uint64_t p = 0; p < 5; ++p) {
    EXPECT_EQ(rep.get_audio_samples(0, orc::FrameID{p}),
              frame_audio(p + 5, orc::VideoSystem::NTSC))
        << "output frame " << p;
  }
}

TEST(FrameMapStageTest, Audio_PalMappingIsSampleExactAtAnyOffset) {
  auto source = make_audio_source(orc::VideoSystem::PAL);
  // PAL is constant-cadence (1920 pairs per frame), so any reordering is
  // sample-exact.
  const orc::FrameMappedRepresentation rep(
      source, {orc::FrameID{3}, orc::FrameID{7}, orc::FrameID{2}}, {},
      "pal_reorder");

  EXPECT_EQ(rep.get_audio_samples(0, orc::FrameID{0}),
            frame_audio(3, orc::VideoSystem::PAL));
  EXPECT_EQ(rep.get_audio_samples(0, orc::FrameID{1}),
            frame_audio(7, orc::VideoSystem::PAL));
  const auto samples = rep.get_audio_samples(0, orc::FrameID{2});
  EXPECT_EQ(samples, frame_audio(2, orc::VideoSystem::PAL));
  EXPECT_EQ(samples.size(), 1920u * 2u);
}

TEST(FrameMapStageTest, Audio_PhaseBreakingNtscMappingPadsOneSilencePair) {
  auto source = make_audio_source(orc::VideoSystem::NTSC);
  // Output frame 0 needs 1602 pairs but source frame 1 natively carries 1601
  // (SMPTE 272M-1994 §14.3) — one trailing silence pair is appended.
  const orc::FrameMappedRepresentation rep(source, {orc::FrameID{1}}, {},
                                           "pad_direction");

  const auto samples = rep.get_audio_samples(0, orc::FrameID{0});
  ASSERT_EQ(samples.size(), 1602u * 2u);
  const auto native = frame_audio(1, orc::VideoSystem::NTSC);
  EXPECT_TRUE(std::equal(native.begin(), native.end(), samples.begin()));
  EXPECT_EQ(samples[samples.size() - 2], 0);
  EXPECT_EQ(samples[samples.size() - 1], 0);
}

TEST(FrameMapStageTest, Audio_PhaseBreakingNtscMappingTruncatesOnePair) {
  auto source = make_audio_source(orc::VideoSystem::NTSC);
  // Output frame 1 needs 1601 pairs but source frame 0 natively carries 1602
  // (SMPTE 272M-1994 §14.3) — the trailing pair is truncated.
  const orc::FrameMappedRepresentation rep(
      source, {orc::FrameID{2}, orc::FrameID{0}}, {}, "truncate_direction");

  const auto samples = rep.get_audio_samples(0, orc::FrameID{1});
  ASSERT_EQ(samples.size(), 1601u * 2u);
  auto expected = frame_audio(0, orc::VideoSystem::NTSC);
  expected.resize(1601u * 2u);
  EXPECT_EQ(samples, expected);
}

TEST(FrameMapStageTest, Audio_PaddingFramesCarryCadenceSizedSilence) {
  auto source = make_audio_source(orc::VideoSystem::NTSC);
  const orc::FrameMappedRepresentation rep(source, {kPad, kPad}, {},
                                           "pad_only");

  // Silence blocks are sized by the OUTPUT frame's cadence position, not a
  // constant per-frame count: 1602 pairs at index 0, 1601 at index 1.
  const auto silence0 = rep.get_audio_samples(0, orc::FrameID{0});
  EXPECT_EQ(silence0.size(), 1602u * 2u);
  EXPECT_TRUE(std::all_of(silence0.begin(), silence0.end(),
                          [](int32_t s) { return s == 0; }));
  const auto silence1 = rep.get_audio_samples(0, orc::FrameID{1});
  EXPECT_EQ(silence1.size(), 1601u * 2u);
  EXPECT_TRUE(std::all_of(silence1.begin(), silence1.end(),
                          [](int32_t s) { return s == 0; }));
}

TEST(FrameMapStageTest, Audio_OutOfRangePairReturnsEmpty) {
  auto source = make_audio_source(orc::VideoSystem::NTSC);
  const orc::FrameMappedRepresentation rep(source, {orc::FrameID{0}, kPad}, {},
                                           "out_of_range");

  EXPECT_TRUE(rep.get_audio_samples(1, orc::FrameID{0}).empty());
  // Padding frames also honour the pair range.
  EXPECT_TRUE(rep.get_audio_samples(1, orc::FrameID{1}).empty());
}

// ── Range specs that overshoot the source ───────────────────────────────────

namespace {

// A source of |count| frames, ids 0..count-1, with nothing but navigation
// answered — enough for the mapping walk. Artifact-backed, so it can be fed
// to execute() as a DAG input.
std::shared_ptr<::testing::NiceMock<MockVideoFrameRepresentationArtifact>>
make_navigable_source(uint64_t count) {
  auto source = std::make_shared<
      ::testing::NiceMock<MockVideoFrameRepresentationArtifact>>();
  ON_CALL(*source, frame_range())
      .WillByDefault(::testing::Return(orc::FrameIDRange{0, count - 1}));
  ON_CALL(*source, frame_count()).WillByDefault(::testing::Return(count));
  ON_CALL(*source, has_frame(::testing::_))
      .WillByDefault(
          ::testing::Invoke([count](orc::FrameID id) { return id < count; }));
  return source;
}

// The stage's execute() takes artifacts; the mock is one by inheritance.
orc::ArtifactPtr as_artifact(
    const std::shared_ptr<
        ::testing::NiceMock<MockVideoFrameRepresentationArtifact>>& source) {
  return std::static_pointer_cast<orc::Artifact>(source);
}

}  // namespace

// A spec reaching past the end of the source keeps the frames that exist and
// drops the rest. The walk must not iterate the overshoot: this runs on every
// execute() of the node, and a spec overshooting by millions once turned it
// into millions of log lines per execution.
TEST(FrameMapStageTest, Execute_SpecPastEndOfSourceKeepsOnlyRealFrames) {
  auto source = make_navigable_source(10);
  orc::FrameMapStage stage;
  orc::ObservationContext ctx;

  // has_frame() is asked once per frame that could exist; a walk that counted
  // its way to 9,999,999 would blow far past that.
  EXPECT_CALL(*source, has_frame(::testing::_)).Times(::testing::AtMost(64));

  const auto outputs = stage.execute(
      {as_artifact(source)}, {{"ranges", std::string("0-9999999")}}, ctx);

  ASSERT_EQ(outputs.size(), 1u);
  auto mapped = std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
      outputs[0]);
  ASSERT_NE(mapped, nullptr);
  EXPECT_EQ(mapped->frame_count(), 10u);
}

// A range entirely past the end yields nothing to map, which is the
// empty-mapping pass-through rather than a crash or a stall.
TEST(FrameMapStageTest, Execute_SpecEntirelyPastEndOfSourcePassesThrough) {
  auto source = make_navigable_source(10);
  orc::FrameMapStage stage;
  orc::ObservationContext ctx;

  const auto outputs = stage.execute({as_artifact(source)},
                                     {{"ranges", std::string("100-200")}}, ctx);

  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_EQ(outputs[0], as_artifact(source));
}

// ── execute() must not write the stage's configuration ──────────────────────

// A DAG's stages are shared: reconfiguring the object from execute() let one
// thread reparse the ranges while another walked them. The parameter map is
// resolved onto the stack instead, so the stage's own configuration — what the
// host reads back and shows in the parameter dialog — is untouched by a run.
TEST(FrameMapStageTest, Execute_LeavesConfiguredParametersAlone) {
  auto source = make_navigable_source(10);
  orc::FrameMapStage stage;
  orc::ObservationContext ctx;

  ASSERT_TRUE(stage.set_parameters({{"ranges", std::string("0-4")},
                                    {"remove_duplicates", false},
                                    {"pad_gaps", false}}));
  const auto configured = stage.get_parameters();

  stage.execute({as_artifact(source)}, {{"ranges", std::string("5-9")}}, ctx);

  EXPECT_EQ(stage.get_parameters(), configured);
}

// The run still honours the parameters it was handed, configuration or not.
TEST(FrameMapStageTest, Execute_UsesTheParametersItWasHanded) {
  auto source = make_navigable_source(10);
  orc::FrameMapStage stage;
  orc::ObservationContext ctx;

  const auto outputs = stage.execute({as_artifact(source)},
                                     {{"ranges", std::string("2-4")}}, ctx);

  ASSERT_EQ(outputs.size(), 1u);
  auto mapped = std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
      outputs[0]);
  ASSERT_NE(mapped, nullptr);
  EXPECT_EQ(mapped->frame_count(), 3u);
}

// ── Padding frame geometry ──────────────────────────────────────────────────

namespace {

constexpr orc::FrameID kPadding{UINT64_MAX};

// Mocked source with the given broadcast geometry and nothing else.
std::shared_ptr<::testing::NiceMock<MockVideoFrameRepresentation>>
make_geometry_source(orc::VideoSystem system, int32_t width, int32_t height) {
  auto source =
      std::make_shared<::testing::NiceMock<MockVideoFrameRepresentation>>();
  orc::SourceParameters params;
  params.system = system;
  params.frame_width_nominal = width;
  params.frame_height = height;
  params.blanking_level = 256;
  ON_CALL(*source, get_video_parameters())
      .WillByDefault(::testing::Return(params));
  return source;
}

// A pad frame must be a whole frame: its descriptor, its sample buffer and
// its last line all have to agree with the source geometry.
void expect_pad_frame_geometry(const orc::FrameMappedRepresentation& rep,
                               orc::FrameID id, size_t height,
                               size_t expected_total) {
  const auto desc = rep.get_frame_descriptor(id);
  ASSERT_TRUE(desc.has_value());
  EXPECT_TRUE(desc->is_padding_frame);
  EXPECT_EQ(desc->samples_total, expected_total);

  const auto copy = rep.get_frame_copy(id);
  EXPECT_EQ(copy.size(), expected_total);

  const auto* frame = rep.get_frame(id);
  ASSERT_NE(frame, nullptr);
  const auto* last_line = rep.get_line(id, height - 1);
  ASSERT_NE(last_line, nullptr);
  const size_t width = desc->samples_per_line_nominal;
  const size_t last_count =
      orc::frame_line_sample_count(desc->system, width, height - 1);
  EXPECT_EQ(static_cast<size_t>(last_line - frame) + last_count,
            expected_total);
  EXPECT_EQ(rep.get_line(id, height), nullptr);
}

}  // namespace

// PAL lines 312 and 624 carry two extra samples each, so a PAL frame is
// 709,379 samples, not 625 x 1135. A short pad shifts every later frame of a
// CVBS export.
TEST(FrameMapStageTest, PalRangeSpecPad_IsAWholeFrame) {
  auto source = make_geometry_source(orc::VideoSystem::PAL, 1135, 625);
  const orc::FrameMappedRepresentation rep(source, {kPadding}, {}, "pal_pad");
  expect_pad_frame_geometry(rep, orc::FrameID{0}, 625, 709379u);
}

TEST(FrameMapStageTest, PalGapPad_IsAWholeFrame) {
  auto source = make_geometry_source(orc::VideoSystem::PAL, 1135, 625);
  const orc::FrameMappedRepresentation::PaddingDescriptor pad{
      orc::FrameID{0}, orc::VideoSystem::PAL, 625, 709379u, 1135};
  const orc::FrameMappedRepresentation rep(source, {kPadding}, {pad},
                                           "pal_gap");
  expect_pad_frame_geometry(rep, orc::FrameID{0}, 625, 709379u);
}

TEST(FrameMapStageTest, NtscRangeSpecPad_IsHeightTimesWidth) {
  auto source = make_geometry_source(orc::VideoSystem::NTSC, 910, 525);
  const orc::FrameMappedRepresentation rep(source, {kPadding}, {}, "ntsc_pad");
  expect_pad_frame_geometry(rep, orc::FrameID{0}, 525, 910u * 525u);
}

TEST(FrameMapStageTest, PalMRangeSpecPad_IsHeightTimesWidth) {
  auto source = make_geometry_source(orc::VideoSystem::PAL_M, 909, 525);
  const orc::FrameMappedRepresentation rep(source, {kPadding}, {}, "palm_pad");
  expect_pad_frame_geometry(rep, orc::FrameID{0}, 525, 909u * 525u);
}

}  // namespace orc_unit_test
