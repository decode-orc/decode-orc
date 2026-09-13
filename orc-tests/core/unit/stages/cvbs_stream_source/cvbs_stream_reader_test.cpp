/*
 * File:        cvbs_stream_reader_test.cpp
 * Module:      orc-core-tests
 * Purpose:     Unit tests for CVBSStreamReader — the sequential, bounded
 *              ring-buffer frame reader behind cvbs_stream_source
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../../../../../orc/plugins/stages/cvbs_stream_source/cvbs_stream_source_stage.h"

namespace orc_unit_test {
namespace {

// Builds a "raw" wire-format input: `frame_count` frames of `frame_samples`
// 16-bit words each, word value == its 0-based position across the whole
// stream (so a test can assert on exact expected sample values). Using
// SampleEncoding::kU10 throughout — an identity transform — keeps the
// written words and the samples read back numerically identical.
std::string make_raw_stream(size_t frame_samples, size_t frame_count) {
  std::string bytes;
  bytes.resize(frame_samples * frame_count * 2);
  auto* words = reinterpret_cast<uint16_t*>(bytes.data());
  for (size_t i = 0; i < frame_samples * frame_count; ++i) {
    words[i] = static_cast<uint16_t>(i & 0xFFFF);
  }
  return bytes;
}

}  // namespace

TEST(CVBSStreamReaderTest, ReadsFramesInForwardOrder_WithExpectedSamples) {
  constexpr size_t kFrameSamples = 8;
  constexpr size_t kFrameCount = 4;
  std::istringstream input(make_raw_stream(kFrameSamples, kFrameCount),
                           std::ios::binary);

  orc::CVBSStreamReader reader(input, kFrameSamples, kFrameCount,
                               /*buffer_frames=*/4, orc::SampleEncoding::kU10,
                               /*blanking_10bit=*/0);

  for (orc::FrameID id = 0; id < kFrameCount; ++id) {
    const int16_t* frame = reader.get_frame(id);
    ASSERT_NE(frame, nullptr) << "frame " << id;
    for (size_t s = 0; s < kFrameSamples; ++s) {
      const uint16_t expected =
          static_cast<uint16_t>((id * kFrameSamples + s) & 0xFFFF);
      EXPECT_EQ(static_cast<uint16_t>(frame[s]), expected)
          << "frame " << id << " sample " << s;
    }
  }
  EXPECT_FALSE(reader.failed());
}

TEST(CVBSStreamReaderTest, OutOfDeclaredRange_ReturnsNullptr) {
  constexpr size_t kFrameSamples = 4;
  constexpr size_t kFrameCount = 2;
  std::istringstream input(make_raw_stream(kFrameSamples, kFrameCount),
                           std::ios::binary);

  orc::CVBSStreamReader reader(input, kFrameSamples, kFrameCount,
                               /*buffer_frames=*/4, orc::SampleEncoding::kU10,
                               /*blanking_10bit=*/0);

  EXPECT_EQ(reader.get_frame(static_cast<orc::FrameID>(kFrameCount)), nullptr);
}

TEST(CVBSStreamReaderTest, ShortInput_FailsWithClearError) {
  constexpr size_t kFrameSamples = 8;
  constexpr size_t kFrameCount = 4;
  // Only 2 frames' worth of data for a stream declaring 4.
  std::istringstream input(make_raw_stream(kFrameSamples, 2), std::ios::binary);

  orc::CVBSStreamReader reader(input, kFrameSamples, kFrameCount,
                               /*buffer_frames=*/4, orc::SampleEncoding::kU10,
                               /*blanking_10bit=*/0);

  EXPECT_EQ(reader.get_frame(3), nullptr);
  EXPECT_TRUE(reader.failed());
  EXPECT_NE(reader.last_error().find("unexpected end of input"),
            std::string::npos);
}

// Requesting a frame far enough behind the reader's current position that it
// has already been overwritten in the ring buffer must fail loudly rather
// than return stale or wrong data.
TEST(CVBSStreamReaderTest, RequestingAnEvictedFrame_FailsLoudly) {
  constexpr size_t kFrameSamples = 4;
  constexpr size_t kFrameCount = 10;
  constexpr size_t kBufferFrames = 2;
  std::istringstream input(make_raw_stream(kFrameSamples, kFrameCount),
                           std::ios::binary);

  orc::CVBSStreamReader reader(input, kFrameSamples, kFrameCount, kBufferFrames,
                               orc::SampleEncoding::kU10,
                               /*blanking_10bit=*/0);

  // Advance the reader well past the buffer window by requesting a late
  // frame first, then go back for an early one that must have scrolled out.
  ASSERT_NE(reader.get_frame(8), nullptr);
  EXPECT_EQ(reader.get_frame(0), nullptr);
  EXPECT_TRUE(reader.failed());
  EXPECT_NE(reader.last_error().find("scrolled out"), std::string::npos);
}

// The core concurrency guarantee: several threads requesting different
// frame ids at once must all get correct data without corrupting each
// other — this is exactly the access pattern VideoSinkStage's parallel
// export workers produce (see the class comment in
// cvbs_stream_source_stage.cpp). buffer_frames == frame_count here so no
// request can ever be evicted regardless of read pacing — an
// istringstream's reads never actually block, so the reader thread can race
// to the end before any worker calls get_frame() at all; the eviction
// boundary itself is covered separately (and deterministically) by
// RequestingAnEvictedFrame_FailsLoudly above. This test is purely about
// concurrent access not corrupting the ring buffer / mutex bookkeeping.
TEST(CVBSStreamReaderTest, ConcurrentRequests_AllSucceed) {
  constexpr size_t kFrameSamples = 16;
  constexpr size_t kFrameCount = 64;
  std::istringstream input(make_raw_stream(kFrameSamples, kFrameCount),
                           std::ios::binary);

  orc::CVBSStreamReader reader(input, kFrameSamples, kFrameCount,
                               /*buffer_frames=*/kFrameCount,
                               orc::SampleEncoding::kU10,
                               /*blanking_10bit=*/0);

  std::vector<std::thread> workers;
  std::vector<bool> ok(kFrameCount, false);
  for (size_t t = 0; t < 8; ++t) {
    workers.emplace_back([&, t] {
      for (size_t id = t; id < kFrameCount; id += 8) {
        const int16_t* frame = reader.get_frame(static_cast<orc::FrameID>(id));
        if (!frame) continue;
        bool good = true;
        for (size_t s = 0; s < kFrameSamples; ++s) {
          const uint16_t expected =
              static_cast<uint16_t>((id * kFrameSamples + s) & 0xFFFF);
          if (static_cast<uint16_t>(frame[s]) != expected) good = false;
        }
        ok[id] = good;
      }
    });
  }
  for (auto& w : workers) w.join();

  EXPECT_FALSE(reader.failed());
  for (size_t id = 0; id < kFrameCount; ++id) {
    EXPECT_TRUE(ok[id]) << "frame " << id;
  }
}

}  // namespace orc_unit_test
