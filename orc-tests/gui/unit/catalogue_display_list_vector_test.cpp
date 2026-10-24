/*
 * File:        catalogue_display_list_vector_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Tier 3 offscreen tests that the display-list renderer draws
 *              vector operations the way ANSI X3.110 describes them
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QColor>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QImage>
#include <QRect>
#include <QSize>
#include <cmath>
#include <string>
#include <vector>

#include "cataloguedisplaylistwidget.h"

namespace gui_unit_test {
namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }
  static int argc = 3;
  static char app_name[] = "orc-gui-catalogue-vector-test";
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

const orc::CatalogueColour kWhite{255, 255, 255};
const orc::CatalogueColour kRed{255, 0, 0};
const orc::CatalogueColour kBlue{0, 0, 255};

// Every test draws into a square image this many pixels across, over a square
// unit screen, so one unit is this many device pixels on both axes.
constexpr int kSize = 512;

/// A list of |ops| over a square unit screen.
orc::CatalogueDisplayList squareList(std::vector<orc::CatalogueDrawOp> ops) {
  orc::CatalogueDisplayList list;
  list.aspect_height = 1.0;
  list.ops = std::move(ops);
  return list;
}

/// |list| drawn still into a square image |size| pixels across.
QImage render(const orc::CatalogueDisplayList& list, int size = kSize) {
  CatalogueDisplayListWidget widget;
  widget.setDisplayList(list);
  return widget.renderPageImage(QSize(size, size));
}

/// The device pixel a unit point lands in. Unit y runs up; device y down.
QPoint pixelAt(double x, double y, int size = kSize) {
  return QPoint(static_cast<int>(std::floor(x * size)),
                static_cast<int>(std::floor(size - y * size)));
}

QColor colourAt(const QImage& image, double x, double y) {
  return image.pixelColor(pixelAt(x, y, image.width()));
}

bool isBlack(const QColor& colour) {
  return colour.red() < 16 && colour.green() < 16 && colour.blue() < 16;
}

/// The smallest rectangle holding every pixel of |image| inside |within| that
/// is not black.
QRect inkBounds(const QImage& image, const QRect& within) {
  QRect bounds;
  for (int y = within.top(); y <= within.bottom(); ++y) {
    for (int x = within.left(); x <= within.right(); ++x) {
      if (!isBlack(image.pixelColor(x, y))) {
        bounds = bounds.united(QRect(x, y, 1, 1));
      }
    }
  }
  return bounds;
}

/// Lit and unlit runs along row |row| between |from| and |to|, as the x of each
/// change, with a blended pixel counting as neither side.
std::vector<int> transitionsAlongRow(const QImage& image, int row, int from,
                                     int to) {
  std::vector<int> transitions;
  int state = -1;
  for (int x = from; x < to; ++x) {
    const int red = image.pixelColor(x, row).red();
    const int here = red > 200 ? 1 : (red < 55 ? 0 : -1);
    if (here < 0) {
      continue;
    }
    if (state >= 0 && here != state) {
      transitions.push_back(x);
    }
    state = here;
  }
  return transitions;
}

orc::CatalogueDrawOp lineOp(orc::CataloguePoint from, orc::CataloguePoint to,
                            orc::CatalogueSize pel) {
  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kLine;
  op.colour = kWhite;
  op.pen_size = pel;
  op.points = {from, to};
  op.origin = from;
  return op;
}

orc::CatalogueDrawOp rectangleOp(orc::CataloguePoint origin,
                                 orc::CatalogueSize size, bool filled) {
  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kRectangle;
  op.colour = kWhite;
  op.origin = origin;
  op.size = size;
  op.points = {origin};
  op.filled = filled;
  return op;
}

orc::CatalogueDrawOp textOp(const std::string& text, orc::CataloguePoint origin,
                            orc::CatalogueSize field) {
  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kText;
  op.colour = kWhite;
  op.text = text;
  op.origin = origin;
  op.points = {origin};
  op.size = field;
  op.advance = orc::CatalogueSize{std::fabs(field.dx), 0.0};
  op.character_count = 1;
  return op;
}

/// Whether the platform has a face that draws |character|. Text tests need a
/// real glyph to measure; a host with no fonts at all draws every character as
/// the replacement box, which says nothing about how a glyph is fitted.
bool faceHas(char32_t character) {
  return QFontMetricsF(QFontDatabase::systemFont(QFontDatabase::FixedFont))
      .inFontUcs4(character);
}

}  // namespace

// --- The logical pel is a rectangle hung from its corner -------------------

// X3.110 §5.3.2.2.6: the pel is "a rectangle whose orientation is fixed with
// respect to the Cartesian coordinate system", with the drawing point at its
// lower left while both dimensions are positive. A line is that rectangle swept
// along it, so it lies wholly above the line, not straddling it.
TEST(CatalogueDisplayListVectorTest, ThePelHangsFromTheLineByItsCorner) {
  ensureApplication();

  // A pel an eighth as wide as it is tall: 8 by 32 device pixels.
  const QImage upward = render(
      squareList({lineOp({0.1, 0.5}, {0.9, 0.5}, {1.0 / 64, 1.0 / 16})}));
  EXPECT_FALSE(isBlack(upward.pixelColor(256, 240))) << "no stroke above";
  EXPECT_TRUE(isBlack(upward.pixelColor(256, 264)))
      << "the stroke straddled the line instead of hanging above it";
  EXPECT_TRUE(isBlack(upward.pixelColor(256, 219)));

  // A negative height puts the drawing point at the pel's upper left.
  const QImage downward = render(
      squareList({lineOp({0.1, 0.5}, {0.9, 0.5}, {1.0 / 64, -1.0 / 16})}));
  EXPECT_FALSE(isBlack(downward.pixelColor(256, 272)));
  EXPECT_TRUE(isBlack(downward.pixelColor(256, 248)));
}

// The pel keeps its own shape rather than collapsing to a round pen of its
// larger dimension: a vertical line swept by a tall narrow pel is as narrow as
// the pel is, and a negative width hangs it to the left.
TEST(CatalogueDisplayListVectorTest, AStrokeIsAsWideAsThePelAcrossIt) {
  ensureApplication();

  const QImage right = render(
      squareList({lineOp({0.5, 0.1}, {0.5, 0.9}, {1.0 / 64, 1.0 / 16})}));
  int lit = 0;
  for (int x = 0; x < kSize; ++x) {
    if (!isBlack(right.pixelColor(x, 256))) {
      ++lit;
    }
  }
  EXPECT_NEAR(lit, 8, 1) << "a vertical stroke is one pel width across";
  EXPECT_FALSE(isBlack(right.pixelColor(260, 256)));
  EXPECT_TRUE(isBlack(right.pixelColor(250, 256)));

  const QImage left = render(
      squareList({lineOp({0.5, 0.1}, {0.5, 0.9}, {-1.0 / 64, 1.0 / 16})}));
  EXPECT_FALSE(isBlack(left.pixelColor(252, 256)));
  EXPECT_TRUE(isBlack(left.pixelColor(260, 256)));
}

// §5.3.3.1.4: a visible POINT is the pel laid down once. With dy negative the
// drawing point is its upper left, so the point hangs below and to the right.
TEST(CatalogueDisplayListVectorTest, APointIsThePelAtItsCorner) {
  ensureApplication();

  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kPoint;
  op.colour = kWhite;
  op.pen_size = orc::CatalogueSize{1.0 / 32, -1.0 / 32};
  op.points = {orc::CataloguePoint{0.5, 0.5}};
  const QImage image = render(squareList({op}));
  EXPECT_FALSE(isBlack(image.pixelColor(264, 264)));
  EXPECT_TRUE(isBlack(image.pixelColor(264, 248)));
  EXPECT_TRUE(isBlack(image.pixelColor(248, 264)));
}

// --- Outlined rectangles take the line texture ------------------------------

// §5.3.3.4.1: "Outlined rectangles are drawn in the current color(s) and have a
// line width that is determined by the logical pel size and a line texture
// that is specified by the TEXTURE command."
TEST(CatalogueDisplayListVectorTest, AnOutlinedRectangleIsDashed) {
  ensureApplication();

  orc::CatalogueDrawOp op =
      rectangleOp({0.1, 0.1}, {0.8, 0.8}, /*filled=*/false);
  op.pen_size = orc::CatalogueSize{1.0 / 64, 1.0 / 64};
  op.line_style = orc::CatalogueLineStyle::kDashed;
  const QImage image = render(squareList({op}));

  // The bottom edge, 8 pixels high above unit y 0.1, away from the corners.
  const auto transitions =
      transitionsAlongRow(image, 456, static_cast<int>(0.15 * kSize),
                          static_cast<int>(0.85 * kSize));
  ASSERT_GE(transitions.size(), 4u) << "the outline was drawn solid";
  // A dash and the gap after it are each three pels of 8 pixels.
  for (size_t i = 0; i + 1 < transitions.size(); ++i) {
    EXPECT_NEAR(transitions[i + 1] - transitions[i], 24, 2);
  }
}

