/*
 * File:        preview_render_cost_view.h
 * Module:      orc-presenters
 * Purpose:     View-facing breakdown of what one preview render cost the
 *              render worker
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstdint>

namespace orc::presenters {

/**
 * @brief Worker-side cost of one preview render, in microseconds.
 *
 * Two of the things a displayed frame pays for happen on the render worker,
 * inside the wait the view already measures as render latency, and neither is
 * visible from the GUI thread. Reporting them as scalars lets the frame
 * profiler attribute that wait without the view knowing anything about the
 * DAG or the observation store.
 *
 * Plain scalars only, so this crosses the presenter/view boundary.
 */
struct PreviewRenderCostView {
  /// Time inside DAG execution. Preview rendering runs with the artifact cache
  /// disabled so that each stage's execute() is called again, which on a deep
  /// graph can be most of what a frame costs.
  std::int64_t dag_execution_us = 0;

  /// Time filling the observation store for the rendered frame's fields. This
  /// materialises the frame a second time, through the cache's own renderer,
  /// purely so the observer dialogues have records to read.
  std::int64_t observation_fill_us = 0;
};

}  // namespace orc::presenters
