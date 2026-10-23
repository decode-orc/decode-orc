/*
 * File:        filtergraph_import_test.cpp
 * Module:      orc-presenters unit tests
 * Purpose:     Unit tests for import_filtergraph_into_project(), mocking
 *              IProjectPresenter per AGENTS.md §4.2 (no filesystem, no real
 *              stage registry — the class under test only ever talks to the
 *              interface).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "filtergraph_import.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "i_project_presenter.h"

namespace orc::presenters::test {

// Self-contained GMock scaffold for IProjectPresenter, scoped to this test
// file (mirrors orc-tests/gui/unit/mocks/mock_project_presenter.h, which
// belongs to the GUI test suite and is not reused here to avoid coupling
// two independently-built test binaries).
class MockProjectPresenter : public IProjectPresenter {
 public:
  MOCK_METHOD(bool, createQuickProject,
              (VideoFormat format, SourceType source,
               const std::vector<std::string>& input_files),
              (override));
  MOCK_METHOD(bool, loadProject, (const std::string& project_path), (override));
  MOCK_METHOD(bool, saveProject, (const std::string& project_path), (override));
  MOCK_METHOD(void, clearProject, (), (override));
  MOCK_METHOD(bool, isModified, (), (const, override));
  MOCK_METHOD(void, clearModifiedFlag, (), (override));
  MOCK_METHOD(std::string, getProjectPath, (), (const, override));

  MOCK_METHOD(std::string, getProjectName, (), (const, override));
  MOCK_METHOD(void, setProjectName, (const std::string& name), (override));
  MOCK_METHOD(std::string, getProjectDescription, (), (const, override));
  MOCK_METHOD(void, setProjectDescription, (const std::string& description),
              (override));
  MOCK_METHOD(VideoFormat, getVideoFormat, (), (const, override));
  MOCK_METHOD(void, setVideoFormat, (VideoFormat format), (override));
  MOCK_METHOD(SourceType, getSourceFormat, (), (const, override));
  MOCK_METHOD(void, setSourceFormat, (SourceType source), (override));
  MOCK_METHOD(std::shared_ptr<const void>, createSnapshot, (),
              (const, override));
  MOCK_METHOD(SourceType, getSourceType, (), (const, override));
  MOCK_METHOD(void, setSourceType, (SourceType source), (override));
  MOCK_METHOD(orc::AmplitudeDisplayUnit, getAmplitudeUnit, (),
              (const, override));
  MOCK_METHOD(void, setAmplitudeUnit, (orc::AmplitudeDisplayUnit unit),
              (override));

  MOCK_METHOD(NodeID, addNode,
              (const std::string& stage_name, double x_position,
               double y_position),
              (override));
  MOCK_METHOD(bool, removeNode, (NodeID node_id), (override));
  MOCK_METHOD(bool, canRemoveNode, (NodeID node_id, std::string* reason),
              (const, override));
  MOCK_METHOD(void, setNodePosition, (NodeID node_id, double x, double y),
              (override));
  MOCK_METHOD(void, setNodeLabel, (NodeID node_id, const std::string& label),
              (override));
  MOCK_METHOD(void, setNodeParameters,
              (NodeID node_id,
               (const std::map<std::string, std::string>& parameters)),
              (override));
  MOCK_METHOD(void, addEdge, (NodeID source_node, NodeID target_node),
              (override));
  MOCK_METHOD(void, removeEdge, (NodeID source_node, NodeID target_node),
              (override));
  MOCK_METHOD(std::vector<NodeInfo>, getNodes, (), (const, override));
  MOCK_METHOD(NodeID, getFirstNode, (), (const, override));
  MOCK_METHOD(bool, hasNode, (NodeID node_id), (const, override));
  MOCK_METHOD(std::vector<EdgeInfo>, getEdges, (), (const, override));
  MOCK_METHOD(NodeInfo, getNodeInfo, (NodeID node_id), (const, override));

  MOCK_METHOD(std::vector<StageInfo>, listAvailableStagesForFormat,
              (VideoFormat format), (const, override));
  MOCK_METHOD(std::vector<StageInfo>, listAllStages, (), (const, override));
  MOCK_METHOD(bool, stageExists, (const std::string& stage_name),
              (const, override));
  MOCK_METHOD(std::shared_ptr<void>, instantiateStage,
              (const std::string& stage_name), (const, override));
  MOCK_METHOD(std::vector<LoadedPluginInfo>, listLoadedPlugins, (),
              (const, override));
  MOCK_METHOD(std::vector<PluginDiagnosticInfo>, listPluginDiagnostics, (),
              (const, override));
  MOCK_METHOD(std::vector<std::string>, listPluginSearchPaths, (),
              (const, override));
  MOCK_METHOD(PluginRegistryInfo, getPluginRegistry, (), (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, addPlugin,
              (const std::string& path, const std::string& plugin_id,
               const std::string& plugin_version,
               const std::string& license_spdx, bool is_core_plugin,
               bool trusted),
              (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, addPluginEntry,
              (const PluginRegistryEntryInfo& entry_info), (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, addPluginFromUrl,
              (const std::string& releases_url, bool trusted),
              (const, override));
  MOCK_METHOD(PluginSelectorResolution, resolvePluginSelector,
              (const std::string& selector), (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, removePluginEntry,
              (const std::string& selector), (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, setPluginEnabled,
              (const std::string& selector, bool enabled), (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, setPluginTrusted,
              (const std::string& selector, bool trusted), (const, override));
  MOCK_METHOD(PluginIndexInfo, fetchPluginIndex, (), (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, installPluginFromIndex,
              (const std::string& plugin_id), (const, override));
  MOCK_METHOD(std::vector<PluginUpdateStatusInfo>, checkPluginUpdates, (),
              (const, override));
  MOCK_METHOD(PluginRegistryMutationResult, updatePluginToLatestRelease,
              (const std::string& plugin_id), (const, override));

  MOCK_METHOD(bool, canTriggerNode, (NodeID node_id, std::string* reason),
              (const, override));
  MOCK_METHOD(bool, triggerNode,
              (NodeID node_id, ProgressCallback progress_callback), (override));
  MOCK_METHOD(bool, triggerAllSinks, (ProgressCallback progress_callback),
              (override));

  MOCK_METHOD(bool, validateProject, (), (const, override));
  MOCK_METHOD(std::vector<std::string>, getValidationErrors, (),
              (const, override));
  MOCK_METHOD(std::vector<std::string>, validatePipeExecution, (),
              (const, override));

  MOCK_METHOD(orc::ConfigurationStatus, getNodeConfigurationStatus,
              (NodeID node_id), (const, override));

  MOCK_METHOD(std::shared_ptr<void>, getDAG, (), (const, override));
  MOCK_METHOD(std::shared_ptr<void>, buildDAG, (), (override));
  MOCK_METHOD(bool, validateDAG, (), (override));

  MOCK_METHOD(std::string, getStageInstructions,
              (const std::string& stage_name), (const, override));

  MOCK_METHOD(std::vector<ParameterDescriptor>, getStageParameters,
              (const std::string& stage_name), (override));
  MOCK_METHOD((std::map<std::string, ParameterValue>), getNodeParameters,
              (NodeID node_id), (override));
  MOCK_METHOD(bool, setNodeParameters,
              (NodeID node_id,
               (const std::map<std::string, ParameterValue>& params)),
              (override));

  MOCK_METHOD(void*, getCoreProjectHandle, (), (override));
};

}  // namespace orc::presenters::test

namespace orc_unit_test {
namespace {

using ::testing::_;
using ::testing::InSequence;
using ::testing::NiceMock;
using ::testing::Return;

using orc::NodeID;
using orc::ParameterDescriptor;
using orc::presenters::import_filtergraph_into_project;
using orc::presenters::SourceType;
using orc::presenters::StageInfo;
using orc::presenters::VideoFormat;
using orc::presenters::test::MockProjectPresenter;

StageInfo make_stage(const std::string& name, bool is_source, bool is_sink) {
  StageInfo info;
  info.name = name;
  info.display_name = name;
  info.is_source = is_source;
  info.is_sink = is_sink;
  return info;
}

ParameterDescriptor make_param(const std::string& name, bool required) {
  ParameterDescriptor p;
  p.name = name;
  p.constraints.required = required;
  return p;
}

ParameterDescriptor make_typed_param(const std::string& name,
                                     orc::ParameterType type) {
  ParameterDescriptor p;
  p.name = name;
  p.type = type;
  return p;
}

// Wires up the common expectations shared by most tests: a source stage and
// a sink stage, both compatible with every video format (so no accidental
// video-format inference kicks in unless a test wants it to), with no
// required parameters unless a test overrides that stage's descriptors.
void set_up_generic_source_and_sink(NiceMock<MockProjectPresenter>& presenter,
                                    const std::string& source_name,
                                    const std::string& sink_name) {
  const std::vector<StageInfo> all_stages = {
      make_stage(source_name, /*is_source=*/true, /*is_sink=*/false),
      make_stage(sink_name, /*is_source=*/false, /*is_sink=*/true)};

  ON_CALL(presenter, stageExists(_)).WillByDefault(Return(true));
  ON_CALL(presenter, listAllStages()).WillByDefault(Return(all_stages));
  // Present in every format list => contributes no video-format signal.
  ON_CALL(presenter, listAvailableStagesForFormat(_))
      .WillByDefault(Return(all_stages));
  ON_CALL(presenter, getStageParameters(_))
      .WillByDefault(Return(std::vector<ParameterDescriptor>{}));

  // A single fixed default is enough for every test that doesn't care about
  // exact, distinct NodeID sequencing (most of them, below) — tests that do
  // (BuildsSimpleLinearGraph) override this locally with their own
  // EXPECT_CALL, which GMock matches in preference to this ON_CALL default.
  ON_CALL(presenter, addNode(_, _, _)).WillByDefault(Return(NodeID(0)));

  // The typed overload is the one the importer uses, and a false return there
  // means the parameters were rejected; accept them by default.
  ON_CALL(
      presenter,
      setNodeParameters(
          _,
          ::testing::Matcher<const std::map<std::string, orc::ParameterValue>&>(
              _)))
      .WillByDefault(Return(true));
}

