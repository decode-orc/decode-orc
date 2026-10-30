/*
 * File:        decoded_comparison.h
 * Module:      orc-videosynth-support
 * Purpose:     The difference between two decodes of a videosynth case, as
 *              the video sink writes them in raw planar YUV444P16
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace orc_videosynth {

// A raw planar YUV444P16 file: each frame its Y, U and V planes in turn,
// every sample a little-endian 16-bit value.
class DecodedFile {
 public:
  // The file split into |frame_count| frames; nullopt, with |error| set,
  // when it cannot be read or does not divide into three equal planes per
  // frame.
  static std::optional<DecodedFile> read(const std::filesystem::path& path,
                                         size_t frame_count,
                                         std::string* error = nullptr);

  // From samples already in memory, in the file's order.
  DecodedFile(std::vector<uint16_t> samples, size_t frame_count);

  size_t frame_count() const { return frame_count_; }
  size_t plane_samples() const { return plane_samples_; }

  // Plane |plane| (0 = Y, 1 = U, 2 = V) of frame |frame|.
  const uint16_t* plane(size_t frame, size_t plane) const;

 private:
  std::vector<uint16_t> samples_;
  size_t frame_count_ = 0;
  size_t plane_samples_ = 0;
};

// The RMS difference per plane, in percent of the 16-bit full scale.
struct PlaneRms {
  double y = 0.0;
  double u = 0.0;
  double v = 0.0;
};

// The difference between |a| and |b| over frames [first, first + count);
// nullopt when the two differ in geometry or the frames are out of range.
std::optional<PlaneRms> plane_rms(const DecodedFile& a, const DecodedFile& b,
                                  size_t first, size_t count);

}  // namespace orc_videosynth
