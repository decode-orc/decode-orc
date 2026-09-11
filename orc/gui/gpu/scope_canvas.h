/*
 * File:        scope_canvas.h
 * Module:      orc-gui
 * Purpose:     Qt RHI canvas for the vectorscope and waveform monitor
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_GUI_SCOPE_CANVAS_H
#define ORC_GUI_SCOPE_CANVAS_H

#include <QRhiWidget>
#include <memory>
#include <vector>

#include "i_scope_surface.h"

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
 * @brief A scope's trace drawn through Qt's RHI, in two passes.
 *
 * Pass one accumulates the vertices into an off-screen floating-point canvas,
 * letting the blend unit do what the CPU renderers do with a count buffer:
 * sum what lands on a pixel, or keep the largest of what does. Points and
 * lines share the pass, so a plot that joins its samples costs one extra draw
 * rather than a Bresenham rasteriser on the GUI thread.
 *
 * When the plot's brightness is read as dwell rather than as a count, three
 * more stages sit between them: a separable Gaussian beam spot, a reduction
 * chain that carries the plot's brightest level and its total charge down to
 * a single texel, and a charge histogram scattered by the rasteriser and
 * walked down to the level above which half the charge sits. That last figure
 * is the brightness anchor, and computing it on the device is what lets the
 * composite plot leave the processor at all: it is a reduction over the whole
 * plot, and reading the plot back to make it would stall the frame.
 *
 * Pass two maps counts to pixels - brightness, colour, and the composition
 * between the graticule behind the trace and the one in front of it - and
 * scales the canvas into the widget. Because it reads a texture rather than
 * redrawing, resizing the window costs no accumulation at all.
 *
 * Graticules, grids, axes and level markers stay with the QPainter code that
 * has always drawn them: they are handed over as the underlay and overlay
 * images of a ScopeFrame, and the canvas re-uploads one only when the image
 * it was given is not the one it already holds.
 *
 * Thread safety: GUI thread only.
 */
class ScopeCanvas final : public QRhiWidget, public IScopeSurface {
  Q_OBJECT

 public:
  explicit ScopeCanvas(QWidget* parent = nullptr);
  ~ScopeCanvas() override;

  void setFrame(ScopeFrame frame) override;
  void setBackgroundColor(const QColor& color) override;
  void refresh() override;
  QWidget* widget() override { return this; }

