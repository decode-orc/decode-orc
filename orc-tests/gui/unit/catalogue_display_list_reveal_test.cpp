/*
 * File:        catalogue_display_list_reveal_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 3 offscreen tests that the display-list view sweeps an
 *              operation onto the screen a row at a time, as its source drew
 *              it, and that a still and the saved document agree with it
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QImage>
#include <QSize>
#include <QString>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "catalogue_animation_clock.h"
#include "cataloguedisplaylistwidget.h"

namespace gui_unit_test {
namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }
  static int argc = 3;
  static char app_name[] = "orc-gui-catalogue-reveal-test";
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

/// A clock a test moves by hand.
class HandClock : public ICatalogueAnimationClock {
 public:
  int64_t nowMs() const override { return now_ms_; }
  int64_t now_ms_ = 0;
};

const orc::CatalogueColour kWhite{255, 255, 255};

// Every test draws into a square image this many pixels across, over a square
// unit screen.
constexpr int kSize = 400;

/// A white rectangle over the whole unit screen, swept onto it over the 200 ms
/// before 1000 ms from unit y |from| to |to|.
orc::CatalogueDrawOp sweptSquare(double from, double to) {
  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kRectangle;
  op.filled = true;
  op.colour = kWhite;
  op.origin = orc::CataloguePoint{0.0, 0.0};
  op.size = orc::CatalogueSize{1.0, 1.0};
  op.points.push_back(op.origin);
  op.appear_ms = 1000;
  op.reveal_duration_ms = 200;
  op.reveal_from_y = from;
  op.reveal_to_y = to;
  return op;
}

orc::CatalogueDisplayList squareList(std::vector<orc::CatalogueDrawOp> ops) {
  orc::CatalogueDisplayList list;
  list.aspect_height = 1.0;
  list.ops = std::move(ops);
  return list;
}

QImage frameAt(const orc::CatalogueDisplayList& list, uint32_t time_ms) {
  CatalogueDisplayListWidget widget;
  widget.setDisplayList(list);
  return widget.renderFrameImage(QSize(kSize, kSize), time_ms);
}

bool whiteAt(const QImage& image, int x, int y) {
  return image.pixelColor(x, y) == QColor(Qt::white);
}

bool blackAt(const QImage& image, int x, int y) {
  return image.pixelColor(x, y) == QColor(Qt::black);
}

}  // namespace

// A sweep down the screen shows the part of the figure above its line. Half
// way through the sweep's 200 ms — the field that starts 100 ms in — the line
// is half way down, so the top half is drawn and the bottom half is not;
// before the sweep starts nothing is, and from the appear time all of it.
TEST(CatalogueDisplayListRevealTest, ASweepDownShowsWhatIsAboveItsLine) {
  ensureApplication();
  const orc::CatalogueDisplayList list = squareList({sweptSquare(1.0, 0.0)});

  const QImage before = frameAt(list, 799);
  EXPECT_TRUE(blackAt(before, 200, 20));

  const QImage half = frameAt(list, 900);
  EXPECT_TRUE(whiteAt(half, 200, 20));
  EXPECT_TRUE(whiteAt(half, 200, 190));
  EXPECT_TRUE(blackAt(half, 200, 210));
  EXPECT_TRUE(blackAt(half, 200, 380));

  const QImage whole = frameAt(list, 1000);
  EXPECT_TRUE(whiteAt(whole, 200, 20));
  EXPECT_TRUE(whiteAt(whole, 200, 380));
}

// A sweep up the screen — the Norpak TTX 650's way — shows the part below.
TEST(CatalogueDisplayListRevealTest, ASweepUpShowsWhatIsBelowItsLine) {
  ensureApplication();
  const orc::CatalogueDisplayList list = squareList({sweptSquare(0.0, 1.0)});

  const QImage half = frameAt(list, 900);
  EXPECT_TRUE(blackAt(half, 200, 20));
  EXPECT_TRUE(blackAt(half, 200, 190));
  EXPECT_TRUE(whiteAt(half, 200, 210));
  EXPECT_TRUE(whiteAt(half, 200, 380));
}

// A still is the finished page: a swept figure is drawn whole, exactly as the
// same figure with no sweep is, pixel for pixel.
TEST(CatalogueDisplayListRevealTest, AStillIsTheSameWithOrWithoutASweep) {
  ensureApplication();
  orc::CatalogueDrawOp swept = sweptSquare(1.0, 0.0);
  swept.size = orc::CatalogueSize{0.5, 0.4};
  swept.origin = orc::CataloguePoint{0.2, 0.3};
  orc::CatalogueDrawOp whole = swept;
  whole.reveal_duration_ms = 0;
  whole.reveal_from_y = 0.0;
  whole.reveal_to_y = 0.0;

  CatalogueDisplayListWidget with_sweep;
  with_sweep.setDisplayList(squareList({swept}));
  CatalogueDisplayListWidget without;
  without.setDisplayList(squareList({whole}));
  const QImage a = with_sweep.renderPageImage(QSize(kSize, kSize));
  const QImage b = without.renderPageImage(QSize(kSize, kSize));
  ASSERT_FALSE(a.isNull());
  EXPECT_TRUE(a == b);
  const QByteArray still_a = with_sweep.renderSvgDocument(/*animated=*/false);
  const QByteArray still_b = without.renderSvgDocument(/*animated=*/false);
  EXPECT_EQ(still_a, still_b);
}

