/*
 * File:        orc_graphics_view_navigation_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Keyboard node navigation tests for the DAG canvas view
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QScrollBar>
#include <QTest>
#include <QtNodes/Definitions>
#include <memory>

#include "mocks/mock_project_presenter.h"
#include "orcgraphicsscene.h"
#include "orcgraphicsview.h"
#include "orcgraphmodel.h"

namespace gui_unit_test {

using ::testing::NiceMock;
using ::testing::Return;

namespace {

// Requires QApplication. Run under the "gui-widget" CTest label.
QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-view-navigation-test";
  static char platform_opt[] = "-platform";
  static char platform_val[] = "offscreen";
  static char* argv[] = {app_name, platform_opt, platform_val, nullptr};
  static QApplication* app = [] {
    auto* created_app = new QApplication(argc, argv);
    created_app->setQuitOnLastWindowClosed(false);
    return created_app;
  }();
  return *app;
}

orc::presenters::NodeInfo makeNodeInfo(int32_t id, const std::string& stage,
                                       const std::string& label, double x,
                                       double y) {
  return orc::presenters::NodeInfo{
      orc::NodeID(id), stage, label, x, y, true, true, "", ""};
}

// A source, a transform to its right, and a second transform below the first,
// so both horizontal and vertical moves have somewhere to go. QtNodes assigns
// node ids 0, 1 and 2 in the order the presenter reports them.
struct CanvasFixture {
  NiceMock<orc::presenters::test::MockProjectPresenter> presenter;
  std::unique_ptr<OrcGraphModel> model;
  std::unique_ptr<OrcGraphicsScene> scene;
  std::unique_ptr<OrcGraphicsView> view;

  CanvasFixture() {
    const auto source = makeNodeInfo(10, "tbc_source", "Source", 0.0, 0.0);
    const auto right = makeNodeInfo(20, "dropout_correct", "Right", 300.0, 0.0);
    const auto below =
        makeNodeInfo(30, "dropout_correct", "Below", 300.0, 300.0);

    ON_CALL(presenter, getNodes())
        .WillByDefault(Return(
            std::vector<orc::presenters::NodeInfo>{source, right, below}));
    ON_CALL(presenter, getEdges())
        .WillByDefault(Return(std::vector<orc::presenters::EdgeInfo>{}));
    ON_CALL(presenter, getNodeInfo(orc::NodeID(10)))
        .WillByDefault(Return(source));
    ON_CALL(presenter, getNodeInfo(orc::NodeID(20)))
        .WillByDefault(Return(right));
    ON_CALL(presenter, getNodeInfo(orc::NodeID(30)))
        .WillByDefault(Return(below));

    model = std::make_unique<OrcGraphModel>(presenter);
    scene = std::make_unique<OrcGraphicsScene>(*model);
    view = std::make_unique<OrcGraphicsView>();
    view->setScene(scene.get());
    view->resize(800, 600);
    view->show();
    QCoreApplication::processEvents();
  }

  ~CanvasFixture() {
    view->setScene(nullptr);
    view.reset();
    scene.reset();
    model.reset();
  }

  QtNodes::NodeId selectedNode() const { return scene->lastSelectedNodeId(); }
};

constexpr QtNodes::NodeId kSourceNode = 0;
constexpr QtNodes::NodeId kRightNode = 1;
constexpr QtNodes::NodeId kBelowNode = 2;

}  // namespace

TEST(OrcGraphicsViewNavigationTest, CursorKeysMoveTheSelectionBetweenNodes) {
  (void)ensureApplication();
  CanvasFixture canvas;

  canvas.scene->selectNode(kSourceNode);
  ASSERT_EQ(canvas.selectedNode(), kSourceNode);

  QTest::keyClick(canvas.view.get(), Qt::Key_Right);
  EXPECT_EQ(canvas.selectedNode(), kRightNode);

  QTest::keyClick(canvas.view.get(), Qt::Key_Down);
  EXPECT_EQ(canvas.selectedNode(), kBelowNode);

  QTest::keyClick(canvas.view.get(), Qt::Key_Up);
  EXPECT_EQ(canvas.selectedNode(), kRightNode);

  QTest::keyClick(canvas.view.get(), Qt::Key_Left);
  EXPECT_EQ(canvas.selectedNode(), kSourceNode);
}

TEST(OrcGraphicsViewNavigationTest, CursorKeyWithNoNeighbourKeepsTheSelection) {
  (void)ensureApplication();
  CanvasFixture canvas;

  canvas.scene->selectNode(kSourceNode);

  QTest::keyClick(canvas.view.get(), Qt::Key_Left);
  EXPECT_EQ(canvas.selectedNode(), kSourceNode);
}

TEST(OrcGraphicsViewNavigationTest, CursorKeyWithNoSelectionPicksANode) {
  (void)ensureApplication();
  CanvasFixture canvas;

  ASSERT_EQ(canvas.selectedNode(), QtNodes::InvalidNodeId);

  QTest::keyClick(canvas.view.get(), Qt::Key_Right);
  EXPECT_NE(canvas.selectedNode(), QtNodes::InvalidNodeId);
}

TEST(OrcGraphicsViewNavigationTest, ControlCursorKeyLeavesTheSelectionAlone) {
  (void)ensureApplication();
  CanvasFixture canvas;

  canvas.scene->selectNode(kSourceNode);

  QTest::keyClick(canvas.view.get(), Qt::Key_Right, Qt::ControlModifier);
  EXPECT_EQ(canvas.selectedNode(), kSourceNode);
}

TEST(OrcGraphicsViewNavigationTest, TabCyclesThroughEveryNodeAndWraps) {
  (void)ensureApplication();
  CanvasFixture canvas;

  canvas.scene->selectNode(kSourceNode);

  QTest::keyClick(canvas.view.get(), Qt::Key_Tab);
  EXPECT_EQ(canvas.selectedNode(), kRightNode);

  QTest::keyClick(canvas.view.get(), Qt::Key_Tab);
  EXPECT_EQ(canvas.selectedNode(), kBelowNode);

  QTest::keyClick(canvas.view.get(), Qt::Key_Tab);
  EXPECT_EQ(canvas.selectedNode(), kSourceNode);
}

TEST(OrcGraphicsViewNavigationTest, ShiftTabCyclesBackwards) {
  (void)ensureApplication();
  CanvasFixture canvas;

  canvas.scene->selectNode(kSourceNode);

  QTest::keyClick(canvas.view.get(), Qt::Key_Tab, Qt::ShiftModifier);
  EXPECT_EQ(canvas.selectedNode(), kBelowNode);

  QTest::keyClick(canvas.view.get(), Qt::Key_Tab, Qt::ShiftModifier);
  EXPECT_EQ(canvas.selectedNode(), kRightNode);
}

// A node that is already fully on screen must not move the canvas: enforcing a
// reveal margin unconditionally scrolled the whole DAG whenever the selection
// landed near a viewport edge, which looked like the cursor key had fallen
// through to the canvas panning underneath.
TEST(OrcGraphicsViewNavigationTest,
     SelectingAVisibleNodeDoesNotScrollTheCanvas) {
  (void)ensureApplication();
  CanvasFixture canvas;

  // Mirror MainWindow::positionViewToTopLeft(): the top-left node sits 20px
  // in from the corner of the viewport, well inside any reveal margin.
  canvas.scene->setSceneRect(-2000.0, -2000.0, 4000.0, 4000.0);
  const QRectF viewport_rect = canvas.view->viewport()->rect();
  canvas.view->centerOn(QPointF(viewport_rect.width() / 2 - 20.0,
                                viewport_rect.height() / 2 - 20.0));
  canvas.scene->selectNode(kSourceNode);
  QCoreApplication::processEvents();

  const int horizontal = canvas.view->horizontalScrollBar()->value();
  const int vertical = canvas.view->verticalScrollBar()->value();

  QTest::keyClick(canvas.view.get(), Qt::Key_Right);
  ASSERT_EQ(canvas.selectedNode(), kRightNode);

  EXPECT_EQ(canvas.view->horizontalScrollBar()->value(), horizontal);
  EXPECT_EQ(canvas.view->verticalScrollBar()->value(), vertical);
}

TEST(OrcGraphicsViewNavigationTest, SelectingAnOffscreenNodeScrollsItIntoView) {
  (void)ensureApplication();
  CanvasFixture canvas;

  canvas.scene->setSceneRect(-2000.0, -2000.0, 4000.0, 4000.0);
  canvas.view->resize(260, 200);
  QCoreApplication::processEvents();

  // Park the view on the source node so the node to its right is off screen.
  canvas.view->centerOn(QPointF(70.0, 40.0));
  QCoreApplication::processEvents();
  canvas.scene->selectNode(kSourceNode);

  const int horizontal = canvas.view->horizontalScrollBar()->value();

  QTest::keyClick(canvas.view.get(), Qt::Key_Right);
  ASSERT_EQ(canvas.selectedNode(), kRightNode);

  EXPECT_GT(canvas.view->horizontalScrollBar()->value(), horizontal);
}

}  // namespace gui_unit_test