// --- Arcs of more than three points ------------------------------------------

// §5.3.3.3: a spline's "minimum implementation shall be a series of lines
// connecting the start, intermediate, and end points". A curve fitted through
// them bulges past the points; the series of lines does not.
TEST(CatalogueDisplayListVectorTest, ASplineIsDrawnAsItsSeriesOfLines) {
  ensureApplication();

  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kArc;
  op.colour = kWhite;
  op.pen_size = orc::CatalogueSize{1.0 / 128, 1.0 / 128};
  op.points = {{0.1, 0.2}, {0.3, 0.8}, {0.7, 0.8}, {0.9, 0.2}};
  op.origin = op.points.front();

  const QImage outlined = render(squareList({op}));
  // On the straight line between the two middle points, at unit y 0.8.
  EXPECT_FALSE(isBlack(outlined.pixelColor(256, 100)));
  // Where a curve through the points rises between them.
  EXPECT_TRUE(isBlack(outlined.pixelColor(256, 64)));

  op.filled = true;
  const QImage filled = render(squareList({op}));
  EXPECT_FALSE(isBlack(colourAt(filled, 0.5, 0.5)));
  EXPECT_TRUE(isBlack(colourAt(filled, 0.5, 0.85)))
      << "the filled spline bulged past its points";
}

// --- Text fills its character field ------------------------------------------

// A receiver deposits its character pattern over the whole field (§5.3.2.3.9
// only asks that the character be "completely contained within the area
// defined by the current character field"), so a wide field gets a wide
// character and a tall one a tall one, rather than a glyph of the font's own
// proportions sitting in a corner of it.
TEST(CatalogueDisplayListVectorTest, ACharacterIsStretchedOverItsField) {
  ensureApplication();
  if (!faceHas(U'H')) {
    GTEST_SKIP() << "no platform face draws 'H', so there is no glyph to fit";
  }

  // Eight times as wide as tall: x 51 to 461, y 230 to 282.
  const QImage wide =
      render(squareList({textOp("H", {0.1, 0.45}, {0.8, 0.1})}));
  const QRect wide_ink = inkBounds(wide, QRect(0, 0, kSize, kSize));
  ASSERT_FALSE(wide_ink.isEmpty()) << "nothing was drawn";
  EXPECT_GE(wide_ink.width(), 0.6 * 0.8 * kSize)
      << "the character kept the font's proportions";
  EXPECT_GE(wide_ink.left(), 51);
  EXPECT_LE(wide_ink.right(), 461);

  // Ten times as tall as wide: x 205 to 230, y 154 to 410.
  const QImage tall =
      render(squareList({textOp("H", {0.4, 0.2}, {0.05, 0.5})}));
  const QRect tall_ink = inkBounds(tall, QRect(0, 0, kSize, kSize));
  ASSERT_FALSE(tall_ink.isEmpty()) << "nothing was drawn";
  EXPECT_GE(tall_ink.height(), 0.5 * 0.5 * kSize);
  EXPECT_GE(tall_ink.left(), 204) << "the character spilled out of its field";
  EXPECT_LE(tall_ink.right(), 231) << "the character spilled out of its field";
}

