/*
 * File:        overlay_primitive_builder_test.cpp
 * Module:      orc-gui-tests
 * Purpose:     Unit tests for frame-surface overlay geometry
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "gpu/overlay_primitive_builder.h"

#include <gtest/gtest.h>

#include <algorithm>

#include "gpu/overlay_primitives.h"

using orc::gui::FrameViewGeometry;
using orc::gui::gpu::OverlayPrimitiveBuilder;
using orc::gui::gpu::OverlayPrimitives;
using orc::gui::gpu::OverlayQuad;
using orc::gui::gpu::OverlaySpan;

namespace {

FrameViewGeometry makeGeometry(const QSize& image, const QSize& viewport,
                               double zoom = 1.0) {
  FrameViewGeometry geometry;
  geometry.setImageSize(image);
  geometry.setViewportSize(viewport);
  geometry.setZoom(zoom);
  return geometry;
}

TEST(OverlayPrimitiveBuilder, DropoutBandSpansTheSamplesOnItsScanline) {
  const auto geometry = makeGeometry(QSize(100, 50), QSize(100, 50));
  OverlayPrimitives out;
  OverlayPrimitiveBuilder::appendDropoutBand(out, geometry,
                                             OverlaySpan{10, 20, 40}, Qt::red,
                                             /*thickness=*/4);

  ASSERT_EQ(out.quads.size(), 1u);
  const QRectF& band = out.quads.front().rect;
  EXPECT_DOUBLE_EQ(band.left(), 20.0);
  EXPECT_DOUBLE_EQ(band.right(), 40.0);
  // Centred on the scanline, so half the thickness sits either side of it.
  EXPECT_DOUBLE_EQ(band.top(), 8.0);
  EXPECT_DOUBLE_EQ(band.height(), 4.0);
}

TEST(OverlayPrimitiveBuilder, DropoutBandSkipsSpansOutsideTheImage) {
  const auto geometry = makeGeometry(QSize(100, 50), QSize(100, 50));
  OverlayPrimitives out;

  for (const OverlaySpan& span : {OverlaySpan{-1, 0, 10},      // above
                                  OverlaySpan{50, 0, 10},      // below
                                  OverlaySpan{10, -1, 10},     // left of frame
                                  OverlaySpan{10, 0, 101},     // past the width
                                  OverlaySpan{10, 40, 40}}) {  // empty
    OverlayPrimitiveBuilder::appendDropoutBand(out, geometry, span, Qt::red, 4);
  }

  EXPECT_TRUE(out.quads.empty());
}

TEST(OverlayPrimitiveBuilder, DropoutBandThicknessTracksTheDisplayedHeight) {
  // One pixel on a thumbnail, four on anything tall, as the preview has
  // always drawn them.
  EXPECT_EQ(OverlayPrimitiveBuilder::dropoutBandThickness(0), 1);
  EXPECT_EQ(OverlayPrimitiveBuilder::dropoutBandThickness(199), 1);
  EXPECT_EQ(OverlayPrimitiveBuilder::dropoutBandThickness(600), 3);
  EXPECT_EQ(OverlayPrimitiveBuilder::dropoutBandThickness(4000), 4);
}

TEST(OverlayPrimitiveBuilder, CrosshairsCrossTheCentreOfTheNamedPixel) {
  const auto geometry = makeGeometry(QSize(100, 50), QSize(200, 100),
                                     /*zoom=*/2.0);
  OverlayPrimitives out;
  OverlayPrimitiveBuilder::appendCrosshairs(out, geometry, QPoint(10, 20),
                                            QRect(0, 0, 200, 100), Qt::green);

  ASSERT_EQ(out.lines.size(), 2u);
  // Pixel 10 covers widget x 20..22 at 2x, so its centre is 21.
  EXPECT_DOUBLE_EQ(out.lines[0].from.x(), 21.0);
  EXPECT_DOUBLE_EQ(out.lines[0].to.x(), 21.0);
  EXPECT_DOUBLE_EQ(out.lines[1].from.y(), 41.0);
  EXPECT_DOUBLE_EQ(out.lines[1].to.y(), 41.0);
}

