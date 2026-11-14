/*
 * File:        pal_picture_number_source.h
 * Module:      orc-core-tests
 * Purpose:     An in-memory PAL source whose frames carry CAV picture
 *              numbers in their VBI, for tools that read a disc's addresses
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/artifact.h>
#include <orc/stage/common_types.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/frame_descriptor.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/frame_line_util.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace orc_unit_test {

// Blank PAL frames, each field carrying one IEC 60856 CAV picture number on
// VBI lines 17 and 18 (0xF followed by five BCD digits) and nothing on
// line 16. A picture number of 0 leaves the frame without VBI. Nothing is
// read from or written to disk.
class PalPictureNumberSource : public orc::VideoFrameRepresentation,
                               public orc::Artifact {
 public:
  explicit PalPictureNumberSource(std::vector<int32_t> picture_numbers)
      : orc::Artifact(orc::ArtifactID("pal_picture_numbers"),
                      orc::Provenance{}) {
    for (const int32_t pn : picture_numbers) frames_.push_back(make(pn));
  }

  std::string type_name() const override { return "pal_picture_numbers"; }
  orc::FrameIDRange frame_range() const override {
    return orc::FrameIDRange{0u, frames_.size() - 1};
  }
  size_t frame_count() const override { return frames_.size(); }
  bool has_frame(orc::FrameID id) const override { return id < frames_.size(); }
  std::optional<orc::FrameDescriptor> get_frame_descriptor(
      orc::FrameID id) const override {
    if (!has_frame(id)) return std::nullopt;
    orc::FrameDescriptor d;
    d.frame_id = id;
    d.system = orc::VideoSystem::PAL;
    d.height = static_cast<size_t>(orc::kPalFrameLines);
    d.samples_total = frames_[id].size();
    d.samples_per_line_nominal =
        static_cast<size_t>(orc::kPalSamplesPerLineNominal);
    return d;
  }
  const sample_type* get_frame(orc::FrameID id) const override {
    if (!has_frame(id)) return nullptr;
    reads.fetch_add(1, std::memory_order_relaxed);
    return frames_[id].data();
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return has_frame(id) ? frames_[id] : std::vector<sample_type>{};
  }
  std::optional<orc::SourceParameters> get_video_parameters() const override {
    orc::SourceParameters p{};
    p.system = orc::VideoSystem::PAL;
    p.frame_width_nominal = orc::kPalSamplesPerLineNominal;
    p.frame_height = orc::kPalFrameLines;
    p.blanking_level = orc::kPalBlanking;
    p.white_level = orc::kPalWhite;
    p.active_video_start = orc::kPalActiveVideoStart;
    return p;
  }

  /// Frames read, by any thread.
  mutable std::atomic<int> reads{0};

 private:
  static uint32_t bcd(int32_t value) {
    uint32_t out = 0;
    for (int shift = 0; shift < 20; shift += 4) {
      out |= static_cast<uint32_t>(value % 10) << shift;
      value /= 10;
    }
    return out;
  }

  // Manchester-encode a 24-bit word the way the biphase decoder reads it: a
  // 2 us cell per bit, 1 -> low half then high half, 0 -> the reverse.
  static void write_word(int16_t* line, size_t width, uint32_t word) {
    const double cell = orc::kPalSampleRate * 2e-6;
    const auto low = static_cast<int16_t>(orc::kPalBlanking);
    const auto high = static_cast<int16_t>(orc::kPalWhite);
    const auto start = static_cast<double>(orc::kPalActiveVideoStart);
    for (int bit = 23; bit >= 0; --bit) {
      const double cell_start = start + (23 - static_cast<double>(bit)) * cell;
      const double mid = cell_start + cell / 2.0;
      const bool one = ((word >> bit) & 1u) != 0u;
      for (auto s = static_cast<size_t>(cell_start);
           s < static_cast<size_t>(mid) && s < width; ++s) {
        line[s] = one ? low : high;
      }
      for (auto s = static_cast<size_t>(mid);
           s < static_cast<size_t>(cell_start + cell) && s < width; ++s) {
        line[s] = one ? high : low;
      }
    }
  }

  static std::vector<int16_t> make(int32_t picture_number) {
    const auto spl = static_cast<size_t>(orc::kPalSamplesPerLineNominal);
    const orc::VideoSystem pal = orc::VideoSystem::PAL;
    std::vector<int16_t> samples(
        orc::frame_line_sample_offset(pal, spl,
                                      static_cast<size_t>(orc::kPalFrameLines)),
        static_cast<int16_t>(orc::kPalBlanking));
    if (picture_number <= 0) return samples;
    const uint32_t word = 0xF00000u | bcd(picture_number);
    const size_t f1 = orc::field1_lines(pal);
    for (size_t field = 0; field < 2; ++field) {
      for (size_t vbi_line : {17u, 18u}) {
        const size_t flat_line = (field == 0 ? 0 : f1) + vbi_line - 1;
        write_word(
            samples.data() + orc::frame_line_sample_offset(pal, spl, flat_line),
            orc::frame_line_sample_count(pal, spl, flat_line), word);
      }
    }
    return samples;
  }

  std::vector<std::vector<int16_t>> frames_;
};

}  // namespace orc_unit_test