// --- Rotation and reflection, character by character ------------------------

// §5.3.2.3.2: rotation turns "the character field and the cursor ...
// counterclockwise about the character field origin", and §5.3.2.3.3 keeps the
// path "independent of the character rotation". So each character of a run
// turns about its own origin, and the run still steps along its path. The
// fields are drawn in the background colour here so that where they went is
// plain whatever the font.
TEST(CatalogueDisplayListVectorTest, EachCharacterTurnsAboutItsOwnOrigin) {
  ensureApplication();

  orc::CatalogueDrawOp op = textOp("AB", {0.3, 0.3}, {0.1, 0.2});
  op.advance = orc::CatalogueSize{0.2, 0.0};
  op.character_count = 2;
  op.rotation_degrees = 90;
  op.has_background = true;
  op.background = kBlue;
  const QImage image = render(squareList({op}));

  // Turned a quarter anticlockwise, a field 0.1 by 0.2 lies to the left of its
  // origin and 0.1 above it: x 0.1 to 0.3 for the first character, 0.3 to 0.5
  // for the second, and y 0.3 to 0.4 for both.
  EXPECT_FALSE(isBlack(colourAt(image, 0.2, 0.35))) << "first field";
  EXPECT_FALSE(isBlack(colourAt(image, 0.4, 0.35))) << "second field";
  // Not where the fields would be unturned, nor where turning the whole run
  // about its first origin would carry the second character, up the screen.
  EXPECT_TRUE(isBlack(colourAt(image, 0.35, 0.45)));
  EXPECT_TRUE(isBlack(colourAt(image, 0.2, 0.55)));
}

// §5.3.2.3.9: a negative field dimension reflects "the character patterns ...
// about the vertical center axis of the character field" (or the horizontal
// one), and §5.3.2.3.2 keeps the origin at the field's lower left "regardless
// of the sign of the character field dimensions".
TEST(CatalogueDisplayListVectorTest, ANegativeFieldReflectsThePatternInPlace) {
  ensureApplication();

  orc::CatalogueDisplayList list;
  list.aspect_height = 1.0;
  orc::CatalogueBitmap left_column;
  left_column.width = 2;
  left_column.height = 2;
  // Row 0 is the bottom: the left element of both rows lit.
  left_column.elements = {true, false, true, false};
  orc::CatalogueBitmap bottom_row;
  bottom_row.width = 2;
  bottom_row.height = 2;
  bottom_row.elements = {true, true, false, false};
  list.glyphs = {left_column, bottom_row};

  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kGlyph;
  op.colour = kWhite;
  op.origin = orc::CataloguePoint{0.4, 0.4};
  op.points = {op.origin};

  op.glyph_index = 0;
  op.size = orc::CatalogueSize{-0.2, 0.2};
  list.ops = {op};
  const QImage across = render(list);
  EXPECT_FALSE(isBlack(colourAt(across, 0.55, 0.5)))
      << "the pattern was not reflected across the field";
  EXPECT_TRUE(isBlack(colourAt(across, 0.45, 0.5)));
  EXPECT_TRUE(isBlack(colourAt(across, 0.35, 0.5)))
      << "the field was drawn back from its origin";

  op.glyph_index = 1;
  op.size = orc::CatalogueSize{0.2, -0.2};
  list.ops = {op};
  const QImage down = render(list);
  EXPECT_FALSE(isBlack(colourAt(down, 0.5, 0.55)))
      << "the pattern was not reflected down the field";
  EXPECT_TRUE(isBlack(colourAt(down, 0.5, 0.45)));
  EXPECT_TRUE(isBlack(colourAt(down, 0.5, 0.35)))
      << "the field was drawn back from its origin";
}

// A mosaic is a character like any other (§5.3.2.3.2 names "mosaics, and
// separated mosaic characters" among what rotation affects): turned half a
// turn, its top-left element lands at the bottom right of the turned field.
TEST(CatalogueDisplayListVectorTest, AMosaicTurnsWithItsField) {
  ensureApplication();

  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kMosaic;
  op.colour = kWhite;
  op.origin = orc::CataloguePoint{0.5, 0.5};
  op.points = {op.origin};
  op.size = orc::CatalogueSize{0.2, 0.3};
  op.mosaic_pattern = 1;  // top left
  op.rotation_degrees = 180;
  const QImage image = render(squareList({op}));
  EXPECT_FALSE(isBlack(colourAt(image, 0.45, 0.25)));
  EXPECT_TRUE(isBlack(colourAt(image, 0.55, 0.75)))
      << "the mosaic did not turn";
}

// --- Line textures step in whole pels, along curves too ---------------------

