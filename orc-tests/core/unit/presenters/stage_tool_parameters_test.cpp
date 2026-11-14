/*
 * File:        stage_tool_parameters_test.cpp
 * Module:      orc-presenters unit tests
 * Purpose:     A plugin view's parameter write-back is merged into the node's
 *              parameters only when every value fits what the stage declares
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "../../../../orc/presenters/include/stage_tool_parameters.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace orc_unit_test {
namespace {

using orc::ParameterDescriptor;
using orc::ParameterType;
using orc::ParameterValue;
using orc::presenters::mergeToolParameters;

ParameterDescriptor declared(const std::string& name, ParameterType type) {
  ParameterDescriptor descriptor;
  descriptor.name = name;
  descriptor.type = type;
  return descriptor;
}

std::vector<ParameterDescriptor> mapStageParameters() {
  auto offset = declared("offset", ParameterType::INT32);
  offset.constraints.min_value = int32_t{-100};
  offset.constraints.max_value = int32_t{100};
  auto mode = declared("mode", ParameterType::STRING);
  mode.constraints.allowed_strings = {"cav", "clv"};
  return {offset,
          mode,
          declared("ranges", ParameterType::STRING),
          declared("frames", ParameterType::UINT32),
          declared("gain", ParameterType::DOUBLE),
          declared("enabled", ParameterType::BOOL),
          declared(orc::kInputNodeIdsParameter, ParameterType::STRING)};
}

struct Merge {
  bool ok = false;
  std::map<std::string, ParameterValue> merged;
  std::string error;
};

Merge merge(const std::map<std::string, ParameterValue>& current,
            const orc::view::ToolParameterMap& values) {
  Merge result;
  result.merged = {{"untouched", std::string("sentinel")}};
  result.ok = mergeToolParameters(mapStageParameters(), current, values,
                                  &result.merged, &result.error);
  return result;
}

}  // namespace

TEST(StageToolParametersTest, MergesTheValuesKeepingTheOthers) {
  const auto result =
      merge({{"ranges", std::string("0-10")}, {"offset", int32_t{3}}},
            {{"ranges", "0-20,30-40"}, {"enabled", "true"}});

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(std::get<std::string>(result.merged.at("ranges")), "0-20,30-40");
  EXPECT_TRUE(std::get<bool>(result.merged.at("enabled")));
  EXPECT_EQ(std::get<int32_t>(result.merged.at("offset")), 3);
  EXPECT_EQ(result.merged.count("untouched"), 0u);
}

TEST(StageToolParametersTest, ConvertsEachValueToItsDeclaredType) {
  const auto result =
      merge({}, {{"offset", "-7"}, {"frames", "54000"}, {"gain", "0.25"}});

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(std::get<int32_t>(result.merged.at("offset")), -7);
  EXPECT_EQ(std::get<uint32_t>(result.merged.at("frames")), 54000u);
  EXPECT_DOUBLE_EQ(std::get<double>(result.merged.at("gain")), 0.25);
}

TEST(StageToolParametersTest, RefusesAParameterTheStageDoesNotDeclare) {
  const auto result = merge({}, {{"ranges", "1-2"}, {"rnages", "1-2"}});

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("rnages"), std::string::npos);
  EXPECT_EQ(result.merged.count("untouched"), 1u) << "nothing is merged";
}

TEST(StageToolParametersTest, RefusesTheHostOwnedInputNodeIds) {
  const auto result = merge({}, {{orc::kInputNodeIdsParameter, "1,2"}});

  EXPECT_FALSE(result.ok);
}

TEST(StageToolParametersTest, RefusesANumberWithTrailingText) {
  EXPECT_FALSE(merge({}, {{"offset", "12abc"}}).ok);
  EXPECT_FALSE(merge({}, {{"gain", "0.5x"}}).ok);
  EXPECT_FALSE(merge({}, {{"frames", "-1"}}).ok);
  EXPECT_FALSE(merge({}, {{"enabled", "maybe"}}).ok);
}

TEST(StageToolParametersTest, RefusesAValueOutsideTheDeclaredRange) {
  EXPECT_FALSE(merge({}, {{"offset", "101"}}).ok);
  EXPECT_FALSE(merge({}, {{"offset", "-101"}}).ok);
  EXPECT_TRUE(merge({}, {{"offset", "100"}}).ok);
}

TEST(StageToolParametersTest, RefusesAStringNotInTheDeclaredList) {
  EXPECT_FALSE(merge({}, {{"mode", "ntsc"}}).ok);
  EXPECT_TRUE(merge({}, {{"mode", "clv"}}).ok);
}

// --- Stored parameters as a view reads them
// ----------------------------------------------------

TEST(ToolParameterTextTest, WritesEachValueAsTheWriteBackReadsIt) {
  const std::map<std::string, ParameterValue> stored = {
      {"enabled", true},
      {"offset", int32_t{-12}},
      {"count", uint32_t{7}},
      {"gain", 0.1},
      {"tiny", 1e-7},
      {"mode", std::string("clv")},
      {orc::kInputNodeIdsParameter, std::string("node_1,node_2")}};

  const auto text = orc::presenters::toolParameterText(stored);

  EXPECT_EQ(text.at("enabled"), "true");
  EXPECT_EQ(text.at("offset"), "-12");
  EXPECT_EQ(text.at("count"), "7");
  // Not std::to_string's "0.100000", and 1e-7 is not rounded to zero.
  EXPECT_EQ(text.at("gain"), "0.1");
  EXPECT_EQ(std::stod(text.at("tiny")), 1e-7);
  EXPECT_EQ(text.at("mode"), "clv");
  EXPECT_EQ(text.count(orc::kInputNodeIdsParameter), 0u);
}

TEST(ToolParameterTextTest, ReadsBackToTheSameValues) {
  const std::map<std::string, ParameterValue> stored = {
      {"offset", int32_t{42}}, {"mode", std::string("cav")}};
  std::map<std::string, ParameterValue> merged;
  std::string error;
  ASSERT_TRUE(mergeToolParameters(mapStageParameters(), {},
                                  orc::presenters::toolParameterText(stored),
                                  &merged, &error))
      << error;
  EXPECT_EQ(merged, stored);
}

}  // namespace orc_unit_test
