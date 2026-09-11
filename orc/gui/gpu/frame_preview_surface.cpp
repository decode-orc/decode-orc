/*
 * File:        frame_preview_surface.cpp
 * Module:      orc-gui
 * Purpose:     Qt RHI implementation of the frame surface seam
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "frame_preview_surface.h"

#include <rhi/qrhi.h>

#include <QFile>
#include <QMatrix4x4>
#include <QPainter>
#include <cstddef>
#include <vector>

#include "../logging.h"
#include "gpu_surface_policy.h"

namespace orc::gui::gpu {

namespace {

/// Floats per vertex: x, y, u, v.
constexpr int kFrameVertexFloats = 4;
/// Floats per vertex: x, y, r, g, b, a.
constexpr int kOverlayVertexFloats = 6;
/// Bytes in the shared uniform block (one mat4).
constexpr quint32 kUniformBlockSize = 64;

QShader loadShader(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    ORC_LOG_ERROR("GPU surface: shader {} is missing from the resources",
                  path.toStdString());
    return QShader();
  }
  return QShader::fromSerialized(file.readAll());
}

void appendColor(std::vector<float>& out, const QColor& color) {
  out.push_back(static_cast<float>(color.redF()));
  out.push_back(static_cast<float>(color.greenF()));
  out.push_back(static_cast<float>(color.blueF()));
  out.push_back(static_cast<float>(color.alphaF()));
}

/// Two triangles for an axis-aligned rectangle.
void appendQuad(std::vector<float>& out, const QRectF& rect,
                const QColor& color) {
  const float left = static_cast<float>(rect.left());
  const float top = static_cast<float>(rect.top());
  const float right = static_cast<float>(rect.right());
  const float bottom = static_cast<float>(rect.bottom());

  const float corners[6][2] = {{left, top},  {right, top},    {left, bottom},
                               {right, top}, {right, bottom}, {left, bottom}};
  for (const auto& corner : corners) {
    out.push_back(corner[0]);
    out.push_back(corner[1]);
    appendColor(out, color);
  }
}

}  // namespace

FramePreviewSurface::FramePreviewSurface(QWidget* parent) : QRhiWidget(parent) {
  // The owner keeps every interaction it had; this child only draws.
  setAttribute(Qt::WA_TransparentForMouseEvents, true);

  connect(this, &QRhiWidget::renderFailed, this, [this]() {
    GpuSurfacePolicy::instance().noteRenderFailure(
        QStringLiteral("QRhiWidget::renderFailed"));
  });
}

FramePreviewSurface::~FramePreviewSurface() = default;

void FramePreviewSurface::setFrameImage(const QImage& image) {
  // RHI has no packed RGB format, so the texture is RGBA8. The expansion
  // itself happens on the render worker (see previewImageToRgbaQImage); this
  // conversion is the safety net for a caller that handed over something
  // else, and is a no-op in the normal path.
  frame_ = image.isNull() || image.format() == QImage::Format_RGBA8888
               ? image
               : image.convertToFormat(QImage::Format_RGBA8888);
  frame_dirty_ = true;
}

void FramePreviewSurface::setTargetRect(const QRect& rect) {
  target_rect_ = rect;
}

void FramePreviewSurface::setBackgroundColor(const QColor& color) {
  background_ = color;
}

void FramePreviewSurface::setOverlay(OverlayPrimitives primitives) {
  overlay_ = std::move(primitives);
}

void FramePreviewSurface::setSmoothScaling(bool smooth) { smooth_ = smooth; }

void FramePreviewSurface::refresh() { update(); }

void FramePreviewSurface::paint(QPainter& painter, const QRect& damaged) {
  Q_UNUSED(painter);
  Q_UNUSED(damaged);
}

void FramePreviewSurface::initialize(QRhiCommandBuffer* cb) {
  Q_UNUSED(cb);

  if (rhi_ != rhi()) {
    // A new QRhi means every resource built against the old one is gone.
    releaseResources();
    rhi_ = rhi();
    pipelines_failed_ = false;
  }

  if (rhi_ == nullptr || pipelines_failed_) {
    return;
  }

  GpuSurfacePolicy::instance().setBackendName(
      QString::fromUtf8(rhi_->backendName()));

  if (!frame_pipeline_ &&
      !createPipelines(renderTarget()->renderPassDescriptor(),
                       renderTarget()->sampleCount())) {
    pipelines_failed_ = true;
    GpuSurfacePolicy::instance().noteRenderFailure(
        QStringLiteral("pipeline creation"));
  }
}

bool FramePreviewSurface::buildResourcesForTesting(
    QRhi* rhi, QRhiRenderPassDescriptor* pass, int sample_count) {
  releaseResources();
  rhi_ = rhi;
  return createPipelines(pass, sample_count);
}

bool FramePreviewSurface::createPipelines(QRhiRenderPassDescriptor* pass,
                                          int sample_count) {
  const QShader frame_vert =
      loadShader(QStringLiteral(":/orc/gpu/shaders/frame_preview.vert.qsb"));
  const QShader frame_frag =
      loadShader(QStringLiteral(":/orc/gpu/shaders/frame_preview.frag.qsb"));
  const QShader overlay_vert =
      loadShader(QStringLiteral(":/orc/gpu/shaders/overlay.vert.qsb"));
  const QShader overlay_frag =
      loadShader(QStringLiteral(":/orc/gpu/shaders/overlay.frag.qsb"));
  if (!frame_vert.isValid() || !frame_frag.isValid() ||
      !overlay_vert.isValid() || !overlay_frag.isValid()) {
    return false;
  }

  uniform_buffer_.reset(rhi_->newBuffer(
      QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformBlockSize));
  if (!uniform_buffer_->create()) {
    return false;
  }

  // Four vertices, one quad: the frame is always a single rectangle.
  frame_vertex_buffer_.reset(
      rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                      4 * kFrameVertexFloats * quint32{sizeof(float)}));
  if (!frame_vertex_buffer_->create()) {
    return false;
  }

  linear_sampler_.reset(rhi_->newSampler(
      QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
      QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
  nearest_sampler_.reset(rhi_->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
      QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
  if (!linear_sampler_->create() || !nearest_sampler_->create()) {
    return false;
  }

  // A one-pixel placeholder keeps the bindings valid before the first frame
  // arrives; ensureTexture() replaces it as soon as one does.
  frame_texture_.reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
  if (!frame_texture_->create()) {
    return false;
  }
  uploaded_size_ = QSize(1, 1);

  const auto make_frame_bindings = [this](QRhiSampler* sampler) {
    auto bindings = std::unique_ptr<QRhiShaderResourceBindings>(
        rhi_->newShaderResourceBindings());
    bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage, uniform_buffer_.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, frame_texture_.get(),
            sampler),
    });
    return bindings->create() ? std::move(bindings) : nullptr;
  };

  frame_bindings_linear_ = make_frame_bindings(linear_sampler_.get());
  frame_bindings_nearest_ = make_frame_bindings(nearest_sampler_.get());
  if (!frame_bindings_linear_ || !frame_bindings_nearest_) {
    return false;
  }

  overlay_bindings_.reset(rhi_->newShaderResourceBindings());
  overlay_bindings_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(
          0, QRhiShaderResourceBinding::VertexStage, uniform_buffer_.get()),
  });
  if (!overlay_bindings_->create()) {
    return false;
  }

  // Straight (non-premultiplied) source-over, matching the QPainter
  // composition the raster path uses for the same colours.
  QRhiGraphicsPipeline::TargetBlend blend;
  blend.enable = true;
  blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
  blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  blend.srcAlpha = QRhiGraphicsPipeline::One;
  blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;

  QRhiVertexInputLayout frame_layout;
  frame_layout.setBindings(
      {QRhiVertexInputBinding(kFrameVertexFloats * sizeof(float))});
  frame_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2,
                               2 * sizeof(float)),
  });

  frame_pipeline_.reset(rhi_->newGraphicsPipeline());
  frame_pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
  frame_pipeline_->setShaderStages({{QRhiShaderStage::Vertex, frame_vert},
                                    {QRhiShaderStage::Fragment, frame_frag}});
  frame_pipeline_->setVertexInputLayout(frame_layout);
  frame_pipeline_->setShaderResourceBindings(frame_bindings_linear_.get());
  frame_pipeline_->setRenderPassDescriptor(pass);
  frame_pipeline_->setSampleCount(sample_count);
  if (!frame_pipeline_->create()) {
    return false;
  }

  QRhiVertexInputLayout overlay_layout;
  overlay_layout.setBindings(
      {QRhiVertexInputBinding(kOverlayVertexFloats * sizeof(float))});
  overlay_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4,
                               2 * sizeof(float)),
  });

  const auto make_overlay_pipeline =
      [&](QRhiGraphicsPipeline::Topology topology) {
        auto pipeline =
            std::unique_ptr<QRhiGraphicsPipeline>(rhi_->newGraphicsPipeline());
        pipeline->setTopology(topology);
        pipeline->setTargetBlends({blend});
        pipeline->setShaderStages({{QRhiShaderStage::Vertex, overlay_vert},
                                   {QRhiShaderStage::Fragment, overlay_frag}});
        pipeline->setVertexInputLayout(overlay_layout);
        pipeline->setShaderResourceBindings(overlay_bindings_.get());
        pipeline->setRenderPassDescriptor(pass);
        pipeline->setSampleCount(sample_count);
        return pipeline->create() ? std::move(pipeline) : nullptr;
      };

  overlay_quad_pipeline_ =
      make_overlay_pipeline(QRhiGraphicsPipeline::Triangles);
  overlay_line_pipeline_ = make_overlay_pipeline(QRhiGraphicsPipeline::Lines);
  if (!overlay_quad_pipeline_ || !overlay_line_pipeline_) {
    return false;
  }

  // The texture is new, so whatever frame is already held has not reached it.
  frame_dirty_ = true;
  return true;
}

bool FramePreviewSurface::ensureTexture() {
  if (frame_.isNull()) {
    return false;
  }
  if (frame_.size() == uploaded_size_) {
    return true;
  }

  frame_texture_.reset(rhi_->newTexture(QRhiTexture::RGBA8, frame_.size()));
  if (!frame_texture_->create()) {
    frame_texture_.reset();
    uploaded_size_ = QSize();
    return false;
  }
  uploaded_size_ = frame_.size();

  // The bindings name the texture object, so a reallocation invalidates them.
  for (auto* bindings :
       {frame_bindings_linear_.get(), frame_bindings_nearest_.get()}) {
    bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage, uniform_buffer_.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, frame_texture_.get(),
            bindings == frame_bindings_linear_.get() ? linear_sampler_.get()
                                                     : nearest_sampler_.get()),
    });
    bindings->updateResources();
  }
  return true;
}

void FramePreviewSurface::buildFrameVertices(std::vector<float>& out) const {
  out.clear();
  if (target_rect_.isEmpty()) {
    return;
  }

  const double ratio = devicePixelRatioF();
  const float left = static_cast<float>(target_rect_.left() * ratio);
  const float top = static_cast<float>(target_rect_.top() * ratio);
  const float right =
      static_cast<float>((target_rect_.left() + target_rect_.width()) * ratio);
  const float bottom =
      static_cast<float>((target_rect_.top() + target_rect_.height()) * ratio);

  // Triangle strip order: top-left, top-right, bottom-left, bottom-right.
  const float vertices[4][kFrameVertexFloats] = {
      {left, top, 0.0F, 0.0F},
      {right, top, 1.0F, 0.0F},
      {left, bottom, 0.0F, 1.0F},
      {right, bottom, 1.0F, 1.0F},
  };
  out.assign(&vertices[0][0], &vertices[0][0] + static_cast<std::ptrdiff_t>(4) *
                                                    kFrameVertexFloats);
}

void FramePreviewSurface::buildOverlayVertices(
    std::vector<float>& quads, std::vector<float>& lines) const {
  quads.clear();
  lines.clear();

  const OverlayPrimitives device = scaled(overlay_, devicePixelRatioF());

  quads.reserve(device.quads.size() * 6 * kOverlayVertexFloats);
  for (const OverlayQuad& quad : device.quads) {
    appendQuad(quads, quad.rect, quad.color);
  }

  lines.reserve(device.lines.size() * 2 * kOverlayVertexFloats);
  for (const OverlayLine& line : device.lines) {
    for (const QPointF& point : {line.from, line.to}) {
      lines.push_back(static_cast<float>(point.x()));
      lines.push_back(static_cast<float>(point.y()));
      appendColor(lines, line.color);
    }
  }
}

void FramePreviewSurface::render(QRhiCommandBuffer* cb) {
  if (rhi_ == nullptr || !frame_pipeline_) {
    return;
  }

  const QSize output = renderTarget()->pixelSize();
  QRhiResourceUpdateBatch* updates = rhi_->nextResourceUpdateBatch();

  // Positions are in render-target pixels with y down, the same space
  // FrameViewGeometry works in, so nothing above this has to think in NDC.
  QMatrix4x4 mvp = rhi_->clipSpaceCorrMatrix();
  mvp.ortho(0.0F, static_cast<float>(output.width()),
            static_cast<float>(output.height()), 0.0F, -1.0F, 1.0F);
  updates->updateDynamicBuffer(uniform_buffer_.get(), 0, kUniformBlockSize,
                               mvp.constData());

  const bool have_frame = ensureTexture();
  if (have_frame && frame_dirty_) {
    QRhiTextureUploadDescription upload(QRhiTextureUploadEntry(
        0, 0, QRhiTextureSubresourceUploadDescription(frame_)));
    updates->uploadTexture(frame_texture_.get(), upload);
    frame_dirty_ = false;
  }

  std::vector<float> frame_vertices;
  buildFrameVertices(frame_vertices);
  if (!frame_vertices.empty()) {
    updates->updateDynamicBuffer(
        frame_vertex_buffer_.get(), 0,
        static_cast<quint32>(frame_vertices.size() * sizeof(float)),
        frame_vertices.data());
  }

  std::vector<float> overlay_quads;
  std::vector<float> overlay_lines;
  buildOverlayVertices(overlay_quads, overlay_lines);

  const quint32 quad_bytes =
      static_cast<quint32>(overlay_quads.size() * sizeof(float));
  const quint32 line_bytes =
      static_cast<quint32>(overlay_lines.size() * sizeof(float));
  const quint32 overlay_bytes = quad_bytes + line_bytes;

  if (overlay_bytes > 0 && overlay_bytes > overlay_buffer_capacity_) {
    // Grown, never shrunk: hovering a region changes the overlay count on
    // every mouse move, and reallocating each time would stall the frame.
    const quint32 capacity = overlay_bytes + overlay_bytes / 2;
    overlay_vertex_buffer_.reset(rhi_->newBuffer(
        QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, capacity));
    if (overlay_vertex_buffer_->create()) {
      overlay_buffer_capacity_ = capacity;
    } else {
      overlay_vertex_buffer_.reset();
      overlay_buffer_capacity_ = 0;
    }
  }

  if (overlay_vertex_buffer_ && overlay_bytes > 0) {
    if (quad_bytes > 0) {
      updates->updateDynamicBuffer(overlay_vertex_buffer_.get(), 0, quad_bytes,
                                   overlay_quads.data());
    }
    if (line_bytes > 0) {
      updates->updateDynamicBuffer(overlay_vertex_buffer_.get(), quad_bytes,
                                   line_bytes, overlay_lines.data());
    }
  }

  cb->beginPass(renderTarget(), background_, {1.0F, 0}, updates);
  cb->setViewport(QRhiViewport(0.0F, 0.0F, static_cast<float>(output.width()),
                               static_cast<float>(output.height())));

  if (have_frame && !frame_vertices.empty()) {
    cb->setGraphicsPipeline(frame_pipeline_.get());
    cb->setShaderResources(smooth_ ? frame_bindings_linear_.get()
                                   : frame_bindings_nearest_.get());
    const QRhiCommandBuffer::VertexInput input(frame_vertex_buffer_.get(), 0);
    cb->setVertexInput(0, 1, &input);
    cb->draw(4);
  }

  if (overlay_vertex_buffer_ && quad_bytes > 0) {
    cb->setGraphicsPipeline(overlay_quad_pipeline_.get());
    cb->setShaderResources(overlay_bindings_.get());
    const QRhiCommandBuffer::VertexInput input(overlay_vertex_buffer_.get(), 0);
    cb->setVertexInput(0, 1, &input);
    cb->draw(static_cast<quint32>(overlay_quads.size() / kOverlayVertexFloats));
  }

  if (overlay_vertex_buffer_ && line_bytes > 0) {
    cb->setGraphicsPipeline(overlay_line_pipeline_.get());
    cb->setShaderResources(overlay_bindings_.get());
    const QRhiCommandBuffer::VertexInput input(overlay_vertex_buffer_.get(),
                                               quad_bytes);
    cb->setVertexInput(0, 1, &input);
    cb->draw(static_cast<quint32>(overlay_lines.size() / kOverlayVertexFloats));
  }

  cb->endPass();
}

void FramePreviewSurface::releaseResources() {
  frame_pipeline_.reset();
  overlay_quad_pipeline_.reset();
  overlay_line_pipeline_.reset();
  frame_bindings_linear_.reset();
  frame_bindings_nearest_.reset();
  overlay_bindings_.reset();
  linear_sampler_.reset();
  nearest_sampler_.reset();
  frame_texture_.reset();
  frame_vertex_buffer_.reset();
  overlay_vertex_buffer_.reset();
  uniform_buffer_.reset();
  overlay_buffer_capacity_ = 0;
  uploaded_size_ = QSize();
  frame_dirty_ = true;
  rhi_ = nullptr;
}

}  // namespace orc::gui::gpu
