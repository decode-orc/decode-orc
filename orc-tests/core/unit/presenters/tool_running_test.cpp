/*
 * File:        tool_running_test.cpp
 * Module:      orc-presenters unit tests
 * Purpose:     Unit tests for the checks behind `orc-cli tools run`: how a
 *              tool runs without the GUI, the defaults it starts from, and
 *              the upstream refusals
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "tool_running.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace orc::presenters::test {

namespace {

NodeInfo make_node(int32_t id, const std::string& label,
                   const std::string& stage) {
  return NodeInfo{NodeID(id), stage, label, 0.0, 0.0, true, true, "", ""};
}

EdgeInfo edge(int32_t from, int32_t to) { return {NodeID(from), NodeID(to)}; }

orc::AnalysisToolInfo make_tool(const std::string& id,
                                const std::string& kind) {
  orc::AnalysisToolInfo tool;
  tool.id = id;
  tool.name = id + " name";
  tool.priority = 1;
  tool.stage_tool_kind = kind;
  return tool;
}

/// Two discs, each mapped, aligned into one stack.
struct Stack {
  std::vector<NodeInfo> nodes;
  std::vector<EdgeInfo> edges;
};

Stack two_disc_stack(const std::string& first_source,
                     const std::string& second_source) {
  return {
      {make_node(1, "disc1", first_source),
       make_node(2, "disc2", second_source), make_node(3, "map1", "frame_map"),
       make_node(4, "map2", "frame_map"), make_node(5, "align", "source_align"),
       make_node(6, "stack", "stacker")},
      {edge(1, 3), edge(2, 4), edge(3, 5), edge(4, 5), edge(5, 6)}};
}

const auto kAllConfigured = [](NodeID) { return true; };

}  // namespace

TEST(ToolRunningTest, AnalysisToolsRunAndInteractiveOnesAreRefused) {
  EXPECT_EQ(toolRunSupport(make_tool("field_mapping", "")).mode,
            ToolRunMode::Analysis);
  EXPECT_EQ(toolRunSupport(make_tool("snr_analysis", "batch_analysis")).mode,
            ToolRunMode::BatchAnalysis);

  for (const auto* kind : {"config_dialog", "non_modal_editor",
                           "preview_utility", "catalogue_browser"}) {
    const auto support = toolRunSupport(make_tool("t", kind));
    EXPECT_EQ(support.mode, ToolRunMode::GuiOnly) << kind;
    EXPECT_FALSE(support.reason.empty()) << kind;
  }
  EXPECT_NE(toolRunSupport(make_tool("ffmpeg_preset_config", "config_dialog"))
                .reason.find("nodes set"),
            std::string::npos);
}

TEST(ToolRunningTest, KindNameCallsAPlainToolAnAnalysis) {
  EXPECT_EQ(toolKindName(make_tool("field_mapping", "")), "analysis");
  EXPECT_EQ(toolKindName(make_tool("dropout_editor", "non_modal_editor")),
            "non_modal_editor");
}

TEST(ToolRunningTest, DefaultsFillOnlyWhatWasNotGiven) {
  ParameterDescriptor with_default;
  with_default.name = "include_lead_in_out";
  with_default.type = ParameterType::BOOL;
  with_default.constraints.default_value = true;
  ParameterDescriptor no_default;
  no_default.name = "startAddress";
  no_default.type = ParameterType::STRING;

  const auto filled = withParameterDefaults({with_default, no_default}, {});
  ASSERT_EQ(filled.size(), 1u);
  EXPECT_EQ(std::get<bool>(filled.at("include_lead_in_out")), true);

  const auto given = withParameterDefaults(
      {with_default}, {{"include_lead_in_out", ParameterValue{false}}});
  EXPECT_EQ(std::get<bool>(given.at("include_lead_in_out")), false);
}

TEST(ToolRunningTest, StreamSourcesAreRecognisedByName) {
  EXPECT_TRUE(isStreamSourceStage("pal_tbc_stream_source"));
  EXPECT_TRUE(isStreamSourceStage("ntsc_cvbs_stream_source"));
  EXPECT_FALSE(isStreamSourceStage("tbc_source"));
  EXPECT_FALSE(isStreamSourceStage("PAL_CVBS_Source"));
}

TEST(ToolRunningTest, UpstreamNodesAreNearestFirstAndExcludeTheNode) {
  const auto stack = two_disc_stack("tbc_source", "tbc_source");

  const auto upstream = upstreamNodes(stack.edges, NodeID(6));
  ASSERT_EQ(upstream.size(), 5u);
  EXPECT_EQ(upstream.front(), NodeID(5));
  EXPECT_EQ(std::set<NodeID>(upstream.begin(), upstream.end()),
            (std::set<NodeID>{NodeID(1), NodeID(2), NodeID(3), NodeID(4),
                              NodeID(5)}));

  EXPECT_TRUE(upstreamNodes(stack.edges, NodeID(1)).empty());
}

TEST(ToolRunningTest, AStackOfOneCaptureFormatIsAccepted) {
  for (const auto* source : {"tbc_source", "NTSC_CVBS_Source"}) {
    const auto stack = two_disc_stack(source, source);
    EXPECT_FALSE(
        checkToolUpstream(stack.nodes, stack.edges, NodeID(5), kAllConfigured))
        << source;
  }
}

TEST(ToolRunningTest, MixedTbcAndCvbsSourcesAreRefusedByName) {
  const auto stack = two_disc_stack("tbc_source", "PAL_CVBS_Source");

  const auto refusal =
      checkToolUpstream(stack.nodes, stack.edges, NodeID(5), kAllConfigured);
  ASSERT_TRUE(refusal);
  EXPECT_NE(refusal->find("'disc1' (tbc_source)"), std::string::npos);
  EXPECT_NE(refusal->find("'disc2' (PAL_CVBS_Source)"), std::string::npos);

  // One disc's own frame map reads one source, so nothing is mixed there.
  EXPECT_FALSE(
      checkToolUpstream(stack.nodes, stack.edges, NodeID(3), kAllConfigured));
}

TEST(ToolRunningTest, AStreamSourceUpstreamIsRefused) {
  const auto stack = two_disc_stack("pal_tbc_stream_source", "tbc_source");

  const auto refusal =
      checkToolUpstream(stack.nodes, stack.edges, NodeID(3), kAllConfigured);
  ASSERT_TRUE(refusal);
  EXPECT_NE(refusal->find("stream source"), std::string::npos);
  EXPECT_NE(refusal->find("'disc1'"), std::string::npos);
}

TEST(ToolRunningTest, AnUnconfiguredSourceIsRefused) {
  const auto stack = two_disc_stack("tbc_source", "tbc_source");

  const auto refusal =
      checkToolUpstream(stack.nodes, stack.edges, NodeID(5),
                        [](NodeID id) { return id != NodeID(2); });
  ASSERT_TRUE(refusal);
  EXPECT_NE(refusal->find("not configured"), std::string::npos);
  EXPECT_NE(refusal->find("'disc2'"), std::string::npos);
  EXPECT_EQ(refusal->find("'disc1'"), std::string::npos);
}

TEST(ToolRunningTest, OnlySourcesAreCheckedForConfiguration) {
  // The frame maps are unconfigured until their tool runs; that must not
  // stop the tool that configures them, or the alignment downstream.
  const auto stack = two_disc_stack("tbc_source", "tbc_source");

  EXPECT_FALSE(checkToolUpstream(
      stack.nodes, stack.edges, NodeID(5),
      [](NodeID id) { return id == NodeID(1) || id == NodeID(2); }));
}

}  // namespace orc::presenters::test