// A receiver's pixel page sweeps its runs a whole row at a time: a run is
// shown once the line has passed all of it and not before. Four rows over the
// 160 ms before 1000 ms; at the field 50 ms in, 50/160 of four rows is one and
// a quarter, so the top row is drawn and the second is not.
TEST(CatalogueDisplayListRevealTest, APixelPageSweepsAWholeRowAtATime) {
  ensureApplication();
  orc::CatalogueDisplayList list = squareList({});
  list.nominal_width = 4;
  list.nominal_height = 4;
  list.pixel_aligned = true;
  for (int row = 0; row < 4; ++row) {
    orc::CatalogueDrawOp run = sweptSquare(1.0, 0.0);
    run.origin = orc::CataloguePoint{0.0, row * 0.25};
    run.size = orc::CatalogueSize{1.0, 0.25};
    run.points = {run.origin};
    run.reveal_duration_ms = 160;
    list.ops.push_back(run);
  }

  const QImage image = frameAt(list, 890);
  // Device rows 0 to 99 are the top grid row, 100 to 199 the next.
  EXPECT_TRUE(whiteAt(image, 200, 0));
  EXPECT_TRUE(whiteAt(image, 200, 99));
  EXPECT_TRUE(blackAt(image, 200, 100));
  EXPECT_TRUE(blackAt(image, 200, 399));
}

// A sweep is a change of the screen at each field, so a playing view wakes for
// each one: a field apart, once the sweep has started.
TEST(CatalogueDisplayListRevealTest, AViewWakesAtEachFieldOfASweep) {
  ensureApplication();
  auto clock = std::make_shared<HandClock>();
  CatalogueDisplayListWidget widget(clock);
  widget.resize(kSize, kSize);
  widget.show();
  widget.setDisplayList(squareList({sweptSquare(1.0, 0.0)}));
  ASSERT_TRUE(widget.isPlaying());

  clock->now_ms_ = 816;
  widget.fireScheduledWake();
  EXPECT_EQ(widget.scheduledWakeMs(), std::optional<int>(833 - 816));
  clock->now_ms_ = 990;
  widget.fireScheduledWake();
  EXPECT_EQ(widget.scheduledWakeMs(), std::optional<int>(1000 - 990));
}

// The saved document sweeps the figure on through the same positions as the
// view: shown from the sweep's start, clipped to a rectangle that takes each
// field's position in turn, and unclipped at its appear time.
TEST(CatalogueDisplayListRevealTest, TheSavedDocumentSweepsLikeTheView) {
  ensureApplication();
  CatalogueDisplayListWidget widget;
  widget.setDisplayList(squareList({sweptSquare(1.0, 0.0)}));
  const QString svg = QString::fromUtf8(widget.renderSvgDocument(true));
  EXPECT_TRUE(svg.contains(QStringLiteral("<clipPath id=\"r0\"")));
  EXPECT_TRUE(svg.contains(
      QStringLiteral("<animate attributeName=\"height\" calcMode=\"discrete\" "
                     "begin=\"800ms\" dur=\"200ms\" fill=\"freeze\"")));
  EXPECT_TRUE(svg.contains(QStringLiteral(
      "<set attributeName=\"height\" to=\"720\" begin=\"1000ms\"/>")))
      << svg.toStdString();
  EXPECT_TRUE(svg.contains(QStringLiteral(
      "<g display=\"none\" clip-path=\"url(#r0)\"><set "
      "attributeName=\"display\" to=\"inline\" begin=\"800ms\"/>")));
}

}  // namespace gui_unit_test