TEST(OverlayPrimitiveBuilder, CrosshairsRunTheFullWidthAndHeightOfTheFrame) {
  const auto geometry = makeGeometry(QSize(100, 50), QSize(300, 200));
  const QRect bounds(100, 75, 100, 50);
  OverlayPrimitives out;
  OverlayPrimitiveBuilder::appendCrosshairs(out, geometry, QPoint(0, 0), bounds,
                                            Qt::green);

  ASSERT_EQ(out.lines.size(), 2u);
  EXPECT_DOUBLE_EQ(out.lines[0].from.y(), bounds.top());
  EXPECT_DOUBLE_EQ(out.lines[0].to.y(), bounds.bottom());
  EXPECT_DOUBLE_EQ(out.lines[1].from.x(), bounds.left());
  EXPECT_DOUBLE_EQ(out.lines[1].to.x(), bounds.right());
}

TEST(OverlayPrimitiveBuilder, EditorBandKeepsItsThicknessAsTheZoomGrows) {
  // The point of the constant thickness: at 0.25x a one-line band would be a
  // quarter of a pixel tall and neither visible nor clickable.
  for (double zoom : {0.25, 1.0, 4.0}) {
    const auto geometry = makeGeometry(QSize(100, 50), QSize(400, 400), zoom);
    const QRectF band = OverlayPrimitiveBuilder::regionBandRect(
        geometry, OverlaySpan{10, 20, 40}, /*emphasized=*/false);
    EXPECT_DOUBLE_EQ(band.height(), std::max(4.0, zoom)) << "zoom " << zoom;
  }
}

TEST(OverlayPrimitiveBuilder, AnEmphasisedBandIsThickerAroundTheSameCentre) {
  const auto geometry = makeGeometry(QSize(100, 50), QSize(100, 50));
  const QRectF plain = OverlayPrimitiveBuilder::regionBandRect(
      geometry, OverlaySpan{10, 20, 40}, false);
  const QRectF emphasised = OverlayPrimitiveBuilder::regionBandRect(
      geometry, OverlaySpan{10, 20, 40}, true);

  EXPECT_DOUBLE_EQ(emphasised.height(), plain.height() + 3.0);
  EXPECT_DOUBLE_EQ(emphasised.center().y(), plain.center().y());
  EXPECT_DOUBLE_EQ(emphasised.left(), plain.left());
  EXPECT_DOUBLE_EQ(emphasised.right(), plain.right());
}

TEST(OverlayPrimitiveBuilder, HandlesStraddleTheBandsEnds) {
  const QRectF band(20.0, 8.0, 20.0, 4.0);
  const QRectF left = OverlayPrimitiveBuilder::leftHandleRect(band);
  const QRectF right = OverlayPrimitiveBuilder::rightHandleRect(band);

  EXPECT_DOUBLE_EQ(left.center().x(), band.left());
  EXPECT_DOUBLE_EQ(right.center().x(), band.right());
  EXPECT_DOUBLE_EQ(left.center().y(), band.center().y());
  EXPECT_DOUBLE_EQ(left.width(), 8.0);
  EXPECT_DOUBLE_EQ(left.height(), 8.0);
}

TEST(OverlayPrimitiveBuilder, AStruckBandIsDashedAcrossItsMiddle) {
  const auto geometry = makeGeometry(QSize(100, 50), QSize(100, 50));
  OverlayPrimitives out;
  OverlayPrimitiveBuilder::appendRegionBand(out, geometry,
                                            OverlaySpan{10, 20, 40}, Qt::gray,
                                            /*emphasized=*/false,
                                            /*struck=*/true);

  ASSERT_FALSE(out.quads.empty());
  const QRectF band = out.quads.front().rect;

  // Everything after the band itself is a dash: they run along its centre,
  // stay inside it, and there is more than one of them.
  const std::vector<orc::gui::gpu::OverlayQuad> dashes(out.quads.begin() + 1,
                                                       out.quads.end());
  ASSERT_GT(dashes.size(), 1u);
  for (const OverlayQuad& dash : dashes) {
    EXPECT_NEAR(dash.rect.center().y(), band.center().y(), 0.001);
    EXPECT_GE(dash.rect.left(), band.left());
    EXPECT_LE(dash.rect.right(), band.right());
  }
}

