/*
 * File:        preview_dialog_gpu_window_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     The preview dialog's window has to be able to host the GPU
 *              render surface it is built with
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QMenuBar>
#include <QRhiWidget>
#include <QWindow>

#include "fieldpreviewwidget.h"
#include "gpu/gpu_surface_policy.h"
#include "gpu/rhi_window_support.h"
#include "previewdialog.h"

namespace gui_unit_test {

namespace {

QApplication& ensureApplication() {
  if (auto* existing_app =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing_app;
  }
  static int argc = 1;
  static char app_name[] = "orc-gui-preview-window-test";
  static char* argv[] = {app_name, nullptr};
  static QApplication* app = [] {
    auto* created = new QApplication(argc, argv);
    created->setQuitOnLastWindowClosed(false);
    return created;
  }();
  return *app;
}

}  // namespace

// A QMenuBar brings its window into existence as soon as it is attached (macOS
// does this for the native menu bar), and a window's surface type is settled
// when it is created, from the render surfaces in the tree at that moment — a
// later creation does not revisit it. Building the preview widget after the
// menu bar therefore leaves its GPU surface in a window that can never draw
// it, and the preview falls back to the CPU. These tests fail on such a
// platform if the construction order in setupUI() is ever reversed.

// The same rule stated about the window itself, for a platform that creates it
// during construction: whatever made the window, it has to have been made with
// the render surface in view.
TEST(PreviewDialogGpuWindow, ItsWindowCanBackTheRenderSurface) {
  ensureApplication();
  orc::gui::gpu::GpuSurfacePolicy::instance().resetForTesting();

  PreviewDialog dialog;
  if (dialog.windowHandle() == nullptr) {
    GTEST_SKIP() << "this platform does not create the window that early";
  }

  EXPECT_TRUE(orc::gui::gpu::windowSurfaceSupportsRhiApi(
      dialog.windowHandle()->surfaceType(), orc::gui::gpu::defaultRhiApi()))
      << "the dialog's window cannot host a render surface, so the preview "
         "can only draw on the CPU";
}

// The ordering rule this depends on, stated where a reader of setupUI() would
// look for it: the menu bar is attached after the preview widget exists.
TEST(PreviewDialogGpuWindow, TheMenuBarIsNotWhatBringsTheWindowUp) {
  ensureApplication();
  orc::gui::gpu::GpuSurfacePolicy::instance().resetForTesting();

  PreviewDialog dialog;
  const auto* menu_bar = dialog.findChild<QMenuBar*>();
  ASSERT_NE(menu_bar, nullptr);
  // Both exist; the preview widget is the one that was built first.
  const auto* preview = dialog.findChild<FieldPreviewWidget*>();
  ASSERT_NE(preview, nullptr);
  EXPECT_LT(dialog.children().indexOf(preview),
            dialog.children().indexOf(menu_bar))
      << "the menu bar was attached before the preview widget was built";
}

// The other side of the startup fix: the main window settles its own
// backingstore before this dialogue is built, so that a machine with no
// working GL can start. That must not cost the preview its GPU path — this
// dialogue is a window in its own right and is evaluated on its own tree when
// it is shown, whatever its parent decided earlier.
TEST(PreviewDialogGpuWindow, ShowingItGivesItAWindowThatBacksTheSurface) {
  ensureApplication();
  orc::gui::gpu::GpuSurfacePolicy::instance().resetForTesting();

  QWidget parent;
  // As the main window does: created, and hidden, before the dialogue exists.
  orc::gui::gpu::settleBackingStoreBeforeChildWindows(&parent);

  PreviewDialog dialog(&parent);
  if (dialog.findChild<QRhiWidget*>() == nullptr) {
    GTEST_SKIP() << "this platform has no RHI, so the dialog is on the CPU "
                    "path and has no surface to back";
  }

  dialog.show();
  QCoreApplication::processEvents();

  ASSERT_NE(dialog.windowHandle(), nullptr);
  EXPECT_TRUE(orc::gui::gpu::windowSurfaceSupportsRhiApi(
      dialog.windowHandle()->surfaceType(), orc::gui::gpu::defaultRhiApi()))
      << "the preview lost its GPU path to its parent's earlier window";
  dialog.hide();
}

}  // namespace gui_unit_test
