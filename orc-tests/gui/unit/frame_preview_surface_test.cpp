/*
 * File:        frame_preview_surface_test.cpp
 * Module:      orc-gui-tests
 * Purpose:     Smoke tests for the Qt RHI frame surface
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "gpu/frame_preview_surface.h"

#include <gtest/gtest.h>
#include <rhi/qrhi.h>

#include <QApplication>
#include <QFile>
#include <QTest>
#include <memory>

#include "gpu/frame_surface_factory.h"
#include "gpu/gpu_surface_policy.h"
#include "gpu/overlay_primitive_builder.h"

using orc::gui::gpu::FramePreviewSurface;
using orc::gui::gpu::GpuSurfacePolicy;
using orc::gui::gpu::OverlayPrimitives;

namespace {

QApplication& ensureApplication() {
  if (auto* existing =
          qobject_cast<QApplication*>(QCoreApplication::instance())) {
    return *existing;
  }
  static int argc = 1;
  static char arg0[] = "frame_preview_surface_test";
  static char* argv[] = {arg0, nullptr};
  static QApplication app(argc, argv);
  return app;
}

QImage makeFrame(const QSize& size) {
  QImage image(size, QImage::Format_RGBA8888);
  image.fill(QColor(32, 64, 96));
  return image;
}

OverlayPrimitives makeOverlay() {
  OverlayPrimitives primitives;
  primitives.quads.push_back({QRectF(4.0, 6.0, 20.0, 4.0), QColor(255, 0, 0)});
  primitives.lines.push_back(
      {QPointF(0.0, 12.0), QPointF(64.0, 12.0), QColor(0, 255, 0)});
  return primitives;
}

/**
 * @brief Drive a surface to its first submitted frame.
 *
 * @return True when a frame was submitted; false when the host could not
 *         bring the device up, which is the caller's cue to skip.
 */
bool renderOneFrame(FramePreviewSurface& surface, bool& failed) {
  bool submitted = false;
  QObject::connect(&surface, &QRhiWidget::frameSubmitted,
                   [&submitted]() { submitted = true; });
  QObject::connect(&surface, &QRhiWidget::renderFailed,
                   [&failed]() { failed = true; });

  surface.resize(64, 48);
  surface.show();
  QTest::qWaitFor([&]() { return submitted || failed; }, 5000);
  return submitted;
}

/// The shaders are compiled at build time and shipped in the library's
/// resources. This needs no device at all, so it is the one test here that
/// never skips: if the build integration is wrong, every GPU surface fails.
TEST(FramePreviewSurfaceShaders, AreCompiledIntoTheResources) {
  for (const QString& path :
       {QStringLiteral(":/orc/gpu/shaders/frame_preview.vert.qsb"),
        QStringLiteral(":/orc/gpu/shaders/frame_preview.frag.qsb"),
        QStringLiteral(":/orc/gpu/shaders/overlay.vert.qsb"),
        QStringLiteral(":/orc/gpu/shaders/overlay.frag.qsb")}) {
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    EXPECT_TRUE(QShader::fromSerialized(file.readAll()).isValid())
        << path.toStdString();
  }
}

/**
 * @brief Build the surface's pipelines headlessly on QRhi::Null.
 *
 * The widget tests below need a platform that can host a render-to-texture
 * widget, which a headless runner has not got. This one needs no platform at
 * all, so it is where CI actually checks that the compiled shaders load and
 * that the vertex and binding layouts the pipelines declare are accepted.
 */
