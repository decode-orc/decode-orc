/*
 * File:        node_reset_test.cpp
 * Module:      orc-presenters unit tests
 * Purpose:     Unit tests for what a parameter reset gives each parameter,
 *              shared by the parameter dialogue and `orc-cli nodes reset`
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "node_reset.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace orc::presenters::test {

namespace {

ParameterDescriptor int_descriptor(const std::string& name,
                                   std::optional<int32_t> default_value) {
  ParameterDescriptor descriptor;
  descriptor.name = name;
  descriptor.display_name = name;
  descriptor.type = ParameterType::INT32;
  if (default_value) {
    descriptor.constraints.default_value = *default_value;
  }
  return descriptor;
}

std::vector<ParameterDescriptor> video_params_descriptors() {
  std::vector<ParameterDescriptor> descriptors;
  for (const char* name :
       {"activeVideoStart", "activeVideoEnd", "firstActiveFrameLine",
        "lastActiveFrameLine", "whiteLevel", "blackLevel"}) {
    descriptors.push_back(int_descriptor(name, 0));
  }
  return descriptors;
}

orc::SourceParameters source_parameters() {
  orc::SourceParameters params{};
  params.active_video_start = 185;
  params.active_video_end = 1107;
  params.first_active_frame_line = 44;
  params.last_active_frame_line = 620;
  params.white_level = 840;
  params.black_level = 260;
  return params;
}

int32_t int_value(const std::map<std::string, ParameterValue>& values,
                  const std::string& name) {
  return std::get<int32_t>(values.at(name));
}

}  // namespace

TEST(NodeResetTest, OnlyVideoParametersResetsToSourceMetadata) {
  EXPECT_TRUE(stageResetsToSourceMetadata("video_params"));
  EXPECT_FALSE(stageResetsToSourceMetadata("frame_map"));
  EXPECT_FALSE(stageResetsToSourceMetadata("tbc_source"));
}

TEST(NodeResetTest, SourceFiguresMapToEveryVideoParametersKey) {
  const auto values =
      sourceParametersToVideoParamsStageValues(source_parameters());
  EXPECT_EQ(values.size(), 6u);
  EXPECT_EQ(int_value(values, "activeVideoStart"), 185);
  EXPECT_EQ(int_value(values, "activeVideoEnd"), 1107);
  EXPECT_EQ(int_value(values, "firstActiveFrameLine"), 44);
  EXPECT_EQ(int_value(values, "lastActiveFrameLine"), 620);
  EXPECT_EQ(int_value(values, "whiteLevel"), 840);
  EXPECT_EQ(int_value(values, "blackLevel"), 260);
}

TEST(NodeResetTest, InputNodeIsTheSourceOfTheEdgeIntoTheNode) {
  const std::vector<EdgeInfo> edges{{NodeID(1), NodeID(2)},
                                    {NodeID(2), NodeID(3)}};
  EXPECT_EQ(resetInputNode(edges, NodeID(3)), NodeID(2));
  EXPECT_EQ(resetInputNode(edges, NodeID(2)), NodeID(1));
}

TEST(NodeResetTest, ANodeWithNothingConnectedIsItsOwnInput) {
  const std::vector<EdgeInfo> edges{{NodeID(1), NodeID(2)}};
  EXPECT_EQ(resetInputNode(edges, NodeID(1)), NodeID(1));
}

TEST(NodeResetTest, ResetValuesWinThenDefaultsAndNeitherLeavesTheKeyOut) {
  const std::vector<ParameterDescriptor> descriptors{
      int_descriptor("from_reset", 1), int_descriptor("from_default", 2),
      int_descriptor("no_default", std::nullopt)};
  const std::map<std::string, ParameterValue> reset_values{
      {"from_reset", static_cast<int32_t>(10)},
      {"undeclared", static_cast<int32_t>(99)}};

  const auto values = resetParameterValues(descriptors, reset_values);
  EXPECT_EQ(values.size(), 2u);
  EXPECT_EQ(int_value(values, "from_reset"), 10);
  EXPECT_EQ(int_value(values, "from_default"), 2);
  EXPECT_EQ(values.count("no_default"), 0u);
  // A reset value for a key the stage does not declare is not a parameter.
  EXPECT_EQ(values.count("undeclared"), 0u);
}

TEST(NodeResetTest, AnOrdinaryStageResetsToItsDefaultsAndKeepsTheRest) {
  const std::vector<ParameterDescriptor> descriptors{
      int_descriptor("threshold", 100),
      int_descriptor("input_path_like", std::nullopt)};
  const std::map<std::string, ParameterValue> stored{
      {"threshold", static_cast<int32_t>(42)},
      {"input_path_like", static_cast<int32_t>(7)}};

  bool read_called = false;
  const auto plan = planNodeReset("dropout_correct", descriptors, stored, [&] {
    read_called = true;
    return std::optional<orc::SourceParameters>(source_parameters());
  });

  EXPECT_TRUE(plan.problem.empty());
  EXPECT_FALSE(plan.from_source_metadata);
  EXPECT_FALSE(read_called) << "only Video Parameters reads its input";
  EXPECT_EQ(plan.reset_count, 1u);
  EXPECT_EQ(int_value(plan.values, "threshold"), 100);
  EXPECT_EQ(int_value(plan.values, "input_path_like"), 7);
}

TEST(NodeResetTest, VideoParametersResetsToWhatItsInputReports) {
  std::map<std::string, ParameterValue> stored{
      {"blackLevel", static_cast<int32_t>(300)}};
  const auto plan = planNodeReset(
      "video_params", video_params_descriptors(), stored,
      [] { return std::optional<orc::SourceParameters>(source_parameters()); });

  EXPECT_TRUE(plan.problem.empty());
  EXPECT_TRUE(plan.from_source_metadata);
  EXPECT_EQ(plan.reset_count, 6u);
  EXPECT_EQ(int_value(plan.values, "blackLevel"), 260);
  EXPECT_EQ(int_value(plan.values, "activeVideoStart"), 185);
}

TEST(NodeResetTest, VideoParametersWithAnUnreadableInputIsAProblem) {
  // Falling back to the format's figures would give a script values that only
  // look right, so the reset is refused and nothing is planned.
  const auto plan =
      planNodeReset("video_params", video_params_descriptors(), {},
                    [] { return std::optional<orc::SourceParameters>(); });

  EXPECT_FALSE(plan.problem.empty());
  EXPECT_TRUE(plan.values.empty());
  EXPECT_EQ(plan.reset_count, 0u);
}

}  // namespace orc::presenters::test