TEST(FiltergraphImportTest, BuildsSimpleLinearGraph) {
  NiceMock<MockProjectPresenter> presenter;
  set_up_generic_source_and_sink(presenter, "tbc_source", "video_sink");

  EXPECT_CALL(presenter, addNode("tbc_source", _, _))
      .Times(1)
      .WillOnce(Return(NodeID(0)));
  EXPECT_CALL(presenter, addNode("video_sink", _, _))
      .Times(1)
      .WillOnce(Return(NodeID(1)));
  // Parameters go in typed, not as the raw filtergraph text: the stage reads
  // them by their declared alternative. Only tbc_source has a non-empty map.
  EXPECT_CALL(
      presenter,
      setNodeParameters(
          _,
          ::testing::Matcher<const std::map<std::string, orc::ParameterValue>&>(
              _)))
      .Times(1)
      .WillOnce(Return(true));
  EXPECT_CALL(presenter, addEdge(NodeID(0), NodeID(1))).Times(1);

  const auto result = import_filtergraph_into_project(
      presenter, "tbc_source=input_path=a.tbc, video_sink");

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.errors.empty());
}

TEST(FiltergraphImportTest, UnknownStageFailsWithoutBuildingAnything) {
  NiceMock<MockProjectPresenter> presenter;
  ON_CALL(presenter, stageExists(_)).WillByDefault(Return(false));

  EXPECT_CALL(presenter, addNode(_, _, _)).Times(0);

  const auto result =
      import_filtergraph_into_project(presenter, "not_a_real_stage");

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_THAT(result.errors[0], ::testing::HasSubstr("not_a_real_stage"));
}

