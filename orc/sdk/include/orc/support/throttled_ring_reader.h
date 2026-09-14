/*
 * File:        throttled_ring_reader.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Generic bounded-ring-buffer, throttled-read-ahead reader for
 *              a pipe-compatible sequential source stage
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/support/logging.h>

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace orc {
namespace pipe_io {

// Background-thread producer + bounded ring buffer behind a pipe-compatible
// sequential source stage — the shared machinery cvbs_stream_source and
// tbc_stream_source each build their own frame reader on top of (see either
// plugin's own reader class for the specific "how do I decode frame N"
// logic each plugs in via `produce`).
//
// A single background thread calls `produce(i, out, error)` for i = 0, 1,
// 2, ... in strict order, each call filling one ring slot; any number of
// caller threads call get_frame(id) to block until that slot is ready and
// read it back. Only the most recent `buffer_frames` results are kept —
// get_frame() fails once its slot has been overwritten — so this is for a
// genuinely forward-only, non-seekable input (see the "-" convention in
// orc/support/pipe_io.h), never for anything read out of a wide or
// unpredictable range.
//
// Throttling: production only ever runs `buffer_frames` ahead of the LOWEST
// frame id any thread currently has a get_frame() call blocked on (or ahead
// of 0, before the first call ever arrives) — never of the highest id ever
// requested. An earlier version of the reader this class replaces (see
// cvbs_stream_source's history) throttled on the highest id instead, which
// let one thread's fast, high-id request "unlock" the reader to race past a
// different thread's still-pending low-id one, evicting a frame that was
// never actually late. Gating on the minimum of what is genuinely
// outstanding right now makes that impossible: a straggler thread simply
// keeps the floor pinned at its own id until it is served, and nothing lets
// the reader jump ahead of it early. Without this throttle at all — the
// reader's original design — production could race to completion before a
// single get_frame() call ever arrived (e.g. while a worker pool was still
// constructing decoders), evicting every early frame before anyone had a
// chance to ask; the floor defaults to 0 specifically to still bound that
// case, rather than allowing unlimited speculative read-ahead.
template <typename T>
class ThrottledRingReader {
 public:
  // Fills `out` with the frame at `frame_index` — called once per index, in
  // strict ascending order starting at 0, never called again once it has
  // returned (successfully or not). Returns true on success; returns false
  // with `error` set on a genuine read failure, which permanently fails the
  // reader (every current and future get_frame() call then returns nullptr).
  using ProduceFn = std::function<bool(
      std::size_t frame_index, std::vector<T>& out, std::string& error)>;

  // `log_tag` prefixes every logged failure (e.g. "CVBSStreamReader",
  // "TBCStreamReader") so a shared component's log lines still read as
  // belonging to the specific reader that hit them.
  ThrottledRingReader(ProduceFn produce, std::size_t frame_count,
                      std::size_t buffer_frames,
                      std::string log_tag = "ThrottledRingReader")
      : produce_(std::move(produce)),
        frame_count_(frame_count),
        buffer_frames_(buffer_frames == 0 ? 1 : buffer_frames),
        log_tag_(std::move(log_tag)) {
    ring_.resize(buffer_frames_);
    thread_ = std::thread(&ThrottledRingReader::reader_loop, this);
  }

  ~ThrottledRingReader() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
  }

  ThrottledRingReader(const ThrottledRingReader&) = delete;
  ThrottledRingReader& operator=(const ThrottledRingReader&) = delete;

  // Blocks until frame `id`'s samples are available. Returned pointer is
  // valid until the frame scrolls out of the ring buffer or this object is
  // destroyed — do not retain across calls. Returns nullptr when `id` is out
  // of [0, frame_count), the reader has failed, or `id` was requested after
  // it already scrolled out of the buffer window.
  const T* get_frame(std::size_t id) const {
    if (id >= frame_count_) return nullptr;

    std::unique_lock<std::mutex> lock(mutex_);
    // Register as pending before waiting so reader_loop() knows not to race
    // past this id — see the class comment above. Inserting can only lower
    // (or leave unchanged) the floor, so this is safe to do unconditionally
    // before checking anything else.
    pending_requests_.insert(id);
    read_ahead_floor_ = *pending_requests_.begin();
    cv_.notify_all();  // reader_loop() may now have less room than it thought

    cv_.wait(lock, [&] { return failed_ || produced_ > id; });

    pending_requests_.erase(pending_requests_.find(id));
    if (!pending_requests_.empty()) {
      read_ahead_floor_ = *pending_requests_.begin();
    }
    // else: leave read_ahead_floor_ at its last value — resetting to 0 would
    // throw away progress already made and force the reader back to square
    // one during any idle stretch between one call returning and the next
    // one starting.
    cv_.notify_all();  // reader_loop() may now have more room

    if (failed_) return nullptr;
    if (produced_ - id > buffer_frames_) {
      fail_locked("frame " + std::to_string(id) +
                  " requested after it scrolled out of the " +
                  std::to_string(buffer_frames_) +
                  "-frame buffer (reader is now at frame " +
                  std::to_string(produced_) +
                  "); increase buffer_frames if this access pattern is "
                  "legitimate");
      return nullptr;
    }
    return ring_[id % buffer_frames_].data();
  }

  bool failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
  }

  std::string last_error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

 private:
  // Called with mutex_ already held, from either get_frame() (const) or
  // reader_loop() (non-const) — both hand it the same lock, so this stays
  // logically const from the caller's point of view even though it mutates
  // the (mutable) failure state.
  void fail_locked(const std::string& message) const {
    if (!failed_) {
      failed_ = true;
      error_ = message;
      ORC_LOG_ERROR("{}: {}", log_tag_, message);
    }
    cv_.notify_all();
  }

  void reader_loop() {
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stop_ || failed_ || produced_ >= frame_count_) return;
        // Wait for room: producing past read_ahead_floor_ + buffer_frames_
        // would only overwrite a ring slot nothing has asked for yet, at the
        // cost of racing arbitrarily far ahead of consumers that have not
        // started yet — see the class comment above.
        cv_.wait(lock, [&] {
          return stop_ || failed_ || produced_ >= frame_count_ ||
                 produced_ < read_ahead_floor_ + buffer_frames_;
        });
        if (stop_ || failed_ || produced_ >= frame_count_) return;
      }

      // Produce outside the lock: a slow/blocked produce() must not stall
      // other threads' get_frame() lookups against already-produced frames.
      std::vector<T> frame;
      std::string error;
      const bool ok = produce_(produced_, frame, error);

      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) return;  // destructor requested shutdown while this ran
      if (!ok) {
        fail_locked(error);
        return;
      }
      ring_[produced_ % buffer_frames_] = std::move(frame);
      ++produced_;
      cv_.notify_all();
    }
  }

  ProduceFn produce_;
  const std::size_t frame_count_;
  const std::size_t buffer_frames_;
  const std::string log_tag_;

  mutable std::mutex mutex_;
  mutable std::condition_variable cv_;
  std::vector<std::vector<T>> ring_;
  std::size_t produced_ = 0;
  mutable bool failed_ = false;
  mutable std::string error_;
  bool stop_ = false;
  std::thread thread_;

  // IDs currently being waited on inside get_frame() — see get_frame()'s own
  // comment. read_ahead_floor_ tracks the minimum of this set.
  mutable std::multiset<std::size_t> pending_requests_;
  mutable std::size_t read_ahead_floor_ = 0;
};

}  // namespace pipe_io
}  // namespace orc
