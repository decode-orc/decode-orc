/*
 * File:        node_editing_test.cpp
 * Module:      orc-presenters unit tests
 * Purpose:     Unit tests for node addressing and text-to-parameter
 *              conversion behind `orc-cli nodes`
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "node_editing.h"

#include <gtest/gtest.h>
#include <stage_ux_strings.h>

#include <cstdint>
#include <string>
#include <vector>

namespace orc::presenters::test {

namespace {

NodeInfo make_node(int32_t id, const std::string& label,
                   const std::string& stage) {
  return NodeInfo{NodeID(id), stage, label, 0.0, 0.0, true, true, "", ""};
}

std::vector<NodeInfo> sample_nodes() {
  return {make_node(1, "Disc A", "tbc_source"),
          make_node(2, "Disc A map", "frame_map"),
          make_node(12, "Video Sink", "video_sink")};
}

ParameterDescriptor make_descriptor(const std::string& name,
                                    ParameterType type) {
  ParameterDescriptor descriptor;
  descriptor.name = name;
  descriptor.display_name = name;
  descriptor.type = type;
  return descriptor;
}

ParameterDescriptor ranged_int(const std::string& name, int32_t min_value,
                               int32_t max_value) {
  auto descriptor = make_descriptor(name, ParameterType::INT32);
  descriptor.constraints.min_value = min_value;
  descriptor.constraints.max_value = max_value;
  return descriptor;
}

ParameterDescriptor choice(const std::string& name,
                           std::vector<std::string> allowed) {
  auto descriptor = make_descriptor(name, ParameterType::STRING);
  descriptor.constraints.allowed_strings = std::move(allowed);
  return descriptor;
}

}  // namespace

// --- resolveNodeAddress ----------------------------------------------------

TEST(NodeEditingTest, ResolveNodeAddress_ByLabel) {
  const auto result = resolveNodeAddress(sample_nodes(), "Disc A map");
  ASSERT_TRUE(result.found);
  EXPECT_EQ(result.node_id, NodeID(2));
}

TEST(NodeEditingTest, ResolveNodeAddress_ById) {
  const auto result = resolveNodeAddress(sample_nodes(), "12");
  ASSERT_TRUE(result.found);
  EXPECT_EQ(result.node_id, NodeID(12));
}

TEST(NodeEditingTest, ResolveNodeAddress_LabelMatchIsExact) {
  const auto result = resolveNodeAddress(sample_nodes(), "disc a");
  EXPECT_FALSE(result.found);
}

TEST(NodeEditingTest, ResolveNodeAddress_UnknownLabelListsTheStages) {
  const auto result = resolveNodeAddress(sample_nodes(), "Disc B");
  ASSERT_FALSE(result.found);
  EXPECT_NE(result.error.find("'Disc B'"), std::string::npos);
  EXPECT_NE(result.error.find("Disc A map"), std::string::npos);
  EXPECT_NE(result.error.find("Video Sink"), std::string::npos);
}

TEST(NodeEditingTest, ResolveNodeAddress_UnknownIdIsNotFound) {
  const auto result = resolveNodeAddress(sample_nodes(), "7");
  ASSERT_FALSE(result.found);
  EXPECT_NE(result.error.find("id 7"), std::string::npos);
}

// --- parseParameterText ----------------------------------------------------

TEST(NodeEditingTest, ParseParameterText_ConvertsToTheDeclaredType) {
  std::string error;
  const auto flag = parseParameterText(
      make_descriptor("flag", ParameterType::BOOL), "Yes", &error);
  ASSERT_TRUE(flag.has_value());
  EXPECT_EQ(std::get<bool>(*flag), true);

  const auto count = parseParameterText(
      make_descriptor("count", ParameterType::INT32), " -4 ", &error);
  ASSERT_TRUE(count.has_value());
  EXPECT_EQ(std::get<int32_t>(*count), -4);

  const auto gain = parseParameterText(
      make_descriptor("gain", ParameterType::DOUBLE), "1.5", &error);
  ASSERT_TRUE(gain.has_value());
  EXPECT_DOUBLE_EQ(std::get<double>(*gain), 1.5);
}

TEST(NodeEditingTest, ParseParameterText_RejectsTextThatDoesNotFit) {
  std::string error;
  EXPECT_FALSE(parseParameterText(make_descriptor("n", ParameterType::INT32),
                                  "12abc", &error)
                   .has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(parseParameterText(make_descriptor("n", ParameterType::UINT32),
                                  "-1", &error)
                   .has_value());
  EXPECT_FALSE(parseParameterText(make_descriptor("b", ParameterType::BOOL),
                                  "maybe", &error)
                   .has_value());
}

TEST(NodeEditingTest, ParseParameterText_KeepsPathsExactlyAsTyped) {
  std::string error;
  const auto path = parseParameterText(
      make_descriptor("input_path", ParameterType::FILE_PATH),
      " ${PROJECT_ROOT}/disc 1.tbc", &error);
  ASSERT_TRUE(path.has_value());
  EXPECT_EQ(std::get<std::string>(*path), " ${PROJECT_ROOT}/disc 1.tbc");
}

// --- checkParameterConstraints ---------------------------------------------

TEST(NodeEditingTest, CheckParameterConstraints_EnforcesTheRange) {
  const auto crf = ranged_int("encoder_crf", 0, 51);
  std::string error;
  EXPECT_TRUE(checkParameterConstraints(crf, int32_t{51}, &error));
  EXPECT_FALSE(checkParameterConstraints(crf, int32_t{52}, &error));
  EXPECT_NE(error.find("0 to 51"), std::string::npos);
  EXPECT_FALSE(checkParameterConstraints(crf, int32_t{-1}, &error));
}

TEST(NodeEditingTest, CheckParameterConstraints_ComparesOnlyTheStoredValue) {
  // An allowed entry may carry a display label after the separator; the
  // label is never what a project stores.
  const std::string labelled = std::string("pal2d") +
                               stage_ux::kComboValueLabelSeparator +
                               "PAL 2D comb filter";
  const auto decoder = choice("decoder_type", {labelled, "transform3d"});
  std::string error;
  EXPECT_TRUE(checkParameterConstraints(decoder, std::string("pal2d"), &error));
  EXPECT_TRUE(
      checkParameterConstraints(decoder, std::string("transform3d"), &error));
  EXPECT_FALSE(checkParameterConstraints(
      decoder, std::string("PAL 2D comb filter"), &error));
  EXPECT_NE(error.find("'pal2d'"), std::string::npos);
  EXPECT_EQ(error.find(stage_ux::kComboValueLabelSeparator), std::string::npos);
}

TEST(NodeEditingTest, CheckParameterConstraints_FreeStringsPass) {
  std::string error;
  EXPECT_TRUE(
      checkParameterConstraints(make_descriptor("note", ParameterType::STRING),
                                std::string("x"), &error));
}

// --- parseParameterAssignments ---------------------------------------------

TEST(NodeEditingTest, ParseParameterAssignments_TypesEveryValue) {
  const std::vector<ParameterDescriptor> descriptors = {
      make_descriptor("input_path", ParameterType::FILE_PATH),
      ranged_int("encoder_crf", 0, 51)};

  const auto result =
      parseParameterAssignments(descriptors, descriptors, "composite",
                                {"input_path=a=b.tbc", "encoder_crf=20"});

  EXPECT_TRUE(result.unknown_keys.empty());
  EXPECT_TRUE(result.errors.empty());
  ASSERT_EQ(result.values.size(), 2U);
  // Only the first '=' separates key from value
  EXPECT_EQ(std::get<std::string>(result.values.at("input_path")), "a=b.tbc");
  EXPECT_EQ(std::get<int32_t>(result.values.at("encoder_crf")), 20);
}

TEST(NodeEditingTest, ParseParameterAssignments_RefusesAnUnknownKey) {
  const std::vector<ParameterDescriptor> descriptors = {
      make_descriptor("input_path", ParameterType::FILE_PATH)};

  const auto result = parseParameterAssignments(descriptors, descriptors,
                                                "composite", {"inptu_path=x"});

  ASSERT_EQ(result.unknown_keys.size(), 1U);
  EXPECT_NE(result.unknown_keys[0].find("not a parameter of this stage"),
            std::string::npos);
  EXPECT_TRUE(result.values.empty());
}

TEST(NodeEditingTest, ParseParameterAssignments_ExplainsAWrongFormatKey) {
  // A Y/C project's source offers y_path and c_path, not input_path
  const std::vector<ParameterDescriptor> yc = {
      make_descriptor("y_path", ParameterType::FILE_PATH),
      make_descriptor("c_path", ParameterType::FILE_PATH)};
  auto any_format = yc;
  any_format.push_back(make_descriptor("input_path", ParameterType::FILE_PATH));

  const auto result =
      parseParameterAssignments(yc, any_format, "Y/C", {"input_path=a.tbc"});

  ASSERT_EQ(result.unknown_keys.size(), 1U);
  EXPECT_NE(result.unknown_keys[0].find("does not apply to a Y/C project"),
            std::string::npos);
}

TEST(NodeEditingTest, ParseParameterAssignments_ReportsBadValuesAndSyntax) {
  const std::vector<ParameterDescriptor> descriptors = {
      ranged_int("encoder_crf", 0, 51)};

  const auto result =
      parseParameterAssignments(descriptors, descriptors, "composite",
                                {"encoder_crf=99", "encoder_crf", "=5"});

  EXPECT_EQ(result.errors.size(), 3U);
  EXPECT_TRUE(result.values.empty());
}

TEST(NodeEditingTest, ConfigurationStatusName_MatchesTheGuiColours) {
  EXPECT_EQ(configurationStatusName(orc::ConfigurationStatus::Green), "green");
  EXPECT_EQ(configurationStatusName(orc::ConfigurationStatus::Yellow),
            "yellow");
  EXPECT_EQ(configurationStatusName(orc::ConfigurationStatus::Red), "red");
}

}  // namespace orc::presenters::test
