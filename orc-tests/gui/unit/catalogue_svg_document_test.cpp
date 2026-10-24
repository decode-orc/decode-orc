/*
 * File:        catalogue_svg_document_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 1 tests of the SVG document a display list is saved as,
 *              and of the SMIL animation that carries its presentation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "catalogue_svg_document.h"

#include <gtest/gtest.h>

#include <QBrush>
#include <QColor>
#include <QImage>
#include <QPainterPath>
#include <QString>
#include <QTransform>
#include <optional>

namespace gui_unit_test {
namespace {

using orc::gui::CanvasPaint;
using orc::gui::SvgDocumentCanvas;

const orc::CatalogueColour kRed{255, 0, 0};
const orc::CatalogueColour kGreen{0, 255, 0};
const orc::CatalogueColour kBlue{0, 0, 255};

const QSizeF kSize(100.0, 80.0);
const QRectF kArea(0.0, 0.0, 100.0, 80.0);

CanvasPaint solid(const orc::CatalogueColour& colour) {
  return CanvasPaint{QBrush(QColor(colour.red, colour.green, colour.blue)),
                     colour, std::nullopt};
}

orc::CatalogueColour inEntry(orc::CatalogueColour colour, int16_t entry) {
  colour.palette_index = entry;
  return colour;
}

/// A square filled by the winding rule, SVG's default: QPainterPath's own
/// default is odd-even.
QPainterPath square(qreal x, qreal y, qreal side) {
  QPainterPath path;
  path.setFillRule(Qt::WindingFill);
  path.addRect(x, y, side, side);
  return path;
}

QString written(const SvgDocumentCanvas& canvas) {
  return QString::fromUtf8(canvas.document());
}

/// A list whose palette entry 3 is red, turns blue at 500 ms, and from
/// 1000 ms loops green then red every 800 ms.
orc::CatalogueDisplayList listWithAChangingEntry() {
  orc::CatalogueDisplayList list;
  list.palette_tracks.resize(16);
  orc::CataloguePaletteTrack& track = list.palette_tracks[3];
  track.steps = {orc::CatalogueColourStep{0, kRed},
                 orc::CatalogueColourStep{500, kBlue},
                 orc::CatalogueColourStep{1000, kGreen},
                 orc::CatalogueColourStep{1400, kRed}};
  track.loop_start_ms = 1000;
  track.loop_period_ms = 800;
  return list;
}

}  // namespace

// The document shows the drawable area at the size an image of it would have,
// on the black ground a view draws, with each fill a path in its colour.
TEST(SvgDocumentCanvasTest, WritesEachFillAsAPathInItsColour) {
  SvgDocumentCanvas canvas(kSize, kArea, orc::CatalogueDisplayList{},
                           /*animated=*/false);
  canvas.fillPath(square(10, 10, 20), solid(kRed));

  const QString svg = written(canvas);
  EXPECT_TRUE(svg.contains(
      QStringLiteral("width=\"100\" height=\"80\" viewBox=\"0 0 100 80\"")));
  EXPECT_TRUE(svg.contains(QStringLiteral(
      "<rect x=\"0\" y=\"0\" width=\"100\" height=\"80\" fill=\"#000000\"/>")));
  EXPECT_TRUE(svg.contains(QStringLiteral(
      "<path d=\"M10 10L30 10L30 30L10 30L10 10\" fill=\"#ff0000\"/>")))
      << svg.toStdString();
}

// The pixel modes and the odd-even figures both depend on the rule a path is
// filled by: a frame drawn as one path keeps its hole.
TEST(SvgDocumentCanvasTest, KeepsAPathsFillRule) {
  SvgDocumentCanvas canvas(kSize, kArea, orc::CatalogueDisplayList{}, false);
  QPainterPath frame = square(10, 10, 40);
  frame.addPath(square(20, 20, 20));
  frame.setFillRule(Qt::OddEvenFill);
  canvas.fillPath(frame, solid(kRed));
  EXPECT_TRUE(
      written(canvas).contains(QStringLiteral("fill-rule=\"evenodd\"")));
}

// A pixel of a receiver is drawn hard-edged whatever the rest of the drawing
// does.
TEST(SvgDocumentCanvasTest, DrawsAHardEdgedRectangleWithoutSmoothing) {
  SvgDocumentCanvas canvas(kSize, kArea, orc::CatalogueDisplayList{}, false);
  canvas.fillRect(QRectF(1, 2, 3, 4), solid(kRed), /*hard_edged=*/true);
  EXPECT_TRUE(written(canvas).contains(
      QStringLiteral("<rect x=\"1\" y=\"2\" width=\"3\" height=\"4\" "
                     "fill=\"#ff0000\" shape-rendering=\"crispEdges\"/>")));
}