  /**
   * @brief Build every shader, buffer and pipeline against a caller's target.
   *
   * What initialize() does, with the render pass supplied rather than taken
   * from the widget's own target. Exposed so a test can build the pipelines
   * on QRhi::Null with an off-screen target, which is the only way a headless
   * host can check that the shaders load and that the vertex and binding
   * layouts the pipelines declare are accepted.
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
  /// pipeline could not be created, which drops the window to the CPU path.
  bool createPipelines(QRhiRenderPassDescriptor* pass, int sample_count);

  /// (Re)allocate the accumulation target when the canvas size changes.
  bool ensureCanvas();

  /// (Re)build the reduction chain for the current canvas size.
  bool ensureReductionChain();

  /// Spread, reduce, scatter and threshold: everything between accumulating
  /// the plot and mapping it that only a dwell-read plot needs.
  void recordDwellPasses(QRhiCommandBuffer* cb);

  /// Fill the uniform blocks the dwell passes read.
  void updateDwellUniforms(QRhiResourceUpdateBatch* updates);

  /// True when this frame's plot is read as dwell and can be.
  bool dwellPassesWanted() const;

  /// (Re)upload an underlay or overlay image that is not the one held.
  bool ensureImageTexture(const QImage& image,
                          std::unique_ptr<QRhiTexture>& texture,
                          qint64& held_key, QRhiResourceUpdateBatch* updates);

  /// Point both map binding sets at the textures currently allocated.
  void refreshMapBindings();

  /// Where in the widget, in device pixels, the canvas is drawn.
  QRectF contentRect() const;

  /// Grow a vertex buffer to hold @p bytes, never shrinking it.
  bool ensureVertexBuffer(std::unique_ptr<QRhiBuffer>& buffer,
                          quint32& capacity, quint32 bytes);

  QRhi* rhi_ = nullptr;

  /// One uniform block per trace channel: the samples are the same buffer
  /// whether they are being landed on or traced through, and only which
  /// channel they deposit in differs.
  std::unique_ptr<QRhiBuffer> accumulate_uniforms_points_;
  std::unique_ptr<QRhiBuffer> accumulate_uniforms_strips_;
  std::unique_ptr<QRhiBuffer> map_uniforms_;
  std::unique_ptr<QRhiBuffer> point_buffer_;
  std::unique_ptr<QRhiBuffer> map_buffer_;
  quint32 point_capacity_ = 0;

  std::unique_ptr<QRhiTexture> accumulation_;
  std::unique_ptr<QRhiTextureRenderTarget> accumulation_target_;
  std::unique_ptr<QRhiRenderPassDescriptor> accumulation_pass_;
  std::unique_ptr<QRhiTexture> underlay_;
  std::unique_ptr<QRhiTexture> overlay_;

  // ---- Dwell stages ------------------------------------------------------
  /// Half of the separable spread: the horizontal pass writes here and the
  /// vertical one reads it back into the accumulation.
  std::unique_ptr<QRhiTexture> spread_scratch_;
  std::unique_ptr<QRhiTextureRenderTarget> spread_scratch_target_;
  std::unique_ptr<QRhiBuffer> spread_uniforms_horizontal_;
  std::unique_ptr<QRhiBuffer> spread_uniforms_vertical_;
  std::unique_ptr<QRhiShaderResourceBindings> spread_bindings_horizontal_;
  std::unique_ptr<QRhiShaderResourceBindings> spread_bindings_vertical_;
  std::unique_ptr<QRhiGraphicsPipeline> spread_pipeline_;

  /// One level per division by four, ending at a single texel holding the
  /// plot's brightest level and its total charge.
  struct ReductionLevel {
    std::unique_ptr<QRhiTexture> texture;
    std::unique_ptr<QRhiTextureRenderTarget> target;
    std::unique_ptr<QRhiBuffer> uniforms;
    std::unique_ptr<QRhiShaderResourceBindings> bindings;
    QSize size;
    QSize source_size;
    bool first = false;
  };
  std::vector<ReductionLevel> reduction_;
  std::unique_ptr<QRhiRenderPassDescriptor> reduction_pass_;
  std::unique_ptr<QRhiGraphicsPipeline> reduction_pipeline_;

  std::unique_ptr<QRhiTexture> histogram_;
  std::unique_ptr<QRhiTextureRenderTarget> histogram_target_;
  std::unique_ptr<QRhiBuffer> histogram_uniforms_;
  std::unique_ptr<QRhiShaderResourceBindings> histogram_bindings_;
  std::unique_ptr<QRhiGraphicsPipeline> histogram_pipeline_;

  std::unique_ptr<QRhiTexture> anchor_;
  std::unique_ptr<QRhiTextureRenderTarget> anchor_target_;
  std::unique_ptr<QRhiBuffer> anchor_uniforms_;
  std::unique_ptr<QRhiShaderResourceBindings> anchor_bindings_;
  std::unique_ptr<QRhiGraphicsPipeline> anchor_pipeline_;
  std::unique_ptr<QRhiRenderPassDescriptor> single_channel_pass_;

  std::unique_ptr<QRhiSampler> image_sampler_;
  std::unique_ptr<QRhiSampler> trace_linear_;
  std::unique_ptr<QRhiSampler> trace_nearest_;

  std::unique_ptr<QRhiShaderResourceBindings> accumulate_bindings_points_;
  std::unique_ptr<QRhiShaderResourceBindings> accumulate_bindings_strips_;
  std::unique_ptr<QRhiShaderResourceBindings> map_bindings_smooth_;
  std::unique_ptr<QRhiShaderResourceBindings> map_bindings_sharp_;

  std::unique_ptr<QRhiGraphicsPipeline> point_pipeline_add_;
  std::unique_ptr<QRhiGraphicsPipeline> point_pipeline_max_;
  std::unique_ptr<QRhiGraphicsPipeline> strip_pipeline_;
  std::unique_ptr<QRhiGraphicsPipeline> map_pipeline_;

  ScopeFrame frame_;
  QSize allocated_canvas_;
  /// QImage::cacheKey() of the image each texture was filled from, so a
  /// dialogue that hands back the graticule it drew last time uploads nothing.
  qint64 underlay_key_ = 0;
  qint64 overlay_key_ = 0;
  bool vertices_dirty_ = true;

  QColor background_ = Qt::black;
  bool pipelines_failed_ = false;
};

}  // namespace orc::gui::gpu

#endif  // ORC_GUI_SCOPE_CANVAS_H
