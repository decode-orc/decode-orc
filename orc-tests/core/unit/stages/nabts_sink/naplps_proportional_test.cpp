/*
 * File:        naplps_proportional_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for proportional spacing: T.101 Annex D's width
 * classes and displacement function
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "naplps_proportional.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace orc {
namespace {

/// A displacement in reference pixels, 256 to the unit screen.
double pixels(double unit) { return unit * 256.0; }

// Clause 11.1, Tables 1 and 2: SPACE and the widest letters are class 9, the
// narrowest punctuation and stems class 0, and a code no table gives a class
// is taken as full width.
TEST(NaplpsProportional, WidthClassesFollowTheTables) {
  EXPECT_EQ(naplps_width_class(' ', false), 9);
  EXPECT_EQ(naplps_width_class('M', false), 9);
  EXPECT_EQ(naplps_width_class('W', false), 9);
  EXPECT_EQ(naplps_width_class('m', false), 9);
  EXPECT_EQ(naplps_width_class('i', false), 0);
  EXPECT_EQ(naplps_width_class('l', false), 0);
  EXPECT_EQ(naplps_width_class('.', false), 0);
  EXPECT_EQ(naplps_width_class(',', false), 3);
  EXPECT_EQ(naplps_width_class('1', false), 1);
  EXPECT_EQ(naplps_width_class('a', false), 5);
  EXPECT_EQ(naplps_width_class('t', false), 2);
  EXPECT_EQ(naplps_width_class(0x7F, false), 9);
  // The supplementary set: the grave and circumflex are class 2, and the
  // non-spacing underline makes whatever it is put on full width.
  EXPECT_EQ(naplps_width_class(0x41, true), 2);
  EXPECT_EQ(naplps_width_class(0x43, true), 2);
  EXPECT_EQ(naplps_width_class(0x4C, true), 9);
}

// Clause 11.5: below 12/256 the width picks one of six tables by its whole
// part, scaled by a power of two into 6 to 11. The default field, 6.4 pixels,
// is size 6: an i moves 2 pixels, an a 5 and an M 6 — the field's width,
// truncated to the spacing unit.
TEST(NaplpsProportional, NarrowFieldsUseTheFontSpacingTables) {
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(0, 6.4 / 256)), 2);
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(5, 6.4 / 256)), 5);
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(9, 6.4 / 256)), 6);
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(9, 8.0 / 256)), 8);
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(5, 8.0 / 256)), 6);
  // Half the default width takes the same table at half the scale.
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(9, 3.2 / 256)), 3);
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(0, 3.2 / 256)), 1);
  // Size 11's two entries the printed table gives as 0, read as 10.
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(6, 11.0 / 256)), 10);
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(8, 11.0 / 256)), 10);
  EXPECT_DOUBLE_EQ(naplps_proportional_displacement(9, 0.0), 0.0);
}

// Clause 11.6: from 12/256 the widest class moves the width truncated to whole
// units, N, and each narrower class less by its table units times an even
// scale factor F over 6, rounded: at N = 20, F is 14, so class 0 moves 20 - 14
// and class 5 moves 20 - 5.
TEST(NaplpsProportional, WideFieldsScaleTheSecondTable) {
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(9, 20.0 / 256)), 20);
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(0, 20.0 / 256)), 6);
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(5, 20.0 / 256)), 15);
  // N = 12: F is 8.
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(9, 12.0 / 256)), 12);
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(0, 12.0 / 256)), 4);
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(5, 12.0 / 256)), 9);
  // Clause 11.3 (a): the widest class is the width truncated to the unit.
  EXPECT_DOUBLE_EQ(pixels(naplps_proportional_displacement(9, 13.7 / 256)), 13);
}

// Clause 11.3 (b): the gap between letters is the same all along a line, so a
// narrower class never moves further than a wider one at any size.
TEST(NaplpsProportional, ANarrowerClassNeverMovesFurther) {
  const int order[10] = {0, 1, 3, 2, 4, 7, 5, 8, 6, 9};
  for (const double width : {3.2, 6.0, 6.4, 7.0, 9.6, 11.0, 12.8, 19.2, 64.0}) {
    double previous = 0.0;
    for (const int width_class : order) {
      const double moved =
          naplps_proportional_displacement(width_class, width / 256.0);
      EXPECT_GE(moved, previous) << width << " class " << width_class;
      previous = moved;
    }
  }
}

}  // namespace
}  // namespace orc
