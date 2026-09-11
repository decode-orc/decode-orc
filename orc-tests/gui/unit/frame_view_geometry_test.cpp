/*
 * File:        frame_view_geometry_test.cpp
 * Module:      orc-gui-tests
 * Purpose:     Unit tests for shared frame viewport display geometry
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "frame_view_geometry.h"

#include <gtest/gtest.h>

using orc::gui::FrameViewGeometry;

namespace {

FrameViewGeometry makeGeometry(const QSize& image, const QSize& viewport,
                               double aspect = 1.0, double zoom = 1.0) {
  FrameViewGeometry geometry;
  geometry.setImageSize(image);
  geometry.setViewportSize(viewport);
  geometry.setAspectCorrection(aspect);
  geometry.setZoom(zoom);
  return geometry;
}

TEST(FrameViewGeometry_DisplaySize, IdentityAtUnityZoomAndAspect) {
  auto geometry = makeGeometry(QSize(928, 625), QSize(928, 625));
  EXPECT_EQ(geometry.displaySize(), QSize(928, 625));
}

TEST(FrameViewGeometry_DisplaySize, ScalesWidthByAspectCorrection) {
  auto geometry = makeGeometry(QSize(1000, 500), QSize(1000, 500), 0.7);
  EXPECT_EQ(geometry.displaySize(), QSize(700, 500));
}

TEST(FrameViewGeometry_DisplaySize, ScalesBothAxesByZoom) {
  auto geometry = makeGeometry(QSize(100, 50), QSize(400, 400), 1.0, 2.0);
  EXPECT_EQ(geometry.displaySize(), QSize(200, 100));
}

TEST(FrameViewGeometry_DisplaySize, EmptyWhenNoImage) {
  FrameViewGeometry geometry;
  geometry.setViewportSize(QSize(800, 600));
  EXPECT_FALSE(geometry.hasImage());
  EXPECT_TRUE(geometry.displaySize().isEmpty());
  EXPECT_TRUE(geometry.targetRect().isEmpty());
}

TEST(FrameViewGeometry_TargetRect, CentersDisplayWithinViewport) {
  auto geometry = makeGeometry(QSize(100, 100), QSize(300, 200));
  EXPECT_EQ(geometry.targetRect(), QRect(100, 50, 100, 100));
}

TEST(FrameViewGeometry_TargetRect, CoversWidgetWhenViewportMatchesDisplay) {
  auto geometry = makeGeometry(QSize(200, 100), QSize(400, 200), 1.0, 2.0);
  EXPECT_EQ(geometry.targetRect(), QRect(0, 0, 400, 200));
}

TEST(FrameViewGeometry_FitZoom, LimitedByWidth) {
  // Corrected width 700 into 350 viewport -> 0.5; height 500 into 500 -> 1.0
  auto geometry = makeGeometry(QSize(1000, 500), QSize(350, 500), 0.7);
  EXPECT_DOUBLE_EQ(geometry.fitZoom(), 0.5);
}

TEST(FrameViewGeometry_FitZoom, LimitedByHeight) {
  auto geometry = makeGeometry(QSize(100, 400), QSize(400, 200));
  EXPECT_DOUBLE_EQ(geometry.fitZoom(), 0.5);
}

TEST(FrameViewGeometry_FitZoom, UnityFallbackWithoutViewport) {
  FrameViewGeometry geometry;
  geometry.setImageSize(QSize(100, 100));
  EXPECT_DOUBLE_EQ(geometry.fitZoom(), 1.0);
}

TEST(FrameViewGeometry_Mapping, WidgetImageRoundTripAtZoom) {
  auto geometry = makeGeometry(QSize(928, 625), QSize(1856, 1250), 1.0, 2.0);

  const QPointF image_pos(464.0, 312.0);
  const QPointF widget_pos = geometry.widgetFromImage(image_pos);
  const QPointF back = geometry.imageFromWidget(widget_pos);

  EXPECT_NEAR(back.x(), image_pos.x(), 1e-9);
  EXPECT_NEAR(back.y(), image_pos.y(), 1e-9);
}

TEST(FrameViewGeometry_Mapping, RoundTripWithAspectCorrection) {
  auto geometry = makeGeometry(QSize(1135, 625), QSize(2000, 1500), 0.7, 1.5);

  const QPointF image_pos(1000.25, 313.5);
  const QPointF back =
      geometry.imageFromWidget(geometry.widgetFromImage(image_pos));

  EXPECT_NEAR(back.x(), image_pos.x(), 1e-9);
  EXPECT_NEAR(back.y(), image_pos.y(), 1e-9);
}

TEST(FrameViewGeometry_Mapping, LetterboxOffsetAccounted) {
  // 100x100 image centered in 300x200 viewport -> target rect at (100, 50)
  auto geometry = makeGeometry(QSize(100, 100), QSize(300, 200));

  EXPECT_EQ(geometry.imagePixelFromWidget(QPoint(100, 50)), QPoint(0, 0));
  EXPECT_EQ(geometry.imagePixelFromWidget(QPoint(150, 100)), QPoint(50, 50));
}

TEST(FrameViewGeometry_Mapping, PixelClampedToImageBounds) {
  auto geometry = makeGeometry(QSize(100, 100), QSize(100, 100));

  EXPECT_EQ(geometry.imagePixelFromWidget(QPoint(-10, -10)), QPoint(0, 0));
  EXPECT_EQ(geometry.imagePixelFromWidget(QPoint(500, 500)), QPoint(99, 99));
}

TEST(FrameViewGeometry_Mapping, WidgetPointOnImageRespectsLetterbox) {
  auto geometry = makeGeometry(QSize(100, 100), QSize(300, 200));

  EXPECT_TRUE(geometry.widgetPointOnImage(QPoint(150, 100)));
  EXPECT_FALSE(geometry.widgetPointOnImage(QPoint(10, 100)));   // left bar
  EXPECT_FALSE(geometry.widgetPointOnImage(QPoint(250, 100)));  // right bar
}

TEST(FrameViewGeometry_ScrollAfterZoom, KeepsCursorContentStationary) {
  // Content point under the cursor: scroll 100 + cursor 50 = 150.
  // After zooming 2x it sits at 300; new scroll must be 300 - 50 = 250.
  const QPoint new_scroll =
      FrameViewGeometry::scrollAfterZoom(QPoint(100, 40), QPoint(50, 20), 2.0);
  EXPECT_EQ(new_scroll, QPoint(250, 100));
}

TEST(FrameViewGeometry_ScrollAfterZoom, IdentityAtUnityRatio) {
  const QPoint scroll(123, 456);
  EXPECT_EQ(FrameViewGeometry::scrollAfterZoom(scroll, QPoint(10, 10), 1.0),
            scroll);
}

TEST(FrameViewGeometry_VisibleOrigin, PansTheDisplayRectWithoutResizingIt) {
  auto geometry = makeGeometry(QSize(800, 600), QSize(400, 300), 1.0, 2.0);
  ASSERT_EQ(geometry.displaySize(), QSize(1600, 1200));
  EXPECT_EQ(geometry.targetRect(), QRect(0, 0, 1600, 1200));

  geometry.setVisibleOrigin(QPoint(300, 150));
  EXPECT_EQ(geometry.targetRect(), QRect(-300, -150, 1600, 1200));
}

TEST(FrameViewGeometry_VisibleOrigin, StopsAtTheEdgeOfTheContent) {
  auto geometry = makeGeometry(QSize(800, 600), QSize(400, 300), 1.0, 2.0);
  EXPECT_EQ(geometry.maxVisibleOrigin(), QSize(1200, 900));

  geometry.setVisibleOrigin(QPoint(5000, 5000));
  EXPECT_EQ(geometry.visibleOrigin(), QPoint(1200, 900));

  geometry.setVisibleOrigin(QPoint(-40, -40));
  EXPECT_EQ(geometry.visibleOrigin(), QPoint(0, 0));
}

TEST(FrameViewGeometry_VisibleOrigin, StaysAtZeroWhileTheContentFits) {
  auto geometry = makeGeometry(QSize(200, 100), QSize(400, 300));
  EXPECT_EQ(geometry.maxVisibleOrigin(), QSize(0, 0));

  geometry.setVisibleOrigin(QPoint(50, 50));
  EXPECT_EQ(geometry.visibleOrigin(), QPoint(0, 0));
  // ... and the content is still centred, as it was before panning existed.
  EXPECT_EQ(geometry.targetRect(), QRect(100, 100, 200, 100));
}

TEST(FrameViewGeometry_VisibleOrigin,
     IsPulledBackWhenZoomingOutShrinksTheRange) {
  auto geometry = makeGeometry(QSize(800, 600), QSize(400, 300), 1.0, 2.0);
  geometry.setVisibleOrigin(QPoint(1200, 900));

  geometry.setZoom(1.0);
  EXPECT_EQ(geometry.maxVisibleOrigin(), QSize(400, 300));
  EXPECT_EQ(geometry.visibleOrigin(), QPoint(400, 300));
}

TEST(FrameViewGeometry_VisibleOrigin, RoundTripsAtEveryZoomInTheEditorRange) {
  // The dropout editor's range. A point that maps to the widget and back must
  // land on itself at every step, or a band would be drawn somewhere other
  // than where it can be grabbed.
  for (double zoom = 0.25; zoom <= 8.0; zoom *= 2.0) {
    auto geometry = makeGeometry(QSize(928, 625), QSize(400, 300), 0.7, zoom);
    geometry.setVisibleOrigin(QPoint(123, 77));

    for (const QPointF& image_point :
         {QPointF(0, 0), QPointF(400.5, 300.25), QPointF(927, 624)}) {
      const QPointF widget_point = geometry.widgetFromImage(image_point);
      const QPointF back = geometry.imageFromWidget(widget_point);
      EXPECT_NEAR(back.x(), image_point.x(), 1e-9) << "zoom " << zoom;
      EXPECT_NEAR(back.y(), image_point.y(), 1e-9) << "zoom " << zoom;
    }
  }
}

TEST(FrameViewGeometry_VisibleOrigin, ZoomAtCursorKeepsThePointUnderItFixed) {
  // What Ctrl+wheel does: apply scrollAfterZoom to the origin, and the image
  // pixel under the cursor is the one that must not move.
  auto geometry = makeGeometry(QSize(928, 625), QSize(400, 300), 1.0, 2.0);
  geometry.setVisibleOrigin(QPoint(200, 120));

  const QPoint cursor(180, 140);
  const QPointF image_under_cursor = geometry.imageFromWidget(cursor);

  const double ratio = 1.5;
  const QPoint old_origin = geometry.visibleOrigin();
  geometry.setZoom(geometry.zoom() * ratio);
  geometry.setVisibleOrigin(
      FrameViewGeometry::scrollAfterZoom(old_origin, cursor, ratio));

  const QPointF after = geometry.imageFromWidget(cursor);
  EXPECT_NEAR(after.x(), image_under_cursor.x(), 0.5);
  EXPECT_NEAR(after.y(), image_under_cursor.y(), 0.5);
}

TEST(FrameViewGeometry_Validation, RejectsNonPositiveZoomAndAspect) {
  FrameViewGeometry geometry;
  geometry.setZoom(-2.0);
  EXPECT_DOUBLE_EQ(geometry.zoom(), 1.0);
  geometry.setAspectCorrection(0.0);
  EXPECT_DOUBLE_EQ(geometry.aspectCorrection(), 1.0);
}

}  // namespace
