/*
 * File:        frame_claim_window.h
 * Module:      orc-stage-plugin-video-sink
 * Purpose:     Bounds how far the export's decode workers run ahead of the
 *              in-order writer
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace orc {

// Workers claim frames in order and decode them in parallel; the writer
// writes them in order. A frame that is slow to decode holds the writer, and
// without a bound the other workers keep decoding the frames after it, each
// held decoded in memory until the slow one is written: on a long export that
// grows without limit.
//
// The window lets a worker claim frame n only while fewer than |max_ahead|
// frames are claimed and not yet written, so at most |max_ahead| decoded
// frames are ever held. The frame the writer waits for has always been
// claimed already (claims are in order), so the window cannot deadlock.
//
// Thread-safety: all members may be called from any thread.
class FrameClaimWindow {
 public:
  FrameClaimWindow(int64_t first_frame, int64_t max_ahead)
      : next_claim_(first_frame),
        next_written_(first_frame),
        max_ahead_(max_ahead < 1 ? 1 : max_ahead) {}

  // Claim the next frame, waiting for room. Returns nullopt as soon as
  // |stop| is true. A frame |unbounded| says to hand out at once (one past
  // the end of a stream, which the worker claims only to learn that) is not
  // held to the bound.
  std::optional<int64_t> claim(
      const std::function<bool()>& stop,
      const std::function<bool(int64_t)>& unbounded = {}) {
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
      if (stop && stop()) return std::nullopt;
      if (next_claim_ - next_written_ < max_ahead_ ||
          (unbounded && unbounded(next_claim_))) {
        return next_claim_++;
      }
      // The writer advancing, an abort and a cancel all notify; the timeout
      // only covers a stop condition that changed without a call to wake().
      room_.wait_for(lock, std::chrono::milliseconds(50));
    }
  }

  // Frames before |next_written| have been written.
  void written(int64_t next_written) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (next_written > next_written_) next_written_ = next_written;
    }
    room_.notify_all();
  }

  // Something a waiting claim's stop condition reads has changed.
  void wake() { room_.notify_all(); }

  int64_t max_ahead() const { return max_ahead_; }

 private:
  std::mutex mutex_;
  std::condition_variable room_;
  int64_t next_claim_;
  int64_t next_written_;
  const int64_t max_ahead_;
};

}  // namespace orc
