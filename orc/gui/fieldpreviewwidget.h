/*
 * File:        fieldpreviewwidget.h
 * Module:      orc-gui
 * Purpose:     Field preview widget
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#ifndef FIELDPREVIEWWIDGET_H
#define FIELDPREVIEWWIDGET_H

#include <orc/stage/preview/orc_rendering.h>  // For public API types

#include <QImage>
#include <QTimer>
#include <QWidget>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "frame_view_geometry.h"
#include "gpu/i_frame_surface.h"

// Forward declarations
namespace orc {
namespace public_api {
struct PreviewImage;
}
}  // namespace orc

/**
 * Widget for displaying rendered previews from orc-core
 *
 * This widget is now a thin display client - all rendering
 * logic is in orc::PreviewRenderer. The widget only:
 * - Displays RGB888 data from core
 * - Handles aspect ratio correction for display
 * - Manages widget sizing
 */
class FieldPreviewWidget : public QWidget {
  Q_OBJECT

 public:
  explicit FieldPreviewWidget(QWidget* parent = nullptr);
  ~FieldPreviewWidget();

  /**
   * @brief Set the rendered image to display
   * @param image PreviewImage from orc::PreviewRenderer
   */
  void setImage(const orc::PreviewImage& image);

  /**
   * @brief Set an already-converted frame
   *
   * The render worker expands the frame for the GPU surface before it hands
   * the render over, so the hot path does no pixel work on the GUI thread.
   * The image keeps whatever format the worker produced.
   *
   * @param image Converted frame image
   * @param dropout_regions The frame's dropouts, for the overlay
   */
  void setImage(const QImage& image,
                const std::vector<orc::DropoutRegion>& dropout_regions);

  /**
   * @brief Clear the display
   */
  void clearImage();

  /**
   * @brief Set the aspect ratio correction for display
   * @param correction Width scaling factor (1.0 = no correction, <1.0 =
   * narrower)
   */
  void setAspectCorrection(double correction);

  /**
   * @brief Get the current original image size (uncorrected)
   * @return Size of the current image, or QSize(0,0) if no image
   */
  QSize originalImageSize() const { return current_image_.size(); }

  /**
   * @brief Get the current aspect correction value
   * @return The aspect correction factor
   */
  double aspectCorrection() const { return geometry_.aspectCorrection(); }

  /**
   * @brief Set whether to show dropout regions
   * @param show True to show dropouts, false to hide
   */
  void setShowDropouts(bool show);

  /**
   * @brief Enable or disable cross-hairs display
   * @param enabled True to allow cross-hairs, false to hide them
   */
  void setCrosshairsEnabled(bool enabled);

  /**
   * @brief Whether cross-hairs display is currently enabled
   */
  bool crosshairsEnabled() const { return crosshairs_enabled_; }

  /**
   * @brief Update cross-hairs position (in image coordinates)
   * @param image_x X coordinate in image space
   * @param image_y Y coordinate in image space
   */
  void updateCrosshairsPosition(int image_x, int image_y);

  QSize sizeHint() const override;

 signals:
  /**
   * @brief Emitted when user clicks on a line in the preview
   * @param image_x X coordinate in image space (0 to width-1)
   * @param image_y Y coordinate in image space (0 to height-1)
   */
  void lineClicked(int image_x, int image_y);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void resizeEvent(QResizeEvent* event) override;
  void mouseMoveEvent(QMouseEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void changeEvent(QEvent* event) override;

 private:
  /// Refresh the shared display geometry after image/aspect/size changes.
  void updateViewGeometry();

  /// Rebuild the overlay primitives the surface draws over the frame.
  void rebuildOverlay();

  /// The image pixel the cross-hairs sit on, when they are showing.
  std::optional<QPoint> crosshairPixel() const;

  /// Hand the widget's background colour to the surface.
  void applyBackgroundColor();

  /// Keep a GPU surface child filling the widget.
  void applySurfaceGeometry();

  /// Repaint, first dropping to the raster path if the GPU surface has failed
  /// at run time.
  void refreshSurface();

  /// Where the frame is drawn, and its overlays: either QPainter in this
  /// widget's paintEvent or a QRhiWidget child. Chosen once, at construction.
  std::unique_ptr<orc::gui::gpu::IFrameSurface> surface_;

  QImage current_image_;
  // Shared display geometry (fit-to-widget: zoom is always fitZoom())
  orc::gui::FrameViewGeometry geometry_;
  std::vector<orc::DropoutRegion> dropout_regions_;
  bool show_dropouts_ = false;
  QPoint mouse_pos_;         // Current mouse position
  bool mouse_over_ = false;  // Whether mouse is over the widget
  QRect image_rect_;         // Where the image is drawn in widget coordinates

  // Cross-hairs locking
  bool crosshairs_enabled_ =
      false;  // Whether cross-hairs are enabled (line scope open)
  // Locked cross-hairs position in IMAGE pixels (not widget coordinates), so
  // the cross-hairs track any rescaling of the preview — widget resize,
  // aspect-ratio change — by remapping at paint time.
  std::optional<QPoint> locked_crosshairs_image_;

  // Line scope update throttling
  QTimer* line_scope_update_timer_;
  QPoint pending_line_scope_pos_;
  bool line_scope_update_pending_ = false;
  bool mouse_button_pressed_ = false;

 private slots:
  void onLineScopeUpdateTimer();
};

#endif  // FIELDPREVIEWWIDGET_H
