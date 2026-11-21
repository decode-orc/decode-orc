/*
 * File:        relocated_stages_test.cpp
 * Module:      orc-core unit tests
 * Purpose:     The message a project gets for a stage that moved to a plugin
 *              which is not installed
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "../../../orc/core/include/relocated_stages.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "../../../orc/core/include/project.h"
#include "../../../orc/core/include/project_to_dag.h"
#include "../../../orc/core/include/stage_registry.h"

namespace orc_unit_test {
namespace {

constexpr const char* kTeletextPluginId = "org.decodeorc.stage.teletext";

// A project saved while the teletext sink was built in: no required_plugins
// entry, because built-in stages are never recorded.
constexpr const char* kTeletextProjectWithoutRequirement = R"yaml(
project:
  name: teletext-project
  version: "2.0"
  video_format: PAL
  source_format: Composite
  amplitude_unit: mV
dag:
  nodes:
    - id: 7
      stage: teletext_sink
      display_name: Teletext Sink
      node_type: SINK
      x: 0.0
      y: 0.0
  edges: []
)yaml";

// The same project saved with the plugin installed from the Plugin Manager,
// which records the requirement.
constexpr const char* kNabtsProjectWithRequirement = R"yaml(
project:
  name: nabts-project
  version: "2.0"
  video_format: NTSC
  source_format: Composite
  amplitude_unit: mV
required_plugins:
  - plugin_id: org.decodeorc.stage.teletext
    plugin_version: 1.0.0
    source_repo_url: https://github.com/decode-orc/orc-plugin_teletext
    artifact_source: release_asset
    stage_names: [nabts_sink]
dag:
  nodes:
    - id: 3
      stage: nabts_sink
      display_name: NABTS Sink
      node_type: SINK
      x: 0.0
      y: 0.0
  edges: []
)yaml";

// A project saved while the teletext sink was built in, under the sink's
// older name and wired to a VBI source as it would be in practice. The edge
// must not turn the missing stage into a connection error at load.
constexpr const char* kConnectedLegacyTeletextProject = R"yaml(
project:
  name: connected-teletext-project
  version: "2.0"
  video_format: PAL
  source_format: Composite
  amplitude_unit: 10bit
dag:
  nodes:
    - id: 1
      stage: vbi_source
      display_name: VBI Capture Source
      node_type: SOURCE
      x: 0.0
      y: 0.0
    - id: 2
      stage: teletext_analysis_sink
      display_name: Teletext Sink
      node_type: SINK
      x: 200.0
      y: 0.0
  edges:
    - from: 1
      to: 2
)yaml";

void expect_install_message(const std::string& message,
                            const std::string& stage_name,
                            const std::string& node_id) {
  EXPECT_NE(
      message.find("Unknown stage type: " + stage_name + " in node " + node_id),
      std::string::npos)
      << message;
  EXPECT_NE(message.find(std::string("Required plugin: ") + kTeletextPluginId),
            std::string::npos)
      << message;
  EXPECT_NE(message.find("https://github.com/decode-orc/orc-plugin_teletext"),
            std::string::npos)
      << message;
  EXPECT_NE(message.find("Plugin Manager > Browse Plugins..."),
            std::string::npos)
      << message;
  EXPECT_NE(
      message.find(std::string("orc-cli plugins install ") + kTeletextPluginId),
      std::string::npos)
      << message;
}

TEST(RelocatedStagesTest, BothVbiDataSinks_MapToTheTeletextPlugin) {
  for (const std::string stage : {"teletext_sink", "nabts_sink"}) {
    const auto requirement = orc::relocated_stage_requirement(stage);
    ASSERT_TRUE(requirement.has_value()) << stage;
    EXPECT_EQ(requirement->plugin_id, kTeletextPluginId);
    EXPECT_EQ(requirement->stage_names, std::vector<std::string>{stage});
    EXPECT_FALSE(requirement->is_core_plugin);
  }
}

TEST(RelocatedStagesTest, AStageThatNeverMoved_HasNoRelocationEntry) {
  EXPECT_FALSE(orc::relocated_stage_requirement("video_sink").has_value());
  EXPECT_FALSE(orc::relocated_stage_requirement("vbi_source").has_value());
}

TEST(RelocatedStagesTest,
     MissingStageMessage_NamesThePlugin_WhenTheProjectHasNoRequirement) {
  const std::string message =
      orc::missing_stage_message({}, "teletext_sink", orc::NodeID(7));
  expect_install_message(message, "teletext_sink", "7");
}

TEST(RelocatedStagesTest,
     MissingStageMessage_UsesTheProjectsEntry_WhenItHasOne) {
  orc::ProjectPluginRequirement requirement;
  requirement.plugin_id = kTeletextPluginId;
  requirement.source_repo_url =
      "https://github.com/decode-orc/orc-plugin_teletext";
  requirement.stage_names = {"nabts_sink"};

  const std::string message =
      orc::missing_stage_message({requirement}, "nabts_sink", orc::NodeID(3));
  expect_install_message(message, "nabts_sink", "3");
}

TEST(RelocatedStagesTest,
     MissingStageMessage_ProjectEntryWins_OverTheRelocationTable) {
  orc::ProjectPluginRequirement requirement;
  requirement.plugin_id = "org.example.fork.teletext";
  requirement.source_repo_url = "https://example.org/teletext-fork";
  requirement.stage_names = {"teletext_sink"};

  const std::string message = orc::missing_stage_message(
      {requirement}, "teletext_sink", orc::NodeID(1));
  EXPECT_NE(message.find("Required plugin: org.example.fork.teletext"),
            std::string::npos)
      << message;
  EXPECT_EQ(message.find(kTeletextPluginId), std::string::npos) << message;
}

TEST(RelocatedStagesTest, MissingStageMessage_UnknownStage_SaysOnlyThat) {
  EXPECT_EQ(orc::missing_stage_message({}, "no_such_stage", orc::NodeID(9)),
            "Unknown stage type: no_such_stage in node 9");
}

// The same two cases through project_to_dag(), which is where a project meets
// the message. Skipped where the plugin is loaded (a build with the plugin
// checkout built in, or an installed copy), since the projects then convert.
void expect_conversion_names_plugin(const char* yaml,
                                    const std::string& stage_name,
                                    const std::string& node_id) {
  if (orc::StageRegistry::instance().has_stage(stage_name)) {
    GTEST_SKIP() << stage_name << " is registered (teletext plugin loaded)";
  }
  const auto project =
      orc::project_io::load_project_from_yaml(yaml, "/tmp/relocated.orcprj");
  try {
    (void)orc::project_to_dag(project);
    FAIL() << "project_to_dag accepted an unregistered " << stage_name;
  } catch (const orc::ProjectConversionError& error) {
    expect_install_message(error.what(), stage_name, node_id);
  }
}

TEST(RelocatedStagesTest,
     ProjectToDag_ProjectWithoutRequirement_NamesTheTeletextPlugin) {
  expect_conversion_names_plugin(kTeletextProjectWithoutRequirement,
                                 "teletext_sink", "7");
}

TEST(RelocatedStagesTest,
     ProjectToDag_ProjectWithRequirement_NamesTheTeletextPlugin) {
  expect_conversion_names_plugin(kNabtsProjectWithRequirement, "nabts_sink",
                                 "3");
}

// The legacy name is migrated to teletext_sink at load, so the message names
// the stage the plugin provides.
TEST(RelocatedStagesTest,
     ProjectToDag_ConnectedLegacyProject_NamesThePluginNotAConnectionError) {
  expect_conversion_names_plugin(kConnectedLegacyTeletextProject,
                                 "teletext_sink", "2");
}

}  // namespace
}  // namespace orc_unit_test
