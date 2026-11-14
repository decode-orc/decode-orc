/*
 * File:        tool_frame_renderer.h
 * Module:      orc-gui
 * Purpose:     Renders whole frames for plugin views (render_frame) through
 *              the preview pipeline, off the GUI thread
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/node_id.h>
#include <orc/view/stage_tool_session.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

namespace orc::presenters {
class IRenderPresenter;
}  // namespace orc::presenters

/**
 * @brief Renders one frame of a node for a plugin view
 *
 * Made on the GUI thread for the graph as it is then, and used only on the
 * plugin view host's render thread afterwards.
 */
class IToolFrameRenderer {
 public:
  virtual ~IToolFrameRenderer() = default;

  /// Render frame @p frame_index (0-based) of @p node as frame-flat RGB.
  virtual orc::view::ToolFrameImage render(const orc::NodeID& node,
                                           uint64_t frame_index) = 0;
};

/**
 * @brief IToolFrameRenderer on a render presenter of its own
 *
 * Uses the "sequential" preview layout (image rows are the frame's lines in
 * order), the luma of a Y/C source, and no dropout overlay: the node's
 * dropouts are reported beside the image instead, for a view to draw. Not
 * thread-safe: one thread at a time.
 */
class PreviewToolFrameRenderer final : public IToolFrameRenderer {
 public:
  /// @param presenter A presenter with its DAG set and dropout overlay off
  explicit PreviewToolFrameRenderer(
      std::shared_ptr<orc::presenters::IRenderPresenter> presenter);
  ~PreviewToolFrameRenderer() override;

  orc::view::ToolFrameImage render(const orc::NodeID& node,
                                   uint64_t frame_index) override;

 private:
  struct Output;

  std::shared_ptr<orc::presenters::IRenderPresenter> presenter_;
  // What each node offers, found on its first render.
  std::map<orc::NodeID, std::shared_ptr<Output>> outputs_;
};
