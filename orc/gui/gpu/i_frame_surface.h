/*
 * File:        i_frame_surface.h
 * Module:      orc-gui
 * Purpose:     The frame surface as orc-gui uses it: the view-support
 *              surface, plus handing the GPU path unconverted planes
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_I_FRAME_SURFACE_H
#define ORC_GUI_I_FRAME_SURFACE_H

#include <orc/view-support/frame_surface.h>
#include <orc/view-support/overlay_primitives.h>

#include <memory>

namespace orc {
struct PreviewPlanes;
}  // namespace orc

namespace orc::gui::gpu {

// The surface, its overlay shapes and the raster path live in
// orc-view-support, so plugin views draw through the same code; the GPU
// surface is orc-gui's own.
using orc::view::IFrameSurface;
using orc::view::OverlayLine;
using orc::view::OverlayPrimitives;
using orc::view::OverlayQuad;
using orc::view::RasterFrameSurface;
using orc::view::scaled;

/**
 * @brief A surface that can take a frame as its unconverted planes.
 *
 * The conversion to display RGB is a pass over every sample, so a render
 * whose only consumer is a graphics device hands the planes over instead and
 * the device's fragment shader finishes the job. Only the GPU surface is one
 * of these: the preview asks with dynamic_cast, and a surface that is not
 * (the raster path) is given a converted image.
 *
 * Kept out of IFrameSurface because the planes are a stage-tier type, which
 * the view-support tier may not name.
 */
class IFramePlanesSurface {
 public:
  virtual ~IFramePlanesSurface() = default;

  /**
   * @brief The frame as the component planes it was decoded into.
   *
   * Replaces whatever frame was set before, image or planes. Shared rather
   * than copied: the payload is three floats per sample of the frame, and it
   * is produced on the render worker for this one consumer.
   *
   * @return False when this surface cannot convert planes now, so the
   *         caller knows it still owes it a converted image.
   */
  virtual bool setFramePlanes(
      std::shared_ptr<const orc::PreviewPlanes> planes) = 0;
};

/// Hand @p planes to @p surface when it can take them.
/// @return False when it cannot (the raster path, or a GPU surface without
///         plane conversion), and a converted image is still owed.
inline bool setFramePlanes(IFrameSurface& surface,
                           std::shared_ptr<const orc::PreviewPlanes> planes) {
  auto* planes_surface = dynamic_cast<IFramePlanesSurface*>(&surface);
  return planes_surface != nullptr &&
         planes_surface->setFramePlanes(std::move(planes));
}

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_I_FRAME_SURFACE_H