// §5.3.2.4.2 measures a texture in pels along the line — pel widths where it
// runs level, pel heights where it runs upright — so a square pel steps
// further along a diagonal than along an axis, and a dashed circle carries
// fewer dashes than its circumference in pels would suggest. Stepping the
// whole curve at one pel of arc would give about fifty dashes here.
TEST(CatalogueDisplayListVectorTest, ADashedCircleStepsInPelsAlongItsCurve) {
  ensureApplication();

  constexpr int kLarge = 1024;
  const double radius = 0.375;
  const double pel = 1.0 / 128;
  orc::CatalogueDrawOp op;
  op.kind = orc::CatalogueDrawKind::kArc;
  op.colour = kWhite;
  op.pen_size = orc::CatalogueSize{pel, pel};
  op.line_style = orc::CatalogueLineStyle::kDashed;
  op.points = {{0.5 + radius, 0.5}, {0.5 - radius, 0.5}, {0.5 + radius, 0.5}};
  op.origin = op.points.front();
  const QImage image = render(squareList({op}), kLarge);

  // Round the circle at the centre of the pel hung from each point of it, which
  // is inside a dash wherever the circle is.
  constexpr int kSamples = 8192;
  const double pi = std::acos(-1.0);
  // Counted where a dash starts, over one sample past the full turn, so the
  // dash the circle starts on is counted once whichever side of the start
  // point it begins.
  int dashes = 0;
  bool previous = false;
  for (int k = 0; k <= kSamples; ++k) {
    const double angle = 2.0 * pi * k / kSamples;
    const double x = 0.5 + radius * std::cos(angle) + pel / 2.0;
    const double y = 0.5 + radius * std::sin(angle) + pel / 2.0;
    const bool lit = image.pixelColor(pixelAt(x, y, kLarge)).red() > 128;
    if (k > 0 && lit && !previous) {
      ++dashes;
    }
    previous = lit;
  }
  // 5.657 pels of travel per pel of radius (8 sin 45°) round the circle, 48
  // pels of radius, six pels a period: 45 and a quarter periods.
  EXPECT_GE(dashes, 44);
  EXPECT_LE(dashes, 47);
}

// --- Colour mode 2 fills what a texture leaves ------------------------------

// §5.3.2.4.4: "In color mode 2, the fill areas not drawn in the drawing color
// are drawn in the background color" — the spaces between hatch lines are not
// holes.
TEST(CatalogueDisplayListVectorTest, HatchGapsTakeTheBackgroundInModeTwo) {
  ensureApplication();

  orc::CatalogueDrawOp op =
      rectangleOp({0.1, 0.1}, {0.8, 0.8}, /*filled=*/true);
  op.fill_pattern = orc::CatalogueFillPattern::kVerticalHatch;
  op.pen_size = orc::CatalogueSize{1.0 / 32, 1.0 / 32};
  op.has_background = true;
  op.background = kBlue;
  const QImage image = render(squareList({op}));

  int drawn = 0;
  int ground = 0;
  for (int x = static_cast<int>(0.15 * kSize); x < 0.85 * kSize; ++x) {
    const QColor colour = image.pixelColor(x, 256);
    EXPECT_GE(colour.blue(), 250) << "a hatch gap left a hole at x=" << x;
    if (colour.red() > 250) {
      ++drawn;
    } else if (colour.red() < 5) {
      ++ground;
    }
  }
  EXPECT_GT(drawn, 0);
  EXPECT_GT(ground, 0) << "the gaps were not drawn in the background";
}

// §5.3.2.4.2: "In color mode 2, the inter-dot and inter-dash spacing is drawn
// in the background color."
TEST(CatalogueDisplayListVectorTest, TextureSpacesTakeTheBackgroundInModeTwo) {
  ensureApplication();

  orc::CatalogueDrawOp op =
      lineOp({0.1, 0.5}, {0.9, 0.5}, {1.0 / 64, 1.0 / 64});
  op.line_style = orc::CatalogueLineStyle::kDotted;
  op.has_background = true;
  op.background = kBlue;
  const QImage image = render(squareList({op}));

  int dots = 0;
  int spaces = 0;
  for (int x = static_cast<int>(0.12 * kSize); x < 0.88 * kSize; ++x) {
    const QColor colour = image.pixelColor(x, 252);
    EXPECT_GE(colour.blue(), 250) << "a space was left empty at x=" << x;
    if (colour.red() > 250) {
      ++dots;
    } else if (colour.red() < 5) {
      ++spaces;
    }
  }
  EXPECT_GT(dots, 0);
  EXPECT_GT(spaces, 0);
}

// The same for a programmable mask's unlit elements.
TEST(CatalogueDisplayListVectorTest, MaskGapsTakeTheBackgroundInModeTwo) {
  ensureApplication();

  orc::CatalogueDrawOp op = rectangleOp({0.0, 0.0}, {1.0, 1.0}, true);
  op.fill_pattern = orc::CatalogueFillPattern::kMask0;
  op.fill_mask_size = orc::CatalogueSize{1.0 / 8, 1.0 / 8};
  op.has_background = true;
  op.background = kBlue;
  orc::CatalogueDisplayList list = squareList({op});
  orc::CatalogueBitmap mask;
  mask.width = 2;
  mask.height = 2;
  mask.elements = {true, false, false, true};
  list.fill_masks.push_back(mask);
  const QImage image = render(list);

  // One tile is 64 pixels and one element 32, from the bottom left.
  EXPECT_EQ(image.pixelColor(16, kSize - 16), QColor(255, 255, 255));
  EXPECT_EQ(image.pixelColor(48, kSize - 16), QColor(0, 0, 255));
}

// --- A programmable mask that was never defined -----------------------------

// §5.3.2.4.4: "The default pattern for the four programmable texture masks is a
// null texture pattern, resulting in no fill in color modes 0 and 1, and a fill
// with the background color in color mode 2."
TEST(CatalogueDisplayListVectorTest, AnUndefinedMaskFillsNothingOrBackground) {
  ensureApplication();

  orc::CatalogueDrawOp op = rectangleOp({0.1, 0.1}, {0.8, 0.8}, true);
  op.fill_pattern = orc::CatalogueFillPattern::kMask0;
  op.fill_mask_size = orc::CatalogueSize{1.0 / 8, 1.0 / 8};
  orc::CatalogueDisplayList list = squareList({op});
  list.fill_masks.push_back(orc::CatalogueBitmap{});  // never defined

  EXPECT_TRUE(isBlack(colourAt(render(list), 0.5, 0.5)))
      << "an undefined mask filled solid";

  // A mask the list carries no slot for at all is no more defined.
  list.ops.front().fill_pattern = orc::CatalogueFillPattern::kMask2;
  EXPECT_TRUE(isBlack(colourAt(render(list), 0.5, 0.5)));

  list.ops.front().fill_pattern = orc::CatalogueFillPattern::kMask0;
  list.ops.front().has_background = true;
  list.ops.front().background = kBlue;
  EXPECT_EQ(colourAt(render(list), 0.5, 0.5), QColor(0, 0, 255));
}

