/*
 * File:        tool_frame_renderer.cpp
 * Module:      orc-gui
 * Purpose:     Renders whole frames for plugin views through the preview
 *              pipeline
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "tool_frame_renderer.h"

#include <exception>
#include <utility>

#include "render_coordinator.h"  // IRenderPresenter

namespace {

// Image rows are the frame's lines in order, both fields interleaved.
constexpr const char* kSequentialOptionId = "sequential_clamped";
// The same layout showing the luma of a Y/C source.
constexpr const char* kSequentialLumaSuffix = "_yc";

}  // namespace

struct PreviewToolFrameRenderer::Output {
  bool available = false;
  std::string error;
  orc::PreviewOutputType type = orc::PreviewOutputType::Frame_Field1_First;
  std::string option_id;
  uint64_t frame_count = 0;
  double dar_aspect_correction = 1.0;
};

PreviewToolFrameRenderer::PreviewToolFrameRenderer(
    std::shared_ptr<orc::presenters::IRenderPresenter> presenter)
    : presenter_(std::move(presenter)) {}

PreviewToolFrameRenderer::~PreviewToolFrameRenderer() = default;

orc::view::ToolFrameImage PreviewToolFrameRenderer::render(
    const orc::NodeID& node, uint64_t frame_index) {
  orc::view::ToolFrameImage image;
  image.frame_index = frame_index;
  if (!presenter_) {
    image.error = "Frames cannot be rendered now";
    return image;
  }

  auto& output = outputs_[node];
  if (!output) {
    output = std::make_shared<Output>();
    try {
      // Executes the graph up to the node, on this thread.
      for (const auto& info : presenter_->getAvailableOutputs(node)) {
        if (info.option_id == kSequentialOptionId) {
          output->type = info.type;
          output->frame_count = info.count;
          output->dar_aspect_correction = info.dar_aspect_correction;
          output->option_id = kSequentialOptionId;
          if (info.has_separate_channels) {
            output->option_id += kSequentialLumaSuffix;
          }
          output->available = info.count > 0;
          break;
        }
      }
      if (!output->available) {
        output->error = "The node has no frames to render";
      }
    } catch (const std::exception& e) {
      output->error = std::string("The node cannot be rendered: ") + e.what();
    }
  }
  image.frame_count = output->frame_count;
  image.dar_aspect_correction = output->dar_aspect_correction;
  if (!output->available) {
    image.error = output->error;
    return image;
  }
  if (frame_index >= output->frame_count) {
    image.error = "The node has no frame " + std::to_string(frame_index);
    return image;
  }

  try {
    auto result = presenter_->renderPreview(
        node, output->type, frame_index, output->option_id,
        orc::PreviewNavigationHint::Random, orc::PreviewPixelDelivery::Rgb);
    if (!result.success || !result.image.is_valid()) {
      image.error = result.error_message.empty() ? "The frame did not render"
                                                 : result.error_message;
      return image;
    }
    image.width = result.image.width;
    image.height = result.image.height;
    image.rgb = std::move(result.image.rgb_data);
    image.dropouts.reserve(result.image.dropout_regions.size());
    for (const auto& region : result.image.dropout_regions) {
      image.dropouts.push_back(orc::view::ToolFrameDropout{
          region.line, region.start_sample, region.end_sample});
    }
    image.ok = true;
  } catch (const std::exception& e) {
    image.error = std::string("The frame did not render: ") + e.what();
  }
  return image;
}
