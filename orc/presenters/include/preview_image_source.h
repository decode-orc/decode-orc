/*
 * File:        preview_image_source.h
 * Module:      orc-presenters
 * Purpose:     Where an A/B comparison gets its preview images from
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/common_types.h>
#include <orc/stage/node_id.h>
#include <orc/stage/preview/orc_rendering.h>

#include <cstdint>
#include <string>
#include <vector>

namespace orc::presenters {

/**
 * @brief Renders preview images for a comparison.
 *
 * RenderPresenter answers it from its preview renderer: RGB delivery, with no
 * dropout highlighting, so neither the overlay nor the main preview's
 * settings can differ between the two sides. Tests answer it from a mock.
 *
 * Thread safety: implementations need not be thread-safe. A caller uses one
 * source from one thread at a time; RenderPresenter's is used on the render
 * worker, the only thread its renderer may run on.
 */
class IPreviewImageSource {
 public:
  virtual ~IPreviewImageSource() = default;

  /// Render one output of a node as an RGB image at native size.
  virtual orc::PreviewRenderResult render(NodeID node,
                                          orc::PreviewOutputType type,
                                          uint64_t index,
                                          const std::string& option_id) = 0;

  /// The outputs a node offers; empty when it offers none or does not exist.
  virtual std::vector<orc::PreviewOutputInfo> outputs(NodeID node) = 0;
};

}  // namespace orc::presenters
