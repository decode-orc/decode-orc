/*
 * File:        audio_resampler_test.cpp
 * Module:      orc-tests/core/unit/stages/audio_resample
 * Purpose:     Unit tests for the shared AudioResampler library (16→24-bit
 *              widening, any-rate → synchronous 48 kHz conversion, cadence
 *              segmentation)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "audio-resample/audio_resampler.h"

#include <gtest/gtest.h>
#include <orc/stage/audio/audio_channel_pair.h>
#include <orc/stage/common_types.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "audio-resample/synchronous_audio_cache.h"

namespace orc {
namespace tests {

namespace {

// Interleaved stereo int32 ramp: pair i carries i on both channels, so every
// stream position is uniquely identifiable in segmentation tests.
std::vector<int32_t> make_ramp_pairs(size_t pair_count) {
  std::vector<int32_t> samples;
  samples.reserve(pair_count * 2);
  for (size_t i = 0; i < pair_count; ++i) {
    const int32_t v = static_cast<int32_t>(i);
    samples.push_back(v);
    samples.push_back(v);
  }
  return samples;
}

// A constant-DC stereo signal in the 24-bit-in-int32 carrier.
std::vector<int32_t> make_dc_pairs(size_t pair_count, int32_t level) {
  return std::vector<int32_t>(pair_count * 2, level);
}

}  // namespace

// ===========================================================================
// widen_16_to_24
// ===========================================================================

TEST(AudioResamplerTest, Widen16To24_ShiftsLeftEightSignPreserving) {
  const std::vector<int16_t> input = {0, 1, -1, 1000, -1000, 32767, -32768};
  const auto output = AudioResampler::widen_16_to_24(input);

  ASSERT_EQ(output.size(), input.size());
  EXPECT_EQ(output[0], 0);
  EXPECT_EQ(output[1], 256);
  EXPECT_EQ(output[2], -256);
  EXPECT_EQ(output[3], 256000);
  EXPECT_EQ(output[4], -256000);
  EXPECT_EQ(output[5], 8388352);   // 32767 << 8
  EXPECT_EQ(output[6], -8388608);  // -32768 << 8 = 24-bit minimum

  // Exactly reversible: shifting back recovers the 16-bit values.
  for (size_t i = 0; i < input.size(); ++i) {
    EXPECT_EQ(static_cast<int16_t>(output[i] >> 8), input[i]);
  }
}

TEST(AudioResamplerTest, Widen16To24_EmptyInputReturnsEmpty) {
  EXPECT_TRUE(AudioResampler::widen_16_to_24({}).empty());
}

// ===========================================================================
// resample
// ===========================================================================

TEST(AudioResamplerTest, Resample_EmptyInputReturnsEmpty) {
  EXPECT_TRUE(AudioResampler::resample({}, 44100.0, 48000.0).empty());
}

TEST(AudioResamplerTest, Resample_SameRateIsPassthrough) {
  const auto input = make_ramp_pairs(1000);
  EXPECT_EQ(AudioResampler::resample(input, 48000.0, 48000.0), input);
}

TEST(AudioResamplerTest, Resample_44100To48000PreservesDuration) {
  // One second of 44.1 kHz audio must convert to one second at 48 kHz:
  // output pair count within ±2 of round(n × 48000 / 44100).
  const size_t in_pairs = 44100;
  const auto input = make_dc_pairs(in_pairs, 256000);
  const auto output = AudioResampler::resample(input, 44100.0, 48000.0);

  ASSERT_FALSE(output.empty());
  ASSERT_EQ(output.size() % 2, 0u);
  const auto out_pairs = static_cast<int64_t>(output.size() / 2);
  const auto expected = static_cast<int64_t>(
      std::llround(static_cast<double>(in_pairs) * 48000.0 / 44100.0));
  EXPECT_NEAR(static_cast<double>(out_pairs), static_cast<double>(expected),
              2.0);
}

TEST(AudioResamplerTest, Resample_PreservesDCLevel) {
  // A constant (DC) signal in the 24-bit carrier must stay at the same level
  // through SoXR (linear, unity passband gain).
  const int32_t level = 1000 << 8;  // 256000
  const auto input = make_dc_pairs(44100, level);
  const auto output = AudioResampler::resample(input, 44100.0, 48000.0);

  ASSERT_FALSE(output.empty());
  // Sample away from the filter edges.
  const size_t mid = (output.size() / 4) * 2;
  EXPECT_NEAR(static_cast<double>(output[mid]), static_cast<double>(level),
              512.0);
  EXPECT_NEAR(static_cast<double>(output[mid + 1]), static_cast<double>(level),
              512.0);
}

TEST(AudioResamplerTest, Resample_SpansManyInternalChunksContinuously) {
  // The resampler drives SoXR in fixed 65536-frame chunks (issue #230: a
  // single whole-stream call is quadratic on allocators that cannot extend a
  // large block in place). A stream several chunks long must come back as one
  // continuous signal — no dropped, duplicated or discontinuous material at
  // the internal chunk seams.
  const size_t in_pairs = 65536 * 4 + 12345;  // deliberately not chunk-aligned
  const int32_t level = 1000 << 8;
  const auto input = make_dc_pairs(in_pairs, level);
  const auto output = AudioResampler::resample(input, 44100.0, 48000.0);

  ASSERT_EQ(output.size() % 2, 0u);
  const auto out_pairs = static_cast<int64_t>(output.size() / 2);
  const auto expected = static_cast<int64_t>(
      std::llround(static_cast<double>(in_pairs) * 48000.0 / 44100.0));
  EXPECT_NEAR(static_cast<double>(out_pairs), static_cast<double>(expected),
              2.0);

  // A chunk seam would show up as a step away from DC. Check across the whole
  // stream, skipping the filter warm-up and drain at the very ends.
  const size_t skip = 2000;
  ASSERT_GT(output.size(), skip * 2 * 2);
  for (size_t i = skip * 2; i < output.size() - skip * 2; ++i) {
    ASSERT_NEAR(static_cast<double>(output[i]), static_cast<double>(level),
                512.0)
        << "discontinuity at sample " << i;
  }
}

TEST(AudioResamplerTest, Resample_RampSurvivesChunkSeamsInOrder) {
  // A monotonic ramp exposes reordering or loss at the chunk seams that a DC
  // signal cannot: resampled output must stay monotonically non-decreasing.
  const size_t in_pairs = 65536 * 3;
  std::vector<int32_t> input;
  input.reserve(in_pairs * 2);
  for (size_t i = 0; i < in_pairs; ++i) {
    const auto v = static_cast<int32_t>(i * 32);
    input.push_back(v);
    input.push_back(v);
  }
  const auto output = AudioResampler::resample(input, 44100.0, 48000.0);
  ASSERT_FALSE(output.empty());

  const size_t skip = 2000;
  ASSERT_GT(output.size() / 2, skip * 2);
  for (size_t p = skip + 1; p < output.size() / 2 - skip; ++p) {
    ASSERT_GE(output[p * 2], output[(p - 1) * 2])
        << "ramp went backwards at pair " << p;
  }
}

// ===========================================================================
// resample_to_synchronous
// ===========================================================================

TEST(AudioResamplerTest, ResampleToSynchronous_PAL_IsSampleExactSegmentation) {
  // 48 kHz PAL input needs no resample: pure 1920-pair cadence slicing.
  const auto input = make_ramp_pairs(1920 * 2);
  const auto frames = AudioResampler::resample_to_synchronous(
      input, 48000.0, VideoSystem::PAL, 2);

  ASSERT_EQ(frames.size(), 2u);
  ASSERT_EQ(frames[0].size(), 1920u * 2);
  ASSERT_EQ(frames[1].size(), 1920u * 2);
  EXPECT_EQ(frames[0][0], 0);
  EXPECT_EQ(frames[0][2], 1);
  // Frame 1 starts at pair 1920 exactly.
  EXPECT_EQ(frames[1][0], 1920);
  EXPECT_EQ(frames[1].back(), 1920 * 2 - 1);
}

TEST(AudioResamplerTest, ResampleToSynchronous_PAL_ZeroPadsShortFinalFrame) {
  // 1920 + 100 pairs: frame 1 has 100 real pairs then silence.
  const auto input = make_ramp_pairs(1920 + 100);
  const auto frames = AudioResampler::resample_to_synchronous(
      input, 48000.0, VideoSystem::PAL, 2);

  ASSERT_EQ(frames.size(), 2u);
  ASSERT_EQ(frames[1].size(), 1920u * 2);
  EXPECT_EQ(frames[1][0], 1920);
  EXPECT_EQ(frames[1][99 * 2], 2019);  // last real pair
  EXPECT_EQ(frames[1][100 * 2], 0);    // padding starts at pair 100
  EXPECT_EQ(frames[1].back(), 0);
}

TEST(AudioResamplerTest, ResampleToSynchronous_TruncatesExcessInput) {
  // Three frames of material segmented into two frames: the third frame's
  // pairs are discarded and the blocks total audio_pair_offset(2).
  const auto input = make_ramp_pairs(1920 * 3);
  const auto frames = AudioResampler::resample_to_synchronous(
      input, 48000.0, VideoSystem::PAL, 2);

  ASSERT_EQ(frames.size(), 2u);
  size_t total_pairs = 0;
  for (const auto& f : frames) total_pairs += f.size() / 2;
  EXPECT_EQ(total_pairs, audio_pair_offset(2, VideoSystem::PAL));
  EXPECT_EQ(frames[1].back(), 1920 * 2 - 1);
}

TEST(AudioResamplerTest, ResampleToSynchronous_NTSC_FollowsCadence) {
  // NTSC cadence: 1602/1601/1602/1601/1602 pairs over the 5-frame audio
  // frame sequence (SMPTE 272M-1994 §14.3), totalling exactly
  // audio_pair_offset(5) = 8008 pairs, each block starting at its
  // cumulative offset.
  const auto input = make_ramp_pairs(8008);
  const auto frames = AudioResampler::resample_to_synchronous(
      input, 48000.0, VideoSystem::NTSC, 5);

  ASSERT_EQ(frames.size(), 5u);
  size_t total_pairs = 0;
  for (uint64_t i = 0; i < 5; ++i) {
    const auto expected_pairs = audio_pairs_in_frame(i, VideoSystem::NTSC);
    ASSERT_EQ(frames[i].size(), static_cast<size_t>(expected_pairs) * 2)
        << "frame " << i;
    EXPECT_EQ(frames[i][0],
              static_cast<int32_t>(audio_pair_offset(i, VideoSystem::NTSC)))
        << "frame " << i;
    total_pairs += frames[i].size() / 2;
  }
  EXPECT_EQ(frames[0].size(), 1602u * 2);
  EXPECT_EQ(frames[1].size(), 1601u * 2);
  EXPECT_EQ(total_pairs, audio_pair_offset(5, VideoSystem::NTSC));
  EXPECT_EQ(frames[4].back(), 8007);
}

TEST(AudioResamplerTest, ResampleToSynchronous_ResamplesNonSynchronousInput) {
  // 44.1 kHz DC input for two PAL frames: blocks are cadence-sized and stay
  // near the DC level after resampling.
  const int32_t level = 256000;
  const auto input = make_dc_pairs(44100, level);  // one second of material
  const auto frames = AudioResampler::resample_to_synchronous(
      input, 44100.0, VideoSystem::PAL, 2);

  ASSERT_EQ(frames.size(), 2u);
  ASSERT_EQ(frames[0].size(), 1920u * 2);
  ASSERT_EQ(frames[1].size(), 1920u * 2);
  // Sample away from the filter edges (middle of frame 1).
  EXPECT_NEAR(static_cast<double>(frames[1][1920]), static_cast<double>(level),
              512.0);
}

TEST(AudioResamplerTest, ResampleToSynchronous_EmptyInputYieldsSilentFrames) {
  const auto frames = AudioResampler::resample_to_synchronous(
      {}, 48000.0, VideoSystem::NTSC, 3);

  ASSERT_EQ(frames.size(), 3u);
  for (uint64_t i = 0; i < 3; ++i) {
    ASSERT_EQ(
        frames[i].size(),
        static_cast<size_t>(audio_pairs_in_frame(i, VideoSystem::NTSC)) * 2);
    EXPECT_EQ(frames[i][0], 0);
    EXPECT_EQ(frames[i].back(), 0);
  }
}

TEST(AudioResamplerTest, ResampleToSynchronous_UnknownSystemYieldsEmptyFrames) {
  const auto input = make_ramp_pairs(1000);
  const auto frames = AudioResampler::resample_to_synchronous(
      input, 48000.0, VideoSystem::Unknown, 2);

  ASSERT_EQ(frames.size(), 2u);
  EXPECT_TRUE(frames[0].empty());
  EXPECT_TRUE(frames[1].empty());
}

// ===========================================================================
// AudioResampleStream
// ===========================================================================

namespace {

// Deterministic non-trivial stereo signal: a sum of two tones plus a
// pseudo-random component, different on each channel, in the 24-bit range.
std::vector<int32_t> make_programme_pairs(size_t pair_count) {
  std::vector<int32_t> samples(pair_count * 2);
  uint32_t lcg = 12345;
  for (size_t i = 0; i < pair_count; ++i) {
    lcg = lcg * 1664525u + 1013904223u;
    const double t = static_cast<double>(i);
    const double noise = static_cast<double>(lcg >> 16) - 32768.0;
    samples[i * 2] =
        static_cast<int32_t>(3000000.0 * std::sin(t * 0.031) + 8.0 * noise);
    samples[i * 2 + 1] = static_cast<int32_t>(
        2000000.0 * std::sin(t * 0.0071 + 1.0) - 6.0 * noise);
  }
  return samples;
}

}  // namespace

TEST(AudioResampleStreamTest, PiecewiseFeedEqualsWholeStreamResample) {
  // Pieces of uneven size, some larger and some smaller than the internal
  // chunk, must give the whole-stream output sample for sample.
  const auto input = make_programme_pairs(65536 * 3 + 777);
  const auto whole = AudioResampler::resample(input, 44100.0, 48000.0);

  AudioResampleStream stream(44100.0, 48000.0);
  ASSERT_TRUE(stream.ok());
  std::vector<int32_t> pieces;
  const std::array<size_t, 5> sizes = {1, 1000, 65536, 100000, 4093};
  size_t pos = 0;
  size_t k = 0;
  while (pos < input.size() / 2) {
    const size_t n =
        std::min(sizes[k++ % sizes.size()], input.size() / 2 - pos);
    ASSERT_TRUE(stream.process(input.data() + pos * 2, n, pieces));
    pos += n;
  }
  stream.finish(pieces);
  EXPECT_EQ(pieces, whole);
}

// ===========================================================================
// SynchronousAudioCache
// ===========================================================================

namespace {

// Source reader over an in-memory stream that counts the pairs it was asked
// for, so tests can see how far ahead of the request the cache read.
struct CountingSource {
  std::vector<int32_t> samples;
  std::shared_ptr<std::atomic<uint64_t>> furthest =
      std::make_shared<std::atomic<uint64_t>>(0);

  SynchronousAudioCache::SourceReader reader() const {
    const auto* data = &samples;
    auto max_seen = furthest;
    return [data, max_seen](uint64_t first, size_t count) {
      const uint64_t total = data->size() / 2;
      const uint64_t begin = std::min<uint64_t>(first, total);
      const uint64_t end = std::min<uint64_t>(first + count, total);
      uint64_t prev = max_seen->load();
      while (end > prev && !max_seen->compare_exchange_weak(prev, end)) {
      }
      return std::vector<int32_t>(
          data->begin() + static_cast<std::ptrdiff_t>(begin * 2),
          data->begin() + static_cast<std::ptrdiff_t>(end * 2));
    };
  }
};

SynchronousAudioCache::StoreFactory memory_store() {
  return [] { return make_memory_audio_store(); };
}

}  // namespace

TEST(SynchronousAudioCacheTest, ResampledBlocksEqualWholeStreamConversion) {
  // 44.1 kHz NTSC material several chunks long, read in a scattered order
  // (last frame first), must give exactly the whole-stream blocks.
  constexpr uint64_t kFrames = 200;
  CountingSource source;
  source.samples = make_programme_pairs(1470 * kFrames + 333);
  const auto expected = AudioResampler::resample_to_synchronous(
      source.samples, 44100.0, VideoSystem::NTSC, kFrames);

  SynchronousAudioCache cache(source.reader(), 44100.0, VideoSystem::NTSC,
                              kFrames, memory_store());
  EXPECT_FALSE(cache.is_pass_through());
  EXPECT_EQ(cache.frame_block(kFrames - 1), expected[kFrames - 1]);
  for (uint64_t i = 0; i < kFrames; ++i) {
    ASSERT_EQ(cache.frame_block(i), expected[i]) << "frame " << i;
  }
  EXPECT_TRUE(cache.frame_block(kFrames).empty());
}

TEST(SynchronousAudioCacheTest, PassThroughEqualsWholeStreamSegmentation) {
  // 48 kHz material is not resampled and needs no store: each block is read
  // straight from the source; the short tail is zero-padded.
  constexpr uint64_t kFrames = 7;
  CountingSource source;
  source.samples = make_ramp_pairs(1920 * 6 + 100);
  const auto expected = AudioResampler::resample_to_synchronous(
      source.samples, 48000.0, VideoSystem::PAL, kFrames);

  SynchronousAudioCache cache(source.reader(), 48000.0, VideoSystem::PAL,
                              kFrames, [] {
                                ADD_FAILURE() << "pass-through made a store";
                                return make_memory_audio_store();
                              });
  EXPECT_TRUE(cache.is_pass_through());
  for (uint64_t i = 0; i < kFrames; ++i) {
    ASSERT_EQ(cache.frame_block(i), expected[i]) << "frame " << i;
  }
  EXPECT_EQ(cache.converted_pairs(), 0u);
}

TEST(SynchronousAudioCacheTest, ConvertsAheadOfTheReaderNotTheWholeStream) {
  // Asking for the first frame converts about one chunk, not the stream.
  constexpr uint64_t kFrames = 2000;  // 80 s of PAL
  CountingSource source;
  source.samples = make_dc_pairs(1764 * kFrames, 256000);
  SynchronousAudioCache cache(source.reader(), 44100.0, VideoSystem::PAL,
                              kFrames, memory_store());

  const auto block = cache.frame_block(0);
  ASSERT_EQ(block.size(), 1920u * 2);
  EXPECT_LE(source.furthest->load(), AudioResampleStream::kChunkFrames);
  EXPECT_LT(cache.converted_pairs(), 2u * AudioResampleStream::kChunkFrames);

  // A frame further on converts up to it.
  cache.frame_block(100);
  EXPECT_GE(cache.converted_pairs(), audio_pair_offset(101, VideoSystem::PAL));
  EXPECT_LT(source.furthest->load(), source.samples.size() / 2);
}

TEST(SynchronousAudioCacheTest, StopsReadingOnceEveryFrameIsCovered) {
  // Excess source material past the last frame is never read in full, and
  // the blocks total exactly audio_pair_offset(frame_count).
  constexpr uint64_t kFrames = 30;
  CountingSource source;
  source.samples = make_programme_pairs(1764 * kFrames * 20);
  const auto expected = AudioResampler::resample_to_synchronous(
      source.samples, 44100.0, VideoSystem::PAL, kFrames);
  SynchronousAudioCache cache(source.reader(), 44100.0, VideoSystem::PAL,
                              kFrames, memory_store());
  cache.convert_all();
  EXPECT_EQ(cache.converted_pairs(),
            audio_pair_offset(kFrames, VideoSystem::PAL));
  EXPECT_LT(source.furthest->load(), source.samples.size() / 2);
  for (uint64_t i = 0; i < kFrames; ++i) {
    ASSERT_EQ(cache.frame_block(i), expected[i]) << "frame " << i;
  }
}

TEST(SynchronousAudioCacheTest, ShortAndEmptySourcesServeSilence) {
  constexpr uint64_t kFrames = 50;
  CountingSource empty;
  SynchronousAudioCache silent(empty.reader(), 44100.0, VideoSystem::PAL,
                               kFrames, memory_store());
  for (uint64_t i : {uint64_t{0}, uint64_t{49}}) {
    const auto block = silent.frame_block(i);
    ASSERT_EQ(block.size(), 1920u * 2);
    EXPECT_TRUE(std::all_of(block.begin(), block.end(),
                            [](int32_t s) { return s == 0; }));
  }

  CountingSource short_source;
  short_source.samples = make_programme_pairs(1764 * 10);
  const auto expected = AudioResampler::resample_to_synchronous(
      short_source.samples, 44100.0, VideoSystem::PAL, kFrames);
  SynchronousAudioCache cache(short_source.reader(), 44100.0, VideoSystem::PAL,
                              kFrames, memory_store());
  for (uint64_t i = 0; i < kFrames; ++i) {
    ASSERT_EQ(cache.frame_block(i), expected[i]) << "frame " << i;
  }
}

TEST(SynchronousAudioCacheTest, NoStoreServesCadenceSizedSilence) {
  CountingSource source;
  source.samples = make_dc_pairs(1764 * 4, 256000);
  SynchronousAudioCache cache(source.reader(), 44100.0, VideoSystem::PAL, 4,
                              [] { return nullptr; });
  const auto block = cache.frame_block(1);
  ASSERT_EQ(block.size(), 1920u * 2);
  EXPECT_TRUE(std::all_of(block.begin(), block.end(),
                          [](int32_t s) { return s == 0; }));
}

TEST(SynchronousAudioCacheTest, ConcurrentReadersSeeTheSameBlocks) {
  constexpr uint64_t kFrames = 300;
  CountingSource source;
  source.samples = make_programme_pairs(1764 * kFrames);
  const auto expected = AudioResampler::resample_to_synchronous(
      source.samples, 44100.0, VideoSystem::PAL, kFrames);
  SynchronousAudioCache cache(source.reader(), 44100.0, VideoSystem::PAL,
                              kFrames, memory_store());

  std::atomic<int> mismatches{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      for (uint64_t i = static_cast<uint64_t>(t); i < kFrames; i += 3) {
        if (cache.frame_block(i) != expected[i]) ++mismatches;
      }
    });
  }
  for (auto& th : threads) th.join();
  EXPECT_EQ(mismatches.load(), 0);
}

// ===========================================================================
// SDK cadence helpers used by the resampler contract
// ===========================================================================

TEST(AudioChannelPairHelpersTest, PairsPerFrame_MatchSmpte272MCadence) {
  EXPECT_EQ(audio_pairs_in_frame(0, VideoSystem::PAL), 1920u);
  EXPECT_EQ(audio_pairs_in_frame(1, VideoSystem::PAL), 1920u);
  // SMPTE 272M-1994 §14.3 Table 1: 1602/1601/1602/1601/1602.
  EXPECT_EQ(audio_pairs_in_frame(0, VideoSystem::NTSC), 1602u);
  EXPECT_EQ(audio_pairs_in_frame(1, VideoSystem::NTSC), 1601u);
  EXPECT_EQ(audio_pairs_in_frame(2, VideoSystem::NTSC), 1602u);
  EXPECT_EQ(audio_pairs_in_frame(3, VideoSystem::NTSC), 1601u);
  EXPECT_EQ(audio_pairs_in_frame(4, VideoSystem::NTSC), 1602u);
  EXPECT_EQ(audio_pairs_in_frame(2, VideoSystem::PAL_M), 1602u);
  EXPECT_EQ(audio_pairs_in_frame(0, VideoSystem::Unknown), 0u);
}

}  // namespace tests
}  // namespace orc
