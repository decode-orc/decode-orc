/*
 * File:        stage_parameter_context_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests for the parameter dialogue's graph-derived context
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "stage_parameter_context.h"

#include <gtest/gtest.h>
#include <stage_ux_strings.h>

#include <map>
#include <string>
#include <vector>

namespace gui_unit_test {
namespace {

constexpr char kSep = orc::stage_ux::kComboValueLabelSeparator;

orc::ParameterDescriptor makeStringDescriptor(
    const std::string& name, const std::vector<std::string>& allowed_strings) {
  orc::ParameterDescriptor desc;
  desc.name = name;
  desc.display_name = name;
  desc.description = name + " description";
  desc.type = orc::ParameterType::STRING;
  desc.constraints.allowed_strings = allowed_strings;
  return desc;
}

orc::ParameterDescriptor makeIntDescriptor(const std::string& name,
                                           int32_t default_value) {
  orc::ParameterDescriptor desc;
  desc.name = name;
  desc.display_name = name;
  desc.type = orc::ParameterType::INT32;
  desc.constraints.default_value = default_value;
  return desc;
}

const orc::ParameterDescriptor* findDescriptor(
    const std::vector<orc::ParameterDescriptor>& descriptors,
    const std::string& name) {
  for (const auto& desc : descriptors) {
    if (desc.name == name) return &desc;
  }
  return nullptr;
}

orc::SourceParameters makeSourceParameters() {
  orc::SourceParameters params{};
  params.active_video_start = 185;
  params.active_video_end = 1107;
  params.first_active_frame_line = 44;
  params.last_active_frame_line = 620;
  params.white_level = 54016;
  params.black_level = 16384;
  return params;
}

}  // namespace

// ---------------------------------------------------------------------------
// Audio channel-pair narrowing
// ---------------------------------------------------------------------------

TEST(StageParameterContextTest, AudioStagesNarrowChannelPairsToTheInputsPairs) {
  // Every stage that takes a channel pair narrows it the same way; a stage
  // added to the list without being narrowed would silently offer container
  // slots its input cannot satisfy.
  for (const std::string& stage_name :
       {"audio_channel_map", "audio_align", "AudioSink", "tbc_sink"}) {
    ASSERT_TRUE(orc::gui::stageNarrowsAudioChannelPairs(stage_name))
        << stage_name;

    orc::gui::StageParameterContextInputs inputs;
    inputs.stage_name = stage_name;
    inputs.descriptors.push_back(
        makeStringDescriptor("channel_pair", {"0", "1", "2", "3"}));
    inputs.input_audio_pair_names =
        std::vector<std::string>{"Analogue", "EFM digital audio"};

    const auto context = orc::gui::buildStageParameterContext(inputs);
    const auto* desc = findDescriptor(context.descriptors, "channel_pair");
    ASSERT_NE(desc, nullptr) << stage_name;

    const std::vector<std::string> expected = {
        std::string("0") + kSep + "0: Analogue",
        std::string("1") + kSep + "1: EFM digital audio"};
    EXPECT_EQ(desc->constraints.allowed_strings, expected) << stage_name;
  }
}

TEST(StageParameterContextTest, AudioChannelPairParameterIsNarrowedToo) {
  // The sinks name the same thing differently; both spellings are narrowed.
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "tbc_sink";
  inputs.descriptors.push_back(
      makeStringDescriptor("audio_channel_pair", {"0", "1", "2"}));
  inputs.input_audio_pair_names = std::vector<std::string>{"Analogue"};

  const auto context = orc::gui::buildStageParameterContext(inputs);
  const auto* desc = findDescriptor(context.descriptors, "audio_channel_pair");
  ASSERT_NE(desc, nullptr);
  ASSERT_EQ(desc->constraints.allowed_strings.size(), 1u);
  EXPECT_EQ(desc->constraints.allowed_strings.front(),
            std::string("0") + kSep + "0: Analogue");
}

TEST(StageParameterContextTest, TargetPairKeepsItsNewChannelPairEntry) {
  // Mapping into a pair the input does not have yet is what target_pair is
  // for, so narrowing must not take that choice away.
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "audio_channel_map";
  inputs.descriptors.push_back(
      makeStringDescriptor("target_pair", {"new", "0", "1", "2"}));
  inputs.input_audio_pair_names = std::vector<std::string>{"Analogue"};

  const auto context = orc::gui::buildStageParameterContext(inputs);
  const auto* desc = findDescriptor(context.descriptors, "target_pair");
  ASSERT_NE(desc, nullptr);

  const std::vector<std::string> expected = {
      std::string("new") + kSep + "New channel pair",
      std::string("0") + kSep + "0: Analogue"};
  EXPECT_EQ(desc->constraints.allowed_strings, expected);
}

TEST(StageParameterContextTest, UnnamedPairShowsItsIndexAlone) {
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "audio_align";
  inputs.descriptors.push_back(makeStringDescriptor("channel_pair", {"0"}));
  inputs.input_audio_pair_names = std::vector<std::string>{""};

  const auto context = orc::gui::buildStageParameterContext(inputs);
  const auto* desc = findDescriptor(context.descriptors, "channel_pair");
  ASSERT_NE(desc, nullptr);
  ASSERT_EQ(desc->constraints.allowed_strings.size(), 1u);
  EXPECT_EQ(desc->constraints.allowed_strings.front(),
            std::string("0") + kSep + "0");
}

TEST(StageParameterContextTest, InputWithNoPairsKeepsTheStagesOwnSlotList) {
  // With nothing to narrow to, the stage's full list stays: the note explains
  // why none of it will do anything, which is more use than an empty dropdown.
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "audio_channel_map";
  inputs.descriptors.push_back(
      makeStringDescriptor("channel_pair", {"0", "1", "2", "3"}));
  inputs.stage_description = "Maps audio channels.";
  inputs.input_audio_pair_names = std::vector<std::string>{};

  const auto context = orc::gui::buildStageParameterContext(inputs);
  const auto* desc = findDescriptor(context.descriptors, "channel_pair");
  ASSERT_NE(desc, nullptr);
  EXPECT_EQ(desc->constraints.allowed_strings.size(), 4u);

  EXPECT_NE(context.stage_description.find("carries no audio channel pairs"),
            std::string::npos);
  EXPECT_NE(context.stage_description.find("pass its input through unchanged"),
            std::string::npos);
}

TEST(StageParameterContextTest, TbcSinkWithNoPairsGetsTheSidecarNote) {
  // The TBC sink's export succeeds either way: only the optional .pcm sidecar
  // is affected, so it must not be told its input will pass through unchanged.
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "tbc_sink";
  inputs.descriptors.push_back(
      makeStringDescriptor("audio_channel_pair", {"0", "1"}));
  inputs.stage_description = "Writes a TBC file.";
  inputs.input_audio_pair_names = std::vector<std::string>{};

  const auto context = orc::gui::buildStageParameterContext(inputs);
  EXPECT_NE(context.stage_description.find("no .pcm"), std::string::npos);
  EXPECT_EQ(context.stage_description.find("pass its input through unchanged"),
            std::string::npos);
}

TEST(StageParameterContextTest, PairsPresentAddNoNote) {
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "audio_align";
  inputs.descriptors.push_back(makeStringDescriptor("channel_pair", {"0"}));
  inputs.stage_description = "Aligns audio.";
  inputs.input_audio_pair_names = std::vector<std::string>{"Analogue"};

  const auto context = orc::gui::buildStageParameterContext(inputs);
  EXPECT_EQ(context.stage_description, "Aligns audio.");
}

TEST(StageParameterContextTest, PairNamesNotLookedUpLeaveTheStageUntouched) {
  // nullopt is "not asked", which is not the same as an input carrying none:
  // nothing is narrowed and nothing is said.
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "audio_channel_map";
  inputs.descriptors.push_back(
      makeStringDescriptor("channel_pair", {"0", "1", "2", "3"}));
  inputs.stage_description = "Maps audio channels.";

  const auto context = orc::gui::buildStageParameterContext(inputs);
  EXPECT_EQ(context.stage_description, "Maps audio channels.");
  const auto* desc = findDescriptor(context.descriptors, "channel_pair");
  ASSERT_NE(desc, nullptr);
  EXPECT_EQ(desc->constraints.allowed_strings.size(), 4u);
}

// ---------------------------------------------------------------------------
// Video Parameters metadata
// ---------------------------------------------------------------------------

TEST(StageParameterContextTest, VideoParamsResetValuesComeFromTheSource) {
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "video_params";
  inputs.descriptors.push_back(makeIntDescriptor("blackLevel", -1));
  inputs.input_video_parameters = makeSourceParameters();

  const auto context = orc::gui::buildStageParameterContext(inputs);
  ASSERT_TRUE(context.reset_values.has_value());
  EXPECT_EQ(std::get<int32_t>(context.reset_values->at("blackLevel")), 16384);
  EXPECT_EQ(std::get<int32_t>(context.reset_values->at("whiteLevel")), 54016);
  EXPECT_EQ(std::get<int32_t>(context.reset_values->at("activeVideoStart")),
            185);
  EXPECT_EQ(std::get<int32_t>(context.reset_values->at("activeVideoEnd")),
            1107);
  EXPECT_EQ(std::get<int32_t>(context.reset_values->at("firstActiveFrameLine")),
            44);
  EXPECT_EQ(std::get<int32_t>(context.reset_values->at("lastActiveFrameLine")),
            620);
}

TEST(StageParameterContextTest, UnsetVideoParamsOverrideShowsTheSourcesFigure) {
  // -1 is the stage's "inherit from source": the form shows what will actually
  // be used rather than a number that means nothing to the user.
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "video_params";
  inputs.descriptors.push_back(makeIntDescriptor("blackLevel", -1));
  inputs.current_values["blackLevel"] = static_cast<int32_t>(-1);
  inputs.input_video_parameters = makeSourceParameters();

  const auto context = orc::gui::buildStageParameterContext(inputs);
  EXPECT_EQ(std::get<int32_t>(context.current_values.at("blackLevel")), 16384);
}

TEST(StageParameterContextTest, SetVideoParamsOverrideIsKept) {
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "video_params";
  inputs.descriptors.push_back(makeIntDescriptor("blackLevel", -1));
  inputs.current_values["blackLevel"] = static_cast<int32_t>(20000);
  inputs.input_video_parameters = makeSourceParameters();

  const auto context = orc::gui::buildStageParameterContext(inputs);
  EXPECT_EQ(std::get<int32_t>(context.current_values.at("blackLevel")), 20000);
}

TEST(StageParameterContextTest, VideoParamsWithoutMetadataHasNoResetValues) {
  // A node whose input cannot be read resets to the descriptor defaults, and
  // the dialogue's reset button says so by naming itself differently.
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "video_params";
  inputs.descriptors.push_back(makeIntDescriptor("blackLevel", -1));
  inputs.current_values["blackLevel"] = static_cast<int32_t>(-1);

  const auto context = orc::gui::buildStageParameterContext(inputs);
  EXPECT_FALSE(context.reset_values.has_value());
  EXPECT_EQ(std::get<int32_t>(context.current_values.at("blackLevel")), -1);
}

// ---------------------------------------------------------------------------
// Source Join input listing
// ---------------------------------------------------------------------------

TEST(StageParameterContextTest, SourceJoinNamesTheNodesConnectedToIt) {
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "source_join";
  inputs.descriptors.push_back(makeStringDescriptor("input_order", {}));
  inputs.stage_description = "Joins sources.";
  inputs.connected_inputs = {{2, "First pressing"}, {4, "Second pressing"}};

  const auto context = orc::gui::buildStageParameterContext(inputs);
  EXPECT_NE(context.stage_description.find("2 (First pressing)"),
            std::string::npos);
  EXPECT_NE(context.stage_description.find("4 (Second pressing)"),
            std::string::npos);
}

TEST(StageParameterContextTest, SourceJoinWithNothingConnectedSaysSo) {
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "source_join";
  inputs.descriptors.push_back(makeStringDescriptor("input_order", {}));
  inputs.stage_description = "Joins sources.";

  const auto context = orc::gui::buildStageParameterContext(inputs);
  EXPECT_NE(context.stage_description.find("Nothing is connected"),
            std::string::npos);
}

// ---------------------------------------------------------------------------
// Stages that consult none of this
// ---------------------------------------------------------------------------

TEST(StageParameterContextTest, OrdinaryStagePassesThroughUntouched) {
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "dropout_correct";
  inputs.descriptors.push_back(makeIntDescriptor("threshold", 100));
  inputs.descriptors.push_back(
      makeStringDescriptor("channel_pair", {"0", "1"}));
  inputs.current_values["threshold"] = static_cast<int32_t>(42);
  inputs.stage_description = "Corrects dropouts.";
  // Deliberately supplied: a stage that does not consult them must ignore
  // them rather than narrow a like-named parameter of its own.
  inputs.input_audio_pair_names = std::vector<std::string>{"Analogue"};
  inputs.input_video_parameters = makeSourceParameters();
  inputs.connected_inputs = {{2, "Source"}};

  const auto context = orc::gui::buildStageParameterContext(inputs);
  EXPECT_EQ(context.stage_description, "Corrects dropouts.");
  EXPECT_FALSE(context.reset_values.has_value());
  EXPECT_EQ(std::get<int32_t>(context.current_values.at("threshold")), 42);
  const auto* desc = findDescriptor(context.descriptors, "channel_pair");
  ASSERT_NE(desc, nullptr);
  EXPECT_EQ(desc->constraints.allowed_strings.size(), 2u);
}

// ---------------------------------------------------------------------------
// Descriptor comparison, which decides whether a refresh rebuilds the form
// ---------------------------------------------------------------------------

TEST(StageParameterContextTest, DescriptorsMatchThemselves) {
  std::vector<orc::ParameterDescriptor> descriptors = {
      makeIntDescriptor("threshold", 100),
      makeStringDescriptor("mode", {"a", "b"})};
  EXPECT_TRUE(orc::gui::descriptorsMatch(descriptors, descriptors));
}

TEST(StageParameterContextTest, NarrowedChoicesDoNotMatchTheStagesOwn) {
  // This is the case the comparison exists for: rewiring an audio stage's
  // input changes the dropdown, and the form has to be built again.
  orc::gui::StageParameterContextInputs inputs;
  inputs.stage_name = "audio_channel_map";
  inputs.descriptors.push_back(
      makeStringDescriptor("channel_pair", {"0", "1", "2", "3"}));
  const auto before = orc::gui::buildStageParameterContext(inputs);

  inputs.input_audio_pair_names = std::vector<std::string>{"Analogue"};
  const auto after = orc::gui::buildStageParameterContext(inputs);

  EXPECT_FALSE(
      orc::gui::descriptorsMatch(before.descriptors, after.descriptors));
}

TEST(StageParameterContextTest, DescriptorDifferencesTheFormReadsAreSeen) {
  const auto base = makeIntDescriptor("threshold", 100);

  auto renamed = base;
  renamed.display_name = "Threshold level";
  EXPECT_FALSE(orc::gui::descriptorsMatch({base}, {renamed}));

  auto retyped = base;
  retyped.type = orc::ParameterType::UINT32;
  EXPECT_FALSE(orc::gui::descriptorsMatch({base}, {retyped}));

  auto bounded = base;
  bounded.constraints.min_value = static_cast<int32_t>(0);
  bounded.constraints.max_value = static_cast<int32_t>(255);
  EXPECT_FALSE(orc::gui::descriptorsMatch({base}, {bounded}));

  auto redefaulted = base;
  redefaulted.constraints.default_value = static_cast<int32_t>(50);
  EXPECT_FALSE(orc::gui::descriptorsMatch({base}, {redefaulted}));

  auto required = base;
  required.constraints.required = true;
  EXPECT_FALSE(orc::gui::descriptorsMatch({base}, {required}));

  auto dependent = base;
  dependent.constraints.depends_on =
      orc::ParameterDependency{"mode", {"manual"}, true};
  EXPECT_FALSE(orc::gui::descriptorsMatch({base}, {dependent}));

  auto shown_when_disabled = dependent;
  shown_when_disabled.constraints.depends_on->hide_when_disabled = false;
  EXPECT_FALSE(orc::gui::descriptorsMatch({dependent}, {shown_when_disabled}));

  EXPECT_FALSE(orc::gui::descriptorsMatch({base}, {base, renamed}));
  EXPECT_FALSE(orc::gui::descriptorsMatch({base, renamed}, {renamed, base}));
}

}  // namespace gui_unit_test
