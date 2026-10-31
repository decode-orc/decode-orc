/*
 * File:        frame_read_ahead.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Background threads that fetch the frames a serial loop is
 *              about to read, so the caches upstream of it are warm
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <orc/stage/frame_id.h>
#include <orc/stage/video_frame_representation.h>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace orc {

// Runs a serial frame loop's reads ahead of it on worker threads.
//
// A loop that hands each frame to an observer in order (the video sink's VBI
// pass, the analysis sinks) reads one frame at a time. When its input is
// expensive to produce, a stacker most of all, the loop runs at the speed of
// one thread however many the host has. A FrameReadAhead beside the loop
// fetches the frames just ahead of it, which leaves them in the caches of
// the stages upstream; the loop itself is unchanged and finds each frame
// ready. Observers and their order are untouched, so results are identical.
//
// The caller reads `first` itself and calls reached(id) as it starts on each
// later frame. Workers fetch frames in (reached, reached + window], each at
// most once, and skip any the caller has already passed.
//
// The window must stay below the smallest frame cache between the loop and
// the stage that is slow to produce a frame, or frames are evicted before the
// loop reads them and are produced twice. Today the smallest is a TBC
// source's 100 fields (50 frames); Dropout Correction, FM Beat Cancel and a
// CVBS source keep 150 frames and the stacker 300. default_window() is at
// most 32. A stage read more widely than the loop's frames counts that
// reach too: a stacker stacking still frames reads each input up to its
// search range either side.
//
// The destructor stops and joins the workers, so a cancel or an early return
// cannot leave one running. A fetch already under way finishes first.
class FrameReadAhead {
 public:
  using Fetch = std::function<void(FrameID)>;

  static unsigned default_threads() {
    return std::max(1u, std::thread::hardware_concurrency());
  }

  static FrameID default_window(unsigned threads) {
    return std::min<FrameID>(2 * static_cast<FrameID>(threads), 32);
  }

  // A Fetch that leaves |input|'s frame in the caches a reader of it uses:
  // the luma and chroma planes of a YC input, the composite frame otherwise.
  // |input| must outlive the FrameReadAhead.
  static Fetch warm(const VideoFrameRepresentation& input) {
    return [&input](FrameID id) {
      if (!input.has_frame(id)) return;
      if (input.has_separate_channels()) {
        (void)input.get_frame_luma(id);
        (void)input.get_frame_chroma(id);
      } else {
        (void)input.get_frame(id);
      }
    };
  }

  // Fetches frames of [first, last] with |fetch| on |threads| workers, at
  // most |window| ahead of the caller. A window of 0 means
  // default_window(threads). No worker starts when |threads| is 0 or the
  // range is empty.
  FrameReadAhead(FrameID first, FrameID last, Fetch fetch,
                 unsigned threads = default_threads(), FrameID window = 0)
      : last_(last),
        window_(window ? window : default_window(threads)),
        fetch_(std::move(fetch)),
        reached_(first),
        next_(first + 1) {
    if (last < first) return;
    workers_.reserve(threads);
    for (unsigned t = 0; t < threads; ++t) {
      workers_.emplace_back([this] { run(); });
    }
  }

  ~FrameReadAhead() { stop(); }

  FrameReadAhead(const FrameReadAhead&) = delete;
  FrameReadAhead& operator=(const FrameReadAhead&) = delete;

  // The caller is starting on frame |id|.
  void reached(FrameID id) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (id <= reached_) return;
      reached_ = id;
      next_ = std::max(next_, id + 1);
    }
    wake_.notify_all();
  }

  // Stops and joins the workers; later calls do nothing.
  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopping_ = true;
    }
    wake_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) worker.join();
    }
    workers_.clear();
  }

 private:
  void run() {
    for (;;) {
      FrameID id = 0;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait(lock, [this] {
          return stopping_ || next_ > last_ || next_ <= reached_ + window_;
        });
        if (stopping_ || next_ > last_) return;
        id = next_++;
      }
      // Only warming: a frame that fails here fails again for the caller,
      // which reports it.
      try {
        fetch_(id);
      } catch (...) {
      }
    }
  }

  const FrameID last_;
  const FrameID window_;
  const Fetch fetch_;

  std::mutex mutex_;
  std::condition_variable wake_;
  FrameID reached_;
  FrameID next_;
  bool stopping_ = false;
  std::vector<std::thread> workers_;
};

}  // namespace orc
