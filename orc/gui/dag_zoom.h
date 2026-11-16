/*
 * File:        dag_zoom.h
 * Module:      orc-gui
 * Purpose:     Zoom step and limit rules for the DAG canvas
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef DAG_ZOOM_H
#define DAG_ZOOM_H

#include <algorithm>
#include <cmath>

namespace orc::gui {

// Smallest canvas scale. Below this the stage captions and port labels stop
// being readable.
inline constexpr double kDagMinScale = 0.7;

// Largest canvas scale: stages are drawn at their natural size.
inline constexpr double kDagMaxScale = 1.0;

// Scale used by View > Reset Zoom.
inline constexpr double kDagDefaultScale = kDagMaxScale;

// Scale factor applied per zoom step (one wheel notch or one button press);
// 10% rather than the QtNodes default of 20% so the small range above still
// offers several distinct steps.
inline constexpr double kDagZoomStep = 1.1;

// Scales closer than this are treated as equal, so floating-point drift from
// repeated multiplication never leaves a step that changes nothing visible.
inline constexpr double kDagScaleEpsilon = 0.001;

// Clamp |scale| to the canvas zoom range.
inline double clampDagScale(double scale) {
  return std::clamp(scale, kDagMinScale, kDagMaxScale);
}

// The scale |steps| zoom steps away from |current|: positive steps zoom in,
// negative steps zoom out. The result is clamped to the canvas zoom range.
inline double steppedDagScale(double current, int steps) {
  return clampDagScale(current * std::pow(kDagZoomStep, steps));
}

// True when the canvas can still zoom in from |current|.
inline bool canZoomDagIn(double current) {
  return current < kDagMaxScale - kDagScaleEpsilon;
}

// True when the canvas can still zoom out from |current|.
inline bool canZoomDagOut(double current) {
  return current > kDagMinScale + kDagScaleEpsilon;
}

}  // namespace orc::gui

#endif  // DAG_ZOOM_H
