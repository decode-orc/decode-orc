/*
 * File:        naplps_proportional.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     How far proportionally spaced text moves the cursor
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "naplps_proportional.h"

#include <algorithm>
#include <cmath>

namespace orc {

namespace {

/// Table 1/T.101 (clause 11.1): the primary set's width classes, by row 0 to
/// 15 and column 2 to 7. 7/15 is not a character.
constexpr int kPrimaryClass[16][6] = {
    {9, 5, 9, 5, 1, 5}, {0, 1, 5, 6, 5, 5}, {4, 5, 5, 5, 5, 5},
    {6, 5, 5, 5, 5, 5}, {9, 5, 5, 9, 5, 2}, {9, 5, 5, 5, 5, 5},
    {9, 5, 5, 9, 5, 9}, {0, 5, 8, 9, 5, 9}, {1, 5, 5, 9, 5, 9},
    {1, 5, 2, 9, 0, 5}, {9, 0, 5, 9, 4, 5}, {9, 3, 5, 4, 5, 5},
    {3, 5, 5, 9, 0, 0}, {5, 8, 9, 4, 9, 5}, {0, 5, 5, 2, 5, 9},
    {9, 8, 9, 9, 5, -1}};

/// Table 2/T.101: the supplementary set's, laid out the same way.
constexpr int kSupplementaryClass[16][6] = {
    {9, 4, 9, 5, 6, 9}, {0, 9, 2, 1, 9, 9}, {9, 4, 2, 9, 6, 6},
    {5, 4, 2, 9, 5, 5}, {9, 7, 9, 9, 9, 6}, {9, 6, 5, 6, 9, 0},
    {9, 9, 5, 9, 9, 4}, {6, 0, 0, 9, 5, 4}, {9, 9, 4, 9, 9, 7},
    {1, 1, 9, 9, 9, 9}, {6, 6, 1, 9, 9, 9}, {8, 8, 7, 9, 5, 6},
    {9, 9, 9, 9, 5, 4}, {7, 9, 4, 9, 9, 2}, {9, 9, 8, 9, 6, 5},
    {7, 8, 2, 9, 9, -1}};

/// Clause 11.5: the six font spacing tables for fields narrower than 12/256,
/// in spacing units, by font size 6 to 11 and width class. The printed table
/// gives two entries of size 11 (classes 6 and 8) as 0, which would stop the
/// cursor dead and fits no other row; the same table in Michael Dillon's
/// NAPLPS.ASC (1993, from CSA T500) gives 10, which is used.
constexpr int kSmallFontUnits[6][10] = {
    {2, 3, 4, 3, 4, 5, 6, 4, 5, 6},  {3, 4, 5, 4, 5, 6, 7, 5, 6, 7},
    {2, 3, 4, 4, 5, 6, 7, 6, 7, 8},  {3, 4, 5, 5, 6, 7, 8, 7, 8, 9},
    {4, 5, 6, 6, 7, 8, 9, 8, 9, 10}, {3, 4, 6, 6, 7, 8, 10, 8, 10, 11}};

/// Clause 11.6: the spacing table for fields of 12/256 and wider, by width
/// class.
constexpr int kLargeFontUnits[10] = {6, 5, 4, 4, 3, 2, 1, 2, 1, 0};

/// The spacing unit is 1/256 of the unit screen: DOMAIN 3's eight bits.
constexpr double kUnitsPerScreen = 256.0;

/// Slack for truncating a field width that arithmetic left a hair under a
/// whole number of units.
constexpr double kTruncationSlack = 1e-9;

/// The smallest scale factor clause 11.5 is taken to: a field narrower than
/// this many units is far below anything a receiver could draw.
constexpr double kSmallestScale = 1.0 / 1024.0;

}  // namespace

int naplps_width_class(uint8_t code, bool supplementary) {
  if (code < 0x20 || code > 0x7F) {
    return kNaplpsFullWidthClass;
  }
  const int column = (code >> 4) - 2;
  const int row = code & 0x0F;
  const int width_class = supplementary ? kSupplementaryClass[row][column]
                                        : kPrimaryClass[row][column];
  return width_class < 0 ? kNaplpsFullWidthClass : width_class;
}

double naplps_proportional_displacement(int width_class, double field_width) {
  const int column = std::clamp(width_class, 0, kNaplpsFullWidthClass);
  const double units = std::fabs(field_width) * kUnitsPerScreen;
  if (!(units > 0.0)) {
    return 0.0;
  }

  if (units < 12.0) {
    // Clause 11.5: the power of two that brings the width to between 6 and 12
    // units picks the table — the whole part of width over scale is its font
    // size — and the table's units are scaled back by it.
    double scale = 1.0;
    while (units / scale < 6.0 && scale > kSmallestScale) {
      scale /= 2.0;
    }
    const int size = std::clamp(
        static_cast<int>(std::floor(units / scale + kTruncationSlack)), 6, 11);
    return kSmallFontUnits[size - 6][column] * scale / kUnitsPerScreen;
  }

  // Clause 11.6, in whole units throughout, as the clause works in DOMAIN 3:
  // the width truncated to N; N x 11/13 less a unit, its lowest bit set and a
  // unit taken off again, which makes the scale factor F even, so the widest
  // class can always be scaled to an odd width; then N less the class's units
  // times F over 6, rounded half up.
  const int n = static_cast<int>(std::floor(units + kTruncationSlack));
  const int scale_factor = (((11 * n - 13) / 13) | 1) - 1;
  const int reduction = (2 * kLargeFontUnits[column] * scale_factor + 6) / 12;
  return static_cast<double>(n - reduction) / kUnitsPerScreen;
}

}  // namespace orc
