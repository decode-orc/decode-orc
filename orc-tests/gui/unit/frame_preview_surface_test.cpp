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
#include <orc/stage/preview/orc_preview_carriers.h>
#include <orc/support/colour_preview_conversion.h>
#include <rhi/qrhi.h>

#include <QApplication>
#include <QFile>
#include <QTest>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

#include "gpu/frame_surface_factory.h"
#include "gpu/gpu_surface_policy.h"
#include "gpu/overlay_primitive_builder.h"
#include "gpu/raster_frame_surface.h"

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

/// A carrier whose corners are all different colours, so a converted frame
/// that came out upside down or mirrored cannot pass for a correct one.
orc::ColourFrameCarrier makeCornerCarrier(const QSize& size) {
  orc::ColourFrameCarrier carrier{};
  carrier.width = static_cast<uint32_t>(size.width());
  carrier.height = static_cast<uint32_t>(size.height());
  carrier.system = orc::VideoSystem::PAL;
  carrier.cvbs_blanking = 256.0;
  carrier.cvbs_black = 256.0;
  carrier.cvbs_white = 844.0;
  carrier.colorimetry.matrix_coefficients =
      orc::ColorimetricMatrixCoefficients::BT601_625;
  carrier.colorimetry.transfer_characteristics =
      orc::ColorimetricTransferCharacteristics::Gamma22;

  const size_t samples = static_cast<size_t>(carrier.width) * carrier.height;
  carrier.y_plane.resize(samples);
  carrier.u_plane.resize(samples);
  carrier.v_plane.resize(samples);
  for (size_t i = 0; i < samples; ++i) {
    const bool right = (i % carrier.width) >= (carrier.width / 2);
    const bool lower = (i / carrier.width) >= (carrier.height / 2);
    carrier.y_plane[i] = lower ? 700.0 : 400.0;
    carrier.u_plane[i] = right ? 150.0 : -150.0;
    carrier.v_plane[i] = lower ? -120.0 : 120.0;
  }
  return carrier;
}

/// The payload a render hands the surface: shared, never copied.
std::shared_ptr<const orc::PreviewPlanes> makePlanes(const QSize& size) {
  return std::make_shared<const orc::PreviewPlanes>(
      orc::preview_planes_from_colour_carrier(makeCornerCarrier(size)));
}

/// A signal-domain payload whose samples ramp across the frame, with a dimmed
/// band down the left edge and one burnt-in dropout row.
std::shared_ptr<const orc::PreviewPlanes> makeSignalPlanes(const QSize& size) {
  orc::PreviewPlanes planes;
  planes.domain = orc::PreviewPlaneDomain::Signal;
  planes.width = static_cast<uint32_t>(size.width());
  planes.height = static_cast<uint32_t>(size.height());
  planes.apply_level_scaling = true;
  planes.black_level = 256.0F;
  planes.white_level = 844.0F;
  planes.sync_tip_level = 0.0F;
  planes.peak_level = 1023.0F;

  planes.y_plane.resize(static_cast<size_t>(size.width()) * size.height());
  for (int y = 0; y < size.height(); ++y) {
    for (int x = 0; x < size.width(); ++x) {
      planes.y_plane[(static_cast<size_t>(y) * size.width()) + x] =
          256.0F + (588.0F * static_cast<float>(x) /
                    static_cast<float>(size.width() - 1));
    }
  }

  planes.dimmed_bands.push_back(
      {0, 0, 8, static_cast<uint32_t>(size.height())});
  planes.burn_in_dropouts = true;
  orc::DropoutRegion dropout;
  dropout.line = 3;
  dropout.start_sample = 20;
  dropout.end_sample = 30;
  planes.dropout_regions.push_back(dropout);

  return std::make_shared<const orc::PreviewPlanes>(std::move(planes));
}