TEST(FramePreviewSurfacePipelines, BuildOnTheNullBackendWithoutAWidgetShown) {
  ensureApplication();

  QRhiNullInitParams params;
  const std::unique_ptr<QRhi> rhi(QRhi::create(QRhi::Null, &params));
  ASSERT_NE(rhi, nullptr) << "the null backend is always available";

  const std::unique_ptr<QRhiTexture> colour(rhi->newTexture(
      QRhiTexture::RGBA8, QSize(64, 48), 1, QRhiTexture::RenderTarget));
  ASSERT_TRUE(colour->create());

  const std::unique_ptr<QRhiTextureRenderTarget> target(
      rhi->newTextureRenderTarget({{colour.get()}}));
  const std::unique_ptr<QRhiRenderPassDescriptor> pass(
      target->newCompatibleRenderPassDescriptor());
  target->setRenderPassDescriptor(pass.get());
  ASSERT_TRUE(target->create());

  FramePreviewSurface surface;
  EXPECT_TRUE(surface.buildResourcesForTesting(rhi.get(), pass.get(), 1));
}

TEST(FramePreviewSurfaceNullBackend, BuildsItsPipelinesAndSubmitsAFrame) {
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();

  FramePreviewSurface surface;
  surface.setApi(QRhiWidget::Api::Null);
  surface.setFrameImage(makeFrame(QSize(32, 24)));
  surface.setTargetRect(QRect(0, 0, 64, 48));
  surface.setOverlay(makeOverlay());

  bool failed = false;
  if (!renderOneFrame(surface, failed)) {
    GTEST_SKIP() << "no RHI device on this host";
  }

  EXPECT_FALSE(failed);
  EXPECT_FALSE(GpuSurfacePolicy::instance().renderFailed());
  EXPECT_FALSE(GpuSurfacePolicy::instance().backendName().isEmpty());
}

TEST(FramePreviewSurfaceNullBackend, RedrawsWithNoFrameAtAll) {
  // The empty case has to be a frame like any other: the preview surface is
  // shown before a render has ever completed.
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();

  FramePreviewSurface surface;
  surface.setApi(QRhiWidget::Api::Null);

  bool failed = false;
  if (!renderOneFrame(surface, failed)) {
    GTEST_SKIP() << "no RHI device on this host";
  }
  EXPECT_FALSE(failed);
}

TEST(FramePreviewSurfaceNullBackend, SurvivesAFrameThatChangesSize) {
  // The texture is reallocated only on a size change, and the bindings name
  // the texture object, so this is the path that has to re-point them.
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();

  FramePreviewSurface surface;
  surface.setApi(QRhiWidget::Api::Null);
  surface.setFrameImage(makeFrame(QSize(32, 24)));
  surface.setTargetRect(QRect(0, 0, 64, 48));

  bool failed = false;
  if (!renderOneFrame(surface, failed)) {
    GTEST_SKIP() << "no RHI device on this host";
  }

  surface.setFrameImage(makeFrame(QSize(48, 36)));
  surface.refresh();
  QTest::qWait(50);
  EXPECT_FALSE(failed);
}

TEST(FramePreviewSurfaceFactory, GivesAGpuSurfaceWhileThePolicyAllowsIt) {
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();
  ASSERT_TRUE(GpuSurfacePolicy::instance().useGpuSurface())
      << "this target sets ORC_GUI_GPU_RENDER=1";

  QWidget owner;
  auto surface = orc::gui::gpu::createFrameSurface(&owner);
  ASSERT_NE(surface, nullptr);
  EXPECT_NE(surface->widget(), nullptr) << "expected a QRhiWidget child";
  EXPECT_EQ(surface->widget()->parentWidget(), &owner);
}

TEST(FramePreviewSurfaceFactory, DropsToRasterOnceTheGpuHasFailed) {
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();

  QWidget owner;
  auto surface = orc::gui::gpu::createFrameSurface(&owner);
  ASSERT_NE(surface->widget(), nullptr);

  GpuSurfacePolicy::instance().noteRenderFailure(QStringLiteral("test"));

  EXPECT_TRUE(orc::gui::gpu::downgradeSurfaceIfFailed(surface, &owner));
  EXPECT_EQ(surface->widget(), nullptr) << "the raster path owns no widget";
  // Already downgraded: a second call has nothing to do.
  EXPECT_FALSE(orc::gui::gpu::downgradeSurfaceIfFailed(surface, &owner));

  GpuSurfacePolicy::instance().resetForTesting();
}

}  // namespace
