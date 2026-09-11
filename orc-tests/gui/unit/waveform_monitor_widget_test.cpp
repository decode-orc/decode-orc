/*
 * File:        waveform_monitor_widget_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Offscreen tests that the waveform monitor draws its trace at
 *              the level the samples carry
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QImage>
#include <QPixmap>
#include <optional>
#include <utility>
#include <vector>

#include "waveformmonitorwidget.h"

namespace gui_unit_test {
namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-waveform-monitor-test";
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

constexpr int kLines = 20;
constexpr int kSamplesPerLine = 40;

orc::presenters::VideoParametersView ntscParameters() {
  orc::presenters::VideoParametersView params;
  params.system = orc::presenters::VideoSystem::NTSC;
  params.frame_width_nominal = kSamplesPerLine;
  params.active_video_start = 4;
  params.active_video_end = kSamplesPerLine - 1;
  params.sync_tip_level = 0;
  params.blanking_level = 240;
  params.black_level = 280;
  params.white_level = 800;
  params.peak_level = 1000;
  return params;
}

// Every active sample at one level, so the trace is a single horizontal band
// and its height is the only thing the render can get wrong.
std::vector<int16_t> flatFrame(int16_t level) {
  return std::vector<int16_t>(
      static_cast<size_t>(kLines) * static_cast<size_t>(kSamplesPerLine),
      level);
}

// The widget paints axes, a grid and level markers as well as the trace, and
// all of those are identical between two renders of the same frame geometry.
// Diffing one render against another therefore isolates the trace exactly,
// without the test having to know the theme's colours.
std::vector<int> rowsDifferingBetween(const QImage& before,
                                      const QImage& after) {
  std::vector<int> rows;
  if (before.size() != after.size()) {
    return rows;
  }
  for (int y = 0; y < after.height(); ++y) {
    for (int x = 0; x < after.width(); ++x) {
      if (before.pixel(x, y) != after.pixel(x, y)) {
        rows.push_back(y);
        break;
      }
    }
  }
  return rows;
}

// Consecutive runs of rows, allowing a one-row gap so anti-aliasing at a
// band's edge does not split it in two.
std::vector<std::pair<int, int>> bandsIn(const std::vector<int>& rows) {
  std::vector<std::pair<int, int>> bands;
  for (const int row : rows) {
    if (!bands.empty() && row - bands.back().second <= 2) {
      bands.back().second = row;
      continue;
    }
    bands.emplace_back(row, row);
  }
  return bands;
}

QImage renderWith(WaveformMonitorWidget& widget,
                  const std::vector<int16_t>& samples, int lines) {
  widget.setData(
      samples, lines, 0,
      std::optional<orc::presenters::VideoParametersView>(ntscParameters()));
  QCoreApplication::processEvents();
  return widget.grab().toImage();
}

// A flat signal is one level, so it draws one band - and a higher level draws
// it higher up the plot. Between them that is the whole of what accumulate()
// and the render pass are for: put each sample in a millivolt bin, and map
// bins to rows.
TEST(WaveformMonitorWidgetTest, DrawsEachFlatLevelAsOneBandAtItsOwnHeight) {
  ensureApplication();

  WaveformMonitorWidget widget;
  widget.resize(600, 400);
  widget.show();
  QCoreApplication::processEvents();

  const QImage low = renderWith(widget, flatFrame(300), kLines);
  const QImage high = renderWith(widget, flatFrame(780), kLines);

  const std::vector<std::pair<int, int>> bands =
      bandsIn(rowsDifferingBetween(low, high));
  ASSERT_EQ(bands.size(), 2u)
      << "a flat signal should move one band, not scatter down the plot";
  for (const auto& band : bands) {
    EXPECT_LE(band.second - band.first, 8)
        << "the trace for a single level spread over many rows";
  }
  // Screen y grows downwards, so the two bands are the brighter signal's
  // (upper) and the darker signal's (lower).
  EXPECT_LT(bands.front().second, bands.back().first);
}

TEST(WaveformMonitorWidgetTest, RendersTheSameFrameIdentically) {
  ensureApplication();

  WaveformMonitorWidget widget;
  widget.resize(600, 400);
  widget.show();
  QCoreApplication::processEvents();

  const QImage first = renderWith(widget, flatFrame(500), kLines);
  // The count grid is retained between frames; a second frame of the same
  // samples must not accumulate on top of the first.
  const QImage second = renderWith(widget, flatFrame(500), kLines);

  EXPECT_TRUE(rowsDifferingBetween(first, second).empty());
}

// The grid is retained between frames, so a frame that clears it must not
// leave the previous frame's trace on screen.
TEST(WaveformMonitorWidgetTest, ClearsTheTraceWhenGivenNoSamples) {
  ensureApplication();

  WaveformMonitorWidget widget;
  widget.resize(600, 400);
  widget.show();
  QCoreApplication::processEvents();

  const QImage empty_first = renderWith(widget, {}, 0);
  const QImage with_trace = renderWith(widget, flatFrame(500), kLines);
  ASSERT_FALSE(rowsDifferingBetween(empty_first, with_trace).empty());

  const QImage empty_again = renderWith(widget, {}, 0);
  EXPECT_TRUE(rowsDifferingBetween(empty_first, empty_again).empty());
}

}  // namespace
}  // namespace gui_unit_test
