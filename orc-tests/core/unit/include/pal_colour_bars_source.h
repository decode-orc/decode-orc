/*
 * File:        pal_colour_bars_source.h
 * Module:      orc-core-tests
 * Purpose:     A synthetic PAL composite source of colour bars over a flat
 *              coloured field, with an optional FM beat and noise, for the
 *              FM-beat canceller's tests
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/artifact.h>
#include <orc/stage/cvbs_signal_constants.h>
#include <orc/stage/video_frame_representation.h>
#include <orc/support/fm_beat.h>
#include <orc/support/frame_line_util.h>

#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace orc_unit_test {

// PAL frames, every active line a colour burst, then seven 75 % bars (yellow
// to blue) over the upper half of each field and a flat red field below.
// Lines advance the subcarrier as PAL does (283.7516 cycles a line), with
// V-switch. Optionally a beat, built with fm_beat::FieldBeat at
// |beat_carrier_hz| with amplitude |beat_amplitude| on every picture line,
// and uniform noise of +-|noise_codes|, the same on every frame of a seed.
class PalColourBarsSource : public orc::VideoFrameRepresentation,
                            public orc::Artifact {
 public:
  static constexpr size_t kSpl = 1135;
  static constexpr size_t kHeight = 625;
  static constexpr int16_t kBlack = 256;
  static constexpr int16_t kWhite = 844;

  struct Options {
    size_t frames = 1;
    bool colour = true;
    std::optional<double> beat_carrier_hz;
    std::complex<double> beat_amplitude{200.0, 120.0};
    int noise_codes = 0;
    uint32_t seed = 1;
  };

  explicit PalColourBarsSource(const Options& options)
      : orc::Artifact(orc::ArtifactID("pal_colour_bars"), orc::Provenance{}),
        options_(options) {
    for (size_t f = 0; f < options.frames; ++f) frames_.push_back(make(f));
  }

  std::string type_name() const override { return "pal_colour_bars"; }
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
    d.height = kHeight;
    d.samples_total = frames_[id].size();
    d.samples_per_line_nominal = kSpl;
    return d;
  }
  const sample_type* get_frame(orc::FrameID id) const override {
    return has_frame(id) ? frames_[id].data() : nullptr;
  }
  std::vector<sample_type> get_frame_copy(orc::FrameID id) const override {
    return has_frame(id) ? frames_[id] : std::vector<sample_type>{};
  }
  std::optional<orc::SourceParameters> get_video_parameters() const override {
    return parameters();
  }

  static orc::SourceParameters parameters() {
    orc::SourceParameters p;
    p.system = orc::VideoSystem::PAL;
    p.frame_width_nominal = static_cast<int32_t>(kSpl);
    p.frame_height = static_cast<int32_t>(kHeight);
    p.blanking_level = kBlack;
    p.black_level = kBlack;
    p.white_level = kWhite;
    p.active_video_start = orc::kPalActiveVideoStart;
    p.active_video_end = orc::kPalActiveVideoEnd;
    p.first_active_frame_line = orc::kPalFirstActiveFrameLine;
    p.last_active_frame_line = orc::kPalLastActiveFrameLine;
    return p;
  }

  // The frame without its beat and noise.
  std::vector<int16_t> clean(orc::FrameID id) const {
    return picture(id, options_.colour);
  }

 private:
  Options options_;
  std::vector<std::vector<int16_t>> frames_;

  std::vector<int16_t> picture(size_t frame, bool colour) const {
    std::vector<int16_t> x(
        orc::frame_line_sample_offset(orc::VideoSystem::PAL, kSpl, kHeight),
        kBlack);
    // 75 % bars: luma and (U, V) in IRE.
    constexpr std::array<std::array<double, 3>, 7> kBars{{{50.0, -33.0, 7.0},
                                                          {44.0, 11.0, -46.0},
                                                          {36.0, -22.0, -39.0},
                                                          {29.0, 22.0, 39.0},
                                                          {21.0, -11.0, 46.0},
                                                          {15.0, 33.0, -7.0},
                                                          {0.0, 0.0, 0.0}}};
    const double ire = (kWhite - kBlack) / 100.0;
    const size_t f1 = orc::field1_lines(orc::VideoSystem::PAL);
    for (size_t line = 0; line < kHeight; ++line) {
      const size_t field_line = line < f1 ? line : line - f1;
      const size_t base =
          orc::frame_line_sample_offset(orc::VideoSystem::PAL, kSpl, line);
      const size_t length =
          orc::frame_line_sample_count(orc::VideoSystem::PAL, kSpl, line);
      // Each frame starts a quarter cycle on from the last, as the
      // eight-field sequence does.
      const double phase =
          2.0 * M_PI * ((0.7516 * static_cast<double>(line)) + (0.25 * frame));
      const double v_switch = (line % 2) != 0 ? -1.0 : 1.0;
      const bool upper = field_line < 160;
      for (size_t n = 0; n < length; ++n) {
        const double theta = phase + (static_cast<double>(n) * M_PI / 2.0);
        double y = 0.0;
        double u = 0.0;
        double v = 0.0;
        if (n >= 98 && n < 138) {
          u = -21.0;
          v = 21.0;
        } else if (field_line >= 22 && field_line < 310 && n >= 157 &&
                   n < 1105) {
          if (upper) {
            const auto& bar = kBars[std::min<size_t>((n - 157) / 136, 6)];
            y = 25.0 + bar[0];
            u = bar[1];
            v = bar[2];
          } else {
            y = 30.0;
            u = -15.0;
            v = 50.0;
          }
        }
        if (!colour) {
          u = 0.0;
          v = 0.0;
        }
        const double s =
            y + (u * std::sin(theta)) + (v_switch * v * std::cos(theta));
        x[base + n] = static_cast<int16_t>(std::lround(kBlack + (s * ire)));
      }
    }
    return x;
  }

  std::vector<int16_t> make(size_t frame) const {
    std::vector<int16_t> x = picture(frame, options_.colour);
    if (options_.beat_carrier_hz) {
      // subtract() removes Re(c B); a fit of -c on every line adds it.
      auto with_beat = x;
      const auto desc = descriptor(x.size());
      for (const auto& field : orc::fm_beat::frame_fields(desc, parameters())) {
        const orc::fm_beat::FieldBeat beat(x.data(), field,
                                           *options_.beat_carrier_hz);
        orc::fm_beat::BeatFit fit;
        fit.amplitude.assign(field.picture_end - field.picture_first,
                             -options_.beat_amplitude);
        beat.subtract(fit, with_beat.data());
      }
      x = with_beat;
    }
    if (options_.noise_codes > 0) {
      uint32_t state = options_.seed * 2654435761u + frame;
      for (auto& sample : x) {
        state = (state * 1664525u) + 1013904223u;
        const int span = (2 * options_.noise_codes) + 1;
        sample = static_cast<int16_t>(sample +
                                      static_cast<int>((state >> 8) % span) -
                                      options_.noise_codes);
      }
    }
    return x;
  }

  static orc::FrameDescriptor descriptor(size_t total) {
    orc::FrameDescriptor d;
    d.system = orc::VideoSystem::PAL;
    d.height = kHeight;
    d.samples_total = total;
    d.samples_per_line_nominal = kSpl;
    return d;
  }
};

}  // namespace orc_unit_test
