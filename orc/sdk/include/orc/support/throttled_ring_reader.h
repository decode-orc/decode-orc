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
// Outcome of one ProduceFn call. kEof is a clean, expected end of input (a
// forward-only source — live capture or a finite file piped through stdin —
// running out of data) and is never logged as an error; kError is a genuine
// read failure (corrupt/truncated data, an I/O error) and always is. Both
// permanently stop the reader, but a caller cares which one happened: see
// ThrottledRingReader::is_eof() / failed().
enum class ProduceStatus { kOk, kEof, kError };

template <typename T>
class ThrottledRingReader {
 public:
  // Fills `out` with the frame at `frame_index` — called once per index, in
  // strict ascending order starting at 0, never called again once it has
  // returned kOk or kEof, or after returning kError. Returns kOk on success;
  // kEof when the source has cleanly run out of data (no partial/corrupt
  // read — this frame simply never arrives); kError with `error` set on a
  // genuine read failure. Both kEof and kError permanently stop the reader
  // (every current and future get_frame() call then returns nullptr), but
  // only kError is treated as a failure (is_eof() vs failed() tell them
  // apart).
  using ProduceFn = std::function<ProduceStatus(
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
  // of [0, frame_count), the reader has failed or hit a clean end of input
  // before producing `id`, or `id` was requested after it already scrolled
  // out of the buffer window.
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

    cv_.wait(lock, [&] { return failed_ || eof_ || produced_ > id; });

    pending_requests_.erase(pending_requests_.find(id));
    if (!pending_requests_.empty()) {
      read_ahead_floor_ = *pending_requests_.begin();
    }
    // else: leave read_ahead_floor_ at its last value — resetting to 0 would
    // throw away progress already made and force the reader back to square
    // one during any idle stretch between one call returning and the next
    // one starting.
    cv_.notify_all();  // reader_loop() may now have more room

    // A frame already produced stays retrievable even after the reader
    // later fails or hits eof at some higher index — id's data is real and
    // already sitting in the ring, regardless of what stopped the reader
    // afterwards. Only an id the reader never reached (whether because it
    // failed, hit a clean eof, or simply has not gotten there yet) returns
    // nullptr — which failed_/eof_ being true here guarantees it never will.
    if (produced_ <= id) return nullptr;
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

  // True once the producer has reported a clean end of input (ProduceStatus
  // kEof) — as opposed to failed(), a genuine read error. A consumer that
  // treats "no frame at id" as merely a hole (e.g. Frame Map padding) should
  // instead stop outright once this is true: nothing at or beyond the
  // current produced() count will ever arrive.
  bool is_eof() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return eof_;
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

  // Called with mutex_ already held, from reader_loop() only, on a clean
  // ProduceStatus::kEof. Deliberately not an error-level log: this is the
  // designed, expected way an unbounded/live source ends.
  void eof_locked() const {
    if (!eof_ && !failed_) {
      eof_ = true;
      ORC_LOG_INFO("{}: end of input reached after {} frame(s)", log_tag_,
                   produced_);
    }
    cv_.notify_all();
  }

  void reader_loop() {
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        if (stop_ || failed_ || eof_ || produced_ >= frame_count_) return;
        // Wait for room: producing past read_ahead_floor_ + buffer_frames_
        // would only overwrite a ring slot nothing has asked for yet, at the
        // cost of racing arbitrarily far ahead of consumers that have not
        // started yet — see the class comment above.
        cv_.wait(lock, [&] {
          return stop_ || failed_ || eof_ || produced_ >= frame_count_ ||
                 produced_ < read_ahead_floor_ + buffer_frames_;
        });
        if (stop_ || failed_ || eof_ || produced_ >= frame_count_) return;
      }

      // Produce outside the lock: a slow/blocked produce() must not stall
      // other threads' get_frame() lookups against already-produced frames.
      std::vector<T> frame;
      std::string error;
      const ProduceStatus status = produce_(produced_, frame, error);

      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_) return;  // destructor requested shutdown while this ran
      if (status == ProduceStatus::kError) {
        fail_locked(error);
        return;
      }
      if (status == ProduceStatus::kEof) {
        eof_locked();
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
  mutable bool eof_ = false;
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
