/*
 * File:        observation_pass.cpp
 * Module:      analysis
 * Purpose:     Parallel observer sweep over a frame range for disc analysis
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "observation_pass.h"

#include <biphase_observer.h>
#include <black_psnr_observer.h>
#include <burst_level_observer.h>
#include <orc/stage/field_id.h>
#include <orc/support/logging.h>
#include <white_snr_observer.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace orc {

namespace {

// The four observers the disc mapper depends on:
//  - BiphaseObserver     "biphase" vbi_line_16/17/18 (picture numbers)
//  - BurstLevelObserver  "burst_level" median_burst_10bit
//  - WhiteSNRObserver    "white_snr" snr_db
//  - BlackPSNRObserver   "black_psnr" psnr_db
// The last three supply the signal-quality readings the deduplication stage
// uses to choose between duplicate copies of a disc picture. They run in the
// same pass so that each frame is read once.
class ObserverSet {
 public:
  void observe(VideoFrameRepresentation& source, FrameID id,
               IObservationContext& out) {
    biphase_.process_frame(source, id, out);
    burst_level_.process_frame(source, id, out);
    white_snr_.process_frame(source, id, out);
    black_psnr_.process_frame(source, id, out);
  }

 private:
  BiphaseObserver biphase_;
  BurstLevelObserver burst_level_;
  WhiteSNRObserver white_snr_;
  BlackPSNRObserver black_psnr_;
};

// A range of frames one worker claims, and the observations it produced.
struct Chunk {
  FrameID first = 0;
  FrameID last = 0;  // inclusive
  ObservationContext observations;

  uint64_t frame_count() const {
    return (last >= first) ? last - first + 1 : 0;
  }
};

// Copy every observation recorded for the fields of [first, last] into |into|.
// Observers key on the two fields derived from a frame: FieldID(frame * 2) and
// FieldID(frame * 2 + 1).
void merge_frame_observations(const ObservationContext& from, FrameID first,
                              FrameID last, ObservationContext& into) {
  for (FrameID frame = first; frame <= last; ++frame) {
    for (uint64_t half = 0; half < 2; ++half) {
      const FieldID field_id(frame * 2 + half);
      for (const auto& [namespace_, entries] :
           from.get_all_observations(field_id)) {
        for (const auto& [key, value] : entries) {
          into.set(field_id, namespace_, key, value);
        }
      }
    }
  }
}

void report_progress(AnalysisProgress* progress, uint64_t observed,
                     uint64_t total) {
  if (!progress || total == 0) return;
  progress->setProgress(static_cast<int>(observed * 100 / total));
  progress->setSubStatus("Frame " + std::to_string(observed) + " / " +
                         std::to_string(total));
}

// Single-threaded sweep: used for small ranges, when only one worker is
// available, and as the fallback when private per-worker sources cannot be
// built.
ObservationPassOutcome run_sequential(VideoFrameRepresentation& source,
                                      FrameIDRange range,
                                      ObservationContext& out,
                                      AnalysisProgress* progress) {
  ObservationPassOutcome outcome;
  const uint64_t total = range.count();
  const uint64_t update_interval = std::max<uint64_t>(1, total / 100);

  ObserverSet observers;
  uint64_t observed = 0;
  for (FrameID id = range.first; id <= range.last; ++id) {
    observers.observe(source, id, out);
    ++observed;
    if (progress && observed % update_interval == 0) {
      report_progress(progress, observed, total);
      if (progress->isCancelled()) {
        outcome.cancelled = true;
        return outcome;
      }
    }
  }
  return outcome;
}

}  // namespace

ObservationPassOutcome run_disc_analysis_observers(
    VideoFrameRepresentation& source, FrameIDRange range,
    ObservationContext& out, AnalysisProgress* progress,
    const ObservationWorkerSourceFactory& make_worker_source,
    const ObservationPassOptions& options) {
  if (range.empty()) {
    return {};
  }

  const uint64_t total_frames = range.count();
  const size_t chunk_frames = std::max<size_t>(1, options.chunk_frames);
  const uint64_t chunk_count = (total_frames + chunk_frames - 1) / chunk_frames;

  unsigned hardware = std::thread::hardware_concurrency();
  if (hardware == 0) hardware = 1;
  unsigned worker_count =
      std::min({options.max_workers, hardware,
                static_cast<unsigned>(std::min<uint64_t>(chunk_count, 1024))});
  if (worker_count < 1) worker_count = 1;

  // Build a private source per worker up front. Any failure drops the whole
  // pass to the sequential path rather than letting workers share a source:
  // stages are stateful, and a source's frame cache hands out pointers into
  // storage another thread can evict.
  std::vector<ObservationWorkerSource> worker_sources;
  if (worker_count > 1 && make_worker_source) {
    worker_sources.reserve(worker_count);
    for (unsigned i = 0; i < worker_count; ++i) {
      ObservationWorkerSource worker = make_worker_source();
      if (!worker.source) {
        ORC_LOG_WARN(
            "ObservationPass: could not build a private source for worker "
            "{}; observing single-threaded",
            i);
        worker_sources.clear();
        break;
      }
      worker_sources.push_back(std::move(worker));
    }
  }

  if (worker_sources.size() < 2) {
    ORC_LOG_DEBUG("ObservationPass: observing {} frames single-threaded",
                  total_frames);
    return run_sequential(source, range, out, progress);
  }

  worker_count = static_cast<unsigned>(worker_sources.size());
  ORC_LOG_DEBUG(
      "ObservationPass: observing {} frames across {} worker(s), {} frames "
      "per chunk",
      total_frames, worker_count, chunk_frames);

  std::atomic<uint64_t> next_chunk{0};
  std::atomic<bool> stop{false};

  std::mutex mutex;
  std::condition_variable completed_cv;
  std::deque<Chunk> completed;
  std::exception_ptr worker_error;

  auto worker_main = [&](unsigned index) {
    ObserverSet observers;
    VideoFrameRepresentation& worker_source = *worker_sources[index].source;
    try {
      while (!stop.load(std::memory_order_relaxed)) {
        const uint64_t chunk_index =
            next_chunk.fetch_add(1, std::memory_order_relaxed);
        if (chunk_index >= chunk_count) return;

        Chunk chunk;
        chunk.first = range.first + chunk_index * chunk_frames;
        chunk.last =
            std::min<FrameID>(chunk.first + chunk_frames - 1, range.last);

        for (FrameID id = chunk.first; id <= chunk.last; ++id) {
          if (stop.load(std::memory_order_relaxed)) return;
          observers.observe(worker_source, id, chunk.observations);
        }

        {
          std::lock_guard<std::mutex> lock(mutex);
          completed.push_back(std::move(chunk));
        }
        completed_cv.notify_one();
      }
    } catch (...) {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (!worker_error) worker_error = std::current_exception();
      }
      stop.store(true, std::memory_order_relaxed);
      completed_cv.notify_all();
    }
  };

  std::vector<std::thread> workers;
  workers.reserve(worker_count);
  for (unsigned i = 0; i < worker_count; ++i) {
    workers.emplace_back(worker_main, i);
  }

  // Merge on this thread: ObservationContext is a plain map with no locking,
  // and AnalysisProgress carries no thread-safety guarantee, so both stay
  // owned by the caller's thread.
  ObservationPassOutcome outcome;
  outcome.workers_used = worker_count;
  uint64_t merged_chunks = 0;
  uint64_t observed_frames = 0;

  while (merged_chunks < chunk_count) {
    std::deque<Chunk> batch;
    {
      std::unique_lock<std::mutex> lock(mutex);
      completed_cv.wait(lock, [&] {
        return !completed.empty() || worker_error ||
               stop.load(std::memory_order_relaxed);
      });
      if (worker_error) break;
      batch.swap(completed);
    }
    if (batch.empty()) break;  // stop requested with nothing left to merge

    for (const Chunk& chunk : batch) {
      merge_frame_observations(chunk.observations, chunk.first, chunk.last,
                               out);
      observed_frames += chunk.frame_count();
      ++merged_chunks;
    }

    report_progress(progress, observed_frames, total_frames);
    if (progress && progress->isCancelled()) {
      outcome.cancelled = true;
      stop.store(true, std::memory_order_relaxed);
      break;
    }
  }

  stop.store(true, std::memory_order_relaxed);
  completed_cv.notify_all();
  for (std::thread& worker : workers) {
    if (worker.joinable()) worker.join();
  }

  // Workers are done, so the queue and the error slot are ours without a lock;
  // take it anyway rather than reasoning about it at every future edit.
  std::exception_ptr error;
  {
    std::lock_guard<std::mutex> lock(mutex);
    error = worker_error;
  }
  if (error) std::rethrow_exception(error);

  return outcome;
}

}  // namespace orc
