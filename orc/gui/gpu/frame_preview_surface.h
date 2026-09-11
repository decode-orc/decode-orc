/*
 * File:        frame_preview_surface.h
 * Module:      orc-gui
 * Purpose:     Qt RHI implementation of the frame surface seam
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_FRAME_PREVIEW_SURFACE_H
#define ORC_GUI_FRAME_PREVIEW_SURFACE_H

#include <QRhiWidget>
#include <memory>

#include "i_frame_surface.h"

QT_BEGIN_NAMESPACE
class QRhi;
class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
QT_END_NAMESPACE

namespace orc::gui::gpu {

/**
 * @brief The frame and its overlays drawn through Qt's RHI.
 *
 * A child widget of the frame-viewing widget that owns it, sized to the
 * owner's rect and transparent to mouse events so the owner keeps every
 * interaction it had. Three pipelines share one render pass: a textured quad
 * for the frame, a triangle list for overlay quads and a line list for
 * overlay hairlines - in that order, which is the order the raster path
 * paints them.
 *
 * The frame texture is reallocated only when the frame's dimensions change,
 * so a mouse move that only moves the cross-hairs costs one small vertex
 * upload and one draw, with no pixel work at all.
 *
 * All the RHI use in the application is here and in the scope canvas: Qt's
 * RHI classes promise source compatibility only, so they are kept behind
 * IFrameSurface rather than spread through the widgets.
 *
 * Thread safety: GUI thread only.
 */
class FramePreviewSurface final : public QRhiWidget, public IFrameSurface {
  Q_OBJECT

 public:
  explicit FramePreviewSurface(QWidget* parent = nullptr);
  ~FramePreviewSurface() override;

  void setFrameImage(const QImage& image) override;
  void setTargetRect(const QRect& rect) override;
  void setBackgroundColor(const QColor& color) override;
  void setOverlay(OverlayPrimitives primitives) override;
  void setSmoothScaling(bool smooth) override;
  void refresh() override;
  QWidget* widget() override { return this; }
  void paint(QPainter& painter, const QRect& damaged) override;

  /**
   * @brief Build every shader, buffer and pipeline against a caller's target.
   *
   * What initialize() does, with the render pass supplied rather than taken
   * from the widget's own target. Exposed so a test can build the pipelines
   * on QRhi::Null with an offscreen target, which is the only way a headless
   * host can check that the shaders load and the vertex and binding layouts
   * the pipelines declare are accepted.
   *
   * @return False if any resource could not be created.
   */
  bool buildResourcesForTesting(QRhi* rhi, QRhiRenderPassDescriptor* pass,
                                int sample_count);

 protected:
  void initialize(QRhiCommandBuffer* cb) override;
  void render(QRhiCommandBuffer* cb) override;
  void releaseResources() override;

 private:
  /// Build the pipelines and their fixed resources. False when a shader or a
  /// pipeline could not be created, which drops the window to the raster path.
  bool createPipelines(QRhiRenderPassDescriptor* pass, int sample_count);

  /// (Re)allocate the frame texture when the image dimensions change.
  bool ensureTexture();

  /// Vertices for the frame quad, in render-target pixels.
  void buildFrameVertices(std::vector<float>& out) const;

  /// Vertices for the overlay quads and lines, in render-target pixels.
  void buildOverlayVertices(std::vector<float>& quads,
                            std::vector<float>& lines) const;

  QRhi* rhi_ = nullptr;

  std::unique_ptr<QRhiBuffer> uniform_buffer_;
  std::unique_ptr<QRhiBuffer> frame_vertex_buffer_;
  std::unique_ptr<QRhiBuffer> overlay_vertex_buffer_;
  std::unique_ptr<QRhiTexture> frame_texture_;
  std::unique_ptr<QRhiSampler> linear_sampler_;
  std::unique_ptr<QRhiSampler> nearest_sampler_;
  std::unique_ptr<QRhiShaderResourceBindings> frame_bindings_linear_;
  std::unique_ptr<QRhiShaderResourceBindings> frame_bindings_nearest_;
  std::unique_ptr<QRhiShaderResourceBindings> overlay_bindings_;
  std::unique_ptr<QRhiGraphicsPipeline> frame_pipeline_;
  std::unique_ptr<QRhiGraphicsPipeline> overlay_quad_pipeline_;
  std::unique_ptr<QRhiGraphicsPipeline> overlay_line_pipeline_;

  /// Capacity the overlay vertex buffer was created with, in bytes. The
  /// buffer is grown, never shrunk: overlay counts oscillate frame to frame
  /// as regions are hovered, and reallocating on every change would stall.
  quint32 overlay_buffer_capacity_ = 0;

  QImage frame_;
  QSize uploaded_size_;
  bool frame_dirty_ = false;
  QRect target_rect_;
  QColor background_ = Qt::black;
  OverlayPrimitives overlay_;
  bool smooth_ = true;
  bool pipelines_failed_ = false;
};

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_FRAME_PREVIEW_SURFACE_H
