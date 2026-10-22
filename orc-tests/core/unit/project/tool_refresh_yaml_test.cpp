/*
 * File:        tool_refresh_yaml_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     Unit tests for a node's optional tool_refresh block: loading,
 *              the save round trip, projects without it, and setting it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "../../../orc/core/include/project.h"

namespace orc_unit_test {
namespace {

constexpr const char* kRefreshProject = R"yaml(
project:
  name: refresh
  version: "2.0"
  video_format: PAL
  source_format: Composite
  amplitude_unit: mV
dag:
  nodes:
    - id: 1
      stage: frame_map
      node_type: TRANSFORM
      display_name: Frame Map
      user_label: disc1_map
      x: 0
      y: 0
      tool_refresh:
        tool: field_mapping
        parameters:
          include_lead_in_out:
            type: bool
            value: false
      parameters:
        ranges:
          type: string
          value: "0-9"
    - id: 2
      stage: frame_map
      node_type: TRANSFORM
      display_name: Frame Map
      user_label: disc2_map
      x: 0
      y: 0
  edges: []
)yaml";

// A block that names no tool says nothing, so it is dropped on load.
constexpr const char* kToollessBlockProject = R"yaml(
project:
  name: toolless
  version: "2.0"
  video_format: PAL
  source_format: Composite
  amplitude_unit: mV
dag:
  nodes:
    - id: 1
      stage: frame_map
      node_type: TRANSFORM
      display_name: Frame Map
      user_label: Frame Map
      x: 0
      y: 0
      tool_refresh:
        parameters: {}
  edges: []
)yaml";

const orc::ProjectDAGNode& node(const orc::Project& project, int32_t id) {
  for (const auto& n : project.get_nodes()) {
    if (n.node_id == orc::NodeID(id)) {
      return n;
    }
  }
  throw std::runtime_error("no node " + std::to_string(id));
}

}  // namespace

TEST(ToolRefreshYamlTest, LoadsTheToolAndItsTypedParameters) {
  const auto project =
      orc::project_io::load_project_from_yaml(kRefreshProject, "r.orcprj");

  const auto& refresh = node(project, 1).tool_refresh;
  ASSERT_TRUE(refresh.has_value());
  EXPECT_EQ(refresh->tool_id, "field_mapping");
  ASSERT_EQ(refresh->parameters.count("include_lead_in_out"), 1u);
  EXPECT_FALSE(std::get<bool>(refresh->parameters.at("include_lead_in_out")));
  EXPECT_FALSE(node(project, 2).tool_refresh.has_value());
  EXPECT_FALSE(project.has_unsaved_changes());
}

TEST(ToolRefreshYamlTest, SurvivesASaveAndReload) {
  const auto project =
      orc::project_io::load_project_from_yaml(kRefreshProject, "r.orcprj");
  const auto yaml =
      orc::project_io::serialize_project_to_yaml(project, "r.orcprj");
  const auto reloaded = orc::project_io::load_project_from_yaml(yaml, "r");

  EXPECT_EQ(node(reloaded, 1).tool_refresh, node(project, 1).tool_refresh);
  EXPECT_FALSE(node(reloaded, 2).tool_refresh.has_value());
  EXPECT_EQ(std::get<std::string>(node(reloaded, 1).parameters.at("ranges")),
            "0-9");
}

TEST(ToolRefreshYamlTest, AProjectWithoutItIsWrittenWithoutIt) {
  auto project =
      orc::project_io::load_project_from_yaml(kRefreshProject, "r.orcprj");
  orc::project_io::set_node_tool_refresh(project, orc::NodeID(1), std::nullopt);

  const auto yaml =
      orc::project_io::serialize_project_to_yaml(project, "r.orcprj");
  EXPECT_EQ(yaml.find("tool_refresh"), std::string::npos);
  const auto again = orc::project_io::serialize_project_to_yaml(
      orc::project_io::load_project_from_yaml(yaml, "r"), "r.orcprj");
  EXPECT_EQ(again, yaml);
}

TEST(ToolRefreshYamlTest, ABlockWithoutAToolIsDropped) {
  const auto project =
      orc::project_io::load_project_from_yaml(kToollessBlockProject, "t");
  EXPECT_FALSE(node(project, 1).tool_refresh.has_value());
}

TEST(ToolRefreshYamlTest, SettingMarksTheProjectModifiedOnlyOnAChange) {
  auto project =
      orc::project_io::load_project_from_yaml(kRefreshProject, "r.orcprj");
  project.clear_modified_flag();

  // The same block again changes nothing
  orc::project_io::set_node_tool_refresh(project, orc::NodeID(1),
                                         node(project, 1).tool_refresh);
  EXPECT_FALSE(project.has_unsaved_changes());

  orc::project_io::set_node_tool_refresh(
      project, orc::NodeID(2),
      orc::NodeToolRefresh{"frame_map_range", {{"startAddress", "1"}}});
  EXPECT_TRUE(project.has_unsaved_changes());
  ASSERT_TRUE(node(project, 2).tool_refresh.has_value());
  EXPECT_EQ(node(project, 2).tool_refresh->tool_id, "frame_map_range");

  project.clear_modified_flag();
  orc::project_io::set_node_tool_refresh(project, orc::NodeID(2), std::nullopt);
  EXPECT_TRUE(project.has_unsaved_changes());
  EXPECT_FALSE(node(project, 2).tool_refresh.has_value());
}

TEST(ToolRefreshYamlTest, SettingRejectsAnUnknownNodeAndAnEmptyTool) {
  auto project =
      orc::project_io::load_project_from_yaml(kRefreshProject, "r.orcprj");
  EXPECT_THROW(
      orc::project_io::set_node_tool_refresh(
          project, orc::NodeID(99), orc::NodeToolRefresh{"field_mapping", {}}),
      std::runtime_error);
  EXPECT_THROW(orc::project_io::set_node_tool_refresh(
                   project, orc::NodeID(1), orc::NodeToolRefresh{"", {}}),
               std::runtime_error);
}

}  // namespace orc_unit_test
