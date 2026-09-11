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

#include <orc/stage/preview/orc_rendering.h>

#include <QRhiWidget>
#include <array>
#include <memory>
#include <vector>

#include "i_frame_surface.h"

QT_BEGIN_NAMESPACE
class QRhi;
class QRhiBuffer;
class QRhiGraphicsPipeline;
class QRhiRenderPassDescriptor;
class QRhiResourceUpdateBatch;
class QRhiSampler;
class QRhiShaderResourceBindings;
class QRhiTexture;
class QRhiTextureRenderTarget;
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
 * A frame that arrived as component planes rather than as an image is first
 * converted by a pass of its own into the same texture the quad samples, so
 * everything downstream of it - filtering, overlays, geometry - is unchanged
 * by which representation the render worker sent.
 *
 * The frame texture is reallocated only when the frame's dimensions change,
 * so a mouse move that only moves the cross-hairs costs one small vertex
 * upload and one draw, with no pixel work at all - and no conversion either,
 * which runs only when a new frame has arrived.
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
  bool setFramePlanes(
      std::shared_ptr<const orc::PreviewPlanes> planes) override;
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

  /// Build the plane-conversion pass's own resources. Separate because a
  /// device without floating-point sampled textures can still draw frames
  /// that arrive already converted; only the plane path is lost.
  bool createPlaneResources();

  /// The frame's dimensions, whichever representation it arrived in.
  QSize frameSize() const;

  /// (Re)allocate the frame texture when the image dimensions change. The
  /// texture doubles as the conversion pass's render target on the plane
  /// path, so a change of path reallocates it too.
  bool ensureTexture();

  /// (Re)allocate the three plane textures for the current frame size.
  bool ensurePlaneTextures();

  /// Point the conversion bindings at the textures currently allocated.
  void describePlaneBindings();

  /// Drop everything the plane path owns, leaving the image path intact.
  void releasePlaneResources();

  /// Give up on converting planes for the rest of the session, so that the
  /// next render is asked for an already-converted image instead.
  void abandonPlaneConversion(const char* context);

  /// Upload this frame's planes and convert them into the frame texture.
  /// Does nothing when the frame did not arrive as planes.
  void convertPlanes(QRhiCommandBuffer* cb, QRhiResourceUpdateBatch*& updates);

  /// The dimmed bands and dropout bands of a plane frame, as quads in the
  /// frame's own pixels. Drawn inside the conversion pass, where they land on
  /// the image before it is rescaled - which is where the CPU renderer puts
  /// them too.
  void buildPlaneOverlayVertices(std::vector<float>& out) const;

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

  // ---- Plane conversion --------------------------------------------------
  // Three component planes and the transfer table, converted into the frame
  // texture by one pass at the frame's own resolution. Running it there
  // rather than at the widget's means the filtering, the overlays and the
  // quad below all see exactly what the CPU conversion would have produced.
  std::unique_ptr<QRhiBuffer> plane_uniform_buffer_;
  std::array<std::unique_ptr<QRhiTexture>, 3> plane_textures_;
  std::unique_ptr<QRhiTexture> transfer_texture_;
  std::unique_ptr<QRhiSampler> plane_sampler_;
  std::unique_ptr<QRhiShaderResourceBindings> plane_bindings_;
  std::unique_ptr<QRhiGraphicsPipeline> plane_pipeline_;
  std::unique_ptr<QRhiGraphicsPipeline> signal_plane_pipeline_;
  /// The mask and the dropout bands, drawn over the converted frame inside
  /// the same pass, in the frame's own pixels.
  std::unique_ptr<QRhiBuffer> plane_overlay_uniform_buffer_;
  std::unique_ptr<QRhiBuffer> plane_overlay_vertex_buffer_;
  std::unique_ptr<QRhiShaderResourceBindings> plane_overlay_bindings_;
  std::unique_ptr<QRhiGraphicsPipeline> plane_overlay_pipeline_;
  quint32 plane_overlay_capacity_ = 0;
  std::unique_ptr<QRhiTextureRenderTarget> convert_target_;
  std::unique_ptr<QRhiRenderPassDescriptor> convert_pass_;
  /// Size the plane textures were created at, which the frame texture's own
  /// size can outlive when a smaller frame arrives.
  QSize plane_texture_size_;
  /// The table already uploaded, recognised by identity: the payload shares
  /// one immutable table per transfer characteristic, so a playing preview
  /// hands over the same pointer every frame and uploads nothing.
  std::shared_ptr<const std::vector<float>> uploaded_transfer_lut_;
  QSize transfer_texture_size_;
  bool plane_resources_failed_ = false;

  QImage frame_;
  /// The frame when it arrived as planes - shared with whoever produced it,
  /// never copied. Held for as long as the image on the other path is,
  /// because a device lost and rebuilt has to be able to convert it again.
  std::shared_ptr<const orc::PreviewPlanes> planes_;
  bool planes_dirty_ = false;
  QSize uploaded_size_;
  /// True when the frame texture was created as the conversion pass's
  /// target, which needs a flag an uploaded texture does not carry.
  bool texture_is_convert_target_ = false;
  bool frame_dirty_ = false;
  QRect target_rect_;
  QColor background_ = Qt::black;
  OverlayPrimitives overlay_;
  bool smooth_ = true;
  bool pipelines_failed_ = false;
};

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_FRAME_PREVIEW_SURFACE_H