TEST(FiltergraphImportTest, MissingRequiredParameterFailsWithoutBuilding) {
  NiceMock<MockProjectPresenter> presenter;
  set_up_generic_source_and_sink(presenter, "tbc_source", "video_sink");
  ON_CALL(presenter, getStageParameters("tbc_source"))
      .WillByDefault(Return(
          std::vector<ParameterDescriptor>{make_param("input_path", true)}));

  EXPECT_CALL(presenter, addNode(_, _, _)).Times(0);

  const auto result =
      import_filtergraph_into_project(presenter, "tbc_source, video_sink");

  EXPECT_FALSE(result.ok);
  bool mentions_input_path = false;
  for (const auto& e : result.errors) {
    if (e.find("input_path") != std::string::npos) mentions_input_path = true;
  }
  EXPECT_TRUE(mentions_input_path);
}

// Regression test for the bug found in review: y_path present as a key but
// with an *empty* value (e.g. a composite tbc_source that still carries
// y_path='' for parameter symmetry) must not be mistaken for a Y/C source.
TEST(FiltergraphImportTest, EmptyYPathIsNotTreatedAsYC) {
  NiceMock<MockProjectPresenter> presenter;
  set_up_generic_source_and_sink(presenter, "tbc_source", "video_sink");

  EXPECT_CALL(presenter, setSourceType(SourceType::Composite)).Times(1);
  EXPECT_CALL(presenter, setSourceType(SourceType::YC)).Times(0);

  const auto result = import_filtergraph_into_project(
      presenter, "tbc_source=input_path=a.tbc:y_path='', video_sink");

  EXPECT_TRUE(result.ok);
}

