/*
 * File:        observation_pass.h
 * Module:      analysis
 * Purpose:     Parallel observer sweep over a frame range for disc analysis
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#ifndef ORC_CORE_ANALYSIS_OBSERVATION_PASS_H
#define ORC_CORE_ANALYSIS_OBSERVATION_PASS_H

#include <orc/stage/frame_id.h>
#include <orc/stage/observation/observation_context.h>
#include <orc/stage/video_frame_representation.h>

#include <cstddef>
#include <functional>
#include <memory>

#include "analysis_progress.h"

namespace orc {

/**
 * @brief One worker's private source, plus whatever keeps it alive
 *
 * Stages are stateful and a source's frame cache hands out pointers into
 * storage another thread can evict, so workers must not share a source. Each
 * worker therefore gets its own — in production a private DAG clone executed
 * to the same node (see clone_dag_with_fresh_stages()).
 *
 * @p owner holds whatever the source depends on (the executor, the cloned
 * DAG); it is released after the source. Leave it null when nothing else
 * needs keeping.
 */
struct ObservationWorkerSource {
  std::shared_ptr<void> owner;
  std::shared_ptr<VideoFrameRepresentation> source;
};

/**
 * @brief Builds one independent source per worker
 *
 * Called once per worker, from the calling thread, before any worker starts.
 * Returning an entry with a null source means a private source could not be
 * built; the pass then falls back to running single-threaded on the shared
 * source rather than sharing one across threads.
 */
using ObservationWorkerSourceFactory = std::function<ObservationWorkerSource()>;

struct ObservationPassOptions {
  // Upper bound on worker threads; also clamped to the hardware concurrency
  // and to the number of chunks the range divides into.
  //
  // Four is the measured knee. Reading a 128 GB PAL CVBS source over NFS,
  // whole-frame reads sustained 394 MB/s with one reader, 455 with two, 530
  // with four and 563 with eight: the link is near saturation by four, and the
  // remaining 6% is not worth the memory. Each worker holds its own source,
  // and a source caches recently read frames (~200 MB for PAL 4FSC), so this
  // is also the multiplier on the sweep's peak memory.
  unsigned max_workers = 4;

  // Frames one worker claims at a time. Large enough that claiming is not a
  // bottleneck, small enough to keep progress responsive and the buffered
  // per-chunk observations small.
  size_t chunk_frames = 128;
};

struct ObservationPassOutcome {
  // True when the pass stopped early because progress reported cancellation.
  bool cancelled = false;

  // Workers actually used — 1 when the pass ran single-threaded.
  unsigned workers_used = 1;
};

/**
 * @brief Run the disc-analysis observers over every frame of @p range
 *
 * Runs BiphaseObserver, BurstLevelObserver, WhiteSNRObserver and
 * BlackPSNRObserver over each frame in one pass, so a frame is read once and
 * measured by all four. Results land in @p out.
 *
 * Parallel where possible: the range is split into chunks that workers claim
 * and observe against their own source, and the calling thread merges each
 * finished chunk into @p out. All four observers are registered stateless
 * (see kObserverRegistry) — each frame's measurements depend only on that
 * frame — so chunks may be observed in any order.
 *
 * Threading: @p out, @p progress and @p source are touched only by the
 * calling thread. Workers write into their own private context and source.
 * An exception thrown by a worker is rethrown here once the workers have been
 * joined.
 *
 * @param source     Shared source, used when running single-threaded.
 * @param range      Inclusive frame range to observe.
 * @param out        Receives every observation recorded.
 * @param progress   Optional; polled for cancellation and given percentage.
 * @param make_worker_source  Builds per-worker sources; see the typedef.
 * @param options    Worker and chunk sizing.
 */
ObservationPassOutcome run_disc_analysis_observers(
    VideoFrameRepresentation& source, FrameIDRange range,
    ObservationContext& out, AnalysisProgress* progress,
    const ObservationWorkerSourceFactory& make_worker_source,
    const ObservationPassOptions& options = {});

}  // namespace orc

#endif  // ORC_CORE_ANALYSIS_OBSERVATION_PASS_H
