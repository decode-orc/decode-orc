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
#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>

#include "../logging.h"
#include "frame_plane_uniforms.h"
#include "gpu_surface_policy.h"

namespace orc::gui::gpu {

namespace {

/// Floats per vertex: x, y, u, v.
constexpr int kFrameVertexFloats = 4;
/// Floats per vertex: x, y, r, g, b, a.
constexpr int kOverlayVertexFloats = 6;
/// Bytes in the shared uniform block (one mat4).
constexpr quint32 kUniformBlockSize = 64;
/// Bytes in the plane-conversion uniform block (three vec4s).
constexpr quint32 kPlaneUniformBlockSize =
    kFramePlaneUniformFloats * quint32{sizeof(float)};

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
  planes_.reset();
  planes_dirty_ = false;
}

bool FramePreviewSurface::setFramePlanes(
    std::shared_ptr<const orc::PreviewPlanes> planes) {
  if (plane_resources_failed_ || planes == nullptr || !planes->is_valid()) {
    return false;
  }

  planes_ = std::move(planes);
  planes_dirty_ = true;
  frame_ = QImage();
  frame_dirty_ = false;
  return true;
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

  // The plane path is optional: a device that cannot sample floating-point
  // textures still draws frames the worker converted for it, so a failure
  // here costs the conversion pass and nothing else. The policy is told so
  // that the next render is asked for an image rather than for planes.
  if (!createPlaneResources()) {
    abandonPlaneConversion("frame plane conversion resources");
  }

  // The texture is new, so whatever frame is already held has not reached it.
  frame_dirty_ = true;
  return true;
}

bool FramePreviewSurface::createPlaneResources() {
  // A device with no floating-point sampled texture cannot hold the decoder's
  // component planes, so it keeps the image path and loses only this one.
  if (!rhi_->isTextureFormatSupported(QRhiTexture::R32F)) {
    ORC_LOG_WARN(
        "GPU surface: no floating-point texture format, so frames will be "
        "converted on the render worker");
    return false;
  }

  // Paired with the vertex stage the scope canvas's full-target passes use:
  // one quad from the vertex index, with the source addressed by texel.
  const QShader plane_vert =
      loadShader(QStringLiteral(":/orc/gpu/shaders/scope_fullscreen.vert.qsb"));
  const QShader plane_frag =
      loadShader(QStringLiteral(":/orc/gpu/shaders/frame_planes.frag.qsb"));
  const QShader signal_frag = loadShader(
      QStringLiteral(":/orc/gpu/shaders/frame_planes_signal.frag.qsb"));
  if (!plane_vert.isValid() || !plane_frag.isValid() ||
      !signal_frag.isValid()) {
    return false;
  }

  plane_uniform_buffer_.reset(rhi_->newBuffer(
      QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kPlaneUniformBlockSize));
  if (!plane_uniform_buffer_->create()) {
    return false;
  }

  // Nearest and clamped: every fetch is an exact texel, and the conversion
  // must not blend neighbouring samples before the transfer curve is applied
  // - that is what makes this equal to the CPU conversion rather than close
  // to it.
  plane_sampler_.reset(rhi_->newSampler(
      QRhiSampler::Nearest, QRhiSampler::Nearest, QRhiSampler::None,
      QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
  if (!plane_sampler_->create()) {
    return false;
  }

  // One-texel placeholders keep the bindings valid until the first frame.
  for (auto& plane : plane_textures_) {
    plane.reset(rhi_->newTexture(QRhiTexture::R32F, QSize(1, 1)));
    if (!plane->create()) {
      return false;
    }
  }
  plane_texture_size_ = QSize(1, 1);
  transfer_texture_.reset(rhi_->newTexture(QRhiTexture::R32F, QSize(1, 1)));
  if (!transfer_texture_->create()) {
    return false;
  }
  transfer_texture_size_ = QSize(1, 1);
  uploaded_transfer_lut_.reset();

  plane_bindings_.reset(rhi_->newShaderResourceBindings());
  describePlaneBindings();
  if (!plane_bindings_->create()) {
    return false;
  }

  // The pass descriptor outlives the targets built against it, so it is taken
  // from a throwaway target of the right format rather than from the frame
  // texture, which is reallocated whenever the frame's size changes.
  const std::unique_ptr<QRhiTexture> descriptor_texture(rhi_->newTexture(
      QRhiTexture::RGBA8, QSize(1, 1), 1, QRhiTexture::RenderTarget));
  if (!descriptor_texture->create()) {
    return false;
  }
  const std::unique_ptr<QRhiTextureRenderTarget> descriptor_target(
      rhi_->newTextureRenderTarget(
          {QRhiColorAttachment(descriptor_texture.get())}));
  convert_pass_.reset(descriptor_target->newCompatibleRenderPassDescriptor());

  const auto make_convert_pipeline = [&](const QShader& fragment) {
    auto pipeline =
        std::unique_ptr<QRhiGraphicsPipeline>(rhi_->newGraphicsPipeline());
    pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipeline->setShaderStages({{QRhiShaderStage::Vertex, plane_vert},
                               {QRhiShaderStage::Fragment, fragment}});
    pipeline->setVertexInputLayout({});
    pipeline->setShaderResourceBindings(plane_bindings_.get());
    pipeline->setRenderPassDescriptor(convert_pass_.get());
    pipeline->setSampleCount(1);
    return pipeline->create() ? std::move(pipeline) : nullptr;
  };

  plane_pipeline_ = make_convert_pipeline(plane_frag);
  signal_plane_pipeline_ = make_convert_pipeline(signal_frag);
  if (!plane_pipeline_ || !signal_plane_pipeline_) {
    return false;
  }

  // The inactive-area mask and the dropout bands are drawn over the converted
  // frame inside the same pass, in the frame's own pixels, because that is
  // where the CPU renderer puts them: burned into the image before anything
  // rescales it. Drawing them in the widget's pass instead would let a band
  // thinner than a screen pixel disappear entirely.
  const QShader overlay_vert =
      loadShader(QStringLiteral(":/orc/gpu/shaders/overlay.vert.qsb"));
  const QShader overlay_frag =
      loadShader(QStringLiteral(":/orc/gpu/shaders/overlay.frag.qsb"));
  if (!overlay_vert.isValid() || !overlay_frag.isValid()) {
    return false;
  }

  plane_overlay_uniform_buffer_.reset(rhi_->newBuffer(
      QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kUniformBlockSize));
  if (!plane_overlay_uniform_buffer_->create()) {
    return false;
  }

  plane_overlay_bindings_.reset(rhi_->newShaderResourceBindings());
  plane_overlay_bindings_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(
          0, QRhiShaderResourceBinding::VertexStage,
          plane_overlay_uniform_buffer_.get()),
  });
  if (!plane_overlay_bindings_->create()) {
    return false;
  }

  // Straight source-over, so a band's alpha is the fraction of it that
  // replaces what is underneath - which is how both the 30% dim and the
  // three-quarter red of a dropout are expressed.
  QRhiGraphicsPipeline::TargetBlend band_blend;
  band_blend.enable = true;
  band_blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
  band_blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
  band_blend.srcAlpha = QRhiGraphicsPipeline::One;
  band_blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;

  QRhiVertexInputLayout band_layout;
  band_layout.setBindings(
      {QRhiVertexInputBinding(kOverlayVertexFloats * sizeof(float))});
  band_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float4,
                               2 * sizeof(float)),
  });

  plane_overlay_pipeline_.reset(rhi_->newGraphicsPipeline());
  plane_overlay_pipeline_->setTopology(QRhiGraphicsPipeline::Triangles);
  plane_overlay_pipeline_->setTargetBlends({band_blend});
  plane_overlay_pipeline_->setShaderStages(
      {{QRhiShaderStage::Vertex, overlay_vert},
       {QRhiShaderStage::Fragment, overlay_frag}});
  plane_overlay_pipeline_->setVertexInputLayout(band_layout);
  plane_overlay_pipeline_->setShaderResourceBindings(
      plane_overlay_bindings_.get());
  plane_overlay_pipeline_->setRenderPassDescriptor(convert_pass_.get());
  plane_overlay_pipeline_->setSampleCount(1);
  return plane_overlay_pipeline_->create();
}

