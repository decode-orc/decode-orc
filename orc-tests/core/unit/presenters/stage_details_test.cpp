/*
 * File:        stage_details_test.cpp
 * Module:      orc-tests/core/unit
 * Purpose:     Unit tests for the shared stage and parameter description
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "stage_details.h"

#include <gtest/gtest.h>
#include <stage_ux_strings.h>

#include <algorithm>
#include <string>
#include <vector>

namespace orc::presenters {
namespace {

StageInfo makeStage(const std::string& name, orc::NodeType type) {
  StageInfo stage;
  stage.name = name;
  stage.display_name = "Display " + name;
  stage.description = "Describes " + name;
  stage.node_type = type;
  stage.is_source = (type == orc::NodeType::SOURCE);
  stage.is_sink =
      (type == orc::NodeType::SINK || type == orc::NodeType::ANALYSIS_SINK);
  stage.is_core_plugin = true;
  stage.owning_plugin_id = "com.decode-orc." + name;
  return stage;
}

orc::ParameterDescriptor makeStringParameter(const std::string& name,
                                             const std::string& default_value) {
  orc::ParameterDescriptor descriptor;
  descriptor.name = name;
  descriptor.display_name = "Display " + name;
  descriptor.description = "Describes " + name;
  descriptor.type = orc::ParameterType::STRING;
  descriptor.constraints.default_value = default_value;
  return descriptor;
}

std::string valueOf(const std::vector<StageDetailField>& fields,
                    const std::string& label) {
  const auto it = std::find_if(
      fields.begin(), fields.end(),
      [&label](const StageDetailField& f) { return f.label == label; });
  return it == fields.end() ? std::string() : it->value;
}

bool hasLabel(const std::vector<StageDetailField>& fields,
              const std::string& label) {
  return std::any_of(
      fields.begin(), fields.end(),
      [&label](const StageDetailField& f) { return f.label == label; });
}

// --- Kinds ------------------------------------------------------------------

TEST(StageDetailsTest, KindLabelsMatchTheStageMenuCategories) {
  EXPECT_STREQ(stageKindLabel(makeStage("src", orc::NodeType::SOURCE)),
               "Source");
  EXPECT_STREQ(stageKindLabel(makeStage("xform", orc::NodeType::TRANSFORM)),
               "Transform");
  EXPECT_STREQ(stageKindLabel(makeStage("merge", orc::NodeType::MERGER)),
               "Transform");
  EXPECT_STREQ(stageKindLabel(makeStage("an", orc::NodeType::ANALYSIS_SINK)),
               "Analysis");
  EXPECT_STREQ(stageKindLabel(makeStage("sink", orc::NodeType::SINK)), "Sink");
}

TEST(StageDetailsTest, KindArgumentAcceptsEveryPrintedLabelLowercased) {
  const StageInfo stages[] = {
      makeStage("src", orc::NodeType::SOURCE),
      makeStage("xform", orc::NodeType::TRANSFORM),
      makeStage("an", orc::NodeType::ANALYSIS_SINK),
      makeStage("sink", orc::NodeType::SINK),
  };
  for (const auto& stage : stages) {
    orc::StageCategory kind{};
    ASSERT_TRUE(parseStageKind(stageKindId(stage), &kind)) << stage.name;
    EXPECT_TRUE(stageMatchesKind(stage, kind)) << stage.name;
  }
}

TEST(StageDetailsTest, KindArgumentHasOneWordForTransforms) {
  orc::StageCategory kind{};
  ASSERT_TRUE(parseStageKind("TRANSFORM", &kind));
  EXPECT_EQ(kind, orc::StageCategory::TRANSFORM);
  EXPECT_FALSE(parseStageKind("filter", &kind));
  EXPECT_FALSE(parseStageKind("filters", &kind));
}

TEST(StageDetailsTest, UnknownKindIsRejected) {
  orc::StageCategory kind{};
  EXPECT_FALSE(parseStageKind("decoder", &kind));
  EXPECT_FALSE(parseStageKind("", &kind));
}

// --- Stage fields -----------------------------------------------------------

TEST(StageDetailsTest, StageNameLeadsAndDisplayNameIsASeparateField) {
  const auto stage = makeStage("tbc_source", orc::NodeType::SOURCE);
  const auto fields = makeStageDetails(stage);

  ASSERT_FALSE(fields.empty());
  EXPECT_EQ(fields.front().label, stage_ux::kFieldName);
  EXPECT_EQ(fields.front().value, "tbc_source");
  EXPECT_EQ(valueOf(fields, stage_ux::kFieldDisplayName), "Display tbc_source");
  EXPECT_EQ(valueOf(fields, stage_ux::kFieldKind), "Source");
  EXPECT_EQ(valueOf(fields, stage_ux::kFieldPlugin),
            "com.decode-orc.tbc_source");
}

TEST(StageDetailsTest, StageWithNoOwningPluginPrintsNoPluginField) {
  auto stage = makeStage("host_stage", orc::NodeType::TRANSFORM);
  stage.owning_plugin_id.clear();

  // A plugin field would have to hold a plugin selector; there is none, and a
  // placeholder is not a selector.
  EXPECT_FALSE(hasLabel(makeStageDetails(stage), stage_ux::kFieldPlugin));
}

// --- Parameter fields -------------------------------------------------------

TEST(StageDetailsTest, ParameterFieldsCarryTheDescriptorVerbatim) {
  orc::ParameterDescriptor descriptor;
  descriptor.name = "overcorrect_extension";
  descriptor.display_name = "Overcorrect Extension";
  descriptor.description = "How far to extend a correction";
  descriptor.type = orc::ParameterType::INT32;
  descriptor.constraints.required = true;
  descriptor.constraints.default_value = static_cast<int32_t>(4);
  descriptor.constraints.min_value = static_cast<int32_t>(0);
  descriptor.constraints.max_value = static_cast<int32_t>(16);

  const auto fields = makeStageParameterDetails("dropout_correct", descriptor);

  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldDisplayName),
            "Overcorrect Extension");
  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldDescription),
            "How far to extend a correction");
  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldType), "int32");
  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldRequired), "yes");
  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldDefault), "4");
  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldMinimum), "0");
  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldMaximum), "16");
}

TEST(StageDetailsTest, AllowedValuesLeadWithTheStoredValueNotTheLabel) {
  auto descriptor = makeStringParameter("output_mode", "raw");
  descriptor.constraints.allowed_strings = {
      "raw", std::string("ffmpeg") + stage_ux::kComboValueLabelSeparator +
                 "FFmpeg encoder"};

  const auto fields = makeStageParameterDetails("video_sink", descriptor);

  // The value is what has to be typed into a project file; the label is only
  // what the GUI combo box shows.
  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldAllowed),
            "raw, ffmpeg (FFmpeg encoder)");
}

TEST(StageDetailsTest, DependencyNamesTheParameterAndItsEnablingValues) {
  auto descriptor = makeStringParameter("ffmpeg_format", "mp4");
  orc::ParameterDependency dependency;
  dependency.parameter_name = "output_mode";
  dependency.required_values = {"ffmpeg"};
  descriptor.constraints.depends_on = dependency;

  EXPECT_EQ(valueOf(makeStageParameterDetails("video_sink", descriptor),
                    stage_ux::kParamFieldDependsOn),
            "output_mode = ffmpeg");
}

TEST(StageDetailsTest, IndexedSpecDefaultIsShownOneBasedLikeTheGuiDialog) {
  // frame_map's "ranges" is stored 0-based and presented 1-based; the CLI must
  // not print a raw 0-based value where the GUI dialog prints a 1-based one.
  const auto descriptor = makeStringParameter("ranges", "0-9,20-29");
  const auto fields = makeStageParameterDetails("frame_map", descriptor);

  EXPECT_EQ(valueOf(fields, stage_ux::kParamFieldDefault), "1-10,21-30");
  EXPECT_TRUE(stageHasIndexedSpecParameter("frame_map", {descriptor}));
}

TEST(StageDetailsTest, StoredDefaultKeepsTheZeroBasedValue) {
  // The 1-based presentation is for people; JSON output and `nodes reset` are
  // read back by a script or the project loader, which expect stored values.
  const auto descriptor = makeStringParameter("ranges", "0-9");

  EXPECT_EQ(stageParameterStoredDefault(descriptor), "0-9");
}

TEST(StageDetailsTest, ParameterWithNoIndexedSpecNeedsNoNote) {
  EXPECT_FALSE(stageHasIndexedSpecParameter(
      "dropout_correct", {makeStringParameter("mode", "auto")}));
}

// --- Near matches -----------------------------------------------------------

TEST(StageDetailsTest, NearMatchesRankContainmentAboveDistance) {
  const std::vector<StageInfo> stages = {
      makeStage("tbc_source", orc::NodeType::SOURCE),
      makeStage("cvbs_source", orc::NodeType::SOURCE),
      makeStage("video_sink", orc::NodeType::SINK),
  };

  const auto near = findNearbyStageNames("source", stages, 5);
  ASSERT_GE(near.size(), 2u);
  EXPECT_TRUE(std::find(near.begin(), near.end(), "tbc_source") != near.end());
  EXPECT_TRUE(std::find(near.begin(), near.end(), "cvbs_source") != near.end());
}

TEST(StageDetailsTest, NearMatchesCatchATypoAndRespectTheResultLimit) {
  const std::vector<StageInfo> stages = {
      makeStage("tbc_source", orc::NodeType::SOURCE),
      makeStage("cvbs_source", orc::NodeType::SOURCE),
      makeStage("video_sink", orc::NodeType::SINK),
  };

  const auto typo = findNearbyStageNames("tbc_sourc", stages, 5);
  ASSERT_FALSE(typo.empty());
  EXPECT_EQ(typo.front(), "tbc_source");

  EXPECT_LE(findNearbyStageNames("source", stages, 1).size(), 1u);
}

TEST(StageDetailsTest, NothingIsSuggestedForAnUnrelatedName) {
  const std::vector<StageInfo> stages = {
      makeStage("tbc_source", orc::NodeType::SOURCE),
      makeStage("video_sink", orc::NodeType::SINK),
  };

  EXPECT_TRUE(findNearbyStageNames("zzzzzzzzzzzz", stages, 5).empty());
}

}  // namespace
}  // namespace orc::presenters
