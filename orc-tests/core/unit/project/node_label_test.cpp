/*
 * File:        node_label_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     Unit tests for unique node labels: new-node suffixing, rename
 *              rejection, and the duplicate-label migration on load
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <map>
#include <stdexcept>
#include <string>

#include "../../../orc/core/include/project.h"

namespace orc_unit_test {
namespace {

// Three nodes sharing one label, stored out of node-id order, so the
// migration has to sort by id rather than follow file order.
constexpr const char* kDuplicateLabelsProject = R"yaml(
project:
  name: duplicate-labels
  version: "2.0"
  video_format: PAL
  source_format: Composite
  amplitude_unit: mV
dag:
  nodes:
    - id: 3
      stage: frame_map
      node_type: TRANSFORM
      display_name: Frame Map
      user_label: Frame Map
      x: 0
      y: 0
    - id: 1
      stage: frame_map
      node_type: TRANSFORM
      display_name: Frame Map
      user_label: Frame Map
      x: 0
      y: 0
    - id: 2
      stage: frame_map
      node_type: TRANSFORM
      display_name: Frame Map
      user_label: Frame Map
      x: 0
      y: 0
    - id: 4
      stage: stacker
      node_type: MERGER
      display_name: Stacker
      user_label: Stacker
      x: 0
      y: 0
  edges: []
)yaml";

// "Frame Map 2" is already taken by another node, so the migration has to
// skip that number.
constexpr const char* kTakenSuffixProject = R"yaml(
project:
  name: taken-suffix
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
    - id: 2
      stage: frame_map
      node_type: TRANSFORM
      display_name: Frame Map
      user_label: Frame Map
      x: 0
      y: 0
    - id: 3
      stage: frame_map
      node_type: TRANSFORM
      display_name: Frame Map
      user_label: Frame Map 2
      x: 0
      y: 0
  edges: []
)yaml";

// An empty and an all-digit label, both left over from before labels had to
// be valid node addresses.
constexpr const char* kInvalidLabelsProject = R"yaml(
project:
  name: invalid-labels
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
      user_label: ""
      x: 0
      y: 0
    - id: 2
      stage: frame_map
      node_type: TRANSFORM
      display_name: Frame Map
      user_label: "42"
      x: 0
      y: 0
  edges: []
)yaml";

std::map<int32_t, std::string> labels_by_id(const orc::Project& project) {
  std::map<int32_t, std::string> labels;
  for (const auto& node : project.get_nodes()) {
    labels[node.node_id.value()] = node.user_label;
  }
  return labels;
}

orc::Project make_project_with_two_sources(orc::NodeID& first,
                                           orc::NodeID& second) {
  auto project = orc::project_io::create_empty_project(
      "label-test", orc::VideoSystem::PAL, orc::SourceType::Composite);
  first = orc::project_io::add_node(project, "PAL_CVBS_Source", 0.0, 0.0);
  second = orc::project_io::add_node(project, "PAL_CVBS_Source", 0.0, 100.0);
  return project;
}

}  // namespace

TEST(NodeLabelTest, AddNode_SuffixesATakenDisplayName) {
  const auto* info = orc::get_node_type_info("PAL_CVBS_Source");
  ASSERT_NE(info, nullptr);

  orc::NodeID first;
  orc::NodeID second;
  auto project = make_project_with_two_sources(first, second);
  const auto third =
      orc::project_io::add_node(project, "PAL_CVBS_Source", 0.0, 200.0);

  const auto labels = labels_by_id(project);
  EXPECT_EQ(labels.at(first.value()), info->display_name);
  EXPECT_EQ(labels.at(second.value()), info->display_name + " 2");
  EXPECT_EQ(labels.at(third.value()), info->display_name + " 3");
}

TEST(NodeLabelTest, AddNode_FillsTheFirstFreeSuffix) {
  const auto* info = orc::get_node_type_info("PAL_CVBS_Source");
  ASSERT_NE(info, nullptr);

  orc::NodeID first;
  orc::NodeID second;
  auto project = make_project_with_two_sources(first, second);
  orc::project_io::set_node_label(project, second, "Disc A");
  const auto third =
      orc::project_io::add_node(project, "PAL_CVBS_Source", 0.0, 200.0);

  EXPECT_EQ(labels_by_id(project).at(third.value()), info->display_name + " 2");
}

TEST(NodeLabelTest, SetNodeLabel_RejectsADuplicate) {
  orc::NodeID first;
  orc::NodeID second;
  auto project = make_project_with_two_sources(first, second);
  orc::project_io::set_node_label(project, first, "Disc A");

  EXPECT_THROW(orc::project_io::set_node_label(project, second, "Disc A"),
               std::runtime_error);
  EXPECT_NE(labels_by_id(project).at(second.value()), "Disc A");
}

TEST(NodeLabelTest, SetNodeLabel_AcceptsTheNodesOwnLabel) {
  orc::NodeID first;
  orc::NodeID second;
  auto project = make_project_with_two_sources(first, second);
  orc::project_io::set_node_label(project, first, "Disc A");

  EXPECT_NO_THROW(orc::project_io::set_node_label(project, first, "Disc A"));
}

TEST(NodeLabelTest, SetNodeLabel_RejectsEmptyAndAllDigitLabels) {
  orc::NodeID first;
  orc::NodeID second;
  auto project = make_project_with_two_sources(first, second);
  const auto before = labels_by_id(project).at(first.value());

  EXPECT_THROW(orc::project_io::set_node_label(project, first, ""),
               std::runtime_error);
  EXPECT_THROW(orc::project_io::set_node_label(project, first, "123"),
               std::runtime_error);
  EXPECT_EQ(labels_by_id(project).at(first.value()), before);

  // Digits alongside other characters are fine
  EXPECT_NO_THROW(orc::project_io::set_node_label(project, first, "Disc 1"));
}

TEST(NodeLabelTest, Load_NumbersDuplicatesInNodeIdOrder) {
  const auto project = orc::project_io::load_project_from_yaml(
      kDuplicateLabelsProject, "/tmp/duplicate-labels.orcprj");

  const auto labels = labels_by_id(project);
  EXPECT_EQ(labels.at(1), "Frame Map 1");
  EXPECT_EQ(labels.at(2), "Frame Map 2");
  EXPECT_EQ(labels.at(3), "Frame Map 3");
  EXPECT_TRUE(project.has_unsaved_changes());
}

TEST(NodeLabelTest, Load_LeavesUniqueLabelsAlone) {
  const auto project = orc::project_io::load_project_from_yaml(
      kDuplicateLabelsProject, "/tmp/duplicate-labels.orcprj");

  EXPECT_EQ(labels_by_id(project).at(4), "Stacker");
}

TEST(NodeLabelTest, Load_SkipsANumberAlreadyTaken) {
  const auto project = orc::project_io::load_project_from_yaml(
      kTakenSuffixProject, "/tmp/taken-suffix.orcprj");

  const auto labels = labels_by_id(project);
  EXPECT_EQ(labels.at(1), "Frame Map 1");
  EXPECT_EQ(labels.at(2), "Frame Map 3");
  EXPECT_EQ(labels.at(3), "Frame Map 2");
}

TEST(NodeLabelTest, Load_ReplacesEmptyAndAllDigitLabels) {
  const auto project = orc::project_io::load_project_from_yaml(
      kInvalidLabelsProject, "/tmp/invalid-labels.orcprj");

  const auto labels = labels_by_id(project);
  EXPECT_EQ(labels.at(1), "Stacker");
  EXPECT_EQ(labels.at(2), "Frame Map");
  EXPECT_TRUE(project.has_unsaved_changes());
}

TEST(NodeLabelTest, Load_UniqueProjectIsNotModified) {
  orc::NodeID first;
  orc::NodeID second;
  auto project = make_project_with_two_sources(first, second);
  const auto yaml = orc::project_io::serialize_project_to_yaml(
      project, "/tmp/unique-labels.orcprj");

  const auto reloaded = orc::project_io::load_project_from_yaml(
      yaml, "/tmp/unique-labels.orcprj");

  EXPECT_FALSE(reloaded.has_unsaved_changes());
  EXPECT_EQ(labels_by_id(reloaded), labels_by_id(project));
}

TEST(NodeLabelTest, MigratedLabels_RoundTripThroughSave) {
  const auto project = orc::project_io::load_project_from_yaml(
      kDuplicateLabelsProject, "/tmp/duplicate-labels.orcprj");
  const auto yaml = orc::project_io::serialize_project_to_yaml(
      project, "/tmp/duplicate-labels.orcprj");

  const auto reloaded = orc::project_io::load_project_from_yaml(
      yaml, "/tmp/duplicate-labels.orcprj");

  EXPECT_FALSE(reloaded.has_unsaved_changes());
  EXPECT_EQ(labels_by_id(reloaded), labels_by_id(project));
}

}  // namespace orc_unit_test
