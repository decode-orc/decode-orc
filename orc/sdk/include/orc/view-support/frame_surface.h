/*
 * File:        frame_surface.h
 * Module:      decode-orc Plugin SDK (view-support tier)
 * Purpose:     What a frame viewer draws through: the raster surface, and
 *              the GPU surface the host may provide in its place
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 *
 * STABILITY: PUBLIC — This header is part of the stable plugin SDK.
 */

#pragma once

// SDK TIER: view-support — see <orc/view-support/view_support_export.h>.

#include <orc/view-support/overlay_primitives.h>
#include <orc/view-support/view_support_export.h>

#include <QColor>
#include <QImage>
#include <QRect>
#include <memory>

class QPainter;
class QWidget;

namespace orc::view {

/**
 * @brief What a frame-viewing widget hands its drawing path.
 *
 * A frame viewer does the same three things whatever draws it: put a frame
 * image somewhere in its rect, fill the rest with the background, and draw
 * overlays over the top. This interface is that, and nothing else - the
 * widget keeps every mouse, zoom and selection decision it owns.
 *
 * Two implementations: RasterFrameSurface paints with QPainter into the
 * owner's own paintEvent, and orc-gui's GPU surface is a QRhiWidget child
 * that draws the same thing on the GPU. Which one a widget gets is the
 * host's choice (IFrameSurfaceProvider), made once when it is constructed.
 *
 * Coordinates are the owner widget's own, in logical pixels; the GPU path
 * converts to device pixels itself.
 *
 * Thread safety: GUI thread only.
 */
class ORC_VIEW_SUPPORT_EXPORT IFrameSurface {
 public:
  virtual ~IFrameSurface();

  /// The frame to display. A null image clears it.
  virtual void setFrameImage(const QImage& image) = 0;

  /// Where the frame is drawn, from FrameViewGeometry::targetRect().
  virtual void setTargetRect(const QRect& rect) = 0;

  /// Fill for everything the frame does not cover.
  virtual void setBackgroundColor(const QColor& color) = 0;

  /// This frame's overlays. Replaces whatever was set before.
  virtual void setOverlay(OverlayPrimitives primitives) = 0;

  /// Linear filtering when rescaling (the default, matching the smooth
  /// transform the preview has always used), or nearest for 1:1 inspection.
  virtual void setSmoothScaling(bool smooth) = 0;

  /// Ask for a repaint.
  virtual void refresh() = 0;

  /**
   * @brief The widget that draws, when the path has one of its own.
   *
   * The GPU path returns its QRhiWidget, which the owner must keep sized to
   * its own rect. The raster path returns nullptr, meaning "you paint me":
   * the owner calls paint() from its paintEvent.
   */
  virtual QWidget* widget() = 0;

  /// Draw into the owner's painter, clipped to @p damaged. Does nothing on
  /// the GPU path, which draws in its own child widget.
  virtual void paint(QPainter& painter, const QRect& damaged) = 0;
};

/**
 * @brief The frame surface drawn with QPainter, in the owner's paintEvent.
 *
 * Always available: it is what a widget gets when the host provides no GPU
 * surface or cannot use one, and it is what the offscreen widget tests run
 * against.
 *
 * It holds no widget of its own - the owner paints it - so that the raster
 * path stays exactly the repaint it was before the surface seam existed.
 */
class ORC_VIEW_SUPPORT_EXPORT RasterFrameSurface final : public IFrameSurface {
 public:
  /// @param owner Widget whose update() is called by refresh()
  explicit RasterFrameSurface(QWidget* owner);
  ~RasterFrameSurface() override;

  void setFrameImage(const QImage& image) override;
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

/**
 * @brief Where frame surfaces come from, set by the host
 *
 * orc-gui installs one at start-up that builds its GPU surface when the
 * graphics device allows it. A view never implements this; without a
 * provider (a unit test, say) every widget gets the raster surface.
 */
class ORC_VIEW_SUPPORT_EXPORT IFrameSurfaceProvider {
 public:
  virtual ~IFrameSurfaceProvider();

  /// A surface for @p owner; never null. A GPU surface is a child widget of
  /// @p owner, which must keep it sized to its own rect.
  virtual std::unique_ptr<IFrameSurface> create(QWidget* owner) = 0;

  /// True when @p surface has failed at run time and will keep failing, so
  /// its owner should hide it and ask for a replacement.
  virtual bool hasFailed(IFrameSurface& surface) = 0;

  /// Replace @p surface, if it has failed, with one that works.
  /// @return True when it was replaced; the owner then hands the new surface
  ///         its frame, target rect, background and overlay.
  virtual bool replaceIfFailed(std::unique_ptr<IFrameSurface>& surface,
                               QWidget* owner) = 0;
};

/// Install the host's provider, or nullptr to go back to the raster path.
/// The provider must outlive every widget made while it is installed.
ORC_VIEW_SUPPORT_EXPORT void setFrameSurfaceProvider(
    IFrameSurfaceProvider* provider);

/// A surface for @p owner from the installed provider, or the raster surface
/// when there is none. Never null.
ORC_VIEW_SUPPORT_EXPORT std::unique_ptr<IFrameSurface> createFrameSurface(
    QWidget* owner);

/// IFrameSurfaceProvider::hasFailed() of the installed provider; false
/// without one.
ORC_VIEW_SUPPORT_EXPORT bool frameSurfaceHasFailed(IFrameSurface& surface);

/// IFrameSurfaceProvider::replaceIfFailed() of the installed provider; false
/// without one.
ORC_VIEW_SUPPORT_EXPORT bool replaceFailedFrameSurface(
    std::unique_ptr<IFrameSurface>& surface, QWidget* owner);

}  // namespace orc::view
