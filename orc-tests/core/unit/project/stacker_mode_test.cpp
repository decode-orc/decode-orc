/*
 * File:        stacker_mode_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     Unit tests for storing the Stacker's stacking mode: older
 *              projects keep Auto, new nodes store Weighted
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

TEST(StackerModeTest, Load_StoresAutoForAStackerWithoutAMode) {
  const auto project = orc::project_io::load_project_from_yaml(
      kStackerWithoutModeProject, "/tmp/stacker-without-mode.orcprj");

  EXPECT_EQ(stored_mode(project), "Auto");
  EXPECT_TRUE(project.has_unsaved_changes());
}

TEST(StackerModeTest, Load_SavesTheStoredAutoMode) {
  const auto project = orc::project_io::load_project_from_yaml(
      kStackerWithoutModeProject, "/tmp/stacker-without-mode.orcprj");
  const auto yaml = orc::project_io::serialize_project_to_yaml(
      project, "/tmp/stacker-without-mode.orcprj");

  const auto reloaded = orc::project_io::load_project_from_yaml(
      yaml, "/tmp/stacker-without-mode.orcprj");

  EXPECT_EQ(stored_mode(reloaded), "Auto");
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
