/*
 * File:        nabts_block_scanner_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for slicing blocks of frames on the NABTS sink's
 *              thread pool
 *
 * Covers: a block sliced on the pool is the block sliced on one thread, block
 * after block; each field's buffer holds the lines it reads; a worker's
 * failure reaches the caller; a cancelled block stops; start() hands the
 * block over without waiting for it; and an abandoned block is joined
 * cleanly. Every line of video is synthesised in memory, no I/O is
 * performed, and where a test needs a worker held it is held on a gate the
 * test opens, never on the clock.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "nabts_block_scanner.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <vector>

#include "../../common/vbi-services/teletext_line_synthesizer.h"

namespace orc {
namespace {

// The lines of the default window, each field
// (kNabtsFirstFieldLine-kNabtsLastFieldLine).
constexpr size_t kWindowLines = static_cast<size_t>(
    orc::kNabtsLastFieldLine - orc::kNabtsFirstFieldLine + 1);

// Holds every line read until opened.
class Gate {
 public:
  void wait() const {
    std::unique_lock<std::mutex> lock(mutex_);
    opened_.wait(lock, [this] { return open_; });
  }
  void open() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      open_ = true;
    }
    opened_.notify_all();
  }

 private:
  mutable std::mutex mutex_;
  mutable std::condition_variable opened_;
  bool open_ = false;
};

// A 525-line recording in memory. Field line 14 of field 1 carries a NABTS
// packet on two frames in three, its continuity index the frame number, so
// no two neighbouring frames slice alike; every other line is blanking.
class SyntheticRecording final : public VideoFrameRepresentation {
 public:
  explicit SyntheticRecording(uint64_t frames) : frames_(frames) {
    for (int continuity = 0; continuity < 16; ++continuity) {
      packets_.push_back(tests::synthesize_nabts_line(
          tests::make_nabts_test_payload(0x123, continuity)));
    }
    blank_.assign(static_cast<size_t>(kNtscSamplesPerLine),
                  static_cast<int16_t>(kNtscBlanking));
  }

  FrameIDRange frame_range() const override {
    return FrameIDRange{0, static_cast<FrameID>(frames_ - 1)};
  }
  size_t frame_count() const override { return frames_; }
  bool has_frame(FrameID id) const override { return id < frames_; }
  std::optional<FrameDescriptor> get_frame_descriptor(
      FrameID /*id*/) const override {
    return std::nullopt;
  }
  const sample_type* get_frame(FrameID /*id*/) const override {
    return nullptr;
  }
  std::vector<sample_type> get_frame_copy(FrameID /*id*/) const override {
    return {};
  }

  std::vector<sample_type> get_line_samples(FrameID id,
                                            size_t line) const override {
    if (gate != nullptr) {
      gate->wait();
    }
    ++reads;
    if (failing_frame.has_value() && id == *failing_frame) {
      throw std::runtime_error("frame could not be read");
    }
    if (line == 14 && id % 3 != 1) {
      return packets_[id % packets_.size()];
    }
    return blank_;
  }

  std::optional<SourceParameters> get_video_parameters() const override {
    SourceParameters params;
    params.system = VideoSystem::NTSC;
    params.frame_width_nominal = kNtscSamplesPerLine;
    params.frame_height = kNtscFrameLines;
    params.black_level = kNtscBlack;
    params.white_level = kNtscWhite;
    return params;
  }

  std::optional<FrameID> failing_frame;
  const Gate* gate = nullptr;
  mutable std::atomic<uint64_t> reads{0};

 private:
  uint64_t frames_;
  std::vector<std::vector<int16_t>> packets_;
  std::vector<int16_t> blank_;
};

NabtsFrameSlicer make_slicer() {
  NabtsFrameSlicerOptions options;
  options.detector = TeletextDetector::kThreshold;
  return NabtsFrameSlicer(options);
}

std::vector<NabtsFieldScan> make_buffer(uint64_t frames) {
  return std::vector<NabtsFieldScan>(static_cast<size_t>(frames) *
                                     kNabtsFieldsPerFrame);
}