// A stage is kept out of the drawing until the moment it appears, and then
// stays: a set with no end.
TEST(SvgDocumentCanvasTest, KeepsALaterStageOutOfTheDrawingUntilItAppears) {
  SvgDocumentCanvas canvas(kSize, kArea, orc::CatalogueDisplayList{},
                           /*animated=*/true);
  canvas.beginStage(0);
  canvas.fillPath(square(10, 10, 20), solid(kRed));
  canvas.beginStage(1500);
  canvas.fillPath(square(40, 10, 20), solid(kRed));

  const QString svg = written(canvas);
  const qsizetype first = svg.indexOf(QStringLiteral("M10 10"));
  const qsizetype hidden = svg.indexOf(QStringLiteral(
      "<g display=\"none\"><set attributeName=\"display\" to=\"inline\" "
      "begin=\"1500ms\"/>"));
  const qsizetype second = svg.indexOf(QStringLiteral("M40 10"));
  ASSERT_GE(first, 0);
  EXPECT_GT(hidden, first) << "the first stage was hidden with the second";
  EXPECT_GT(second, hidden) << "the second stage was drawn from the start";
}

// A still document is the finished page: every stage, and no animation.
TEST(SvgDocumentCanvasTest, AStillDocumentShowsEveryStage) {
  SvgDocumentCanvas canvas(kSize, kArea, listWithAChangingEntry(),
                           /*animated=*/false);
  canvas.beginStage(0);
  canvas.fillPath(square(10, 10, 20), solid(inEntry(kRed, 3)));
  canvas.beginStage(1500);
  canvas.fillPath(square(40, 10, 20), solid(kRed));

  const QString svg = written(canvas);
  EXPECT_FALSE(svg.contains(QStringLiteral("display=\"none\"")));
  EXPECT_FALSE(svg.contains(QStringLiteral("<set")));
  EXPECT_FALSE(svg.contains(QStringLiteral("<animate")));
  EXPECT_FALSE(svg.contains(QStringLiteral("url(#p")))
      << "a still document painted from the entry rather than its colour";
}

// A figure in a changing palette entry is painted from the gradient standing
// for the entry: its first colour, each step up to the loop at its moment, and
// the loop repeated for good from where it starts — the whole of what a view
// plays, as SMIL.
TEST(SvgDocumentCanvasTest, FollowsAChangingPaletteEntryThroughItsGradient) {
  SvgDocumentCanvas canvas(kSize, kArea, listWithAChangingEntry(),
                           /*animated=*/true);
  canvas.fillPath(square(10, 10, 20), solid(inEntry(kRed, 3)));

  const QString svg = written(canvas);
  EXPECT_TRUE(svg.contains(QStringLiteral("fill=\"url(#p3)\"")));
  EXPECT_TRUE(svg.contains(QStringLiteral(
      "<linearGradient id=\"p3\" gradientUnits=\"userSpaceOnUse\">"
      "<stop offset=\"0\" stop-color=\"#ff0000\">"
      "<set attributeName=\"stop-color\" to=\"#0000ff\" begin=\"500ms\"/>"
      "<animate attributeName=\"stop-color\" calcMode=\"discrete\" "
      "begin=\"1000ms\" dur=\"800ms\" repeatCount=\"indefinite\" "
      "values=\"#00ff00;#ff0000\" keyTimes=\"0;0.5000000\"/>"
      "</stop></linearGradient>")));
}

// An entry that never changes is just its colour, and only the entries a fill
// follows get a gradient.
TEST(SvgDocumentCanvasTest, AnEntryThatHoldsStillIsItsColour) {
  SvgDocumentCanvas canvas(kSize, kArea, listWithAChangingEntry(),
                           /*animated=*/true);
  canvas.fillPath(square(10, 10, 20), solid(inEntry(kBlue, 5)));

  const QString svg = written(canvas);
  EXPECT_TRUE(svg.contains(QStringLiteral("fill=\"#0000ff\"")));
  EXPECT_FALSE(svg.contains(QStringLiteral("<linearGradient")));
}

