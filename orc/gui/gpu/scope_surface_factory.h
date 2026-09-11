/*
 * File:        scope_surface_factory.h
 * Module:      orc-gui
 * Purpose:     Builds the scope canvas the run-time policy asks for
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_SCOPE_SURFACE_FACTORY_H
#define ORC_GUI_SCOPE_SURFACE_FACTORY_H

#include <memory>

#include "i_scope_surface.h"

class QWidget;

namespace orc::gui::gpu {

/**
 * @brief Build the drawing path for a scope.
 *
 * Asks GpuSurfacePolicy once, when the scope is constructed, so a window's
 * path does not change under it mid-session. A canvas is returned as a child
 * widget of @p owner, which must keep it positioned; the owner keeps its own
 * CPU renderer either way.
 *
 * @return Null when this window plots on the CPU, which is the fallback.
 */
std::unique_ptr<IScopeSurface> createScopeSurface(QWidget* owner);

/**
 * @brief Drop a scope canvas after a run-time render failure.
 *
 * QRhiWidget::renderFailed latches the failure in GpuSurfacePolicy, but the
 * canvas that failed is still in place and will keep failing. Calling this
 * before a repaint removes it, leaving the owner on its CPU renderer; the
 * owner then has to redraw from the acquisition it holds, which is why the
 * removal is reported rather than done silently.
 *
 * @return True when @p surface was dropped.
 */
bool dropScopeSurfaceIfFailed(std::unique_ptr<IScopeSurface>& surface);

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_SCOPE_SURFACE_FACTORY_H
