/*
 * File:        orc_graphics_view_zoom_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Zoom slot tests for the DAG canvas view
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QSignalSpy>

#include "dag_zoom.h"
#include "orcgraphicsview.h"

namespace gui_unit_test {

namespace {

// Requires QApplication. Run under the "gui-widget" CTest label.
QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-view-zoom-test";
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

}  // namespace

TEST(OrcGraphicsViewZoomTest, ZoomOutAndInStepTheScaleAndSignalIt) {
  (void)ensureApplication();
  OrcGraphicsView view;
  view.resize(800, 600);
  view.show();
  QCoreApplication::processEvents();
  QSignalSpy spy(&view, &OrcGraphicsView::zoomScaleChanged);

  ASSERT_DOUBLE_EQ(view.zoomScale(), orc::gui::kDagDefaultScale);

  view.zoomOut();
  EXPECT_NEAR(view.zoomScale(),
              orc::gui::kDagDefaultScale / orc::gui::kDagZoomStep, 1e-9);
  ASSERT_EQ(spy.count(), 1);
  EXPECT_NEAR(spy.at(0).at(0).toDouble(), view.zoomScale(), 1e-9);

  view.zoomIn();
  EXPECT_NEAR(view.zoomScale(), orc::gui::kDagDefaultScale, 1e-9);
  EXPECT_EQ(spy.count(), 2);
}

TEST(OrcGraphicsViewZoomTest, ZoomStopsAtTheRangeEndsWithoutSignalling) {
  (void)ensureApplication();
  OrcGraphicsView view;
  view.show();
  QCoreApplication::processEvents();

  // Already at the top of the range: zooming in is a no-op.
  QSignalSpy spy(&view, &OrcGraphicsView::zoomScaleChanged);
  view.zoomIn();
  EXPECT_EQ(spy.count(), 0);

  for (int i = 0; i < 20; ++i) {
    view.zoomOut();
  }
  EXPECT_NEAR(view.zoomScale(), orc::gui::kDagMinScale, 1e-9);

  const int signals_at_floor = spy.count();
  view.zoomOut();
  EXPECT_EQ(spy.count(), signals_at_floor);
}

TEST(OrcGraphicsViewZoomTest, ResetZoomReturnsToNaturalSize) {
  (void)ensureApplication();
  OrcGraphicsView view;
  view.show();
  QCoreApplication::processEvents();

  view.zoomOut();
  view.zoomOut();
  view.resetZoom();
  EXPECT_NEAR(view.zoomScale(), orc::gui::kDagDefaultScale, 1e-9);
}

}  // namespace gui_unit_test
