/*
 * File:        decoded_comparison.cpp
 * Module:      orc-videosynth-support
 * Purpose:     The difference between two decodes of a videosynth case, as
 *              the video sink writes them in raw planar YUV444P16
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "decoded_comparison.h"

#include <cmath>
#include <fstream>
#include <utility>

namespace orc_videosynth {

std::optional<DecodedFile> DecodedFile::read(const std::filesystem::path& path,
                                             size_t frame_count,
                                             std::string* error) {
  auto fail = [&](const std::string& why) -> std::optional<DecodedFile> {
    if (error != nullptr) *error = path.string() + ": " + why;
    return std::nullopt;
  };
  std::ifstream in(path, std::ios::binary);
  if (!in) return fail("cannot be opened");
  std::error_code ec;
  const auto bytes = std::filesystem::file_size(path, ec);
  if (ec) return fail("has no size");
  if (frame_count == 0 || bytes % (frame_count * 3 * 2) != 0) {
    return fail(std::to_string(bytes) + " bytes do not divide into " +
                std::to_string(frame_count) + " frames of three 16-bit planes");
  }
  std::vector<uint8_t> raw(bytes);
  in.read(reinterpret_cast<char*>(raw.data()),
          static_cast<std::streamsize>(raw.size()));
  if (!in) return fail("cannot be read");
  std::vector<uint16_t> samples(bytes / 2);
  for (size_t i = 0; i < samples.size(); ++i) {
    samples[i] = static_cast<uint16_t>(raw[2 * i] | (raw[2 * i + 1] << 8));
  }
  return DecodedFile(std::move(samples), frame_count);
}

DecodedFile::DecodedFile(std::vector<uint16_t> samples, size_t frame_count)
    : samples_(std::move(samples)),
      frame_count_(frame_count),
      plane_samples_(frame_count == 0 ? 0 : samples_.size() / frame_count / 3) {
}

const uint16_t* DecodedFile::plane(size_t frame, size_t plane) const {
  return samples_.data() + (frame * 3 + plane) * plane_samples_;
}

std::optional<PlaneRms> plane_rms(const DecodedFile& a, const DecodedFile& b,
                                  size_t first, size_t count) {
  if (a.plane_samples() != b.plane_samples() || a.plane_samples() == 0 ||
      first + count > a.frame_count() || first + count > b.frame_count() ||
      count == 0) {
    return std::nullopt;
  }
  double energy[3] = {0.0, 0.0, 0.0};
  for (size_t f = first; f < first + count; ++f) {
    for (size_t p = 0; p < 3; ++p) {
      const uint16_t* pa = a.plane(f, p);
      const uint16_t* pb = b.plane(f, p);
      for (size_t n = 0; n < a.plane_samples(); ++n) {
        const double d = static_cast<double>(pa[n]) - pb[n];
        energy[p] += d * d;
      }
    }
  }
  const double samples = static_cast<double>(count * a.plane_samples());
  auto percent = [&](double e) {
    return std::sqrt(e / samples) / 65535.0 * 100.0;
  };
  return PlaneRms{percent(energy[0]), percent(energy[1]), percent(energy[2])};
}

}  // namespace orc_videosynth