// --- Reverse video
// -------------------------------------------------------------

namespace {

/// A red block under the field of a reverse-video character, so that whatever
/// the character leaves undrawn shows up as red rather than as black.
orc::CatalogueDrawOp groundOp() {
  orc::CatalogueDrawOp ground = rectangleOp({0.2, 0.4}, {0.6, 0.2}, true);
  ground.colour = kRed;
  return ground;
}

}  // namespace

// §6.2.7.4: in reverse video "the pixels surrounding the character shape in the
// character field are drawn in the drawing color. The pixels of the character
// shape are not drawn" — so in colour modes 0 and 1 what is beneath shows
// through the character, rather than the character being painted black.
TEST(CatalogueDisplayListVectorTest, ReverseVideoLeavesTheCharacterUndrawn) {
  ensureApplication();

  orc::CatalogueDrawOp character = textOp("H", {0.3, 0.45}, {0.4, 0.1});
  character.reverse_video = true;
  const QImage image = render(squareList({groundOp(), character}));

  // Inside the field, a pixel or two clear of its edge.
  const QRect field =
      QRect(pixelAt(0.3, 0.55), pixelAt(0.7, 0.45)).adjusted(2, 2, -2, -2);
  int ground_showing = 0;
  int blackened = 0;
  for (int y = field.top(); y <= field.bottom(); ++y) {
    for (int x = field.left(); x <= field.right(); ++x) {
      const QColor colour = image.pixelColor(x, y);
      if (colour.red() < 128) {
        ++blackened;
      } else if (colour.green() < 128) {
        ++ground_showing;
      }
    }
  }
  EXPECT_EQ(blackened, 0) << "the character was painted black";
  EXPECT_GT(ground_showing, 0) << "nothing beneath shows through the character";
}

// In colour mode 2 the shape is drawn in the background colour instead.
TEST(CatalogueDisplayListVectorTest, ReverseVideoInModeTwoDrawsTheBackground) {
  ensureApplication();

  orc::CatalogueDrawOp character = textOp("H", {0.3, 0.45}, {0.4, 0.1});
  character.reverse_video = true;
  character.has_background = true;
  character.background = kBlue;
  const QImage image = render(squareList({groundOp(), character}));

  const QRect field =
      QRect(pixelAt(0.3, 0.55), pixelAt(0.7, 0.45)).adjusted(2, 2, -2, -2);
  int background = 0;
  int ground = 0;
  for (int y = field.top(); y <= field.bottom(); ++y) {
    for (int x = field.left(); x <= field.right(); ++x) {
      const QColor colour = image.pixelColor(x, y);
      if (colour.red() < 128 && colour.blue() > 128) {
        ++background;
      } else if (colour.red() > 128 && colour.green() < 128 &&
                 colour.blue() < 128) {
        ++ground;
      }
    }
  }
  EXPECT_GT(background, 0);
  EXPECT_EQ(ground, 0) << "mode 2 let what was beneath show through";
}

// Mosaics are drawn the same way (§6.2.7.4 covers "alphanumeric text, mosaic,
// and DRCS characters"): the lit element shows the ground, the rest of the
// field the drawing colour.
TEST(CatalogueDisplayListVectorTest,
     AReverseVideoMosaicShowsTheGroundInItsShape) {
  ensureApplication();

  orc::CatalogueDrawOp mosaic;
  mosaic.kind = orc::CatalogueDrawKind::kMosaic;
  mosaic.colour = kWhite;
  mosaic.origin = orc::CataloguePoint{0.3, 0.4};
  mosaic.points = {mosaic.origin};
  mosaic.size = orc::CatalogueSize{0.2, 0.18};
  mosaic.mosaic_pattern = 1;  // top left
  mosaic.reverse_video = true;
  const QImage image = render(squareList({groundOp(), mosaic}));

  // The top-left element is x 0.3 to 0.4, y 0.52 to 0.58.
  EXPECT_EQ(colourAt(image, 0.35, 0.55), QColor(255, 0, 0));
  EXPECT_EQ(colourAt(image, 0.45, 0.45), QColor(255, 255, 255));
}

// --- Characters no face has
// ----------------------------------------------------

// §8.3.3(3): a character must be presented "exactly or in an approximate form
// or as a symbol(s) that cannot be confused with any character in the graphic
// character repertoire" — never as a blank. U+0378 is unassigned, so no face
// has it; it is drawn as a hollow box stretched over its field.
TEST(CatalogueDisplayListVectorTest, ACharacterNoFaceHasIsABox) {
  ensureApplication();

  const QImage image =
      render(squareList({textOp("\xCD\xB8", {0.1, 0.4}, {0.8, 0.2})}));
  const QRect field(pixelAt(0.1, 0.6), pixelAt(0.9, 0.4));
  const QRect ink = inkBounds(image, field);
  ASSERT_FALSE(ink.isEmpty()) << "the character was left blank";
  EXPECT_GE(ink.width(), field.width() / 2);
  EXPECT_GE(ink.height(), field.height() / 2);
  EXPECT_TRUE(isBlack(colourAt(image, 0.5, 0.5))) << "the box is not hollow";
}

// --- Raster colour runs
// -----------------------------------------------------------