// The first |frames| entries of two block buffers, compared line by line on
// everything emission reads.
void expect_same_scans(const std::vector<NabtsFieldScan>& actual,
                       const std::vector<NabtsFieldScan>& expected,
                       uint64_t frames) {
  for (size_t index = 0; index < frames * kNabtsFieldsPerFrame; ++index) {
    ASSERT_EQ(actual[index].lines.size(), expected[index].lines.size())
        << "field " << index;
    EXPECT_EQ(actual[index].lines_skipped, expected[index].lines_skipped);
    for (size_t line = 0; line < expected[index].lines.size(); ++line) {
      const NabtsFrameLineResult& a = actual[index].lines[line];
      const NabtsFrameLineResult& e = expected[index].lines[line];
      EXPECT_EQ(a.field_line, e.field_line);
      EXPECT_EQ(a.flat_line, e.flat_line);
      EXPECT_EQ(a.sliced.valid, e.sliced.valid)
          << "field " << index << " line " << line;
      EXPECT_EQ(a.sliced.bytes, e.sliced.bytes)
          << "field " << index << " line " << line;
    }
  }
}

TEST(NabtsBlockSlicer, ABlockSlicedOnThePoolIsTheBlockSlicedOnOneThread) {
  constexpr uint64_t kFrames = 48;
  const SyntheticRecording recording(kFrames);
  const NabtsFrameSlicer slicer = make_slicer();

  std::vector<NabtsFieldScan> one_thread = make_buffer(kFrames);
  nabts_slice_block(recording, slicer, 0, 0, kFrames, NabtsScanSnapshot{}, 1,
                    nullptr, one_thread);
  // The fixture has to be telling the frames apart for this to mean anything.
  ASSERT_TRUE(one_thread[0].lines[5].sliced.valid);
  ASSERT_FALSE(one_thread[2].lines[5].sliced.valid);

  std::vector<NabtsFieldScan> pooled = make_buffer(kFrames);
  NabtsBlockSlicer pool(6);
  pool.start(recording, slicer, 0, 0, kFrames, NabtsScanSnapshot{}, nullptr,
             pooled);
  pool.finish();

  expect_same_scans(pooled, one_thread, kFrames);
}

// Each field's buffer is sized on the caller's thread to the lines the mask
// leaves it: once a pass has learnt which lines carry data a field reads a few
// of the window's twelve, and a buffer used before for a field that read more
// gives the difference back. What the block yields is the same either way.
TEST(NabtsBlockSlicer, EachFieldsBufferHoldsTheLinesItReads) {
  constexpr uint64_t kFrames = 20;
  const SyntheticRecording recording(kFrames);
  const NabtsFrameSlicer slicer = make_slicer();
  NabtsScanSnapshot snapshot;
  snapshot.mask_lines = true;
  snapshot.line_options.learn_frames = 0;
  snapshot.line_options.recheck_interval = 10;
  snapshot.line_alive[0][14] = true;
  snapshot.line_alive[0][15] = true;
  snapshot.line_alive[1][14] = true;

  std::vector<NabtsFieldScan> pooled = make_buffer(kFrames);
  for (NabtsFieldScan& field : pooled) {
    field.lines.reserve(40);  // as left by a block that read more
  }
  NabtsBlockSlicer pool(4);
  pool.start(recording, slicer, 0, 0, kFrames, snapshot, nullptr, pooled);
  pool.finish();

  std::vector<NabtsFieldScan> one_thread = make_buffer(kFrames);
  nabts_slice_block(recording, slicer, 0, 0, kFrames, snapshot, 1, nullptr,
                    one_thread);
  expect_same_scans(pooled, one_thread, kFrames);
  ASSERT_TRUE(pooled[4].lines[0].sliced.valid);  // frame 2, line 14

  for (uint64_t frame = 0; frame < kFrames; ++frame) {
    // Frames 0 and 10 read the whole window whatever the mask says.
    const bool whole = frame % 10 == 0;
    const size_t index = static_cast<size_t>(frame) * kNabtsFieldsPerFrame;
    EXPECT_EQ(pooled[index].lines.capacity(), whole ? kWindowLines : 2u)
        << frame;
    EXPECT_EQ(pooled[index + 1].lines.capacity(), whole ? kWindowLines : 1u)
        << frame;
  }
}

// The pass's own schedule: blocks growing from 16 frames, each into the
// buffer the previous one is not in, on one pool kept throughout.
TEST(NabtsBlockSlicer, ThePoolSlicesBlockAfterBlock) {
  constexpr uint64_t kFrames = 200;
  const SyntheticRecording recording(kFrames);
  const NabtsFrameSlicer slicer = make_slicer();
  NabtsBlockSlicer pool(4);
  std::vector<NabtsFieldScan> buffers[2] = {make_buffer(64), make_buffer(64)};

  uint64_t first = 0;
  uint64_t size = 16;
  size_t turn = 0;
  while (first < kFrames) {
    const uint64_t count = std::min(size, kFrames - first);
    pool.start(recording, slicer, static_cast<FrameID>(first), first, count,
               NabtsScanSnapshot{}, nullptr, buffers[turn]);
    pool.finish();

    std::vector<NabtsFieldScan> expected = make_buffer(count);
    nabts_slice_block(recording, slicer, static_cast<FrameID>(first), first,
                      count, NabtsScanSnapshot{}, 1, nullptr, expected);
    expect_same_scans(buffers[turn], expected, count);

    first += count;
    size = std::min<uint64_t>(size * 2, 64);
    turn = 1 - turn;
  }
}

