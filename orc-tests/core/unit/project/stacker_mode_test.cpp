/*
 * File:        stacker_mode_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     Unit tests for storing the Stacker's stacking mode: the
 *              deprecated Auto and Smart Mean modes become Weighted, new
 *              nodes store Weighted
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <string>
#include <variant>

#include "../../../orc/core/include/project.h"

namespace orc_unit_test {
namespace {

// A Stacker saved before its mode was always stored, so it has none.
constexpr const char* kStackerWithoutModeProject = R"yaml(
project:
  name: stacker-without-mode
  version: "2.0"
  video_format: PAL
  source_format: Composite
  amplitude_unit: mV
dag:
  nodes:
    - id: 1
      stage: stacker
      node_type: MERGER
      display_name: Stacker
      user_label: Stacker
      x: 0
      y: 0
  edges: []
)yaml";

// A Stacker saved with |mode|.
std::string stacker_project_with_mode(const std::string& mode) {
  return std::string(R"yaml(
project:
  name: stacker-with-mode
  version: "2.0"
  video_format: PAL
  source_format: Composite
  amplitude_unit: mV
dag:
  nodes:
    - id: 1
      stage: stacker
      node_type: MERGER
      display_name: Stacker
      user_label: Stacker
      x: 0
      y: 0
      parameters:
        mode:
          type: string
          value: )yaml") +
         mode + "\n  edges: []\n";
}

// The stored mode of the project's only Stacker, or "" when none is stored.
std::string stored_mode(const orc::Project& project) {
  for (const auto& node : project.get_nodes()) {
    if (node.stage_name != "stacker") continue;
    const auto it = node.parameters.find("mode");
    if (it == node.parameters.end()) return "";
    const auto* mode = std::get_if<std::string>(&it->second);
    return mode ? *mode : "";
  }
  return "";
}

}  // namespace

// A Stacker saved without a mode ran Auto, which is deprecated.
TEST(StackerModeTest, Load_StoresWeightedForAStackerWithoutAMode) {
  const auto project = orc::project_io::load_project_from_yaml(
      kStackerWithoutModeProject, "/tmp/stacker-without-mode.orcprj");

  EXPECT_EQ(stored_mode(project), "Weighted");
  EXPECT_TRUE(project.has_unsaved_changes());
}

TEST(StackerModeTest, Load_StoresTheDeprecatedModesAsWeighted) {
  for (const char* deprecated : {"Auto", "Smart Mean"}) {
    const auto project = orc::project_io::load_project_from_yaml(
        stacker_project_with_mode(deprecated), "/tmp/stacker-with-mode.orcprj");

    EXPECT_EQ(stored_mode(project), "Weighted") << deprecated;
    EXPECT_TRUE(project.has_unsaved_changes()) << deprecated;
  }
}

// Older projects stored the mode as an integer: -1 for Auto, 2 for Smart
// Mean.
TEST(StackerModeTest, Load_StoresTheLegacyIntegerDeprecatedModesAsWeighted) {
  for (const char* deprecated : {"-1", "2"}) {
    auto yaml = stacker_project_with_mode(deprecated);
    yaml.replace(yaml.find("type: string"), 12, "type: int32");
    const auto project = orc::project_io::load_project_from_yaml(
        yaml, "/tmp/stacker-with-mode.orcprj");

    EXPECT_EQ(stored_mode(project), "Weighted") << deprecated;
    EXPECT_TRUE(project.has_unsaved_changes()) << deprecated;
  }
}

// The Smart Mean threshold is no longer a parameter, so it is not kept.
TEST(StackerModeTest, Load_DropsTheSmartMeanThreshold) {
  auto yaml = stacker_project_with_mode("Mean");
  const std::string threshold =
      "        smart_threshold:\n          type: int32\n          value: 15\n";
  yaml.insert(yaml.find("  edges:"), threshold);
  const auto project = orc::project_io::load_project_from_yaml(
      yaml, "/tmp/stacker-with-mode.orcprj");

  EXPECT_EQ(stored_mode(project), "Mean");
  EXPECT_TRUE(project.has_unsaved_changes());
  for (const auto& node : project.get_nodes()) {
    EXPECT_EQ(node.parameters.count("smart_threshold"), 0u);
  }
}

TEST(StackerModeTest, Load_SavesTheMigratedMode) {
  const auto project = orc::project_io::load_project_from_yaml(
      stacker_project_with_mode("Auto"), "/tmp/stacker-with-mode.orcprj");
  const auto yaml = orc::project_io::serialize_project_to_yaml(
      project, "/tmp/stacker-with-mode.orcprj");

  const auto reloaded = orc::project_io::load_project_from_yaml(
      yaml, "/tmp/stacker-with-mode.orcprj");

  EXPECT_EQ(stored_mode(reloaded), "Weighted");
  EXPECT_FALSE(reloaded.has_unsaved_changes());
}

TEST(StackerModeTest, Load_LeavesAStoredModeAlone) {
  const auto project = orc::project_io::load_project_from_yaml(
      stacker_project_with_mode("Mean"), "/tmp/stacker-with-mode.orcprj");

  EXPECT_EQ(stored_mode(project), "Mean");
  EXPECT_FALSE(project.has_unsaved_changes());
}

// The neighbour modes always stacked as Median and are no longer offered.
TEST(StackerModeTest, Load_StoresANeighbourModeAsMedian) {
  for (const char* legacy : {"Neighbor", "Smart Neighbor"}) {
    const auto project = orc::project_io::load_project_from_yaml(
        stacker_project_with_mode(legacy), "/tmp/stacker-with-mode.orcprj");

    EXPECT_EQ(stored_mode(project), "Median") << legacy;
    EXPECT_TRUE(project.has_unsaved_changes()) << legacy;
  }
}

TEST(StackerModeTest, AddNode_StoresTheWeightedMode) {
  auto project = orc::project_io::create_empty_project(
      "stacker-mode", orc::VideoSystem::PAL, orc::SourceType::Composite);
  orc::project_io::add_node(project, "stacker", 0.0, 0.0);

  EXPECT_EQ(stored_mode(project), "Weighted");

  const auto yaml = orc::project_io::serialize_project_to_yaml(
      project, "/tmp/stacker-mode.orcprj");
  const auto reloaded =
      orc::project_io::load_project_from_yaml(yaml, "/tmp/stacker-mode.orcprj");
  EXPECT_EQ(stored_mode(reloaded), "Weighted");
  EXPECT_FALSE(reloaded.has_unsaved_changes());
}

}  // namespace orc_unit_test
