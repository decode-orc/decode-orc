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
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

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
// (for an output-side path), never a literal file named "-". Executing it is
// CLI-only — a GUI process has no meaningful stdin/stdout to redirect to —
// but the officially supported workflow is to build the project in the GUI,
// save it, and run it with `orc-cli ... --process`, so the GUI's FILE_PATH
// parameter editor accepts and saves this value. Every GUI-side code path
// that can execute a real stage instance refuses one configured with it
// instead (see docs/technical/plugin-architecture.md's Stdio Piping
// Convention section), so the value reaches a stage only via the CLI.
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

// The stdin counterpart of stdout_binary_stream(), for an iostream-based
// pipe source reading input_path == "-". A source that may be one of several
// readers of stdin in the same run should use open_stdin_reader() instead.
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

// One reader of a stream split by InputSplitter: sees the whole stream from
// its first byte, at its own pace.
//
// Thread safety: read it from one thread at a time, like any std::istream.
// detach() may be called from any thread.
class SharedInputStream : public std::istream {
 public:
  using Queue = BoundedPipeQueue<std::vector<char>>;

  explicit SharedInputStream(std::shared_ptr<Queue> queue)
      : std::istream(nullptr), buf_(queue), queue_(std::move(queue)) {
    rdbuf(&buf_);
  }
  ~SharedInputStream() override { detach(); }

  SharedInputStream(const SharedInputStream&) = delete;
  SharedInputStream& operator=(const SharedInputStream&) = delete;

  // Stops feeding this reader: a read blocked on it returns end of input,
  // and the splitter no longer waits on it before moving on. Idempotent.
  void detach() { queue_->fail(); }

 private:
  class ChunkBuf : public std::streambuf {
   public:
    explicit ChunkBuf(std::shared_ptr<Queue> queue)
        : queue_(std::move(queue)) {}

   protected:
    int_type underflow() override {
      if (gptr() < egptr()) return traits_type::to_int_type(*gptr());
      if (!queue_->pop(chunk_) || chunk_.empty()) return traits_type::eof();
      setg(chunk_.data(), chunk_.data(), chunk_.data() + chunk_.size());
      return traits_type::to_int_type(*gptr());
    }

   private:
    std::shared_ptr<Queue> queue_;
    std::vector<char> chunk_;
  };

  ChunkBuf buf_;
  std::shared_ptr<Queue> queue_;
};

// Reads one input stream exactly once and hands every chunk to each of a
// fixed number of readers — the in-process equivalent of `tee`, for when
// several stage instances in the same process all need the whole of one
// non-seekable stream (stdin).
//
// Reading starts only once `readers` readers have attached, so none misses
// the start of the stream. Each reader has its own bounded queue; a full
// queue blocks the splitter, so the stream advances at the pace of the
// slowest attached reader and memory stays bounded. A detached reader is
// skipped from then on; once every reader has detached, the splitter stops
// reading.
//
// The splitter thread is detached, not joined: it may be blocked in a read
// on the input that nothing can portably interrupt (see the known
// limitation on ThrottledRingReader's users). It owns its state through a
// shared_ptr, so it outlives this object safely and exits on its next read.
//
// Thread safety: attach() may be called concurrently from any thread.
class InputSplitter {
 public:
  InputSplitter(std::istream& input, std::size_t readers,
                std::size_t chunk_bytes = std::size_t{1} << 16,
                std::size_t queue_chunks = 64)
      : state_(std::make_shared<State>(input, readers == 0 ? 1 : readers,
                                       chunk_bytes, queue_chunks)) {}

  InputSplitter(const InputSplitter&) = delete;
  InputSplitter& operator=(const InputSplitter&) = delete;

  // Returns the next reader, or nullptr once `readers` have already
  // attached. The last attach starts the splitter thread.
  std::unique_ptr<SharedInputStream> attach() {
    std::shared_ptr<SharedInputStream::Queue> queue;
    bool start = false;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->queues.size() >= state_->readers) return nullptr;
      queue = std::make_shared<SharedInputStream::Queue>(state_->queue_chunks);
      state_->queues.push_back(queue);
      start = state_->queues.size() == state_->readers;
    }
    if (start) std::thread(&InputSplitter::run, state_).detach();
    return std::make_unique<SharedInputStream>(std::move(queue));
  }

  bool fully_attached() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->queues.size() >= state_->readers;
  }

 private:
  struct State {
    State(std::istream& in, std::size_t reader_count, std::size_t chunk,
          std::size_t depth)
        : input(in),
          readers(reader_count),
          chunk_bytes(chunk),
          queue_chunks(depth) {}
    std::istream& input;
    const std::size_t readers;
    const std::size_t chunk_bytes;
    const std::size_t queue_chunks;
    mutable std::mutex mutex;
    std::vector<std::shared_ptr<SharedInputStream::Queue>> queues;
  };

  static void run(std::shared_ptr<State> state) {
    // Complete from here on: attach() refuses further readers, so the list
    // can be read without the lock.
    const auto& queues = state->queues;
    for (;;) {
      std::vector<char> chunk(state->chunk_bytes);
      state->input.read(chunk.data(),
                        static_cast<std::streamsize>(chunk.size()));
      chunk.resize(static_cast<std::size_t>(state->input.gcount()));

      bool any_attached = false;
      if (!chunk.empty()) {
        for (const auto& queue : queues) {
          if (queue->failed()) continue;
          queue->push(chunk);
          any_attached = true;
        }
      }
      if (chunk.size() < state->chunk_bytes || !any_attached) {
        for (const auto& queue : queues) queue->close_producer();
        return;
      }
    }
  }

  std::shared_ptr<State> state_;
};

// Returns a reader of the process's standard input (binary mode, see
// stdin_binary_stream()). With `readers` <= 1 it reads stdin directly,
// exactly as stdin_binary_stream() would. With more, the first `readers`
// calls share one InputSplitter over stdin, so each of them sees the whole
// stream; this is how a host running several sinks off one piped source
// gives each sink's source instance its own copy.
//
// The splitter is a function-local static, so it is shared by callers in
// the same module (plugin library) only — which is where every source
// instance of a given stage lives.
inline std::unique_ptr<std::istream> open_stdin_reader(std::size_t readers) {
  if (readers <= 1) {
    return std::make_unique<std::istream>(stdin_binary_stream().rdbuf());
  }
  static std::mutex mutex;
  static std::unique_ptr<InputSplitter> splitter;
  std::lock_guard<std::mutex> lock(mutex);
  if (!splitter || splitter->fully_attached()) {
    splitter = std::make_unique<InputSplitter>(stdin_binary_stream(), readers);
  }
  return splitter->attach();
}

}  // namespace pipe_io
}  // namespace orc
