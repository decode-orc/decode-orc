/*
 * File:        teletext_block_scanner_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for slicing blocks of frames on the teletext sink's
 *              thread pool
 *
 * Covers: a block sliced on the pool is the block sliced on one thread, block
 * after block; a worker's failure reaches the caller; a cancelled block stops;
 * and an abandoned block is joined cleanly. Every line of video is synthesised
 * in memory and no I/O is performed.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Ethan Halsall
 */

#include "teletext_block_scanner.h"

#include <gtest/gtest.h>
#include <orc/stage/cvbs_signal_constants.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "../../common/vbi-services/teletext_line_synthesizer.h"

namespace orc {
namespace {

// A 625-line recording in memory. Field line 7 of field 1 carries a WST
// packet on two frames in three, so no two neighbouring frames slice alike;
// every other line is blanking.
class SyntheticRecording final : public VideoFrameRepresentation {
 public:
  explicit SyntheticRecording(uint64_t frames)
      : frames_(frames),
        packet_(tests::synthesize_teletext_line(
            tests::make_parity_coded_payload())),
        blank_(static_cast<size_t>(kPalSamplesPerLineNominal),
               static_cast<int16_t>(kPalBlanking)) {}

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
    ++reads;
    if (failing_frame.has_value() && id == *failing_frame) {
      throw std::runtime_error("frame could not be read");
    }
    if (line == 7 && id % 3 != 1) {
      return packet_;
    }
    return blank_;
  }

  std::optional<SourceParameters> get_video_parameters() const override {
    SourceParameters params;
    params.system = VideoSystem::PAL;
    params.frame_width_nominal = kPalSamplesPerLineNominal;
    params.frame_height = kPalFrameLines;
    params.black_level = kPalBlack;
    params.white_level = kPalWhite;
    return params;
  }

  std::optional<FrameID> failing_frame;
  mutable std::atomic<uint64_t> reads{0};

