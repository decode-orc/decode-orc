/*
 * File:        scope_canvas.cpp
 * Module:      orc-gui
 * Purpose:     Qt RHI canvas for the vectorscope and waveform monitor
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "scope_canvas.h"

#include <rhi/qrhi.h>

#include <QFile>
#include <QMatrix4x4>
#include <algorithm>
#include <array>
#include <cstddef>

#include "../logging.h"
#include "gpu_surface_policy.h"

namespace orc::gui::gpu {

namespace {

/// Floats per accumulation vertex: x, y, primary weight, secondary weight.
constexpr int kAccumulateVertexFloats = 4;
/// Floats per map vertex: x, y, u, v.
constexpr int kMapVertexFloats = 4;
/// Bytes in the accumulation uniform block (one mat4).
constexpr quint32 kAccumulateUniformBytes = 64;
/// Bytes in the map uniform block (one mat4 and five vec4).
constexpr quint32 kMapUniformBytes =
    64 + (kScopeMapUniformFloats * sizeof(float));

QShader loadShader(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    ORC_LOG_ERROR("Scope canvas: shader {} is missing from the resources",
                  path.toStdString());
    return QShader();
  }
  return QShader::fromSerialized(file.readAll());
}

/// A one-pixel transparent texture stands in for an underlay or overlay the
/// caller did not supply, so the map shader always has something to sample.
const QImage& placeholderImage() {
  static const QImage image = []() {
    QImage blank(1, 1, QImage::Format_RGBA8888);
    blank.fill(Qt::transparent);
    return blank;
  }();
  return image;
}

}  // namespace

ScopeCanvas::ScopeCanvas(QWidget* parent) : QRhiWidget(parent) {
  // The dialogue keeps every interaction it had; this child only draws.
  setAttribute(Qt::WA_TransparentForMouseEvents, true);

  connect(this, &QRhiWidget::renderFailed, this, [this]() {
    GpuSurfacePolicy::instance().noteRenderFailure(
        QStringLiteral("QRhiWidget::renderFailed"));
  });
}

ScopeCanvas::~ScopeCanvas() = default;

void ScopeCanvas::setFrame(ScopeFrame frame) {
  frame_ = std::move(frame);
  vertices_dirty_ = true;
}

void ScopeCanvas::setBackgroundColor(const QColor& color) {
  background_ = color;
}

void ScopeCanvas::refresh() { update(); }

void ScopeCanvas::initialize(QRhiCommandBuffer* cb) {
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

  if (!map_pipeline_ && !createPipelines(renderTarget()->renderPassDescriptor(),
                                         renderTarget()->sampleCount())) {
    pipelines_failed_ = true;
    GpuSurfacePolicy::instance().noteRenderFailure(
        QStringLiteral("scope pipeline creation"));
  }
}

bool ScopeCanvas::buildResourcesForTesting(QRhi* rhi,
                                           QRhiRenderPassDescriptor* pass,
                                           int sample_count) {
  releaseResources();
  rhi_ = rhi;
  return createPipelines(pass, sample_count);
}

bool ScopeCanvas::createPipelines(QRhiRenderPassDescriptor* pass,
                                  int sample_count) {
  // The trace is counted in a floating-point target so a blend can add or
  // compare counts. Half precision is exact to 2048, which is far above the
  // few dozen counts the brightness formula saturates at.
  if (!rhi_->isTextureFormatSupported(QRhiTexture::RGBA16F,
                                      QRhiTexture::RenderTarget)) {
    ORC_LOG_WARN("Scope canvas: no floating-point render target available");
    return false;
  }

  const QShader accumulate_vert =
      loadShader(QStringLiteral(":/orc/gpu/shaders/scope_accumulate.vert.qsb"));
  const QShader accumulate_frag =
      loadShader(QStringLiteral(":/orc/gpu/shaders/scope_accumulate.frag.qsb"));
  const QShader map_vert =
      loadShader(QStringLiteral(":/orc/gpu/shaders/scope_map.vert.qsb"));
  const QShader map_frag =
      loadShader(QStringLiteral(":/orc/gpu/shaders/scope_map.frag.qsb"));
  if (!accumulate_vert.isValid() || !accumulate_frag.isValid() ||
      !map_vert.isValid() || !map_frag.isValid()) {
    return false;
  }

  accumulate_uniforms_.reset(rhi_->newBuffer(
      QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kAccumulateUniformBytes));
  map_uniforms_.reset(rhi_->newBuffer(
      QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kMapUniformBytes));
  if (!accumulate_uniforms_->create() || !map_uniforms_->create()) {
    return false;
  }

  // Four vertices, one quad: the canvas is always a single rectangle.
  map_buffer_.reset(
      rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer,
                      4 * kMapVertexFloats * quint32{sizeof(float)}));
  if (!map_buffer_->create()) {
    return false;
  }

  // The graticules are drawn at a fixed canvas size and scaled down to
  // whatever the window is, so they are sampled through their mip chain: a
  // one-pixel circle stroke minified without one breaks into dashes.
  image_sampler_.reset(rhi_->newSampler(
      QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::Linear,
      QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
  trace_linear_.reset(rhi_->newSampler(
      QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
      QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
  trace_nearest_.reset(rhi_->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
      QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
  if (!image_sampler_->create() || !trace_linear_->create() ||
      !trace_nearest_->create()) {
    return false;
  }

  // One-pixel placeholders keep every binding valid before the first frame
  // arrives; ensureCanvas() and ensureImageTexture() replace them.
  accumulation_.reset(rhi_->newTexture(QRhiTexture::RGBA16F, QSize(1, 1), 1,
                                       QRhiTexture::RenderTarget));
  underlay_.reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
  overlay_.reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
  if (!accumulation_->create() || !underlay_->create() || !overlay_->create()) {
    return false;
  }
  allocated_canvas_ = QSize(1, 1);
  underlay_key_ = 0;
  overlay_key_ = 0;

  accumulation_target_.reset(
      rhi_->newTextureRenderTarget({QRhiColorAttachment(accumulation_.get())}));
  accumulation_pass_.reset(
      accumulation_target_->newCompatibleRenderPassDescriptor());
  accumulation_target_->setRenderPassDescriptor(accumulation_pass_.get());
  if (!accumulation_target_->create()) {
    return false;
  }

  accumulate_bindings_.reset(rhi_->newShaderResourceBindings());
  accumulate_bindings_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(
          0, QRhiShaderResourceBinding::VertexStage,
          accumulate_uniforms_.get()),
  });
  if (!accumulate_bindings_->create()) {
    return false;
  }

  map_bindings_smooth_.reset(rhi_->newShaderResourceBindings());
  map_bindings_sharp_.reset(rhi_->newShaderResourceBindings());
  refreshMapBindings();
  if (!map_bindings_smooth_->create() || !map_bindings_sharp_->create()) {
    return false;
  }

  QRhiVertexInputLayout accumulate_layout;
  accumulate_layout.setBindings(
      {QRhiVertexInputBinding(kAccumulateVertexFloats * sizeof(float))});
  accumulate_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2,
                               2 * sizeof(float)),
  });

  const auto make_accumulate_pipeline =
      [&](QRhiGraphicsPipeline::Topology topology,
          QRhiGraphicsPipeline::BlendOp op) {
        QRhiGraphicsPipeline::TargetBlend blend;
        blend.enable = true;
        blend.srcColor = QRhiGraphicsPipeline::One;
        blend.dstColor = QRhiGraphicsPipeline::One;
        blend.srcAlpha = QRhiGraphicsPipeline::One;
        blend.dstAlpha = QRhiGraphicsPipeline::One;
        blend.opColor = op;
        blend.opAlpha = op;

        auto pipeline =
            std::unique_ptr<QRhiGraphicsPipeline>(rhi_->newGraphicsPipeline());
        pipeline->setTopology(topology);
        pipeline->setTargetBlends({blend});
        pipeline->setShaderStages(
            {{QRhiShaderStage::Vertex, accumulate_vert},
             {QRhiShaderStage::Fragment, accumulate_frag}});
        pipeline->setVertexInputLayout(accumulate_layout);
        pipeline->setShaderResourceBindings(accumulate_bindings_.get());
        pipeline->setRenderPassDescriptor(accumulation_pass_.get());
        pipeline->setSampleCount(1);
        return pipeline->create() ? std::move(pipeline) : nullptr;
      };

  point_pipeline_add_ = make_accumulate_pipeline(QRhiGraphicsPipeline::Points,
                                                 QRhiGraphicsPipeline::Add);
  point_pipeline_max_ = make_accumulate_pipeline(QRhiGraphicsPipeline::Points,
                                                 QRhiGraphicsPipeline::Max);
  line_pipeline_ = make_accumulate_pipeline(QRhiGraphicsPipeline::Lines,
                                            QRhiGraphicsPipeline::Add);
  if (!point_pipeline_add_ || !point_pipeline_max_ || !line_pipeline_) {
    return false;
  }

  QRhiVertexInputLayout map_layout;
  map_layout.setBindings(
      {QRhiVertexInputBinding(kMapVertexFloats * sizeof(float))});
  map_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float2,
                               2 * sizeof(float)),
  });

  map_pipeline_.reset(rhi_->newGraphicsPipeline());
  map_pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
  map_pipeline_->setShaderStages({{QRhiShaderStage::Vertex, map_vert},
                                  {QRhiShaderStage::Fragment, map_frag}});
  map_pipeline_->setVertexInputLayout(map_layout);
  map_pipeline_->setShaderResourceBindings(map_bindings_smooth_.get());
  map_pipeline_->setRenderPassDescriptor(pass);
  map_pipeline_->setSampleCount(sample_count);
  if (!map_pipeline_->create()) {
    return false;
  }

  // Nothing has reached the new textures yet.
  vertices_dirty_ = true;
  return true;
}

void ScopeCanvas::refreshMapBindings() {
  const auto bindings_for = [this](QRhiSampler* trace_sampler) {
    return std::array<QRhiShaderResourceBinding, 4>{
        QRhiShaderResourceBinding::uniformBuffer(
            0,
            QRhiShaderResourceBinding::VertexStage |
                QRhiShaderResourceBinding::FragmentStage,
            map_uniforms_.get()),
        QRhiShaderResourceBinding::sampledTexture(
            1, QRhiShaderResourceBinding::FragmentStage, accumulation_.get(),
            trace_sampler),
        QRhiShaderResourceBinding::sampledTexture(
            2, QRhiShaderResourceBinding::FragmentStage, underlay_.get(),
            image_sampler_.get()),
        QRhiShaderResourceBinding::sampledTexture(
            3, QRhiShaderResourceBinding::FragmentStage, overlay_.get(),
            image_sampler_.get()),
    };
  };
  const auto smooth = bindings_for(trace_linear_.get());
  map_bindings_smooth_->setBindings(smooth.cbegin(), smooth.cend());
  const auto sharp = bindings_for(trace_nearest_.get());
  map_bindings_sharp_->setBindings(sharp.cbegin(), sharp.cend());
}

bool ScopeCanvas::ensureCanvas() {
  // An empty canvas means there is nothing to plot, not that the pass should
  // be skipped: the accumulation still has to be cleared, or the window keeps
  // showing the trace of the frame before.
  const QSize wanted = frame_.canvas_size;
  if (wanted.isEmpty() || wanted == allocated_canvas_) {
    return accumulation_target_ != nullptr;
  }

  accumulation_.reset(rhi_->newTexture(QRhiTexture::RGBA16F, wanted, 1,
                                       QRhiTexture::RenderTarget));
  if (!accumulation_->create()) {
    accumulation_.reset();
    allocated_canvas_ = QSize();
    return false;
  }

  // The render pass descriptor stays valid: only the attachment's size has
  // changed, and compatibility is a matter of format and sample count.
  accumulation_target_.reset(
      rhi_->newTextureRenderTarget({QRhiColorAttachment(accumulation_.get())}));
  accumulation_target_->setRenderPassDescriptor(accumulation_pass_.get());
  if (!accumulation_target_->create()) {
    accumulation_target_.reset();
    allocated_canvas_ = QSize();
    return false;
  }

  allocated_canvas_ = wanted;
  refreshMapBindings();
  map_bindings_smooth_->updateResources();
  map_bindings_sharp_->updateResources();
  return true;
}

bool ScopeCanvas::ensureImageTexture(const QImage& image,
                                     std::unique_ptr<QRhiTexture>& texture,
                                     qint64& held_key,
                                     QRhiResourceUpdateBatch* updates) {
  const QImage source =
      image.isNull() ? placeholderImage()
                     : (image.format() == QImage::Format_RGBA8888
                            ? image
                            : image.convertToFormat(QImage::Format_RGBA8888));

  // A dialogue that redraws its graticule only when the system, the levels or
  // the mode change hands back the same image every other frame, and that is
  // an upload not worth doing.
  if (source.cacheKey() == held_key && texture &&
      texture->pixelSize() == source.size()) {
    return false;
  }
  held_key = source.cacheKey();

  bool reallocated = false;
  if (!texture || texture->pixelSize() != source.size()) {
    texture.reset(rhi_->newTexture(
        QRhiTexture::RGBA8, source.size(), 1,
        QRhiTexture::MipMapped | QRhiTexture::UsedWithGenerateMips));
    if (!texture->create()) {
      texture.reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
      texture->create();
      held_key = 0;
      return true;
    }
    reallocated = true;
  }

  updates->uploadTexture(
      texture.get(),
      QRhiTextureUploadDescription(QRhiTextureUploadEntry(
          0, 0, QRhiTextureSubresourceUploadDescription(source))));
  // A one-pixel placeholder has a single level and nothing to generate.
  if (texture->flags().testFlag(QRhiTexture::UsedWithGenerateMips) &&
      std::max(source.width(), source.height()) > 1) {
    updates->generateMips(texture.get());
  }
  return reallocated;
}

QRectF ScopeCanvas::contentRect() const {
  const QSize output =
      renderTarget() != nullptr ? renderTarget()->pixelSize() : QSize();
  if (output.isEmpty()) {
    return QRectF();
  }
  if (!frame_.preserve_aspect || frame_.canvas_size.isEmpty()) {
    return QRectF(0.0, 0.0, output.width(), output.height());
  }

  // The same fit the aspect-preserving label did: the plot keeps its shape
  // and is centred in whatever room the window gives it.
  const QSizeF fitted =
      QSizeF(frame_.canvas_size).scaled(QSizeF(output), Qt::KeepAspectRatio);
  return QRectF(QPointF((output.width() - fitted.width()) / 2.0,
                        (output.height() - fitted.height()) / 2.0),
                fitted);
}

bool ScopeCanvas::ensureVertexBuffer(std::unique_ptr<QRhiBuffer>& buffer,
                                     quint32& capacity, quint32 bytes) {
  if (bytes == 0) {
    return false;
  }
  if (buffer && bytes <= capacity) {
    return true;
  }
  // Grown, never shrunk: the sample count moves frame to frame as the picture
  // changes, and reallocating on every change would stall the plot.
  const quint32 wanted = bytes + bytes / 2;
  buffer.reset(
      rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, wanted));
  if (!buffer->create()) {
    buffer.reset();
    capacity = 0;
    return false;
  }
  capacity = wanted;
  return true;
}

void ScopeCanvas::render(QRhiCommandBuffer* cb) {
  if (rhi_ == nullptr || !map_pipeline_) {
    return;
  }

  const QSize output = renderTarget()->pixelSize();
  QRhiResourceUpdateBatch* updates = rhi_->nextResourceUpdateBatch();

  const bool have_canvas = ensureCanvas();
  bool bindings_changed = false;
  bindings_changed |=
      ensureImageTexture(frame_.underlay, underlay_, underlay_key_, updates);
  bindings_changed |=
      ensureImageTexture(frame_.overlay, overlay_, overlay_key_, updates);
  if (bindings_changed) {
    refreshMapBindings();
    map_bindings_smooth_->updateResources();
    map_bindings_sharp_->updateResources();
  }

  // Accumulation positions are canvas pixels with y down, which is the space
  // the scopes' own plot geometry works in.
  QMatrix4x4 accumulate_mvp = rhi_->clipSpaceCorrMatrix();
  accumulate_mvp.ortho(0.0F, static_cast<float>(allocated_canvas_.width()),
                       static_cast<float>(allocated_canvas_.height()), 0.0F,
                       -1.0F, 1.0F);
  updates->updateDynamicBuffer(accumulate_uniforms_.get(), 0,
                               kAccumulateUniformBytes,
                               accumulate_mvp.constData());

  QMatrix4x4 map_mvp = rhi_->clipSpaceCorrMatrix();
  map_mvp.ortho(0.0F, static_cast<float>(output.width()),
                static_cast<float>(output.height()), 0.0F, -1.0F, 1.0F);
  updates->updateDynamicBuffer(map_uniforms_.get(), 0, 64, map_mvp.constData());

  const std::array<float, kScopeMapUniformFloats> map_values =
      packScopeMapUniforms(frame_.map, allocated_canvas_,
                           rhi_->isYUpInFramebuffer());
  updates->updateDynamicBuffer(map_uniforms_.get(), 64,
                               kScopeMapUniformFloats * quint32{sizeof(float)},
                               map_values.data());

  const quint32 point_bytes = static_cast<quint32>(
      frame_.points.size() * kAccumulateVertexFloats * sizeof(float));
  const quint32 line_bytes = static_cast<quint32>(
      frame_.lines.size() * kAccumulateVertexFloats * sizeof(float));
  const bool have_points =
      ensureVertexBuffer(point_buffer_, point_capacity_, point_bytes);
  const bool have_lines =
      ensureVertexBuffer(line_buffer_, line_capacity_, line_bytes);
  if (vertices_dirty_) {
    if (have_points) {
      updates->updateDynamicBuffer(point_buffer_.get(), 0, point_bytes,
                                   frame_.points.data());
    }
    if (have_lines) {
      updates->updateDynamicBuffer(line_buffer_.get(), 0, line_bytes,
                                   frame_.lines.data());
    }
    vertices_dirty_ = false;
  }

  const QRectF content = contentRect();
  const float left = static_cast<float>(content.left());
  const float top = static_cast<float>(content.top());
  const float right = static_cast<float>(content.left() + content.width());
  const float bottom = static_cast<float>(content.top() + content.height());
  const float quad[4][kMapVertexFloats] = {
      {left, top, 0.0F, 0.0F},
      {right, top, 1.0F, 0.0F},
      {left, bottom, 0.0F, 1.0F},
      {right, bottom, 1.0F, 1.0F},
  };
  updates->updateDynamicBuffer(map_buffer_.get(), 0,
                               4 * kMapVertexFloats * quint32{sizeof(float)},
                               &quad[0][0]);

  if (have_canvas) {
    // Pass one: count what lands where. The clear is the empty canvas, so
    // nothing carries over from the frame before.
    cb->beginPass(accumulation_target_.get(), QColor(0, 0, 0, 0), {1.0F, 0},
                  updates);
    cb->setViewport(
        QRhiViewport(0.0F, 0.0F, static_cast<float>(allocated_canvas_.width()),
                     static_cast<float>(allocated_canvas_.height())));

    if (have_lines) {
      cb->setGraphicsPipeline(line_pipeline_.get());
      cb->setShaderResources(accumulate_bindings_.get());
      const QRhiCommandBuffer::VertexInput input(line_buffer_.get(), 0);
      cb->setVertexInput(0, 1, &input);
      cb->draw(static_cast<quint32>(frame_.lines.size()));
    }
    if (have_points) {
      cb->setGraphicsPipeline(frame_.blend == ScopeBlend::kMax
                                  ? point_pipeline_max_.get()
                                  : point_pipeline_add_.get());
      cb->setShaderResources(accumulate_bindings_.get());
      const QRhiCommandBuffer::VertexInput input(point_buffer_.get(), 0);
      cb->setVertexInput(0, 1, &input);
      cb->draw(static_cast<quint32>(frame_.points.size()));
    }
    cb->endPass();
    updates = nullptr;
  }

  // Pass two: counts to pixels, and the canvas into the window.
  cb->beginPass(renderTarget(), background_, {1.0F, 0}, updates);
  cb->setViewport(QRhiViewport(0.0F, 0.0F, static_cast<float>(output.width()),
                               static_cast<float>(output.height())));
  if (!content.isEmpty()) {
    cb->setGraphicsPipeline(map_pipeline_.get());
    cb->setShaderResources(frame_.smooth ? map_bindings_smooth_.get()
                                         : map_bindings_sharp_.get());
    const QRhiCommandBuffer::VertexInput input(map_buffer_.get(), 0);
    cb->setVertexInput(0, 1, &input);
    cb->draw(4);
  }
  cb->endPass();
}

void ScopeCanvas::releaseResources() {
  map_pipeline_.reset();
  point_pipeline_add_.reset();
  point_pipeline_max_.reset();
  line_pipeline_.reset();
  map_bindings_smooth_.reset();
  map_bindings_sharp_.reset();
  accumulate_bindings_.reset();
  image_sampler_.reset();
  trace_linear_.reset();
  trace_nearest_.reset();
  accumulation_target_.reset();
  accumulation_pass_.reset();
  accumulation_.reset();
  underlay_.reset();
  overlay_.reset();
  point_buffer_.reset();
  line_buffer_.reset();
  map_buffer_.reset();
  accumulate_uniforms_.reset();
  map_uniforms_.reset();
  point_capacity_ = 0;
  line_capacity_ = 0;
  allocated_canvas_ = QSize();
  underlay_key_ = 0;
  overlay_key_ = 0;
  vertices_dirty_ = true;
  rhi_ = nullptr;
}

}  // namespace orc::gui::gpu
