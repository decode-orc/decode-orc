/*
 * File:        source_join_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for SourceJoinStage ordering, joining and status
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../../orc/plugins/stages/source_join/source_join_stage.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <algorithm>

#include "../../include/video_frame_representation_artifact_mock.h"
#include "../../mocks/mock_video_frame_representation.h"

namespace orc_unit_test {

namespace {

using ::testing::_;
using ::testing::Invoke;
using ::testing::NiceMock;
using ::testing::Return;

const orc::ParameterDescriptor* find_descriptor(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::string& name) {
  auto it = std::find_if(
      descriptors.begin(), descriptors.end(),
      [&](const orc::ParameterDescriptor& d) { return d.name == name; });
  return it == descriptors.end() ? nullptr : &(*it);
}

// A source of |count| frames, ids 0..count-1, answering navigation only.
// Artifact-backed so it can be fed to execute() as a DAG input.
std::shared_ptr<NiceMock<MockVideoFrameRepresentationArtifact>>
make_navigable_source(uint64_t count) {
  auto source =
      std::make_shared<NiceMock<MockVideoFrameRepresentationArtifact>>();
  ON_CALL(*source, frame_range())
      .WillByDefault(Return(orc::FrameIDRange{0, count - 1}));
  ON_CALL(*source, frame_count()).WillByDefault(Return(count));
  ON_CALL(*source, has_frame(_)).WillByDefault(Invoke([count](orc::FrameID id) {
    return id < count;
  }));
  ON_CALL(*source, get_frame_descriptor(_))
      .WillByDefault(Invoke([count](orc::FrameID id) {
        std::optional<orc::FrameDescriptor> desc;
        if (id < count) {
          orc::FrameDescriptor d;
          d.frame_id = id;
          d.system = orc::VideoSystem::PAL;
          d.height = 625;
          d.samples_per_line_nominal = 1135;
          d.samples_total = 625 * 1135;
          desc = d;
        }
        return desc;
      }));
  return source;
}

orc::SourceParameters pal_parameters() {
  orc::SourceParameters params;
  params.system = orc::VideoSystem::PAL;
  params.frame_width_nominal = 1135;
  params.frame_height = 625;
  return params;
}

orc::ArtifactPtr as_artifact(
    const std::shared_ptr<NiceMock<MockVideoFrameRepresentationArtifact>>&
        source) {
  return std::static_pointer_cast<orc::Artifact>(source);
}

// Parameter map as the host hands it to execute(): the user's order plus the
// reserved input-identity parameter the DAG builder fills in.
std::map<std::string, orc::ParameterValue> run_parameters(
    const std::string& order, const std::string& connected) {
  return {{"input_order", order},
          {std::string(orc::kInputNodeIdsParameter), connected}};
}

}  // namespace

// ── NodeTypeInfo / interface contracts ──────────────────────────────────────

TEST(SourceJoinStageTest, RequiredInputCount_IsOne) {
  orc::SourceJoinStage stage;
  EXPECT_EQ(stage.required_input_count(), 1u);
}

TEST(SourceJoinStageTest, OutputCount_IsOne) {
  orc::SourceJoinStage stage;
  EXPECT_EQ(stage.output_count(), 1u);
}

TEST(SourceJoinStageTest, NodeTypeInfo_HasExpectedMetadata) {
  orc::SourceJoinStage stage;
  const auto info = stage.get_node_type_info();
  EXPECT_EQ(info.type, orc::NodeType::TRANSFORM);
  EXPECT_EQ(info.stage_name, "source_join");
  EXPECT_EQ(info.display_name, "Source Join");
  EXPECT_EQ(info.min_inputs, 1u);
  EXPECT_EQ(info.max_inputs, 16u);
  EXPECT_EQ(info.compatible_formats, orc::VideoFormatCompatibility::ALL);
}

// ── Parameter descriptors vs runtime defaults ───────────────────────────────

TEST(SourceJoinStageTest, Descriptor_InputOrderMatchesRuntimeDefault) {
  orc::SourceJoinStage stage;
  const auto descs = stage.get_parameter_descriptors();
  const auto params = stage.get_parameters();

  const auto* d = find_descriptor(descs, "input_order");
  ASSERT_NE(d, nullptr);
  ASSERT_TRUE(d->constraints.default_value.has_value());
  EXPECT_EQ(std::get<std::string>(*d->constraints.default_value),
            std::get<std::string>(params.at("input_order")));
}

// The host fills this parameter in from the node's incoming connections, so
// the stage has to declare it for the DAG builder to recognise the node as
// needing input identity at all.
TEST(SourceJoinStageTest, Descriptor_DeclaresReservedInputIdentityParameter) {
  orc::SourceJoinStage stage;
  const auto descs = stage.get_parameter_descriptors();
  const auto* d = find_descriptor(descs, orc::kInputNodeIdsParameter);
  ASSERT_NE(d, nullptr);
  EXPECT_EQ(d->type, orc::ParameterType::STRING);
}

// ── Node-ID list parsing ────────────────────────────────────────────────────

TEST(SourceJoinStageTest, ParseNodeIdList_AcceptsSpacedList) {
  const auto ids = orc::SourceJoinStage::parse_node_id_list(" 16, 2 ,4 ");
  ASSERT_TRUE(ids.has_value());
  EXPECT_EQ(*ids, (std::vector<orc::NodeID::value_type>{16, 2, 4}));
}

TEST(SourceJoinStageTest, ParseNodeIdList_EmptySpecIsEmptyList) {
  const auto ids = orc::SourceJoinStage::parse_node_id_list("");
  ASSERT_TRUE(ids.has_value());
  EXPECT_TRUE(ids->empty());
}

TEST(SourceJoinStageTest, ParseNodeIdList_RejectsNonNumericToken) {
  EXPECT_FALSE(orc::SourceJoinStage::parse_node_id_list("16,two").has_value());
  EXPECT_FALSE(orc::SourceJoinStage::parse_node_id_list("16x").has_value());
}

TEST(SourceJoinStageTest, ParseNodeIdList_RejectsNegativeId) {
  EXPECT_FALSE(orc::SourceJoinStage::parse_node_id_list("-1").has_value());
}

// A node has one output to connect, so a repeated ID cannot be a request to
// use the same source twice — it is a typo.
TEST(SourceJoinStageTest, ParseNodeIdList_RejectsRepeatedId) {
  EXPECT_FALSE(orc::SourceJoinStage::parse_node_id_list("4,2,4").has_value());
}

// ── Order resolution ────────────────────────────────────────────────────────

TEST(SourceJoinStageTest, ResolveInputPositions_EmptyOrderIsConnectionOrder) {
  std::vector<orc::NodeID::value_type> unresolved;
  std::vector<orc::NodeID::value_type> skipped;
  const auto positions = orc::SourceJoinStage::resolve_input_positions(
      {}, {2, 4, 16}, unresolved, skipped);
  EXPECT_EQ(positions, (std::vector<size_t>{0, 1, 2}));
  EXPECT_TRUE(unresolved.empty());
  EXPECT_TRUE(skipped.empty());
}

TEST(SourceJoinStageTest, ResolveInputPositions_ReordersConnectedInputs) {
  std::vector<orc::NodeID::value_type> unresolved;
  std::vector<orc::NodeID::value_type> skipped;
  const auto positions = orc::SourceJoinStage::resolve_input_positions(
      {16, 2, 4}, {2, 4, 16}, unresolved, skipped);
  EXPECT_EQ(positions, (std::vector<size_t>{2, 0, 1}));
  EXPECT_TRUE(unresolved.empty());
  EXPECT_TRUE(skipped.empty());
}

TEST(SourceJoinStageTest, ResolveInputPositions_ReportsUnconnectedAndSkipped) {
  std::vector<orc::NodeID::value_type> unresolved;
  std::vector<orc::NodeID::value_type> skipped;
  const auto positions = orc::SourceJoinStage::resolve_input_positions(
      {4, 99}, {2, 4}, unresolved, skipped);
  EXPECT_EQ(positions, (std::vector<size_t>{1}));
  EXPECT_EQ(unresolved, (std::vector<orc::NodeID::value_type>{99}));
  EXPECT_EQ(skipped, (std::vector<orc::NodeID::value_type>{2}));
}

// ── set_parameters and configuration status ─────────────────────────────────

TEST(SourceJoinStageTest, SetParameters_AcceptsValidOrder) {
  orc::SourceJoinStage stage;
  EXPECT_TRUE(stage.set_parameters({{"input_order", std::string("16,2,4")}}));
  EXPECT_EQ(std::get<std::string>(stage.get_parameters().at("input_order")),
            "16,2,4");
}

TEST(SourceJoinStageTest, SetParameters_RejectsMalformedOrder) {
  orc::SourceJoinStage stage;
  EXPECT_FALSE(stage.set_parameters({{"input_order", std::string("16,,x")}}));
}

TEST(SourceJoinStageTest, SetParameters_RejectsUnknownParameter) {
  orc::SourceJoinStage stage;
  EXPECT_FALSE(stage.set_parameters({{"bogus", std::string("1")}}));
}

TEST(SourceJoinStageTest, Status_YellowWhenOrderNotSet) {
  orc::SourceJoinStage stage;
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Yellow);
  ASSERT_TRUE(stage.set_parameters(
      {{"input_order", std::string("")},
       {std::string(orc::kInputNodeIdsParameter), std::string("2,4")}}));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Yellow);
}

TEST(SourceJoinStageTest, Status_GreenWhenOrderNamesEveryConnectedInput) {
  orc::SourceJoinStage stage;
  ASSERT_TRUE(stage.set_parameters(
      {{"input_order", std::string("4,2")},
       {std::string(orc::kInputNodeIdsParameter), std::string("2,4")}}));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Green);
}

// Rewiring the node leaves the stored order describing a graph that no longer
// exists; the status dot has to say so.
TEST(SourceJoinStageTest, Status_YellowWhenInputsChangedUnderTheOrder) {
  orc::SourceJoinStage stage;
  ASSERT_TRUE(stage.set_parameters(
      {{"input_order", std::string("4,2")},
       {std::string(orc::kInputNodeIdsParameter), std::string("2,4")}}));
  ASSERT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Green);

  // A third source is connected: the order no longer covers the inputs.
  ASSERT_TRUE(stage.set_parameters(
      {{std::string(orc::kInputNodeIdsParameter), std::string("2,4,7")}}));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Yellow);
}

// Disconnecting every input is the host answering "nothing is connected", not
// the host staying silent: a stored order cannot match an empty graph.
TEST(SourceJoinStageTest, Status_YellowWhenEveryInputIsDisconnected) {
  orc::SourceJoinStage stage;
  ASSERT_TRUE(stage.set_parameters(
      {{"input_order", std::string("4,2")},
       {std::string(orc::kInputNodeIdsParameter), std::string("2,4")}}));
  ASSERT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Green);

  ASSERT_TRUE(stage.set_parameters(
      {{std::string(orc::kInputNodeIdsParameter), std::string("")}}));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Yellow);
}

// A host that never supplies input identity cannot have the order checked
// against it, so the order is taken at face value rather than reported stale.
TEST(SourceJoinStageTest, Status_GreenWhenHostSuppliesNoInputIdentity) {
  orc::SourceJoinStage stage;
  ASSERT_TRUE(stage.set_parameters({{"input_order", std::string("4,2")}}));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Green);
}

TEST(SourceJoinStageTest, Status_YellowWhenOrderNamesAnUnconnectedNode) {
  orc::SourceJoinStage stage;
  ASSERT_TRUE(stage.set_parameters(
      {{"input_order", std::string("4,99")},
       {std::string(orc::kInputNodeIdsParameter), std::string("2,4")}}));
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Yellow);
}

// ── execute() error handling ────────────────────────────────────────────────

TEST(SourceJoinStageTest, Execute_ThrowsWhenNoInputs) {
  orc::SourceJoinStage stage;
  orc::ObservationContext ctx;
  EXPECT_THROW(stage.execute({}, {}, ctx), orc::DAGExecutionError);
}

TEST(SourceJoinStageTest, Execute_ThrowsWhenInputIsNotVFrameR) {
  orc::SourceJoinStage stage;
  orc::ObservationContext ctx;
  struct DummyArtifact : public orc::Artifact {
    DummyArtifact()
        : orc::Artifact(orc::ArtifactID("dummy"), orc::Provenance{}) {}
    std::string type_name() const override { return "dummy"; }
  };
  orc::ArtifactPtr bad = std::make_shared<DummyArtifact>();
  EXPECT_THROW(stage.execute({bad}, {}, ctx), orc::DAGExecutionError);
}

TEST(SourceJoinStageTest, Execute_ThrowsWhenOrderNamesNoConnectedNode) {
  auto a = make_navigable_source(4);
  auto b = make_navigable_source(4);
  orc::SourceJoinStage stage;
  orc::ObservationContext ctx;
  EXPECT_THROW(stage.execute({as_artifact(a), as_artifact(b)},
                             run_parameters("70,71", "2,4"), ctx),
               orc::DAGExecutionError);
}

// A join produces one sequence whose geometry downstream stages read once, so
// inputs that disagree about it cannot be concatenated.
TEST(SourceJoinStageTest, Execute_ThrowsWhenInputGeometriesDiffer) {
  auto pal = make_navigable_source(4);
  ON_CALL(*pal, get_video_parameters()).WillByDefault(Return(pal_parameters()));

  auto ntsc = make_navigable_source(4);
  orc::SourceParameters ntsc_params;
  ntsc_params.system = orc::VideoSystem::NTSC;
  ntsc_params.frame_width_nominal = 910;
  ntsc_params.frame_height = 525;
  ON_CALL(*ntsc, get_video_parameters()).WillByDefault(Return(ntsc_params));

  orc::SourceJoinStage stage;
  orc::ObservationContext ctx;
  EXPECT_THROW(stage.execute({as_artifact(pal), as_artifact(ntsc)},
                             run_parameters("2,4", "2,4"), ctx),
               orc::DAGExecutionError);
}

// ── execute() joining ───────────────────────────────────────────────────────

// One input joins to itself, and handing the artifact straight back keeps the
// frame IDs (and anything keyed on them) untouched.
TEST(SourceJoinStageTest, Execute_SingleInputIsPassthrough) {
  auto source = make_navigable_source(5);
  orc::SourceJoinStage stage;
  orc::ObservationContext ctx;

  const auto outputs =
      stage.execute({as_artifact(source)}, run_parameters("2", "2"), ctx);
  ASSERT_EQ(outputs.size(), 1u);
  EXPECT_EQ(outputs[0], as_artifact(source));
}

TEST(SourceJoinStageTest, Execute_JoinsInputsInTheConfiguredOrder) {
  auto first = make_navigable_source(3);
  auto second = make_navigable_source(2);
  ON_CALL(*first, get_video_parameters())
      .WillByDefault(Return(pal_parameters()));
  ON_CALL(*second, get_video_parameters())
      .WillByDefault(Return(pal_parameters()));

  orc::SourceJoinStage stage;
  orc::ObservationContext ctx;

  // Connections were made as node 2 then node 4; the order asks for 4 first.
  const auto outputs = stage.execute({as_artifact(first), as_artifact(second)},
                                     run_parameters("4,2", "2,4"), ctx);
  ASSERT_EQ(outputs.size(), 1u);
  auto joined = std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
      outputs[0]);
  ASSERT_NE(joined, nullptr);

  EXPECT_EQ(joined->frame_count(), 5u);
  EXPECT_EQ(joined->frame_range().first, orc::FrameID{0});
  EXPECT_EQ(joined->frame_range().last, orc::FrameID{4});
  EXPECT_TRUE(joined->has_frame(orc::FrameID{4}));
  EXPECT_FALSE(joined->has_frame(orc::FrameID{5}));

  // Output frames are renumbered from the start of the join.
  for (uint64_t p = 0; p < 5; ++p) {
    const auto desc = joined->get_frame_descriptor(orc::FrameID{p});
    ASSERT_TRUE(desc.has_value()) << "output frame " << p;
    EXPECT_EQ(desc->frame_id, orc::FrameID{p});
  }
}

TEST(SourceJoinStageTest, Execute_EmptyOrderJoinsInConnectionOrder) {
  auto first = make_navigable_source(3);
  auto second = make_navigable_source(2);
  ON_CALL(*first, get_video_parameters())
      .WillByDefault(Return(pal_parameters()));
  ON_CALL(*second, get_video_parameters())
      .WillByDefault(Return(pal_parameters()));

  orc::SourceJoinStage stage;
  orc::ObservationContext ctx;
  const auto outputs = stage.execute({as_artifact(first), as_artifact(second)},
                                     run_parameters("", "2,4"), ctx);
  ASSERT_EQ(outputs.size(), 1u);
  auto joined = std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
      outputs[0]);
  ASSERT_NE(joined, nullptr);
  EXPECT_EQ(joined->frame_count(), 5u);
}

// A connected source the order does not name contributes nothing; the omission
// is a fact about the run, so it is recorded as an observation.
TEST(SourceJoinStageTest, Execute_UnlistedInputIsExcludedAndObserved) {
  auto first = make_navigable_source(3);
  auto second = make_navigable_source(2);
  auto third = make_navigable_source(4);
  for (auto* source : {&first, &second, &third}) {
    ON_CALL(**source, get_video_parameters())
        .WillByDefault(Return(pal_parameters()));
  }

  orc::SourceJoinStage stage;
  orc::ObservationContext ctx;
  const auto outputs = stage.execute(
      {as_artifact(first), as_artifact(second), as_artifact(third)},
      run_parameters("7,2", "2,4,7"), ctx);
  ASSERT_EQ(outputs.size(), 1u);
  auto joined = std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
      outputs[0]);
  ASSERT_NE(joined, nullptr);
  // Node 7 (4 frames) then node 2 (3 frames); node 4's 2 frames are left out.
  EXPECT_EQ(joined->frame_count(), 7u);

  const auto skipped =
      ctx.get(orc::FieldID(0), "source_join", "inputs_skipped");
  ASSERT_TRUE(skipped.has_value());
  EXPECT_EQ(std::get<int64_t>(*skipped), 1);
}

// Without usable input identity there is no way to apply an order, so the
// artifacts are joined in the order they arrived rather than dropped.
TEST(SourceJoinStageTest, Execute_FallsBackToConnectionOrderWithoutIdentity) {
  auto first = make_navigable_source(3);
  auto second = make_navigable_source(2);
  ON_CALL(*first, get_video_parameters())
      .WillByDefault(Return(pal_parameters()));
  ON_CALL(*second, get_video_parameters())
      .WillByDefault(Return(pal_parameters()));

  orc::SourceJoinStage stage;
  orc::ObservationContext ctx;
  const auto outputs = stage.execute({as_artifact(first), as_artifact(second)},
                                     run_parameters("4,2", ""), ctx);
  ASSERT_EQ(outputs.size(), 1u);
  auto joined = std::dynamic_pointer_cast<const orc::VideoFrameRepresentation>(
      outputs[0]);
  ASSERT_NE(joined, nullptr);
  EXPECT_EQ(joined->frame_count(), 5u);
}

// ── JoinedVideoFrameRepresentation ──────────────────────────────────────────

namespace {

using Entry = orc::JoinedVideoFrameRepresentation::Entry;

std::shared_ptr<NiceMock<MockVideoFrameRepresentation>> make_plain_source() {
  auto source = std::make_shared<NiceMock<MockVideoFrameRepresentation>>();
  ON_CALL(*source, get_video_parameters())
      .WillByDefault(Return(pal_parameters()));
  return source;
}

}  // namespace

// Hints describe the output frame, so they carry the output's numbering —
// consumers such as dropout_map key on the frame_id field.
TEST(SourceJoinStageTest, GetDropoutHints_RewritesFrameIdToJoinedFrame) {
  auto first = make_plain_source();
  auto second = make_plain_source();

  std::vector<orc::DropoutRun> runs{{orc::FrameID{1}, 1000u, 50u, 128}};
  ON_CALL(*second, get_dropout_hints(orc::FrameID{1}))
      .WillByDefault(Return(runs));

  // Output 0-1 come from |first|; output 2 is |second| frame 1.
  const orc::JoinedVideoFrameRepresentation joined(
      {first, second},
      {Entry{0, orc::FrameID{0}}, Entry{0, orc::FrameID{1}},
       Entry{1, orc::FrameID{1}}},
      "test");

  const auto hints = joined.get_dropout_hints(orc::FrameID{2});
  ASSERT_EQ(hints.size(), 1u);
  EXPECT_EQ(hints[0].frame_id, orc::FrameID{2});
  EXPECT_EQ(hints[0].sample_start, 1000u);
  EXPECT_EQ(hints[0].sample_count, 50u);
}

// The pass-through hook is answered per frame ID: it holds for the leading
// input, whose IDs the join leaves alone, and not for anything renumbered.
TEST(SourceJoinStageTest, VideoPassthroughSource_OnlyForUnrenumberedFrames) {
  auto first = make_plain_source();
  auto second = make_plain_source();

  const orc::JoinedVideoFrameRepresentation joined(
      {first, second},
      {Entry{0, orc::FrameID{0}}, Entry{0, orc::FrameID{1}},
       Entry{1, orc::FrameID{0}}},
      "test");

  EXPECT_EQ(joined.video_passthrough_source(orc::FrameID{1}), first);
  EXPECT_EQ(joined.video_passthrough_source(orc::FrameID{2}), nullptr);
  EXPECT_EQ(joined.video_passthrough_source(orc::FrameID{9}), nullptr);
}

// A deferred decode can be nested under any joined input, not only the first.
TEST(SourceJoinStageTest, PrimeAudioDecode_ReachesEveryJoinedInput) {
  auto first = make_plain_source();
  auto second = make_plain_source();
  EXPECT_CALL(*first, prime_audio_decode(_)).Times(1);
  EXPECT_CALL(*second, prime_audio_decode(_)).Times(1);

  const orc::JoinedVideoFrameRepresentation joined(
      {first, second}, {Entry{0, orc::FrameID{0}}, Entry{1, orc::FrameID{0}}},
      "test");
  joined.prime_audio_decode({});
}

namespace {

// Interleaved stereo samples for source frame |frame|: stereo pair p carries
// frame × 10000 + p on both channels, sized to the frame's native SMPTE 272M
// cadence count, so any output sample is traceable to its source frame.
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

std::shared_ptr<NiceMock<MockVideoFrameRepresentation>> make_audio_source(
    orc::VideoSystem system, size_t pair_count) {
  auto source = std::make_shared<NiceMock<MockVideoFrameRepresentation>>();
  orc::SourceParameters params;
  params.system = system;
  params.frame_width_nominal = 1135;
  params.frame_height = 625;
  ON_CALL(*source, get_video_parameters()).WillByDefault(Return(params));
  ON_CALL(*source, audio_channel_pair_count())
      .WillByDefault(Return(pair_count));
  ON_CALL(*source, get_audio_samples(_, _))
      .WillByDefault(Invoke([system, pair_count](size_t pair, orc::FrameID id) {
        if (pair >= pair_count) return std::vector<int32_t>{};
        return frame_audio(id, system);
      }));
  return source;
}

}  // namespace

TEST(SourceJoinStageTest, Audio_PalJoinIsSampleExact) {
  auto first = make_audio_source(orc::VideoSystem::PAL, 1);
  auto second = make_audio_source(orc::VideoSystem::PAL, 1);

  // Output 0-1 from |first|; output 2 is |second| frame 0.
  const orc::JoinedVideoFrameRepresentation joined(
      {first, second},
      {Entry{0, orc::FrameID{0}}, Entry{0, orc::FrameID{1}},
       Entry{1, orc::FrameID{0}}},
      "test");

  EXPECT_EQ(joined.get_audio_samples(0, orc::FrameID{2}),
            frame_audio(0, orc::VideoSystem::PAL));
}

// SMPTE 272M-1994 §14.3: an NTSC frame carries 1602 or 1601 stereo pairs by
// position in the five-frame sequence, so a join that moves a frame to a
// different phase has to trim or silence-pad the window by one pair.
TEST(SourceJoinStageTest, Audio_PhaseBreakingNtscJoinPadsOneSilencePair) {
  auto first = make_audio_source(orc::VideoSystem::NTSC, 1);
  auto second = make_audio_source(orc::VideoSystem::NTSC, 1);

  // Output frame 5 needs 1602 pairs; it is |second| frame 1, which natively
  // carries 1601.
  std::vector<Entry> mapping;
  for (uint64_t i = 0; i < 5; ++i) mapping.push_back(Entry{0, orc::FrameID{i}});
  mapping.push_back(Entry{1, orc::FrameID{1}});

  const orc::JoinedVideoFrameRepresentation joined({first, second},
                                                   std::move(mapping), "test");

  const auto samples = joined.get_audio_samples(0, orc::FrameID{5});
  ASSERT_EQ(samples.size(), 1602u * 2u);
  const auto native = frame_audio(1, orc::VideoSystem::NTSC);
  EXPECT_TRUE(std::equal(native.begin(), native.end(), samples.begin()));
  EXPECT_EQ(samples[samples.size() - 1], 0);
}

// The joined output carries the leading input's channel-pair layout; an input
// with fewer pairs contributes silence rather than a short block that would
// desynchronise the pair.
TEST(SourceJoinStageTest, Audio_InputWithFewerPairsContributesSilence) {
  auto first = make_audio_source(orc::VideoSystem::PAL, 2);
  auto second = make_audio_source(orc::VideoSystem::PAL, 1);

  const orc::JoinedVideoFrameRepresentation joined(
      {first, second}, {Entry{0, orc::FrameID{0}}, Entry{1, orc::FrameID{0}}},
      "test");

  ASSERT_EQ(joined.audio_channel_pair_count(), 2u);
  const auto samples = joined.get_audio_samples(1, orc::FrameID{1});
  ASSERT_EQ(samples.size(), 1920u * 2u);
  EXPECT_TRUE(std::all_of(samples.begin(), samples.end(),
                          [](int32_t s) { return s == 0; }));
}

TEST(SourceJoinStageTest, Audio_OutOfRangePairReturnsEmpty) {
  auto first = make_audio_source(orc::VideoSystem::PAL, 1);
  const orc::JoinedVideoFrameRepresentation joined(
      {first}, {Entry{0, orc::FrameID{0}}}, "test");
  EXPECT_TRUE(joined.get_audio_samples(3, orc::FrameID{0}).empty());
}

}  // namespace orc_unit_test
