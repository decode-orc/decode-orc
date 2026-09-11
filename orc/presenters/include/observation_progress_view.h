/*
 * File:        observation_progress_view.h
 * Module:      orc-presenters
 * Purpose:     View-facing payloads for async observation delivery and
 *              background-workload progress notifications (Phase 5)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace orc::presenters {

/**
 * @brief Snapshot of the background observation workload, forwarded to the GUI.
 *
 * Contains only view-facing scalars — no Qt and no host-internal types — so it
 * can cross the presenter/view boundary. Mirrors the scheduler's
 * ObservationWorkload but drops the raw frame counters the status line does not
 * need. @c percent_complete is always in [0, 100]; when @c active is false the
 * queue is idle and the status line clears.
 */
struct ObservationProgressEvent {
  bool active = false;       ///< True while background work is running.
  int percent_complete = 0;  ///< Overall completion, 0..100.
  /// True when the current batch has actually computed at least one frame;
  /// false while it is only verifying coverage of already-stored frames. Lets
  /// the status line say "Computing…" vs "Checking…" honestly.
  bool computing = false;
  std::size_t outstanding_nodes = 0;  ///< Distinct nodes with pending work.
  /// True while queued whole-node sweep work is held back because a preview is
  /// playing. The status line says so rather than showing a percentage that
  /// has stopped moving with no explanation.
  bool sweep_paused = false;
};

/// Callback invoked with a workload snapshot whenever it changes. Fired on the
/// scheduler's worker thread; subscribers must marshal to their own thread.
using ObservationProgressCallback =
    std::function<void(const ObservationProgressEvent&)>;

/**
 * @brief Delivery callback for an async observation request (Task 5.1).
 *
 * @param request_id   Echoes the id returned by requestObservations(), so the
 *                     caller can drop responses to superseded requests.
 * @param available    True when @p obs_context carries the requested frame's
 *                     observations; false when the frame could not be produced.
 * @param obs_context  Opaque pointer to a core ObservationContext holding the
 *                     frame's observations, or nullptr when @p available is
 *                     false. Valid ONLY for the duration of the callback — the
 *                     receiver must extract what it needs synchronously (e.g.
 *                     via the observation presenters) and must not retain it.
 *
 * May be invoked synchronously inside requestObservations() (store hit) or
 * later on the scheduler's worker thread (store miss). Subscribers marshalling
 * to another thread must therefore extract value-type view models inside the
 * callback, never forward @p obs_context.
 */
using ObservationDataReadyCallback = std::function<void(
    uint64_t request_id, bool available, const void* obs_context)>;

}  // namespace orc::presenters