QSize FramePreviewSurface::frameSize() const {
  if (planes_) {
    return QSize(static_cast<int>(planes_->width),
                 static_cast<int>(planes_->height));
  }
  return frame_.size();
}

bool FramePreviewSurface::ensureTexture() {
  const QSize wanted = frameSize();
  if (wanted.isEmpty()) {
    return false;
  }
  if (wanted == uploaded_size_ &&
      texture_is_convert_target_ == (planes_ != nullptr)) {
    return true;
  }

  // On the plane path the frame texture is what the conversion draws into, so
  // it carries a flag an uploaded texture does not. Changing path therefore
  // reallocates even when the size has not moved.
  const QRhiTexture::Flags flags =
      planes_ ? QRhiTexture::Flags(QRhiTexture::RenderTarget)
              : QRhiTexture::Flags();
  frame_texture_.reset(rhi_->newTexture(QRhiTexture::RGBA8, wanted, 1, flags));
  if (!frame_texture_->create()) {
    frame_texture_.reset();
    uploaded_size_ = QSize();
    return false;
  }
  uploaded_size_ = wanted;
  texture_is_convert_target_ = planes_ != nullptr;

  convert_target_.reset();
  if (planes_) {
    convert_target_.reset(rhi_->newTextureRenderTarget(
        {QRhiColorAttachment(frame_texture_.get())}));
    convert_target_->setRenderPassDescriptor(convert_pass_.get());
    if (!convert_target_->create()) {
      convert_target_.reset();
      return false;
    }
  }

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

bool FramePreviewSurface::ensurePlaneTextures() {
  const QSize wanted = frameSize();
  if (wanted == plane_texture_size_) {
    return true;
  }

  for (auto& plane : plane_textures_) {
    plane.reset(rhi_->newTexture(QRhiTexture::R32F, wanted));
    if (!plane->create()) {
      plane_texture_size_ = QSize();
      return false;
    }
  }
  plane_texture_size_ = wanted;

  describePlaneBindings();
  plane_bindings_->updateResources();
  return true;
}

void FramePreviewSurface::describePlaneBindings() {
  plane_bindings_->setBindings({
      QRhiShaderResourceBinding::uniformBuffer(
          0, QRhiShaderResourceBinding::FragmentStage,
          plane_uniform_buffer_.get()),
      QRhiShaderResourceBinding::sampledTexture(
          1, QRhiShaderResourceBinding::FragmentStage, plane_textures_[0].get(),
          plane_sampler_.get()),
      QRhiShaderResourceBinding::sampledTexture(
          2, QRhiShaderResourceBinding::FragmentStage, plane_textures_[1].get(),
          plane_sampler_.get()),
      QRhiShaderResourceBinding::sampledTexture(
          3, QRhiShaderResourceBinding::FragmentStage, plane_textures_[2].get(),
          plane_sampler_.get()),
      QRhiShaderResourceBinding::sampledTexture(
          4, QRhiShaderResourceBinding::FragmentStage, transfer_texture_.get(),
          plane_sampler_.get()),
  });
}

void FramePreviewSurface::releasePlaneResources() {
  plane_pipeline_.reset();
  signal_plane_pipeline_.reset();
  plane_overlay_pipeline_.reset();
  plane_overlay_bindings_.reset();
  plane_overlay_vertex_buffer_.reset();
  plane_overlay_uniform_buffer_.reset();
  plane_overlay_capacity_ = 0;
  plane_bindings_.reset();
  convert_target_.reset();
  convert_pass_.reset();
  for (auto& plane : plane_textures_) {
    plane.reset();
  }
  transfer_texture_.reset();
  plane_sampler_.reset();
  plane_uniform_buffer_.reset();
  plane_texture_size_ = QSize();
  transfer_texture_size_ = QSize();
  uploaded_transfer_lut_.reset();
}

void FramePreviewSurface::abandonPlaneConversion(const char* context) {
  releasePlaneResources();
  plane_resources_failed_ = true;
  planes_.reset();
  planes_dirty_ = false;
  GpuSurfacePolicy::instance().notePlaneConversionUnavailable(
      QString::fromUtf8(context));
}

void FramePreviewSurface::buildPlaneOverlayVertices(
    std::vector<float>& out) const {
  out.clear();
  if (!planes_) {
    return;
  }

  // Dimmed to ~30%, which source-over expresses as black at the complement of
  // that - the same result as the CPU renderer's multiply.
  const QColor dim(0, 0, 0, 179);
  // Three-quarters red over a quarter of what was there, as render_dropouts()
  // blends it.
  const QColor dropout(255, 0, 0, 191);

  out.reserve((planes_->dimmed_bands.size() + planes_->dropout_regions.size()) *
              6 * kOverlayVertexFloats);

  for (const orc::PreviewPlaneBand& band : planes_->dimmed_bands) {
    appendQuad(out, QRectF(band.x, band.y, band.width, band.height), dim);
  }

  if (!planes_->burn_in_dropouts) {
    return;
  }

  for (const orc::DropoutRegion& region : planes_->dropout_regions) {
    if (region.line >= planes_->height ||
        region.end_sample <= region.start_sample) {
      continue;
    }
    const double left = std::min(region.start_sample, planes_->width);
    const double right = std::min(region.end_sample, planes_->width);
    if (right <= left) {
      continue;
    }
    appendQuad(out, QRectF(left, region.line, right - left, 1.0), dropout);
  }
}

void FramePreviewSurface::convertPlanes(QRhiCommandBuffer* cb,
                                        QRhiResourceUpdateBatch*& updates) {
  if (!planes_dirty_ || !plane_pipeline_ || !convert_target_) {
    return;
  }
  if (!ensurePlaneTextures()) {
    abandonPlaneConversion("frame plane textures");
    return;
  }

  // Only the colour domain has a curve to apply, and only it fills U and V.
  // The table changes only when the source's transfer characteristic does,
  // which for a playing preview is never; the payload shares one table per
  // characteristic so that this compares pointers rather than contents.
  if (planes_->transfer_lut &&
      planes_->transfer_lut != uploaded_transfer_lut_) {
    const std::vector<float> packed =
        packFrameTransferLut(*planes_->transfer_lut);
    const QSize wanted(
        kFrameTransferLutWidth,
        frameTransferLutRows(static_cast<int>(planes_->transfer_lut->size())));
    if (wanted != transfer_texture_size_) {
      transfer_texture_.reset(rhi_->newTexture(QRhiTexture::R32F, wanted));
      if (!transfer_texture_->create()) {
        transfer_texture_size_ = QSize();
        abandonPlaneConversion("frame transfer table texture");
        return;
      }
      transfer_texture_size_ = wanted;
      describePlaneBindings();
      plane_bindings_->updateResources();
    }
    const qsizetype table_bytes =
        static_cast<qsizetype>(packed.size()) * qsizetype{sizeof(float)};
    QRhiTextureSubresourceUploadDescription table;
    table.setData(
        QByteArray(reinterpret_cast<const char*>(packed.data()), table_bytes));
    updates->uploadTexture(
        transfer_texture_.get(),
        QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, table)));
    uploaded_transfer_lut_ = planes_->transfer_lut;
  }

  const std::array<const std::vector<float>*, 3> sources = {
      &planes_->y_plane, &planes_->u_plane, &planes_->v_plane};
  const int plane_count =
      planes_->domain == orc::PreviewPlaneDomain::Signal ? 1 : 3;
  for (int i = 0; i < plane_count; ++i) {
    const qsizetype plane_bytes =
        static_cast<qsizetype>(sources[i]->size()) * qsizetype{sizeof(float)};
    QRhiTextureSubresourceUploadDescription plane;
    // fromRawData rather than a copy: the batch is committed by the beginPass
    // below, while the payload is still held, and a copy here would be three
    // more passes over the frame than this exists to save.
    plane.setData(QByteArray::fromRawData(
        reinterpret_cast<const char*>(sources[i]->data()), plane_bytes));
    updates->uploadTexture(
        plane_textures_[static_cast<size_t>(i)].get(),
        QRhiTextureUploadDescription(QRhiTextureUploadEntry(0, 0, plane)));
  }

  const bool signal_domain = planes_->domain == orc::PreviewPlaneDomain::Signal;
  const std::array<float, kFramePlaneUniformFloats> uniforms =
      signal_domain ? packSignalPlaneUniforms(*planes_)
                    : packFramePlaneUniforms(*planes_);
  updates->updateDynamicBuffer(plane_uniform_buffer_.get(), 0,
                               kPlaneUniformBlockSize, uniforms.data());

  const QSize size = frameSize();

  // The bands are in the frame's own pixels, so the projection for them is the
  // frame's rect rather than the widget's.
  std::vector<float> bands;
  buildPlaneOverlayVertices(bands);
  const quint32 band_bytes = static_cast<quint32>(bands.size() * sizeof(float));
  if (band_bytes > plane_overlay_capacity_) {
    // Grown, never shrunk: the dropout count moves frame to frame.
    const quint32 capacity = band_bytes + (band_bytes / 2);
    plane_overlay_vertex_buffer_.reset(rhi_->newBuffer(
        QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, capacity));
    if (plane_overlay_vertex_buffer_->create()) {
      plane_overlay_capacity_ = capacity;
    } else {
      plane_overlay_vertex_buffer_.reset();
      plane_overlay_capacity_ = 0;
    }
  }
  const bool draw_bands =
      band_bytes > 0 && plane_overlay_vertex_buffer_ != nullptr;
  if (draw_bands) {
    updates->updateDynamicBuffer(plane_overlay_vertex_buffer_.get(), 0,
                                 band_bytes, bands.data());
    // The conversion above writes plane row r at framebuffer row r, because
    // it addresses its source by gl_FragCoord. The bands are placed by a
    // matrix instead, and the two conventions only agree where the
    // framebuffer's y runs downwards - so on a backend where it runs up, the
    // projection is flipped to match what the conversion just wrote.
    QMatrix4x4 band_mvp = rhi_->clipSpaceCorrMatrix();
    if (rhi_->isYUpInFramebuffer()) {
      band_mvp.ortho(0.0F, static_cast<float>(size.width()), 0.0F,
                     static_cast<float>(size.height()), -1.0F, 1.0F);
    } else {
      band_mvp.ortho(0.0F, static_cast<float>(size.width()),
                     static_cast<float>(size.height()), 0.0F, -1.0F, 1.0F);
    }
    updates->updateDynamicBuffer(plane_overlay_uniform_buffer_.get(), 0,
                                 kUniformBlockSize, band_mvp.constData());
  }

  cb->beginPass(convert_target_.get(), Qt::black, {1.0F, 0}, updates);
  updates = nullptr;  // the pass took ownership of the batch
  cb->setGraphicsPipeline(signal_domain ? signal_plane_pipeline_.get()
                                        : plane_pipeline_.get());
  cb->setShaderResources(plane_bindings_.get());
  cb->setViewport(QRhiViewport(0.0F, 0.0F, static_cast<float>(size.width()),
                               static_cast<float>(size.height())));
  cb->draw(4);

  if (draw_bands) {
    cb->setGraphicsPipeline(plane_overlay_pipeline_.get());
    cb->setShaderResources(plane_overlay_bindings_.get());
    const QRhiCommandBuffer::VertexInput input(
        plane_overlay_vertex_buffer_.get(), 0);
    cb->setVertexInput(0, 1, &input);
    cb->draw(static_cast<quint32>(bands.size() / kOverlayVertexFloats));
  }

  cb->endPass();

  planes_dirty_ = false;
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

  // A frame that arrived as planes is converted into the frame texture by a
  // pass of its own, at the frame's resolution, before anything samples it.
  // The pass takes the update batch with it, so what follows starts a new one.
  if (have_frame && planes_) {
    convertPlanes(cb, updates);
  }
  if (updates == nullptr) {
    updates = rhi_->nextResourceUpdateBatch();
  }

  if (have_frame && frame_dirty_ && !planes_) {
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
  releasePlaneResources();
  plane_resources_failed_ = false;
  texture_is_convert_target_ = false;
  // The frame itself is not dropped here - only the device's copy of it. This
  // runs on the way in to the first initialize() as well as on a device loss,
  // and a frame set before the widget was ever shown has to survive that.
  planes_dirty_ = planes_ != nullptr;
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
