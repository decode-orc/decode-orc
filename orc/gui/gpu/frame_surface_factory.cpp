/*
 * File:        frame_surface_factory.cpp
 * Module:      orc-gui
 * Purpose:     Builds the frame surface the run-time policy asks for
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "frame_surface_factory.h"

#include "gpu_surface_policy.h"

#ifdef ORC_GUI_GPU_RENDER
#include "frame_preview_surface.h"
#include "rhi_window_support.h"
#endif

namespace orc::gui::gpu {

std::unique_ptr<IFrameSurface> createFrameSurface(QWidget* owner) {
#ifdef ORC_GUI_GPU_RENDER
  if (GpuSurfacePolicy::instance().useGpuSurface() &&
      windowCanAdoptRhiWidget(owner)) {
    // Parented so Qt lays it out and clips it, but owned by the returned
    // pointer: the owner's members are destroyed before ~QWidget reaches its
    // children, and a QWidget removes itself from its parent when deleted.
    return std::make_unique<FramePreviewSurface>(owner);
  }
#endif
  return std::make_unique<RasterFrameSurface>(owner);
}

bool downgradeSurfaceIfFailed(std::unique_ptr<IFrameSurface>& surface,
                              QWidget* owner) {
  if (!surface || surface->widget() == nullptr) {
    return false;  // Already the raster path.
  }
  if (GpuSurfacePolicy::instance().useGpuSurface()) {
    return false;
  }
  surface = std::make_unique<RasterFrameSurface>(owner);
#ifdef ORC_GUI_GPU_RENDER
  // The window was built to composite through the RHI because of the surface
  // just dropped; without this it would keep trying, and failing, on every
  // flush.
  releaseWindowRhiIfUnused(owner);
#endif
  return true;
}

namespace {

class PolicyFrameSurfaceProvider final
    : public orc::view::IFrameSurfaceProvider {
 public:
  std::unique_ptr<IFrameSurface> create(QWidget* owner) override {
    return createFrameSurface(owner);
  }

  bool hasFailed(IFrameSurface& surface) override {
    // Only a GPU surface has a widget of its own, and the policy latches the
    // first run-time render failure.
    return surface.widget() != nullptr &&
           !GpuSurfacePolicy::instance().useGpuSurface();
  }

  bool replaceIfFailed(std::unique_ptr<IFrameSurface>& surface,
                       QWidget* owner) override {
    return downgradeSurfaceIfFailed(surface, owner);
  }
};

}  // namespace

void installFrameSurfaceProvider() {
  static PolicyFrameSurfaceProvider provider;
  orc::view::setFrameSurfaceProvider(&provider);
}

}  // namespace orc::gui::gpu
