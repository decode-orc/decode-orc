/*
 * File:        vbi_frame_read_ahead_test.cpp
 * Module:      orc-tests
 * Purpose:     Unit tests for decoding stored frames ahead of sequential
 *              readers
 *
 * Every capture is an in-memory byte pattern opened through the injected
 * opener, so nothing here touches the filesystem, and nothing waits on the
 * clock: where a test needs a decoder held, it holds it on a gate it opens
 * itself.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "vbi_frame_read_ahead.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "vbi_byte_source.h"

namespace orc {
namespace {

constexpr uint64_t kFrameBytes = 100;

// The byte every stored frame holds at every offset: distinct per frame and
// per position, so a frame served from the wrong place cannot pass.
uint8_t pattern_byte(uint64_t frame, uint64_t offset) {
  return static_cast<uint8_t>((frame * 31u + offset * 7u + 3u) & 0xFFu);
}

std::shared_ptr<const std::vector<uint8_t>> make_capture(uint64_t frames) {
  auto bytes = std::make_shared<std::vector<uint8_t>>(frames * kFrameBytes, 0u);
  for (uint64_t frame = 0; frame < frames; ++frame) {
    for (uint64_t offset = 0; offset < kFrameBytes; ++offset) {
      (*bytes)[frame * kFrameBytes + offset] = pattern_byte(frame, offset);
    }
  }
  return bytes;
}

bool holds_frame(const VBIFrameReadAhead::FrameBytes& bytes, uint64_t frame) {
  if (bytes == nullptr || bytes->size() != kFrameBytes) {
    return false;
  }
  for (uint64_t offset = 0; offset < kFrameBytes; ++offset) {
    if ((*bytes)[offset] != pattern_byte(frame, offset)) {
      return false;
    }
  }
  return true;
}

// Holds every read until opened, so a test can keep a decoder busy for as
// long as it needs to without timing anything.
class Gate {
 public:
  void wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++waiting_;
    opened_.wait(lock, [this] { return open_; });
    --waiting_;
  }
  // Reads held at the gate right now.
  int waiting() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return waiting_;
  }
  void open() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      open_ = true;
    }
    opened_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable opened_;
  bool open_ = false;
  int waiting_ = 0;
};

// An in-memory capture, opened afresh for every decoder.
class FakeByteSource : public IVBIByteSource {
 public:
  FakeByteSource(std::shared_ptr<const std::vector<uint8_t>> bytes,
                 std::optional<uint64_t> failing_frame,
                 std::optional<uint64_t> short_frame, Gate* gate,
                 std::shared_ptr<std::atomic<uint64_t>> forward_limit)
      : bytes_(std::move(bytes)),
        failing_frame_(failing_frame),
        short_frame_(short_frame),
        gate_(gate),
        forward_limit_(std::move(forward_limit)) {}

  std::optional<uint64_t> size_bytes() const override {
    return static_cast<uint64_t>(bytes_->size());
  }

  size_t read_at(uint64_t byte_offset, size_t count, uint8_t* out_buffer,
                 std::string& error_message) override {
    if (gate_ != nullptr) {
      gate_->wait();
    }
    if (failing_frame_.has_value() &&
        byte_offset / kFrameBytes == *failing_frame_) {
      error_message = "the transport could not read this frame";
      return 0;
    }
    if (byte_offset >= bytes_->size()) {
      return 0;
    }
    size_t produced =
        std::min(count, bytes_->size() - static_cast<size_t>(byte_offset));
    if (short_frame_.has_value() &&
        byte_offset / kFrameBytes == *short_frame_) {
      // The capture ends, or the transport gives up, part-way through.
      produced /= 2;
    }
    std::memcpy(out_buffer, bytes_->data() + byte_offset, produced);
    return produced;
  }

  void set_forward_decode_limit(uint64_t bytes) override {
    forward_limit_->store(bytes);
  }

 private:
  std::shared_ptr<const std::vector<uint8_t>> bytes_;
  std::optional<uint64_t> failing_frame_;
  std::optional<uint64_t> short_frame_;
  Gate* gate_;
  std::shared_ptr<std::atomic<uint64_t>> forward_limit_;
};

struct Capture {
  std::shared_ptr<const std::vector<uint8_t>> bytes;
  std::optional<uint64_t> failing_frame;
  // A frame the transport delivers only the first half of.
  std::optional<uint64_t> short_frame;
  Gate* gate = nullptr;
  bool open_fails = false;
  std::shared_ptr<std::atomic<int>> opens =
      std::make_shared<std::atomic<int>>(0);
  // The last forward-decode limit a decoder set on its transport.
  std::shared_ptr<std::atomic<uint64_t>> forward_limit =
      std::make_shared<std::atomic<uint64_t>>(UINT64_MAX);

  VBIByteSourceOpener opener() const {
    return [bytes = bytes, failing_frame = failing_frame,
            short_frame = short_frame, gate = gate, open_fails = open_fails,
            opens = opens, forward_limit = forward_limit](
               std::string& error) -> std::unique_ptr<IVBIByteSource> {
      ++*opens;
      if (open_fails) {
        error = "the capture could not be opened again";
        return nullptr;
      }
      return std::make_unique<FakeByteSource>(bytes, failing_frame, short_frame,
                                              gate, forward_limit);
    };
  }
};

VBIReadAheadConfig config_for(uint64_t frames, uint32_t max_decoders = 1) {
  VBIReadAheadConfig config;
  config.frame_bytes = kFrameBytes;
  config.frame_count = frames;
  config.chunk_frames = 8;
  config.trailing_frames = 8;
  config.max_decoders = max_decoders;
  config.sequential_distance = 8;
  return config;
}

// ---------------------------------------------------------------------------
// Chunk size
// ---------------------------------------------------------------------------

TEST(VBIReadAheadChunk, IsAboutFourMebibytesOfFrames) {
  // A .tbc VBI crop frame, a cx23885 frame and a bt8x8 frame.
  EXPECT_EQ(vbi_read_ahead_chunk_frames(58240), 73u);
  EXPECT_EQ(vbi_read_ahead_chunk_frames(34560), 122u);
  EXPECT_EQ(vbi_read_ahead_chunk_frames(65536), 64u);
}

TEST(VBIReadAheadChunk, StaysWithinItsBounds) {
  EXPECT_EQ(vbi_read_ahead_chunk_frames(0), 16u);
  EXPECT_EQ(vbi_read_ahead_chunk_frames(1), 256u);
  EXPECT_EQ(vbi_read_ahead_chunk_frames(64ull * 1024 * 1024), 16u);
}

// ---------------------------------------------------------------------------
// Serving readers
// ---------------------------------------------------------------------------

// A reader moving through the capture in order is found on its second
// request and served every frame after that, each the frame it asked for.
TEST(VBIFrameReadAhead, ASequentialReaderIsServedEveryFrameItAsksFor) {
  constexpr uint64_t kFrames = 100;
  Capture capture{make_capture(kFrames)};
  VBIFrameReadAhead read_ahead(config_for(kFrames), capture.opener());

  // The first request is a random access as far as anyone can tell.
  EXPECT_EQ(read_ahead.fetch(0), nullptr);
  for (uint64_t frame = 1; frame < kFrames; ++frame) {
    const VBIFrameReadAhead::FrameBytes bytes = read_ahead.fetch(frame);
    ASSERT_TRUE(holds_frame(bytes, frame)) << "frame " << frame;
  }

  const VBIFrameReadAhead::Stats stats = read_ahead.stats();
  EXPECT_EQ(stats.served, kFrames - 1);
  EXPECT_EQ(stats.declined, 1u);
  EXPECT_EQ(stats.window_moves, 1u);
  EXPECT_EQ(stats.decoders, 1u);
}

// Several readers working a block of frames at once ask a little out of
// order; every frame any of them is handed is the frame it asked for, and
// only the requests made before the window found them are left to them.
TEST(VBIFrameReadAhead, ReadersWorkingTogetherGetTheFramesTheyAskFor) {
  constexpr uint64_t kFrames = 400;
  constexpr int kReaders = 6;
  Capture capture{make_capture(kFrames)};
  VBIFrameReadAhead read_ahead(config_for(kFrames, 3), capture.opener());

  std::atomic<uint64_t> next{0};
  std::atomic<int> wrong{0};
  std::atomic<uint64_t> served{0};
  std::vector<std::thread> readers;
  for (int reader = 0; reader < kReaders; ++reader) {
    readers.emplace_back([&] {
      for (;;) {
        const uint64_t frame = next.fetch_add(1);
        if (frame >= kFrames) {
          return;
        }
        const VBIFrameReadAhead::FrameBytes bytes = read_ahead.fetch(frame);
        if (bytes != nullptr) {
          ++served;
          if (!holds_frame(bytes, frame)) {
            ++wrong;
          }
        }
      }
    });
  }
  for (std::thread& reader : readers) {
    reader.join();
  }

  EXPECT_EQ(wrong.load(), 0);
  const VBIFrameReadAhead::Stats stats = read_ahead.stats();
  EXPECT_EQ(stats.served + stats.declined, kFrames);
  EXPECT_EQ(stats.served, served.load());
  EXPECT_GT(stats.served, kFrames / 2);
  EXPECT_GE(stats.decoders, 1u);
  EXPECT_LE(stats.decoders, 3u);
}

// A frame is handed out once: its reader keeps what it makes of the frame, so
// the window lets the bytes go with it, and a second request for the frame is
// the caller's to read.
TEST(VBIFrameReadAhead, AFrameIsHandedOutOnce) {
  constexpr uint64_t kFrames = 40;
  Capture capture{make_capture(kFrames)};
  VBIFrameReadAhead read_ahead(config_for(kFrames), capture.opener());

  EXPECT_EQ(read_ahead.fetch(0), nullptr);
  ASSERT_TRUE(holds_frame(read_ahead.fetch(1), 1));
  ASSERT_TRUE(holds_frame(read_ahead.fetch(2), 2));
  EXPECT_EQ(read_ahead.fetch(1), nullptr);
  ASSERT_TRUE(holds_frame(read_ahead.fetch(3), 3));

  const VBIFrameReadAhead::Stats stats = read_ahead.stats();
  EXPECT_EQ(stats.served, 3u);
  EXPECT_EQ(stats.declined, 2u);
  EXPECT_EQ(stats.window_moves, 1u);
}

// A random access — a preview jumping to a frame — is left to the caller and
// does not move the window away from the reader it serves.
TEST(VBIFrameReadAhead, ARandomAccessIsLeftToTheCallerAndMovesNothing) {
  constexpr uint64_t kFrames = 200;
  Capture capture{make_capture(kFrames)};
  VBIFrameReadAhead read_ahead(config_for(kFrames), capture.opener());

  EXPECT_EQ(read_ahead.fetch(10), nullptr);
  ASSERT_TRUE(holds_frame(read_ahead.fetch(11), 11));

  EXPECT_EQ(read_ahead.fetch(150), nullptr);
  EXPECT_EQ(read_ahead.stats().window_moves, 1u);

  for (uint64_t frame = 12; frame < 40; ++frame) {
    ASSERT_TRUE(holds_frame(read_ahead.fetch(frame), frame)) << frame;
  }
  EXPECT_EQ(read_ahead.stats().window_moves, 1u);
}

// Two requests close together outside the window are a reader, and the
// window moves to it.
TEST(VBIFrameReadAhead, AReaderFoundElsewhereTakesTheWindow) {
  constexpr uint64_t kFrames = 300;
  Capture capture{make_capture(kFrames)};
  VBIFrameReadAhead read_ahead(config_for(kFrames), capture.opener());

  EXPECT_EQ(read_ahead.fetch(0), nullptr);
  ASSERT_TRUE(holds_frame(read_ahead.fetch(1), 1));

  EXPECT_EQ(read_ahead.fetch(200), nullptr);
  ASSERT_TRUE(holds_frame(read_ahead.fetch(201), 201));
  ASSERT_TRUE(holds_frame(read_ahead.fetch(202), 202));
  EXPECT_EQ(read_ahead.stats().window_moves, 2u);
}

// A request just behind the window is a reader straggling behind the pass:
// it is left to the caller, and however many of them there are they never
// pull the window back.
TEST(VBIFrameReadAhead, StragglersBehindTheWindowNeverPullItBack) {
  constexpr uint64_t kFrames = 200;
  Capture capture{make_capture(kFrames)};
  VBIFrameReadAhead read_ahead(config_for(kFrames), capture.opener());

  EXPECT_EQ(read_ahead.fetch(0), nullptr);
  for (uint64_t frame = 1; frame <= 60; ++frame) {
    ASSERT_TRUE(holds_frame(read_ahead.fetch(frame), frame)) << frame;
  }
  // The window trails the furthest request by eight frames.
  EXPECT_EQ(read_ahead.fetch(50), nullptr);
  EXPECT_EQ(read_ahead.fetch(49), nullptr);
  EXPECT_EQ(read_ahead.fetch(48), nullptr);
  EXPECT_EQ(read_ahead.stats().window_moves, 1u);
  ASSERT_TRUE(holds_frame(read_ahead.fetch(61), 61));
}

TEST(VBIFrameReadAhead, TheLastFramesOfTheCaptureAreServedAndNoneBeyond) {
  constexpr uint64_t kFrames = 21;  // not a whole number of chunks
  Capture capture{make_capture(kFrames)};
  VBIFrameReadAhead read_ahead(config_for(kFrames), capture.opener());

  EXPECT_EQ(read_ahead.fetch(0), nullptr);
  for (uint64_t frame = 1; frame < kFrames; ++frame) {
    ASSERT_TRUE(holds_frame(read_ahead.fetch(frame), frame)) << frame;
  }
  EXPECT_EQ(read_ahead.fetch(kFrames), nullptr);
  EXPECT_EQ(read_ahead.fetch(kFrames + 1), nullptr);
}

// The decoders run a bounded distance ahead of the furthest reader, so the
// window's memory is bounded however slowly the reader goes.
TEST(VBIFrameReadAhead, DecodesNoFurtherAheadThanItsWindow) {
  constexpr uint64_t kFrames = 10000;
  Capture capture{make_capture(kFrames)};
  const VBIReadAheadConfig config = config_for(kFrames, 2);
  VBIFrameReadAhead read_ahead(config, capture.opener());

  EXPECT_EQ(read_ahead.fetch(0), nullptr);
  for (uint64_t frame = 1; frame <= 50; ++frame) {
    ASSERT_TRUE(holds_frame(read_ahead.fetch(frame), frame)) << frame;
  }
  const uint64_t capacity =
      config.trailing_frames +
      (static_cast<uint64_t>(config.max_decoders) + 1) * config.chunk_frames;
  EXPECT_LE(read_ahead.stats().frames_decoded, 50u + capacity);
}

TEST(VBIFrameReadAhead, NeverRunsMoreDecodersThanItIsAllowed) {
  constexpr uint64_t kFrames = 600;
  Capture capture{make_capture(kFrames)};
  VBIFrameReadAhead read_ahead(config_for(kFrames, 1), capture.opener());

  std::atomic<uint64_t> next{0};
  std::vector<std::thread> readers;
  for (int reader = 0; reader < 8; ++reader) {
    readers.emplace_back([&] {
      for (uint64_t frame = next.fetch_add(1); frame < kFrames;
           frame = next.fetch_add(1)) {
        (void)read_ahead.fetch(frame);
      }
    });
  }
  for (std::thread& reader : readers) {
    reader.join();
  }
  EXPECT_EQ(read_ahead.stats().decoders, 1u);
  EXPECT_EQ(*capture.opens, 1);
}

// Readers wait whenever they work at the front of the window, whether or not
// another decoder would bring them their frames any sooner, so however long
// they wait no more decoders run than one for every readers_per_decoder
// threads asking.
TEST(VBIFrameReadAhead, RunsNoMoreDecodersThanItsReadersCallFor) {
  constexpr uint64_t kFrames = 3000;
  constexpr int kReaders = 8;
  Capture capture{make_capture(kFrames)};
  VBIReadAheadConfig config = config_for(kFrames, 8);
  config.readers_per_decoder = 4;
  VBIFrameReadAhead read_ahead(config, capture.opener());

  std::atomic<uint64_t> next{0};
  std::atomic<int> wrong{0};
  std::vector<std::thread> readers;
  for (int reader = 0; reader < kReaders; ++reader) {
    readers.emplace_back([&] {
      for (uint64_t frame = next.fetch_add(1); frame < kFrames;
           frame = next.fetch_add(1)) {
        const VBIFrameReadAhead::FrameBytes bytes = read_ahead.fetch(frame);
        if (bytes != nullptr && !holds_frame(bytes, frame)) {
          ++wrong;
        }
      }
    });
  }
  for (std::thread& reader : readers) {
    reader.join();
  }

  EXPECT_EQ(wrong.load(), 0);
  const VBIFrameReadAhead::Stats stats = read_ahead.stats();
  EXPECT_GE(stats.readers, 1u);
  EXPECT_LE(stats.readers, static_cast<uint32_t>(kReaders));
  EXPECT_LE(stats.decoders, (stats.readers + 3u) / 4u);
  EXPECT_LE(stats.decoders, 2u);
}

// A reader is counted once however many frames it asks for, and afresh by
// each read-ahead it asks of: one thread reading two captures in turn is one
// reader of each, and is served by one decoder of each.
TEST(VBIFrameReadAhead, AThreadIsOneReaderOfEachReadAheadItAsks) {
  constexpr uint64_t kFrames = 200;
  Capture first_capture{make_capture(kFrames)};
  Capture second_capture{make_capture(kFrames)};
  VBIReadAheadConfig config = config_for(kFrames, 4);
  config.readers_per_decoder = 1;
  VBIFrameReadAhead first(config, first_capture.opener());
  VBIFrameReadAhead second(config, second_capture.opener());

  EXPECT_EQ(first.fetch(0), nullptr);
  EXPECT_EQ(second.fetch(0), nullptr);
  for (uint64_t frame = 1; frame < kFrames; ++frame) {
    ASSERT_TRUE(holds_frame(first.fetch(frame), frame)) << frame;
    ASSERT_TRUE(holds_frame(second.fetch(frame), frame)) << frame;
  }
  EXPECT_EQ(first.stats().readers, 1u);
  EXPECT_EQ(second.stats().readers, 1u);
  EXPECT_EQ(first.stats().decoders, 1u);
  EXPECT_EQ(second.stats().decoders, 1u);
}

// The frames' buffers are recycled, so a pass over a long capture allocates
// no more of them than its window holds at once, rather than one per frame.
TEST(VBIFrameReadAhead, AllocatesNoMoreFrameBuffersThanItsWindowHolds) {
  constexpr uint64_t kFrames = 1000;
  Capture capture{make_capture(kFrames)};
  const VBIReadAheadConfig config = config_for(kFrames, 1);
  VBIFrameReadAhead read_ahead(config, capture.opener());

  EXPECT_EQ(read_ahead.fetch(0), nullptr);
  for (uint64_t frame = 1; frame < kFrames; ++frame) {
    ASSERT_TRUE(holds_frame(read_ahead.fetch(frame), frame)) << frame;
  }
  // The window's frames, one more in the decoder's hands and the one the
  // reader holds.
  const uint64_t window =
      config.trailing_frames +
      (static_cast<uint64_t>(config.max_decoders) + 1) * config.chunk_frames;
  EXPECT_LE(read_ahead.stats().buffers, window + 2);
}

// A decoder skips the chunks the others take, so it asks its transport to
// reposition over a gap rather than decode through it.
TEST(VBIFrameReadAhead, DecodersAskTheirTransportNotToDecodeThroughGaps) {
  constexpr uint64_t kFrames = 40;
  Capture capture{make_capture(kFrames)};
  VBIFrameReadAhead read_ahead(config_for(kFrames), capture.opener());

  EXPECT_EQ(read_ahead.fetch(0), nullptr);
  ASSERT_TRUE(holds_frame(read_ahead.fetch(1), 1));
  EXPECT_EQ(capture.forward_limit->load(), 0u);
}

// ---------------------------------------------------------------------------
// Failure and shutdown
// ---------------------------------------------------------------------------

// A frame a decoder cannot read is left to the caller, whose own read reports
// the transport's error; the frames around it are still served.
TEST(VBIFrameReadAhead, AFrameTheDecoderCannotReadIsLeftToTheCaller) {
  constexpr uint64_t kFrames = 40;
  Capture capture{make_capture(kFrames), /*failing_frame=*/uint64_t{7}};
  VBIFrameReadAhead read_ahead(config_for(kFrames), capture.opener());

  EXPECT_EQ(read_ahead.fetch(0), nullptr);
  for (uint64_t frame = 1; frame < kFrames; ++frame) {
    const VBIFrameReadAhead::FrameBytes bytes = read_ahead.fetch(frame);
    if (frame == 7) {
      EXPECT_EQ(bytes, nullptr);
    } else {
      ASSERT_TRUE(holds_frame(bytes, frame)) << "frame " << frame;
    }
  }
}

