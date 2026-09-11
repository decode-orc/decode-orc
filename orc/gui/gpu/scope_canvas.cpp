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

/// Floats per accumulation vertex: x, y, weight.
constexpr int kAccumulateVertexFloats = 3;
/// Floats per map vertex: x, y, u, v.
constexpr int kMapVertexFloats = 4;
/// Bytes in the accumulation uniform block (one mat4 and one vec4).
constexpr quint32 kAccumulateUniformBytes = 80;
/// Bytes in the map uniform block (one mat4 and seven vec4).
constexpr quint32 kMapUniformBytes =
    64 + (kScopeMapUniformFloats * sizeof(float));
/// Bytes in the spread uniform block: two vec4 of configuration and four of
/// half-kernel weights.
constexpr quint32 kSpreadUniformBytes = 96;
/// Bytes in the reduction and histogram uniform blocks (one vec4 each).
constexpr quint32 kSmallUniformBytes = 16;
/// Buckets the charge histogram is divided into, as in the CPU renderer.
constexpr int kHistogramBuckets = 1024;
/// Fraction of the plot's charge that must sit above the anchor.
constexpr float kSaturationDwellFraction = 0.5F;
/// Levels of the reduction chain divide by this in each direction.
constexpr int kReductionFactor = 4;

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
                                      QRhiTexture::RenderTarget) ||
      !rhi_->isTextureFormatSupported(QRhiTexture::RGBA32F,
                                      QRhiTexture::RenderTarget) ||
      !rhi_->isTextureFormatSupported(QRhiTexture::R32F,
                                      QRhiTexture::RenderTarget)) {
    // Half precision carries the plot, which saturates far below its range;
    // full precision carries the reduction, whose sums reach the order of a
    // hundred million. A device without both cannot read a plot as dwell, and
    // one that old is better served by the CPU renderer throughout.
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
  const QShader fullscreen_vert =
      loadShader(QStringLiteral(":/orc/gpu/shaders/scope_fullscreen.vert.qsb"));
  const QShader spread_frag =
      loadShader(QStringLiteral(":/orc/gpu/shaders/scope_spread.frag.qsb"));
  const QShader reduce_frag =
      loadShader(QStringLiteral(":/orc/gpu/shaders/scope_reduce.frag.qsb"));
  const QShader histogram_vert =
      loadShader(QStringLiteral(":/orc/gpu/shaders/scope_histogram.vert.qsb"));
  const QShader histogram_frag =
      loadShader(QStringLiteral(":/orc/gpu/shaders/scope_histogram.frag.qsb"));
  const QShader anchor_frag =
      loadShader(QStringLiteral(":/orc/gpu/shaders/scope_anchor.frag.qsb"));
  if (!accumulate_vert.isValid() || !accumulate_frag.isValid() ||
      !map_vert.isValid() || !map_frag.isValid() ||
      !fullscreen_vert.isValid() || !spread_frag.isValid() ||
      !reduce_frag.isValid() || !histogram_vert.isValid() ||
      !histogram_frag.isValid() || !anchor_frag.isValid()) {
    return false;
  }

  accumulate_uniforms_points_.reset(rhi_->newBuffer(
      QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kAccumulateUniformBytes));
  accumulate_uniforms_strips_.reset(rhi_->newBuffer(
      QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kAccumulateUniformBytes));
  map_uniforms_.reset(rhi_->newBuffer(
      QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kMapUniformBytes));
  if (!accumulate_uniforms_points_->create() ||
      !accumulate_uniforms_strips_->create() || !map_uniforms_->create()) {
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

  const auto make_accumulate_bindings = [this](QRhiBuffer* uniforms) {
    auto bindings = std::unique_ptr<QRhiShaderResourceBindings>(
        rhi_->newShaderResourceBindings());
    bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(
            0, QRhiShaderResourceBinding::VertexStage, uniforms),
    });
    return bindings->create() ? std::move(bindings) : nullptr;
  };
  accumulate_bindings_points_ =
      make_accumulate_bindings(accumulate_uniforms_points_.get());
  accumulate_bindings_strips_ =
      make_accumulate_bindings(accumulate_uniforms_strips_.get());
  if (!accumulate_bindings_points_ || !accumulate_bindings_strips_) {
    return false;
  }

  QRhiVertexInputLayout accumulate_layout;
  accumulate_layout.setBindings(
      {QRhiVertexInputBinding(kAccumulateVertexFloats * sizeof(float))});
  accumulate_layout.setAttributes({
      QRhiVertexInputAttribute(0, 0, QRhiVertexInputAttribute::Float2, 0),
      QRhiVertexInputAttribute(0, 1, QRhiVertexInputAttribute::Float,
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
        pipeline->setShaderResourceBindings(accumulate_bindings_points_.get());
        pipeline->setRenderPassDescriptor(accumulation_pass_.get());
        pipeline->setSampleCount(1);
        return pipeline->create() ? std::move(pipeline) : nullptr;
      };

  point_pipeline_add_ = make_accumulate_pipeline(QRhiGraphicsPipeline::Points,
                                                 QRhiGraphicsPipeline::Add);
  point_pipeline_max_ = make_accumulate_pipeline(QRhiGraphicsPipeline::Points,
                                                 QRhiGraphicsPipeline::Max);
  strip_pipeline_ = make_accumulate_pipeline(QRhiGraphicsPipeline::LineStrip,
                                             QRhiGraphicsPipeline::Add);
  if (!point_pipeline_add_ || !point_pipeline_max_ || !strip_pipeline_) {
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

  // ---- Dwell stages ------------------------------------------------------
  // Built whether or not this scope reads its plot as dwell: the formats were
  // checked above, so a failure here is a failure of the device rather than
  // of one plot, and the whole canvas should give way to the CPU renderer.

  spread_scratch_.reset(rhi_->newTexture(QRhiTexture::RGBA16F, QSize(1, 1), 1,
                                         QRhiTexture::RenderTarget));
  if (!spread_scratch_->create()) {
    return false;
  }
  spread_scratch_target_.reset(rhi_->newTextureRenderTarget(
      {QRhiColorAttachment(spread_scratch_.get())}));
  spread_scratch_target_->setRenderPassDescriptor(accumulation_pass_.get());
  if (!spread_scratch_target_->create()) {
    return false;
  }

  const auto make_uniform_buffer = [this](quint32 bytes) {
    auto buffer = std::unique_ptr<QRhiBuffer>(
        rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, bytes));
    return buffer->create() ? std::move(buffer) : nullptr;
  };

  spread_uniforms_horizontal_ = make_uniform_buffer(kSpreadUniformBytes);
  spread_uniforms_vertical_ = make_uniform_buffer(kSpreadUniformBytes);
  histogram_uniforms_ = make_uniform_buffer(kSmallUniformBytes);
  anchor_uniforms_ = make_uniform_buffer(kSmallUniformBytes);
  if (!spread_uniforms_horizontal_ || !spread_uniforms_vertical_ ||
      !histogram_uniforms_ || !anchor_uniforms_) {
    return false;
  }

  // The reduction's first level reads the plot, so its render pass descriptor
  // comes from a placeholder that ensureReductionChain() later resizes.
  const auto make_single_texel = [this](QRhiTexture::Format format) {
    auto texture = std::unique_ptr<QRhiTexture>(
        rhi_->newTexture(format, QSize(1, 1), 1, QRhiTexture::RenderTarget));
    return texture->create() ? std::move(texture) : nullptr;
  };

  auto reduction_seed = make_single_texel(QRhiTexture::RGBA32F);
  histogram_ = nullptr;
  anchor_ = make_single_texel(QRhiTexture::R32F);
  if (!reduction_seed || !anchor_) {
    return false;
  }
  histogram_.reset(rhi_->newTexture(QRhiTexture::R32F,
                                    QSize(kHistogramBuckets, 1), 1,
                                    QRhiTexture::RenderTarget));
  if (!histogram_->create()) {
    return false;
  }

  const std::unique_ptr<QRhiTextureRenderTarget> reduction_seed_target(
      rhi_->newTextureRenderTarget(
          {QRhiColorAttachment(reduction_seed.get())}));
  reduction_pass_.reset(
      reduction_seed_target->newCompatibleRenderPassDescriptor());

  histogram_target_.reset(
      rhi_->newTextureRenderTarget({QRhiColorAttachment(histogram_.get())}));
  single_channel_pass_.reset(
      histogram_target_->newCompatibleRenderPassDescriptor());
  histogram_target_->setRenderPassDescriptor(single_channel_pass_.get());
  anchor_target_.reset(
      rhi_->newTextureRenderTarget({QRhiColorAttachment(anchor_.get())}));
  anchor_target_->setRenderPassDescriptor(single_channel_pass_.get());
  if (!histogram_target_->create() || !anchor_target_->create()) {
    return false;
  }

  const auto make_sampled_bindings =
      [this](QRhiBuffer* uniforms, QRhiTexture* first, QRhiTexture* second) {
        auto bindings = std::unique_ptr<QRhiShaderResourceBindings>(
            rhi_->newShaderResourceBindings());
        const auto stages = QRhiShaderResourceBinding::VertexStage |
                            QRhiShaderResourceBinding::FragmentStage;
        std::vector<QRhiShaderResourceBinding> list{
            QRhiShaderResourceBinding::uniformBuffer(0, stages, uniforms),
            QRhiShaderResourceBinding::sampledTexture(1, stages, first,
                                                      trace_nearest_.get()),
        };
        if (second != nullptr) {
          list.push_back(QRhiShaderResourceBinding::sampledTexture(
              2, stages, second, trace_nearest_.get()));
        }
        bindings->setBindings(list.cbegin(), list.cend());
        return bindings->create() ? std::move(bindings) : nullptr;
      };

  spread_bindings_horizontal_ = make_sampled_bindings(
      spread_uniforms_horizontal_.get(), accumulation_.get(), nullptr);
  spread_bindings_vertical_ = make_sampled_bindings(
      spread_uniforms_vertical_.get(), spread_scratch_.get(), nullptr);
  if (!spread_bindings_horizontal_ || !spread_bindings_vertical_) {
    return false;
  }

  // Both of these read the last level of the reduction chain, which does not
  // exist until a canvas size is known; they are re-pointed there.
  histogram_bindings_ = make_sampled_bindings(
      histogram_uniforms_.get(), accumulation_.get(), reduction_seed.get());
  anchor_bindings_ = make_sampled_bindings(
      anchor_uniforms_.get(), histogram_.get(), reduction_seed.get());
  if (!histogram_bindings_ || !anchor_bindings_) {
    return false;
  }

  // A pass with no vertex buffer at all: the shape comes from the vertex
  // index, so there is nothing to bind and nothing to upload.
  const QRhiVertexInputLayout empty_layout;

  const auto make_fullscreen_pipeline =
      [&](const QShader& fragment, QRhiRenderPassDescriptor* pass,
          QRhiShaderResourceBindings* bindings) {
        auto pipeline =
            std::unique_ptr<QRhiGraphicsPipeline>(rhi_->newGraphicsPipeline());
        pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
        pipeline->setShaderStages({{QRhiShaderStage::Vertex, fullscreen_vert},
                                   {QRhiShaderStage::Fragment, fragment}});
        pipeline->setVertexInputLayout(empty_layout);
        pipeline->setShaderResourceBindings(bindings);
        pipeline->setRenderPassDescriptor(pass);
        pipeline->setSampleCount(1);
        return pipeline->create() ? std::move(pipeline) : nullptr;
      };

  spread_pipeline_ = make_fullscreen_pipeline(
      spread_frag, accumulation_pass_.get(), spread_bindings_horizontal_.get());
  reduction_pipeline_ = make_fullscreen_pipeline(
      reduce_frag, reduction_pass_.get(), histogram_bindings_.get());
  anchor_pipeline_ = make_fullscreen_pipeline(
      anchor_frag, single_channel_pass_.get(), anchor_bindings_.get());
  if (!spread_pipeline_ || !reduction_pipeline_ || !anchor_pipeline_) {
    return false;
  }

  {
    // Each pixel's charge lands in the bucket for its level and is summed
    // there, so the histogram pass blends additively like the accumulation.
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::One;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::One;

    histogram_pipeline_.reset(rhi_->newGraphicsPipeline());
    histogram_pipeline_->setTopology(QRhiGraphicsPipeline::Points);
    histogram_pipeline_->setTargetBlends({blend});
    histogram_pipeline_->setShaderStages(
        {{QRhiShaderStage::Vertex, histogram_vert},
         {QRhiShaderStage::Fragment, histogram_frag}});
    histogram_pipeline_->setVertexInputLayout(empty_layout);
    histogram_pipeline_->setShaderResourceBindings(histogram_bindings_.get());
    histogram_pipeline_->setRenderPassDescriptor(single_channel_pass_.get());
    histogram_pipeline_->setSampleCount(1);
    if (!histogram_pipeline_->create()) {
      return false;
    }
  }

  // Built here rather than with the other bindings above: the map pass reads
  // the anchor the dwell stages produce, so the texture it names has to exist
  // before the bindings that name it can be created.
  map_bindings_smooth_.reset(rhi_->newShaderResourceBindings());
  map_bindings_sharp_.reset(rhi_->newShaderResourceBindings());
  refreshMapBindings();
  if (!map_bindings_smooth_->create() || !map_bindings_sharp_->create()) {
    return false;
  }

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
    return std::array<QRhiShaderResourceBinding, 5>{
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
        QRhiShaderResourceBinding::sampledTexture(
            4, QRhiShaderResourceBinding::FragmentStage, anchor_.get(),
            trace_nearest_.get()),
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

  spread_scratch_.reset(rhi_->newTexture(QRhiTexture::RGBA16F, wanted, 1,
                                         QRhiTexture::RenderTarget));
  spread_scratch_target_.reset(rhi_->newTextureRenderTarget(
      {QRhiColorAttachment(spread_scratch_.get())}));
  spread_scratch_target_->setRenderPassDescriptor(accumulation_pass_.get());
  if (!spread_scratch_->create() || !spread_scratch_target_->create()) {
    allocated_canvas_ = QSize();
    return false;
  }

  allocated_canvas_ = wanted;

  // Every one of these names a texture object that has just been replaced.
  const auto repoint = [this](QRhiShaderResourceBindings* bindings,
                              QRhiBuffer* uniforms, QRhiTexture* first,
                              QRhiTexture* second) {
    const auto stages = QRhiShaderResourceBinding::VertexStage |
                        QRhiShaderResourceBinding::FragmentStage;
    std::vector<QRhiShaderResourceBinding> list{
        QRhiShaderResourceBinding::uniformBuffer(0, stages, uniforms),
        QRhiShaderResourceBinding::sampledTexture(1, stages, first,
                                                  trace_nearest_.get()),
    };
    if (second != nullptr) {
      list.push_back(QRhiShaderResourceBinding::sampledTexture(
          2, stages, second, trace_nearest_.get()));
    }
    bindings->setBindings(list.cbegin(), list.cend());
    bindings->updateResources();
  };
  repoint(spread_bindings_horizontal_.get(), spread_uniforms_horizontal_.get(),
          accumulation_.get(), nullptr);
  repoint(spread_bindings_vertical_.get(), spread_uniforms_vertical_.get(),
          spread_scratch_.get(), nullptr);

  if (!ensureReductionChain()) {
    allocated_canvas_ = QSize();
    return false;
  }
  repoint(histogram_bindings_.get(), histogram_uniforms_.get(),
          accumulation_.get(), reduction_.back().texture.get());
  repoint(anchor_bindings_.get(), anchor_uniforms_.get(), histogram_.get(),
          reduction_.back().texture.get());

  refreshMapBindings();
  map_bindings_smooth_->updateResources();
  map_bindings_sharp_->updateResources();
  return true;
}

bool ScopeCanvas::ensureReductionChain() {
  reduction_.clear();

  QSize source = allocated_canvas_;
  bool first = true;
  while (true) {
    const QSize level_size(
        std::max(1, (source.width() + kReductionFactor - 1) / kReductionFactor),
        std::max(1,
                 (source.height() + kReductionFactor - 1) / kReductionFactor));

    ReductionLevel level;
    level.size = level_size;
    level.source_size = source;
    level.first = first;
    level.texture.reset(rhi_->newTexture(QRhiTexture::RGBA32F, level_size, 1,
                                         QRhiTexture::RenderTarget));
    if (!level.texture->create()) {
      reduction_.clear();
      return false;
    }
    level.target.reset(rhi_->newTextureRenderTarget(
        {QRhiColorAttachment(level.texture.get())}));
    level.target->setRenderPassDescriptor(reduction_pass_.get());
    if (!level.target->create()) {
      reduction_.clear();
      return false;
    }
    level.uniforms.reset(rhi_->newBuffer(
        QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, kSmallUniformBytes));
    if (!level.uniforms->create()) {
      reduction_.clear();
      return false;
    }

    QRhiTexture* const input =
        first ? accumulation_.get() : reduction_.back().texture.get();
    level.bindings.reset(rhi_->newShaderResourceBindings());
    const auto stages = QRhiShaderResourceBinding::VertexStage |
                        QRhiShaderResourceBinding::FragmentStage;
    const std::array<QRhiShaderResourceBinding, 2> list{
        QRhiShaderResourceBinding::uniformBuffer(0, stages,
                                                 level.uniforms.get()),
        QRhiShaderResourceBinding::sampledTexture(1, stages, input,
                                                  trace_nearest_.get()),
    };
    level.bindings->setBindings(list.cbegin(), list.cend());
    if (!level.bindings->create()) {
      reduction_.clear();
      return false;
    }

    reduction_.push_back(std::move(level));
    if (level_size.width() <= 1 && level_size.height() <= 1) {
      break;
    }
    source = level_size;
    first = false;
  }
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

bool ScopeCanvas::dwellPassesWanted() const {
  return frame_.map.trace_mode == ScopeTraceMode::kDwell &&
         !frame_.spot.isEmpty() && !reduction_.empty();
}

void ScopeCanvas::updateDwellUniforms(QRhiResourceUpdateBatch* updates) {
  if (!dwellPassesWanted()) {
    return;
  }

  // Spread: one direction each, with the half-kernel the CPU renderer would
  // have convolved by.
  const int radius = std::min(frame_.spot.radius, kMaxScopeSpreadRadius);
  std::array<float, kSpreadUniformBytes / sizeof(float)> spread{};
  spread[2] = static_cast<float>(radius);
  spread[4] = static_cast<float>(allocated_canvas_.width());
  spread[5] = static_cast<float>(allocated_canvas_.height());
  for (int t = 0; t <= radius; ++t) {
    spread[8 + static_cast<std::size_t>(t)] =
        frame_.spot.weights[static_cast<std::size_t>(t)];
  }

  spread[0] = 1.0F;
  spread[1] = 0.0F;
  updates->updateDynamicBuffer(spread_uniforms_horizontal_.get(), 0,
                               kSpreadUniformBytes, spread.data());
  spread[0] = 0.0F;
  spread[1] = 1.0F;
  updates->updateDynamicBuffer(spread_uniforms_vertical_.get(), 0,
                               kSpreadUniformBytes, spread.data());

  for (const ReductionLevel& level : reduction_) {
    const std::array<float, 4> values{
        level.first ? 1.0F : 0.0F,
        static_cast<float>(level.source_size.width()),
        static_cast<float>(level.source_size.height()), 0.0F};
    updates->updateDynamicBuffer(level.uniforms.get(), 0, kSmallUniformBytes,
                                 values.data());
  }

  const std::array<float, 4> histogram{
      static_cast<float>(allocated_canvas_.width()),
      static_cast<float>(allocated_canvas_.height()),
      static_cast<float>(kHistogramBuckets), 0.0F};
  updates->updateDynamicBuffer(histogram_uniforms_.get(), 0, kSmallUniformBytes,
                               histogram.data());

  const std::array<float, 4> anchor{static_cast<float>(kHistogramBuckets),
                                    kSaturationDwellFraction, 0.0F, 0.0F};
  updates->updateDynamicBuffer(anchor_uniforms_.get(), 0, kSmallUniformBytes,
                               anchor.data());
}

void ScopeCanvas::recordDwellPasses(QRhiCommandBuffer* cb) {
  const auto fullscreen = [cb](QRhiGraphicsPipeline* pipeline,
                               QRhiShaderResourceBindings* bindings,
                               QRhiRenderTarget* target) {
    const QSize size = target->pixelSize();
    cb->beginPass(target, QColor(0, 0, 0, 0), {1.0F, 0});
    cb->setGraphicsPipeline(pipeline);
    cb->setShaderResources(bindings);
    cb->setViewport(QRhiViewport(0.0F, 0.0F, static_cast<float>(size.width()),
                                 static_cast<float>(size.height())));
    cb->setVertexInput(0, 0, nullptr);
    cb->draw(4);
    cb->endPass();
  };

  // Spread the plot by the beam spot: horizontal into the scratch target,
  // vertical back over the accumulation, which nothing needs unspread again.
  fullscreen(spread_pipeline_.get(), spread_bindings_horizontal_.get(),
             spread_scratch_target_.get());
  fullscreen(spread_pipeline_.get(), spread_bindings_vertical_.get(),
             accumulation_target_.get());

  // Carry the brightest level and the total charge down to a single texel.
  for (const ReductionLevel& level : reduction_) {
    fullscreen(reduction_pipeline_.get(), level.bindings.get(),
               level.target.get());
  }

  // Scatter every lit pixel into the bucket for its level, then walk the
  // buckets down to the half-charge point.
  const QSize canvas = allocated_canvas_;
  cb->beginPass(histogram_target_.get(), QColor(0, 0, 0, 0), {1.0F, 0});
  cb->setGraphicsPipeline(histogram_pipeline_.get());
  cb->setShaderResources(histogram_bindings_.get());
  cb->setViewport(
      QRhiViewport(0.0F, 0.0F, static_cast<float>(kHistogramBuckets), 1.0F));
  cb->setVertexInput(0, 0, nullptr);
  cb->draw(static_cast<quint32>(canvas.width()) *
           static_cast<quint32>(canvas.height()));
  cb->endPass();

  fullscreen(anchor_pipeline_.get(), anchor_bindings_.get(),
             anchor_target_.get());
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
  std::array<float, kAccumulateUniformBytes / sizeof(float)> accumulate{};
  std::copy(accumulate_mvp.constData(), accumulate_mvp.constData() + 16,
            accumulate.begin());
  accumulate[16] = 1.0F;  // landings deposit in the first channel
  accumulate[17] = 0.0F;
  updates->updateDynamicBuffer(accumulate_uniforms_points_.get(), 0,
                               kAccumulateUniformBytes, accumulate.data());
  accumulate[16] = 0.0F;  // transits deposit in the second
  accumulate[17] = 1.0F;
  updates->updateDynamicBuffer(accumulate_uniforms_strips_.get(), 0,
                               kAccumulateUniformBytes, accumulate.data());

  QMatrix4x4 map_mvp = rhi_->clipSpaceCorrMatrix();
  map_mvp.ortho(0.0F, static_cast<float>(output.width()),
                static_cast<float>(output.height()), 0.0F, -1.0F, 1.0F);
  updates->updateDynamicBuffer(map_uniforms_.get(), 0, 64, map_mvp.constData());

  updateDwellUniforms(updates);

  const std::array<float, kScopeMapUniformFloats> map_values =
      packScopeMapUniforms(frame_.map, allocated_canvas_,
                           rhi_->isYUpInFramebuffer());
  updates->updateDynamicBuffer(map_uniforms_.get(), 64,
                               kScopeMapUniformFloats * quint32{sizeof(float)},
                               map_values.data());

  const quint32 point_bytes = static_cast<quint32>(
      frame_.points.size() * kAccumulateVertexFloats * sizeof(float));
  const bool have_points =
      ensureVertexBuffer(point_buffer_, point_capacity_, point_bytes);
  if (vertices_dirty_) {
    if (have_points) {
      updates->updateDynamicBuffer(point_buffer_.get(), 0, point_bytes,
                                   frame_.points.data());
      vertices_dirty_ = false;
    } else if (frame_.points.empty()) {
      // Nothing to upload rather than nowhere to upload it to: a buffer that
      // could not be created has to be tried again on the next frame.
      vertices_dirty_ = false;
    }
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

    if (have_points) {
      const QRhiCommandBuffer::VertexInput input(point_buffer_.get(), 0);

      // The transits first: the same samples, traced through rather than
      // landed on, so the rasteriser fills in the path the beam took between
      // them without any of it being written out a second time.
      if (!frame_.strips.empty()) {
        cb->setGraphicsPipeline(strip_pipeline_.get());
        cb->setShaderResources(accumulate_bindings_strips_.get());
        cb->setVertexInput(0, 1, &input);
        for (const ScopeStrip& strip : frame_.strips) {
          cb->draw(strip.count, 1, strip.first);
        }
      }

      cb->setGraphicsPipeline(frame_.blend == ScopeBlend::kMax
                                  ? point_pipeline_max_.get()
                                  : point_pipeline_add_.get());
      cb->setShaderResources(accumulate_bindings_points_.get());
      cb->setVertexInput(0, 1, &input);
      cb->draw(static_cast<quint32>(frame_.points.size()));
    }
    cb->endPass();
    updates = nullptr;

    if (dwellPassesWanted()) {
      recordDwellPasses(cb);
    }
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
  reduction_.clear();
  spread_pipeline_.reset();
  reduction_pipeline_.reset();
  histogram_pipeline_.reset();
  anchor_pipeline_.reset();
  spread_bindings_horizontal_.reset();
  spread_bindings_vertical_.reset();
  histogram_bindings_.reset();
  anchor_bindings_.reset();
  spread_uniforms_horizontal_.reset();
  spread_uniforms_vertical_.reset();
  histogram_uniforms_.reset();
  anchor_uniforms_.reset();
  spread_scratch_target_.reset();
  spread_scratch_.reset();
  histogram_target_.reset();
  histogram_.reset();
  anchor_target_.reset();
  anchor_.reset();
  reduction_pass_.reset();
  single_channel_pass_.reset();
  map_pipeline_.reset();
  point_pipeline_add_.reset();
  point_pipeline_max_.reset();
  strip_pipeline_.reset();
  map_bindings_smooth_.reset();
  map_bindings_sharp_.reset();
  accumulate_bindings_points_.reset();
  accumulate_bindings_strips_.reset();
  image_sampler_.reset();
  trace_linear_.reset();
  trace_nearest_.reset();
  accumulation_target_.reset();
  accumulation_pass_.reset();
  accumulation_.reset();
  underlay_.reset();
  overlay_.reset();
  point_buffer_.reset();
  map_buffer_.reset();
  accumulate_uniforms_points_.reset();
  accumulate_uniforms_strips_.reset();
  map_uniforms_.reset();
  point_capacity_ = 0;
  allocated_canvas_ = QSize();
  underlay_key_ = 0;
  overlay_key_ = 0;
  vertices_dirty_ = true;
  rhi_ = nullptr;
}

}  // namespace orc::gui::gpu
