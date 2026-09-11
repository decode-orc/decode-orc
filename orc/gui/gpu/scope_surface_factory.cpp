/*
 * File:        scope_surface_factory.cpp
 * Module:      orc-gui
 * Purpose:     Builds the scope canvas the run-time policy asks for
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "scope_surface_factory.h"

#include "gpu_surface_policy.h"

#ifdef ORC_GUI_GPU_RENDER
#include "scope_canvas.h"
#endif

namespace orc::gui::gpu {

std::unique_ptr<IScopeSurface> createScopeSurface(QWidget* owner) {
#ifdef ORC_GUI_GPU_RENDER
  if (GpuSurfacePolicy::instance().useGpuSurface()) {
    // Parented so Qt lays it out and clips it, but owned by the returned
    // pointer: the owner's members are destroyed before ~QWidget reaches its
    // children, and a QWidget removes itself from its parent when deleted.
    return std::make_unique<ScopeCanvas>(owner);
  }
#else
  Q_UNUSED(owner);
#endif
  return nullptr;
}

bool dropScopeSurfaceIfFailed(std::unique_ptr<IScopeSurface>& surface) {
  if (!surface || GpuSurfacePolicy::instance().useGpuSurface()) {
    return false;
  }
  surface.reset();
  return true;
}

}  // namespace orc::gui::gpu
