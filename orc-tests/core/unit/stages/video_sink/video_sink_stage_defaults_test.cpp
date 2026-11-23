/*
 * File:        video_sink_stage_defaults_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit test(s) for VideoSinkStage parameter defaults
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <algorithm>

#include "../../../../orc/plugins/stages/sinks/common/video_sink_stage.h"

namespace orc_unit_test {
TEST(VideoSinkStageDefaultsTest, Runtime_DefaultsEnableNtscPhaseCompensation) {
  orc::VideoSinkStage stage;
  auto params = stage.get_parameters();

  auto it = params.find("ntsc_phase_comp");
  ASSERT_NE(it, params.end());
  ASSERT_TRUE(std::holds_alternative<bool>(it->second));
  EXPECT_TRUE(std::get<bool>(it->second));
}

TEST(VideoSinkStageDefaultsTest,
     Descriptor_DefaultsEnableNtscPhaseCompensationForNtscProjects) {
  orc::VideoSinkStage stage;
  auto descriptors = stage.get_parameter_descriptors(orc::VideoSystem::NTSC,
                                                     orc::SourceType::YC);

  auto it = std::find_if(descriptors.begin(), descriptors.end(),
                         [](const orc::ParameterDescriptor& desc) {
                           return desc.name == "ntsc_phase_comp";
                         });

  ASSERT_NE(it, descriptors.end());
  if (!it->constraints.default_value.has_value()) {
    FAIL() << "Expected default_value to have a value";
    return;
  }
  ASSERT_TRUE(std::holds_alternative<bool>(*it->constraints.default_value));
  EXPECT_TRUE(std::get<bool>(*it->constraints.default_value));
}

TEST(VideoSinkStageDefaultsTest,
     Ntsc3dThresholds_DescriptorsMatchRuntimeDefaultsAndApplyToNtsc3dOnly) {
  orc::VideoSinkStage stage;
  const auto params = stage.get_parameters();
  const auto descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);

  for (const auto& [name, expected] : {std::pair{"motion_threshold", 4.0},
                                       std::pair{"detail_threshold", 2.0}}) {
    const auto value = params.find(name);
    ASSERT_NE(value, params.end()) << name;
    ASSERT_TRUE(std::holds_alternative<double>(value->second)) << name;
    EXPECT_DOUBLE_EQ(std::get<double>(value->second), expected) << name;

    const auto it = std::find_if(descriptors.begin(), descriptors.end(),
                                 [&](const orc::ParameterDescriptor& desc) {
                                   return desc.name == name;
                                 });
    ASSERT_NE(it, descriptors.end()) << name;
    ASSERT_TRUE(it->constraints.default_value.has_value()) << name;
    EXPECT_DOUBLE_EQ(std::get<double>(*it->constraints.default_value), expected)
        << name;
    ASSERT_TRUE(it->constraints.depends_on.has_value()) << name;
    const auto& types = it->constraints.depends_on->required_values;
    EXPECT_NE(std::find(types.begin(), types.end(), "ntsc3d"), types.end())
        << name;
    EXPECT_EQ(std::find(types.begin(), types.end(), "ntsc3dnoadapt"),
              types.end())
        << name;
  }
}

TEST(VideoSinkStageDefaultsTest,
     Ntsc3dThresholds_ReplaceTheOldCandidateSettings) {
  orc::VideoSinkStage stage;
  const auto descriptors = stage.get_parameter_descriptors(
      orc::VideoSystem::NTSC, orc::SourceType::Composite);
  for (const auto* old_name : {"chroma_weight", "adapt_threshold"}) {
    EXPECT_EQ(std::find_if(descriptors.begin(), descriptors.end(),
                           [&](const orc::ParameterDescriptor& desc) {
                             return desc.name == old_name;
                           }),
              descriptors.end())
        << old_name;
    EXPECT_EQ(stage.get_parameters().count(old_name), 0U) << old_name;
  }

  // Projects saved with the old settings still load: unknown keys are
  // ignored.
  ASSERT_TRUE(stage.set_parameters({{"chroma_weight", 2.0},
                                    {"adapt_threshold", 3.0},
                                    {"motion_threshold", 6.0},
                                    {"detail_threshold", 0.5}}));
  const auto params = stage.get_parameters();
  EXPECT_DOUBLE_EQ(std::get<double>(params.at("motion_threshold")), 6.0);
  EXPECT_DOUBLE_EQ(std::get<double>(params.at("detail_threshold")), 0.5);
}
}  // namespace orc_unit_test
