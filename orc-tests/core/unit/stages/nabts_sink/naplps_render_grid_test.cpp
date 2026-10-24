/*
 * File:        naplps_render_grid_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for the NAPLPS receiver render-grid presets
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "naplps_render_grid.h"

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace orc {
namespace {

// Table D1 item 10: "Resolution shall be on the order of 256 pixels horizontal
// by 200 pixels vertical". The default mode is that receiver, because it is the
// one the standard's service reference model requires and so the one a decoder
// of the period presented.
TEST(NaplpsRenderGridTest, TheDefaultGridIsTheServiceReferenceModelResolution) {
  const NaplpsRenderGrid grid =
      naplps_render_grid(NaplpsRenderMode::kReference);
  EXPECT_EQ(grid.width, 256);
  EXPECT_EQ(grid.height, 200);
}

// Every grid covers the same unit-screen area, so their pixels stay square in
// unit space: Table D1 item 10 makes y 0 to 0.78125 visible, and 0.78125 * 256
// is 200 exactly. A grid whose pixel was not square would distort a circle the
// source drew as a circle.
TEST(NaplpsRenderGridTest, EveryGridHasASquarePixelInUnitSpace) {
  for (const NaplpsRenderMode mode :
       {NaplpsRenderMode::kReference, NaplpsRenderMode::kThrice,
        NaplpsRenderMode::kTwice, NaplpsRenderMode::kTwiceVector}) {
    const NaplpsRenderGrid grid = naplps_render_grid(mode);
    EXPECT_NEAR(grid.pitch_x(), grid.pitch_y(), 1e-12)
        << "mode " << naplps_render_mode_name(mode);
  }
}

// Every grid is a *whole* multiple of the reference resolution. A page is
// authored against that resolution, so a fraction of it puts what the author
// placed on a pixel boundary between two, which thickens strokes unevenly and
// — since a character pattern is a bitmap of a fixed cell — breaks letterforms.
TEST(NaplpsRenderGridTest, TheGridsAreWholeMultiplesOfTheReference) {
  const NaplpsRenderGrid reference =
      naplps_render_grid(NaplpsRenderMode::kReference);

  for (const auto& [mode, multiple] :
       std::vector<std::pair<NaplpsRenderMode, int>>{
           {NaplpsRenderMode::kReference, 1},
           {NaplpsRenderMode::kTwice, 2},
           {NaplpsRenderMode::kThrice, 3},
           {NaplpsRenderMode::kTwiceVector, 1}}) {
    const NaplpsRenderGrid grid = naplps_render_grid(mode);
    EXPECT_EQ(grid.width, reference.width * multiple)
        << "mode " << naplps_render_mode_name(mode);
    EXPECT_EQ(grid.height, reference.height * multiple)
        << "mode " << naplps_render_mode_name(mode);
  }
}

// The 6 by 10 character cell of Appendix B's readability example lands on whole
// pixels of every grid, which is the property a whole multiple buys and the one
// a bitmap character pattern needs: scaled by anything else it duplicates some
// rows and columns and not others, and the letterform comes apart.
TEST(NaplpsRenderGridTest, ACharacterCellLandsOnWholePixelsOfEveryGrid) {
  const double field_dx = 6.0 / 256.0;
  const double field_dy = 10.0 / 200.0 * kNabtsDisplayAreaHeight;

  for (const NaplpsRenderMode mode :
       {NaplpsRenderMode::kReference, NaplpsRenderMode::kTwice,
        NaplpsRenderMode::kThrice}) {
    const NaplpsRenderGrid grid = naplps_render_grid(mode);
    const double columns = field_dx / grid.pitch_x();
    const double rows = field_dy / grid.pitch_y();
    EXPECT_NEAR(columns, std::round(columns), 1e-9)
        << "mode " << naplps_render_mode_name(mode) << " gives " << columns
        << " columns to a character cell";
    EXPECT_NEAR(rows, std::round(rows), 1e-9)
        << "mode " << naplps_render_mode_name(mode) << " gives " << rows
        << " rows to a character cell";
  }
}

// One pixel of the reference grid is 1/256 of the unit screen across, which is
// the width a stroke of the default dimensionless pel (§5.3.2.2.6) covers.
TEST(NaplpsRenderGridTest, ThePitchIsOnePixelOfTheUnitScreen) {
  const NaplpsRenderGrid grid =
      naplps_render_grid(NaplpsRenderMode::kReference);
  EXPECT_NEAR(grid.pitch_x(), 1.0 / 256.0, 1e-12);
  EXPECT_NEAR(grid.pitch_y(), 0.78125 / 200.0, 1e-12);
}

// The vector mode is the reference receiver's page drawn as shapes, so a
// renderer floors a stroke at, and plots a point in, that receiver's pixel
// (§5.3.2.2.6, Appendix B) — which is where and how heavy both receivers of the
// day drew them, driven through their firmware. Resolved against the
// twice-reference grid, as it once was, a dimensionless line was half their
// width and the edge of every filled figure a pixel short of theirs.
TEST(NaplpsRenderGridTest, TheVectorModeSharesTheReferenceGrid) {
  EXPECT_EQ(naplps_render_grid(NaplpsRenderMode::kTwiceVector).width,
            naplps_render_grid(NaplpsRenderMode::kReference).width);
  EXPECT_EQ(naplps_render_grid(NaplpsRenderMode::kTwiceVector).height,
            naplps_render_grid(NaplpsRenderMode::kReference).height);

  EXPECT_FALSE(naplps_mode_emits_pixels(NaplpsRenderMode::kTwiceVector));
  EXPECT_TRUE(naplps_mode_emits_pixels(NaplpsRenderMode::kReference));
  EXPECT_TRUE(naplps_mode_emits_pixels(NaplpsRenderMode::kThrice));
  EXPECT_TRUE(naplps_mode_emits_pixels(NaplpsRenderMode::kTwice));
}

// The mode is stored in the project by name, so a name has to survive the round
// trip that saving and reloading a project makes it take.
TEST(NaplpsRenderGridTest, ModeNamesRoundTrip) {
  for (const NaplpsRenderMode mode :
       {NaplpsRenderMode::kReference, NaplpsRenderMode::kThrice,
        NaplpsRenderMode::kTwice, NaplpsRenderMode::kTwiceVector}) {
    EXPECT_EQ(naplps_render_mode_from_name(naplps_render_mode_name(mode)),
              mode);
  }
}

// A name from a project written by some other version is not worth refusing a
// decode over: the page still renders, at the resolution asked for instead.
TEST(NaplpsRenderGridTest, AnUnknownNameFallsBackRatherThanFailing) {
  EXPECT_EQ(naplps_render_mode_from_name("1080p"),
            NaplpsRenderMode::kReference);
  EXPECT_EQ(naplps_render_mode_from_name("", NaplpsRenderMode::kTwice),
            NaplpsRenderMode::kTwice);

  // Including the line-count shorthand these were once named after, which no
  // longer selects anything: a project carrying one gets the reference
  // receiver, which is the default it would have got anyway.
  for (const char* legacy : {"240p", "320p", "480p", "480p (vector)"}) {
    EXPECT_EQ(naplps_render_mode_from_name(legacy),
              NaplpsRenderMode::kReference)
        << legacy;
  }
}

// The vector mode is named for the grid it is resolved against, which is now
// the reference one; the name it had while it was resolved against the
// twice-reference grid still selects it rather than another receiver.
TEST(NaplpsRenderGridTest, TheVectorModesEarlierNameStillSelectsIt) {
  EXPECT_EQ(naplps_render_mode_name(NaplpsRenderMode::kTwiceVector),
            "256 x 200 (vector)");
  EXPECT_EQ(naplps_render_mode_from_name("512 x 400 (vector)"),
            NaplpsRenderMode::kTwiceVector);
}

}  // namespace
}  // namespace orc
