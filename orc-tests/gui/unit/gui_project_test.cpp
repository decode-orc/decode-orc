/*
 * File:        gui_project_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Unit tests for GUIProject model behavior through
 * IProjectPresenter seam
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QString>

#include "guiproject.h"
#include "mocks/mock_project_presenter.h"

namespace gui_unit_test {

using ::testing::Return;
using ::testing::StrictMock;

TEST(GUIProjectTest, NewEmptyProject_DelegatesLifecycleAndClearsModified) {
  auto mock = std::make_unique<
      StrictMock<orc::presenters::test::MockProjectPresenter>>();
  auto* mock_presenter = mock.get();
  GUIProject project(std::move(mock));

  EXPECT_CALL(*mock_presenter, clearProject()).Times(1);
  EXPECT_CALL(*mock_presenter, setProjectName("test-project")).Times(1);
  EXPECT_CALL(*mock_presenter,
              setVideoFormat(orc::presenters::VideoFormat::NTSC))
      .Times(1);
  EXPECT_CALL(*mock_presenter,
              setSourceType(orc::presenters::SourceType::Composite))
      .Times(1);
  // NTSC projects default to IRE (SMPTE 170M-2004 convention).
  EXPECT_CALL(*mock_presenter, setAmplitudeUnit(orc::AmplitudeDisplayUnit::IRE))
      .Times(1);
  EXPECT_CALL(*mock_presenter, clearModifiedFlag()).Times(1);

  QString error;
  EXPECT_TRUE(project.newEmptyProject(
      "test-project", orc::presenters::VideoFormat::NTSC,
      orc::presenters::SourceType::Composite, &error));
  EXPECT_TRUE(error.isEmpty());
}

TEST(GUIProjectTest, IsModified_DelegatesToPresenter) {
  auto mock = std::make_unique<
      StrictMock<orc::presenters::test::MockProjectPresenter>>();
  auto* mock_presenter = mock.get();
  GUIProject project(std::move(mock));

  EXPECT_CALL(*mock_presenter, isModified()).WillOnce(Return(true));
  EXPECT_TRUE(project.isModified());

  // Compatibility no-op: GUIProject does not directly push modified state into
  // presenter.
  project.setModified(false);
}

TEST(GUIProjectTest, SaveToFile_DelegatesToPresenterAndStoresPath) {
  auto mock = std::make_unique<
      StrictMock<orc::presenters::test::MockProjectPresenter>>();
  auto* mock_presenter = mock.get();
  GUIProject project(std::move(mock));

  EXPECT_CALL(*mock_presenter, saveProject("/tmp/test-save.orcprj"))
      .WillOnce(Return(true));

  QString error;
  EXPECT_TRUE(project.saveToFile("/tmp/test-save.orcprj", &error));
  EXPECT_TRUE(error.isEmpty());
  EXPECT_EQ(project.projectPath(), QString("/tmp/test-save.orcprj"));
}

TEST(GUIProjectTest, LoadFromFile_DelegatesToPresenterBuildsDagAndStoresPath) {
  auto mock = std::make_unique<
      StrictMock<orc::presenters::test::MockProjectPresenter>>();
  auto* mock_presenter = mock.get();
  GUIProject project(std::move(mock));

  EXPECT_CALL(*mock_presenter, loadProject("/tmp/test-load.orcprj"))
      .WillOnce(Return(true));
  EXPECT_CALL(*mock_presenter, buildDAG())
      .WillOnce(Return(std::make_shared<int>(42)));
  // The missing-plugin guard rejects a project referencing an unregistered
  // stage; with none reported, and no nodes for hasSource(), the load
  // proceeds.
  EXPECT_CALL(*mock_presenter, missingStagesMessage())
      .WillOnce(Return(std::string{}));
  EXPECT_CALL(*mock_presenter, getNodes())
      .WillRepeatedly(Return(std::vector<orc::presenters::NodeInfo>{}));
  EXPECT_CALL(*mock_presenter, listAllStages())
      .WillRepeatedly(Return(std::vector<orc::presenters::StageInfo>{}));

  QString error;
  EXPECT_TRUE(project.loadFromFile("/tmp/test-load.orcprj", &error));
  EXPECT_TRUE(error.isEmpty());
  EXPECT_EQ(project.projectPath(), QString("/tmp/test-load.orcprj"));
}

namespace {

// A node/stage pair the presenter can report; `is_source` decides whether
// reloadSources() counts it.
orc::presenters::NodeInfo makeNode(int id, const std::string& stage_name) {
  orc::presenters::NodeInfo node;
  node.node_id = orc::NodeID(id);
  node.stage_name = stage_name;
  return node;
}

orc::presenters::StageInfo makeStage(const std::string& name, bool is_source) {
  orc::presenters::StageInfo stage;
  stage.name = name;
  stage.node_type = orc::NodeType::SOURCE;
  stage.is_source = is_source;
  stage.is_sink = false;
  return stage;
}

}  // namespace

TEST(GUIProjectTest, ReloadSources_RebuildsDagAndCountsEverySource) {
  auto mock = std::make_unique<
      StrictMock<orc::presenters::test::MockProjectPresenter>>();
  auto* mock_presenter = mock.get();
  GUIProject project(std::move(mock));

  EXPECT_CALL(*mock_presenter, getNodes())
      .WillRepeatedly(Return(std::vector<orc::presenters::NodeInfo>{
          makeNode(1, "tbc_source"), makeNode(2, "dropout_correct"),
          makeNode(3, "cvbs_source")}));
  EXPECT_CALL(*mock_presenter, listAllStages())
      .WillRepeatedly(Return(std::vector<orc::presenters::StageInfo>{
          makeStage("tbc_source", true), makeStage("dropout_correct", false),
          makeStage("cvbs_source", true)}));
  EXPECT_CALL(*mock_presenter, buildDAG())
      .WillOnce(Return(std::make_shared<int>(7)));

  const auto result = project.reloadSources();

  EXPECT_EQ(result.source_count, 2);
  EXPECT_TRUE(result.rebuilt);
}

TEST(GUIProjectTest, ReloadSources_ReportsFailureWhenDagCannotBeBuilt) {
  auto mock = std::make_unique<
      StrictMock<orc::presenters::test::MockProjectPresenter>>();
  auto* mock_presenter = mock.get();
  GUIProject project(std::move(mock));

  EXPECT_CALL(*mock_presenter, getNodes())
      .WillRepeatedly(Return(
          std::vector<orc::presenters::NodeInfo>{makeNode(1, "tbc_source")}));
  EXPECT_CALL(*mock_presenter, listAllStages())
      .WillRepeatedly(Return(std::vector<orc::presenters::StageInfo>{
          makeStage("tbc_source", true)}));
  // A source file that has been moved or truncated makes the rebuild fail.
  EXPECT_CALL(*mock_presenter, buildDAG())
      .WillOnce(Return(std::shared_ptr<void>{}));

  const auto result = project.reloadSources();

  EXPECT_EQ(result.source_count, 1);
  EXPECT_FALSE(result.rebuilt);
}

TEST(GUIProjectTest, ReloadSources_SourcelessProjectIsLeftAlone) {
  auto mock = std::make_unique<
      StrictMock<orc::presenters::test::MockProjectPresenter>>();
  auto* mock_presenter = mock.get();
  GUIProject project(std::move(mock));

  EXPECT_CALL(*mock_presenter, getNodes())
      .WillRepeatedly(Return(
          std::vector<orc::presenters::NodeInfo>{makeNode(1, "video_sink")}));
  EXPECT_CALL(*mock_presenter, listAllStages())
      .WillRepeatedly(Return(std::vector<orc::presenters::StageInfo>{
          makeStage("video_sink", false)}));
  // StrictMock: no buildDAG() expectation, so rebuilding would fail the test.

  const auto result = project.reloadSources();

  EXPECT_EQ(result.source_count, 0);
  EXPECT_FALSE(result.rebuilt);
}

TEST(GUIProjectTest, Clear_ResetsPathAndDelegatesProjectReset) {
  auto mock = std::make_unique<
      StrictMock<orc::presenters::test::MockProjectPresenter>>();
  auto* mock_presenter = mock.get();
  GUIProject project(std::move(mock));

  project.setProjectPath("/tmp/will-be-cleared.orcprj");

  EXPECT_CALL(*mock_presenter, clearProject()).Times(1);
  EXPECT_CALL(*mock_presenter, clearModifiedFlag()).Times(1);

  project.clear();

  EXPECT_TRUE(project.projectPath().isEmpty());
}

}  // namespace gui_unit_test