TEST(FiltergraphImportTest, BothYAndCPathNonEmptyIsDetectedAsYC) {
  NiceMock<MockProjectPresenter> presenter;
  set_up_generic_source_and_sink(presenter, "tbc_source", "video_sink");

  EXPECT_CALL(presenter, setSourceType(SourceType::YC)).Times(1);

  const auto result = import_filtergraph_into_project(
      presenter, "tbc_source=y_path=a.y:c_path=a.c, video_sink");

  EXPECT_TRUE(result.ok);
}

TEST(FiltergraphImportTest, OnlyYPathAloneDoesNotImplyYC) {
  NiceMock<MockProjectPresenter> presenter;
  set_up_generic_source_and_sink(presenter, "tbc_source", "video_sink");

  EXPECT_CALL(presenter, setSourceType(_)).Times(0);  // stays Unknown

  const auto result = import_filtergraph_into_project(
      presenter, "tbc_source=y_path=a.y, video_sink");

  EXPECT_TRUE(result.ok);
}

// Regression test for the validation-ordering bug found in review:
// getStageParameters() must be called *after* setVideoFormat()/
// setSourceType(), not before, so stages that narrow their descriptor set
// by format see the real context rather than Unknown.
TEST(FiltergraphImportTest, FormatIsSetBeforeParameterDescriptorsAreFetched) {
  NiceMock<MockProjectPresenter> presenter;
  const std::vector<StageInfo> all_stages = {
      make_stage("NTSC_CVBS_Source", true, false),
      make_stage("video_sink", false, true)};
  ON_CALL(presenter, stageExists(_)).WillByDefault(Return(true));
  ON_CALL(presenter, listAllStages()).WillByDefault(Return(all_stages));
  ON_CALL(presenter, listAvailableStagesForFormat(VideoFormat::NTSC))
      .WillByDefault(Return(all_stages));
  ON_CALL(presenter, listAvailableStagesForFormat(VideoFormat::PAL))
      .WillByDefault(Return(
          std::vector<StageInfo>{make_stage("video_sink", false, true)}));
  ON_CALL(presenter, listAvailableStagesForFormat(VideoFormat::PAL_M))
      .WillByDefault(Return(
          std::vector<StageInfo>{make_stage("video_sink", false, true)}));
  ON_CALL(presenter, getStageParameters(_))
      .WillByDefault(Return(std::vector<ParameterDescriptor>{}));
  ON_CALL(presenter, addNode(_, _, _)).WillByDefault(Return(NodeID(0)));
  ON_CALL(
      presenter,
      setNodeParameters(
          _,
          ::testing::Matcher<const std::map<std::string, orc::ParameterValue>&>(
              _)))
      .WillByDefault(Return(true));

  {
    InSequence seq;
    EXPECT_CALL(presenter, setVideoFormat(VideoFormat::NTSC));
    EXPECT_CALL(presenter, getStageParameters(_)).Times(::testing::AnyNumber());
  }

  const auto result = import_filtergraph_into_project(
      presenter, "NTSC_CVBS_Source=input_path=a.cvbs, video_sink");

  EXPECT_TRUE(result.ok);
}

