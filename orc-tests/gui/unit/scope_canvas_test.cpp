/*
 * File:        scope_canvas_test.cpp
 * Module:      orc-gui-tests
 * Purpose:     Smoke tests for the Qt RHI scope canvas
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "gpu/scope_canvas.h"

#include <gtest/gtest.h>
#include <rhi/qrhi.h>

#include <QApplication>
#include <QFile>
#include <QTest>
#include <memory>

#include "gpu/gpu_surface_policy.h"
#include "gpu/scope_surface_factory.h"
#include "gpu/scope_vertex_builder.h"

using orc::gui::gpu::GpuSurfacePolicy;
using orc::gui::gpu::ScopeBlend;
using orc::gui::gpu::ScopeCanvas;
using orc::gui::gpu::ScopeFrame;
using orc::gui::gpu::ScopeVertex;

namespace {

QApplication& ensureApplication() {
  if (auto* existing =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing;
  }
  static int argc = 1;
  static char arg0[] = "scope_canvas_test";
  static char* argv[] = {arg0, nullptr};
  static QApplication app(argc, argv);
  return app;
}

QImage makeFurniture(const QSize& size, const QColor& fill) {
  QImage image(size, QImage::Format_RGBA8888);
  image.fill(fill);
  return image;
}

ScopeFrame makePlot(const QSize& canvas) {
  ScopeFrame frame;
  frame.canvas_size = canvas;
  frame.points = {ScopeVertex{4.5F, 4.5F, 1.0F, 0.0F},
                  ScopeVertex{8.5F, 12.5F, 3.0F, 0.0F}};
  frame.lines = {ScopeVertex{4.5F, 4.5F, 0.0F, 1.0F},
                 ScopeVertex{8.5F, 12.5F, 0.0F, 1.0F}};
  frame.underlay = makeFurniture(canvas, QColor(0, 0, 0));
  frame.overlay = makeFurniture(canvas, QColor(0, 0, 0, 0));
  frame.map.primary_scale = 0.02F;
  frame.map.secondary_scale = 0.02F;
  frame.map.brightness_bias = 0.25F;
  return frame;
}

/**
 * @brief Drive a canvas to its first submitted frame.
 *
 * @return True when a frame was submitted; false when the host could not
 *         bring the device up, which is the caller's cue to skip.
 */
bool renderOneFrame(ScopeCanvas& canvas, bool& failed) {
  bool submitted = false;
  QObject::connect(&canvas, &QRhiWidget::frameSubmitted,
                   [&submitted]() { submitted = true; });
  QObject::connect(&canvas, &QRhiWidget::renderFailed,
                   [&failed]() { failed = true; });

  canvas.resize(64, 64);
  canvas.show();
  // The result says whether the wait timed out; what matters here is which of
  // the two outcomes arrived, which the flags carry.
  (void)QTest::qWaitFor([&]() { return submitted || failed; }, 5000);
  return submitted;
}

/// The shaders are compiled at build time and shipped in the library's
/// resources. This needs no device at all, so it is the one test here that
/// never skips: if the build integration is wrong, every scope canvas fails.
TEST(ScopeCanvasShaders, AreCompiledIntoTheResources) {
  for (const QString& path :
       {QStringLiteral(":/orc/gpu/shaders/scope_accumulate.vert.qsb"),
        QStringLiteral(":/orc/gpu/shaders/scope_accumulate.frag.qsb"),
        QStringLiteral(":/orc/gpu/shaders/scope_map.vert.qsb"),
        QStringLiteral(":/orc/gpu/shaders/scope_map.frag.qsb")}) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    EXPECT_TRUE(QShader::fromSerialized(file.readAll()).isValid())
        << path.toStdString();
  }
}

/**
 * @brief Build the canvas's pipelines headlessly on QRhi::Null.
 *
 * The widget tests below need a platform that can host a render-to-texture
 * widget, which a headless runner has not got. This one needs no platform at
 * all, so it is where CI actually checks that the compiled shaders load, that
 * the off-screen accumulation target can be created, and that the vertex and
 * binding layouts the three pipelines declare are accepted.
 */
TEST(ScopeCanvasPipelines, BuildOnTheNullBackendWithoutAWidgetShown) {
  ensureApplication();

  QRhiNullInitParams params;
  const std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::Null, &params));
  ASSERT_NE(rhi, nullptr) << "the null backend is always available";

  const std::unique_ptr<QRhiTexture> colour(rhi->newTexture(
      QRhiTexture::RGBA8, QSize(64, 64), 1, QRhiTexture::RenderTarget));
  ASSERT_TRUE(colour->create());

  const std::unique_ptr<QRhiTextureRenderTarget> target(
      rhi->newTextureRenderTarget({{colour.get()}}));
  const std::unique_ptr<QRhiRenderPassDescriptor> pass(
      target->newCompatibleRenderPassDescriptor());
  target->setRenderPassDescriptor(pass.get());
  ASSERT_TRUE(target->create());

  ScopeCanvas canvas;
  EXPECT_TRUE(canvas.buildResourcesForTesting(rhi.get(), pass.get(), 1));
}

