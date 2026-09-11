/*
 * File:        histogram_dialog_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     Offscreen tests for the histogram dialogue's per-frame update
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "preview/histogram_dialog.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QGraphicsScene>
#include <QGraphicsView>

#include "orc_histogram.h"

namespace gui_unit_test {
namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }

  static int argc = 3;
  static char app_name[] = "orc-gui-histogram-dialog-test";
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

// Every plot is a QGraphicsView over its own scene; counting the items in all
// of them is how the dialogue's churn is visible from outside.
int totalGraphicsItems(QWidget& dialog) {
  int count = 0;
  for (QGraphicsView* view : dialog.findChildren<QGraphicsView*>()) {
    if (view->scene() != nullptr) {
      count += static_cast<int>(view->scene()->items().size());
    }
  }
  return count;
}

orc::VideoHistogramData makeHistogram(uint64_t field_number,
                                      orc::VideoSystem system) {
  orc::VideoHistogramData data;
  data.system = system;
  data.field_number = field_number;
  data.width = 928;
  data.height = 576;
  // A different distribution per frame, so an update that did nothing would
  // not be mistaken for one that worked.
  for (size_t bin = 0; bin < orc::VideoHistogramData::kBinCount; ++bin) {
    const uint32_t value = static_cast<uint32_t>((bin + field_number) % 97);
    data.y_bins[bin] = value;
    data.u_bins[bin] = value / 2;
    data.v_bins[bin] = value / 3;
  }
  return data;
}

// Playback drives this 25 times a second. Deleting and re-creating every zone,
// guide line and trace in three plots on each of those frames was the cost;
// the picture differs only in the bin heights.
TEST(HistogramDialogTest, RepeatedUpdatesReuseTheirPlotItems) {
  ensureApplication();

  HistogramDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  dialog.updateHistogram(makeHistogram(0, orc::VideoSystem::PAL));
  QCoreApplication::processEvents();

  const int after_first = totalGraphicsItems(dialog);
  ASSERT_GT(after_first, 0);

  for (uint64_t field = 1; field <= 100; ++field) {
    dialog.updateHistogram(makeHistogram(field, orc::VideoSystem::PAL));
  }
  QCoreApplication::processEvents();

  EXPECT_EQ(totalGraphicsItems(dialog), after_first)
      << "the plots grew or churned their items across 100 updates";
}

// The zones and guide lines differ between a luma and a chroma channel, and
// between the two video systems, so those changes must still rebuild them.
TEST(HistogramDialogTest, RebuildsPlotItemsWhenTheVideoSystemChanges) {
  ensureApplication();

  HistogramDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  dialog.updateHistogram(makeHistogram(0, orc::VideoSystem::PAL));
  QCoreApplication::processEvents();
  const int pal_items = totalGraphicsItems(dialog);

  // NTSC adds the black-pedestal guide line to the luma plot.
  dialog.updateHistogram(makeHistogram(1, orc::VideoSystem::NTSC));
  QCoreApplication::processEvents();
  EXPECT_NE(totalGraphicsItems(dialog), pal_items);

  // ...and going back drops it again, rather than accumulating.
  dialog.updateHistogram(makeHistogram(2, orc::VideoSystem::PAL));
  QCoreApplication::processEvents();
  EXPECT_EQ(totalGraphicsItems(dialog), pal_items);
}

// clearDisplay() deletes every item in every plot, so the remembered trace
// pointers are dangling until the next update rebuilds them.
TEST(HistogramDialogTest, UpdatesAgainAfterTheDisplayIsCleared) {
  ensureApplication();

  HistogramDialog dialog;
  dialog.show();
  QCoreApplication::processEvents();

  dialog.updateHistogram(makeHistogram(0, orc::VideoSystem::PAL));
  QCoreApplication::processEvents();
  const int before = totalGraphicsItems(dialog);

  dialog.clearDisplay();
  QCoreApplication::processEvents();

  dialog.updateHistogram(makeHistogram(1, orc::VideoSystem::PAL));
  QCoreApplication::processEvents();
  EXPECT_EQ(totalGraphicsItems(dialog), before);
}

}  // namespace
}  // namespace gui_unit_test