/// The greyscale code scale_10bit_to_8bit() gives a clamped preview.
int cpuGreyCode(float sample_value) {
  const int range = 844 - 256;
  const int scaled = ((static_cast<int>(sample_value) - 256) * 255) / range;
  return std::clamp(scaled, 0, 255);
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

/// Compare a converted frame with the CPU conversion of the same carrier,
/// pixel by pixel. The corners differ from one another in the test pattern,
/// so a result that came out flipped or mirrored fails here rather than
/// passing on a symmetry.
void expectMatchesConversion(const QImage& produced, const QImage& reference,
                             int tolerance) {
  ASSERT_EQ(produced.size(), reference.size());

  int worst = 0;
  QPoint worst_at;
  for (int y = 0; y < produced.height(); ++y) {
    for (int x = 0; x < produced.width(); ++x) {
      const QColor got = produced.pixelColor(x, y);
      const QColor want = reference.pixelColor(x, y);
      const int difference = std::max({std::abs(got.red() - want.red()),
                                       std::abs(got.green() - want.green()),
                                       std::abs(got.blue() - want.blue())});
      if (difference > worst) {
        worst = difference;
        worst_at = QPoint(x, y);
      }
    }
  }

  EXPECT_LE(worst, tolerance)
      << "worst difference at " << worst_at.x() << "," << worst_at.y();
}

/// The shaders are compiled at build time and shipped in the library's
/// resources. This needs no device at all, so it is the one test here that
/// never skips: if the build integration is wrong, every GPU surface fails.
TEST(FramePreviewSurfaceShaders, AreCompiledIntoTheResources) {
  for (const QString& path :
       {QStringLiteral(":/orc/gpu/shaders/frame_preview.vert.qsb"),
        QStringLiteral(":/orc/gpu/shaders/frame_preview.frag.qsb"),
        QStringLiteral(":/orc/gpu/shaders/overlay.vert.qsb"),
        QStringLiteral(":/orc/gpu/shaders/overlay.frag.qsb"),
        QStringLiteral(":/orc/gpu/shaders/scope_fullscreen.vert.qsb"),
        QStringLiteral(":/orc/gpu/shaders/frame_planes.frag.qsb"),
        QStringLiteral(":/orc/gpu/shaders/frame_planes_signal.frag.qsb")}) {
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

TEST(FramePreviewSurfacePlanes, AreRefusedByTheRasterPath) {
  // QPainter has no shader to finish the conversion with, and the caller has
  // to be told rather than shown an empty widget.
  orc::gui::gpu::RasterFrameSurface surface(nullptr);
  const auto planes = makePlanes(QSize(8, 8));
  ASSERT_TRUE(planes->is_valid());
  EXPECT_FALSE(surface.setFramePlanes(planes));
}

TEST(FramePreviewSurfaceNullBackend, SubmitsAFrameThatArrivedAsPlanes) {
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();

  FramePreviewSurface surface;
  surface.setApi(QRhiWidget::Api::Null);
  surface.setTargetRect(QRect(0, 0, 64, 48));
  ASSERT_TRUE(surface.setFramePlanes(makePlanes(QSize(32, 24))));

  bool failed = false;
  if (!renderOneFrame(surface, failed)) {
    GTEST_SKIP() << "no RHI device on this host";
  }

  EXPECT_FALSE(failed);
  EXPECT_FALSE(GpuSurfacePolicy::instance().renderFailed());
  EXPECT_TRUE(GpuSurfacePolicy::instance().planeConversionAvailable());
}

TEST(FramePreviewSurfaceNullBackend, SwitchesBetweenPlanesAndImages) {
  // The frame texture is the conversion pass's target on one path and an
  // uploaded texture on the other, and it carries a flag only the first
  // needs, so changing path reallocates it even at the same size.
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();

  FramePreviewSurface surface;
  surface.setApi(QRhiWidget::Api::Null);
  surface.setTargetRect(QRect(0, 0, 64, 48));
  surface.setFrameImage(makeFrame(QSize(32, 24)));

  bool failed = false;
  if (!renderOneFrame(surface, failed)) {
    GTEST_SKIP() << "no RHI device on this host";
  }

  ASSERT_TRUE(surface.setFramePlanes(makePlanes(QSize(32, 24))));
  surface.refresh();
  QTest::qWait(50);

  surface.setFrameImage(makeFrame(QSize(32, 24)));
  surface.refresh();
  QTest::qWait(50);

  EXPECT_FALSE(failed);
}

/**
 * @brief Convert planes on whatever device this host actually has.
 *
 * The null backend accepts the conversion pass without executing it, so it
 * cannot say whether the shader compiles for a real backend, whether a
 * floating-point texture can be fetched by texel, or which way up the result
 * lands. Only a real device answers those, which is why this test uses the
 * host's own and skips where there is not one.
 */
TEST(FramePreviewSurfaceDefaultBackend, ConvertsPlanesOnTheHostsOwnDevice) {
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();

  const QSize frame(64, 48);
  const orc::ColourFrameCarrier carrier = makeCornerCarrier(frame);
  const orc::PreviewImage expected =
      orc::render_preview_from_colour_carrier(carrier);
  ASSERT_TRUE(expected.is_valid());

  FramePreviewSurface surface;
  // Nearest filtering at 1:1, so every output pixel is one input texel and a
  // difference is the conversion's rather than the rescale's.
  surface.setSmoothScaling(false);
  surface.setTargetRect(QRect(QPoint(0, 0), frame));
  ASSERT_TRUE(surface.setFramePlanes(std::make_shared<const orc::PreviewPlanes>(
      orc::preview_planes_from_colour_carrier(carrier))));

  bool submitted = false;
  bool failed = false;
  QObject::connect(&surface, &QRhiWidget::frameSubmitted,
                   [&submitted]() { submitted = true; });
  QObject::connect(&surface, &QRhiWidget::renderFailed,
                   [&failed]() { failed = true; });
  surface.resize(frame);
  surface.show();
  (void)QTest::qWaitFor([&]() { return submitted || failed; }, 5000);

  if (!submitted || failed) {
    GTEST_SKIP() << "no usable graphics device on this host";
  }
  if (surface.devicePixelRatioF() != 1.0) {
    GTEST_SKIP() << "a scaled display makes the grab a rescale of the frame";
  }

  const QImage grabbed = surface.grabFramebuffer();
  if (grabbed.isNull() || grabbed.size() != frame) {
    GTEST_SKIP() << "this backend cannot hand the frame back";
  }

  QImage reference(expected.rgb_data.data(), static_cast<int>(expected.width),
                   static_cast<int>(expected.height),
                   static_cast<qsizetype>(expected.width) * 3,
                   QImage::Format_RGB888);
  // One code of slack: the device works in single precision where the CPU
  // conversion works in double, and a backend is free to hand the grab back
  // through its own 8-bit round trip. On the OpenGL backend this frame comes
  // out with no difference at all.
  expectMatchesConversion(grabbed.convertToFormat(QImage::Format_RGB888),
                          reference.convertToFormat(QImage::Format_RGB888), 1);
}

/**
 * @brief Convert a signal frame on whatever device this host actually has.
 *
 * The greyscale mapping is integer on the CPU and floating point on the
 * device, and the mask and the dropout burn-in are drawn as bands over the
 * converted frame rather than written into it. Only a real device says
 * whether all three land where the image path put them.
 */
TEST(FramePreviewSurfaceDefaultBackend,
     ConvertsSignalPlanesOnTheHostsOwnDevice) {
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();

  const QSize frame(64, 48);
  const auto planes = makeSignalPlanes(frame);

  FramePreviewSurface surface;
  surface.setSmoothScaling(false);
  surface.setTargetRect(QRect(QPoint(0, 0), frame));
  ASSERT_TRUE(surface.setFramePlanes(planes));

  bool submitted = false;
  bool failed = false;
  QObject::connect(&surface, &QRhiWidget::frameSubmitted,
                   [&submitted]() { submitted = true; });
  QObject::connect(&surface, &QRhiWidget::renderFailed,
                   [&failed]() { failed = true; });
  surface.resize(frame);
  surface.show();
  (void)QTest::qWaitFor([&]() { return submitted || failed; }, 5000);

  if (!submitted || failed) {
    GTEST_SKIP() << "no usable graphics device on this host";
  }
  if (surface.devicePixelRatioF() != 1.0) {
    GTEST_SKIP() << "a scaled display makes the grab a rescale of the frame";
  }
  const QImage grabbed = surface.grabFramebuffer();
  if (grabbed.isNull() || grabbed.size() != frame) {
    GTEST_SKIP() << "this backend cannot hand the frame back";
  }

  // Away from the mask and the dropout, the greyscale mapping stands alone.
  for (int x = 16; x < frame.width(); ++x) {
    const int expected = cpuGreyCode(
        planes->y_plane[(static_cast<size_t>(20) * frame.width()) + x]);
    const QColor got = grabbed.pixelColor(x, 20);
    EXPECT_LE(std::abs(got.red() - expected), 1) << "at column " << x;
    EXPECT_EQ(got.red(), got.green()) << "at column " << x;
    EXPECT_EQ(got.green(), got.blue()) << "at column " << x;
  }

  // The masked column is dimmed to about 30% of what it would have been.
  const int undimmed = cpuGreyCode(
      planes->y_plane[(static_cast<size_t>(20) * frame.width()) + 4]);
  const int dimmed = grabbed.pixelColor(4, 20).red();
  EXPECT_LT(dimmed, undimmed);
  EXPECT_NEAR(dimmed, undimmed * 3 / 10, 4);

  // The dropout row is red where the region covers it and untouched beside it.
  const QColor on_dropout = grabbed.pixelColor(25, 3);
  EXPECT_GT(on_dropout.red(), 180);
  EXPECT_LT(on_dropout.green(), 80);
  EXPECT_LT(on_dropout.blue(), 80);
  const QColor beside_dropout = grabbed.pixelColor(35, 3);
  EXPECT_EQ(beside_dropout.red(), beside_dropout.green());
}

TEST(FramePreviewSurfaceNullBackend, SubmitsASignalFrameThatArrivedAsPlanes) {
  ensureApplication();
  GpuSurfacePolicy::instance().resetForTesting();

  FramePreviewSurface surface;
  surface.setApi(QRhiWidget::Api::Null);
  surface.setTargetRect(QRect(0, 0, 64, 48));
  ASSERT_TRUE(surface.setFramePlanes(makeSignalPlanes(QSize(32, 24))));

  bool failed = false;
  if (!renderOneFrame(surface, failed)) {
    GTEST_SKIP() << "no RHI device on this host";
  }
  EXPECT_FALSE(failed);
  EXPECT_TRUE(GpuSurfacePolicy::instance().planeConversionAvailable());
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