// §5.3.3.6.3 lays INCREMENTAL POINT colours raster-sequentially across the
// field, one pel apiece; the list carries one row of them per operation. A pel
// finer than one pixel of the page's grid is drawn a grid pixel wide so that it
// is seen, but the row keeps its own step, so all of its colours stay on it.
TEST(CatalogueDisplayListVectorTest, AColourRunStaysOnItsRow) {
  ensureApplication();

  orc::CatalogueDisplayList list;
  // A 64 by 48 grid of square pixels, 8 device pixels each at 512 by 384.
  list.aspect_height = 0.75;
  list.nominal_width = 64;
  list.nominal_height = 48;
  orc::CatalogueDrawOp run;
  run.kind = orc::CatalogueDrawKind::kColourRun;
  // A pel half a grid pixel across: 4 device pixels.
  const double pel = 1.0 / 128;
  run.pen_size = orc::CatalogueSize{pel, pel};
  run.origin = orc::CataloguePoint{0.25, 0.375};
  run.points = {run.origin};
  run.size = orc::CatalogueSize{8 * pel, pel};
  for (int i = 0; i < 8; ++i) {
    run.colour_run.push_back(
        orc::CatalogueColour{static_cast<uint8_t>(30 * (i + 1)),
                             static_cast<uint8_t>(255 - 30 * i), 0});
  }
  list.ops.push_back(run);

  CatalogueDisplayListWidget widget;
  widget.setDisplayList(list);
  const QImage image = widget.renderPageImage(QSize(512, 384));
  ASSERT_FALSE(image.isNull());

  // The row's top is unit y 0.375 + 1/128, which is device y 188.
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(image.pixelColor(128 + 4 * i + 2, 190),
              QColor(30 * (i + 1), 255 - 30 * i, 0))
        << "colour " << i << " is not where its step puts it on the row";
  }
  EXPECT_TRUE(isBlack(image.pixelColor(130, 200))) << "the row wrapped";
}

// --- Pixel pages
// ---------------------------------------------------------------------

// A pixel page's rectangles are the receiver's own pixels, so each is drawn
// exactly as large as it says — nothing traced round it spills onto its
// neighbours.
TEST(CatalogueDisplayListVectorTest, APixelRunIsExactlyItsOwnPixels) {
  ensureApplication();

  orc::CatalogueDisplayList list;
  list.aspect_height = 1.0;
  list.nominal_width = 4;
  list.nominal_height = 4;
  list.pixel_aligned = true;
  list.ops.push_back(rectangleOp({0.25, 0.25}, {0.25, 0.25}, true));
  CatalogueDisplayListWidget widget;
  widget.setDisplayList(list);
  const QImage image = widget.renderPageImage(QSize(400, 400));
  ASSERT_FALSE(image.isNull());

  // Grid column 1 and row 1 from the bottom: x 100 to 199, y 200 to 299.
  EXPECT_EQ(image.pixelColor(100, 250), QColor(Qt::white));
  EXPECT_EQ(image.pixelColor(199, 250), QColor(Qt::white));
  EXPECT_EQ(image.pixelColor(150, 200), QColor(Qt::white));
  EXPECT_EQ(image.pixelColor(150, 299), QColor(Qt::white));
  EXPECT_EQ(image.pixelColor(99, 250), QColor(Qt::black));
  EXPECT_EQ(image.pixelColor(200, 250), QColor(Qt::black));
  EXPECT_EQ(image.pixelColor(150, 199), QColor(Qt::black));
  EXPECT_EQ(image.pixelColor(150, 300), QColor(Qt::black));
}

// --- A receiver's page: the pel on the receiver's grid -----------------------

namespace {

// The reference receiver of Table D1 item 10: 256 by 200 pixels over unit y 0
// to 0.78125, shown at 4:3 — a pixel is 1/256 of unit space either way.
constexpr double kReceiverPixel = 1.0 / 256.0;

/// A list of |ops| drawn for the reference receiver, as the NABTS sink's
/// vector mode emits it.
orc::CatalogueDisplayList receiverPage(std::vector<orc::CatalogueDrawOp> ops) {
  orc::CatalogueDisplayList list;
  list.aspect_height = 0.78125;
  list.display_aspect_height = 0.75;
  list.nominal_width = 256;
  list.nominal_height = 200;
  list.ops = std::move(ops);
  return list;
}

/// |list| drawn still at eight device pixels a receiver pixel across (7.68
/// down), fine enough that an edge on the receiver's grid is never near the
/// middle of one of its pixels.
QImage renderReceiverPage(const orc::CatalogueDisplayList& list,
                          QSize size = QSize(2048, 1536)) {
  CatalogueDisplayListWidget widget;
  widget.setDisplayList(list);
  return widget.renderPageImage(size);
}

/// The colour at the middle of receiver pixel (|column|, |row|), rows counted
/// up from the foot of the display area.
QColor receiverPixel(const QImage& image, int column, int row) {
  const double across = image.width() / 256.0;
  const double down = image.height() / 200.0;
  return image.pixelColor(
      static_cast<int>(std::floor((column + 0.5) * across)),
      static_cast<int>(std::floor(image.height() - (row + 0.5) * down)));
}

bool litAt(const QImage& image, int column, int row) {
  return receiverPixel(image, column, row).red() > 160;
}

bool darkAt(const QImage& image, int column, int row) {
  return receiverPixel(image, column, row).red() < 96;
}

/// A line from receiver pixel corner (|x0|, |y0|) to (|x1|, |y1|).
orc::CatalogueDrawOp receiverLine(double x0, double y0, double x1, double y1,
                                  orc::CatalogueSize pel = {0.0, 0.0}) {
  return lineOp({x0 * kReceiverPixel, y0 * kReceiverPixel},
                {x1 * kReceiverPixel, y1 * kReceiverPixel}, pel);
}

}  // namespace