// A buffer is reused for frame after frame, so a read that fills only part of
// one must never be served: the rest of it still holds an earlier frame. The
// frame is left to the caller, whose own read reports what went wrong.
TEST(VBIFrameReadAhead, AFrameReadOnlyInPartIsLeftToTheCaller) {
  constexpr uint64_t kFrames = 200;
  Capture capture{make_capture(kFrames)};
  capture.short_frame = uint64_t{150};
  VBIFrameReadAhead read_ahead(config_for(kFrames), capture.opener());

  EXPECT_EQ(read_ahead.fetch(0), nullptr);
  for (uint64_t frame = 1; frame < kFrames; ++frame) {
    const VBIFrameReadAhead::FrameBytes bytes = read_ahead.fetch(frame);
    if (frame == 150) {
      EXPECT_EQ(bytes, nullptr);
    } else {
      ASSERT_TRUE(holds_frame(bytes, frame)) << "frame " << frame;
    }
  }
  // Long since recycling: the window holds 24 frames.
  EXPECT_LT(read_ahead.stats().buffers, 150u);
}

// A capture that cannot be opened a second time leaves every read to the
// caller, and a reader already waiting on the window is released.
TEST(VBIFrameReadAhead,
     ACaptureThatCannotBeOpenedAgainLeavesEveryReadToTheCaller) {
  constexpr uint64_t kFrames = 40;
  Capture capture{make_capture(kFrames)};
  capture.open_fails = true;
  VBIFrameReadAhead read_ahead(config_for(kFrames), capture.opener());

  EXPECT_EQ(read_ahead.fetch(0), nullptr);
  EXPECT_EQ(read_ahead.fetch(1), nullptr);
  for (uint64_t frame = 2; frame < 10; ++frame) {
    EXPECT_EQ(read_ahead.fetch(frame), nullptr);
  }
  EXPECT_EQ(read_ahead.stats().served, 0u);
}

