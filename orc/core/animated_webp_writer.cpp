/*
 * File:        animated_webp_writer.cpp
 * Module:      orc-core
 * Purpose:     Lossless animated WebP encoding of preview images (A/B
 *              comparison export)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#include "animated_webp_writer.h"

#include <webp/encode.h>
#include <webp/mux.h>

#include <climits>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace orc {

namespace {

std::string size_text(const PreviewImage& image) {
  return std::to_string(image.width) + "x" + std::to_string(image.height);
}

void validate_frames(const std::vector<PreviewImage>& frames,
                     uint32_t frame_interval_ms, uint32_t loop_count) {
  if (frames.size() < 2) {
    throw std::invalid_argument(
        "Animated WebP needs at least two frames, got " +
        std::to_string(frames.size()));
  }
  for (size_t i = 0; i < frames.size(); ++i) {
    if (!frames[i].is_valid()) {
      throw std::invalid_argument("Animated WebP frame " + std::to_string(i) +
                                  " is not a valid image");
    }
    if (frames[i].width != frames[0].width ||
        frames[i].height != frames[0].height) {
      throw std::invalid_argument(
          "Animated WebP frames must all be the same size: frame 0 is " +
          size_text(frames[0]) + ", frame " + std::to_string(i) + " is " +
          size_text(frames[i]));
    }
  }
  if (frames[0].width > WEBP_MAX_DIMENSION ||
      frames[0].height > WEBP_MAX_DIMENSION) {
    throw std::invalid_argument(
        "Animated WebP frames are " + size_text(frames[0]) +
        "; WebP allows at most " + std::to_string(WEBP_MAX_DIMENSION) +
        " on each side");
  }
  if (frame_interval_ms == 0 || frame_interval_ms > kMaxWebPFrameIntervalMs) {
    throw std::invalid_argument("Animated WebP frame interval must be 1 to " +
                                std::to_string(kMaxWebPFrameIntervalMs) +
                                " ms, got " +
                                std::to_string(frame_interval_ms));
  }
  // Timestamps are int milliseconds; the end timestamp must not overflow.
  if (static_cast<uint64_t>(frame_interval_ms) * frames.size() > INT_MAX) {
    throw std::invalid_argument(
        "Animated WebP is too long: " + std::to_string(frames.size()) +
        " frames of " + std::to_string(frame_interval_ms) + " ms");
  }
  if (loop_count > kMaxWebPLoopCount) {
    throw std::invalid_argument("Animated WebP loop count must be 0 to " +
                                std::to_string(kMaxWebPLoopCount) + ", got " +
                                std::to_string(loop_count));
  }
}

struct EncoderDeleter {
  void operator()(WebPAnimEncoder* encoder) const {
    WebPAnimEncoderDelete(encoder);
  }
};
using EncoderPtr = std::unique_ptr<WebPAnimEncoder, EncoderDeleter>;

/// Frees a WebPPicture's buffers when it goes out of scope.
struct PictureGuard {
  WebPPicture picture;
  PictureGuard() {
    if (!WebPPictureInit(&picture)) {
      throw std::runtime_error("libwebp version mismatch (WebPPictureInit)");
    }
  }
  ~PictureGuard() { WebPPictureFree(&picture); }
  PictureGuard(const PictureGuard&) = delete;
  PictureGuard& operator=(const PictureGuard&) = delete;
};

std::string encoder_error(WebPAnimEncoder* encoder, const std::string& step) {
  const char* detail = WebPAnimEncoderGetError(encoder);
  return "Animated WebP encoding failed (" + step + ")" +
         ((detail != nullptr && *detail != '\0') ? std::string(": ") + detail
                                                 : std::string());
}

}  // namespace

std::vector<uint8_t> encode_animated_webp(
    const std::vector<PreviewImage>& frames, uint32_t frame_interval_ms,
    uint32_t loop_count) {
  validate_frames(frames, frame_interval_ms, loop_count);

  const int width = static_cast<int>(frames[0].width);
  const int height = static_cast<int>(frames[0].height);

  WebPAnimEncoderOptions options;
  if (!WebPAnimEncoderOptionsInit(&options)) {
    throw std::runtime_error(
        "libwebp version mismatch (WebPAnimEncoderOptionsInit)");
  }
  options.anim_params.loop_count = static_cast<int>(loop_count);
  options.allow_mixed = 0;  // every frame lossless

  WebPConfig config;
  if (!WebPConfigInit(&config)) {
    throw std::runtime_error("libwebp version mismatch (WebPConfigInit)");
  }
  config.lossless = 1;
  config.exact = 1;
  if (!WebPValidateConfig(&config)) {
    throw std::runtime_error("libwebp rejected the lossless configuration");
  }

  EncoderPtr encoder(WebPAnimEncoderNew(width, height, &options));
  if (!encoder) {
    throw std::runtime_error("Animated WebP encoder could not be created for " +
                             size_text(frames[0]));
  }

  int timestamp_ms = 0;
  for (size_t i = 0; i < frames.size(); ++i) {
    PictureGuard guard;
    guard.picture.use_argb = 1;
    guard.picture.width = width;
    guard.picture.height = height;
    if (!WebPPictureImportRGB(&guard.picture, frames[i].rgb_data.data(),
                              width * 3)) {
      throw std::runtime_error("Animated WebP frame " + std::to_string(i) +
                               " could not be imported (out of memory)");
    }
    if (!WebPAnimEncoderAdd(encoder.get(), &guard.picture, timestamp_ms,
                            &config)) {
      throw std::runtime_error(
          encoder_error(encoder.get(), "frame " + std::to_string(i)));
    }
    timestamp_ms += static_cast<int>(frame_interval_ms);
  }
  // A null frame marks the end, giving the last frame its duration.
  if (!WebPAnimEncoderAdd(encoder.get(), nullptr, timestamp_ms, nullptr)) {
    throw std::runtime_error(encoder_error(encoder.get(), "end of stream"));
  }

  WebPData data;
  WebPDataInit(&data);
  if (!WebPAnimEncoderAssemble(encoder.get(), &data)) {
    throw std::runtime_error(encoder_error(encoder.get(), "assemble"));
  }
  std::vector<uint8_t> bytes(data.bytes, data.bytes + data.size);
  WebPDataClear(&data);
  return bytes;
}

void write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("Cannot open " + path + " for writing");
  }
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  out.close();
  if (!out) {
    throw std::runtime_error("Failed writing " + path);
  }
}

}  // namespace orc
