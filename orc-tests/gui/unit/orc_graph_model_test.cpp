/*
 * File:        orc_graph_model_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Unit tests for OrcGraphModel model behavior via
 * IProjectPresenter seam
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QPointF>
#include <QVariant>
#include <QtNodes/Definitions>
#include <QtNodes/NodeData>
#include <stdexcept>

#include "mocks/mock_project_presenter.h"
#include "orcgraphmodel.h"

namespace gui_unit_test {

using ::testing::Contains;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::UnorderedElementsAre;

static orc::presenters::NodeInfo makeNodeInfo(int32_t id,
                                              const std::string& stage,
                                              const std::string& label,
                                              double x, double y) {
  return orc::presenters::NodeInfo{
      orc::NodeID(id), stage, label, x, y, true, true, "", ""};
}

TEST(OrcGraphModelTest, Constructor_BuildsMappingsAndProjectsNodeData) {
  NiceMock<orc::presenters::test::MockProjectPresenter> presenter;

  const auto source = makeNodeInfo(10, "PALYCSource", "Source A", 12.5, 4.0);
  const auto transform = makeNodeInfo(20, "FieldInvert", "Invert B", 20.0, 8.0);

  EXPECT_CALL(presenter, getNodes())
      .Times(1)
      .WillOnce(
          Return(std::vector<orc::presenters::NodeInfo>{source, transform}));
  EXPECT_CALL(presenter, getEdges())
      .Times(1)
      .WillOnce(Return(std::vector<orc::presenters::EdgeInfo>{
          {orc::NodeID(10), orc::NodeID(20)}}));

  EXPECT_CALL(presenter, getNodeInfo(orc::NodeID(10)))
      .WillRepeatedly(Return(source));
  EXPECT_CALL(presenter, getNodeInfo(orc::NodeID(20)))
      .WillRepeatedly(Return(transform));

  OrcGraphModel model(presenter);

  const auto all_ids = model.allNodeIds();
  EXPECT_THAT(all_ids, UnorderedElementsAre(0, 1));

  EXPECT_EQ(model.getOrcNodeId(0), orc::NodeID(10));
  EXPECT_EQ(model.getOrcNodeId(1), orc::NodeID(20));
  EXPECT_EQ(model.getQtNodeId(orc::NodeID(10)), 0);
  EXPECT_EQ(model.getQtNodeId(orc::NodeID(20)), 1);

  EXPECT_EQ(model.nodeData(0, QtNodes::NodeRole::Type).toString().toStdString(),
            "PALYCSource");
  EXPECT_EQ(
      model.nodeData(1, QtNodes::NodeRole::Caption).toString().toStdString(),
      "Invert B");

  const QtNodes::ConnectionId expected{0, 0, 1, 0};
  EXPECT_TRUE(model.connectionExists(expected));
  EXPECT_THAT(model.allConnectionIds(0), Contains(expected));
}

TEST(OrcGraphModelTest, ConnectionRoundTrip_DelegatesToPresenter) {
  NiceMock<orc::presenters::test::MockProjectPresenter> presenter;

  EXPECT_CALL(presenter, getNodes())
      .Times(1)
      .WillOnce(Return(std::vector<orc::presenters::NodeInfo>{
          makeNodeInfo(31, "PALYCSource", "Src", 0.0, 0.0),
          makeNodeInfo(32, "FieldInvert", "Xform", 50.0, 10.0)}));
  EXPECT_CALL(presenter, getEdges())
      .Times(1)
      .WillOnce(Return(std::vector<orc::presenters::EdgeInfo>{}));

  OrcGraphModel model(presenter);

  const QtNodes::ConnectionId cid{0, 0, 1, 0};
  EXPECT_TRUE(model.connectionPossible(cid));

  EXPECT_CALL(presenter, addEdge(orc::NodeID(31), orc::NodeID(32))).Times(1);
  model.addConnection(cid);
  EXPECT_TRUE(model.connectionExists(cid));

  EXPECT_CALL(presenter, removeEdge(orc::NodeID(31), orc::NodeID(32))).Times(1);
  EXPECT_TRUE(model.deleteConnection(cid));
  EXPECT_FALSE(model.connectionExists(cid));
}

// A stage can judge its configuration by the nodes wired to it (Source Join
// orders its inputs by node ID), so a connection change has to drop the cached
// status of both ends — otherwise the dot keeps the colour it had before the
// wiring changed until something else resets the model.
TEST(OrcGraphModelTest, ConnectionChange_RefreshesCachedConfigurationStatus) {
  NiceMock<orc::presenters::test::MockProjectPresenter> presenter;

  EXPECT_CALL(presenter, getNodes())
      .Times(1)
      .WillOnce(Return(std::vector<orc::presenters::NodeInfo>{
          makeNodeInfo(41, "PALYCSource", "Src", 0.0, 0.0),
          makeNodeInfo(42, "source_join", "Join", 50.0, 10.0)}));
  EXPECT_CALL(presenter, getEdges())
      .Times(1)
      .WillOnce(Return(std::vector<orc::presenters::EdgeInfo>{}));

  OrcGraphModel model(presenter);

  // Unwired the join has no order to satisfy; wired it does.
  EXPECT_CALL(presenter, getNodeConfigurationStatus(orc::NodeID(42)))
      .WillOnce(Return(orc::ConfigurationStatus::Green))
      .WillOnce(Return(orc::ConfigurationStatus::Yellow))
      .WillOnce(Return(orc::ConfigurationStatus::Green));

  EXPECT_EQ(model.getConfigurationStatus(1), orc::ConfigurationStatus::Green);
  // Cached: a second read must not reach the presenter again.
  EXPECT_EQ(model.getConfigurationStatus(1), orc::ConfigurationStatus::Green);

  const QtNodes::ConnectionId cid{0, 0, 1, 0};
  model.addConnection(cid);
  EXPECT_EQ(model.getConfigurationStatus(1), orc::ConfigurationStatus::Yellow);

  EXPECT_TRUE(model.deleteConnection(cid));
  EXPECT_EQ(model.getConfigurationStatus(1), orc::ConfigurationStatus::Green);
}

TEST(OrcGraphModelTest,
     ConnectionPossible_RejectsInvalidAndDuplicateConnections) {
  NiceMock<orc::presenters::test::MockProjectPresenter> presenter;

  EXPECT_CALL(presenter, getNodes())
      .Times(1)
      .WillOnce(Return(std::vector<orc::presenters::NodeInfo>{
          makeNodeInfo(100, "PALYCSource", "Src", 0.0, 0.0),
          makeNodeInfo(101, "FieldInvert", "Xform", 0.0, 0.0)}));
  EXPECT_CALL(presenter, getEdges())
      .Times(1)
      .WillOnce(Return(std::vector<orc::presenters::EdgeInfo>{
          {orc::NodeID(100), orc::NodeID(101)}}));

  OrcGraphModel model(presenter);

  const QtNodes::ConnectionId duplicate{0, 0, 1, 0};
  const QtNodes::ConnectionId self_connection{0, 0, 0, 0};
  const QtNodes::ConnectionId missing_node{0, 0, 42, 0};

  EXPECT_FALSE(model.connectionPossible(duplicate));
  EXPECT_FALSE(model.connectionPossible(self_connection));
  EXPECT_FALSE(model.connectionPossible(missing_node));
}

TEST(OrcGraphModelTest, Set_NodeDataCaptionAndPositionDelegateToPresenter) {
  NiceMock<orc::presenters::test::MockProjectPresenter> presenter;

  EXPECT_CALL(presenter, getNodes())
      .Times(1)
      .WillOnce(Return(std::vector<orc::presenters::NodeInfo>{
          makeNodeInfo(501, "FieldInvert", "old", 1.0, 2.0)}));
  EXPECT_CALL(presenter, getEdges())
      .Times(1)
      .WillOnce(Return(std::vector<orc::presenters::EdgeInfo>{}));

  OrcGraphModel model(presenter);

  EXPECT_CALL(presenter, setNodeLabel(orc::NodeID(501), "renamed")).Times(1);
  EXPECT_TRUE(model.setNodeData(0, QtNodes::NodeRole::Caption,
                                QVariant(QString("renamed"))));

  EXPECT_CALL(presenter, setNodePosition(orc::NodeID(501), 11.0, 22.0))
      .Times(1);
  EXPECT_TRUE(model.setNodeData(0, QtNodes::NodeRole::Position,
                                QVariant(QPointF(11.0, 22.0))));
}

TEST(OrcGraphModelTest, SetNodeDataCaption_RefusedRenameKeepsTheReason) {
  NiceMock<orc::presenters::test::MockProjectPresenter> presenter;

  EXPECT_CALL(presenter, getNodes())
      .Times(1)
      .WillOnce(Return(std::vector<orc::presenters::NodeInfo>{
          makeNodeInfo(501, "FieldInvert", "old", 1.0, 2.0)}));
  EXPECT_CALL(presenter, getEdges())
      .Times(1)
      .WillOnce(Return(std::vector<orc::presenters::EdgeInfo>{}));

  OrcGraphModel model(presenter);

  EXPECT_CALL(presenter, setNodeLabel(orc::NodeID(501), "taken"))
      .WillOnce(
          ::testing::Throw(std::runtime_error("Stage names must be unique.")));
  EXPECT_FALSE(model.setNodeData(0, QtNodes::NodeRole::Caption,
                                 QVariant(QString("taken"))));
  EXPECT_EQ(model.lastCaptionError().toStdString(),
            "Stage names must be unique.");

  // A later successful rename clears the reason
  EXPECT_CALL(presenter, setNodeLabel(orc::NodeID(501), "free")).Times(1);
  EXPECT_TRUE(model.setNodeData(0, QtNodes::NodeRole::Caption,
                                QVariant(QString("free"))));
  EXPECT_TRUE(model.lastCaptionError().isEmpty());
}

TEST(OrcGraphModelTest, DeleteNodeUnknownNode_Fails) {
  NiceMock<orc::presenters::test::MockProjectPresenter> presenter;

  EXPECT_CALL(presenter, getNodes())
      .Times(1)
      .WillOnce(Return(std::vector<orc::presenters::NodeInfo>{}));
  EXPECT_CALL(presenter, getEdges())
      .Times(1)
      .WillOnce(Return(std::vector<orc::presenters::EdgeInfo>{}));

  OrcGraphModel model(presenter);

  EXPECT_FALSE(model.deleteNode(1234));
}

}  // namespace gui_unit_test
