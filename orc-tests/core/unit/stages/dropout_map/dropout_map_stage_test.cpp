/*
 * File:        dropout_map_stage_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for DropoutMapStage (VFrameR)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/plugins/stages/dropout_map/dropout_map_stage.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <orc/stage/observation/observation_context.h>

#include <algorithm>

#include "../../mocks/mock_video_frame_representation.h"

using ::testing::Return;

namespace orc_unit_test {
namespace {

orc::SourceParameters make_ntsc_params() {
  orc::SourceParameters p;
  p.system = orc::VideoSystem::NTSC;
  p.frame_width_nominal = 910;
  p.frame_height = 525;
  p.blanking_level = 240;
  p.white_level = 800;
  return p;
}

}  // namespace

TEST(DropoutMapStageTest, RequiredInputCount_IsOne) {
  orc::DropoutMapStage stage;
  EXPECT_EQ(stage.required_input_count(), 1u);
}

TEST(DropoutMapStageTest, OutputCount_IsOne) {
  orc::DropoutMapStage stage;
  EXPECT_EQ(stage.output_count(), 1u);
}

TEST(DropoutMapStageTest, ConfigurationStatus_YellowWithoutMapGreenWithMap) {
  orc::DropoutMapStage stage;
  // The stage is pass-through with an empty map, so it is never Red.
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Yellow);

  stage.set_parameters(
      {{"dropout_map",
        orc::ParameterValue{std::string(
            "[{frame:1,add:[{line:10,start:100,end:200}],remove:[]}]")}}});
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Green);

  stage.set_parameters(
      {{"dropout_map", orc::ParameterValue{std::string("[]")}}});
  EXPECT_EQ(stage.get_configuration_status(), orc::ConfigurationStatus::Yellow);
}

TEST(DropoutMapStageTest, NodeTypeInfo_HasExpectedMetadata) {
  orc::DropoutMapStage stage;
  auto info = stage.get_node_type_info();
  EXPECT_EQ(info.type, orc::NodeType::TRANSFORM);
  EXPECT_EQ(info.stage_name, "dropout_map");
  EXPECT_EQ(info.compatible_formats, orc::VideoFormatCompatibility::ALL);
}

TEST(DropoutMapStageTest, DescriptorDefault_MatchesRuntimeDefault) {
  orc::DropoutMapStage stage;
  const auto descriptors = stage.get_parameter_descriptors();
  const auto params = stage.get_parameters();
  auto it = std::find_if(descriptors.begin(), descriptors.end(),
                         [](const orc::ParameterDescriptor& d) {
                           return d.name == "dropout_map";
                         });
  ASSERT_NE(it, descriptors.end());
  ASSERT_TRUE(it->constraints.default_value.has_value());
  ASSERT_TRUE(
      std::holds_alternative<std::string>(*it->constraints.default_value));
  ASSERT_TRUE(std::holds_alternative<std::string>(params.at("dropout_map")));
  EXPECT_EQ(std::get<std::string>(*it->constraints.default_value),
            std::get<std::string>(params.at("dropout_map")));
}

TEST(DropoutMapStageTest, SetParameters_AcceptsDropoutMapString) {
  orc::DropoutMapStage stage;
  const bool ok = stage.set_parameters(
      {{"dropout_map",
        std::string(
            "[{frame:0,add:[{line:10,start:100,end:200}],remove:[]}]")}});
  EXPECT_TRUE(ok);
  const auto params = stage.get_parameters();
  EXPECT_EQ(std::get<std::string>(params.at("dropout_map")),
            "[{frame:0,add:[{line:10,start:100,end:200}],remove:[]}]");
}

TEST(DropoutMapStageTest, Execute_ThrowsWhenInputMissing) {
  orc::DropoutMapStage stage;
  orc::ObservationContext ctx;
  EXPECT_THROW(stage.execute({}, {}, ctx), orc::DAGExecutionError);
}

TEST(DropoutMapStageTest, Execute_ThrowsWhenInputIsWrongType) {
  struct FakeArt : public orc::Artifact {
    FakeArt() : Artifact(orc::ArtifactID("x"), orc::Provenance{}) {}
    std::string type_name() const override { return "x"; }
  };
  orc::DropoutMapStage stage;
  orc::ObservationContext ctx;
  EXPECT_THROW(stage.execute({std::make_shared<FakeArt>()}, {}, ctx),
               orc::DAGExecutionError);
}

TEST(DropoutMapStageTest, ParseEncodeRoundTrip_EmptyMap) {
  const auto map = orc::DropoutMapStage::parse_dropout_map("[]");
  EXPECT_TRUE(map.empty());
  EXPECT_EQ(orc::DropoutMapStage::encode_dropout_map(map), "[]");
}

TEST(DropoutMapStageTest, ParseEncodeRoundTrip_SingleEntry) {
  const std::string spec =
      "[{frame:3,add:[{line:10,start:100,end:200}],remove:[{line:5,start:50,"
      "end:75}]}]";
  const auto map = orc::DropoutMapStage::parse_dropout_map(spec);
  ASSERT_EQ(map.size(), 1u);
  ASSERT_TRUE(map.count(3));
  EXPECT_EQ(map.at(3).additions.size(), 1u);
  EXPECT_EQ(map.at(3).removals.size(), 1u);
  EXPECT_EQ(map.at(3).additions[0].line, 10u);
  EXPECT_EQ(map.at(3).additions[0].start_sample, 100u);
  EXPECT_EQ(map.at(3).additions[0].end_sample, 200u);
  EXPECT_EQ(map.at(3).removals[0].line, 5u);
  EXPECT_EQ(map.at(3).removals[0].start_sample, 50u);
  EXPECT_EQ(map.at(3).removals[0].end_sample, 75u);
  // Round-trip encode
  const auto encoded = orc::DropoutMapStage::encode_dropout_map(map);
  const auto map2 = orc::DropoutMapStage::parse_dropout_map(encoded);
  ASSERT_EQ(map2.size(), 1u);
  EXPECT_EQ(map2.at(3).additions.size(), 1u);
  EXPECT_EQ(map2.at(3).removals.size(), 1u);
}

TEST(DropoutMapStageTest, Parse_SkipsUnknownKeysAndSpaces) {
  const auto map = orc::dropout_map_text::parse(
      " [ { frame : 7 , note : 3 , add : [ { line : 1 , start : 2 , end : 3 , "
      "basis : 9 } ] } ] ");
  ASSERT_EQ(map.size(), 1u);
  ASSERT_EQ(map.at(7).additions.size(), 1u);
  EXPECT_EQ(map.at(7).additions[0], (orc::dropout_map_text::Span{1, 2, 3}));
  EXPECT_TRUE(map.at(7).removals.empty());
}

TEST(DropoutMapStageTest, Parse_StopsAtGarbageRatherThanLooping) {
  // Characters the reader cannot place used to leave it where it was.
  EXPECT_TRUE(orc::dropout_map_text::parse("[x]").empty());
  const auto map =
      orc::dropout_map_text::parse("[{frame:1,add:[{line:2,start:3,end:4}x]}]");
  ASSERT_EQ(map.count(1), 1u);
  EXPECT_EQ(map.at(1).additions.size(), 1u);
}

TEST(DropoutMapStageTest, Parse_MalformedTextIsEmpty) {
  EXPECT_TRUE(orc::dropout_map_text::is_malformed("{frame:1}"));
  EXPECT_FALSE(orc::dropout_map_text::is_malformed("  []"));
  EXPECT_FALSE(orc::dropout_map_text::is_malformed(""));
  EXPECT_TRUE(orc::DropoutMapStage::parse_dropout_map("{frame:1}").empty());
}

TEST(DropoutMapStageTest, Encode_WritesFramesInOrderAndOmitsEmptyLists) {
  orc::dropout_map_text::EditMap map;
  map[4].frame_id = 4;
  map[4].removals.push_back({3, 1, 2});
  map[1].frame_id = 1;
  map[1].additions.push_back({10, 20, 30});
  EXPECT_EQ(orc::dropout_map_text::encode(map),
            "[{frame:1,add:[{line:10,start:20,end:30}]},"
            "{frame:4,remove:[{line:3,start:1,end:2}]}]");
}

TEST(DropoutMapStageTest, OffersTheDropoutEditorAsAnEditorView) {
  const orc::DropoutMapStage stage;
  const auto tools = stage.get_stage_tools();
  ASSERT_EQ(tools.size(), 1u);
  EXPECT_EQ(tools[0].tool_id, "dropout_editor");
  EXPECT_EQ(tools[0].kind, orc::StageToolKind::NonModalEditor);
  EXPECT_TRUE(tools[0].non_modal);
  EXPECT_EQ(tools[0].contract_id,
            orc::dropout_map_text::kDropoutEditorContractId);
}

TEST(DropoutMapStageTest, GetDropoutHints_ReturnsSourceHints_WhenNoMapping) {
  auto source =
      std::make_shared<testing::NiceMock<MockVideoFrameRepresentation>>();
  const orc::FrameID fid{5};
  std::vector<orc::DropoutRun> source_hints{
      orc::DropoutRun{fid, 1000u, 50u, 128}};

  ON_CALL(*source, get_video_parameters())
      .WillByDefault(Return(make_ntsc_params()));
  ON_CALL(*source, get_dropout_hints(fid)).WillByDefault(Return(source_hints));

  std::map<uint64_t, orc::FrameDropoutMapEntry> empty_map;
  const orc::DropoutMappedFrameRepresentation rep(source, empty_map);

  const auto hints = rep.get_dropout_hints(fid);
  ASSERT_EQ(hints.size(), 1u);
  EXPECT_EQ(hints[0].sample_start, 1000u);
  EXPECT_EQ(hints[0].sample_count, 50u);
}

TEST(DropoutMapStageTest, GetDropoutHints_AddsDropoutRun) {
  auto source =
      std::make_shared<testing::NiceMock<MockVideoFrameRepresentation>>();
  const orc::FrameID fid{0};

  ON_CALL(*source, get_video_parameters())
      .WillByDefault(Return(make_ntsc_params()));
  ON_CALL(*source, get_dropout_hints(fid))
      .WillByDefault(Return(std::vector<orc::DropoutRun>{}));

  // Add a dropout on NTSC frame 0, line 10, samples 100-200
  orc::FrameDropoutMapEntry entry;
  entry.frame_id = 0;
  entry.additions.push_back({10u, 100u, 200u});
  std::map<uint64_t, orc::FrameDropoutMapEntry> dmap{{0, entry}};
  const orc::DropoutMappedFrameRepresentation rep(source, dmap);

  const auto hints = rep.get_dropout_hints(fid);
  ASSERT_EQ(hints.size(), 1u);
  EXPECT_EQ(hints[0].frame_id, fid);
  // NTSC: all lines 910 samples wide. Line 10 offset = 10*910 = 9100.
  // sample_start = 9100 + 100 = 9200, count = 200-100+1 = 101
  EXPECT_EQ(hints[0].sample_start, 9200u);
  EXPECT_EQ(hints[0].sample_count, 101u);
}

TEST(DropoutMapStageTest, GetDropoutHints_RemovesOverlappingSourceRun) {
  auto source =
      std::make_shared<testing::NiceMock<MockVideoFrameRepresentation>>();
  const orc::FrameID fid{2};

  // Source has a dropout on frame 2, at frame-flat offset 9200 (line 10,
  // sample 100 in NTSC), count 101.
  std::vector<orc::DropoutRun> src{{fid, 9200u, 101u, 128}};
  ON_CALL(*source, get_video_parameters())
      .WillByDefault(Return(make_ntsc_params()));
  ON_CALL(*source, get_dropout_hints(fid)).WillByDefault(Return(src));

  // Remove exactly that region: line 10, start 100, end 200 on frame 2
  orc::FrameDropoutMapEntry entry;
  entry.frame_id = 2;
  entry.removals.push_back({10u, 100u, 200u});
  std::map<uint64_t, orc::FrameDropoutMapEntry> dmap{{2, entry}};
  const orc::DropoutMappedFrameRepresentation rep(source, dmap);

  const auto hints = rep.get_dropout_hints(fid);
  EXPECT_TRUE(hints.empty());
}

// Regression: upstream wrappers do not reliably preserve DropoutRun::frame_id
// (the stacker used to emit frame_id 0; frame_map passed through the source
// frame's id). Removal matching must not depend on that field — the runs were
// fetched for this frame.
TEST(DropoutMapStageTest, GetDropoutHints_RemovesRun_DespiteStaleRunFrameId) {
  auto source =
      std::make_shared<testing::NiceMock<MockVideoFrameRepresentation>>();
  const orc::FrameID fid{2};

  // Same geometry as above, but the run carries a stale frame_id of 0.
  std::vector<orc::DropoutRun> src{{orc::FrameID{0}, 9200u, 101u, 128}};
  ON_CALL(*source, get_video_parameters())
      .WillByDefault(Return(make_ntsc_params()));
  ON_CALL(*source, get_dropout_hints(fid)).WillByDefault(Return(src));

  orc::FrameDropoutMapEntry entry;
  entry.frame_id = 2;
  entry.removals.push_back({10u, 100u, 200u});
  std::map<uint64_t, orc::FrameDropoutMapEntry> dmap{{2, entry}};
  const orc::DropoutMappedFrameRepresentation rep(source, dmap);

  const auto hints = rep.get_dropout_hints(fid);
  EXPECT_TRUE(hints.empty());
}

TEST(DropoutMapStageTest, GetDropoutHints_NoChangeOnOtherFrame) {
  auto source =
      std::make_shared<testing::NiceMock<MockVideoFrameRepresentation>>();
  const orc::FrameID fid_mapped{1};
  const orc::FrameID fid_other{7};

  std::vector<orc::DropoutRun> src{{fid_other, 500u, 10u, 64}};
  ON_CALL(*source, get_video_parameters())
      .WillByDefault(Return(make_ntsc_params()));
  ON_CALL(*source, get_dropout_hints(fid_other)).WillByDefault(Return(src));

  orc::FrameDropoutMapEntry entry;
  entry.frame_id = fid_mapped;
  entry.additions.push_back({5u, 0u, 99u});
  std::map<uint64_t, orc::FrameDropoutMapEntry> dmap{{fid_mapped, entry}};
  const orc::DropoutMappedFrameRepresentation rep(source, dmap);

  // Frame 7 is unmodified — should pass through source hints unchanged.
  const auto hints = rep.get_dropout_hints(fid_other);
  ASSERT_EQ(hints.size(), 1u);
  EXPECT_EQ(hints[0].sample_start, 500u);
  EXPECT_EQ(hints[0].sample_count, 10u);
}

TEST(DropoutMapStageTest,
     GetDropoutHints_PassesThroughBothRuns_WhenAdditionEncompassesSource) {
  // The map stage assembles hints faithfully; overlapping hints are passed
  // through as-is so the dropout correction stage can handle them with full
  // context.  Source has a small dropout; addition is wider.  Both runs must
  // appear in the output — the correction stage iterates sample-by-sample and
  // naturally covers the union of all hint ranges.
  auto source =
      std::make_shared<testing::NiceMock<MockVideoFrameRepresentation>>();
  const orc::FrameID fid{0};

  // Source: NTSC line 10, samples 100-200 → sample_start=9200, count=101
  std::vector<orc::DropoutRun> src{{fid, 9200u, 101u, 128}};
  ON_CALL(*source, get_video_parameters())
      .WillByDefault(Return(make_ntsc_params()));
  ON_CALL(*source, get_dropout_hints(fid)).WillByDefault(Return(src));

  // Addition: line 10, start=50, end=250 → sample_start=9150, count=201
  orc::FrameDropoutMapEntry entry;
  entry.frame_id = 0;
  entry.additions.push_back({10u, 50u, 250u});
  std::map<uint64_t, orc::FrameDropoutMapEntry> dmap{{0, entry}};
  const orc::DropoutMappedFrameRepresentation rep(source, dmap);

  const auto hints = rep.get_dropout_hints(fid);
  // Both source and addition runs are returned (sorted by sample_start).
  ASSERT_EQ(hints.size(), 2u);
  EXPECT_EQ(hints[0].sample_start, 9150u);  // addition (wider, starts earlier)
  EXPECT_EQ(hints[0].sample_count, 201u);
  EXPECT_EQ(hints[1].sample_start, 9200u);  // source
  EXPECT_EQ(hints[1].sample_count, 101u);
}

TEST(DropoutMapStageTest,
     GetDropoutHints_PassesThroughBothRuns_WhenAdditionExtendsSourceRight) {
  // Addition starts inside the source and extends it to the right.  Both
  // runs must be returned so the correction stage corrects the full union.
  auto source =
      std::make_shared<testing::NiceMock<MockVideoFrameRepresentation>>();
  const orc::FrameID fid{0};

  // Source: NTSC line 10, samples 100-300 → sample_start=9200, count=201
  std::vector<orc::DropoutRun> src{{fid, 9200u, 201u, 128}};
  ON_CALL(*source, get_video_parameters())
      .WillByDefault(Return(make_ntsc_params()));
  ON_CALL(*source, get_dropout_hints(fid)).WillByDefault(Return(src));

  // Addition: line 10, start=150, end=400 → sample_start=9250, count=251
  orc::FrameDropoutMapEntry entry;
  entry.frame_id = 0;
  entry.additions.push_back({10u, 150u, 400u});
  std::map<uint64_t, orc::FrameDropoutMapEntry> dmap{{0, entry}};
  const orc::DropoutMappedFrameRepresentation rep(source, dmap);

  const auto hints = rep.get_dropout_hints(fid);
  ASSERT_EQ(hints.size(), 2u);
  EXPECT_EQ(hints[0].sample_start, 9200u);  // source (starts earlier)
  EXPECT_EQ(hints[0].sample_count, 201u);
  EXPECT_EQ(hints[1].sample_start, 9250u);  // addition
  EXPECT_EQ(hints[1].sample_count, 251u);
}

TEST(DropoutMapStageTest,
     GetDropoutHints_ReturnsBothRuns_WhenRunsDoNotOverlap) {
  // Two additions on different lines: both runs are returned unchanged.
  auto source =
      std::make_shared<testing::NiceMock<MockVideoFrameRepresentation>>();
  const orc::FrameID fid{0};

  ON_CALL(*source, get_video_parameters())
      .WillByDefault(Return(make_ntsc_params()));
  ON_CALL(*source, get_dropout_hints(fid))
      .WillByDefault(Return(std::vector<orc::DropoutRun>{}));

  orc::FrameDropoutMapEntry entry;
  entry.frame_id = 0;
  entry.additions.push_back({10u, 100u, 200u});  // line 10
  entry.additions.push_back({20u, 100u, 200u});  // line 20
  std::map<uint64_t, orc::FrameDropoutMapEntry> dmap{{0, entry}};
  const orc::DropoutMappedFrameRepresentation rep(source, dmap);

  const auto hints = rep.get_dropout_hints(fid);
  ASSERT_EQ(hints.size(), 2u);
}

}  // namespace orc_unit_test