 private:
  uint64_t frames_;
  std::vector<int16_t> packet_;
  std::vector<int16_t> blank_;
};

TeletextFrameSlicer make_slicer() {
  TeletextFrameSlicerOptions options;
  options.detector = TeletextDetector::kThreshold;
  return TeletextFrameSlicer(options);
}

std::vector<TeletextFieldScan> make_buffer(uint64_t frames) {
  return std::vector<TeletextFieldScan>(static_cast<size_t>(frames) *
                                        kFieldsPerFrame);
}

void expect_same_scans(const std::vector<TeletextFieldScan>& actual,
                       const std::vector<TeletextFieldScan>& expected,
                       uint64_t frames) {
  for (size_t index = 0; index < frames * kFieldsPerFrame; ++index) {
    ASSERT_EQ(actual[index].lines.size(), expected[index].lines.size())
        << "field " << index;
    EXPECT_EQ(actual[index].lines_skipped, expected[index].lines_skipped);
    for (size_t line = 0; line < expected[index].lines.size(); ++line) {
      const TeletextFrameLineResult& a = actual[index].lines[line];
      const TeletextFrameLineResult& e = expected[index].lines[line];
      EXPECT_EQ(a.field_line, e.field_line);
      EXPECT_EQ(a.flat_line, e.flat_line);
      EXPECT_EQ(a.sliced.valid, e.sliced.valid)
          << "field " << index << " line " << line;
      EXPECT_EQ(a.sliced.bytes, e.sliced.bytes)
          << "field " << index << " line " << line;
    }
  }
}

// Index of field line 7 among the 625-line candidate lines, 5 to 21.
constexpr size_t kPacketLineIndex = 2;

TEST(TeletextBlockSlicer, ABlockSlicedOnThePoolIsTheBlockSlicedOnOneThread) {
  constexpr uint64_t kFrames = 36;
  const SyntheticRecording recording(kFrames);
  const TeletextFrameSlicer slicer = make_slicer();

  std::vector<TeletextFieldScan> one_thread = make_buffer(kFrames);
  slice_block(recording, slicer, 0, 0, kFrames, TeletextScanSnapshot{}, 1,
              nullptr, one_thread);
  // The fixture has to be telling the frames apart for this to mean anything.
  ASSERT_TRUE(one_thread[0].lines[kPacketLineIndex].sliced.valid);
  ASSERT_FALSE(one_thread[2].lines[kPacketLineIndex].sliced.valid);

  std::vector<TeletextFieldScan> pooled = make_buffer(kFrames);
  TeletextBlockSlicer pool(6);
  pool.start(recording, slicer, 0, 0, kFrames, TeletextScanSnapshot{}, nullptr,
             pooled);
  pool.finish();

  expect_same_scans(pooled, one_thread, kFrames);
}

TEST(TeletextBlockSlicer, ThePoolSlicesBlockAfterBlock) {
  constexpr uint64_t kFrames = 150;
  const SyntheticRecording recording(kFrames);
  const TeletextFrameSlicer slicer = make_slicer();
  TeletextBlockSlicer pool(4);
  std::vector<TeletextFieldScan> buffers[2] = {make_buffer(64),
                                               make_buffer(64)};

  uint64_t first = 0;
  uint64_t size = 16;
  size_t turn = 0;
  while (first < kFrames) {
    const uint64_t count = std::min(size, kFrames - first);
    pool.start(recording, slicer, static_cast<FrameID>(first), first, count,
               TeletextScanSnapshot{}, nullptr, buffers[turn]);
    pool.finish();

    std::vector<TeletextFieldScan> expected = make_buffer(count);
    slice_block(recording, slicer, static_cast<FrameID>(first), first, count,
                TeletextScanSnapshot{}, 1, nullptr, expected);
    expect_same_scans(buffers[turn], expected, count);

    first += count;
    size = std::min<uint64_t>(size * 2, 64);
    turn = 1 - turn;
  }
}

TEST(TeletextBlockSlicer, AWorkersFailureIsThrownFromFinish) {
  constexpr uint64_t kFrames = 30;
  SyntheticRecording recording(kFrames);
  recording.failing_frame = 11;
  const TeletextFrameSlicer slicer = make_slicer();
  TeletextBlockSlicer pool(4);
  std::vector<TeletextFieldScan> buffer = make_buffer(kFrames);

  pool.start(recording, slicer, 0, 0, kFrames, TeletextScanSnapshot{}, nullptr,
             buffer);
  EXPECT_THROW(pool.finish(), std::runtime_error);

  recording.failing_frame.reset();
  pool.start(recording, slicer, 0, 0, kFrames, TeletextScanSnapshot{}, nullptr,
             buffer);
  EXPECT_NO_THROW(pool.finish());
}

TEST(TeletextBlockSlicer, ACancelledBlockTakesNoMoreFrames) {
  constexpr uint64_t kFrames = 30;
  const SyntheticRecording recording(kFrames);
  const TeletextFrameSlicer slicer = make_slicer();
  TeletextBlockSlicer pool(4);
  std::vector<TeletextFieldScan> buffer = make_buffer(kFrames);
  const std::atomic<bool> cancelled{true};

  pool.start(recording, slicer, 0, 0, kFrames, TeletextScanSnapshot{},
             &cancelled, buffer);
  pool.finish();
  EXPECT_EQ(recording.reads.load(), 0u);
}

TEST(TeletextBlockSlicer, IsDestroyedCleanlyWithABlockInProgress) {
  constexpr uint64_t kFrames = 300;
  const SyntheticRecording recording(kFrames);
  const TeletextFrameSlicer slicer = make_slicer();
  std::vector<TeletextFieldScan> buffer = make_buffer(kFrames);
  {
    TeletextBlockSlicer pool(4);
    pool.start(recording, slicer, 0, 0, kFrames, TeletextScanSnapshot{},
               nullptr, buffer);
  }
  EXPECT_LE(recording.reads.load(), kFrames * 2u * 17u);
}

}  // namespace
}  // namespace orc
