/*
 * File:        nabts_block_scanner.cpp
 * Module:      orc-stage-plugin-nabts_sink
 * Purpose:     Block-parallel frame scanning implementation
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "nabts_block_scanner.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

namespace orc {

size_t nabts_resolve_worker_count(uint64_t frame_count, int32_t requested) {
  uint64_t wanted = static_cast<uint64_t>(std::max(requested, 0));
  if (wanted == 0) {
    unsigned hardware = std::thread::hardware_concurrency();
    if (hardware == 0) {
      hardware = 4;  // hardware_concurrency() is allowed to say it cannot tell
    }
    wanted = hardware;
  }
  const uint64_t capped = std::min(wanted, frame_count);
  return static_cast<size_t>(std::max<uint64_t>(1, capped));
}

NabtsBlockSlicer::NabtsBlockSlicer(size_t worker_count) {
  // One fewer thread than workers: the calling thread takes a share too, in
  // finish(), which keeps a single-core machine honest.
  const size_t threads = worker_count > 1 ? worker_count - 1 : 0;
  threads_.reserve(threads);
  for (size_t index = 0; index < threads; ++index) {
    try {
      threads_.emplace_back(&NabtsBlockSlicer::run_worker, this);
    } catch (const std::system_error&) {
      // The system would not give us another thread. The pass runs on the
      // ones it did give, which changes how long it takes and nothing else.
      break;
    }
  }
}

NabtsBlockSlicer::~NabtsBlockSlicer() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    stopping_ = true;
    // A block still being sliced is being abandoned — the caller is unwinding
    // — so the workers stop at the frame they are on.
    next_frame_.store(block_.frame_count, std::memory_order_relaxed);
  }
  block_started_.notify_all();
  for (std::thread& thread : threads_) {
    thread.join();
  }
}

void NabtsBlockSlicer::start(const VideoFrameRepresentation& representation,
                             const NabtsFrameSlicer& slicer,
                             FrameID first_frame, uint64_t first_frame_index,
                             uint64_t frame_count,
                             const NabtsScanSnapshot& snapshot,
                             const std::atomic<bool>* cancel_requested,
                             std::vector<NabtsFieldScan>& out) {
  // Each field's buffer sized here, on the caller's thread, to the lines it
  // will read. Sized to the whole window they held 10 MB across the two
  // blocks a pass keeps, where a pass that has learnt its data lines reads a
  // quarter to a half of them; and sized here, they are allocated and given
  // back on the one thread, rather than spread over every worker's allocator
  // arena and freed back across them block after block.
  for (uint64_t offset = 0; offset < frame_count; ++offset) {
    for (size_t field_idx = 0; field_idx < kNabtsFieldsPerFrame; ++field_idx) {
      out[static_cast<size_t>(offset) * kNabtsFieldsPerFrame + field_idx].fit(
          slicer.lines_to_read(representation, field_idx,
                               first_frame_index + offset, snapshot));
    }
  }
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    block_.representation = &representation;
    block_.slicer = &slicer;
    block_.first_frame = first_frame;
    block_.first_frame_index = first_frame_index;
    block_.frame_count = frame_count;
    block_.snapshot = snapshot;
    block_.cancel_requested = cancel_requested;
    block_.out = &out;
    failure_.clear();
    next_frame_.store(0, std::memory_order_relaxed);
    workers_busy_ = threads_.size();
    ++block_generation_;
  }
  block_started_.notify_all();
}

void NabtsBlockSlicer::finish() {
  slice_frames();

  std::string failure;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    block_finished_.wait(lock, [this] { return workers_busy_ == 0; });
    failure.swap(failure_);
  }
  if (!failure.empty()) {
    throw std::runtime_error(failure);
  }
}

void NabtsBlockSlicer::run_worker() {
  uint64_t seen_generation = 0;
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    block_started_.wait(lock, [this, seen_generation] {
      return stopping_ || block_generation_ != seen_generation;
    });
    if (stopping_) {
      return;
    }
    seen_generation = block_generation_;

    // The block is not touched by start() again until every worker has
    // reported it finished, so it is read without the lock.
    lock.unlock();
    slice_frames();
    lock.lock();

    --workers_busy_;
    if (workers_busy_ == 0) {
      block_finished_.notify_all();
    }
  }
}

void NabtsBlockSlicer::slice_frames() {
  const Block& block = block_;
  try {
    for (;;) {
      const uint64_t offset =
          next_frame_.fetch_add(1, std::memory_order_relaxed);
      if (offset >= block.frame_count) {
        return;
      }
      if (block.cancel_requested != nullptr && block.cancel_requested->load()) {
        return;  // The caller discards the whole block, so stop taking work.
      }
      // A whole frame at a time rather than a field: both fields read the
      // same frame from the source, so taking them together turns what would
      // be two fetches on two threads into one fetch and a cache hit.
      const FrameID frame_id = block.first_frame + static_cast<FrameID>(offset);
      const size_t base = static_cast<size_t>(offset) * kNabtsFieldsPerFrame;
      for (size_t field_idx = 0; field_idx < kNabtsFieldsPerFrame;
           ++field_idx) {
        block.slicer->slice_field(*block.representation, frame_id, field_idx,
                                  block.first_frame_index + offset,
                                  block.snapshot,
                                  (*block.out)[base + field_idx]);
      }
    }
  } catch (const std::exception& e) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (failure_.empty()) {
      failure_ = e.what();
    }
    // Drain the queue so the other workers stop rather than finish a block
    // whose result is about to be thrown away.
    next_frame_.store(block.frame_count, std::memory_order_relaxed);
  }
}

void nabts_slice_block(const VideoFrameRepresentation& representation,
                       const NabtsFrameSlicer& slicer, FrameID first_frame,
                       uint64_t first_frame_index, uint64_t frame_count,
                       const NabtsScanSnapshot& snapshot, size_t worker_count,
                       const std::atomic<bool>* cancel_requested,
                       std::vector<NabtsFieldScan>& out) {
  NabtsBlockSlicer block_slicer(worker_count);
  block_slicer.start(representation, slicer, first_frame, first_frame_index,
                     frame_count, snapshot, cancel_requested, out);
  block_slicer.finish();
}

}  // namespace orc
