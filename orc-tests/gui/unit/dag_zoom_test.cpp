/*
 * File:        dag_zoom_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Zoom step and limit rules for the DAG canvas
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "dag_zoom.h"

#include <gtest/gtest.h>

namespace gui_unit_test {

using orc::gui::canZoomDagIn;
using orc::gui::canZoomDagOut;
using orc::gui::kDagMaxScale;
using orc::gui::kDagMinScale;
using orc::gui::kDagZoomStep;
using orc::gui::steppedDagScale;

TEST(DagZoomTest, OneStepScalesByTheZoomStep) {
  EXPECT_DOUBLE_EQ(steppedDagScale(0.8, 1), 0.8 * kDagZoomStep);
  EXPECT_DOUBLE_EQ(steppedDagScale(0.8, -1), 0.8 / kDagZoomStep);
}

TEST(DagZoomTest, SteppingPastTheRangeEndsClamps) {
  EXPECT_DOUBLE_EQ(steppedDagScale(kDagMaxScale, 1), kDagMaxScale);
  EXPECT_DOUBLE_EQ(steppedDagScale(kDagMinScale, -1), kDagMinScale);
  EXPECT_DOUBLE_EQ(steppedDagScale(0.75, -3), kDagMinScale);
}

TEST(DagZoomTest, RepeatedStepsReachBothEndsOfTheRange) {
  double scale = kDagMaxScale;
  int steps_out = 0;
  while (canZoomDagOut(scale) && steps_out < 100) {
    scale = steppedDagScale(scale, -1);
    ++steps_out;
  }
  EXPECT_DOUBLE_EQ(scale, kDagMinScale);
  // More than one distinct step, or the buttons would be a toggle.
  EXPECT_GT(steps_out, 1);

  int steps_in = 0;
  while (canZoomDagIn(scale) && steps_in < 100) {
    scale = steppedDagScale(scale, 1);
    ++steps_in;
  }
  EXPECT_DOUBLE_EQ(scale, kDagMaxScale);
}

TEST(DagZoomTest, LimitsReportWhichDirectionIsStillAvailable) {
  EXPECT_FALSE(canZoomDagIn(kDagMaxScale));
  EXPECT_TRUE(canZoomDagOut(kDagMaxScale));
  EXPECT_TRUE(canZoomDagIn(kDagMinScale + 0.1));
  EXPECT_FALSE(canZoomDagOut(kDagMinScale));
  // Drift within the comparison tolerance still counts as at the limit.
  EXPECT_FALSE(canZoomDagIn(kDagMaxScale - 0.0001));
}

}  // namespace gui_unit_test