// §5.3.2.2.6: a dimensionless pel draws "the physical picture elements
// (pixels) through which the infinitely small locus point passes", and
// Appendix B maps a point to the pixel it falls in by truncation: a line on
// the grid lights the column or row its coordinate names, one pixel wide —
// not a pixel's width centred on the line, which straddled two pixels and
// left a line along the right or top edge of the screen half off it. Both
// receivers of the day, driven through their firmware with these lines, light
// exactly these pixels.
TEST(CatalogueDisplayListVectorTest, ADimensionlessStrokeLightsThePixelItIsIn) {
  ensureApplication();

  const QImage image = renderReceiverPage(receiverPage(
      {receiverLine(17, 20, 17, 146), receiverLine(40, 27, 160, 27),
       receiverLine(0, 10, 0, 190), receiverLine(255, 10, 255, 190),
       receiverLine(10, 0, 245, 0), receiverLine(10, 199, 245, 199)}));
  ASSERT_FALSE(image.isNull());

  EXPECT_TRUE(litAt(image, 17, 80));
  EXPECT_TRUE(darkAt(image, 16, 80));
  EXPECT_TRUE(darkAt(image, 18, 80));
  EXPECT_TRUE(litAt(image, 100, 27));
  EXPECT_TRUE(darkAt(image, 100, 26));
  EXPECT_TRUE(darkAt(image, 100, 28));
  EXPECT_TRUE(litAt(image, 0, 100)) << "the left edge";
  EXPECT_TRUE(litAt(image, 255, 100)) << "the right edge";
  EXPECT_TRUE(litAt(image, 100, 0)) << "the foot";
  EXPECT_TRUE(litAt(image, 100, 199)) << "the top";
}

// One pixel of the receiver's grid, whatever size the page is drawn at.
TEST(CatalogueDisplayListVectorTest, ADimensionlessStrokeIsOneReceiverPixel) {
  ensureApplication();

  const orc::CatalogueDisplayList list =
      receiverPage({receiverLine(17, 20, 17, 146)});
  for (const int across : {768, 2048}) {
    const QImage image =
        renderReceiverPage(list, QSize(across, across * 3 / 4));
    ASSERT_FALSE(image.isNull());
    const int row = image.height() / 2;
    int lit = 0;
    for (int x = 0; x < image.width(); ++x) {
      if (image.pixelColor(x, row).red() > 160) {
        ++lit;
      }
    }
    EXPECT_EQ(lit, across / 256) << "drawn " << across << " across";
  }
}

// §5.3.3.4.1 fills a rectangle with "the region of the outline traced by the
// logical pel" in it, so the pel at the far corner takes in the pixel the far
// edge falls in: 60 by 40 from (40, 30) is columns 40 to 100 and rows 30 to 70,
// on both receivers of the day — and so is the same rectangle drawn back from
// (100, 100).
TEST(CatalogueDisplayListVectorTest, AFilledRectangleTakesInItsFarEdgesPixels) {
  ensureApplication();

  orc::CatalogueDrawOp forward =
      rectangleOp({40 * kReceiverPixel, 30 * kReceiverPixel},
                  {60 * kReceiverPixel, 40 * kReceiverPixel}, true);
  const QImage image = renderReceiverPage(receiverPage({forward}));
  ASSERT_FALSE(image.isNull());
  EXPECT_TRUE(litAt(image, 40, 50));
  EXPECT_TRUE(litAt(image, 100, 50));
  EXPECT_TRUE(darkAt(image, 39, 50));
  EXPECT_TRUE(darkAt(image, 101, 50));
  EXPECT_TRUE(litAt(image, 70, 30));
  EXPECT_TRUE(litAt(image, 70, 70));
  EXPECT_TRUE(darkAt(image, 70, 29));
  EXPECT_TRUE(darkAt(image, 70, 71));

  orc::CatalogueDrawOp back =
      rectangleOp({100 * kReceiverPixel, 100 * kReceiverPixel},
                  {-60 * kReceiverPixel, -40 * kReceiverPixel}, true);
  const QImage drawn_back = renderReceiverPage(receiverPage({back}));
  EXPECT_TRUE(litAt(drawn_back, 40, 80));
  EXPECT_TRUE(litAt(drawn_back, 100, 80));
  EXPECT_TRUE(darkAt(drawn_back, 101, 80));
  EXPECT_TRUE(litAt(drawn_back, 70, 60));
  EXPECT_TRUE(litAt(drawn_back, 70, 100));
  EXPECT_TRUE(darkAt(drawn_back, 70, 101));
}

// A negative pel is laid back from the pixel the drawing point is in, as both
// receivers lay it: index pages can rule their stripes with a pel one pixel
// wide and minus one high, and each stripe sits on the row its
// coordinate names — §5.3.2.2.6's corner alone would put it on the row below.
TEST(CatalogueDisplayListVectorTest, ANegativePelIsLaidFromItsOwnPixel) {
  ensureApplication();

  const QImage image = renderReceiverPage(receiverPage(
      {receiverLine(10, 100, 240, 100, {kReceiverPixel, -kReceiverPixel}),
       receiverLine(100, 120, 100, 190, {-kReceiverPixel, kReceiverPixel})}));
  ASSERT_FALSE(image.isNull());
  EXPECT_TRUE(litAt(image, 60, 100));
  EXPECT_TRUE(darkAt(image, 60, 99));
  EXPECT_TRUE(darkAt(image, 60, 101));
  EXPECT_TRUE(litAt(image, 100, 150));
  EXPECT_TRUE(darkAt(image, 99, 150));
  EXPECT_TRUE(darkAt(image, 101, 150));
}

// A receiver's "straight line algorithm" (§5.3.2.2.6) puts the pel down once a
// step along the axis a line moves faster on, so a shallow line of a
// one-pixel pel lights one pixel in every column it crosses, as a level line
// does — a pel swept corner to corner would light two in some.
TEST(CatalogueDisplayListVectorTest, AShallowLineIsOnePixelAColumn) {
  ensureApplication();

  // A slope of a third, so the line is never half way between two rows at a
  // column's middle and each column has one pixel to light.
  const QImage image =
      renderReceiverPage(receiverPage({receiverLine(20, 20, 110, 50)}));
  ASSERT_FALSE(image.isNull());
  for (int column = 21; column < 110; ++column) {
    int lit = 0;
    for (int row = 10; row < 60; ++row) {
      lit += litAt(image, column, row) ? 1 : 0;
    }
    EXPECT_EQ(lit, 1) << "column " << column;
  }
}

