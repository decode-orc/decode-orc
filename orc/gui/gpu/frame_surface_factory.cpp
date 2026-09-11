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
#include "raster_frame_surface.h"

#ifdef ORC_GUI_GPU_RENDER
#include "frame_preview_surface.h"
#endif

namespace orc::gui::gpu {

std::unique_ptr<IFrameSurface> createFrameSurface(QWidget* owner) {
#ifdef ORC_GUI_GPU_RENDER
  if (GpuSurfacePolicy::instance().useGpuSurface()) {
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
  return true;
}

}  // namespace orc::gui::gpu
