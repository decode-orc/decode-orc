/*
 * File:        frame_viewport_smoothing_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     FrameViewportWidget::setSmoothScaling on the raster surface:
 *              smoothing off paints nearest-neighbour blocks
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>
#include <orc/view-support/frame_viewport_widget.h>

#include <QApplication>
#include <QColor>
#include <QImage>
#include <QPixmap>
#include <set>

namespace gui_unit_test {

namespace {

QApplication& ensureApplication() {
  if (auto* existing =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing;
  }
  static int argc = 3;
  static char app_name[] = "orc-gui-widget-test";
  static char platform_opt[] = "-platform";
  static char platform_val[] = "offscreen";
  static char* argv[] = {app_name, platform_opt, platform_val, nullptr};
  static QApplication* app = new QApplication(argc, argv);
  return *app;
}

// Black and white columns: any scaling that filters shows a grey between
// them; nearest-neighbour shows only black and white.
QImage stripes() {
  QImage image(4, 2, QImage::Format_RGB888);
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      image.setPixelColor(x, y, x % 2 == 0 ? Qt::black : Qt::white);
    }
  }
  return image;
}

// The grey levels painted across the middle row of the frame.
std::set<int> levelsAcrossFrame(orc::view::FrameViewportWidget& view) {
  const QImage grabbed = view.grab().toImage();
  const QPointF left = view.widgetFromImage(QPointF(0.0, 1.0));
  const QPointF right = view.widgetFromImage(QPointF(4.0, 1.0));
  std::set<int> levels;
  const int y = static_cast<int>(left.y());
  for (int x = static_cast<int>(left.x()) + 1;
       x < static_cast<int>(right.x()) - 1; ++x) {
    levels.insert(qGray(grabbed.pixel(x, y)));
  }
  return levels;
}

}  // namespace

TEST(FrameViewportSmoothingTest, SmoothingIsOnByDefault) {
  ensureApplication();
  orc::view::FrameViewportWidget view;
  EXPECT_TRUE(view.smoothScaling());
}

TEST(FrameViewportSmoothingTest, SmoothingOffPaintsWithoutFiltering) {
  ensureApplication();
  orc::view::FrameViewportWidget view;
  view.resize(200, 120);
  view.setImage(stripes());
  view.setZoomLevel(8.0);
  view.setSmoothScaling(false);
  EXPECT_FALSE(view.smoothScaling());

  const auto levels = levelsAcrossFrame(view);
  EXPECT_EQ(levels, (std::set<int>{0, 255}))
      << "nearest-neighbour scaling left a filtered grey between samples";
}

TEST(FrameViewportSmoothingTest, SmoothingOnFiltersBetweenSamples) {
  ensureApplication();
  orc::view::FrameViewportWidget view;
  view.resize(200, 120);
  view.setImage(stripes());
  view.setZoomLevel(8.0);
  view.setSmoothScaling(true);

  const auto levels = levelsAcrossFrame(view);
  EXPECT_GT(levels.size(), 2u)
      << "smooth scaling painted only the two sample levels";
}

}  // namespace gui_unit_test