TEST(ScopeCanvasNullBackend, AccumulatesAndMapsAPlotInOneFrame) {
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();

  ScopeCanvas canvas;
  canvas.setApi(QRhiWidget::Api::Null);
  canvas.setFrame(makePlot(QSize(32, 32)));

  bool failed = false;
  if (!renderOneFrame(canvas, failed)) {
    GTEST_SKIP() << "no RHI device on this host";
  }

  EXPECT_FALSE(failed);
  EXPECT_FALSE(GpuSurfacePolicy::instance().renderFailed());
  EXPECT_FALSE(GpuSurfacePolicy::instance().backendName().isEmpty());
}

TEST(ScopeCanvasNullBackend, RedrawsWithNothingToPlot) {
  // A scope is shown before it has ever been given an acquisition, and its
  // dialogue also uses the canvas to display a CPU-rendered plot, which has
  // no vertices at all.
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();

  ScopeCanvas canvas;
  canvas.setApi(QRhiWidget::Api::Null);

  bool failed = false;
  if (!renderOneFrame(canvas, failed)) {
    GTEST_SKIP() << "no RHI device on this host";
  }
  EXPECT_FALSE(failed);
}

TEST(ScopeCanvasNullBackend, SurvivesAFrameThatChangesTheCanvasSize) {
  // The accumulation target is reallocated on a size change, and the map
  // bindings name the texture object, so this is the path that re-points them.
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();

  ScopeCanvas canvas;
  canvas.setApi(QRhiWidget::Api::Null);
  canvas.setFrame(makePlot(QSize(32, 32)));

  bool failed = false;
  if (!renderOneFrame(canvas, failed)) {
    GTEST_SKIP() << "no RHI device on this host";
  }

  ScopeFrame larger = makePlot(QSize(48, 24));
  larger.blend = ScopeBlend::kMax;
  larger.preserve_aspect = false;
  larger.smooth = false;
  canvas.setFrame(std::move(larger));
  canvas.refresh();
  QTest::qWait(50);
  EXPECT_FALSE(failed);
}

/**
 * @brief Run both passes on whatever device the host actually has.
 *
 * The tests above pin the backend to Null, which accepts every command and
 * executes none of them: they check that the pipelines are well formed, not
 * that a device will run them. This one takes the backend the application
 * would take, so on a developer's machine it is the shaders themselves that
 * are exercised. A headless runner has no such device and skips.
 */
TEST(ScopeCanvasDefaultBackend, RendersBothPassesOnTheHostsOwnDevice) {
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();

  ScopeCanvas canvas;
  canvas.setFrame(makePlot(QSize(32, 32)));

  bool failed = false;
  if (!renderOneFrame(canvas, failed)) {
    GTEST_SKIP() << "no RHI device on this host";
  }

  EXPECT_FALSE(failed);
  EXPECT_FALSE(GpuSurfacePolicy::instance().renderFailed());
}

TEST(ScopeCanvasFactory, GivesACanvasWhileThePolicyAllowsIt) {
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();
  ASSERT_TRUE(GpuSurfacePolicy::instance().useGpuSurface())
      << "this target sets ORC_GUI_GPU_RENDER=1";

  QWidget owner;
  auto surface = orc::gui::gpu::createScopeSurface(&owner);
  ASSERT_NE(surface, nullptr);
  ASSERT_NE(surface->widget(), nullptr);
  EXPECT_EQ(surface->widget()->parentWidget(), &owner);
}

TEST(ScopeCanvasFactory, DropsTheCanvasOnceTheGpuHasFailed) {
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();

  QWidget owner;
  auto surface = orc::gui::gpu::createScopeSurface(&owner);
  ASSERT_NE(surface, nullptr);

  GpuSurfacePolicy::instance().noteRenderFailure(QStringLiteral("test"));

  EXPECT_TRUE(orc::gui::gpu::dropScopeSurfaceIfFailed(surface));
  EXPECT_EQ(surface, nullptr) << "the scope falls back to its own renderer";
  // Already dropped: a second call has nothing to do.
  EXPECT_FALSE(orc::gui::gpu::dropScopeSurfaceIfFailed(surface));

  GpuSurfacePolicy::instance().resetForTesting();
}

}  // namespace
