/*
 * File:        project_presenter_validation_test.cpp
 * Module:      orc-presenters unit tests
 * Purpose:     Validation logic for ProjectPresenter source/sink detection
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../../../../orc/core/include/project.h"
#include "../../../../orc/presenters/include/project_presenter.h"

namespace orc_unit_test {
namespace {

// Builds an in-memory v2.0 project (no edges, so no registry lookups are
// triggered during load) from a list of nodes described as
// "stage_name / node_type" pairs. The stage names are deliberately neutral so
// the source-format consistency checks in the loader never fire.
struct NodeSpec {
  int id;
  std::string stage;
  std::string node_type;
};

orc::Project make_project(const std::vector<NodeSpec>& nodes) {
  std::string yaml =
      "project:\n"
      "  name: validation-test\n"
      "  version: \"2.0\"\n"
      "  video_format: PAL\n"
      "  source_format: Composite\n"
      "  amplitude_unit: mV\n"
      "dag:\n";
  if (nodes.empty()) {
    yaml += "  nodes: []\n";
  } else {
    yaml += "  nodes:\n";
    for (const auto& n : nodes) {
      yaml += "    - id: " + std::to_string(n.id) + "\n";
      yaml += "      stage: " + n.stage + "\n";
      yaml += "      node_type: " + n.node_type + "\n";
    }
  }
  yaml += "  edges: []\n";
  return orc::project_io::load_project_from_yaml(yaml, "/virtual/test.orcprj");
}

orc::presenters::ProjectPresenter wrap(orc::Project& project) {
  return orc::presenters::ProjectPresenter(static_cast<void*>(&project));
}

}  // namespace

TEST(ProjectPresenterValidationTest, EmptyProject_FailsValidation) {
  auto project = make_project({});
  auto presenter = wrap(project);

  EXPECT_FALSE(presenter.validateProject());
  EXPECT_EQ(presenter.getValidationErrors(),
            std::vector<std::string>{"Project has no nodes"});
}

TEST(ProjectPresenterValidationTest, SourceOnly_FailsValidation) {
  auto project = make_project({{1, "tbc_source", "SOURCE"}});
  auto presenter = wrap(project);

  EXPECT_FALSE(presenter.validateProject());
  EXPECT_EQ(presenter.getValidationErrors(),
            std::vector<std::string>{"Project has no sink nodes"});
}

TEST(ProjectPresenterValidationTest, SinkOnly_ReportsNoSourceNodes) {
  auto project = make_project({{1, "video_sink", "SINK"}});
  auto presenter = wrap(project);

  EXPECT_FALSE(presenter.validateProject());
  EXPECT_EQ(presenter.getValidationErrors(),
            std::vector<std::string>{"Project has no source nodes"});
}

TEST(ProjectPresenterValidationTest, SourceAndSink_PassesValidation) {
  auto project =
      make_project({{1, "tbc_source", "SOURCE"}, {2, "video_sink", "SINK"}});
  auto presenter = wrap(project);

  EXPECT_TRUE(presenter.validateProject());
  EXPECT_TRUE(presenter.getValidationErrors().empty());
}

TEST(ProjectPresenterValidationTest, TransformBetweenSourceAndSink_Passes) {
  auto project = make_project({{1, "tbc_source", "SOURCE"},
                               {2, "dropout_correct", "TRANSFORM"},
                               {3, "video_sink", "SINK"}});
  auto presenter = wrap(project);

  EXPECT_TRUE(presenter.validateProject());
  EXPECT_TRUE(presenter.getValidationErrors().empty());
}

TEST(ProjectPresenterValidationTest, AnalysisSinkCountsAsSink) {
  auto project = make_project(
      {{1, "tbc_source", "SOURCE"}, {2, "dropout_analysis", "ANALYSIS_SINK"}});
  auto presenter = wrap(project);

  EXPECT_TRUE(presenter.validateProject());
  EXPECT_TRUE(presenter.getValidationErrors().empty());
}

TEST(ProjectPresenterValidationTest, TransformOnly_ReportsBothMissing) {
  auto project = make_project({{1, "dropout_correct", "TRANSFORM"}});
  auto presenter = wrap(project);

  EXPECT_FALSE(presenter.validateProject());
  const std::vector<std::string> expected{"Project has no source nodes",
                                          "Project has no sink nodes"};
  EXPECT_EQ(presenter.getValidationErrors(), expected);
}

// ── Missing stages (what the GUI refuses a project with) ────────────────

TEST(ProjectPresenterValidationTest,
     MissingStagesMessage_EmptyWhenAllRegistered) {
  auto project =
      make_project({{1, "tbc_source", "SOURCE"}, {2, "video_sink", "SINK"}});
  auto presenter = wrap(project);

  EXPECT_EQ(presenter.missingStagesMessage(), "");
}

// A stage that moved to a plugin is named with that plugin even though the
// project records no requirement for it, and an unknown stage is named alone.
TEST(ProjectPresenterValidationTest,
     MissingStagesMessage_NamesThePluginOfARelocatedStage) {
  if (orc::presenters::ProjectPresenter::hasStage("teletext_sink")) {
    GTEST_SKIP() << "teletext_sink is registered (teletext plugin loaded)";
  }
  auto project = make_project({{1, "tbc_source", "SOURCE"},
                               {2, "teletext_sink", "SINK"},
                               {3, "no_such_stage", "SINK"}});
  auto presenter = wrap(project);

  const std::string message = presenter.missingStagesMessage();
  EXPECT_NE(message.find("Unknown stage type: teletext_sink in node 2. "
                         "Required plugin: org.decodeorc.stage.teletext"),
            std::string::npos)
      << message;
  EXPECT_NE(message.find("Unknown stage type: no_such_stage in node 3"),
            std::string::npos)
      << message;
  EXPECT_EQ(message.find("tbc_source"), std::string::npos) << message;
}

// ── Reserved input-identity parameter ────────────────────────────────────

namespace {

// A two-source Source Join project: both sources feed the join node, so the
// presenter has real edges to report as the join's input identity.
orc::Project make_join_project() {
  const std::string yaml =
      "project:\n"
      "  name: join-test\n"
      "  version: \"2.0\"\n"
      "  video_format: PAL\n"
      "  source_format: Composite\n"
      "  amplitude_unit: mV\n"
      "dag:\n"
      "  nodes:\n"
      "    - id: 1\n"
      "      stage: frame_map\n"
      "      node_type: TRANSFORM\n"
      "    - id: 2\n"
      "      stage: frame_map\n"
      "      node_type: TRANSFORM\n"
      "    - id: 3\n"
      "      stage: source_join\n"
      "      node_type: TRANSFORM\n"
      "      parameters:\n"
      "        input_order:\n"
      "          type: string\n"
      "          value: \"2,1\"\n"
      "  edges:\n"
      "    - from: 1\n"
      "      to: 3\n"
      "    - from: 2\n"
      "      to: 3\n";
  return orc::project_io::load_project_from_yaml(yaml, "/virtual/join.orcprj");
}

}  // namespace

// The reserved parameter is host-owned: the DAG builder fills it in from the
// node's connections, so it must never be offered for editing in the GUI
// dialog or on the CLI.
TEST(ProjectPresenterParameterTest, StageParameters_HideInputNodeIds) {
  auto project = make_join_project();
  auto presenter = wrap(project);

  const auto descriptors = presenter.getStageParameters("source_join");
  ASSERT_FALSE(descriptors.empty());
  EXPECT_TRUE(std::any_of(descriptors.begin(), descriptors.end(),
                          [](const orc::ParameterDescriptor& d) {
                            return d.name == "input_order";
                          }));
  EXPECT_FALSE(std::any_of(descriptors.begin(), descriptors.end(),
                           [](const orc::ParameterDescriptor& d) {
                             return d.name == orc::kInputNodeIdsParameter;
                           }));
}

// A stage that judges its configuration against the nodes feeding it has to be
// told what they are, or its status dot reports on inputs it cannot see.
TEST(ProjectPresenterParameterTest, NodeStatus_GreenWhenOrderMatchesInputs) {
  auto project = make_join_project();
  auto presenter = wrap(project);
  EXPECT_EQ(presenter.getNodeConfigurationStatus(orc::NodeID(3)),
            orc::ConfigurationStatus::Green);
}

// Disconnecting every input is not "identity unknown" — it is the graph saying
// there is nothing for the stored order to name.
TEST(ProjectPresenterParameterTest, NodeStatus_YellowWhenAllInputsAreRemoved) {
  auto project = make_join_project();
  orc::project_io::remove_edge(project, orc::NodeID(1), orc::NodeID(3));
  orc::project_io::remove_edge(project, orc::NodeID(2), orc::NodeID(3));
  auto presenter = wrap(project);
  EXPECT_EQ(presenter.getNodeConfigurationStatus(orc::NodeID(3)),
            orc::ConfigurationStatus::Yellow);
}

// Rewiring the node leaves the stored order describing a graph that no longer
// exists; dropping the second connection has to show as an unfinished node.
TEST(ProjectPresenterParameterTest, NodeStatus_YellowWhenAnInputIsRemoved) {
  auto project = make_join_project();
  orc::project_io::remove_edge(project, orc::NodeID(2), orc::NodeID(3));
  auto presenter = wrap(project);
  EXPECT_EQ(presenter.getNodeConfigurationStatus(orc::NodeID(3)),
            orc::ConfigurationStatus::Yellow);
}

}  // namespace orc_unit_test
