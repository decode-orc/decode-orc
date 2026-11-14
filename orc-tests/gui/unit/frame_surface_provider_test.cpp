/*
 * File:        frame_surface_provider_test.cpp
 * Module:      orc-tests/gui/unit
 * Purpose:     orc-view-support's frame surface hook: the raster path
 *              without a provider, and the host's choice with one
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>
#include <orc/view-support/frame_surface.h>

#include <memory>

namespace {

using orc::view::IFrameSurface;
using orc::view::RasterFrameSurface;

// Stands in for orc-gui's provider; its surfaces are raster ones it can
// recognise again.
class FakeProvider final : public orc::view::IFrameSurfaceProvider {
 public:
  std::unique_ptr<IFrameSurface> create(QWidget* owner) override {
    ++created;
    auto surface = std::make_unique<RasterFrameSurface>(owner);
    made = surface.get();
    return surface;
  }
  bool hasFailed(IFrameSurface& surface) override {
    return failed && &surface == made;
  }
  bool replaceIfFailed(std::unique_ptr<IFrameSurface>& surface,
                       QWidget* owner) override {
    if (!hasFailed(*surface)) return false;
    surface = std::make_unique<RasterFrameSurface>(owner);
    made = nullptr;
    return true;
  }

  int created = 0;
  bool failed = false;
  IFrameSurface* made = nullptr;
};

// Every test leaves the process without a provider.
class FrameSurfaceProviderTest : public ::testing::Test {
 protected:
  void TearDown() override { orc::view::setFrameSurfaceProvider(nullptr); }
};

TEST_F(FrameSurfaceProviderTest, WithoutAProviderEverySurfaceIsRaster) {
  auto surface = orc::view::createFrameSurface(nullptr);
  ASSERT_NE(surface, nullptr);
  EXPECT_NE(dynamic_cast<RasterFrameSurface*>(surface.get()), nullptr);
  EXPECT_EQ(surface->widget(), nullptr);
  EXPECT_FALSE(orc::view::frameSurfaceHasFailed(*surface));
  EXPECT_FALSE(orc::view::replaceFailedFrameSurface(surface, nullptr));
}

TEST_F(FrameSurfaceProviderTest, AProviderMakesTheSurfaces) {
  FakeProvider provider;
  orc::view::setFrameSurfaceProvider(&provider);

  auto surface = orc::view::createFrameSurface(nullptr);

  EXPECT_EQ(provider.created, 1);
  EXPECT_EQ(surface.get(), provider.made);
}

TEST_F(FrameSurfaceProviderTest, AFailedSurfaceIsReplacedOnce) {
  FakeProvider provider;
  orc::view::setFrameSurfaceProvider(&provider);
  auto surface = orc::view::createFrameSurface(nullptr);
  IFrameSurface* first = surface.get();

  EXPECT_FALSE(orc::view::frameSurfaceHasFailed(*surface));
  provider.failed = true;
  EXPECT_TRUE(orc::view::frameSurfaceHasFailed(*surface));

  EXPECT_TRUE(orc::view::replaceFailedFrameSurface(surface, nullptr));
  EXPECT_NE(surface.get(), first);
  EXPECT_FALSE(orc::view::replaceFailedFrameSurface(surface, nullptr));
}

}  // namespace
