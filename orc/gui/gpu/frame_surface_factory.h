/*
 * File:        frame_surface_factory.h
 * Module:      orc-gui
 * Purpose:     Builds the frame surface the run-time policy asks for
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_FRAME_SURFACE_FACTORY_H
#define ORC_GUI_FRAME_SURFACE_FACTORY_H

#include <memory>

#include "i_frame_surface.h"

class QWidget;

namespace orc::gui::gpu {

/**
 * @brief Build the drawing path for a frame-viewing widget.
 *
 * Asks GpuSurfacePolicy once, when the widget is constructed, so a window's
 * path does not change under it mid-session. A GPU surface is returned as a
 * child widget of @p owner, which must keep it sized to its own rect; the
 * raster surface has no widget and is painted by the owner.
 *
 * Never returns null: the raster path is always available.
 */
std::unique_ptr<IFrameSurface> createFrameSurface(QWidget* owner);

/**
 * @brief Drop a surface to the raster path after a run-time render failure.
 *
 * QRhiWidget::renderFailed latches the failure in GpuSurfacePolicy, but the
 * surface that failed is still in place and will keep failing. Calling this
 * before a repaint replaces it with the raster path; the owner then has to
 * push the frame, target rect, background and overlay into the new surface,
 * which is why the swap is reported rather than done silently.
 *
 * @return True when @p surface was replaced.
 */
bool downgradeSurfaceIfFailed(std::unique_ptr<IFrameSurface>& surface,
                              QWidget* owner);

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_FRAME_SURFACE_FACTORY_H