TEST(OverlayPrimitiveBuilder, APlainBandIsJustTheBand) {
  const auto geometry = makeGeometry(QSize(100, 50), QSize(100, 50));
  OverlayPrimitives out;
  OverlayPrimitiveBuilder::appendRegionBand(
      out, geometry, OverlaySpan{10, 20, 40}, Qt::red, false, false);

  EXPECT_EQ(out.quads.size(), 1u);
  EXPECT_TRUE(out.lines.empty());
  // Translucent so the frame underneath still reads through it.
  EXPECT_EQ(out.quads.front().color.alpha(), 150);
}

TEST(OverlayPrimitiveBuilder, AnEmphasisedBandIsOutlinedAndMoreOpaque) {
  const auto geometry = makeGeometry(QSize(100, 50), QSize(100, 50));
  OverlayPrimitives out;
  OverlayPrimitiveBuilder::appendRegionBand(out, geometry,
                                            OverlaySpan{10, 20, 40}, Qt::red,
                                            /*emphasized=*/true, false);

  // The band plus the four edges of its outline.
  ASSERT_EQ(out.quads.size(), 5u);
  EXPECT_EQ(out.quads.front().color.alpha(), 220);
  const QRectF band = out.quads.front().rect;
  for (size_t i = 1; i < out.quads.size(); ++i) {
    EXPECT_TRUE(band.contains(out.quads[i].rect))
        << "outline edge " << i << " escapes the band";
  }
}

TEST(OverlayPrimitiveBuilder, NothingIsDrawnForASpanOffTheImage) {
  const auto geometry = makeGeometry(QSize(100, 50), QSize(100, 50));
  OverlayPrimitives out;
  OverlayPrimitiveBuilder::appendRegionBand(
      out, geometry, OverlaySpan{99, 20, 40}, Qt::red, true, true);
  EXPECT_TRUE(out.quads.empty());

  OverlayPrimitiveBuilder::appendResizeHandles(out, QRectF());
  EXPECT_TRUE(out.quads.empty());
}

TEST(OverlayPrimitives, ScalingToDevicePixelsMovesEveryCoordinate) {
  OverlayPrimitives source;
  source.quads.push_back({QRectF(10.0, 20.0, 30.0, 40.0), Qt::red});
  source.lines.push_back({QPointF(1.0, 2.0), QPointF(3.0, 4.0), Qt::green});

  const OverlayPrimitives scaled = orc::gui::gpu::scaled(source, 2.0);

  EXPECT_EQ(scaled.quads.front().rect, QRectF(20.0, 40.0, 60.0, 80.0));
  EXPECT_EQ(scaled.lines.front().from, QPointF(2.0, 4.0));
  EXPECT_EQ(scaled.lines.front().to, QPointF(6.0, 8.0));
  // Colours are not a coordinate.
  EXPECT_EQ(scaled.quads.front().color, QColor(Qt::red));
}

TEST(OverlayPrimitives, AnOutlineThinnerThanItsEdgesBecomesASolidQuad) {
  // A band only a couple of pixels tall cannot hold two one-pixel edges plus
  // a gap; filling it is right, and overlapping edges would double-blend.
  OverlayPrimitives out;
  orc::gui::gpu::appendRectOutline(out, QRectF(0.0, 0.0, 20.0, 2.0), 1.0,
                                   Qt::black);
  ASSERT_EQ(out.quads.size(), 1u);
  EXPECT_EQ(out.quads.front().rect, QRectF(0.0, 0.0, 20.0, 2.0));
}

}  // namespace
