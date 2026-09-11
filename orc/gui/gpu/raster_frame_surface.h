/*
 * File:        raster_frame_surface.h
 * Module:      orc-gui
 * Purpose:     QPainter implementation of the frame surface seam
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_RASTER_FRAME_SURFACE_H
#define ORC_GUI_RASTER_FRAME_SURFACE_H

#include "i_frame_surface.h"

class QWidget;

namespace orc::gui::gpu {

/**
 * @brief The frame surface drawn with QPainter, in the owner's paintEvent.
 *
 * Always available, and always built: it is the fallback for every reason
 * GpuSurfacePolicy can give for not using the GPU, and it is what the
 * offscreen widget tests run against.
 *
 * It holds no widget of its own - the owner paints it - so that the raster
 * path stays exactly the repaint it was before the surface seam existed.
 */
class RasterFrameSurface final : public IFrameSurface {
 public:
  /// @param owner Widget whose update() is called by refresh()
  explicit RasterFrameSurface(QWidget* owner);

  void setFrameImage(const QImage& image) override;
  bool setFramePlanes(
      std::shared_ptr<const orc::PreviewPlanes> planes) override;
  void setTargetRect(const QRect& rect) override;
  void setBackgroundColor(const QColor& color) override;
  void setOverlay(OverlayPrimitives primitives) override;
  void setSmoothScaling(bool smooth) override;
  void refresh() override;
  QWidget* widget() override { return nullptr; }
  void paint(QPainter& painter, const QRect& damaged) override;

 private:
  QWidget* owner_ = nullptr;
  QImage frame_;
  QRect target_rect_;
  QColor background_ = Qt::black;
  OverlayPrimitives overlay_;
  bool smooth_ = true;
};

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_RASTER_FRAME_SURFACE_H
