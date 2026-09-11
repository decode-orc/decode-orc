/*
 * File:        i_scope_surface.h
 * Module:      orc-gui
 * Purpose:     Seam between a scope dialogue and its GPU canvas
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_I_SCOPE_SURFACE_H
#define ORC_GUI_I_SCOPE_SURFACE_H

#include <QColor>

#include "scope_frame.h"

class QWidget;

namespace orc::gui::gpu {

/**
 * @brief A canvas a scope hands accumulated vertices to.
 *
 * Unlike IFrameSurface there is no raster implementation of this: a scope that
 * is not on the canvas keeps its own CPU renderer, which produces the plot the
 * canvas is checked against. The absence of a surface - createScopeSurface()
 * returning null - is the fallback.
 *
 * Thread safety: GUI thread only.
 */
class IScopeSurface {
 public:
  virtual ~IScopeSurface() = default;

  /// Replace everything the canvas draws. Does not repaint on its own.
  virtual void setFrame(ScopeFrame frame) = 0;

  /// Colour behind the plot, where the canvas does not reach.
  virtual void setBackgroundColor(const QColor& color) = 0;

  /// Ask for a repaint of the frame last set.
  virtual void refresh() = 0;

  /// The child widget that draws, for the owner to position.
  virtual QWidget* widget() = 0;
};

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_I_SCOPE_SURFACE_H
