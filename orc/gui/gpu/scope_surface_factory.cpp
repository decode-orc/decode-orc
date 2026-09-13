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
#include <QWidget>

#include "rhi_window_support.h"
#include "scope_canvas.h"
#endif

namespace orc::gui::gpu {

std::unique_ptr<IScopeSurface> createScopeSurface(QWidget* owner) {
#ifdef ORC_GUI_GPU_RENDER
  if (GpuSurfacePolicy::instance().useGpuSurface() &&
      windowCanAdoptRhiWidget(owner)) {
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
#ifdef ORC_GUI_GPU_RENDER
  // Read before the canvas goes: the window is what has to be handed back,
  // and the canvas is the only way to it from here.
  QWidget* owner = surface->widget() != nullptr
                       ? surface->widget()->parentWidget()
                       : nullptr;
#endif
  surface.reset();
#ifdef ORC_GUI_GPU_RENDER
  // The window was built to composite through the RHI because of the canvas
  // just dropped; without this it would keep trying, and failing, on every
  // flush.
  releaseWindowRhiIfUnused(owner);
#endif
  return true;
}

}  // namespace orc::gui::gpu