// A worker that throws must not take the process with it: the failure is
// thrown from finish(), once every worker has stopped, and the pool slices
// the next block as if nothing had happened.
TEST(NabtsBlockSlicer, AWorkersFailureIsThrownFromFinish) {
  constexpr uint64_t kFrames = 40;
  SyntheticRecording recording(kFrames);
  recording.failing_frame = 17;
  const NabtsFrameSlicer slicer = make_slicer();
  NabtsBlockSlicer pool(4);
  std::vector<NabtsFieldScan> buffer = make_buffer(kFrames);

  pool.start(recording, slicer, 0, 0, kFrames, NabtsScanSnapshot{}, nullptr,
             buffer);
  EXPECT_THROW(pool.finish(), std::runtime_error);

  recording.failing_frame.reset();
  pool.start(recording, slicer, 0, 0, kFrames, NabtsScanSnapshot{}, nullptr,
             buffer);
  EXPECT_NO_THROW(pool.finish());
}

TEST(NabtsBlockSlicer, ACancelledBlockTakesNoMoreFrames) {
  constexpr uint64_t kFrames = 40;
  const SyntheticRecording recording(kFrames);
  const NabtsFrameSlicer slicer = make_slicer();
  NabtsBlockSlicer pool(4);
  std::vector<NabtsFieldScan> buffer = make_buffer(kFrames);
  const std::atomic<bool> cancelled{true};

  pool.start(recording, slicer, 0, 0, kFrames, NabtsScanSnapshot{}, &cancelled,
             buffer);
  pool.finish();
  EXPECT_EQ(recording.reads.load(), 0u);
}

// start() hands the block over and returns: the caller is free to do its own
// work — emitting the block before — while the pool's threads slice.
TEST(NabtsBlockSlicer, StartReturnsWhileThePoolSlices) {
  constexpr uint64_t kFrames = 8;
  Gate gate;
  SyntheticRecording recording(kFrames);
  recording.gate = &gate;
  const NabtsFrameSlicer slicer = make_slicer();
  NabtsBlockSlicer pool(2);
  std::vector<NabtsFieldScan> buffer = make_buffer(kFrames);

  pool.start(recording, slicer, 0, 0, kFrames, NabtsScanSnapshot{}, nullptr,
             buffer);
  // The pool's thread is held on its first line read, and start() has
  // returned all the same.
  gate.open();
  pool.finish();
  EXPECT_GT(recording.reads.load(), 0u);
  EXPECT_TRUE(buffer[0].lines[5].sliced.valid);
}

// A pass that unwinds with a block in progress destroys the pool mid-block;
// its threads stop at the frame they are on and are joined.
TEST(NabtsBlockSlicer, IsDestroyedCleanlyWithABlockInProgress) {
  constexpr uint64_t kFrames = 400;
  const SyntheticRecording recording(kFrames);
  const NabtsFrameSlicer slicer = make_slicer();
  std::vector<NabtsFieldScan> buffer = make_buffer(kFrames);
  {
    NabtsBlockSlicer pool(4);
    pool.start(recording, slicer, 0, 0, kFrames, NabtsScanSnapshot{}, nullptr,
               buffer);
  }
  EXPECT_LE(recording.reads.load(), kFrames * 2u * 12u);
}

TEST(NabtsBlockSlicer, OneWorkerSlicesOnTheCallingThreadAlone) {
  constexpr uint64_t kFrames = 12;
  const SyntheticRecording recording(kFrames);
  const NabtsFrameSlicer slicer = make_slicer();
  NabtsBlockSlicer pool(1);
  std::vector<NabtsFieldScan> buffer = make_buffer(kFrames);

  pool.start(recording, slicer, 0, 0, kFrames, NabtsScanSnapshot{}, nullptr,
             buffer);
  // Nothing has been sliced yet: there is no thread to have done it.
  EXPECT_EQ(recording.reads.load(), 0u);
  pool.finish();
  EXPECT_EQ(recording.reads.load(), kFrames * 2u * kWindowLines);
}

}  // namespace
}  // namespace orc