// Appendix B truncates a point to the pixel it falls in, so a list that names
// its receiver's grid plots each point of a point, line, rectangle or polygon
// at the corner of that pixel: a line at x = 17.6 pixels is column 17's, and a
// rectangle from (40.6, 30.3) to (100.4, 70.7) covers the pixels from (40, 30)
// to (100, 70), as the receivers draw them.
TEST(CatalogueDisplayListVectorTest, APointIsPlottedInThePixelItFallsIn) {
  ensureApplication();

  orc::CatalogueDrawOp point;
  point.kind = orc::CatalogueDrawKind::kPoint;
  point.colour = kWhite;
  point.points = {{60.7 * kReceiverPixel, 160.2 * kReceiverPixel}};
  orc::CatalogueDrawOp rectangle =
      rectangleOp({40.6 * kReceiverPixel, 30.3 * kReceiverPixel},
                  {59.8 * kReceiverPixel, 40.4 * kReceiverPixel}, true);
  const QImage image = renderReceiverPage(
      receiverPage({receiverLine(17.6, 100, 17.6, 140), point, rectangle}));
  ASSERT_FALSE(image.isNull());

  EXPECT_TRUE(litAt(image, 17, 120));
  EXPECT_TRUE(darkAt(image, 18, 120));
  EXPECT_TRUE(litAt(image, 60, 160));
  EXPECT_TRUE(darkAt(image, 61, 160));
  EXPECT_TRUE(darkAt(image, 60, 161));
  EXPECT_TRUE(litAt(image, 40, 30));
  EXPECT_TRUE(litAt(image, 100, 70));
  EXPECT_TRUE(darkAt(image, 101, 50));
  EXPECT_TRUE(darkAt(image, 70, 71));
}

// With a dimensionless pel "the separation of separated mosaics is the
// minimal physical separation which may be presented; that is, a separation
// of one physical pixel" (T.101 Annex D Addendum §6): separated mosaics never
// look contiguous (Table D1 item 6). In a 6 by 9 field the elements are three
// pixels square and lose a pixel from their right and their top.
TEST(CatalogueDisplayListVectorTest, SeparatedMosaicsAreAPixelApartAtLeast) {
  ensureApplication();

  orc::CatalogueDrawOp mosaic;
  mosaic.kind = orc::CatalogueDrawKind::kMosaic;
  mosaic.colour = kWhite;
  mosaic.origin = orc::CataloguePoint{60 * kReceiverPixel, 60 * kReceiverPixel};
  mosaic.points = {mosaic.origin};
  mosaic.size = orc::CatalogueSize{6 * kReceiverPixel, 9 * kReceiverPixel};
  mosaic.mosaic_pattern = 0x3F;
  mosaic.mosaic_separated = true;
  const QImage image = renderReceiverPage(receiverPage({mosaic}));
  ASSERT_FALSE(image.isNull());

  // The bottom pair of elements: columns 60-61 and 63-64, rows 60-61.
  EXPECT_TRUE(litAt(image, 61, 61));
  EXPECT_TRUE(darkAt(image, 62, 61)) << "no gap between the elements";
  EXPECT_TRUE(litAt(image, 63, 61));
  EXPECT_TRUE(darkAt(image, 61, 62)) << "no gap above the element";
  EXPECT_TRUE(litAt(image, 61, 63));
}

// A dotted straight line takes its pattern from the screen, as the receivers of
// the day laid it and the pixel modes draw it: the Norpak TTX 650 lights a
// pixel of the line where its column — its row, on a line steeper than 45
// degrees — falls on the pattern, one pixel in every two pels. Lines started a
// pixel apart light the same columns and rows, and the start that falls in a
// gap of the pattern is drawn all the same (§5.3.2.4.2). A pattern counted
// from each line's start would put the dots of the second line between the
// first's.
TEST(CatalogueDisplayListVectorTest, ADottedLineIsRegisteredToTheScreen) {
  ensureApplication();

  const orc::CatalogueSize pel{kReceiverPixel, kReceiverPixel};
  orc::CatalogueDrawOp even = receiverLine(10, 100, 40, 100, pel);
  orc::CatalogueDrawOp odd = receiverLine(11, 110, 40, 110, pel);
  orc::CatalogueDrawOp upright = receiverLine(70, 21, 70, 51, pel);
  for (orc::CatalogueDrawOp* op : {&even, &odd, &upright}) {
    op->line_style = orc::CatalogueLineStyle::kDotted;
  }
  const QImage image = renderReceiverPage(receiverPage({even, odd, upright}));
  ASSERT_FALSE(image.isNull());

  for (int column = 12; column < 40; column += 2) {
    EXPECT_TRUE(litAt(image, column, 100)) << "column " << column;
    EXPECT_TRUE(darkAt(image, column + 1, 100)) << "column " << column + 1;
    EXPECT_TRUE(litAt(image, column, 110)) << "column " << column;
    EXPECT_TRUE(darkAt(image, column + 1, 110)) << "column " << column + 1;
  }
  EXPECT_TRUE(litAt(image, 11, 110)) << "the start, in a gap of the pattern";
  for (int row = 22; row < 50; row += 2) {
    EXPECT_TRUE(litAt(image, 70, row)) << "row " << row;
    EXPECT_TRUE(darkAt(image, 70, row + 1)) << "row " << row + 1;
  }
  EXPECT_TRUE(litAt(image, 70, 21)) << "the start, in a gap of the pattern";
  EXPECT_TRUE(litAt(image, 70, 51)) << "the end, in a gap of the pattern";
}

}  // namespace gui_unit_test
