/*
 * File:        animated_webp_writer.h
 * Module:      orc-core
 * Purpose:     Lossless animated WebP encoding of preview images (A/B
 *              comparison export)
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include <orc/stage/preview/orc_rendering.h>

#include <cstdint>
#include <string>
#include <vector>

namespace orc {

/// Default time each frame of an A/B export is shown, in milliseconds.
inline constexpr uint32_t kDefaultWebPFrameIntervalMs = 500;
/// Longest frame duration a WebP ANMF chunk can hold (24-bit field).
inline constexpr uint32_t kMaxWebPFrameIntervalMs = (1u << 24) - 1;
/// Largest loop count the ANIM chunk can hold; 0 means loop forever.
inline constexpr uint32_t kMaxWebPLoopCount = 0xFFFF;

/**
 * @brief Encode preview images as a lossless animated WebP.
 *
 * Every frame is shown for @p frame_interval_ms. Frames are encoded with
 * WebP lossless (exact = 1), so a decoder gives back each frame's RGB samples
 * unchanged; the result is opaque.
 *
 * libwebp drops a frame identical to the one before it and lengthens the
 * earlier frame instead. If that leaves one frame (every image identical) the
 * result is a still, non-animated WebP. What a viewer sees is the same; a
 * caller that wants to report "A and B are identical" should compare the
 * images itself.
 *
 * @param frames At least two valid images, all the same size.
 * @param frame_interval_ms 1 – kMaxWebPFrameIntervalMs.
 * @param loop_count 0 (loop forever) – kMaxWebPLoopCount.
 * @return The bytes of the .webp file.
 * @throws std::invalid_argument for fewer than two frames, an invalid frame,
 *         frames of different sizes or an out-of-range interval or loop count.
 * @throws std::runtime_error if libwebp fails.
 */
std::vector<uint8_t> encode_animated_webp(
    const std::vector<PreviewImage>& frames,
    uint32_t frame_interval_ms = kDefaultWebPFrameIntervalMs,
    uint32_t loop_count = 0);

/**
 * @brief Write encoded bytes to a file, replacing any file already there.
 *
 * @throws std::runtime_error if the file cannot be written; the message
 *         names the path.
 */
void write_file(const std::string& path, const std::vector<uint8_t>& bytes);

}  // namespace orc