// Regression test: a filtergraph is text, so every value arrives as a string.
// Handing them to the stage that way left every non-string parameter invisible
// to the std::holds_alternative check stages read them with, and the stage
// silently used its default instead — with no error and nothing in the log.
TEST(FiltergraphImportTest, NonStringParametersReachTheStageTyped) {
  NiceMock<MockProjectPresenter> presenter;
  set_up_generic_source_and_sink(presenter, "test_source", "test_sink");
  ON_CALL(presenter, getStageParameters("test_sink"))
      .WillByDefault(Return(std::vector<ParameterDescriptor>{
          make_typed_param("flag", orc::ParameterType::BOOL),
          make_typed_param("count", orc::ParameterType::INT32),
          make_typed_param("gain", orc::ParameterType::DOUBLE),
          make_typed_param("name", orc::ParameterType::STRING)}));

  std::map<std::string, orc::ParameterValue> applied;
  EXPECT_CALL(
      presenter,
      setNodeParameters(
          _,
          ::testing::Matcher<const std::map<std::string, orc::ParameterValue>&>(
              _)))
      .WillRepeatedly(
          [&applied](NodeID,
                     const std::map<std::string, orc::ParameterValue>& params) {
            applied.insert(params.begin(), params.end());
            return true;
          });

  const auto result = import_filtergraph_into_project(
      presenter,
      "test_source, test_sink=flag=true:count=7:gain=1.5:name=hello");

  ASSERT_TRUE(result.ok) << (result.errors.empty() ? ""
                                                   : result.errors.front());
  EXPECT_EQ(std::get<bool>(applied.at("flag")), true);
  EXPECT_EQ(std::get<int32_t>(applied.at("count")), 7);
  EXPECT_EQ(std::get<double>(applied.at("gain")), 1.5);
  EXPECT_EQ(std::get<std::string>(applied.at("name")), "hello");
}

// A value that does not fit its declared type is an error, not a silent
// fallback to the default — the whole failure mode this replaced.
TEST(FiltergraphImportTest, MalformedTypedParameterIsRejected) {
  NiceMock<MockProjectPresenter> presenter;
  set_up_generic_source_and_sink(presenter, "test_source", "test_sink");
  ON_CALL(presenter, getStageParameters("test_sink"))
      .WillByDefault(Return(std::vector<ParameterDescriptor>{
          make_typed_param("flag", orc::ParameterType::BOOL)}));

  const auto result = import_filtergraph_into_project(
      presenter, "test_source, test_sink=flag=maybe");

  EXPECT_FALSE(result.ok);
  ASSERT_FALSE(result.errors.empty());
  EXPECT_NE(result.errors.front().find("flag"), std::string::npos);
}