// stop() releases a reader waiting on a frame a decoder is still reading, and
// the read-ahead declines everything after it.
TEST(VBIFrameReadAhead, StopReleasesAWaitingReaderAndDeclinesFromThenOn) {
  constexpr uint64_t kFrames = 40;
  Gate gate;
  Capture capture{make_capture(kFrames)};
  capture.gate = &gate;
  VBIFrameReadAhead read_ahead(config_for(kFrames), capture.opener());

  std::atomic<bool> released{false};
  VBIFrameReadAhead::FrameBytes waited_for;
  std::thread reader([&] {
    (void)read_ahead.fetch(0);
    // The decoder is held on the gate, so this waits until stop().
    waited_for = read_ahead.fetch(1);
    released = true;
  });

  std::thread stopper([&] { read_ahead.stop(); });
  reader.join();
  EXPECT_TRUE(released.load());
  EXPECT_EQ(waited_for, nullptr);

  // The decoder is still inside its read; let it out so stop() can join it.
  gate.open();
  stopper.join();

  EXPECT_EQ(read_ahead.fetch(2), nullptr);
}

// Readers that outrun the decoders wait on frames no decoder has claimed yet.
// When another reader carries the window past such a frame before a decoder
// reaches it, the claim pointer jumps over the frame and nothing would ever
// signal its slot again, so the reader waiting on it has to be released there
// and then, to read the frame itself. Left waiting, it held a NABTS pass for
// days on a 192-thread machine: a block's workers spread over more frames than
// the trailing margin while a slow mount kept the decoders behind them.
TEST(VBIFrameReadAhead, AReaderWaitingOnAnUnclaimedFrameIsReleasedWhenPassed) {
  constexpr uint64_t kFrames = 400;
  Gate gate;
  Capture capture{make_capture(kFrames)};
  capture.gate = &gate;
  // One decoder, chunks of 8, a trailing margin of 8: a 24-frame window.
  VBIFrameReadAhead read_ahead(config_for(kFrames, 1), capture.opener());

  // The reader is found at frame 0. The decoder claims frames 0-7 and is held
  // at the gate inside frame 0, and the request for frame 1 waits on it.
  EXPECT_EQ(read_ahead.fetch(0), nullptr);
  std::thread first([&] { (void)read_ahead.fetch(1); });
  while (gate.waiting() == 0 || read_ahead.stats().window_moves == 0) {
    std::this_thread::yield();
  }

  // Frame 12 is in the window and unclaimed. Asking for it moves the base to
  // 4, which releases frame 1 (the first reader gives up: a second decline)
  // and leaves this reader waiting on a frame nobody has claimed.
  std::promise<VBIFrameReadAhead::FrameBytes> waited;
  std::future<VBIFrameReadAhead::FrameBytes> result = waited.get_future();
  std::thread ahead([&] { waited.set_value(read_ahead.fetch(12)); });
  while (read_ahead.stats().declined < 2) {
    std::this_thread::yield();
  }

  // A reader further on carries the base to 13, past frame 12.
  std::thread further([&] { (void)read_ahead.fetch(21); });

  // Frame 12 is no longer the window's to deliver, and its reader hears so at
  // once, not when some later frame happens to land in its slot (frame 36,
  // once the gate opens). The bound turns the old behaviour into a failure
  // rather than a hang.
  const bool released =
      result.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
  gate.open();
  first.join();
  ahead.join();
  further.join();
  ASSERT_TRUE(released)
      << "a reader waiting on a frame the window passed was never woken";
  EXPECT_EQ(result.get(), nullptr);
}

// Destroying the read-ahead with decoders running and the window full joins
// them cleanly.
TEST(VBIFrameReadAhead, IsDestroyedCleanlyMidPass) {
  constexpr uint64_t kFrames = 5000;
  Capture capture{make_capture(kFrames)};
  {
    VBIFrameReadAhead read_ahead(config_for(kFrames, 2), capture.opener());
    EXPECT_EQ(read_ahead.fetch(0), nullptr);
    for (uint64_t frame = 1; frame < 20; ++frame) {
      ASSERT_TRUE(holds_frame(read_ahead.fetch(frame), frame)) << frame;
    }
  }
  SUCCEED();
}

}  // namespace
}  // namespace orc
