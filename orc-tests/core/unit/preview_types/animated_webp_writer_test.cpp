/*
 * File:        animated_webp_writer_test.cpp
 * Module:      orc-core-unit-tests
 * Purpose:     Lossless animated WebP encoding: round trip through
 *              libwebp's demuxer and decoder, timing and input checks
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "animated_webp_writer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <webp/demux.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace orc_unit_test {

using orc::encode_animated_webp;
using orc::PreviewImage;

namespace {

/// A deterministic pseudo-random image, so every sample value is exercised.
PreviewImage pattern_image(uint32_t width, uint32_t height, uint32_t seed) {
  PreviewImage image;
  image.width = width;
  image.height = height;
  image.rgb_data.resize(static_cast<size_t>(width) * height * 3);
  uint32_t state = seed * 2654435761u + 1u;
  for (auto& sample : image.rgb_data) {
    state = state * 1664525u + 1013904223u;
    sample = static_cast<uint8_t>(state >> 24);
  }
  return image;
}

PreviewImage flat_image(uint32_t width, uint32_t height, uint8_t value) {
  PreviewImage image;
  image.width = width;
  image.height = height;
  image.rgb_data.assign(static_cast<size_t>(width) * height * 3, value);
  return image;
}

struct DecodedAnimation {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t loop_count = 0;
  std::vector<PreviewImage> frames;  ///< Full-canvas RGB of each frame
  std::vector<int> durations_ms;     ///< From each ANMF chunk
  bool all_opaque = true;
  bool animated = false;  ///< ANIMATION flag in the VP8X chunk
};

DecodedAnimation decode(const std::vector<uint8_t>& bytes) {
  DecodedAnimation result;
  WebPData data{bytes.data(), bytes.size()};

  WebPAnimDecoderOptions options;
  EXPECT_TRUE(WebPAnimDecoderOptionsInit(&options));
  options.color_mode = MODE_RGBA;
  options.use_threads = 0;
  WebPAnimDecoder* decoder = WebPAnimDecoderNew(&data, &options);
  if (decoder == nullptr) {
    ADD_FAILURE() << "WebPAnimDecoderNew rejected the encoded bytes";
    return result;
  }
  WebPAnimInfo info;
  EXPECT_TRUE(WebPAnimDecoderGetInfo(decoder, &info));
  result.width = info.canvas_width;
  result.height = info.canvas_height;
  result.loop_count = info.loop_count;

  while (WebPAnimDecoderHasMoreFrames(decoder)) {
    uint8_t* rgba = nullptr;
    int timestamp = 0;
    if (!WebPAnimDecoderGetNext(decoder, &rgba, &timestamp)) {
      ADD_FAILURE() << "WebPAnimDecoderGetNext failed";
      break;
    }
    PreviewImage frame;
    frame.width = info.canvas_width;
    frame.height = info.canvas_height;
    const size_t pixels = static_cast<size_t>(frame.width) * frame.height;
    frame.rgb_data.resize(pixels * 3);
    for (size_t i = 0; i < pixels; ++i) {
      frame.rgb_data[i * 3 + 0] = rgba[i * 4 + 0];
      frame.rgb_data[i * 3 + 1] = rgba[i * 4 + 1];
      frame.rgb_data[i * 3 + 2] = rgba[i * 4 + 2];
      if (rgba[i * 4 + 3] != 255) {
        result.all_opaque = false;
      }
    }
    result.frames.push_back(std::move(frame));
  }
  WebPAnimDecoderDelete(decoder);

  WebPDemuxer* demux = WebPDemux(&data);
  if (demux == nullptr) {
    ADD_FAILURE() << "WebPDemux rejected the encoded bytes";
    return result;
  }
  result.animated =
      (WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS) & ANIMATION_FLAG) != 0;
  WebPIterator iter;
  if (WebPDemuxGetFrame(demux, 1, &iter)) {
    do {
      result.durations_ms.push_back(iter.duration);
    } while (WebPDemuxNextFrame(&iter));
    WebPDemuxReleaseIterator(&iter);
  }
  WebPDemuxDelete(demux);
  return result;
}

/// Run @p call and return its std::invalid_argument message.
template <typename Call>
std::string invalid_argument_message(Call call) {
  try {
    call();
  } catch (const std::invalid_argument& error) {
    return error.what();
  }
  ADD_FAILURE() << "expected std::invalid_argument";
  return {};
}

}  // namespace

TEST(AnimatedWebPWriterTest, TwoFramesRoundTripExactly) {
  const PreviewImage a = pattern_image(37, 23, 1);
  const PreviewImage b = pattern_image(37, 23, 2);

  const DecodedAnimation decoded = decode(encode_animated_webp({a, b}, 500));

  EXPECT_TRUE(decoded.animated);
  EXPECT_EQ(decoded.width, 37u);
  EXPECT_EQ(decoded.height, 23u);
  ASSERT_EQ(decoded.frames.size(), 2u);
  EXPECT_EQ(decoded.frames[0].rgb_data, a.rgb_data);
  EXPECT_EQ(decoded.frames[1].rgb_data, b.rgb_data);
  EXPECT_TRUE(decoded.all_opaque);
}

TEST(AnimatedWebPWriterTest, OneLevelDifferenceSurvives) {
  // The point of the export: a one-code-value change must not be lost.
  const PreviewImage a = flat_image(16, 16, 100);
  PreviewImage b = a;
  b.rgb_data[(5 * 16 + 7) * 3 + 1] = 101;

  const DecodedAnimation decoded = decode(encode_animated_webp({a, b}, 500));

  ASSERT_EQ(decoded.frames.size(), 2u);
  EXPECT_EQ(decoded.frames[0].rgb_data, a.rgb_data);
  EXPECT_EQ(decoded.frames[1].rgb_data, b.rgb_data);
}

TEST(AnimatedWebPWriterTest, ThreeFramesKeepOrderAndContent) {
  const std::vector<PreviewImage> frames = {pattern_image(20, 11, 7),
                                            pattern_image(20, 11, 8),
                                            pattern_image(20, 11, 9)};

  const DecodedAnimation decoded = decode(encode_animated_webp(frames, 250));

  ASSERT_EQ(decoded.frames.size(), 3u);
  for (size_t i = 0; i < frames.size(); ++i) {
    EXPECT_EQ(decoded.frames[i].rgb_data, frames[i].rgb_data) << "frame " << i;
  }
}

TEST(AnimatedWebPWriterTest, EveryFrameLastsTheInterval) {
  const DecodedAnimation decoded = decode(encode_animated_webp(
      {pattern_image(8, 8, 1), pattern_image(8, 8, 2), pattern_image(8, 8, 3)},
      333));

  EXPECT_THAT(decoded.durations_ms, ::testing::ElementsAre(333, 333, 333));
}

TEST(AnimatedWebPWriterTest, LoopsForeverByDefault) {
  const DecodedAnimation decoded = decode(
      encode_animated_webp({pattern_image(8, 8, 1), pattern_image(8, 8, 2)}));

  EXPECT_EQ(decoded.loop_count, 0u);
  EXPECT_THAT(decoded.durations_ms,
              ::testing::ElementsAre(
                  static_cast<int>(orc::kDefaultWebPFrameIntervalMs),
                  static_cast<int>(orc::kDefaultWebPFrameIntervalMs)));
}

TEST(AnimatedWebPWriterTest, KeepsAFiniteLoopCount) {
  const DecodedAnimation decoded = decode(encode_animated_webp(
      {pattern_image(8, 8, 1), pattern_image(8, 8, 2)}, 100, 3));

  EXPECT_EQ(decoded.loop_count, 3u);
}

TEST(AnimatedWebPWriterTest, IdenticalFramesGiveAStillImage) {
  // libwebp drops a frame identical to the one before it; with one frame
  // left it writes a still image. Viewers show the same picture either way.
  const PreviewImage a = pattern_image(12, 9, 4);

  const DecodedAnimation decoded = decode(encode_animated_webp({a, a}, 400));

  EXPECT_FALSE(decoded.animated);
  ASSERT_EQ(decoded.frames.size(), 1u);
  EXPECT_EQ(decoded.frames[0].rgb_data, a.rgb_data);
}

TEST(AnimatedWebPWriterTest, OutputIsAWebPRiffFile) {
  const std::vector<uint8_t> bytes =
      encode_animated_webp({pattern_image(4, 4, 1), pattern_image(4, 4, 2)});

  ASSERT_GE(bytes.size(), 12u);
  EXPECT_EQ(std::string(bytes.begin(), bytes.begin() + 4), "RIFF");
  EXPECT_EQ(std::string(bytes.begin() + 8, bytes.begin() + 12), "WEBP");
}

TEST(AnimatedWebPWriterTest, RefusesNoFrames) {
  EXPECT_THAT(invalid_argument_message([] { encode_animated_webp({}); }),
              ::testing::HasSubstr("at least two frames, got 0"));
}

TEST(AnimatedWebPWriterTest, RefusesASingleFrame) {
  EXPECT_THAT(invalid_argument_message(
                  [] { encode_animated_webp({pattern_image(4, 4, 1)}); }),
              ::testing::HasSubstr("at least two frames, got 1"));
}

TEST(AnimatedWebPWriterTest, RefusesFramesOfDifferentSizes) {
  EXPECT_THAT(invalid_argument_message([] {
                encode_animated_webp(
                    {pattern_image(10, 6, 1), pattern_image(10, 7, 2)});
              }),
              ::testing::HasSubstr("frame 0 is 10x6, frame 1 is 10x7"));
}

TEST(AnimatedWebPWriterTest, RefusesAnInvalidFrame) {
  PreviewImage broken = pattern_image(4, 4, 2);
  broken.rgb_data.pop_back();

  EXPECT_THAT(invalid_argument_message([&] {
                encode_animated_webp({pattern_image(4, 4, 1), broken});
              }),
              ::testing::HasSubstr("frame 1 is not a valid image"));
}

TEST(AnimatedWebPWriterTest, RefusesAZeroInterval) {
  EXPECT_THAT(invalid_argument_message([] {
                encode_animated_webp(
                    {pattern_image(4, 4, 1), pattern_image(4, 4, 2)}, 0);
              }),
              ::testing::HasSubstr("frame interval must be 1 to"));
}

TEST(AnimatedWebPWriterTest, RefusesAnIntervalTooLongForTheFormat) {
  EXPECT_THAT(invalid_argument_message([] {
                encode_animated_webp(
                    {pattern_image(4, 4, 1), pattern_image(4, 4, 2)},
                    orc::kMaxWebPFrameIntervalMs + 1);
              }),
              ::testing::HasSubstr("frame interval must be 1 to"));
}

TEST(AnimatedWebPWriterTest, AcceptsTheLongestInterval) {
  const DecodedAnimation decoded = decode(
      encode_animated_webp({pattern_image(4, 4, 1), pattern_image(4, 4, 2)},
                           orc::kMaxWebPFrameIntervalMs));

  EXPECT_THAT(decoded.durations_ms,
              ::testing::Each(static_cast<int>(orc::kMaxWebPFrameIntervalMs)));
}

TEST(AnimatedWebPWriterTest, RefusesALoopCountTooLargeForTheFormat) {
  EXPECT_THAT(invalid_argument_message([] {
                encode_animated_webp(
                    {pattern_image(4, 4, 1), pattern_image(4, 4, 2)}, 100,
                    orc::kMaxWebPLoopCount + 1);
              }),
              ::testing::HasSubstr("loop count must be 0 to 65535"));
}

}  // namespace orc_unit_test