// Regression test: if parameter validation fails *after* the format was
// already set (needed so descriptor-fetching sees the right context), the
// presenter must be left exactly as it started — the format set above must
// be rolled back to Unknown, matching the documented contract in
// filtergraph_import.h.
TEST(FiltergraphImportTest, FailedParameterValidationRollsBackFormat) {
  NiceMock<MockProjectPresenter> presenter;
  const std::vector<StageInfo> all_stages = {
      make_stage("NTSC_CVBS_Source", true, false),
      make_stage("video_sink", false, true)};
  ON_CALL(presenter, stageExists(_)).WillByDefault(Return(true));
  ON_CALL(presenter, listAllStages()).WillByDefault(Return(all_stages));
  ON_CALL(presenter, listAvailableStagesForFormat(VideoFormat::NTSC))
      .WillByDefault(Return(all_stages));
  ON_CALL(presenter, listAvailableStagesForFormat(VideoFormat::PAL))
      .WillByDefault(Return(
          std::vector<StageInfo>{make_stage("video_sink", false, true)}));
  ON_CALL(presenter, listAvailableStagesForFormat(VideoFormat::PAL_M))
      .WillByDefault(Return(
          std::vector<StageInfo>{make_stage("video_sink", false, true)}));
  // Force a missing-required-parameter failure on the source stage.
  ON_CALL(presenter, getStageParameters("NTSC_CVBS_Source"))
      .WillByDefault(Return(
          std::vector<ParameterDescriptor>{make_param("input_path", true)}));
  ON_CALL(presenter, getStageParameters("video_sink"))
      .WillByDefault(Return(std::vector<ParameterDescriptor>{}));

  EXPECT_CALL(presenter, setVideoFormat(VideoFormat::NTSC)).Times(1);
  EXPECT_CALL(presenter, setVideoFormat(VideoFormat::Unknown)).Times(1);
  EXPECT_CALL(presenter, addNode(_, _, _)).Times(0);

  const auto result = import_filtergraph_into_project(
      presenter, "NTSC_CVBS_Source, video_sink");

  EXPECT_FALSE(result.ok);
}

TEST(FiltergraphImportTest, ConflictingVideoFormatsFailWithoutMutation) {
  NiceMock<MockProjectPresenter> presenter;
  const std::vector<StageInfo> ntsc_source = {
      make_stage("NTSC_CVBS_Source", true, false)};
  const std::vector<StageInfo> pal_source = {
      make_stage("PAL_CVBS_Source", true, false)};
  const std::vector<StageInfo> sink_only = {
      make_stage("video_sink", false, true)};
  const std::vector<StageInfo> all_stages = {
      make_stage("NTSC_CVBS_Source", true, false),
      make_stage("PAL_CVBS_Source", true, false),
      make_stage("video_sink", false, true)};

  ON_CALL(presenter, stageExists(_)).WillByDefault(Return(true));
  ON_CALL(presenter, listAllStages()).WillByDefault(Return(all_stages));
  ON_CALL(presenter, listAvailableStagesForFormat(VideoFormat::NTSC))
      .WillByDefault(Return(
          std::vector<StageInfo>{make_stage("NTSC_CVBS_Source", true, false),
                                 make_stage("video_sink", false, true)}));
  ON_CALL(presenter, listAvailableStagesForFormat(VideoFormat::PAL))
      .WillByDefault(Return(
          std::vector<StageInfo>{make_stage("PAL_CVBS_Source", true, false),
                                 make_stage("video_sink", false, true)}));
  ON_CALL(presenter, listAvailableStagesForFormat(VideoFormat::PAL_M))
      .WillByDefault(Return(sink_only));

  EXPECT_CALL(presenter, setVideoFormat(_)).Times(0);
  EXPECT_CALL(presenter, addNode(_, _, _)).Times(0);

  const auto result = import_filtergraph_into_project(
      presenter,
      "NTSC_CVBS_Source=input_path=a.cvbs[x]; "
      "PAL_CVBS_Source=input_path=b.cvbs[y]; [x][y] video_sink");

  EXPECT_FALSE(result.ok);
}

}  // namespace
}  // namespace orc_unit_test
