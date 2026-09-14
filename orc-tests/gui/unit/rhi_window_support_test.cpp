/*
 * File:        rhi_window_support_test.cpp
 * Module:      orc-gui-tests
 * Purpose:     Unit tests for the RHI window support decision
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "gpu/rhi_window_support.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QDialog>
#include <QWidget>
#include <QWindow>

using orc::gui::gpu::rhiWidgetWindowUsable;
using orc::gui::gpu::settleBackingStoreBeforeChildWindows;
using orc::gui::gpu::surfaceTypeForRhiApi;
using orc::gui::gpu::windowCanAdoptRhiWidget;
using orc::gui::gpu::windowSurfaceSupportsRhiApi;

namespace {

QApplication& ensureApplication() {
  if (auto* existing =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing;
  }
  static int argc = 1;
  static char arg0[] = "rhi_window_support_test";
  static char* argv[] = {arg0, nullptr};
  static QApplication app(argc, argv);
  return app;
}

TEST(RhiWindowSupport, EachBackendNamesItsSurfaceType) {
  EXPECT_EQ(surfaceTypeForRhiApi(QRhiWidget::Api::Metal),
            QSurface::MetalSurface);
  EXPECT_EQ(surfaceTypeForRhiApi(QRhiWidget::Api::OpenGL),
            QSurface::OpenGLSurface);
  EXPECT_EQ(surfaceTypeForRhiApi(QRhiWidget::Api::Vulkan),
            QSurface::VulkanSurface);
  EXPECT_EQ(surfaceTypeForRhiApi(QRhiWidget::Api::Direct3D11),
            QSurface::Direct3DSurface);
  EXPECT_EQ(surfaceTypeForRhiApi(QRhiWidget::Api::Direct3D12),
            QSurface::Direct3DSurface);
}

TEST(RhiWindowSupport, AMatchingSurfaceIsAccepted) {
  EXPECT_TRUE(windowSurfaceSupportsRhiApi(QSurface::MetalSurface,
                                          QRhiWidget::Api::Metal));
  EXPECT_TRUE(windowSurfaceSupportsRhiApi(QSurface::VulkanSurface,
                                          QRhiWidget::Api::Vulkan));
}

// The crash this guard exists for: a Metal QRhiWidget in a raster window.
TEST(RhiWindowSupport, ARasterWindowCannotBackMetal) {
  EXPECT_FALSE(windowSurfaceSupportsRhiApi(QSurface::RasterSurface,
                                           QRhiWidget::Api::Metal));
}

TEST(RhiWindowSupport, OneBackendsSurfaceDoesNotServeAnother) {
  EXPECT_FALSE(windowSurfaceSupportsRhiApi(QSurface::MetalSurface,
                                           QRhiWidget::Api::Vulkan));
  EXPECT_FALSE(windowSurfaceSupportsRhiApi(QSurface::OpenGLSurface,
                                           QRhiWidget::Api::Metal));
}

// Api::Null draws nowhere, so no window can be the wrong one for it.
TEST(RhiWindowSupport, TheNullBackendAcceptsAnyWindow) {
  EXPECT_TRUE(windowSurfaceSupportsRhiApi(QSurface::RasterSurface,
                                          QRhiWidget::Api::Null));
  EXPECT_TRUE(windowSurfaceSupportsRhiApi(QSurface::MetalSurface,
                                          QRhiWidget::Api::Null));
}

// An owner whose window does not exist yet is the ordinary case: the surface
// type is still open, and Qt will decide it with this widget in the tree.
TEST(RhiWindowSupport, AWindowThatDoesNotExistYetCanAdoptOne) {
  ensureApplication();
  QWidget owner;
  EXPECT_TRUE(windowCanAdoptRhiWidget(&owner, QRhiWidget::Api::Metal));
}

// The state behind the blank preview: the window was created before the RHI
// widget existed, so its surface type was decided without it and cannot be
// revisited. Adding one anyway is what leaves the window unable to flush.
TEST(RhiWindowSupport, AnAlreadyCreatedWindowIsJudgedByItsSurfaceType) {
  ensureApplication();
  QWidget owner;
  owner.winId();
  ASSERT_NE(owner.windowHandle(), nullptr);

  const QSurface::SurfaceType actual = owner.windowHandle()->surfaceType();
  EXPECT_EQ(windowCanAdoptRhiWidget(&owner, QRhiWidget::Api::Metal),
            windowSurfaceSupportsRhiApi(actual, QRhiWidget::Api::Metal));
  // Whatever the platform made it, a window is always fit for the backend its
  // own surface type names.
  EXPECT_TRUE(windowCanAdoptRhiWidget(&owner, QRhiWidget::Api::Null));
}

TEST(RhiWindowSupport, NoOwnerCannotAdoptOne) {
  EXPECT_FALSE(windowCanAdoptRhiWidget(nullptr, QRhiWidget::Api::Metal));
}

TEST(RhiWindowSupport, NoWidgetIsNotUsable) {
  EXPECT_FALSE(rhiWidgetWindowUsable(nullptr));
}

// Before the window exists there is nothing to object to, and QRhiWidget
// declines the paint itself while it has no QRhi.
TEST(RhiWindowSupport, AWidgetWithNoWindowYetIsNotRefused) {
  ensureApplication();
  QRhiWidget widget;
  EXPECT_TRUE(rhiWidgetWindowUsable(&widget));
}

// The widget's own window is what Qt sets the surface type on, so a widget
// sitting in a created window is judged by that window.
TEST(RhiWindowSupport, AWidgetIsJudgedByItsTopLevelWindow) {
  ensureApplication();
  QWidget top_level;
  auto* widget = new QRhiWidget(&top_level);
  top_level.winId();  // force the platform window into existence

  ASSERT_NE(top_level.windowHandle(), nullptr);
  EXPECT_EQ(rhiWidgetWindowUsable(widget),
            windowSurfaceSupportsRhiApi(top_level.windowHandle()->surfaceType(),
                                        widget->api()));
}

// The startup abort this exists for: Qt evaluates a window's backingstore by
// walking its whole child object tree, child windows included, so a hidden
// dialogue holding a render surface hands its need for an OpenGL surface to
// the main window - which then builds a GL context before anything is on
// screen, and on a machine whose GLX cannot supply one Qt calls qFatal.
// Settling the decision first is what keeps the parent out of it.
TEST(RhiWindowSupport, SettlingTheBackingStoreCreatesTheWindowWhileHidden) {
  ensureApplication();
  QWidget top_level;
  ASSERT_EQ(top_level.internalWinId(), 0U);

  settleBackingStoreBeforeChildWindows(&top_level);

  EXPECT_NE(top_level.internalWinId(), 0U)
      << "the decision is made when the native window is created; without one "
         "there is nothing settled";
  EXPECT_FALSE(top_level.isVisible());
}

// A dialogue built afterwards must not be dragged into existence with it: the
// point of settling early is that the render surfaces stay uncreated until
// their own window is shown.
TEST(RhiWindowSupport, ALaterChildWindowIsNotCreatedWithTheParent) {
  ensureApplication();
  QWidget top_level;
  settleBackingStoreBeforeChildWindows(&top_level);

  auto* dialog = new QDialog(&top_level);
  top_level.show();
  QCoreApplication::processEvents();

  EXPECT_EQ(dialog->internalWinId(), 0U);
  top_level.hide();
}

// Only a window has a backingstore to settle.
TEST(RhiWindowSupport, SettlingSomethingThatIsNotAWindowDoesNothing) {
  ensureApplication();
  QWidget top_level;
  auto* child = new QWidget(&top_level);

  settleBackingStoreBeforeChildWindows(child);
  EXPECT_EQ(child->internalWinId(), 0U);
}

TEST(RhiWindowSupport, SettlingNothingIsHarmless) {
  ensureApplication();
  settleBackingStoreBeforeChildWindows(nullptr);
}

}  // namespace
