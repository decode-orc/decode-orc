/*
 * File:        pipe_io.h
 * Module:      decode-orc Plugin SDK (support tier)
 * Purpose:     Shared convention and utilities for stages that support
 *              piping through stdin/stdout instead of a real file
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

// SDK TIER: support — compiled-into-plugin utility. NOT part of the binary
// ABI; changes never force an ABI bump (recompile the plugin at your leisure).

#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <queue>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

// See docs/technical/plugin-architecture.md for the full "-" stdio
// convention this header implements, including why it is CLI-only and how
// the host guards against two nodes claiming the same stdio stream.

namespace orc {
namespace pipe_io {

// The decode-orc convention for a FILE_PATH parameter: "-" means the CLI
// process's own standard input (for an input-side path) or standard output
// (for an output-side path), never a literal file named "-". CLI-only — a
// GUI process has no meaningful stdin/stdout to redirect to, so the GUI's
// FILE_PATH parameter editor rejects this value before it ever reaches a
// stage.
inline constexpr const char kStdioPathToken[] = "-";

// True when `path` is either the "-" stdio convention or an actual POSIX
// named pipe already on disk. is_fifo() only recognises POSIX FIFOs — a
// Windows named pipe (\\.\pipe\...) falls through to false here and is
// handled exactly like a regular file/URL by whatever backend opens it.
//
// A stage uses this to decide whether it can honour its current output_path
// (or input_path) at all: streaming to a pipe rules out formats that need to
// seek back and rewrite a header/index at the end (e.g. plain MP4).
inline bool is_pipe_path(const std::string& path) {
  if (path == kStdioPathToken) return true;
  std::error_code ec;
  return std::filesystem::is_fifo(path, ec) && !ec;
}

// True when `path` is a live network destination one of libav's own
// protocol handlers opens directly — udp, rtmp(s), rtp, srt, tcp. Recognised
// separately from is_pipe_path() because only a libav-backed backend
// (avio_open()/avformat_alloc_output_context2(), via to_libav_io_url() below)
// can actually open one of these; an iostream-based backend
// (raw_output_backend, cvbs_stream_source's stdin reader) has no way to write
// or read a network socket and must keep checking for the literal "-" token
// instead.
//
// Shares the same non-seekable restriction as "-", though: none of these
// protocols support seeking back to patch an earlier-written header, so a
// stage or container that needs random access is equally unsafe here — see
// is_container_pipe_safe() in ffmpeg_output_backend.cpp and
// IStreamingCompatibility, both of which check this alongside is_pipe_path().
inline bool is_network_stream_url(const std::string& path) {
  static const char* const kSchemes[] = {"udp://", "rtmp://", "rtmps://",
                                         "rtp://", "srt://",  "tcp://"};
  for (const char* scheme : kSchemes) {
    if (path.rfind(scheme, 0) == 0) return true;
  }
  return false;
}

// Which end of the pipe "-" should resolve to for a libav-based backend.
enum class StdioDirection { INPUT, OUTPUT };

// Translates the "-" convention into libav's own pipe URL scheme. Passed
// straight to avio_open()/avformat_alloc_output_context2(), a literal "-"
// would try (and typically fail, or worse, silently create) a file actually
// named "-" in the working directory — bytes would never reach the shell
// pipe on the other end. libav's "pipe:" protocol is the portable way to
// mean stdin/stdout on both POSIX and Windows.
//
// Any other path — including a real named pipe — is returned unchanged:
// avio_open() already opens those directly by path.
inline std::string to_libav_io_url(const std::string& path,
                                   StdioDirection direction) {
  if (path != kStdioPathToken) return path;
  return direction == StdioDirection::INPUT ? "pipe:0" : "pipe:1";
}

// For a backend that writes through iostreams rather than libav's own I/O
// layer (see to_libav_io_url() for the avio_open() alternative): returns
// std::cout after ensuring the process's real stdout is in binary mode.
// Call this instead of writing to std::cout directly whenever output_path
// is exactly the "-" token — otherwise, on Windows, the CRT's text-mode
// CR/LF translation silently corrupts binary output. A no-op beyond
// returning std::cout on POSIX, where standard streams are already binary.
// Safe to call more than once; the mode switch happens at most once.
inline std::ostream& stdout_binary_stream() {
#if defined(_WIN32)
  static const int ignored = (_setmode(_fileno(stdout), _O_BINARY), 0);
  (void)ignored;
#endif
  return std::cout;
}

// The stdin counterpart of stdout_binary_stream(), for a future
// iostream-based pipe source reading input_path == "-".
inline std::istream& stdin_binary_stream() {
#if defined(_WIN32)
  static const int ignored = (_setmode(_fileno(stdin), _O_BINARY), 0);
  (void)ignored;
#endif
  return std::cin;
}

// Bounded producer/consumer queue decoupling a decode/encode loop from a
// downstream writer. Streaming to a pipe means the writer's pace is set by
// whatever is reading the other end (e.g. `| ffplay -`); writing each chunk
// synchronously from the encode loop would block the next chunk's encode on
// that chunk's *playback* time on top of its own encode time, instead of the
// two overlapping. Bounding the queue caps how far the producer can run
// ahead of a slow consumer — it can get ahead, just not arbitrarily far
// ahead.
//
// Thread safety: push()/close_producer() are for the producer thread only;
// pop() is for the consumer thread only. fail() may be called from either.
template <typename T>
class BoundedPipeQueue {
 public:
  explicit BoundedPipeQueue(std::size_t max_depth) : max_depth_(max_depth) {}

  // Blocks while the queue is already at max_depth and neither closed nor
  // failed. Silently drops `item` if the queue has failed.
  void push(T item) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return queue_.size() < max_depth_ || failed_; });
    if (failed_) return;
    queue_.push(std::move(item));
    lock.unlock();
    cv_.notify_all();
  }

  // Signals that no more items will be pushed. Wakes a consumer blocked in
  // pop() so it can drain what remains and then return.
  void close_producer() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      producer_done_ = true;
    }
    cv_.notify_all();
  }

  // Marks the queue failed: push() stops blocking (and drops the item) and
  // pop() stops blocking (and returns false), so producer and consumer can
  // both unwind without deadlocking on a partner that already gave up.
  void fail() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      failed_ = true;
    }
    cv_.notify_all();
  }

  bool failed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
  }

  // Blocks until an item is available, the producer has closed with nothing
  // left queued, or the queue has failed. Returns false in the latter two
  // cases (call failed() to tell them apart); otherwise moves the next item
  // into `out` and returns true.
  bool pop(T& out) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock,
             [&] { return !queue_.empty() || producer_done_ || failed_; });
    if (failed_ || queue_.empty()) return false;
    out = std::move(queue_.front());
    queue_.pop();
    lock.unlock();
    cv_.notify_all();
    return true;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<T> queue_;
  const std::size_t max_depth_;
  bool producer_done_ = false;
  bool failed_ = false;
};

}  // namespace pipe_io
}  // namespace orc