// A texture is its brush's own tile, placed on the page by the brush's
// transform — the size and registration the standard gives the pattern.
TEST(SvgDocumentCanvasTest, DrawsATextureAsAPatternPlacedByItsBrush) {
  QImage tile(2, 2, QImage::Format_ARGB32_Premultiplied);
  tile.fill(Qt::transparent);
  tile.setPixelColor(0, 0, QColor(255, 0, 0));
  tile.setPixelColor(1, 1, QColor(255, 0, 0));
  QBrush brush(tile);
  QTransform to_page;
  to_page.translate(5, 7);
  to_page.scale(3, -3);
  brush.setTransform(to_page);

  SvgDocumentCanvas canvas(kSize, kArea, orc::CatalogueDisplayList{}, false);
  canvas.fillPath(square(10, 10, 20), CanvasPaint{brush, kRed, std::nullopt});
  canvas.fillPath(square(40, 10, 20), CanvasPaint{brush, kRed, std::nullopt});

  const QString svg = written(canvas);
  EXPECT_TRUE(svg.contains(QStringLiteral(
      "<pattern id=\"t0\" patternUnits=\"userSpaceOnUse\" width=\"2\" "
      "height=\"2\" patternTransform=\"matrix(3 0 0 -3 5 7)\" "
      "shape-rendering=\"crispEdges\">"
      "<rect x=\"0\" y=\"0\" width=\"1\" height=\"1\" fill=\"#ff0000\"/>"
      "<rect x=\"1\" y=\"1\" width=\"1\" height=\"1\" fill=\"#ff0000\"/>"
      "</pattern>")));
  EXPECT_EQ(svg.count(QStringLiteral("<pattern ")), 1)
      << "one tile used twice was written twice";
  EXPECT_EQ(svg.count(QStringLiteral("fill=\"url(#t0)\"")), 2);
}

// A stage swept onto the screen is shown from its sweep's start, clipped to a
// rectangle that takes each position of the sweep from that position's moment
// — a discrete animation, so the document holds each until the next as a view
// does — and that grows to the whole viewport when the stage is whole.
TEST(SvgDocumentCanvasTest, ShowsASweptStageThroughItsSweep) {
  SvgDocumentCanvas canvas(kSize, kArea, orc::CatalogueDisplayList{},
                           /*animated=*/true);
  canvas.beginStage(0);
  canvas.fillPath(square(10, 10, 20), solid(kRed));
  canvas.beginStage(
      1000, {SvgDocumentCanvas::RevealFrame{800, QRectF(0, 0, 100, 0)},
             SvgDocumentCanvas::RevealFrame{816, QRectF(0, 0, 100, 10)},
             SvgDocumentCanvas::RevealFrame{833, QRectF(0, 0, 100, 20)}});
  canvas.fillPath(square(40, 10, 20), solid(kRed));

  const QString svg = written(canvas);
  EXPECT_TRUE(svg.contains(QStringLiteral(
      "<clipPath id=\"r0\" clipPathUnits=\"userSpaceOnUse\"><rect x=\"0\" "
      "y=\"0\" width=\"100\" height=\"0\">")))
      << svg.toStdString();
  EXPECT_TRUE(svg.contains(QStringLiteral(
      "<animate attributeName=\"height\" calcMode=\"discrete\" "
      "begin=\"800ms\" dur=\"200ms\" fill=\"freeze\" values=\"0;10;20\" "
      "keyTimes=\"0.0000000;0.0800000;0.1650000\"/>")));
  EXPECT_TRUE(svg.contains(QStringLiteral(
      "<set attributeName=\"height\" to=\"80\" begin=\"1000ms\"/>")));
  const qsizetype group = svg.indexOf(QStringLiteral(
      "<g display=\"none\" clip-path=\"url(#r0)\"><set "
      "attributeName=\"display\" to=\"inline\" begin=\"800ms\"/>"));
  const qsizetype second = svg.indexOf(QStringLiteral("M40 10"));
  ASSERT_GE(group, 0);
  EXPECT_GT(second, group) << "the swept square was drawn outside its sweep";
}

// A still document has no sweep: the stage is drawn whole like any other.
TEST(SvgDocumentCanvasTest, AStillDocumentDrawsASweptStageWhole) {
  SvgDocumentCanvas canvas(kSize, kArea, orc::CatalogueDisplayList{},
                           /*animated=*/false);
  canvas.beginStage(
      1000, {SvgDocumentCanvas::RevealFrame{800, QRectF(0, 0, 100, 0)},
             SvgDocumentCanvas::RevealFrame{816, QRectF(0, 0, 100, 10)}});
  canvas.fillPath(square(40, 10, 20), solid(kRed));
  const QString svg = written(canvas);
  EXPECT_FALSE(svg.contains(QStringLiteral("clipPath")));
  EXPECT_FALSE(svg.contains(QStringLiteral("<set")));
}

}  // namespace gui_unit_test
